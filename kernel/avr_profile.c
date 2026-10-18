// SPDX-License-Identifier: GPL-2.0
/*
 * Rakkashin kernel: avr performance profiles.
 *
 * One switch that moves several kernel knobs together:
 *
 *   echo game > /sys/kernel/avr_profile/mode
 *   cat /sys/kernel/avr_profile/mode        # "battery [balanced] performance game"
 *
 *   mode         cpufreq floor/cap  PELT   launch boost  writeback  thread boost  slice
 *   battery      efficiency cap     32 ms  uclamp 640    30 s       off           200%
 *   balanced     smart cap (below)  16 ms  uclamp 1024   15 s       off           100%
 *   performance  floor 30% of max   8 ms   uclamp 1024   15 s       +15%          70%
 *   game         floor 50% of max   8 ms   uclamp 1024   15 s       +25%          70%
 *   extreme      pinned at max      8 ms   uclamp 1024   15 s       +50%          100%
 *
 * Per-app profiles: "mode" is the global profile. app_profiles maps app UIDs
 * to their own mode; when a task of such a UID is moved into the top-app
 * cpuset (the framework does that when the app comes to the foreground) its
 * mode takes effect, and once top-app no longer holds any of its tasks the
 * global mode returns. No polling: decisions run off cpuset attach. Mode
 * changes sysfs_notify() "mode" so userspace can follow with inotify.
 *
 *   echo "10234:game 10077:battery" > /sys/kernel/avr_profile/app_profiles
 *   echo 0 > /sys/kernel/avr_profile/per_app    # ignore app_profiles
 *
 * Battery caps each cluster at its efficiency knee, read from the Energy
 * Model: the highest frequency whose cost (energy per unit of work) is
 * within 25% of the cluster's most efficient state, kept within 50..85% of
 * max. Without a real (non-artificial) EM it falls back to 70%. The caps in
 * use are shown in battery_caps. (Voltage itself is set by firmware/CPR on
 * this SoC and is not reachable from the kernel.)
 *
 * Smart Balanced: in balanced, CPUs are capped at smart_cap percent of max
 * (default 80) while nobody touches the screen, and the cap is lifted the
 * moment a touch event arrives (in-kernel input handler) or an app launches,
 * held for SMART_HOLD_MS after the last touch (WAKE_HOLD_MS after the power
 * key, so waking the phone and the unlock animation before the first touch
 * aren't capped). The top of the frequency
 * range is where energy per unit of work is worst; this trims it during
 * passive use (reading, video, background work) without touching interactive
 * performance. smart_cap=100 restores plain balanced.
 *
 * Sustained performance (sustained=1): long sessions slow down because the
 * phone heats up until thermal mitigation throttles it. So while the skin
 * is cool nothing changes (full speed on touch); once the skin temperature
 * (front/back virtual sensors) reaches sustained_temp, the demand-matched
 * caps stay on while the screen is touched too. Their headroom shrinks from
 * 70% at sustained_temp to 30% SUST_SPAN_C degrees above it, so the device
 * runs only as fast as the work needs exactly when heat is building up.
 * While touched the caps start from the top, jump to the top when a CPU is
 * saturated, and only come down after a second of lower load, so frame
 * bursts are never squeezed. The fixed smart_cap is still lifted on touch.
 *
 * Frame-aware (encore_fas): the foreground app's frames are timed by a
 * uprobe on Surface::queueBuffer (drivers/misc/encore_fas, registered by
 * the control module's rk_gpud for top_pid). A late frame, an overdue frame
 * or a frame rate deficit lifts the demand cap of every cluster whose cap
 * was actually holding it back (running at it within the last
 * JANK_BOUND_TICKS) and leaves a learned floor there: held for 10 s, held
 * twice as long each time it is hit again (up to 2 min), then probed 5% of
 * max lower every 5 s while frames stay on time. So the caps settle just
 * above what each game really needs instead of hunting around it, and a
 * jank that our caps didn't cause costs no power at all.
 *
 * While frames are timed, a late one is caught within a frame, so the
 * headroom is trimmed adaptively (additive increase, multiplicative
 * decrease): one point less after every 5 s with no cap-caused jank, up to
 * FAS_CUT_MAX, and FAS_CUT_BACK points back at once when our caps held a
 * frame back. Each app converges to the least headroom its frames tolerate.
 * The learned floors and the trim are remembered for the last APP_MEMORY
 * apps (by UID), so returning to a game starts where it left off.
 *
 * The demand cap is rounded up to the next real OPP (a QoS max between two
 * OPPs means the one below, which would eat the headroom) and never set
 * below the policy's most energy-efficient OPP (lowest EM cost: running
 * slower than that costs more energy per unit of work, not less).
 *
 * CPU frequency is steered only through cpufreq's frequency QoS, the same
 * mechanism thermal mitigation uses. QoS takes the lowest max request and
 * the highest min request, and the policy max always wins over min, so a
 * thermal cap is never overridden by a floor set here.
 */

#include <linux/avr_profile.h>
#include <linux/cgroup.h>
#include <linux/cpufreq.h>
#include <linux/cred.h>
#include <linux/energy_model.h>
#include <linux/input.h>
#include <linux/kobject.h>
#include <linux/launch_boost.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/power_supply.h>
#include <linux/rk_tboost.h>
#include <linux/sched/sysctl.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>

#define RAKKASHIN_VERSION	"Rakkashin Kernel v3.1 (avr, KernelSU-Next)"

extern int sched_pelt_set_multiplier(unsigned int mult);
extern unsigned long rk_cpu_util(int cpu);
extern unsigned int sysctl_sched_base_slice;
static unsigned int stock_slice;	/* captured before the first change */

enum avr_mode {
	AVR_BATTERY,
	AVR_BALANCED,
	AVR_PERFORMANCE,
	AVR_GAME,
	AVR_EXTREME,
	AVR_NR_MODES,
};

struct avr_mode_params {
	const char *name;
	unsigned int min_pct;		/* cpufreq floor, % of cpuinfo max; 0 = none */
	unsigned int max_pct;		/* cpufreq cap, % of cpuinfo max; 100 = none */
	bool eff_cap;			/* use the EM efficiency knee instead of max_pct */
	unsigned int pelt_mult;		/* 1: 32ms, 2: 16ms, 4: 8ms half-life */
	unsigned int launch_uclamp;	/* launch boost floor, 0 = off */
	unsigned int writeback_cs;	/* dirty_writeback_interval, centiseconds */
	unsigned int tboost_pct;	/* foreground thread headroom boost, 0 = off */
	unsigned int slice_pct;		/* EEVDF base slice, % of the stock value */
	bool smart;			/* touch-aware smart_cap applies */
	bool demand;			/* cap at measured demand + headroom when idle */
	bool floor_touch;		/* min_pct only while the screen is touched */
};

static const char * const mode_names[AVR_NR_MODES] = {
	[AVR_BATTERY]	  = "battery",
	[AVR_BALANCED]	  = "balanced",
	[AVR_PERFORMANCE] = "performance",
	[AVR_GAME]	  = "game",
	[AVR_EXTREME]	  = "extreme",
};

static const struct avr_mode_params modes[AVR_NR_MODES] = {
	/*                                 min max  eff   pelt launch  wb   tb  slice smart  demand floor_touch */
	[AVR_BATTERY]	  = { "battery",	 0,  70, true,  1,  640, 3000,  0, 200, false, true,  false },
	[AVR_BALANCED]	  = { "balanced",	 0, 100, false, 2, 1024, 1500,  0, 100, true,  true,  false },
	[AVR_PERFORMANCE] = { "performance",	30, 100, false, 4, 1024, 1500, 15,  70, false, false, true  },
	[AVR_GAME]	  = { "game",		50, 100, false, 4, 1024, 1500, 25,  70, false, false, false },
	/*
	 * extreme: benchmark-grade, heat be damned. CPUs pinned at max (no
	 * ramp-up between benchmark runs), heaviest threads boosted +50%,
	 * launch boost and fastest PELT. Thermal caps (QoS max) still win.
	 */
	[AVR_EXTREME]	  = { "extreme",	100, 100, false, 4, 1024, 1500, 50, 100, false, false, false },
};

