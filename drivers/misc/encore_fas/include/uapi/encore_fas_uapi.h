// SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note

#ifndef _UAPI_ENCORE_FAS_H
#define _UAPI_ENCORE_FAS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FAS_ABI_VERSION 3

#define FAS_MAX_PATH_LEN 256
#define FAS_MAX_TARGETS 8
#define FAS_MAX_LISTENERS 16

/* Configuration flag definitions. */
#define FAS_CFG_LOCK_DOWN (1u << 0)

/* Status flags for struct fas_state. */
#define FAS_STATE_ACQUIRING (1u << 0)
#define FAS_STATE_DEGRADED (1u << 1)
#define FAS_STATE_PAUSED (1u << 2)

/* Event flags for struct fas_event. */
#define FAS_EVF_WATCHDOG (1u << 0)

/**
 * @brief Module version information.
 */
struct fas_version {
	// Git commit count at build time.
	__u32 version;
	// Header ABI version (must equal FAS_ABI_VERSION).
	__u32 abi;
	// Frequency of system counter in Hz.
	__u32 counter_hz;
	// Reserved field.
	__u32 reserved;
};

/**
 * @brief Target configuration for listener.
 */
struct fas_config {
	// Configuration flags (FAS_CFG_*).
	__u32 flags;
	// Display vsync period in nanoseconds.
	__u32 vsync_ns;
	// Count of valid frame rates in fps array (1 to FAS_MAX_TARGETS).
	__u32 count;

	// Reserved field.
	__u32 reserved;

	// Configured frame rates in fps.
	__u32 fps[FAS_MAX_TARGETS];
};

/**
 * @brief Input structure for FAS_IOC_REGISTER command.
 */
struct fas_register_args {
	// Target process ID.
	__s32 pid;
	// Output listener ID assigned by module.
	__s32 ctx_id;
	// File offset of probed function.
	__u64 offset;
	// Initial configuration targets.
	struct fas_config cfg;
	// Path to target binary file.
	char path[FAS_MAX_PATH_LEN];
};

struct fas_remove_args {
	__s32 ctx_id;
};

struct fas_config_args {
	__s32 ctx_id;
	__u32 reserved;
	struct fas_config cfg;
};

/**
 * @brief Output structure for FAS_IOC_GET_STATE command.
 */
struct fas_state {
	// Listener ID.
	__s32 ctx_id;
	// Active target frame rate in fps.
	__u32 fps;
	// Status flags (FAS_STATE_*).
	__u32 flags;
	// Deficit pressure in Q16 format.
	__u32 pressure_q16;
	// Sequence number of last event.
	__u32 seq;
	// Count of dropped events due to full queue.
	__u32 dropped;
	// Reference interval of the hitch threshold in nanoseconds.
	__u32 ref_ns;
	// Hitch margin in nanoseconds. The soft watchdog fires at ref_ns + margin_ns.
	__u32 margin_ns;
	// Effective display vsync period in nanoseconds.
	__u32 vsync_ns;

	// Reserved field.
	__u32 reserved;
};

struct fas_listener_info {
	__s32 ctx_id;
	__s32 pid;
};

struct fas_listener_list {
	__u32 count;
	__u32 reserved;
	struct fas_listener_info listeners[FAS_MAX_LISTENERS];
};

enum fas_event_type {
	FAS_EVENT_NONE = 0,
	FAS_EVENT_SMALL_JANK = 1,
	FAS_EVENT_BIG_JANK = 2,
	FAS_EVENT_BOOST_SOFT = 3,
	FAS_EVENT_BOOST_HARD = 4,
	FAS_EVENT_DEGRADED = 5,
	FAS_EVENT_RECOVERED = 6,
	FAS_EVENT_PAUSED = 7,
	FAS_EVENT_RESUMED = 8,
	FAS_EVENT_RATE_SWITCH = 9,
};

/**
 * @brief Event record returned by read operations.
 */
struct fas_event {
	// Listener ID.
	__s32 ctx_id;
	// Event type (enum fas_event_type).
	__u32 type;
	// Event timestamp using CLOCK_MONOTONIC.
	__u64 timestamp_ns;
	// Frame interval in nanoseconds.
	__u64 frametime_ns;
	// Active frame rate target when event occurred.
	__u32 fps;
	// Count of missed frame slots.
	__u32 missed;
	// Event flags (FAS_EVF_*).
	__u32 flags;
	// Deficit pressure in Q16 format.
	__u32 pressure_q16;
	// Event sequence counter for listener.
	__u32 seq;
	// Reserved field.
	__u32 reserved;
} __attribute__((aligned(8)));

#define FAS_IOC_MAGIC 'F'

#define FAS_IOC_GET_VERSION _IOR(FAS_IOC_MAGIC, 0, struct fas_version)
#define FAS_IOC_REGISTER _IOWR(FAS_IOC_MAGIC, 1, struct fas_register_args)
#define FAS_IOC_REMOVE _IOW(FAS_IOC_MAGIC, 2, struct fas_remove_args)
#define FAS_IOC_SET_CONFIG _IOW(FAS_IOC_MAGIC, 3, struct fas_config_args)
#define FAS_IOC_GET_STATE _IOWR(FAS_IOC_MAGIC, 4, struct fas_state)
#define FAS_IOC_LIST _IOR(FAS_IOC_MAGIC, 5, struct fas_listener_list)

#define FAS_IOC_MAXNR 5

#endif
