// SPDX-License-Identifier: GPL-2.0
/*
 * Boeffla generic wakelock blocker
 *
 * Original driver by andip71 (v1.1.0, 2017), as carried by AGNi and NGK.
 * Reworked for 6.12: the block list is published through RCU so that
 * wakeup_source_report_event(), which runs with ws->lock held and IRQs
 * off, never reads a half-written string.
 *
 * sysfs, unchanged from the original:
 *   /sys/class/misc/boeffla_wakelock_blocker/wakelock_blocker
 *   /sys/class/misc/boeffla_wakelock_blocker/wakelock_blocker_default
 *	semicolon separated wakelock names; both lists are blocked
 *   /sys/class/misc/boeffla_wakelock_blocker/debug
 *	write 0/1 to log every request/block to dmesg, read for internals
 *   /sys/class/misc/boeffla_wakelock_blocker/version
 */

#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/pm_wakeup.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "power.h"

#define BOEFFLA_WL_BLOCKER_VERSION	"1.1.0-6.12"

#define LIST_WL_DEFAULT		"qcom_rx_wakelock;wlan;wlan_wow_wl;wlan_extscan_wl;netmgr_wl;NETLINK"

#define LENGTH_LIST_WL		255
#define LENGTH_LIST_WL_DEFAULT	100
/* wakelocks with longer names are never blocked, as in the original */
#define MAX_WL_NAME		50

struct wl_search {
	struct rcu_head rcu;
	/* ";default;user;" so a lookup is a single strstr() for ";name;" */
	char str[];
};

static struct wl_search __rcu *wl_search;
static DEFINE_MUTEX(wl_lock);
static char list_wl[LENGTH_LIST_WL + 1];
static char list_wl_default[LENGTH_LIST_WL_DEFAULT + 1] = LIST_WL_DEFAULT;
static bool wl_blocker_debug;

/* Called with wl_lock held. */
static int rebuild_search(void)
{
	struct wl_search *new = NULL, *old;
	size_t len = strlen(list_wl_default) + strlen(list_wl) + 4;

	/* Both lists empty: publish NULL so the hot path bails out early. */
	if (list_wl_default[0] || list_wl[0]) {
		new = kmalloc(sizeof(*new) + len, GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		snprintf(new->str, len, ";%s;%s;", list_wl_default, list_wl);
	}

	old = rcu_replace_pointer(wl_search, new, lockdep_is_held(&wl_lock));
	if (old)
		kfree_rcu(old, rcu);
	return 0;
}

bool boeffla_wl_blocked(struct wakeup_source *ws)
{
	struct wl_search *s;
	char needle[MAX_WL_NAME + 3];
	size_t len;
	bool blocked = false;

	if (unlikely(wl_blocker_debug))
		pr_info("Boeffla WL blocker: %s requested\n", ws->name);

	if (likely(!rcu_access_pointer(wl_search)))
		return false;

	len = strlen(ws->name);
	if (len < 1 || len > MAX_WL_NAME)
		return false;
	snprintf(needle, sizeof(needle), ";%s;", ws->name);

	rcu_read_lock();
	s = rcu_dereference(wl_search);
	if (s)
		blocked = strstr(s->str, needle) != NULL;
	rcu_read_unlock();

	if (unlikely(wl_blocker_debug) && blocked)
		pr_info("Boeffla WL blocker: %s blocked\n", ws->name);

	return blocked;
}

static ssize_t store_list(char *dst, size_t size, const char *buf, size_t n)
{
	int ret;

	if (n > size - 1)
		return -EINVAL;

	mutex_lock(&wl_lock);
	/* like the original sscanf("%s"): first whitespace-free token only */
	if (sscanf(buf, "%255s", dst) != 1)
		dst[0] = '\0';
	dst[size - 1] = '\0';
	ret = rebuild_search();
	mutex_unlock(&wl_lock);

	return ret ? ret : n;
}

static ssize_t wakelock_blocker_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", list_wl);
}

static ssize_t wakelock_blocker_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t n)
{
	return store_list(list_wl, sizeof(list_wl), buf, n);
}

static ssize_t wakelock_blocker_default_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%s\n", list_wl_default);
}

static ssize_t wakelock_blocker_default_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t n)
{
	return store_list(list_wl_default, sizeof(list_wl_default), buf, n);
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct wl_search *s;
	ssize_t ret;

	rcu_read_lock();
	s = rcu_dereference(wl_search);
	ret = sysfs_emit(buf,
			 "Debug status: %d\n\nUser list: %s\nDefault list: %s\nSearch list: %s\nActive: %d\n",
			 wl_blocker_debug, list_wl, list_wl_default,
			 s ? s->str : "", !!s);
	rcu_read_unlock();
	return ret;
}

static ssize_t debug_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	WRITE_ONCE(wl_blocker_debug, val == 1);
	return count;
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%s\n", BOEFFLA_WL_BLOCKER_VERSION);
}

static DEVICE_ATTR_RW(wakelock_blocker);
static DEVICE_ATTR_RW(wakelock_blocker_default);
static DEVICE_ATTR(debug, 0664, debug_show, debug_store);
static DEVICE_ATTR_RO(version);

static struct attribute *boeffla_wl_blocker_attrs[] = {
	&dev_attr_wakelock_blocker.attr,
	&dev_attr_wakelock_blocker_default.attr,
	&dev_attr_debug.attr,
	&dev_attr_version.attr,
	NULL
};
ATTRIBUTE_GROUPS(boeffla_wl_blocker);

static struct miscdevice boeffla_wl_blocker_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "boeffla_wakelock_blocker",
	.groups = boeffla_wl_blocker_groups,
};

static int __init boeffla_wl_blocker_init(void)
{
	int ret;

	mutex_lock(&wl_lock);
	ret = rebuild_search();
	mutex_unlock(&wl_lock);
	if (ret)
		return ret;

	ret = misc_register(&boeffla_wl_blocker_device);
	if (ret)
		return ret;

	pr_info("Boeffla WL blocker: driver version %s started\n",
		BOEFFLA_WL_BLOCKER_VERSION);
	return 0;
}
late_initcall(boeffla_wl_blocker_init);
