// SPDX-License-Identifier: GPL-2.0
/*
 * avr performance profiles.
 *
 * One switch that moves several kernel knobs together:
 *
 *   echo game > /sys/kernel/avr_profile/mode
 *   cat /sys/kernel/avr_profile/mode        # "battery [balanced] performance game"
 *
 *   mode         cpufreq floor/cap     PELT half-life  launch boost  writeback
 *   battery      cap 70% of max        32 ms           off           30 s
 *   balanced     -                     16 ms           on            15 s
 *   performance  floor 30% of max      8 ms            on            15 s
 *   game         floor 50% of max      8 ms            on            15 s
 *
 * CPU frequency is steered only through cpufreq's frequency QoS, the same
 * mechanism thermal mitigation uses. QoS takes the lowest max request and
 * the highest min request, and the policy max always wins over min, so a
 * thermal cap is never overridden by a floor set here.
 */

#include <linux/cpufreq.h>
#include <linux/kobject.h>
#include <linux/launch_boost.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/sched/sysctl.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/writeback.h>

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
	unsigned int pelt_mult;		/* 1: 32ms, 2: 16ms, 4: 8ms half-life */
	bool launch_boost;
	unsigned int writeback_cs;	/* dirty_writeback_interval, centiseconds */
};

static const char * const mode_names[AVR_NR_MODES] = {
	[AVR_BATTERY]	  = "battery",
	[AVR_BALANCED]	  = "balanced",
	[AVR_PERFORMANCE] = "performance",
	[AVR_GAME]	  = "game",
};

static const struct avr_mode_params modes[AVR_NR_MODES] = {
	[AVR_BATTERY]	  = { "battery",	 0,  70, 1, false, 3000 },
	[AVR_BALANCED]	  = { "balanced",	 0, 100, 2, true,  1500 },
	[AVR_PERFORMANCE] = { "performance",	30, 100, 4, true,  1500 },
	[AVR_GAME]	  = { "game",		50, 100, 4, true,  1500 },
};

struct avr_policy_reqs {
	bool added;
	struct freq_qos_request min;
	struct freq_qos_request max;
};

/* indexed by policy->cpu, the policy's owning CPU */
static struct avr_policy_reqs reqs[NR_CPUS];
static enum avr_mode cur_mode = AVR_BALANCED;
static DEFINE_MUTEX(avr_lock);

static s32 pct_of(unsigned int max_freq, unsigned int pct)
{
	return (s32)div_u64((u64)max_freq * pct, 100);
}

/* Called with avr_lock held. */
static void apply_policy(struct cpufreq_policy *policy,
			 const struct avr_mode_params *m)
{
	struct avr_policy_reqs *r = &reqs[policy->cpu];
	unsigned int fmax = policy->cpuinfo.max_freq;
	s32 min = m->min_pct ? pct_of(fmax, m->min_pct) : FREQ_QOS_MIN_DEFAULT_VALUE;
	s32 max = m->max_pct < 100 ? pct_of(fmax, m->max_pct) : FREQ_QOS_MAX_DEFAULT_VALUE;

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

	cur_mode = mode;
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
	apply_mode(i);
	mutex_unlock(&avr_lock);

	return count;
}

static struct kobj_attribute mode_attr = __ATTR_RW(mode);

static struct attribute *avr_profile_attrs[] = {
	&mode_attr.attr,
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

	return cpufreq_register_notifier(&avr_policy_nb, CPUFREQ_POLICY_NOTIFIER);
}
late_initcall(avr_profile_init);
