// SPDX-License-Identifier: GPL-2.0
/*
 * rk_gpud - Rakkashin Kernel GPU demand governor and frame watcher.
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
 *  - comes down two steps per reading only after two readings in a row
 *    asked for less (straight to the need when the GPU is nearly idle);
 *    touched, it starts from the top, so a game never waits on a stale
 *    reading,
 *  - verifies every fall: at a lower clock the same work should look busier
 *    in proportion. If the next readings are clearly busier than that, the
 *    frames no longer fit (the GPU also waits on the CPU and vsync, so busy%
 *    alone under-reads what a frame deadline needs): the cap goes back up and
 *    that step becomes a floor. A 1 s reading taken across the fall mixes
 *    both clocks, so it may not reach SAT_PCT even when frames drop.
 *  - learns from saturation the same way: the step that saturated or failed
 *    a fall is remembered, together with the GPU work (busy% x clock) at
 *    that time. The cap stays above it for FLOOR_HOLD_MS, doubled each time
 *    it is hit again soon after (up to FLOOR_HOLD_MAX_MS); after that the
 *    floor is probed one step lower every PROBE_MS, but only once the work
 *    has dropped by PROBE_WORK_PCT (or, with frame timing, while every
 *    frame is on time), so a steady scene is not re-tested for nothing.
 *    It is forgotten after FLOOR_FORGET_MS of an idle GPU. Without this the
 *    cap hunts: too low, saturate, jump, look roomy, fall back, and the
 *    frame rate flips between smooth and choppy,
 *  - restores the original max clock whenever demand_active drops to 0
 *    (touch with Sustained off or cool, Performance/Game/Extreme) and on
 *    exit.
 *
 * Frame timing (needs /dev/encore_fas from the kernel): every app process
 * in top-app (up to MAX_LISTENERS, rescanned when top_pid changes and every
 * RESCAN_MS) is registered with encore_fas, which times its
 * Surface::queueBuffer calls. top-app also holds processes that never draw
 * (a game's bound ad service, a shell child), so the one whose frames
 * arrive becomes the watched app; the others stay silent. A late frame,
 * an overdue frame or a frame rate deficit while the GPU ran at its cap
 * lifts the cap at once (to the top for a bad one) and learns a floor; a
 * jank that the cap didn't cause changes nothing. While frames fall short
 * of their rate the cap never comes down. While frames are timed a late one
 * is caught at once, so the headroom is trimmed adaptively: a point less
 * after every CUT_STEP_MS of on-time frames (up to CUT_MAX), CUT_BACK points
 * back whenever the GPU cap held a frame back. What was learned (floor and
 * trim) is remembered for the last APP_MEMORY apps by UID. The kernel gets
 * the same events directly and does the same for the CPU caps.
 *
 * Either part can be turned off: "0" in CONF "gpu" (GPU caps) or CONF "fas"
 * (frame timing); with GPU caps off it only keeps the foreground app
 * registered, for the kernel's CPU caps.
 *
 * While inactive it sleeps in poll() on demand_active (the kernel
 * sysfs_notify()s it), so it costs nothing; while active it samples every
 * TICK_MS, backing off to IDLE_TICK_MS when the GPU has been idle.
 *
 * Interface (kgsl): /sys/kernel/gpu/{gpu_freq_table,gpu_max_clock,
 * gpu_min_clock,gpu_busy,gpu_clock}; clocks in MHz.
 */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "encore_fas_uapi.h"

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
#ifndef TOP_PID
#define TOP_PID		"/sys/kernel/avr_profile/top_pid"
#endif
#ifndef FAS_DEV
#define FAS_DEV		"/dev/encore_fas"
#endif
#ifndef LIBGUI
#define LIBGUI		"/system/lib64/libgui.so"
#endif
#ifndef STATE
#define STATE		"/data/adb/rakkashin/gpu_orig_max"
#endif
#ifndef FAS_STATE
#define FAS_STATE	"/data/adb/rakkashin/fas_state"
#endif
#ifndef CONF
#define CONF		"/data/adb/rakkashin/"	/* "gpu", "fas": "0" = off */
#endif
#ifndef LOGFILE
#define LOGFILE		"/data/adb/rakkashin/rakkashin.log"	/* shared with the scripts */
#endif
#define LOG_MAX		65536

