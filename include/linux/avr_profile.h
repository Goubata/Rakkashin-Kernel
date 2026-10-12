/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_AVR_PROFILE_H
#define _LINUX_AVR_PROFILE_H

struct cgroup_subsys_state;
struct task_struct;

#ifdef CONFIG_AVR_PROFILE
void avr_profile_cpuset_attach(struct cgroup_subsys_state *css,
			       struct task_struct *task);
#else
static inline void avr_profile_cpuset_attach(struct cgroup_subsys_state *css,
					     struct task_struct *task) { }
#endif

#endif /* _LINUX_AVR_PROFILE_H */