struct avr_policy_reqs {
	bool added;
	struct freq_qos_request min;
	struct freq_qos_request max;
	s32 dcap;		/* demand cap in kHz, 0 = start from measured need,
				   DCAP_TOP = start from the top (touch) */
	unsigned int low_ticks;	/* consecutive ticks the need sat well below dcap */
	unsigned int bound_age;	/* ticks since a CPU last ran at dcap */
	unsigned int sat_age;	/* ticks since a CPU was saturated at the limit */
	s32 jfloor;		/* learned from late frames, kHz; 0 = none */
	unsigned int jticks;	/* ticks until the floor is probed lower */
	unsigned int jhold;	/* hold of the floor, doubles on repeat */
	unsigned int eff_khz;	/* most energy-efficient OPP, 0 = not known yet */
};

/* indexed by policy->cpu, the policy's owning CPU */
#ifndef NO_UID
#define NO_UID			((uid_t)-1)
#endif
static struct avr_policy_reqs reqs[NR_CPUS];
/* per-CPU idle time at the last demand tick, for windowed busy */
static u64 win_wall[NR_CPUS], win_idle[NR_CPUS];
static bool applied;				/* a mode was applied at least once */

/* Smart Balanced state */
#define SMART_HOLD_MS	1000
#define WAKE_HOLD_MS	2000	/* caps stay lifted this long after the power key */
static unsigned long wake_until;		/* jiffies; lifted at least until then */
static unsigned int smart_cap = 80;		/* % of max while idle; 100 = off */
/*
 * Sustained performance: keep the demand-matched caps (CPU here, GPU in
 * rk_gpud via demand_active) while the screen is touched too, with more
 * headroom, so long gaming sessions run cooler and throttle less.
 */
static bool sustained;
#define SUST_TEMP_DEFAULT	40	/* skin degrees C where the touched caps engage */
#define SUST_HYST_MC		1500	/* ...and release this much below it */
#define SUST_SPAN_C		5	/* headroom tightens over this many degrees */
#define SUST_HEADROOM_COOL	70	/* touched headroom at sustained_temp */
#define SUST_HEADROOM_HOT	30	/* ...and SUST_SPAN_C degrees above it */
#define NO_TEMP			INT_MIN
static unsigned int sustained_temp = SUST_TEMP_DEFAULT;
static int skin_mc = NO_TEMP;			/* last skin reading, milli-degC */
static bool skin_read;				/* skin_mc is a fresh reading */
static unsigned int temp_tick;			/* gov ticks since sustained was set */
static bool sustained_hot;			/* skin reached sustained_temp */
static bool smart_active;			/* the mode in effect uses it */
static bool smart_lifted;			/* cap currently lifted */
static bool smart_armed;			/* first touch seen: boot is over */
static unsigned long last_kick;			/* jiffies of the last touch/launch */

/* demand cap and power limit, re-evaluated by gov_work */
#define GOV_MS			20
#define DEMAND_HEADROOM_PCT	25	/* above measured utilization */
#define DEMAND_TOUCH_HEADROOM_PCT 40	/* touched, sustained, no skin sensor */
#define DEMAND_TOUCH_FALL_TICKS	50	/* touched: hold a peak for a second */
#define DEMAND_WIN_MAX_US	200000	/* idle-time windows longer than this are stale */
#define DCAP_TOP		S32_MAX
#define TEMP_EVERY		50	/* read the skin temperature every 1s */
/* frame events (encore_fas) */
#define JANK_BOUND_TICKS	5	/* a cap at work this recently held the frame */
#define JANK_STALE_MS		100	/* older events no longer describe the caps */
#define JFLOOR_HOLD_TICKS	500	/* 10 s */
#define JFLOOR_HOLD_MAX_TICKS	6000	/* 2 min */
#define JFLOOR_PROBE_TICKS	250	/* then 5% lower every 5 s */
static atomic_t fas_pending = ATOMIC_INIT(0);	/* worst level since the last tick */
static unsigned long fas_pending_at;		/* jiffies of that event */
static unsigned int fas_janks;			/* late frames that lifted a cap */
static bool fas_reset;				/* foreground app changed: forget floors */
static pid_t top_pid;				/* foreground app process */
static unsigned long fas_seen;			/* jiffies of the last timed frame */
#define FAS_SEEN_MS		1000	/* frames timed this recently: timing is live */
#define FAS_HEADROOM_MIN	10	/* never trim headroom below this */
#define FAS_CUT_START		5	/* headroom trim for an app we haven't seen */
#define FAS_CUT_MAX		20
#define FAS_CUT_BACK		10	/* given back when our caps held a frame */
#define FAS_CUT_STEP_TICKS	250	/* 5 s of on-time frames per point */
static unsigned int fas_cut = FAS_CUT_START;	/* headroom trim, points */
static unsigned int fas_clean_ticks;
static uid_t top_uid = NO_UID;			/* UID of top_pid */
static uid_t frame_uid = NO_UID;		/* UID whose frames are being timed */
static uid_t frame_cand = NO_UID;		/* a new one, not yet confirmed */
static unsigned int frame_cand_n;
#define FRAME_UID_CONFIRM	30	/* frames before the learned state switches */
static uid_t mem_uid = NO_UID;			/* UID whose state is live */
#define APP_MEMORY		8
static struct {
	uid_t uid;				/* NO_UID = free */
	unsigned int cut;
	unsigned long used;			/* jiffies, for LRU */
	s32 jfloor[NR_CPUS];			/* by policy owner CPU */
} app_mem[APP_MEMORY] = { [0 ... APP_MEMORY - 1] = { .uid = NO_UID } };
#define DEMAND_FLOOR_PCT	30	/* never cap below this share of max */
#define DEMAND_FALL_TICKS	5	/* need must stay low this long before lowering */
#define DEMAND_SAT_PCT		90	/* a CPU this busy at the cap is saturated */
#define POWER_EVERY		50	/* measure battery power every POWER_EVERY ticks (1s) */
#define POWER_MIN_PCT		30
static unsigned int power_limit_mw;		/* 0 = off */
static unsigned int power_pct = 100;		/* CPU cap from the power loop */
static unsigned int power_now_mw;		/* last measurement */
static unsigned int power_base_mw;		/* non-CPU power, from measurements */
static bool power_discharging;			/* last measurement was on battery */
static unsigned int power_tick;			/* gov ticks since the limit was set */

static enum avr_mode cur_mode = AVR_BALANCED;	/* in effect */
static enum avr_mode base_mode = AVR_BALANCED;	/* chosen by the user */
static DEFINE_MUTEX(avr_lock);
static struct kobject *avr_kobj;

#define MAX_APP_PROFILES	64
/* each entry is (uid << 8 | mode), so lockless readers see whole entries */
static u64 app_profiles[MAX_APP_PROFILES];
static unsigned int nr_app_profiles;
static bool per_app = true;
static uid_t active_uid = NO_UID;		/* app whose profile is in effect */
static uid_t pending_uid = NO_UID;		/* handed from cpuset attach to work */
static struct cgroup_subsys_state *top_app_css;	/* cached, never released */

static s32 pct_of(unsigned int max_freq, unsigned int pct)
{
	return (s32)div_u64((u64)max_freq * pct, 100);
}

#define EFF_SLACK_PCT	25	/* accepted cost above the most efficient state */
#define EFF_MIN_PCT	50	/* never cap below this share of max */
#define EFF_MAX_PCT	85	/* ...nor above it */

/*
 * Highest frequency (kHz) whose cost is within EFF_SLACK_PCT of the cluster's
 * most efficient state, or 0 if there is no usable Energy Model.
 */
