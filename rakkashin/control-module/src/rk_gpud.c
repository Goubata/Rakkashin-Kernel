// SPDX-License-Identifier: GPL-2.0
/*
 * rk_gpud - Rakkashin Kernel GPU demand governor.
 *
 * The GPU counterpart of the kernel's demand-matched CPU cap: while
 * /sys/kernel/avr_profile/demand_active is 1 (Battery or Smart Balanced, the
 * screen not being touched) or 2 (touched, Sustained performance engaged
 * because the phone is warm), cap the GPU at the lowest frequency step that
 * still covers its measured load plus headroom, so the vendor governor can't
 * overshoot to steps the frame work doesn't need. Same work, lower clock and
 * voltage.
 *
 * kgsl's gpu_busy is an average that is refreshed only about once a second,
 * so decisions are made once per fresh reading, against the clock averaged
 * over the same time (the clock is sampled every TICK_MS):
 *  - rises at once when the load needs more,
 *  - when the GPU is saturated at the cap, jumps to the top step while
 *    touched, halfway there otherwise,
 *  - untouched, falls to the need with the next reading; touched, starts at
 *    the top and comes down two steps per reading only after two readings
 *    in a row asked for less, so a game never waits on a stale reading,
 *  - restores the original max clock whenever demand_active drops to 0
 *    (touch with Sustained off or cool, Performance/Game/Extreme) and on
 *    exit.
 *
 * While inactive it sleeps in poll() on demand_active (the kernel
 * sysfs_notify()s it), so it costs nothing; while active it samples every
 * TICK_MS, backing off to IDLE_TICK_MS when the GPU has been idle.
 *
 * Interface (kgsl): /sys/kernel/gpu/{gpu_freq_table,gpu_max_clock,
 * gpu_min_clock,gpu_busy,gpu_clock}; clocks in MHz.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* overridable for testing against a mock tree */
#ifndef GPU
#define GPU		"/sys/kernel/gpu/"
#endif
#ifndef ACTIVE
#define ACTIVE		"/sys/kernel/avr_profile/demand_active"
#endif
#ifndef SUST_STATE
#define SUST_STATE	"/sys/kernel/avr_profile/sustained_state"
#endif
#ifndef STATE
#define STATE		"/data/adb/rakkashin/gpu_orig_max"
#endif

#define TICK_MS		50
#define IDLE_TICK_MS	500
#define IDLE_READINGS	2	/* readings of an idle GPU before backing off */
#define HEADROOM_PCT	25
#define TOUCH_HEADROOM_PCT 50	/* touched, if the kernel doesn't say */
#define TOUCH_HEADROOM_MIN 40	/* busy is a 1 s average: keep room for peaks */
#define SAT_PCT		85
#define TOUCH_FALL_READINGS 2	/* touched: readings in a row asking for less */
#define TOUCH_FALL_STEPS 2	/* ...then this many steps per reading */
#define STALE_MS	1500	/* an unchanged busy value counts as a new reading */
#define MAX_STEPS	64

static long steps[MAX_STEPS];	/* MHz, ascending */
static int nsteps;
static long orig_max;
static volatile sig_atomic_t quit;

static void klog(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		dprintf(fd, "rk_gpud: %s\n", msg);
		close(fd);
	}
}

static int read_text(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

static long read_long(const char *path)
{
	char buf[64];

	if (read_text(path, buf, sizeof(buf)))
		return -1;
	return strtol(buf, NULL, 10);
}

static int write_long(const char *path, long v)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int ret;

	if (fd < 0)
		return -1;
	ret = dprintf(fd, "%ld\n", v) > 0 ? 0 : -1;
	close(fd);
	return ret;
}

/* the state file is ours to create, unlike sysfs attributes */
static void save_state(long v)
{
	int fd = open(STATE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd >= 0) {
		dprintf(fd, "%ld\n", v);
		close(fd);
	}
}

/* Hz or kHz values are folded to MHz so they match gpu_max_clock. */
static long to_mhz(long v)
{
	while (v > 100000)
		v /= 1000;
	return v;
}

static int cmp_long(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return (x > y) - (x < y);
}

static int load_steps(void)
{
	char buf[1024], *p, *end;

	if (read_text(GPU "gpu_freq_table", buf, sizeof(buf)))
		return -1;
	for (p = buf; nsteps < MAX_STEPS; p = end) {
		long v = strtol(p, &end, 10);

		if (end == p)
			break;
		if (v > 0)
			steps[nsteps++] = to_mhz(v);
	}
	qsort(steps, nsteps, sizeof(steps[0]), cmp_long);
	return nsteps >= 2 ? 0 : -1;
}

/* Lowest step >= mhz; the top step if none is. */
static int step_for(long mhz)
{
	int i;

	for (i = 0; i < nsteps; i++)
		if (steps[i] >= mhz)
			return i;
	return nsteps - 1;
}

/* GPU busy percent; gpu_busy is "N %" or "busy total". */
static long gpu_busy(void)
{
	char buf[64], *end;
	long a, b;

	if (read_text(GPU "gpu_busy", buf, sizeof(buf)))
		return -1;
	a = strtol(buf, &end, 10);
	b = strtol(end, NULL, 10);
	if (b > 0 && !strchr(buf, '%'))
		return a * 100 / b;
	return a;
}

