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
 * held for SMART_HOLD_MS after the last touch. The top of the frequency
 * range is where energy per unit of work is worst; this trims it during
 * passive use (reading, video, background work) without touching interactive
 * performance. smart_cap=100 restores plain balanced.
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
#include <linux/workqueue.h>
#include <linux/writeback.h>

#define RAKKASHIN_VERSION	"Rakkashin Kernel v2.3 (avr, KernelSU-Next)"

extern int sched_pelt_set_multiplier(unsigned int mult);
extern unsigned long rk_cpu_util(int cpu);
extern unsigned int sysctl_sched_base_slice;
static unsigned int stock_slice;	/* captured before the first change */

enum avr_mode {
	AVR_BATTERY,
	AVR_BALANCED,
	AVR_PERFORMANCE,
	AVR_GAME,
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
};

static const struct avr_mode_params modes[AVR_NR_MODES] = {
	/*                                 min max  eff   pelt launch  wb   tb  slice smart  demand floor_touch */
	[AVR_BATTERY]	  = { "battery",	 0,  70, true,  1,  640, 3000,  0, 200, false, true,  false },
	[AVR_BALANCED]	  = { "balanced",	 0, 100, false, 2, 1024, 1500,  0, 100, true,  true,  false },
	[AVR_PERFORMANCE] = { "performance",	30, 100, false, 4, 1024, 1500, 15,  70, false, false, true  },
	[AVR_GAME]	  = { "game",		50, 100, false, 4, 1024, 1500, 25,  70, false, false, false },
};

struct avr_policy_reqs {
	bool added;
	struct freq_qos_request min;
	struct freq_qos_request max;
	s32 dcap;		/* demand cap in kHz, 0 = start from measured need */
	unsigned int low_ticks;	/* consecutive ticks the need sat well below dcap */
};

/* indexed by policy->cpu, the policy's owning CPU */
static struct avr_policy_reqs reqs[NR_CPUS];
static bool applied;				/* a mode was applied at least once */

/* Smart Balanced state */
#define SMART_HOLD_MS	1000
static unsigned int smart_cap = 80;		/* % of max while idle; 100 = off */
static bool smart_active;			/* the mode in effect uses it */
static bool smart_lifted;			/* cap currently lifted */
static bool smart_armed;			/* first touch seen: boot is over */
static unsigned long last_kick;			/* jiffies of the last touch/launch */

/* demand cap and power limit, re-evaluated by gov_work */
#define GOV_MS			20
#define DEMAND_HEADROOM_PCT	25	/* above measured utilization */
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

static enum avr_mode cur_mode = AVR_BALANCED;	/* in effect */
static enum avr_mode base_mode = AVR_BALANCED;	/* chosen by the user */
static DEFINE_MUTEX(avr_lock);
static struct kobject *avr_kobj;

#define MAX_APP_PROFILES	64
#define NO_UID			((uid_t)-1)
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
 * Demand cap, re-evaluated every GOV_MS while the screen isn't touched.
 *
 * need = frequency the busiest CPU of the policy needs for its current load
 * (utilization is frequency-invariant, so util/capacity of max frequency)
 * plus DEMAND_HEADROOM_PCT. The cap
 *  - rises at once when need exceeds it,
 *  - jumps by half when a CPU is saturated at the cap (its util is then
 *    bounded by the cap itself, so need would under-read the real demand),
 *  - falls only after need stayed below 85% of it for DEMAND_FALL_TICKS,
 *    and then gradually, so short dips between frames don't cost speed.
 * The vendor governor can't overshoot it, so the same work runs at lower
 * frequency and voltage.
 */
static void demand_update(struct cpufreq_policy *policy)
{
	struct avr_policy_reqs *r = &reqs[policy->cpu];
	unsigned int fmax = policy->cpuinfo.max_freq;
	s32 floor = pct_of(fmax, DEMAND_FLOOR_PCT);
	s32 cur = r->dcap ? r->dcap : (s32)fmax;
	unsigned long need = 0;
	bool saturated = false;
	unsigned int cpu;
	s32 want;

	for_each_cpu(cpu, policy->cpus) {
		unsigned long cap = max(arch_scale_cpu_capacity(cpu), 1UL);
		unsigned long u = min(rk_cpu_util(cpu), cap);

		need = max(need, u * fmax / cap);
		/* capacity available at the cap: cap * dcap / fmax */
		if (u * 100 >= DEMAND_SAT_PCT * (cap * (unsigned long)cur / fmax))
			saturated = true;
	}
	want = clamp_t(s32, need * (100 + DEMAND_HEADROOM_PCT) / 100, floor, fmax);

	if (!r->dcap) {			/* fresh start: take the need as is */
		r->dcap = want;
		r->low_ticks = 0;
	} else if (saturated && r->dcap < (s32)fmax) {
		r->dcap = min_t(s32, fmax, r->dcap + r->dcap / 2);
		r->low_ticks = 0;
	} else if (want > r->dcap) {
		r->dcap = want;
		r->low_ticks = 0;
	} else if (want < r->dcap * 85 / 100) {
		if (++r->low_ticks >= DEMAND_FALL_TICKS)
			/* step down by at most 10% of max per tick */
			r->dcap = max_t(s32, want, r->dcap - (s32)(fmax / 10));
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
	if (m->demand && idle && r->dcap)
		max = min(max, r->dcap);
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

	freq_qos_update_request(&r->min, min);
	/*
	 * Caps are re-evaluated every GOV_MS: skip *lowering* them by less than
	 * ~3% of max so load jitter doesn't churn cpufreq limit updates. Any
	 * raise goes through at once, so performance is never held back.
	 */
	if (max < r->max.pnode.prio &&
	    r->max.pnode.prio - max < (s32)(fmax / 32))
		return;
	freq_qos_update_request(&r->max, max);
}

static void gov_fn(struct work_struct *work);
static DECLARE_DEFERRABLE_WORK(gov_work, gov_fn);

/* the loop is needed while a demand cap is live or a power limit is set */
static bool gov_needed(void)
{
	return (modes[cur_mode].demand && smart_armed && !smart_lifted) ||
	       READ_ONCE(power_limit_mw);
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

static void gov_fn(struct work_struct *work)
{
	static unsigned int tick;
	unsigned int limit = READ_ONCE(power_limit_mw);
	unsigned int mw = 0, cpu;
	bool measure = limit && ++tick % POWER_EVERY == 1;

	/* outside avr_lock: the battery driver may talk to firmware */
	if (measure)
		mw = battery_mw();

	mutex_lock(&avr_lock);
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;
		if (policy->cpu == cpu) {
			if (modes[cur_mode].demand && smart_armed && !smart_lifted)
				demand_update(policy);
			else
				reqs[cpu].dcap = 0;	/* re-measure when capping resumes */
		}
		cpufreq_cpu_put(policy);
	}
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
		/* demand caps start over from the load seen after the touch */
		for_each_possible_cpu(cpu)
			reqs[cpu].dcap = 0;
		apply_policies(&modes[cur_mode]);
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

	if (since < hold) {
		mod_delayed_work(system_unbound_wq, &smart_drop_work, hold - since);
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

	if (!READ_ONCE(per_app) || !READ_ONCE(nr_app_profiles))
		return;

	uid = from_kuid(&init_user_ns, task_uid(task));
	if (uid % 100000 < 10000)	/* system UIDs never decide this */
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