static unsigned int efficiency_knee_khz(int cpu)
{
	struct em_perf_domain *pd = em_cpu_get(cpu);
	struct em_perf_state *table;
	unsigned long best = ULONG_MAX, knee = 0;
	int i, n;

	if (!pd || em_is_artificial(pd))
		return 0;

	rcu_read_lock();
	table = em_perf_state_from_pd(pd);
	n = em_pd_nr_perf_states(pd);
	for (i = 0; i < n; i++)
		if (table[i].cost && table[i].cost < best)
			best = table[i].cost;
	for (i = 0; i < n; i++)
		if (table[i].cost &&
		    table[i].cost * 100 <= best * (100 + EFF_SLACK_PCT))
			knee = max(knee, table[i].frequency);
	rcu_read_unlock();

	return knee;
}

/* battery cap in kHz for this policy; *from_em tells whether the EM set it */
static s32 battery_cap(struct cpufreq_policy *policy, unsigned int pct,
		       bool *from_em)
{
	unsigned int fmax = policy->cpuinfo.max_freq;
	unsigned int knee = efficiency_knee_khz(policy->cpu);

	*from_em = knee;
	if (!knee)
		return pct_of(fmax, pct);
	return clamp_t(s32, knee, pct_of(fmax, EFF_MIN_PCT), pct_of(fmax, EFF_MAX_PCT));
}

/*
 * Frequency (kHz) of the policy's lowest-cost Energy Model state: the most
 * work per joule. 0 without a usable EM. Cached; the EM doesn't change.
 */
static unsigned int efficient_khz(struct cpufreq_policy *policy)
{
	struct avr_policy_reqs *r = &reqs[policy->cpu];
	struct em_perf_domain *pd;
	struct em_perf_state *table;
	unsigned long best = ULONG_MAX, f = 0;
	int i, n;

	if (r->eff_khz)
		return r->eff_khz == UINT_MAX ? 0 : r->eff_khz;
	pd = em_cpu_get(policy->cpu);
	if (!pd || em_is_artificial(pd)) {
		r->eff_khz = UINT_MAX;		/* remember: none */
		return 0;
	}
	rcu_read_lock();
	table = em_perf_state_from_pd(pd);
	n = em_pd_nr_perf_states(pd);
	for (i = 0; i < n; i++)
		if (table[i].cost && table[i].cost < best) {
			best = table[i].cost;
			f = table[i].frequency;
		}
	rcu_read_unlock();
	r->eff_khz = f ? f : UINT_MAX;
	return f;
}

/* lowest real OPP >= khz, so a QoS max between two OPPs isn't rounded down */
static s32 opp_ceil(struct cpufreq_policy *policy, s32 khz)
{
	int idx;

	if (!policy->freq_table || khz <= 0)
		return khz;
	idx = cpufreq_table_find_index_al(policy, khz, false);
	return idx >= 0 ? (s32)policy->freq_table[idx].frequency : khz;
}

/* touched with sustained performance and the cap engaged */
static bool sustained_engaged(void)
{
	/* until the first reading: not yet; without a sensor: always */
	return sustained && skin_read && (skin_mc == NO_TEMP || sustained_hot);
}

/* headroom above the measured need while touched (sustained performance) */
static unsigned int touch_headroom(void)
{
	int over;

	if (skin_mc == NO_TEMP)
		return DEMAND_TOUCH_HEADROOM_PCT;
	over = clamp(skin_mc - (int)sustained_temp * 1000, 0, SUST_SPAN_C * 1000);
	return SUST_HEADROOM_COOL -
	       (SUST_HEADROOM_COOL - SUST_HEADROOM_HOT) * over / (SUST_SPAN_C * 1000);
}

/*
 * Demand cap, re-evaluated every GOV_MS while it is tracked.
 *
 * need = frequency the busiest CPU of the policy needs for its current load
 * plus headroom, from two measurements:
 *  - PELT utilization (frequency-invariant, so util/capacity of max), which
 *    remembers recent load,
 *  - the busy share of the last tick from idle time, times the frequency it
 *    ran at, which sees a load step within one tick (PELT takes ~100ms).
 * A CPU that was busy for the whole tick while running at the cap is
 * saturated: its real demand is unknown, so the cap jumps (to the top while
 * touched, by half otherwise). The cap
 *  - rises at once when need exceeds it,
 *  - falls only after need stayed below 85% of it for DEMAND_FALL_TICKS
 *    (a whole second while touched, so the peaks of periodic frame work
 *    set the cap, not its average), and then gradually.
 * The vendor governor can't overshoot it, so the same work runs at lower
 * frequency and voltage.
 */
static void demand_update(struct cpufreq_policy *policy, unsigned int jank)
{
	struct avr_policy_reqs *r = &reqs[policy->cpu];
	unsigned int fmax = policy->cpuinfo.max_freq;
	unsigned int khz = READ_ONCE(policy->cur);
	bool touched = smart_lifted;
	/* the cap is in force (untouched, or touched with Sustained engaged) */
	bool applied = !touched || sustained_engaged();
	/* never below the most efficient OPP (clamped to a sane share of max) */
	s32 floor = max_t(s32, pct_of(fmax, DEMAND_FLOOR_PCT),
			  min_t(s32, efficient_khz(policy), pct_of(fmax, 60)));
	s32 cur, limit;
	unsigned long need = 0;
	bool saturated = false;
	unsigned int cpu, headroom;
	s32 want;

	if (!fmax)
		return;
	if (r->dcap == DCAP_TOP) {	/* touch: start from the top */
		r->dcap = fmax;
		r->low_ticks = 0;
	}
	cur = r->dcap ? r->dcap : (s32)fmax;
	/* the limit actually in force, which may be below our own cap */
	limit = min_t(s32, cur, READ_ONCE(policy->max));
	if (!r->dcap) {
		/* fresh start: never ran at a cap yet */
		r->bound_age = UINT_MAX;
		r->sat_age = UINT_MAX;
	} else if (applied && r->dcap < (s32)fmax && cur <= (s32)READ_ONCE(policy->max) &&
		   (u64)khz * 100 >= (u64)cur * 95) {
		/*
		 * Our cap (not thermal or another limit) is what the CPUs ran
		 * into. Only while it is applied: while touched with Sustained
		 * still cool, dcap is tracked but not in force, and a CPU above
		 * it is simply running free.
		 */
		r->bound_age = 0;
	} else if (r->bound_age < UINT_MAX) {
		r->bound_age++;
	}

	for_each_cpu(cpu, policy->cpus) {
		unsigned long cap = max(arch_scale_cpu_capacity(cpu), 1UL);
		unsigned long u = min(rk_cpu_util(cpu), cap);
		u64 wall, idle = get_cpu_idle_time(cpu, &wall, 0);
		u64 dw = wall - win_wall[cpu], di = idle - win_idle[cpu];

		need = max(need, u * fmax / cap);
		/* capacity available at the cap: cap * dcap / fmax */
		if (u * 100 >= DEMAND_SAT_PCT * (cap * (unsigned long)cur / fmax))
			saturated = true;

		if (win_wall[cpu] && dw && dw <= DEMAND_WIN_MAX_US) {
			unsigned int busy = di < dw ?
				(unsigned int)div64_u64((dw - di) * 100, dw) : 0;

			need = max(need, (unsigned long)khz * busy / 100);
			if (busy >= DEMAND_SAT_PCT &&
			    (u64)khz * 100 >= (u64)limit * 95)
				saturated = true;
		}
		win_wall[cpu] = wall;
		win_idle[cpu] = idle;
	}
	headroom = touched ? touch_headroom() : DEMAND_HEADROOM_PCT;
	if (time_before(jiffies, READ_ONCE(fas_seen) + msecs_to_jiffies(FAS_SEEN_MS)))
		headroom = headroom > FAS_HEADROOM_MIN + fas_cut ?
			   headroom - fas_cut :
			   min_t(unsigned int, headroom, FAS_HEADROOM_MIN);
	want = clamp_t(s32, need * (100 + headroom) / 100, floor, fmax);

	if (saturated && applied && r->bound_age == 0)
		r->sat_age = 0;
	else if (r->sat_age < UINT_MAX)
		r->sat_age++;

	/*
	 * A late frame while this cap held the CPUs back: a CPU was running at
	 * our cap *and* busy for the whole tick (running at the cap alone is
	 * normal in a game, and its own hitches - shader compiles, loading -
	 * aren't ours). Lift, and raise the floor in small steps so it settles
	 * at the least that keeps frames on time.
	 */
	if (jank && r->dcap && r->dcap < (s32)fmax &&
	    r->sat_age <= JANK_BOUND_TICKS) {
		s32 f = min_t(s32, fmax, r->dcap + fmax / (jank > 1 ? 10 : 20));

		r->jhold = r->jfloor ? min_t(unsigned int, r->jhold * 2,
					     JFLOOR_HOLD_MAX_TICKS) :
				       JFLOOR_HOLD_TICKS;
		r->jfloor = max(r->jfloor, f);
		r->jticks = r->jhold;
		r->dcap = jank > 1 ? (s32)fmax : max(r->dcap, f);
		r->low_ticks = 0;
		fas_janks++;
	} else if (r->jfloor) {
		/* frames on time: try a little lower now and then */
		if (r->jticks) {
			r->jticks--;
		} else {
			r->jfloor -= fmax / 20;
			r->jticks = JFLOOR_PROBE_TICKS;
			if (r->jfloor <= floor)
				r->jfloor = 0;
		}
	}
	want = max(want, r->jfloor);

	if (!r->dcap) {			/* fresh start: take the need as is */
		r->dcap = want;
		r->low_ticks = 0;
	} else if (saturated && r->dcap < (s32)fmax) {
		r->dcap = touched ? (s32)fmax :
			  min_t(s32, fmax, r->dcap + r->dcap / 2);
		r->low_ticks = 0;
	} else if (want > r->dcap) {
		r->dcap = want;
		r->low_ticks = 0;
	} else if (want < r->dcap * 85 / 100) {
		if (++r->low_ticks >= (touched ? DEMAND_TOUCH_FALL_TICKS :
					DEMAND_FALL_TICKS))
			/* step down by at most 5% (touched) or 10% of max per tick */
			r->dcap = max_t(s32, want, r->dcap -
					(s32)(fmax / (touched ? 20 : 10)));
	} else {
		r->low_ticks = 0;
	}
}