static void set_max(long mhz, long *cur)
{
	if (mhz != *cur && !write_long(GPU "gpu_max_clock", mhz))
		*cur = mhz;
}

static void restore(long *cur)
{
	if (orig_max > 0)
		set_max(orig_max, cur);
}

/* demand_active: 0 off, 1 untouched, 2 touched (Sustained performance) */
static int read_active(int fd)
{
	char c = '0';

	if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, &c, 1) != 1)
		return 0;
	return c == '1' ? 1 : c == '2' ? 2 : 0;
}

/*
 * Headroom while touched: the kernel's, which shrinks as the phone gets
 * hotter, but never below TOUCH_HEADROOM_MIN.
 */
static long touch_headroom(void)
{
	char buf[96], *p;
	long hr;

	if (read_text(SUST_STATE, buf, sizeof(buf)))
		return TOUCH_HEADROOM_PCT;
	p = strstr(buf, "headroom=");
	if (!p)
		return TOUCH_HEADROOM_PCT;
	hr = strtol(p + 9, NULL, 10);
	return hr > TOUCH_HEADROOM_MIN ? hr : TOUCH_HEADROOM_MIN;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

int main(void)
{
	struct sigaction sa = { .sa_handler = on_signal };
	long cur_max, saved, last_busy = -1, last_eval = 0, clk_sum = 0;
	int afd, active = 0, check = 1, cap = -1, low = 0, idle = 0, nclk = 0;

	if (load_steps()) {
		klog("no usable gpu_freq_table, exiting");
		return 1;
	}

	/*
	 * The uncapped value: kept in STATE across restarts so a crash while
	 * capped can't make a lowered value the new "original".
	 */
	saved = read_long(STATE);
	orig_max = saved > 0 ? saved : read_long(GPU "gpu_max_clock");
	if (orig_max <= 0)
		orig_max = steps[nsteps - 1];
	if (saved <= 0)
		save_state(orig_max);
	cur_max = read_long(GPU "gpu_max_clock");

	afd = open(ACTIVE, O_RDONLY | O_CLOEXEC);
	if (afd < 0) {
		klog("kernel has no demand_active, exiting");
		return 1;
	}

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	klog("started");

	while (!quit) {
		struct pollfd pfd = { .fd = afd, .events = POLLPRI | POLLERR };
		int timeout = !active ? -1 : idle >= IDLE_READINGS ? IDLE_TICK_MS : TICK_MS;

#ifdef POLL_EVERY_TICK
		if (!active)
			timeout = TICK_MS;
#endif

		/* inactive: sleep until the kernel notifies demand_active */
		if (!check) {
			int r = poll(&pfd, 1, timeout);

			if (r < 0 && errno != EINTR)
				break;
			check = r > 0;
#ifdef POLL_EVERY_TICK	/* tests: a regular file never signals POLLPRI */
			check = 1;
#endif
		}
		if (check) {
			int now = read_active(afd);

			check = 0;
			if (now != active) {
				int was = active;

				active = now;
				idle = 0;
				if (!active) {
					restore(&cur_max);
					cap = -1;
					continue;
				}
				/*
				 * Starting, or a touch: the work it brings isn't
				 * in any reading yet, so start from the top and
				 * let fresh readings bring the cap down.
				 */
				if (!was || active == 2) {
					cap = nsteps - 1;
					low = 0;
					/*
					 * untouched: use the current reading;
					 * touched: it predates the touch, wait
					 * for the next one
					 */
					last_busy = active == 2 ? gpu_busy() : -1;
					last_eval = now_ms();
					clk_sum = 0;
					nclk = 0;
				}
			}
		}
		if (!active || quit)
			continue;

		{
			long clk = read_long(GPU "gpu_clock");
			long busy = gpu_busy();
			long t = now_ms(), avg;
			int touched = active == 2, want;

			if (clk > 0) {
				clk_sum += clk;
				nclk++;
			}
			if (busy < 0)
				continue;
			/* act on a fresh reading only */
			if (busy == last_busy && t - last_eval < STALE_MS)
				goto apply;
			last_busy = busy;
			last_eval = t;
			idle = busy ? 0 : idle + 1;

			/* the clock the reading was taken at */
			avg = nclk ? clk_sum / nclk : steps[cap];
			clk_sum = 0;
			nclk = 0;

			/* frequency the current work needs, plus headroom */
			want = step_for(avg * busy / 100 *
					(100 + (touched ? touch_headroom() : HEADROOM_PCT)) / 100);

			if (busy >= SAT_PCT && avg * 100 >= steps[cap] * 90 &&
			    cap < nsteps - 1) {
				cap = touched ? nsteps - 1 : (cap + nsteps) / 2;
				low = 0;
			} else if (want > cap) {
				cap = want;
				low = 0;
			} else if (want < cap) {
				if (!touched) {
					cap = want;
				} else if (++low >= TOUCH_FALL_READINGS) {
					cap -= TOUCH_FALL_STEPS;
					if (cap < want)
						cap = want;
				}
			} else {
				low = 0;
			}
apply:
			/* never above what the user/system had set */
			set_max(steps[cap] < orig_max ? steps[cap] : orig_max, &cur_max);
		}
	}

	restore(&cur_max);
	klog("stopped");
	return 0;
}