#define TICK_MS		50
#define IDLE_TICK_MS	500
#define IDLE_READINGS	2	/* readings of an idle GPU before backing off */
#define HEADROOM_PCT	25
#define TOUCH_HEADROOM_PCT 50	/* touched, if the kernel doesn't say */
#define TOUCH_HEADROOM_MIN 40	/* busy is a 1 s average: keep room for peaks */
#define SAT_PCT		85
#define STALE_MS	1500	/* an unchanged busy value counts as a new reading */
#define FALL_READINGS	2	/* readings in a row asking for less before falling */
#define FALL_STEPS	2	/* ...then at most this many steps per reading */
#define IDLE_BUSY	10	/* below this the GPU is nearly idle: fall at once */
#define FLOOR_HOLD_MS	10000	/* keep above a saturated step this long */
#define FLOOR_HOLD_MAX_MS 120000
#define PROBE_MS	5000	/* then try one step lower this often... */
#define PROBE_WORK_PCT	15	/* ...if the work fell by this much since */
#define FLOOR_FORGET_MS	30000	/* idle this long: the game is gone */
#define VERIFY_READINGS	2	/* readings after a fall that must look right */
#define VERIFY_MARGIN	8	/* busy points above the proportional estimate */
#define MAX_STEPS	64

/* frame timing */
#define REG_DELAY_MS	500	/* let a foreground switch settle before probing */
#define RESCAN_MS	5000	/* look at top-app this often anyway */
#define MAX_LISTENERS	8
#define PRIMARY_IDLE_MS	1000	/* another app's frames take over after this */
#define JANK_BOUND_PCT	90	/* the GPU ran this close to the cap: it held the frame */
#define FAS_STATE_MS	2000	/* write fas_state at most this often */
#define FAS_TOUCH_HEADROOM_MIN 30	/* touched floor of the trim, see TOUCH_HEADROOM_MIN */
#define FAS_HEADROOM_MIN 10	/* untouched floor of the trim */
#define CUT_START	5	/* headroom trim for an app we haven't seen */
#define CUT_MAX		20
#define CUT_BACK	10
#define CUT_STEP_MS	5000
#define APP_MEMORY	8

static long steps[MAX_STEPS];	/* MHz, ascending */
static int nsteps;
static long orig_max;
static volatile sig_atomic_t quit;

/* the governor */
static struct {
	int active;		/* demand_active: 0, 1 untouched, 2 touched */
	int cap;		/* step index, -1 = uncapped */
	long cur_max;		/* gpu_max_clock as last written */
	int low;		/* readings in a row asking for less */
	int idle;		/* idle readings in a row */
	long last_busy, last_eval;
	long clk_sum;		/* clock samples since the last reading */
	int nclk;
	long last_clk;		/* latest clock sample */
	/* last fall, being verified: the step it came from, the busy% expected */
	int fell_from, verify;
	long expect, fell_work;
	long work;		/* busy% x MHz of the last reading */
} g = { .cap = -1, .last_busy = -1, .fell_from = -1 };

/* learned floor: the lowest step known not to saturate, see the top */
static int floor_i;		/* 0 = none */
static long floor_until;	/* hold the floor until then (ms) */
static long floor_hold = FLOOR_HOLD_MS;
static long floor_work;		/* busy% x MHz when the floor was learned */
static long idle_since = -1;	/* when the GPU went nearly idle (ms) */

/* frame timing (encore_fas) */
/* adaptive headroom trim while frames are timed */
static int cut = CUT_START;
static long cut_since;		/* ms of the last trim step or jank */

/* what was learned per app, by UID */
static struct {
	long uid;		/* -1 = free */
	int floor_i, cut;
	long floor_hold, floor_work, used;
} app_mem[APP_MEMORY] = { [0 ... APP_MEMORY - 1] = { .uid = -1 } };
static long mem_uid = -1;	/* UID whose state is live */

static struct {
	int fd;			/* /dev/encore_fas, -1 = off */
	uint64_t offset;	/* Surface::queueBuffer in LIBGUI */
	int ctx;		/* listener of the app whose frames we follow, -1 = none */
	int pid;		/* its process */
	long last_ev;		/* ms of its last event */
	long reg_at;		/* ms of the next top-app scan, 0 = none pending */
	long synced_at;		/* ms of the last scan */
	int degraded;		/* frame rate below its target right now */
	int paused;		/* the app stopped drawing (loading, background) */
	unsigned int fps;	/* target frame rate the detector settled on */
	long last_jank;		/* ms */
	unsigned long janks;	/* late frames that lifted the GPU cap */
	unsigned long frames_late; /* all late-frame events since start */
	unsigned long app_late;	/* late-frame events of the app being watched */
	unsigned long app_events; /* all events of the app being watched */
	long state_at;		/* ms of the last fas_state write */
	int state_dirty;
} fas = { .fd = -1, .ctx = -1 };