/* Called with avr_lock held. */
static void apply_policy(struct cpufreq_policy *policy,
			 const struct avr_mode_params *m)
{
	struct avr_policy_reqs *r = &reqs[policy->cpu];
	unsigned int fmax = policy->cpuinfo.max_freq;
	bool idle = smart_armed && !smart_lifted;	/* no touch right now */
	/* demand cap live: untouched, or touched with sustained engaged */
	bool demand_on = smart_armed && (!smart_lifted || sustained_engaged());
	s32 min = FREQ_QOS_MIN_DEFAULT_VALUE;
	s32 max = fmax;
	bool from_em;

	if (m->min_pct && (!m->floor_touch || !idle))
		min = pct_of(fmax, m->min_pct);

	if (m->eff_cap)
		max = battery_cap(policy, m->max_pct, &from_em);
	else if (m->max_pct < 100)
		max = pct_of(fmax, m->max_pct);
	if (m->smart && idle && smart_cap < 100)
		max = min(max, pct_of(fmax, smart_cap));
	if (m->demand && demand_on && r->dcap && r->dcap != DCAP_TOP)
		max = min(max, opp_ceil(policy, r->dcap));
	if (power_pct < 100)
		max = min(max, pct_of(fmax, power_pct));
	if (max >= (s32)fmax)
		max = FREQ_QOS_MAX_DEFAULT_VALUE;

	if (!r->added) {
		if (freq_qos_add_request(&policy->constraints, &r->min,
					 FREQ_QOS_MIN, min) < 0)
			return;
		if (freq_qos_add_request(&policy->constraints, &r->max,
					 FREQ_QOS_MAX, max) < 0) {
			freq_qos_remove_request(&r->min);
			return;
		}
		r->added = true;
		return;
	}

	/*
	 * Only call into QoS on a real change: the loop runs every GOV_MS and
	 * freq_qos_update_request() fires a vendor hook even when nothing moved.
	 */
	if (r->min.pnode.prio != min)
		freq_qos_update_request(&r->min, min);
	/*
	 * Caps are re-evaluated every GOV_MS: skip *lowering* them by less than
	 * ~3% of max so load jitter doesn't churn cpufreq limit updates. Any
	 * raise goes through at once, so performance is never held back.
	 */
	if (max < r->max.pnode.prio &&
	    r->max.pnode.prio - max < (s32)(fmax / 32))
		return;
	if (r->max.pnode.prio != max)
		freq_qos_update_request(&r->max, max);
}

static void gov_fn(struct work_struct *work);
static DECLARE_DEFERRABLE_WORK(gov_work, gov_fn);

/* the demand-matched cap is live: a demand mode, booted, screen not touched */
static bool demand_live(void)
{
	return modes[cur_mode].demand && smart_armed &&
	       (!smart_lifted || sustained_engaged());
}

/*
 * The demand cap is measured (not necessarily applied): also while touched
 * with sustained performance on but the skin still cool, so the cap is
 * already settled when it engages.
 */
static bool demand_tracked(void)
{
	return modes[cur_mode].demand && smart_armed &&
	       (!smart_lifted || sustained);
}

/* demand_active: 0 off, 1 live and untouched, 2 live while touched */
static int demand_state(void)
{
	if (!demand_live())
		return 0;
	return smart_lifted ? 2 : 1;
}

/*
 * Tell userspace (the control module's GPU governor) when demand_live()
 * flips, so it can lift or restore its own caps at once. Called with
 * avr_lock held after any state change.
 */
static void demand_notify(void)
{
	static int last;
	int now = demand_state();

	if (now != last && avr_kobj) {
		last = now;
		sysfs_notify(avr_kobj, NULL, "demand_active");
	}
}

/* the loop is needed while a demand cap is live or a power limit is set */
static bool gov_needed(void)
{
	return demand_tracked() || READ_ONCE(power_limit_mw);
}

static void gov_kick(void)
{
	mod_delayed_work(system_unbound_wq, &gov_work, msecs_to_jiffies(GOV_MS));
}

/* Called with avr_lock held. */
static void apply_policies(const struct avr_mode_params *m)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;
		if (policy->cpu == cpu)
			apply_policy(policy, m);
		cpufreq_cpu_put(policy);
	}
	demand_notify();
}

/* Called with avr_lock held. */
static void apply_mode(enum avr_mode mode)
{
	const struct avr_mode_params *m = &modes[mode];

	applied = true;
	WRITE_ONCE(smart_active, m->smart || m->demand || m->floor_touch);
	apply_policies(m);
	gov_kick();

	sched_pelt_set_multiplier(m->pelt_mult);
	launch_boost_set(m->launch_uclamp);

	/*
	 * EEVDF base slice, relative to whatever the system set up (a vendor
	 * scheduler may tune it): longer slices mean fewer context switches
	 * and less cache churn, shorter ones mean quicker wakeup preemption.
	 */
	if (m->slice_pct != 100 && !stock_slice)
		/* captured late (first real change), after vendor init tuned it */
		stock_slice = READ_ONCE(sysctl_sched_base_slice);
	if (stock_slice)
		WRITE_ONCE(sysctl_sched_base_slice, stock_slice / 100 * m->slice_pct);
	WRITE_ONCE(dirty_writeback_interval, m->writeback_cs);
	rk_tboost_set_headroom(m->tboost_pct);

	if (cur_mode != mode) {
		cur_mode = mode;
		if (avr_kobj)
			sysfs_notify(avr_kobj, NULL, "mode");
	}
}

