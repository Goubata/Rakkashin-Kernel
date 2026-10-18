// SPDX-License-Identifier: GPL-2.0-only

#ifndef ENCORE_FAS_TIME_H
#define ENCORE_FAS_TIME_H

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/time64.h>
#include <linux/types.h>

/**
 * @brief System counter frequency parameters.
 */
struct fas_clock {
	/** Counter frequency in Hz. */
	u64 freq;
	/** Multiplier for tick-to-nanosecond conversion. */
	u64 mult;
	/** Maximum tick value to prevent calculation overflow. */
	u64 max_ticks;
	/** Shift count for tick-to-nanosecond conversion. */
	u32 shift;
};

extern struct fas_clock fas_clk;

/**
 * @brief Reads current virtual counter value.
 *
 * @return Counter value in ticks.
 */
static __always_inline u64 fas_ticks(void)
{
	u64 val;

	asm volatile("mrs %0, cntvct_el0" : "=r"(val));
	return val;
}

/**
 * @brief Converts timer ticks to nanoseconds.
 *
 * @param ticks Input value in ticks. Clamped to max_ticks.
 * @return Equivalent duration in nanoseconds.
 */
static __always_inline u64 fas_ticks_to_ns(u64 ticks)
{
	return (min(ticks, fas_clk.max_ticks) * fas_clk.mult) >> fas_clk.shift;
}

int fas_time_init(void);
u64 fas_ns_to_ticks(u64 ns);

#endif
