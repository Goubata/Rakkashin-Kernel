// SPDX-License-Identifier: GPL-2.0
/*
 * Rakkashin kernel: avr performance profiles.
 *
 * One switch that moves several kernel knobs together:
 *
 *   echo game > /sys/kernel/avr_profile/mode
 *   cat /sys/kernel/avr_profile/mode        # "battery [balanced] performance game"
 *
 *   mode         cpufreq floor/cap  PELT   launch boost  writeback  thread boost
 *   battery      efficiency cap     32 ms  off           30 s       off
 *   balanced     -                  16 ms  on            15 s       off
 *   performance  floor 30% of max   8 ms   on            15 s       +15%
 *   game         floor 50% of max   8 ms   on            15 s       +25%
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
#include <linux/kobject.h>
#include <linux/launch_boost.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/rk_tboost.h>
#include <linux/sched/sysctl.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>

#define RAKKASHIN_VERSION	"Rakkashin Kernel v1.4 (avr, KernelSU-Next)"

extern int sched_pelt_set_multiplier(unsigned int mult);

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
	bool launch_boost;
	unsigned int writeback_cs;	/* dirty_writeback_interval, centiseconds */
	unsigned int tboost_pct;	/* foreground thread headroom boost, 0 = off */
};

static const char * const mode_names[AVR_NR_MODES] = {
	[AVR_BATTERY]	  = "battery",
	[AVR_BALANCED]	  = "balanced",
	[AVR_PERFORMANCE] = "performance",
	[AVR_GAME]	  = "game",
};

static const struct avr_mode_params modes[AVR_NR_MODES] = {
	[AVR_BATTERY]	  = { "battery",	 0,  70, true,  1, false, 3000,  0 },
	[AVR_BALANCED]	  = { "balanced",	 0, 100, false, 2, true,  1500,  0 },
	[AVR_PERFORMANCE] = { "performance",	30, 100, false, 4, true,  1500, 15 },
	[AVR_GAME]	  = { "game",		50, 100, false, 4, true,  1500, 25 },
};

struct avr_policy_reqs {
	bool added;
	struct freq_qos_request min;
	struct freq_qos_request max;
};

/* indexed by policy->cpu, the policy's owning CPU */
static struct avr_policy_reqs reqs[NR_CPUS];
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

/* Called with avr_lock held. */
static void apply_policy(struct cpufreq_policy *policy,
			 const struct avr_mode_params *m)
{
	struct avr_policy_reqs *r = &reqs[policy->cpu];
	unsigned int fmax = policy->cpuinfo.max_freq;
	s32 min = m->min_pct ? pct_of(fmax, m->min_pct) : FREQ_QOS_MIN_DEFAULT_VALUE;
	s32 max = m->max_pct < 100 ? pct_of(fmax, m->max_pct) : FREQ_QOS_MAX_DEFAULT_VALUE;
	bool from_em;

	if (m->eff_cap)
		max = battery_cap(policy, m->max_pct, &from_em);

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
	freq_qos_update_request(&r->max, max);
}

/* Called with avr_lock held. */
static void apply_mode(enum avr_mode mode)
{
	const struct avr_mode_params *m = &modes[mode];
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;
		if (policy->cpu == cpu)
			apply_policy(policy, m);
		cpufreq_cpu_put(policy);
	}

	sched_pelt_set_multiplier(m->pelt_mult);
	launch_boost_set_enabled(m->launch_boost);
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
		if (!r->added && cur_mode != AVR_BALANCED)
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
	pr_info("%s\n", RAKKASHIN_VERSION);
	return cpufreq_register_notifier(&avr_policy_nb, CPUFREQ_POLICY_NOTIFIER);
}
late_initcall(avr_profile_init);