static int avr_policy_notifier(struct notifier_block *nb, unsigned long event,
			       void *data)
{
	struct cpufreq_policy *policy = data;
	struct avr_policy_reqs *r = &reqs[policy->cpu];

	mutex_lock(&avr_lock);
	if (event == CPUFREQ_CREATE_POLICY) {
		/* a policy (re)appeared: bring it in line with the current mode */
		if (!r->added && applied)
			apply_policy(policy, &modes[cur_mode]);
	} else if (event == CPUFREQ_REMOVE_POLICY && r->added) {
		freq_qos_remove_request(&r->min);
		freq_qos_remove_request(&r->max);
		r->added = false;
	}
	mutex_unlock(&avr_lock);

	return NOTIFY_OK;
}

static struct notifier_block avr_policy_nb = {
	.notifier_call = avr_policy_notifier,
};

/* ---- demand cap and power limit loop ---------------------------------- */

/* Battery discharge power in mW, or 0 if unknown or charging. May sleep. */
static unsigned int battery_mw(void)
{
	struct power_supply *psy = power_supply_get_by_name("battery");
	union power_supply_propval i, v, st;
	unsigned int mw = 0;

	if (!psy)
		return 0;
	if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS, &st) &&
	    st.intval != POWER_SUPPLY_STATUS_CHARGING &&
	    st.intval != POWER_SUPPLY_STATUS_FULL &&
	    !power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_NOW, &i) &&
	    !power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &v))
		/* uA * uV = pW; / 1e9 = mW */
		mw = div_u64((u64)abs(i.intval) * (u64)abs(v.intval), 1000000000);
	power_supply_put(psy);
	return mw;
}

/*
 * Power model: predicted CPU power of a policy, in uW, if its CPUs ran at
 * the highest OPP <= @khz (or at policy->cur when @khz is 0) with their
 * current load. From the Energy Model: power(OPP) times the busy fraction,
 * util / (capacity at that OPP), per CPU. Returns -1 without a usable EM.
 */
static s64 cpu_power_uw(struct cpufreq_policy *policy, unsigned int khz)
{
	struct em_perf_domain *pd = em_cpu_get(policy->cpu);
	unsigned int fmax = policy->cpuinfo.max_freq;
	struct em_perf_state *table;
	unsigned long f, pw;
	unsigned int cpu;
	s64 total = 0;
	int i, n;

	if (!pd || em_is_artificial(pd) || !(pd->flags & EM_PERF_DOMAIN_MICROWATTS))
		return -1;
	if (!khz)
		khz = READ_ONCE(policy->cur);

	rcu_read_lock();
	table = em_perf_state_from_pd(pd);
	n = em_pd_nr_perf_states(pd);
	f = table[0].frequency;
	pw = table[0].power;
	for (i = 1; i < n && table[i].frequency <= khz; i++) {
		f = table[i].frequency;
		pw = table[i].power;
	}
	rcu_read_unlock();

	for_each_cpu(cpu, policy->cpus) {
		unsigned long cap = max(arch_scale_cpu_capacity(cpu), 1UL);
		unsigned long u = min(rk_cpu_util(cpu), cap);
		/* busy share at f, in permille: util / (cap * f / fmax) */
		unsigned long busy = min(1000UL, u * fmax * 1000 / (cap * max(f, 1UL)));

		total += (s64)pw * busy / 1000;
	}
	return total;
}

/* Sum of cpu_power_uw() over all policies; -1 if any lacks a model. */
static s64 cpus_power_uw(unsigned int pct)
{
	unsigned int cpu;
	s64 total = 0;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		s64 p;

		if (!policy)
			continue;
		p = policy->cpu != cpu ? 0 :
		    cpu_power_uw(policy, pct ? pct_of(policy->cpuinfo.max_freq, pct) : 0);
		cpufreq_cpu_put(policy);
		if (p < 0)
			return -1;
		total += p;
	}
	return total;
}

/*
 * Power limit, called every GOV_MS with avr_lock held. With an Energy Model
 * it is predictive: budget = limit - non-CPU power (calibrated each second
 * from a real battery reading), and the CPU cap is the highest 5% step whose
 * predicted CPU power fits the budget, so a load spike is answered within
 * one tick instead of after the next measurement. Without a usable EM it
 * steps the cap from the 1s measurements alone.
 */
static void power_update(unsigned int limit, bool measured, unsigned int mw)
{
	s64 now_uw;
	unsigned int pct;

	if (!limit) {
		power_pct = 100;
		return;
	}

	if (measured) {
		power_now_mw = mw;
		power_discharging = mw;
		now_uw = cpus_power_uw(0);
		if (mw && now_uw >= 0) {
			unsigned int base = mw > now_uw / 1000 ? mw - now_uw / 1000 : 0;

			/* smooth, but follow a fresh calibration quickly */
			power_base_mw = power_base_mw ? (power_base_mw + base) / 2 : base;
		}
	}
	if (!power_discharging) {	/* charging or unknown: nothing to limit */
		power_pct = 100;
		return;
	}

	if (cpus_power_uw(100) < 0) {
		/* no model: measured stepping */
		if (!measured)
			return;
		if (mw > limit)
			power_pct = max_t(int, POWER_MIN_PCT, (int)power_pct -
					  (int)clamp_t(unsigned int, (mw - limit) * 50 / limit, 5, 20));
		else if (mw < limit * 85 / 100)
			power_pct = min(100U, power_pct + 5);
		return;
	}

	for (pct = 100; pct > POWER_MIN_PCT; pct -= 5) {
		s64 budget_uw = ((s64)limit - power_base_mw) * 1000;

		if (cpus_power_uw(pct) <= budget_uw)
			break;
	}
	/* drop at once, rise by at most 10% per tick to avoid see-sawing */
	power_pct = pct < power_pct ? pct : min(pct, power_pct + 10);
}

/*
 * Adaptive headroom trim, once per tick with avr_lock held: a point less per
 * FAS_CUT_STEP_TICKS of timed frames without a cap-caused jank, FAS_CUT_BACK
 * back when our caps held a frame back (@lifted).
 */
static void fas_trim(bool lifted)
{
	if (lifted) {
		fas_cut = fas_cut > FAS_CUT_BACK ? fas_cut - FAS_CUT_BACK : 0;
		fas_clean_ticks = 0;
		return;
	}
	if (!time_before(jiffies, READ_ONCE(fas_seen) + msecs_to_jiffies(FAS_SEEN_MS)))
		return;			/* not timing frames: nothing learned */
	if (++fas_clean_ticks >= FAS_CUT_STEP_TICKS) {
		fas_clean_ticks = 0;
		if (fas_cut < FAS_CUT_MAX)
			fas_cut++;
	}
}

/*
 * The app in front changed (avr_lock held): keep what was learned for the
 * one leaving, restore what we know of the one arriving.
 */
static void app_mem_switch(uid_t uid)
{
	int i, slot = -1, lru = 0;
	unsigned int cpu;

	if (mem_uid != NO_UID) {
		for (i = 0; i < APP_MEMORY; i++)
			if (app_mem[i].uid == mem_uid)
				slot = i;
		if (slot < 0) {
			for (i = 0; i < APP_MEMORY; i++) {
				if (app_mem[i].uid == NO_UID) {
					lru = i;
					break;
				}
				if (time_before(app_mem[i].used, app_mem[lru].used))
					lru = i;
			}
			slot = lru;
		}
		app_mem[slot].uid = mem_uid;
		app_mem[slot].cut = fas_cut;
		app_mem[slot].used = jiffies;
		for_each_possible_cpu(cpu)
			app_mem[slot].jfloor[cpu] = reqs[cpu].jfloor;
	}

	mem_uid = uid;
	fas_cut = FAS_CUT_START;
	fas_clean_ticks = 0;
	for_each_possible_cpu(cpu) {
		reqs[cpu].jfloor = 0;
		reqs[cpu].jhold = 0;
	}
	if (uid == NO_UID)
		return;
	for (i = 0; i < APP_MEMORY; i++) {
		if (app_mem[i].uid != uid)
			continue;
		fas_cut = app_mem[i].cut;
		app_mem[i].used = jiffies;
		for_each_possible_cpu(cpu) {
			reqs[cpu].jfloor = app_mem[i].jfloor[cpu];
			/* a fresh hold: the scene may differ from when it was saved */
			reqs[cpu].jticks = JFLOOR_HOLD_TICKS;
			reqs[cpu].jhold = JFLOOR_HOLD_TICKS;
		}
		break;
	}
}