/* every app process in top-app is registered; see the top */
static struct {
	int pid, ctx;
	long uid;
} lis[MAX_LISTENERS];
static int nlis;

/* to dmesg and to the module log, which survives the ring buffer wrapping */
static void klog(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	struct stat st;
	char ts[32];
	time_t now;

	if (fd >= 0) {
		dprintf(fd, "rk_gpud: %s\n", msg);
		close(fd);
	}
	if (!stat(LOGFILE, &st) && st.st_size > LOG_MAX)
		rename(LOGFILE, LOGFILE ".old");
	fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	now = time(NULL);
	strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", localtime(&now));
	dprintf(fd, "%s rk_gpud: %s\n", ts, msg);
	close(fd);
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

static void set_max(long mhz)
{
	if (mhz != g.cur_max && !write_long(GPU "gpu_max_clock", mhz))
		g.cur_max = mhz;
}

/* write the cap, never above what the user/system had set */
static void apply_cap(void)
{
	if (g.cap >= 0)
		set_max(steps[g.cap] < orig_max ? steps[g.cap] : orig_max);
}

static void restore(void)
{
	if (orig_max > 0)
		set_max(orig_max);
}

/* demand_active: 0 off, 1 untouched, 2 touched (Sustained performance) */
static int read_active(int fd)
{
	char c = '0';

	if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, &c, 1) != 1)
		return 0;
	return c == '1' ? 1 : c == '2' ? 2 : 0;
}

