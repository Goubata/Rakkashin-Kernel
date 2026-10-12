// SPDX-License-Identifier: GPL-2.0
/*
 * Rakkashin Kernel: foreground thread headroom boost.
 *
 * cpufreq follows utilization after the fact, so when a game's frame work
 * jumps, the frame that jumps runs at the old, too-low frequency. While the
 * profile asks for it (performance, game), every period this finds the
 * heaviest threads of the foreground app (top-app cpuset) by their PELT
 * utilization (or util_est, if higher) and gives each a uclamp.min of
 * util + headroom, so frequency
 * and core placement already have spare capacity when the spike lands.
 *
 * Boosted threads get SCHED_FLAG_RESET_ON_FORK so threads they spawn don't
 * inherit the floor; a thread that falls out of the top set, or every thread
 * when the profile no longer asks for boosting, gets its own clamp back
 * (unless something else changed it meanwhile).
 *
 *   /sys/module/rk_tboost/parameters/enabled      master switch (default Y)
 *   /sys/module/rk_tboost/parameters/nr_threads   threads boosted (default 4)
 *   /sys/module/rk_tboost/parameters/period_ms    rescan period (default 50)
 *   /sys/module/rk_tboost/parameters/min_util     ignore lighter threads (64)
 */

#include <linux/cgroup.h>
#include <linux/moduleparam.h>
#include <linux/rk_tboost.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/types.h>
#include <uapi/linux/sched/types.h>
#include <linux/workqueue.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "rk_tboost."

#define MAX_SLOTS	8

static bool enabled = true;
module_param(enabled, bool, 0644);

static unsigned int nr_threads = 4;
module_param(nr_threads, uint, 0644);

static unsigned int period_ms = 50;
module_param(period_ms, uint, 0644);

static unsigned int min_util = 64;
module_param(min_util, uint, 0644);

struct slot {
	struct task_struct *p;	/* holds a task reference while set */
	unsigned int boost;	/* the clamp we set */
	unsigned int orig_min;
	bool orig_reset_on_fork;
};

static struct slot slots[MAX_SLOTS];	/* touched only by the work below */
static unsigned int headroom_pct;	/* 0 = off; set by the profile */
static struct cgroup_subsys_state *top_app;

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

static void release(struct slot *s)
{
	struct task_struct *p = s->p;

	/* only undo our own clamp; someone else may have set one since */
	if (!(p->flags & PF_EXITING) &&
	    p->uclamp_req[UCLAMP_MIN].value == s->boost)
		set_clamp(p, s->orig_min, s->orig_reset_on_fork);
	put_task_struct(p);
	s->p = NULL;
}

static void release_all(void)
{
	unsigned int i;

	for (i = 0; i < MAX_SLOTS; i++)
		if (slots[i].p)
			release(&slots[i]);
}

/*
 * Load of a thread: the larger of PELT's running average and its util_est
 * (the peak of recent activations), so a thread that just went idle between
 * frames still counts with the load it will bring back next frame.
 */
static unsigned long thread_util(struct task_struct *t)
{
	unsigned long avg = READ_ONCE(t->se.avg.util_avg);
	unsigned long est = READ_ONCE(t->se.avg.util_est) & ~UTIL_AVG_UNCHANGED;

	return max(avg, est);
}

static unsigned int target_of(unsigned long util, unsigned int headroom)
{
	unsigned long t = util + util * headroom / 100;

	return min_t(unsigned long, t, SCHED_CAPACITY_SCALE);
}

static void tboost_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(tboost_work, tboost_fn);

static void tboost_fn(struct work_struct *work)
{
	struct task_struct *top[MAX_SLOTS] = { };
	unsigned long top_util[MAX_SLOTS] = { };
	unsigned int headroom = READ_ONCE(headroom_pct);
	unsigned int n = min_t(unsigned int, READ_ONCE(nr_threads), MAX_SLOTS);
	unsigned int i, j;
	struct css_task_iter it;
	struct task_struct *t;

	if (!headroom) {
		release_all();
		return;
	}
	if (!READ_ONCE(enabled) || !n || !READ_ONCE(top_app)) {
		/* idle but wanted: check back slowly for re-enable / top-app */
		release_all();
		queue_delayed_work(system_unbound_wq, &tboost_work, HZ);
		return;
	}

	/* pick the n heaviest foreground threads */
	css_task_iter_start(top_app, 0, &it);
	while ((t = css_task_iter_next(&it))) {
		unsigned long u = thread_util(t);

		if (u < READ_ONCE(min_util) || (t->flags & (PF_EXITING | PF_KTHREAD)))
			continue;
		for (i = 0; i < n; i++) {
			if (u > top_util[i])
				break;
		}
		if (i == n)
			continue;
		if (top[n - 1])
			put_task_struct(top[n - 1]);
		for (j = n - 1; j > i; j--) {
			top[j] = top[j - 1];
			top_util[j] = top_util[j - 1];
		}
		get_task_struct(t);
		top[i] = t;
		top_util[i] = u;
	}
	css_task_iter_end(&it);

	/* drop slots whose thread is no longer among the heaviest */
	for (i = 0; i < MAX_SLOTS; i++) {
		bool keep = false;

		if (!slots[i].p)
			continue;
		for (j = 0; j < n; j++)
			keep |= top[j] == slots[i].p;
		if (!keep)
			release(&slots[i]);
	}

	/* boost the current top set, re-targeting existing slots */
	for (j = 0; j < n && top[j]; j++) {
		unsigned int want = target_of(top_util[j], headroom);
		struct slot *s = NULL, *free = NULL;

		for (i = 0; i < MAX_SLOTS; i++) {
			if (slots[i].p == top[j])
				s = &slots[i];
			else if (!slots[i].p && !free)
				free = &slots[i];
		}

		if (s) {
			/* still ours? then follow the thread's load (with hysteresis) */
			if (top[j]->uclamp_req[UCLAMP_MIN].value == s->boost &&
			    abs((int)want - (int)s->boost) > 32 &&
			    !set_clamp(top[j], want, true))
				s->boost = want;
		} else if (free && top[j]->uclamp_req[UCLAMP_MIN].value < want) {
			free->orig_min = top[j]->uclamp_req[UCLAMP_MIN].value;
			free->orig_reset_on_fork = top[j]->sched_reset_on_fork;
			if (!set_clamp(top[j], want, true)) {
				get_task_struct(top[j]);
				free->p = top[j];
				free->boost = want;
			}
		}
		put_task_struct(top[j]);
	}

	queue_delayed_work(system_unbound_wq, &tboost_work,
			   msecs_to_jiffies(max(READ_ONCE(period_ms), 10U)));
}

void rk_tboost_set_top_app(struct cgroup_subsys_state *css)
{
	WRITE_ONCE(top_app, css);
}

/* headroom in percent above current utilization; 0 stops boosting */
void rk_tboost_set_headroom(unsigned int pct)
{
	WRITE_ONCE(headroom_pct, pct);
	/* starts the loop, or lets a running one notice and clean up */
	mod_delayed_work(system_unbound_wq, &tboost_work, 0);
}