/*
 * Skin temperature in milli-degC: the hotter of the front and back virtual
 * sensors, or NO_TEMP if there are none. May sleep; once a second at most.
 */
static int skin_temp(void)
{
	static const char * const zones[] = { "front_temp", "back_temp" };
	int best = NO_TEMP;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(zones); i++) {
		struct thermal_zone_device *tz = thermal_zone_get_zone_by_name(zones[i]);
		int t;

		if (!IS_ERR(tz) && !thermal_zone_get_temp(tz, &t) &&
		    t > -40000 && t < 150000)
			best = max(best, t);
	}
	return best;
}

static void gov_fn(struct work_struct *work)
{
	unsigned int limit = READ_ONCE(power_limit_mw);
	unsigned int mw = 0, cpu, jank, janks_before;
	/* tick 1 after a (re)set measures at once, then every POWER_EVERY */
	bool measure = limit && ++power_tick % POWER_EVERY == 1;
	bool tmeasure = READ_ONCE(sustained) && temp_tick++ % TEMP_EVERY == 0;
	int temp = NO_TEMP;

	/* outside avr_lock: the battery driver may talk to firmware */
	if (measure)
		mw = battery_mw();
	if (tmeasure)
		temp = skin_temp();

	jank = atomic_xchg(&fas_pending, 0);
	if (jank && time_after(jiffies, READ_ONCE(fas_pending_at) +
			       msecs_to_jiffies(JANK_STALE_MS)))
		jank = 0;

	mutex_lock(&avr_lock);
	if (READ_ONCE(fas_reset)) {
		WRITE_ONCE(fas_reset, false);
		app_mem_switch(READ_ONCE(frame_uid));
	}
	if (tmeasure) {
		skin_mc = temp;
		skin_read = true;
		if (temp == NO_TEMP || temp >= (int)sustained_temp * 1000)
			sustained_hot = temp != NO_TEMP;
		else if (temp < (int)sustained_temp * 1000 - SUST_HYST_MC)
			sustained_hot = false;
	}
	janks_before = fas_janks;
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;
		if (policy->cpu == cpu) {
			if (demand_tracked())
				demand_update(policy, jank);
			else {
				reqs[cpu].dcap = 0;	/* re-measure when capping resumes */
				reqs[cpu].bound_age = UINT_MAX;
				reqs[cpu].sat_age = UINT_MAX;
			}
		}
		cpufreq_cpu_put(policy);
	}
	fas_trim(fas_janks != janks_before);
	power_update(limit, measure, mw);
	apply_policies(&modes[cur_mode]);
	if (gov_needed())
		gov_kick();
	mutex_unlock(&avr_lock);
}

/* ---- Smart Balanced ----------------------------------------------------- */

static void smart_drop_fn(struct work_struct *work);
static DECLARE_DEFERRABLE_WORK(smart_drop_work, smart_drop_fn);

static void smart_lift_fn(struct work_struct *work)
{
	mutex_lock(&avr_lock);
	if (smart_active && !smart_lifted) {
		unsigned int cpu;

		smart_lifted = true;
		/*
		 * A touch is about to bring work, which the load measured
		 * before it can't show: touched demand caps start from the
		 * top and come down only once the load after the touch says so.
		 */
		for_each_possible_cpu(cpu)
			reqs[cpu].dcap = DCAP_TOP;
		apply_policies(&modes[cur_mode]);
		/* with sustained performance the loop keeps running while touched */
		if (gov_needed())
			gov_kick();
	}
	mutex_unlock(&avr_lock);
	mod_delayed_work(system_unbound_wq, &smart_drop_work,
			 msecs_to_jiffies(SMART_HOLD_MS));
}
static DECLARE_WORK(smart_lift_work, smart_lift_fn);

/* Re-cap once SMART_HOLD_MS passed without a touch; deferrable, so idle CPUs stay idle. */
static void smart_drop_fn(struct work_struct *work)
{
	unsigned long hold = msecs_to_jiffies(SMART_HOLD_MS);
	unsigned long since = jiffies - READ_ONCE(last_kick);
	unsigned long wake = READ_ONCE(wake_until);

	if (since < hold) {
		mod_delayed_work(system_unbound_wq, &smart_drop_work, hold - since);
		return;
	}
	if (time_before(jiffies, wake)) {
		mod_delayed_work(system_unbound_wq, &smart_drop_work, wake - jiffies);
		return;
	}

	mutex_lock(&avr_lock);
	if (smart_lifted) {
		smart_lifted = false;
		if (smart_active)
			apply_policies(&modes[cur_mode]);
		if (gov_needed())
			gov_kick();
	}
	mutex_unlock(&avr_lock);
}

/* every frame of a timed app (encore_fas uprobe handler): timing is live */
void avr_profile_frame_seen(void)
{
	unsigned long now = jiffies;
	uid_t uid = from_kuid(&init_user_ns, current_uid());

	if (READ_ONCE(fas_seen) != now)
		WRITE_ONCE(fas_seen, now);
	/*
	 * The app that actually draws decides whose learned state is live, not
	 * whatever joined top-app last (a bound ad service, a shell child).
	 * Racy counters are fine: one drawing app at a time in practice.
	 */
	if (likely(uid == READ_ONCE(frame_uid)))
		return;
	if (uid != READ_ONCE(frame_cand)) {
		WRITE_ONCE(frame_cand, uid);
		WRITE_ONCE(frame_cand_n, 0);
		return;
	}
	if (READ_ONCE(frame_cand_n) + 1 >= FRAME_UID_CONFIRM) {
		WRITE_ONCE(frame_uid, uid);
		WRITE_ONCE(fas_reset, true);
	} else {
		WRITE_ONCE(frame_cand_n, READ_ONCE(frame_cand_n) + 1);
	}
}

/*
 * A frame of the foreground app was late, from encore_fas: uprobe handler
 * (process context, after its spinlock) or its watchdog hrtimer (softirq).
 * Lock-free; the demand loop runs at once and lifts the caps that held the
 * frame back. Without a demand cap there is nothing to lift.
 */
void avr_profile_frame_event(unsigned int level)
{
	int old = atomic_read(&fas_pending);

	if (!READ_ONCE(smart_armed) || !modes[READ_ONCE(cur_mode)].demand)
		return;
	while (old < (int)level &&
	       !atomic_try_cmpxchg(&fas_pending, &old, (int)level))
		;
	WRITE_ONCE(fas_pending_at, jiffies);
	mod_delayed_work(system_unbound_wq, &gov_work, 0);
}

/* Touch or app launch: cheap enough to call from input event context. */
void avr_profile_kick(void)
{
	WRITE_ONCE(last_kick, jiffies);
	if (READ_ONCE(smart_active) && !READ_ONCE(smart_lifted))
		queue_work(system_highpri_wq, &smart_lift_work);
}

static void smart_input_event(struct input_handle *handle, unsigned int type,
			      unsigned int code, int value)
{
	if (type == EV_ABS || (type == EV_KEY && code == BTN_TOUCH)) {
		/* nobody touches the screen while booting: start capping now */
		if (unlikely(!READ_ONCE(smart_armed)))
			WRITE_ONCE(smart_armed, true);
		avr_profile_kick();
	} else if (type == EV_KEY && code == KEY_POWER && value == 1) {
		/* waking up: lift for the unlock before anything is touched */
		WRITE_ONCE(wake_until, jiffies + msecs_to_jiffies(WAKE_HOLD_MS));
		avr_profile_kick();
	}
}