static long read_fd_long(int fd)
{
	char buf[32];
	ssize_t n;

	if (fd < 0 || lseek(fd, 0, SEEK_SET) < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return strtol(buf, NULL, 10);
}

/*
 * Headroom while touched: the kernel's, which shrinks as the phone gets
 * hotter, but never below TOUCH_HEADROOM_MIN.
 */
static long touch_headroom(int timed)
{
	long min = timed ? FAS_TOUCH_HEADROOM_MIN : TOUCH_HEADROOM_MIN;
	char buf[96], *p;
	long hr;

	if (read_text(SUST_STATE, buf, sizeof(buf)))
		return TOUCH_HEADROOM_PCT;
	p = strstr(buf, "headroom=");
	if (!p)
		return TOUCH_HEADROOM_PCT;
	hr = strtol(p + 9, NULL, 10);
	if (timed)
		hr -= cut;
	return hr > min ? hr : min;
}

/* untouched headroom: trimmed while frames are timed */
static long idle_headroom(int timed)
{
	long hr = timed ? HEADROOM_PCT - cut : HEADROOM_PCT;

	return hr > FAS_HEADROOM_MIN ? hr : FAS_HEADROOM_MIN;
}

/* frames of the app in front are being timed right now */
static int frames_timed(void)
{
	return fas.ctx >= 0 && !fas.paused;
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

/* ---- learned floor ---------------------------------------------------- */

/* step c saturated, failed a fall or held a frame back: keep above it */
static void floor_learn(int c, long work, long t)
{
	int f = c + 1 < nsteps ? c + 1 : nsteps - 1;

	if (c < 0)
		return;
	/* hit again while held or soon after: hold it longer */
	if (floor_i && t < floor_until + FLOOR_HOLD_MS)
		floor_hold = floor_hold * 2 < FLOOR_HOLD_MAX_MS ?
			     floor_hold * 2 : FLOOR_HOLD_MAX_MS;
	if (f > floor_i)
		floor_i = f;
	floor_until = t + floor_hold;
	floor_work = work;
}

/*
 * Frame timing says the frames are fine right now: registered, the rate is
 * met and nothing was late for a hold period.
 */
static int frames_ok(long t)
{
	return fas.ctx >= 0 && !fas.degraded && t - fas.last_jank >= FLOOR_HOLD_MS;
}

/* once per reading: probe the floor lower after its hold, forget when idle */
static void floor_age(long busy, long work, long t)
{
	if (busy < IDLE_BUSY) {
		if (idle_since < 0)
			idle_since = t;
		if (t - idle_since >= FLOOR_FORGET_MS) {
			floor_i = 0;
			floor_hold = FLOOR_HOLD_MS;
		}
		return;
	}
	idle_since = -1;
	if (floor_i && t >= floor_until &&
	    (work * 100 <= floor_work * (100 - PROBE_WORK_PCT) || frames_ok(t))) {
		floor_i--;
		floor_until = t + PROBE_MS;
		floor_work = work;
		if (!floor_i)
			floor_hold = FLOOR_HOLD_MS;
	}
}

/* ---- the governor ----------------------------------------------------- */

/* demand_active changed */
static void gov_set_active(int now)
{
	int was = g.active;

	g.active = now;
	g.idle = 0;
	if (!now) {
		restore();
		g.cap = -1;
		return;
	}
	/*
	 * Starting, or a touch: the work it brings isn't in any reading yet,
	 * so start from the top and let fresh readings bring the cap down.
	 */
	if (!was || now == 2) {
		g.cap = nsteps - 1;
		g.low = 0;
		g.verify = 0;
		/* untouched: use the current reading; touched: it predates the touch */
		g.last_busy = now == 2 ? gpu_busy() : -1;
		g.last_eval = now_ms();
		g.clk_sum = 0;
		g.nclk = 0;
	}
}

/* one TICK_MS sample while active */
static void gov_tick(void)
{
	long clk = read_long(GPU "gpu_clock");
	long busy = gpu_busy();
	long t = now_ms(), avg;
	int touched = g.active == 2, want;

	if (clk > 0) {
		g.clk_sum += clk;
		g.nclk++;
		g.last_clk = clk;
	}
	if (busy < 0 || g.cap < 0)
		return;
	/* act on a fresh reading only */
	if (busy == g.last_busy && t - g.last_eval < STALE_MS) {
		apply_cap();
		return;
	}
	g.last_busy = busy;
	g.last_eval = t;
	g.idle = busy ? 0 : g.idle + 1;

	/* the clock the reading was taken at */
	avg = g.nclk ? g.clk_sum / g.nclk : steps[g.cap];
	g.clk_sum = 0;
	g.nclk = 0;
	g.work = busy * avg;

	/* frequency the current work needs, plus headroom */
	/* frames on time for another CUT_STEP_MS: a point less headroom */
	if (frames_timed() && !fas.degraded && t - cut_since >= CUT_STEP_MS &&
	    t - fas.last_jank >= CUT_STEP_MS) {
		cut_since = t;
		if (cut < CUT_MAX)
			cut++;
	}
	want = step_for(avg * busy / 100 *
			(100 + (touched ? touch_headroom(frames_timed()) :
				idle_headroom(frames_timed()))) / 100);

	floor_age(busy, g.work, t);
	if (want < floor_i)
		want = floor_i;

	if (g.verify && busy >= IDLE_BUSY && busy > g.expect + VERIFY_MARGIN) {
		/* the fall hurt: back up, and stay above it */
		floor_learn(g.fell_from - 1, g.fell_work, t);
		if (g.cap < g.fell_from)
			g.cap = g.fell_from;
		g.verify = 0;
		g.low = 0;
	} else if (busy >= SAT_PCT && avg * 100 >= steps[g.cap] * 90 &&
		   g.cap < nsteps - 1) {
		floor_learn(g.cap, g.work, t);
		g.cap = touched ? nsteps - 1 : (g.cap + nsteps) / 2;
		g.verify = 0;
		g.low = 0;
	} else if (want > g.cap) {
		g.cap = want;
		g.low = 0;
	} else if (want < g.cap && !fas.degraded) {
		if (busy < IDLE_BUSY) {
			g.cap = want;
		} else if (++g.low >= FALL_READINGS) {
			g.fell_from = g.cap;
			g.fell_work = g.work;
			g.cap -= FALL_STEPS;
			if (g.cap < want)
				g.cap = want;
			/*
			 * same work at the clock it will run at; a cap above
			 * the clock in use changes nothing and needs no check
			 */
			if (steps[g.cap] < avg) {
				g.expect = busy * avg / steps[g.cap];
				g.verify = VERIFY_READINGS + 1;
			}
			g.low = 0;
		}
	} else {
		g.low = 0;
	}
	if (g.verify)
		g.verify--;
	apply_cap();
}

/* a late frame (level 1) or a badly late one (2), from encore_fas */
static void gov_jank(int level, long t)
{
	if (!g.active || g.cap < 0 || g.cap >= nsteps - 1)
		return;
	/* only if the GPU ran into the cap; otherwise the cap didn't do it */
	if (g.last_clk * 100 < steps[g.cap] * JANK_BOUND_PCT)
		return;
	floor_learn(g.cap, g.work, t);
	/* the cap held a frame back: give headroom back at once */
	cut = cut > CUT_BACK ? cut - CUT_BACK : 0;
	cut_since = t;
	g.cap = level > 1 ? nsteps - 1 :
		g.cap + 2 < nsteps ? g.cap + 2 : nsteps - 1;
	g.low = 0;
	g.verify = 0;
	fas.janks++;
	fas.state_dirty = 1;
	apply_cap();
}

/* ---- frame timing (encore_fas) ---------------------------------------- */

/*
 * File offset of the first of @names defined in an arm64 ELF, from its
 * symbol tables. After fas_elf.c of encore_fas (GPL-2.0).
 */
static int elf_offset(const char *path, const char *const *names, int n,
		      uint64_t *out)
{
	const Elf64_Ehdr *eh;
	const Elf64_Shdr *sh;
	const uint8_t *base;
	struct stat st;
	int fd, i, k, ret = -1;
	size_t size;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
		close(fd);
		return -1;
	}
	size = st.st_size;
	base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (base == MAP_FAILED)
		return -1;

#define IN_FILE(off, len) ((uint64_t)(off) <= size && (uint64_t)(len) <= size - (uint64_t)(off))
	eh = (const Elf64_Ehdr *)base;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_AARCH64 ||
	    eh->e_shentsize != sizeof(Elf64_Shdr) ||
	    !IN_FILE(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr)) ||
	    !IN_FILE(eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr)))
		goto out;
	sh = (const Elf64_Shdr *)(base + eh->e_shoff);

	for (k = 0; k < n && ret; k++) {
		for (i = 0; i < eh->e_shnum && ret; i++) {
			const Elf64_Shdr *str;
			const Elf64_Sym *sym;
			const char *strs;
			size_t j, nsym;

			if ((sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) ||
			    sh[i].sh_link >= eh->e_shnum ||
			    sh[i].sh_entsize != sizeof(Elf64_Sym))
				continue;
			str = &sh[sh[i].sh_link];
			if (!IN_FILE(sh[i].sh_offset, sh[i].sh_size) ||
			    !IN_FILE(str->sh_offset, str->sh_size) || !str->sh_size)
				continue;
			sym = (const Elf64_Sym *)(base + sh[i].sh_offset);
			strs = (const char *)(base + str->sh_offset);
			nsym = sh[i].sh_size / sizeof(Elf64_Sym);

			for (j = 0; j < nsym && ret; j++) {
				const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
				uint64_t va = sym[j].st_value;
				int p;

				if (ELF64_ST_TYPE(sym[j].st_info) != STT_FUNC ||
				    sym[j].st_shndx == SHN_UNDEF || !va ||
				    sym[j].st_name >= str->sh_size ||
				    strncmp(strs + sym[j].st_name, names[k],
					    str->sh_size - sym[j].st_name))
					continue;
				/* virtual address -> file offset */
				for (p = 0; p < eh->e_phnum; p++) {
					if (ph[p].p_type != PT_LOAD ||
					    !IN_FILE(ph[p].p_offset, ph[p].p_filesz))
						continue;
					if (va >= ph[p].p_vaddr && va - ph[p].p_vaddr < ph[p].p_filesz) {
						*out = ph[p].p_offset + (va - ph[p].p_vaddr);
						ret = 0;
						break;
					}
				}
			}
		}
	}
