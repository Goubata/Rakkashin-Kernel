/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_AVR_PROFILE_H
#define _LINUX_AVR_PROFILE_H

struct cgroup_subsys_state;
struct task_struct;

#ifdef CONFIG_AVR_PROFILE
void avr_profile_cpuset_attach(struct cgroup_subsys_state *css,
			       struct task_struct *task);
void avr_profile_kick(void);
/* a frame of the foreground app was late (1) or badly late (2), from FAS */
void avr_profile_frame_event(unsigned int level);
void avr_profile_frame_seen(void);
#else
static inline void avr_profile_cpuset_attach(struct cgroup_subsys_state *css,
					     struct task_struct *task) { }
static inline void avr_profile_kick(void) { }
static inline void avr_profile_frame_event(unsigned int level) { }
static inline void avr_profile_frame_seen(void) { }
#endif

#endif /* _LINUX_AVR_PROFILE_H */