static int smart_input_connect(struct input_handler *handler,
			       struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "rakkashin_smart";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void smart_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/* touchscreens only: multitouch position axes */
static const struct input_device_id smart_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] = BIT_MASK(ABS_MT_POSITION_X) },
	},
	{	/* the power key (PMIC pwrkey, gpio-keys) */
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_POWER)] = BIT_MASK(KEY_POWER) },
	},
	{ },
};

static struct input_handler smart_input_handler = {
	.event		= smart_input_event,
	.connect	= smart_input_connect,
	.disconnect	= smart_input_disconnect,
	.name		= "rakkashin_smart",
	.id_table	= smart_input_ids,
};

/* ---- per-app profiles ------------------------------------------------- */

/* Returns the app's mode, or -1 if it has none. Lockless. */
static int app_mode(uid_t uid)
{
	unsigned int i, n = READ_ONCE(nr_app_profiles);

	for (i = 0; i < n; i++) {
		u64 e = READ_ONCE(app_profiles[i]);

		if ((uid_t)(e >> 8) == uid)
			return e & 0xff;
	}
	return -1;
}

static void app_enter_fn(struct work_struct *work)
{
	uid_t uid = READ_ONCE(pending_uid);
	int m;

	mutex_lock(&avr_lock);
	m = app_mode(uid);
	if (per_app && m >= 0 && active_uid != uid) {
		active_uid = uid;
		apply_mode(m);
	}
	mutex_unlock(&avr_lock);
}
static DECLARE_WORK(app_enter_work, app_enter_fn);

/* Fall back to the global mode once the active app has left top-app. */
static void app_check_fn(struct work_struct *work)
{
	struct css_task_iter it;
	struct task_struct *t;
	uid_t uid = READ_ONCE(active_uid);
	bool still = false;

	if (uid == NO_UID || !top_app_css)
		return;

	css_task_iter_start(top_app_css, 0, &it);
	while ((t = css_task_iter_next(&it))) {
		if (from_kuid(&init_user_ns, task_uid(t)) == uid) {
			still = true;
			break;
		}
	}
	css_task_iter_end(&it);

	mutex_lock(&avr_lock);
	if (!still && active_uid == uid) {
		active_uid = NO_UID;
		apply_mode(base_mode);
	}
	mutex_unlock(&avr_lock);
}
static DECLARE_DELAYED_WORK(app_check_work, app_check_fn);

/*
 * Called from cpuset_attach_task() with cpuset_mutex held: only look and
 * queue work here, the mode switch itself takes cpufreq locks.
 */
static void top_pid_fn(struct work_struct *work)
{
	if (avr_kobj)
		sysfs_notify(avr_kobj, NULL, "top_pid");
}
static DECLARE_WORK(top_pid_work, top_pid_fn);

void avr_profile_cpuset_attach(struct cgroup_subsys_state *css,
			       struct task_struct *task)
{
	uid_t uid;

	if (css != READ_ONCE(top_app_css)) {
		if (top_app_css || strcmp(css->cgroup->kn->name, "top-app"))
			return;
		/* first sight of top-app: cache it for good (thread boost too) */
		css_get(css);
		WRITE_ONCE(top_app_css, css);
		rk_tboost_set_top_app(css);
	}

	uid = from_kuid(&init_user_ns, task_uid(task));
	if (uid % 100000 < 10000)	/* system UIDs never decide this */
		return;

	/*
	 * The app process now in front, for frame timing (encore_fas): a
	 * regular app UID only, not an isolated or SDK sandbox process (a
	 * browser's renderer joins top-app too but draws through another).
	 */
	if (uid % 100000 <= 19999 && thread_group_leader(task) &&
	    task->tgid != READ_ONCE(top_pid)) {
		WRITE_ONCE(top_pid, task->tgid);
		WRITE_ONCE(top_uid, uid);
		queue_work(system_unbound_wq, &top_pid_work);
	}

	if (!READ_ONCE(per_app) || !READ_ONCE(nr_app_profiles))
		return;

	if (app_mode(uid) >= 0) {
		if (uid != READ_ONCE(active_uid)) {
			WRITE_ONCE(pending_uid, uid);
			queue_work(system_unbound_wq, &app_enter_work);
		}
		cancel_delayed_work(&app_check_work);
	} else if (READ_ONCE(active_uid) != NO_UID) {
		/* let a transient (shade, dialog) settle before deciding */
		mod_delayed_work(system_unbound_wq, &app_check_work, HZ);
	}
}

static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	int i, len = 0;

	for (i = 0; i < AVR_NR_MODES; i++)
		len += sysfs_emit_at(buf, len, i == cur_mode ? "[%s] " : "%s ",
				     modes[i].name);
	buf[len - 1] = '\n';
	return len;
}

static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int i = sysfs_match_string(mode_names, buf);

	if (i < 0)
		return -EINVAL;

	mutex_lock(&avr_lock);
	base_mode = i;
	/* an explicit choice wins until the next per-app switch */
	active_uid = NO_UID;
	apply_mode(i);
	mutex_unlock(&avr_lock);

	return count;
}

static struct kobj_attribute mode_attr = __ATTR_RW(mode);

static ssize_t per_app_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(per_app));
}

static ssize_t per_app_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;

	mutex_lock(&avr_lock);
	WRITE_ONCE(per_app, on);
	if (!on && active_uid != NO_UID) {
		active_uid = NO_UID;
		apply_mode(base_mode);
	}
	mutex_unlock(&avr_lock);
	return count;
}

static struct kobj_attribute per_app_attr = __ATTR_RW(per_app);

/* "global=<mode> active=<mode> app=<uid|->" for the control UI */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	ssize_t len;

	mutex_lock(&avr_lock);
	if (active_uid == NO_UID)
		len = sysfs_emit(buf, "global=%s active=%s app=-\n",
				 mode_names[base_mode], mode_names[cur_mode]);
	else
		len = sysfs_emit(buf, "global=%s active=%s app=%u\n",
				 mode_names[base_mode], mode_names[cur_mode],
				 active_uid);
	mutex_unlock(&avr_lock);
	return len;
}

static struct kobj_attribute status_attr = __ATTR_RO(status);

static ssize_t smart_cap_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(smart_cap));
}

/* 50..100; 100 turns Smart Balanced off */
static ssize_t smart_cap_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int v;

	if (kstrtouint(buf, 10, &v) || v < 50 || v > 100)
		return -EINVAL;

	mutex_lock(&avr_lock);
	WRITE_ONCE(smart_cap, v);
	if (smart_active)
		apply_policies(&modes[cur_mode]);
	mutex_unlock(&avr_lock);
	return count;
}

static struct kobj_attribute smart_cap_attr = __ATTR_RW(smart_cap);

/*
 * 0 off, 1 while the demand-matched cap is live, 2 while it is live during
 * a touch (sustained performance engaged); pollable (sysfs_notify on change)
 */
static ssize_t demand_active_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	int state;

	mutex_lock(&avr_lock);
	state = demand_state();
	mutex_unlock(&avr_lock);
	return sysfs_emit(buf, "%d\n", state);
}

static struct kobj_attribute demand_active_attr = __ATTR_RO(demand_active);

static ssize_t sustained_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(sustained));
}

static ssize_t sustained_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;

	mutex_lock(&avr_lock);
	WRITE_ONCE(sustained, on);
	/* judge by a reading taken from now on, starting with the next tick */
	skin_read = false;
	temp_tick = 0;
	apply_policies(&modes[cur_mode]);	/* also notifies demand_active */
	if (gov_needed())
		gov_kick();
	mutex_unlock(&avr_lock);
	return count;
}

static struct kobj_attribute sustained_attr = __ATTR_RW(sustained);

/* skin degrees C where sustained performance starts capping touched work */
static ssize_t sustained_temp_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(sustained_temp));
}