#undef IN_FILE
out:
	munmap((void *)base, size);
	return ret;
}

/*
 * fas_state for the WebUI and rk_diag.sh; while an app is watched it is
 * rewritten every FAS_STATE_MS with the detector's own view (target fps,
 * acquiring/degraded/paused, event sequence: seq=0 means no frame of the
 * app has reached the probe yet).
 */
static void fas_write_state(long t)
{
	char tmp[] = FAS_STATE ".tmp";
	struct fas_state st = { .ctx_id = fas.ctx };
	int fd, have = 0;

	if (!fas.state_dirty || t - fas.state_at < FAS_STATE_MS)
		return;
	if (fas.ctx >= 0 && !ioctl(fas.fd, FAS_IOC_GET_STATE, &st))
		have = 1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	dprintf(fd, "pid=%d fps=%u degraded=%d late=%lu gpu_lifts=%lu gpu_floor=%ld cut=%d"
		" app_late=%lu app_events=%lu",
		fas.ctx >= 0 ? fas.pid : 0, fas.ctx >= 0 ? fas.fps : 0,
		fas.degraded, fas.frames_late, fas.janks,
		floor_i ? steps[floor_i] : 0L, cut, fas.app_late, fas.app_events);
	if (have)
		dprintf(fd, " det_fps=%u acquiring=%d det_degraded=%d paused=%d seq=%u dropped=%u",
			st.fps, !!(st.flags & FAS_STATE_ACQUIRING),
			!!(st.flags & FAS_STATE_DEGRADED), !!(st.flags & FAS_STATE_PAUSED),
			st.seq, st.dropped);
	else if (fas.ctx >= 0)
		dprintf(fd, " det=gone");
	dprintf(fd, "\n");
	close(fd);
	rename(tmp, FAS_STATE);
	fas.state_at = t;
	/* keep refreshing while an app is watched */
	fas.state_dirty = fas.ctx >= 0;
}

