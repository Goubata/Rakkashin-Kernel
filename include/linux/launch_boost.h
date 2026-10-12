/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_LAUNCH_BOOST_H
#define _LINUX_LAUNCH_BOOST_H

#include <linux/types.h>

#ifdef CONFIG_LAUNCH_BOOST
void launch_boost_setuid(uid_t old_uid, uid_t new_uid);
void launch_boost_set_enabled(bool on);
#else
static inline void launch_boost_setuid(uid_t old_uid, uid_t new_uid) { }
static inline void launch_boost_set_enabled(bool on) { }
#endif

#endif /* _LINUX_LAUNCH_BOOST_H */