static ssize_t sustained_temp_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned int v;

	if (kstrtouint(buf, 10, &v) || v < 30 || v > 50)
		return -EINVAL;

	mutex_lock(&avr_lock);
	WRITE_ONCE(sustained_temp, v);
	/* re-judge with the next reading */
	if (skin_mc != NO_TEMP)
		sustained_hot = skin_mc >= (int)v * 1000;
	apply_policies(&modes[cur_mode]);
	if (gov_needed())
		gov_kick();
	mutex_unlock(&avr_lock);
	return count;
}

static struct kobj_attribute sustained_temp_attr = __ATTR_RW(sustained_temp);

/* "skin=<milli-degC|-> engaged=<0|1> headroom=<%>" */
static ssize_t sustained_state_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	int t, engaged;
	unsigned int hr;

	mutex_lock(&avr_lock);
	t = skin_mc;
	engaged = sustained_engaged();
	hr = touch_headroom();
	mutex_unlock(&avr_lock);
	if (t == NO_TEMP)
		return sysfs_emit(buf, "skin=- engaged=%d headroom=%u\n", engaged, hr);
	return sysfs_emit(buf, "skin=%d engaged=%d headroom=%u\n", t, engaged, hr);
}

static struct kobj_attribute sustained_state_attr = __ATTR_RO(sustained_state);

/* the foreground app's process; pollable (sysfs_notify on change) */
static ssize_t top_pid_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(top_pid));
}

static struct kobj_attribute top_pid_attr = __ATTR_RO(top_pid);

/* "janks=<n> cut=<points> floors=cpu0:<kHz>,cpu6:<kHz>": frame-aware state */
static ssize_t frame_state_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	unsigned int cpu;
	int len;
	bool first = true;

	mutex_lock(&avr_lock);
	len = sysfs_emit(buf, "janks=%u cut=%u floors=", fas_janks, fas_cut);
	for_each_possible_cpu(cpu) {
		if (!reqs[cpu].added)
			continue;
		len += sysfs_emit_at(buf, len, "%scpu%u:%d", first ? "" : ",",
				     cpu, reqs[cpu].jfloor);
		first = false;
	}
	mutex_unlock(&avr_lock);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static struct kobj_attribute frame_state_attr = __ATTR_RO(frame_state);

/* "limit=<mW> now=<mW> cpu_cap=<%>"; write a limit in mW, 0 = off */
static ssize_t power_limit_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "limit=%u now=%u cpu_cap=%u\n",
			  READ_ONCE(power_limit_mw), READ_ONCE(power_now_mw),
			  READ_ONCE(power_pct));
}

static ssize_t power_limit_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int v;

	if (kstrtouint(buf, 10, &v) || (v && v < 500) || v > 30000)
		return -EINVAL;

	mutex_lock(&avr_lock);
	WRITE_ONCE(power_limit_mw, v);
	/* calibrate afresh: measure on the next tick, forget the old baseline */
	power_tick = 0;
	power_base_mw = 0;
	power_discharging = false;
	if (!v) {
		power_pct = 100;
		power_now_mw = 0;
		apply_policies(&modes[cur_mode]);
	}
	gov_kick();
	mutex_unlock(&avr_lock);
	return count;
}

static struct kobj_attribute power_limit_attr = __ATTR_RW(power_limit);

/* per cluster: "cpuN: <cap kHz>/<max kHz> (energy-model|fallback)" */
static ssize_t battery_caps_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	unsigned int cpu;
	int len = 0;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		bool from_em;
		s32 cap;

		if (!policy)
			continue;
		if (policy->cpu == cpu) {
			cap = battery_cap(policy, modes[AVR_BATTERY].max_pct, &from_em);
			len += sysfs_emit_at(buf, len, "cpu%u: %d/%u kHz (%s)\n", cpu,
					     cap, policy->cpuinfo.max_freq,
					     from_em ? "energy-model" : "fallback");
		}
		cpufreq_cpu_put(policy);
	}
	return len;
}

static struct kobj_attribute battery_caps_attr = __ATTR_RO(battery_caps);

static ssize_t app_profiles_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	unsigned int i;
	int len = 0;

	mutex_lock(&avr_lock);
	for (i = 0; i < nr_app_profiles; i++)
		len += sysfs_emit_at(buf, len, "%u:%s ",
				     (uid_t)(app_profiles[i] >> 8),
				     mode_names[app_profiles[i] & 0xff]);
	mutex_unlock(&avr_lock);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

/* "uid:mode" tokens separated by spaces/commas/newlines; replaces the table */
static ssize_t app_profiles_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	u64 new[MAX_APP_PROFILES];
	unsigned int n = 0, i;
	char *dup, *s, *tok;
	int ret = count;

	dup = kstrndup(buf, count, GFP_KERNEL);
	if (!dup)
		return -ENOMEM;

	s = dup;
	while ((tok = strsep(&s, " ,\n\t"))) {
		char *colon = strchr(tok, ':');
		int m;
		u32 uid;

		if (!*tok)
			continue;
		if (!colon || n == MAX_APP_PROFILES)
			goto inval;
		*colon = '\0';
		m = match_string(mode_names, AVR_NR_MODES, colon + 1);
		if (m < 0 || kstrtou32(tok, 10, &uid))
			goto inval;
		new[n++] = (u64)uid << 8 | m;
	}

	mutex_lock(&avr_lock);
	/* shrink first so lockless readers never see half-updated slots */
	WRITE_ONCE(nr_app_profiles, 0);
	for (i = 0; i < n; i++)
		WRITE_ONCE(app_profiles[i], new[i]);
	WRITE_ONCE(nr_app_profiles, n);
	/* the active app may have lost or changed its profile */
	if (active_uid != NO_UID) {
		int m = app_mode(active_uid);

		if (m < 0) {
			active_uid = NO_UID;
			m = base_mode;
		}
		apply_mode(m);
	}
	mutex_unlock(&avr_lock);
	goto out;
inval:
	ret = -EINVAL;
out:
	kfree(dup);
	return ret;
}

static struct kobj_attribute app_profiles_attr = __ATTR_RW(app_profiles);

/* lets the control UI tell it is running on this kernel, and which build */
static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%s\n", RAKKASHIN_VERSION);
}

static struct kobj_attribute version_attr = __ATTR_RO(version);

static struct attribute *avr_profile_attrs[] = {
	&mode_attr.attr,
	&version_attr.attr,
	&per_app_attr.attr,
	&app_profiles_attr.attr,
	&status_attr.attr,
	&battery_caps_attr.attr,
	&smart_cap_attr.attr,
	&demand_active_attr.attr,
	&sustained_attr.attr,
	&sustained_temp_attr.attr,
	&sustained_state_attr.attr,
	&top_pid_attr.attr,
	&frame_state_attr.attr,
	&power_limit_attr.attr,
	NULL
};

static const struct attribute_group avr_profile_group = {
	.attrs = avr_profile_attrs,
};

static int __init avr_profile_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("avr_profile", kernel_kobj);
	if (!kobj)
		return -ENOMEM;

	ret = sysfs_create_group(kobj, &avr_profile_group);
	if (ret) {
		kobject_put(kobj);
		return ret;
	}

	avr_kobj = kobj;

	/*
	 * Run balanced from the start, so Smart Balanced works without the
	 * control module; cpufreq policies created later pick it up through
	 * the policy notifier. The cap itself waits for the first touch.
	 */
	mutex_lock(&avr_lock);
	apply_mode(AVR_BALANCED);
	mutex_unlock(&avr_lock);

	if (input_register_handler(&smart_input_handler))
		pr_warn("rakkashin: no touch input handler, Smart Balanced stays capped\n");
	pr_info("%s\n", RAKKASHIN_VERSION);
	return cpufreq_register_notifier(&avr_policy_nb, CPUFREQ_POLICY_NOTIFIER);
}
late_initcall(avr_profile_init);