/* stop following an app (its listener may stay registered) */
static void fas_forget_primary(void)
{
	fas.ctx = -1;
	fas.pid = 0;
	fas.degraded = 0;
	fas.paused = 0;
	fas.fps = 0;
	fas.state_dirty = 1;
}

static void fas_drop(int i)
{
	struct fas_remove_args r = { .ctx_id = lis[i].ctx };

	ioctl(fas.fd, FAS_IOC_REMOVE, &r);
	if (lis[i].ctx == fas.ctx)
		fas_forget_primary();
	lis[i] = lis[--nlis];
}

static void fas_unregister(void)
{
	while (nlis)
		fas_drop(nlis - 1);
	fas_forget_primary();
}

/*
 * Every rate a game commonly runs at on a 60-144 Hz panel; the detector
 * settles on the one the app actually renders at. Neighbours must differ
 * by 12% at least.
 */
static const unsigned int fas_rates[] = { 144, 120, 90, 72, 60, 45, 40, 30 };

/* the app in front changed: keep what was learned, restore what we know */
static void app_mem_switch(long uid)
{
	int i, slot = -1, lru = 0;
	long t = now_ms();

	if (mem_uid >= 0) {
		for (i = 0; i < APP_MEMORY; i++)
			if (app_mem[i].uid == mem_uid)
				slot = i;
		if (slot < 0) {
			for (i = 0; i < APP_MEMORY; i++) {
				if (app_mem[i].uid < 0) {
					lru = i;
					break;
				}
				if (app_mem[i].used < app_mem[lru].used)
					lru = i;
			}
			slot = lru;
		}
		app_mem[slot].uid = mem_uid;
		app_mem[slot].floor_i = floor_i;
		app_mem[slot].floor_hold = floor_hold;
		app_mem[slot].floor_work = floor_work;
		app_mem[slot].cut = cut;
		app_mem[slot].used = t;
	}
	mem_uid = uid;
	floor_i = 0;
	floor_hold = FLOOR_HOLD_MS;
	floor_work = 0;
	cut = CUT_START;
	cut_since = t;
	for (i = 0; uid >= 0 && i < APP_MEMORY; i++) {
		if (app_mem[i].uid != uid)
			continue;
		floor_i = app_mem[i].floor_i < nsteps ? app_mem[i].floor_i : 0;
		floor_hold = app_mem[i].floor_hold;
		floor_work = app_mem[i].floor_work;
		cut = app_mem[i].cut;
		app_mem[i].used = t;
		/* a fresh hold: the scene may differ from when it was saved */
		floor_until = t + FLOOR_HOLD_MS;
		break;
	}
}

/* process name, for the log */
static const char *app_name(int pid)
{
	static char name[64];
	char path[48];

	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	if (read_text(path, name, sizeof(name)) || !name[0])
		snprintf(name, sizeof(name), "?");
	return name;
}

static void fas_add(int pid, long uid)
{
	struct fas_register_args a;
	unsigned int i;
	char msg[128];

	memset(&a, 0, sizeof(a));
	a.pid = pid;
	a.offset = fas.offset;
	a.cfg.count = sizeof(fas_rates) / sizeof(fas_rates[0]);
	for (i = 0; i < a.cfg.count; i++)
		a.cfg.fps[i] = fas_rates[i];
	snprintf(a.path, sizeof(a.path), "%s", LIBGUI);
	if (ioctl(fas.fd, FAS_IOC_REGISTER, &a)) {
		/* gone already, or another FAS client watches it (EEXIST) */
		if (errno != ESRCH) {
			snprintf(msg, sizeof(msg), "frame timing for %.40s (pid %d) failed: %s",
				 app_name(pid), pid, strerror(errno));
			klog(msg);
		}
		return;
	}
	lis[nlis].pid = pid;
	lis[nlis].ctx = a.ctx_id;
	lis[nlis].uid = uid;
	nlis++;
}

