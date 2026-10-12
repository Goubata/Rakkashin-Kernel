/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RK_TBOOST_H
#define _LINUX_RK_TBOOST_H

struct cgroup_subsys_state;

#ifdef CONFIG_RK_TBOOST
void rk_tboost_set_top_app(struct cgroup_subsys_state *css);
void rk_tboost_set_headroom(unsigned int pct);
#else
static inline void rk_tboost_set_top_app(struct cgroup_subsys_state *css) { }
static inline void rk_tboost_set_headroom(unsigned int pct) { }
#endif

#endif /* _LINUX_RK_TBOOST_H */
