// SPDX-License-Identifier: GPL-2.0
/*
 * App launch boost (avr).
 *
 * When zygote specializes a fork into an app (setresuid from root to an
 * app UID), give that first thread a uclamp.min floor for a short window so
 * the cold start runs on fast cores at high frequency from the first
 * instruction, before the framework's own hints arrive.
 *
 * The boost is not inherited: SCHED_FLAG_RESET_ON_FORK is set for the
 * window, so threads created during launch (RenderThread, binder threads,
 * ...) start unclamped. When the window ends the original clamp and
 * reset-on-fork state are restored, unless something else changed the
 * clamp meanwhile, in which case it is left alone.
 *
 *   /sys/module/launch_boost/parameters/enabled      (default Y)
 *   /sys/module/launch_boost/parameters/duration_ms  (default 1000)
 *   /sys/module/launch_boost/parameters/uclamp_min   (0..1024, default 1024)
 */

#include <linux/launch_boost.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/types.h>
#include <uapi/linux/sched/types.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "launch_boost."

#define AID_APP_START	10000	/* first app UID */
#define AID_APP_END	19999	/* last app UID; above are sandboxes/isolated */
#define AID_USER_OFFSET	100000	/* per-user UID range */

static bool enabled = true;
module_param(enabled, bool, 0644);

static unsigned int duration_ms = 1000;
module_param(duration_ms, uint, 0644);

static unsigned int uclamp_min = SCHED_CAPACITY_SCALE;
module_param(uclamp_min, uint, 0644);

void launch_boost_set_enabled(bool on)
{
	WRITE_ONCE(enabled, on);
}

struct launch_boost {
	struct delayed_work work;
	struct task_struct *p;
	unsigned int boost;
	unsigned int orig_min;
	bool orig_reset_on_fork;
};

static int set_clamp(struct task_struct *p, unsigned int min, bool reset_on_fork)
{
	struct sched_attr attr = {
		.size		= sizeof(attr),
		/* explicit policy: with KEEP_POLICY, reset_on_fork is ignored */
		.sched_policy	= p->policy,
		.sched_nice	= task_nice(p),
		.sched_priority	= p->rt_priority,
		.sched_flags	= SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN |
				  (reset_on_fork ? SCHED_FLAG_RESET_ON_FORK : 0),
		.sched_util_min	= min,
	};

	return sched_setattr_nocheck(p, &attr);
}

static void launch_boost_end(struct work_struct *work)
{
	struct launch_boost *lb = container_of(to_delayed_work(work),
					       struct launch_boost, work);
	struct task_struct *p = lb->p;

	/* uclamp_req is a bitfield; a racy read only risks skipping the restore */
	if (!(p->flags & PF_EXITING) &&
	    p->uclamp_req[UCLAMP_MIN].value == lb->boost)
		set_clamp(p, lb->orig_min, lb->orig_reset_on_fork);

	put_task_struct(p);
	kfree(lb);
}

static bool is_app_uid(uid_t uid)
{
	uid_t appid = uid % AID_USER_OFFSET;

	return appid >= AID_APP_START && appid <= AID_APP_END;
}

void launch_boost_setuid(uid_t old_uid, uid_t new_uid)
{
	struct task_struct *p = current;
	struct launch_boost *lb;
	unsigned int boost = min_t(unsigned int, READ_ONCE(uclamp_min),
				   SCHED_CAPACITY_SCALE);

	if (!READ_ONCE(enabled) || old_uid != 0 || !is_app_uid(new_uid) ||
	    !p->mm || (p->flags & PF_KTHREAD) || !boost)
		return;

	/* already at or above the floor, e.g. set by the framework */
	if (p->uclamp_req[UCLAMP_MIN].value >= boost)
		return;

	lb = kmalloc(sizeof(*lb), GFP_KERNEL);
	if (!lb)
		return;

	lb->orig_min = p->uclamp_req[UCLAMP_MIN].value;
	lb->orig_reset_on_fork = p->sched_reset_on_fork;
	lb->boost = boost;

	if (set_clamp(p, boost, true)) {
		kfree(lb);
		return;
	}

	get_task_struct(p);
	lb->p = p;
	INIT_DELAYED_WORK(&lb->work, launch_boost_end);
	queue_delayed_work(system_unbound_wq, &lb->work,
			   msecs_to_jiffies(READ_ONCE(duration_ms)));
}