/* app processes in top-app: regular app UIDs, named like a package */
static int top_app_procs(int *pids, long *uids, int max)
{
	char buf[4096], path[64], st[512], *p, *save = NULL;
	int n = 0;

	if (read_text("/dev/cpuset/top-app/cgroup.procs", buf, sizeof(buf)))
		return 0;
	for (p = strtok_r(buf, "\n", &save); p && n < max; p = strtok_r(NULL, "\n", &save)) {
		int pid = atoi(p);
		long uid;
		char *u;

		snprintf(path, sizeof(path), "/proc/%d/status", pid);
		if (pid <= 0 || read_text(path, st, sizeof(st)))
			continue;
		u = strstr(st, "\nUid:");
		if (!u)
			continue;
		uid = atol(u + 5);
		/* not a system, isolated or sandbox process */
		if (uid % 100000 < 10000 || uid % 100000 > 19999)
			continue;
		/* an app process is named after its package; a shell child isn't */
		if (app_name(pid)[0] == '/' || app_name(pid)[0] == '?')
			continue;
		pids[n] = pid;
		uids[n] = uid;
		n++;
	}
	return n;
}

/* bring the listeners in line with what top-app holds now */
static void fas_sync(long t)
{
	int pids[MAX_LISTENERS], n, i, j;
	long uids[MAX_LISTENERS];

	fas.synced_at = t;
	n = top_app_procs(pids, uids, MAX_LISTENERS);
	for (i = nlis - 1; i >= 0; i--) {
		for (j = 0; j < n && pids[j] != lis[i].pid; j++)
			;
		if (j == n)
			fas_drop(i);
	}
	for (j = 0; j < n && nlis < MAX_LISTENERS; j++) {
		for (i = 0; i < nlis && lis[i].pid != pids[j]; i++)
			;
		if (i == nlis)
			fas_add(pids[j], uids[j]);
	}
}

/* frames arrive from listener i: follow that app from now on */
static void fas_follow(int i, long t)
{
	char msg[128];

	fas_forget_primary();
	fas.ctx = lis[i].ctx;
	fas.pid = lis[i].pid;
	fas.app_late = 0;
	fas.app_events = 0;
	fas.last_jank = t;	/* no probing until it proves healthy */
	app_mem_switch(lis[i].uid);
	snprintf(msg, sizeof(msg), "watching frames of %.48s (pid %d)",
		 app_name(lis[i].pid), lis[i].pid);
	klog(msg);
}

static void fas_events(long t)
{
	struct fas_event ev[16];
	ssize_t n;
	int i, worst = 0;

	n = read(fas.fd, ev, sizeof(ev));
	if (n <= 0)
		return;
	for (i = 0; i < (int)(n / (ssize_t)sizeof(ev[0])); i++) {
		int level = 0;

		if (ev[i].ctx_id != fas.ctx) {
			int k;

			for (k = 0; k < nlis && lis[k].ctx != ev[i].ctx_id; k++)
				;
			/* not ours, or another app draws while ours still does */
			if (k == nlis || (fas.ctx >= 0 && t - fas.last_ev < PRIMARY_IDLE_MS))
				continue;
			fas_follow(k, t);
		}
		fas.last_ev = t;
		fas.app_events++;
		if (ev[i].fps)		/* every event carries the active target */
			fas.fps = ev[i].fps;
		switch (ev[i].type) {
		case FAS_EVENT_SMALL_JANK:
		case FAS_EVENT_BOOST_SOFT:
			level = 1;
			break;
		case FAS_EVENT_BIG_JANK:
		case FAS_EVENT_BOOST_HARD:
			level = 2;
			break;
		case FAS_EVENT_DEGRADED:
			fas.degraded = 1;
			level = 2;
			break;
		case FAS_EVENT_RECOVERED:
			fas.degraded = 0;
			break;
		case FAS_EVENT_PAUSED:
			fas.degraded = 0;
			fas.paused = 1;
			break;
		case FAS_EVENT_RESUMED:
			fas.paused = 0;
			break;
		case FAS_EVENT_RATE_SWITCH:
			fas.fps = ev[i].fps;
			fas.degraded = 0;
			break;
		}
		if (level) {
			fas.frames_late++;
			fas.app_late++;
			fas.last_jank = t;
			if (level > worst)
				worst = level;
		}
		fas.state_dirty = 1;
	}
	if (worst)
		gov_jank(worst, t);
}

static void fas_init(void)
{
	static const char *const names[] = {
		"_ZN7android7Surface11queueBufferEP19ANativeWindowBufferiPNS_24SurfaceQueueBufferOutputE",
		"_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi",
	};
	struct fas_version v;

	fas.fd = open(FAS_DEV, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fas.fd < 0) {
		klog("no " FAS_DEV ", frame timing off");
		return;
	}
	if (ioctl(fas.fd, FAS_IOC_GET_VERSION, &v) || v.abi != FAS_ABI_VERSION) {
		klog("encore_fas ABI mismatch, frame timing off");
		goto off;
	}
	if (elf_offset(LIBGUI, names, 2, &fas.offset)) {
		klog("Surface::queueBuffer not found in " LIBGUI ", frame timing off");
		goto off;
	}
	klog("frame timing on");
	return;
off:
	close(fas.fd);
	fas.fd = -1;
}

/* a setting file says "0": off; missing or anything else: on */
static int conf_on(const char *name)
{
	char path[128];

	snprintf(path, sizeof(path), "%s%s", CONF, name);
	return read_long(path) != 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_signal };
	long saved;
	int afd, tfd = -1, check = 1, i;
	int use_gpu = conf_on("gpu"), use_fas = conf_on("fas");

	for (i = 1; i < argc; i++) {	/* for tests */
		if (!strcmp(argv[i], "--no-fas"))
			use_fas = 0;
		if (!strcmp(argv[i], "--no-gpu"))
			use_gpu = 0;
	}

	if (use_gpu && (access(GPU "gpu_max_clock", W_OK) || load_steps())) {
		klog("no usable kgsl gpu_max_clock/gpu_freq_table, GPU caps off");
		use_gpu = 0;
	}
	if (!use_gpu && !use_fas)
		return 0;

	if (use_gpu) {
		/*
		 * The uncapped value: kept in STATE across restarts so a crash
		 * while capped can't make a lowered value the new "original".
		 */
		saved = read_long(STATE);
		orig_max = saved > 0 ? saved : read_long(GPU "gpu_max_clock");
		if (orig_max <= 0)
			orig_max = steps[nsteps - 1];
		if (saved <= 0)
			save_state(orig_max);
		g.cur_max = read_long(GPU "gpu_max_clock");
	}

	afd = open(ACTIVE, O_RDONLY | O_CLOEXEC);
	if (afd < 0) {
		klog("kernel has no demand_active, exiting");
		return 1;
	}

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	klog("started");

	if (use_fas) {
		tfd = open(TOP_PID, O_RDONLY | O_CLOEXEC);
		if (tfd >= 0)
			fas_init();
		else
			klog("kernel has no top_pid, frame timing off");
		if (fas.fd >= 0) {
			fas.reg_at = now_ms();
		} else if (tfd >= 0) {
			close(tfd);
			tfd = -1;
		}
	}
	if (!use_gpu && fas.fd < 0) {
		close(afd);
		return 0;
	}

	while (!quit) {
		struct pollfd pfd[3] = {
			{ .fd = afd, .events = POLLPRI | POLLERR },
			{ .fd = tfd, .events = POLLPRI | POLLERR },
			{ .fd = fas.fd, .events = POLLIN },
		};
		int timeout = !g.active ? -1 :
			      g.idle >= IDLE_READINGS ? IDLE_TICK_MS : TICK_MS;
		long t = now_ms();

		if (fas.reg_at) {
			long d = fas.reg_at > t ? fas.reg_at - t : 0;

			if (timeout < 0 || d < timeout)
				timeout = (int)d;
		}
		if (fas.state_dirty && fas.fd >= 0) {
			long d = fas.state_at + FAS_STATE_MS - t;

			if (d < 0)
				d = 0;
			if (timeout < 0 || d < timeout)
				timeout = (int)d;
		}
#ifdef POLL_EVERY_TICK
		if (!g.active)
			timeout = TICK_MS;
#endif

		if (!check) {
			int r = poll(pfd, 3, timeout);

			if (r < 0 && errno != EINTR)
				break;
			if (r > 0) {
				check = !!pfd[0].revents;
				if (pfd[1].revents) {
					read_fd_long(tfd);	/* re-arm the notification */
					if (!fas.reg_at)
						fas.reg_at = now_ms() + REG_DELAY_MS;
				}
				if (pfd[2].revents & POLLIN)
					fas_events(now_ms());
			}
#ifdef POLL_EVERY_TICK	/* tests: a regular file never signals POLLPRI */
			check = 1;
#endif
		}
		if (check) {
			int now = use_gpu ? read_active(afd) : 0;

			check = 0;
			if (now != g.active)
				gov_set_active(now);
		}
		if (quit)
			break;

		t = now_ms();
		if (fas.fd >= 0 && !fas.reg_at && t - fas.synced_at >= RESCAN_MS)
			fas.reg_at = t;
		if (fas.reg_at && t >= fas.reg_at) {
			fas.reg_at = 0;
			fas_sync(t);
		}
		if (fas.fd >= 0)
			fas_write_state(t);
		if (g.active)
			gov_tick();
	}

	if (fas.fd >= 0)
		fas_unregister();
	if (use_gpu)
		restore();
	klog("stopped");
	return 0;
}
