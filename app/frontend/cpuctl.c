/*
 * CPU frequency in game, driven by the frontend.
 *
 * The RK3326 has one clock for its four A35 cores. The emulator does one
 * burst of work per vblank and then sleeps until the flip, so schedutil
 * alone sees ~40% idle, settles at an OPP where the frame no longer fits
 * and, once vblanks are missed, sees even less load and never recovers
 * (measured: stuck at 816 MHz with ~15 ms per retro_run). So in game the
 * frontend picks the frequency floor itself from the frame times.
 *
 * Actuator: utilization clamping (CONFIG_UCLAMP_TASK). The controller's
 * target OPP becomes a uclamp_min on the thread that runs retro_run (our
 * only thread), set with sched_setattr(SCHED_FLAG_UTIL_CLAMP_MIN |
 * SCHED_FLAG_KEEP_ALL). This is the standard kernel interface for a task
 * to ask for performance (Android's ADPF hint sessions build on it), it
 * only boosts the emulation thread rather than the whole system, and
 * schedutil stays the governor everywhere: it still goes above the floor
 * on its own when the real load asks for it, and the floor is retained
 * while the thread sleeps for the vblank (uclamp keeps an idle rq at its
 * last clamp). In 6.18 schedutil computes
 *
 *   perf = max(uclamp_min, min(1.25 * util, uclamp_max))
 *   freq = max_freq * perf / 1024, rounded up to the next OPP
 *
 * (sugov_effective_cpu_perf, get_next_freq with the capacity reference
 * frequency, which is cpuinfo.max_freq here, and RELATION_L), so the
 * clamp has no headroom applied and util_min = floor(opp * 1024 / max)
 * lands exactly on that OPP: 474, 644, 796, 948 and 1024 for 600, 816,
 * 1008, 1200 and 1296 MHz. The observed scaling_cur_freq is logged after
 * every change. If sched_setattr fails (no uclamp in the kernel) the floor
 * is written to scaling_min_freq instead, once logged.
 *
 * Modes (settings.cpu, "cpu=" in settings.txt):
 *   adaptive   the controller below (default)
 *   max        floor at the top OPP (util_min 1024)
 *   <MHz>      floor at that OPP, for experiments
 * In the ROM list and the menus there is no floor (util_min 0).
 *
 * Adaptive controller, fed with each frame's work time (retro_run, audio
 * and the blit: CPU time until the frame is ready for the flip) and the
 * vblanks missed:
 *   - starts at the top OPP, or at the OPP learned for this ROM
 *   - every CTL_WINDOW frames: if the window had no missed vblank, no hold
 *     is active, and its p99 and max, scaled by f_cur / f_lower to predict
 *     the next lower OPP, stay below DOWN_P99_FRAC and DOWN_MAX_FRAC of the
 *     frame period, step down one OPP (f_cur is the observed frequency if
 *     schedutil went above the floor)
 *   - a missed vblank whose frame worked at least MISS_CPU_FRAC of the
 *     period jumps to the top right away; a miss with less work is a stall
 *     (fork, slow sysfs, scheduling) that a faster clock would not have
 *     avoided, it only blocks the step down for that window
 *   - UP_COUNT frames over UP_FRAC of the period within one window (its
 *     p99 is over it) step up one OPP right away
 *   - every step up holds the OPP HOLD_BASE_NS before stepping down again;
 *     the hold doubles (up to HOLD_MAX_NS) when the step up came less than
 *     RECENT_DOWN_NS after a step down, so a load that does not fit a lower
 *     OPP does not make the clock bounce; it resets after HOLD_RESET_NS
 *     without a step up
 *   - the first CTL_WARMUP frames after a start or resume are not judged
 *     (audio restart, first frame) except that WARMUP_MISSES CPU misses
 *     there still jump to the top; the frame right after a change is not
 *     sampled
 * Missed vblanks are reported by the flip event of the next frame, so a
 * miss is attributed to the previous frame's work and OPP.
 *
 * Learning: on game end the lowest OPP that ran at least LEARN_MIN_S with
 * at most 0.1% CPU misses and at most one step up per minute is stored as
 * cpu_start in the ROM's settings file; the next session starts there. If
 * that start steps up within PROBATION_NS, it is forgotten (back to the
 * top next time).
 *
 * Statistics: per (adaptive or pinned, OPP): frames, game seconds, work
 * avg/p50/p99/max (0.1 ms histogram), missed vblanks (and CPU ones),
 * audio underruns, battery current and voltage, backlight. Battery
 * samples (battery.c polls every 5 s; the RK817 driver itself refreshes
 * every 8 s) are attributed to the OPP that had at least BAT_MAJORITY of
 * the game time since the previous sample, and skipped when there was
 * less than BAT_MIN_GAME_S of game time (menus) or a charger is on. Menu
 * time is not game time. On game end one CSV row per OPP plus a summary
 * row are appended to <data>/log/perf.csv (header written once), with the
 * kernel's cpufreq time_in_state deltas (CONFIG_CPU_FREQ_STAT), and the
 * same summary goes to frontend.txt. Nothing is written or synced in the
 * frame loop besides log lines on changes.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "battery.h"
#include "cpuctl.h"
#include "fe.h"

#define NS 1000000000LL
#define CPUFREQ_SYSFS "/sys/devices/system/cpu/cpu0/cpufreq"

#define CTL_WINDOW 120          /* frames, ~2 s */
#define CTL_WARMUP 30           /* frames not judged after start and resume */
#define WARMUP_MISSES 3         /* CPU misses in warmup that still jump */
#define DOWN_P99_FRAC 0.75      /* predicted p99 at the lower OPP */
#define DOWN_MAX_FRAC 0.95      /* predicted max at the lower OPP */
#define UP_FRAC 0.88
#define UP_COUNT 2
#define MISS_CPU_FRAC 0.80
#define HOLD_BASE_NS (10 * NS)
#define HOLD_MAX_NS (160 * NS)
#define RECENT_DOWN_NS (30 * NS)
#define HOLD_RESET_NS (120 * NS)
#define PROBATION_NS (20 * NS)
#define LEARN_MIN_S 30.0
#define VERIFY_FRAMES 10        /* read scaling_cur_freq after a change */

#define HIST_BIN_MS 0.1
#define HIST_BINS 1000
#define MAX_DT_NS (250 * 1000000LL)
#define BAT_MIN_GAME_S 3.0
#define BAT_MAJORITY 0.8

#define UCLAMP_MAX_VAL 1024
/* uapi linux/sched.h; not included because it clashes with musl's sched.h */
#define FE_SCHED_FLAG_KEEP_ALL 0x18
#define FE_SCHED_FLAG_UTIL_CLAMP_MIN 0x20

enum { CLS_ADAPTIVE, CLS_PINNED, CLS_COUNT };
enum { ACT_NONE, ACT_UCLAMP, ACT_MINFREQ, ACT_FAKE };
static const char *const act_names[] = {
	"none", "uclamp_min", "scaling_min_freq", "simulated uclamp_min",
};

/* As in uapi linux/sched/types.h (SCHED_ATTR_SIZE_VER1). */
struct fe_sched_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;
	int32_t sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime, sched_deadline, sched_period;
	uint32_t sched_util_min, sched_util_max;
};

struct opp_acc {
	uint64_t frames;
	double seconds, work_sum, bl_sum;
	float work_max;
	unsigned missed, missed_cpu, underruns, entered, left_up;
	unsigned bat_n;
	double bat_ua, bat_uv, bat_mw;
	uint32_t hist[HIST_BINS];
};

struct wstats {
	int n;
	float p50, p99, max;
};

static struct {
	char root[256];
	int act;
	long khz[CPU_MAX_OPPS];
	int n, top;
	int mode;
	int applied;        /* OPP floor in effect, -2 none, -1 unknown */

	/* Game session, cleared on start. */
	struct {
		int in_game, active;
		char rom[128];
		int cls, opp;
		double period_ms;
		float win[CTL_WINDOW];
		int wn, over;
		unsigned win_missed;
		int warmup, settle, verify;
		unsigned warmup_cpu_missed;
		int64_t hold_until, hold, last_down, last_up, last_ns;
		float prev_work;
		int prev_cls, prev_opp;
		unsigned xruns_seen, bat_seq;
		double bat_t[CLS_COUNT][CPU_MAX_OPPS];
		int start_mhz, start_failed;
		int64_t probation_until;
		unsigned changes, ups, downs, jumps;
		int have_kstats;
		long tis0[CPU_MAX_OPPS], trans0;
		struct opp_acc acc[CLS_COUNT][CPU_MAX_OPPS];
	} s;
} c = { .applied = -1 };

/* ---------------------------------------------------------------- sysfs */

static long read_long(const char *name, long fallback)
{
	return read_attr_long(c.root, name, fallback);
}

static void write_long(const char *name, long v)
{
	char buf[24];
	int err;

	snprintf(buf, sizeof(buf), "%ld\n", v);
	err = write_attr(c.root, name, buf);
	if (err)
		log_msg("cpufreq: write %s/%s: %s", c.root, name, strerror(err));
}

long cpu_freq_khz(void)
{
	return read_long("scaling_cur_freq", -1);
}

static int cmp_long(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return x < y ? -1 : x > y;
}

int cpuctl_fake_sysfs(const char *dir)
{
	static const char *const files[][2] = {
		{ "scaling_available_frequencies",
		  "600000 816000 1008000 1200000 1296000 \n" },
		{ "scaling_governor", "schedutil\n" },
		{ "scaling_min_freq", "600000\n" },
		{ "scaling_max_freq", "1296000\n" },
		{ "scaling_cur_freq", "816000\n" },
		{ "uclamp_min", "0\n" },
		{ "stats/time_in_state", "600000 100\n816000 200\n1008000 0\n"
		  "1200000 0\n1296000 300\n" },
		{ "stats/total_trans", "7\n" },
	};
	char path[320];

	snprintf(path, sizeof(path), "%s/stats", dir);
	if ((mkdir(dir, 0755) && errno != EEXIST) ||
	    (mkdir(path, 0755) && errno != EEXIST))
		return -1;
	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, files[i][0]);
		if (write_file_atomic(path, files[i][1], strlen(files[i][1])))
			return -1;
	}
	return 0;
}

void cpuctl_init(const char *root)
{
	char buf[256], list[128], *p, *end;
	size_t ln = 0;

	snprintf(c.root, sizeof(c.root), "%s", root ? root : CPUFREQ_SYSFS);
	c.act = root ? ACT_FAKE : fe_is_init ? ACT_UCLAMP : ACT_NONE;
	c.applied = -1;
	c.n = 0;
	read_attr(c.root, "scaling_available_frequencies", buf, sizeof(buf));
	for (p = buf; c.n < CPU_MAX_OPPS; p = end) {
		long v = strtol(p, &end, 10);

		if (end == p)
			break;
		if (v > 0)
			c.khz[c.n++] = v;
	}
	qsort(c.khz, c.n, sizeof(c.khz[0]), cmp_long);
	if (!c.n) {
		/* One pseudo OPP so the statistics still work. */
		c.khz[0] = 0;
		c.n = 1;
		c.act = ACT_NONE;
	}
	c.top = c.n - 1;
	list[0] = 0;
	for (int i = 0; i < c.n; i++)
		ln += snprintf(list + ln, sizeof(list) - ln, "%s%ld",
			       i ? " " : "", c.khz[i] / 1000);
	log_msg("cpufreq: %s: OPPs %s MHz, actuator %s", c.root, list,
		act_names[c.act]);
}

int cpuctl_opp_count(void)
{
	return c.khz[0] ? c.n : 0;
}

long cpuctl_opp_khz(int i)
{
	return i >= 0 && i < c.n ? c.khz[i] : 0;
}

/* ------------------------------------------------------------- actuator */

/* util_min that makes schedutil pick OPP i (see the top comment). */
static unsigned opp_util(int i)
{
	long fmax = c.khz[c.top];
	unsigned u;

	if (i >= c.top || fmax <= 0)
		return UCLAMP_MAX_VAL;
	u = (unsigned)((int64_t)c.khz[i] * UCLAMP_MAX_VAL / fmax);
	if (i > 0 && (int64_t)fmax * u / UCLAMP_MAX_VAL <= c.khz[i - 1])
		u++;
	return u;
}

/* What schedutil does with a util_min, for the simulated actuator. */
static long util_freq(unsigned u)
{
	int64_t raw = (int64_t)c.khz[c.top] * u / UCLAMP_MAX_VAL;

	for (int i = 0; i < c.n; i++)
		if (c.khz[i] >= raw)
			return c.khz[i];
	return c.khz[c.top];
}

static int set_uclamp_min(unsigned u)
{
	struct fe_sched_attr a = {
		.size = sizeof(a),
		.sched_flags = FE_SCHED_FLAG_KEEP_ALL | FE_SCHED_FLAG_UTIL_CLAMP_MIN,
		.sched_util_min = u,
	};

	return (int)syscall(SYS_sched_setattr, 0, &a, 0);
}

/* Floor at OPP opp, or none for opp < 0. */
static void actuate(int opp)
{
	unsigned u = opp < 0 ? 0 : opp_util(opp);
	int key = opp < 0 ? -2 : opp;

	if (c.act == ACT_NONE || key == c.applied)
		return;
	if (c.act == ACT_UCLAMP && set_uclamp_min(u)) {
		log_msg("cpufreq: sched_setattr(uclamp_min %u): %s, falling back "
			"to scaling_min_freq", u, strerror(errno));
		c.act = ACT_MINFREQ;
	}
	if (c.act == ACT_MINFREQ)
		write_long("scaling_min_freq", c.khz[opp < 0 ? 0 : opp]);
	if (c.act == ACT_FAKE) {
		write_long("uclamp_min", u);
		write_long("scaling_cur_freq", util_freq(u));
	}
	c.applied = key;
}

static void apply(void)
{
	if (c.s.active)
		actuate(c.s.opp);
	else
		actuate(-1);
}

/* ---------------------------------------------------------------- modes */

static int mode_opp(int mode)
{
	if (mode == CPU_MODE_ADAPTIVE || mode == CPU_MODE_MAX)
		return c.top;
	for (int i = c.top; i > 0; i--)
		if (c.khz[i] <= mode * 1000L)
			return i;
	return 0;
}

static void mode_name(int mode, char *buf, size_t size)
{
	if (mode == CPU_MODE_ADAPTIVE)
		snprintf(buf, size, "adaptive");
	else if (mode == CPU_MODE_MAX)
		snprintf(buf, size, "max");
	else
		snprintf(buf, size, "fixed %d MHz", mode);
}

void cpuctl_mode_label(int mode, char *buf, size_t size)
{
	if (mode == CPU_MODE_ADAPTIVE)
		snprintf(buf, size, "Adaptive");
	else if (mode == CPU_MODE_MAX)
		snprintf(buf, size, "Max");
	else
		snprintf(buf, size, "%dMHz", mode);
}

int cpuctl_mode_step(int mode, int d)
{
	int list[CPU_MAX_OPPS + 2], n = 0, cur = 0;

	list[n++] = CPU_MODE_ADAPTIVE;
	list[n++] = CPU_MODE_MAX;
	for (int i = 0; i < c.top; i++)
		if (c.khz[i] >= 1000)
			list[n++] = (int)(c.khz[i] / 1000);
	for (int i = 0; i < n; i++)
		if (list[i] == mode)
			cur = i;
	return list[(cur + (d < 0 ? n - 1 : 1)) % n];
}

static void window_reset(void)
{
	c.s.wn = 0;
	c.s.over = 0;
	c.s.win_missed = 0;
}

void cpuctl_set_mode(int mode)
{
	char a[24], b[24];
	int opp;

	if (mode == c.mode)
		return;
	mode_name(c.mode, a, sizeof(a));
	mode_name(mode, b, sizeof(b));
	c.mode = mode;
	if (!c.s.in_game) {
		log_msg("cpufreq: mode %s", b);
		return;
	}
	opp = mode_opp(mode);
	/* Changes are counted (perf summary); the lines are debug only. */
	if (log_debug())
		log_msg("cpufreq: %ld -> %ld kHz (mode change %s -> %s)",
			c.khz[c.s.opp], c.khz[opp], a, b);
	if (opp != c.s.opp) {
		c.s.changes++;
		c.s.acc[mode == CPU_MODE_ADAPTIVE ? CLS_ADAPTIVE : CLS_PINNED][opp]
			.entered++;
	}
	c.s.cls = mode == CPU_MODE_ADAPTIVE ? CLS_ADAPTIVE : CLS_PINNED;
	c.s.opp = opp;
	c.s.hold = HOLD_BASE_NS;
	c.s.hold_until = 0;
	c.s.probation_until = 0;
	c.s.settle = 1;
	c.s.verify = VERIFY_FRAMES;
	window_reset();
	apply();
}

/* ------------------------------------------------------------ statistics */

static int read_kstats(long *tis, long *trans)
{
	char buf[1024], *p, *end;
	int got = 0;

	memset(tis, 0, sizeof(long) * CPU_MAX_OPPS);
	*trans = read_long("stats/total_trans", -1);
	if (read_attr(c.root, "stats/time_in_state", buf, sizeof(buf)) <= 0 ||
	    *trans < 0)
		return -1;
	for (p = buf; *p; p = end) {
		long khz = strtol(p, &end, 10), t;

		if (end == p)
			break;
		p = end;
		t = strtol(p, &end, 10);
		if (end == p)
			break;
		for (int i = 0; i < c.n; i++)
			if (c.khz[i] == khz) {
				tis[i] = t;  /* USER_HZ, 10 ms */
				got++;
			}
	}
	return got ? 0 : -1;
}

static void format_kstats(char *buf, size_t size, const long *tis,
			  long trans, const char *sign)
{
	size_t n = snprintf(buf, size, "%s%ld transitions, time_in_state", sign,
			    trans);

	for (int i = 0; i < c.n && n < size; i++)
		n += snprintf(buf + n, size - n, " %ld:%s%.2fs", c.khz[i] / 1000,
			      sign, tis[i] / 100.0);
}

static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

static void window_stats(struct wstats *w)
{
	float v[CTL_WINDOW];
	int n = c.s.wn, i99;

	memset(w, 0, sizeof(*w));
	if (!n)
		return;
	memcpy(v, c.s.win, n * sizeof(v[0]));
	qsort(v, n, sizeof(v[0]), cmp_float);
	i99 = (n * 99 + 99) / 100 - 1;
	w->n = n;
	w->p50 = v[(n - 1) / 2];
	w->p99 = v[i99 < 0 ? 0 : i99];
	w->max = v[n - 1];
}

static float hist_pct(const struct opp_acc *a, double q)
{
	uint64_t need = (uint64_t)ceil(a->frames * q), cum = 0;

	if (!a->frames)
		return 0;
	if (need < 1)
		need = 1;
	for (int i = 0; i < HIST_BINS; i++) {
		cum += a->hist[i];
		if (cum >= need) {
			float v = (float)((i + 1) * HIST_BIN_MS);

			return v < a->work_max ? v : a->work_max;
		}
	}
	return a->work_max;
}

static void acc_add(struct opp_acc *t, const struct opp_acc *a)
{
	t->frames += a->frames;
	t->seconds += a->seconds;
	t->work_sum += a->work_sum;
	t->bl_sum += a->bl_sum;
	if (a->work_max > t->work_max)
		t->work_max = a->work_max;
	t->missed += a->missed;
	t->missed_cpu += a->missed_cpu;
	t->underruns += a->underruns;
	t->entered += a->entered;
	t->left_up += a->left_up;
	t->bat_n += a->bat_n;
	t->bat_ua += a->bat_ua;
	t->bat_uv += a->bat_uv;
	t->bat_mw += a->bat_mw;
	for (int i = 0; i < HIST_BINS; i++)
		t->hist[i] += a->hist[i];
}

static void bat_sample(const struct battery *b)
{
	double tot = 0, best = 0;
	int bc = 0, bo = 0;

	for (int k = 0; k < CLS_COUNT; k++)
		for (int i = 0; i < c.n; i++) {
			tot += c.s.bat_t[k][i];
			if (c.s.bat_t[k][i] > best) {
				best = c.s.bat_t[k][i];
				bc = k;
				bo = i;
			}
		}
	memset(c.s.bat_t, 0, sizeof(c.s.bat_t));
	if (b->valid && !b->online && b->ua < 0 && b->uv > 0 &&
	    tot >= BAT_MIN_GAME_S && best >= BAT_MAJORITY * tot) {
		struct opp_acc *a = &c.s.acc[bc][bo];

		a->bat_n++;
		a->bat_ua += -(double)b->ua;
		a->bat_uv += b->uv;
		a->bat_mw += -(double)b->ua * b->uv / 1e9;
	}
}

/* ------------------------------------------------------------ controller */

static void change_opp(int to, const char *reason, const char *extra,
		       int64_t now)
{
	struct wstats w;
	double p = c.s.period_ms;
	int from = c.s.opp;

	window_stats(&w);
	if (to > from) {
		c.s.acc[c.s.cls][from].left_up++;
		c.s.ups++;
		if (c.s.last_up && now - c.s.last_up > HOLD_RESET_NS)
			c.s.hold = HOLD_BASE_NS;
		else if (c.s.last_down && now - c.s.last_down < RECENT_DOWN_NS)
			c.s.hold = c.s.hold * 2 > HOLD_MAX_NS ? HOLD_MAX_NS :
				   c.s.hold * 2;
		c.s.hold_until = now + c.s.hold;
		c.s.last_up = now;
		if (now < c.s.probation_until && !c.s.start_failed) {
			c.s.start_failed = 1;
			log_msg("cpufreq: learned start %d MHz stepped up within "
				"%lld s, will start at the top next time",
				c.s.start_mhz, (long long)(PROBATION_NS / NS));
		}
	} else {
		c.s.downs++;
		c.s.last_down = now;
	}
	if (log_debug())
		log_msg("cpufreq: %ld -> %ld kHz (%s), %d frames work p50/p99/max "
			"%.2f/%.2f/%.2f ms (%.0f/%.0f/%.0f%% of %.2f ms)%s%s, "
			"hold %lld s", c.khz[from], c.khz[to], reason, w.n, w.p50,
			w.p99, w.max, 100 * w.p50 / p, 100 * w.p99 / p,
			100 * w.max / p, p, extra ? ", " : "", extra ? extra : "",
			(long long)(c.s.hold_until > now ?
				    (c.s.hold_until - now) / NS : 0));
	c.s.opp = to;
	c.s.changes++;
	c.s.acc[c.s.cls][to].entered++;
	window_reset();
	c.s.settle = 1;
	c.s.verify = VERIFY_FRAMES;
	apply();
}

static void control(float ms, unsigned missed, int cpu_miss, int64_t now)
{
	double p = c.s.period_ms;
	struct wstats w;
	char extra[96];

	if (c.s.warmup > 0) {
		/* Single misses are expected here (audio restart); repeated
		 * CPU bound ones mean the floor is too low. */
		c.s.warmup--;
		c.s.warmup_cpu_missed += cpu_miss ? missed : 0;
		if (c.s.warmup_cpu_missed >= WARMUP_MISSES && c.s.opp < c.top) {
			c.s.jumps++;
			change_opp(c.top, "missed vblanks in warmup", NULL, now);
		}
		return;
	}
	if (missed) {
		if (cpu_miss && c.s.opp < c.top) {
			c.s.jumps++;
			change_opp(c.top, "missed vblank", NULL, now);
			return;
		}
		c.s.win_missed += missed;
	}
	if (c.s.settle > 0) {
		c.s.settle--;
		return;
	}
	c.s.win[c.s.wn++] = ms;
	if (ms > UP_FRAC * p && ++c.s.over >= UP_COUNT && c.s.opp < c.top) {
		change_opp(c.s.opp + 1, "high load", NULL, now);
		return;
	}
	if (c.s.wn < CTL_WINDOW)
		return;
	window_stats(&w);
	if (c.s.opp > 0 && !c.s.win_missed && now >= c.s.hold_until) {
		long cur = cpu_freq_khz(), f = c.khz[c.s.opp];
		double r = (double)(cur > f ? cur : f) / c.khz[c.s.opp - 1];

		if (w.p99 * r < DOWN_P99_FRAC * p && w.max * r < DOWN_MAX_FRAC * p) {
			snprintf(extra, sizeof(extra), "observed %ld kHz, predicted "
				 "p99/max %.2f/%.2f ms (%.0f/%.0f%%)", cur, w.p99 * r,
				 w.max * r, 100 * w.p99 * r / p, 100 * w.max * r / p);
			change_opp(c.s.opp - 1, "margin", extra, now);
			return;
		}
	}
	window_reset();
}

void cpuctl_game_frame(int64_t work_ns, unsigned missed, int64_t now)
{
	const struct battery *b = battery_get();
	struct opp_acc *a;
	float ms = (float)(work_ns / 1e6);
	int64_t dt;
	double dts;
	int cpu_miss = 0, bin;
	unsigned x;

	if (!c.s.active)
		return;
	dt = c.s.last_ns ? now - c.s.last_ns : 0;
	if (dt <= 0 || dt > MAX_DT_NS)
		dt = (int64_t)(c.s.period_ms * 1e6);
	c.s.last_ns = now;
	dts = dt / 1e9;

	a = &c.s.acc[c.s.cls][c.s.opp];
	a->frames++;
	a->seconds += dts;
	a->work_sum += ms;
	if (ms > a->work_max)
		a->work_max = ms;
	bin = (int)(ms / HIST_BIN_MS);
	a->hist[bin < 0 ? 0 : bin >= HIST_BINS ? HIST_BINS - 1 : bin]++;
	a->bl_sum += settings.brightness * dts;
	x = audio_xruns_total();
	a->underruns += x - c.s.xruns_seen;
	c.s.xruns_seen = x;
	c.s.bat_t[c.s.cls][c.s.opp] += dts;
	if (b->seq != c.s.bat_seq) {
		c.s.bat_seq = b->seq;
		bat_sample(b);
	}
	if (missed) {
		struct opp_acc *pa = &c.s.acc[c.s.prev_cls][c.s.prev_opp];

		cpu_miss = c.s.prev_work >= MISS_CPU_FRAC * c.s.period_ms;
		pa->missed += missed;
		if (cpu_miss)
			pa->missed_cpu += missed;
	}
	c.s.prev_work = ms;
	c.s.prev_cls = c.s.cls;
	c.s.prev_opp = c.s.opp;

	if (c.s.verify > 0 && --c.s.verify == 0 && log_debug())
		log_msg("cpufreq: target %ld kHz, util_min %u (%s), observed %ld kHz",
			c.khz[c.s.opp], opp_util(c.s.opp), act_names[c.act],
			cpu_freq_khz());
	if (c.mode == CPU_MODE_ADAPTIVE && c.n > 1)
		control(ms, missed, cpu_miss, now);
}

/* --------------------------------------------------------------- session */

void cpuctl_game_start(const char *rom_name, double refresh_hz, int start_mhz,
		       int64_t now)
{
	char mode[24], ks[256] = "not available";

	memset(&c.s, 0, sizeof(c.s));
	snprintf(c.s.rom, sizeof(c.s.rom), "%s", rom_name);
	c.s.in_game = c.s.active = 1;
	c.s.period_ms = 1000.0 / (refresh_hz > 1 ? refresh_hz : 60);
	c.s.cls = c.mode == CPU_MODE_ADAPTIVE ? CLS_ADAPTIVE : CLS_PINNED;
	c.s.opp = mode_opp(c.mode);
	c.s.start_mhz = start_mhz;
	if (c.mode == CPU_MODE_ADAPTIVE && start_mhz > 0) {
		c.s.opp = mode_opp(start_mhz);
		if (c.s.opp < c.top)
			c.s.probation_until = now + PROBATION_NS;
	}
	c.s.prev_cls = c.s.cls;
	c.s.prev_opp = c.s.opp;
	c.s.hold = HOLD_BASE_NS;
	c.s.warmup = CTL_WARMUP;
	c.s.verify = VERIFY_FRAMES;
	c.s.xruns_seen = audio_xruns_total();
	c.s.bat_seq = battery_get()->seq;
	c.s.acc[c.s.cls][c.s.opp].entered++;
	c.s.have_kstats = read_kstats(c.s.tis0, &c.s.trans0) == 0;
	if (c.s.have_kstats)
		format_kstats(ks, sizeof(ks), c.s.tis0, c.s.trans0, "");
	mode_name(c.mode, mode, sizeof(mode));
	log_msg("cpufreq: game start, mode %s, %ld kHz (util_min %u%s), "
		"actuator %s, governor stays schedutil", mode, c.khz[c.s.opp],
		opp_util(c.s.opp), c.s.probation_until ? ", learned start" : "",
		act_names[c.act]);
	log_msg("cpufreq stats at game start: %s", ks);
	apply();
}

void cpuctl_game_pause(void)
{
	if (!c.s.in_game || !c.s.active)
		return;
	c.s.active = 0;
	apply();
	log_msg("cpufreq: paused (menu or sleep), no floor (game target %ld "
		"kHz kept)",
		c.khz[c.s.opp]);
}

void cpuctl_game_resume(void)
{
	if (!c.s.in_game || c.s.active)
		return;
	c.s.active = 1;
	c.s.warmup = CTL_WARMUP;
	c.s.warmup_cpu_missed = 0;
	c.s.settle = 0;
	c.s.last_ns = 0;
	c.s.prev_work = 0;
	c.s.xruns_seen = audio_xruns_total();
	window_reset();
	apply();
	log_msg("cpufreq: resume at %ld kHz", c.khz[c.s.opp]);
}

/*
 * Light sleep (no suspend to RAM): no floor and scaling_max_freq at the
 * lowest OPP, so whatever still runs does so at the cheapest clock.
 */
void cpuctl_sleep(int on)
{
	if (c.act == ACT_NONE || !cpuctl_opp_count())
		return;
	if (on) {
		actuate(-1);
		write_long("scaling_max_freq", c.khz[0]);
	} else {
		write_long("scaling_max_freq", c.khz[c.top]);
		apply();
	}
	log_msg("cpufreq: %s, scaling_max_freq %ld kHz, cur %ld kHz",
		on ? "light sleep" : "awake", read_long("scaling_max_freq", -1),
		cpu_freq_khz());
}

int cpuctl_in_game(void)
{
	return c.s.in_game;
}

static int learn(void)
{
	int found = -1, mhz;

	if (c.mode != CPU_MODE_ADAPTIVE || c.n < 2)
		return c.s.start_mhz;
	if (c.s.start_failed)
		return 0;
	for (int i = 0; i <= c.top && found < 0; i++) {
		const struct opp_acc *a = &c.s.acc[CLS_ADAPTIVE][i];

		if (a->seconds >= LEARN_MIN_S && a->missed_cpu * 1000 <= a->frames &&
		    a->left_up * 60 <= a->seconds)
			found = i;
	}
	if (found < 0)
		return c.s.start_mhz;
	mhz = found == c.top ? 0 : (int)(c.khz[found] / 1000);
	if (mhz != c.s.start_mhz)
		log_msg("cpufreq: %s: start at %d MHz next time", c.s.rom,
			mhz ? mhz : (int)(c.khz[c.top] / 1000));
	return mhz;
}

static void timestamp(char *buf, size_t size)
{
	struct tm tm;

	if (realtime_utc(&tm))
		strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
	else
		snprintf(buf, size, "boot+%.0f", boot_ms() / 1000.0);
}

static size_t csv_quote(char *buf, size_t size, const char *s)
{
	size_t n = 0;

	if (size < 3)
		return 0;
	buf[n++] = '"';
	for (; *s && n + 3 < size; s++) {
		if (*s == '"')
			buf[n++] = '"';
		buf[n++] = *s;
	}
	buf[n++] = '"';
	buf[n] = 0;
	return n;
}

static const char csv_header[] =
	"time,boot_s,rom,mode,opp_khz,frames,seconds,work_avg_ms,work_p50_ms,"
	"work_p99_ms,work_max_ms,missed,missed_cpu,underruns,changes,"
	"bat_samples,avg_mA,avg_mV,avg_mW,backlight_pct,grid,grid_dim,"
	"grid_boost,color,kernel_s,kernel_trans,start_mhz,learned_mhz,"
	"audio_mode,audio_config,audio_measured_hz,audio_fill_min,"
	"audio_fill_max\n";

/* One CSV row and one log line. opp < 0: summary of a class. */
static size_t emit(char *buf, size_t size, const char *when, const char *qrom,
		   int cls, int opp, const struct opp_acc *a, double kernel_s,
		   long ktrans, int learned)
{
	const char *mode = cls == CLS_ADAPTIVE ? "adaptive" :
			   opp < 0 ? "pinned" : opp == c.top ? "max" : "fixed";
	double avg = a->frames ? a->work_sum / a->frames : 0;
	float p50 = hist_pct(a, 0.5), p99 = hist_pct(a, 0.99);
	char oppk[16], bat[80] = "", batl[80] = "no samples", ks[24] = "",
	     kt[24] = "", st[16] = "", le[16] = "", au[160];
	size_t n;

	audio_csv_fields(au, sizeof(au));

	if (opp < 0)
		snprintf(oppk, sizeof(oppk), "all");
	else
		snprintf(oppk, sizeof(oppk), "%ld", c.khz[opp]);
	if (a->bat_n) {
		snprintf(bat, sizeof(bat), "%.0f,%.0f,%.0f",
			 a->bat_ua / a->bat_n / 1000, a->bat_uv / a->bat_n / 1000,
			 a->bat_mw / a->bat_n);
		snprintf(batl, sizeof(batl), "%u samples %.0f mA %.0f mV %.0f mW",
			 a->bat_n, a->bat_ua / a->bat_n / 1000,
			 a->bat_uv / a->bat_n / 1000, a->bat_mw / a->bat_n);
	} else {
		snprintf(bat, sizeof(bat), ",,");
	}
	if (kernel_s >= 0)
		snprintf(ks, sizeof(ks), "%.2f", kernel_s);
	if (ktrans >= 0)
		snprintf(kt, sizeof(kt), "%ld", ktrans);
	if (opp < 0) {
		snprintf(st, sizeof(st), "%d", c.s.start_mhz);
		snprintf(le, sizeof(le), "%d", learned);
	}
	n = snprintf(buf, size, "%s,%.1f,%s,%s,%s,%llu,%.1f,%.2f,%.2f,%.2f,%.2f,"
		     "%u,%u,%u,%u,%u,%s,%.0f,%d,%d,%d,%s,%s,%s,%s,%s,%s\n", when,
		     boot_ms() / 1000.0, qrom, mode, oppk,
		     (unsigned long long)a->frames, a->seconds, avg, p50, p99,
		     a->work_max, a->missed, a->missed_cpu, a->underruns,
		     opp < 0 ? c.s.changes : a->entered, a->bat_n, bat,
		     a->seconds > 0 ? a->bl_sum / a->seconds : settings.brightness,
		     settings.grid, settings.grid_dim, settings.grid_boost,
		     color_mode_names[rom_settings.color], ks, kt, st, le, au);
	log_msg("perf summary: %s %s: %llu frames %.1f s, work avg/p50/p99/max "
		"%.2f/%.2f/%.2f/%.2f ms, missed %u (cpu %u), underruns %u, "
		"%s %u, bat %s, backlight %.0f%%", mode, oppk,
		(unsigned long long)a->frames, a->seconds, avg, p50, p99,
		a->work_max, a->missed, a->missed_cpu, a->underruns,
		opp < 0 ? "changes" : "entered", opp < 0 ? c.s.changes : a->entered,
		batl, a->seconds > 0 ? a->bl_sum / a->seconds : settings.brightness);
	return n < size ? n : size;
}

/*
 * A perf.csv with a different header (older build, fewer columns) is
 * renamed to perf-old.csv (replacing an earlier one) and a new file is
 * started, so every file has one consistent header.
 */
static void csv_rotate_old(const char *path)
{
	char head[sizeof(csv_header)], old[300];
	int n = read_file(path, head, sizeof(head) - 1);

	if (n <= 0)
		return;
	head[n] = 0;
	if (!strncmp(head, csv_header, sizeof(csv_header) - 1))
		return;
	snprintf(old, sizeof(old), "%s/log/perf-old.csv", fe_data_dir);
	if (rename(path, old) == 0)
		log_msg("cpufreq: perf.csv had another header, moved to %s", old);
}

static void csv_append(const char *rows, size_t len)
{
	char path[300];
	struct stat st;
	int fd;

	snprintf(path, sizeof(path), "%s/log/perf.csv", fe_data_dir);
	csv_rotate_old(path);
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0) {
		log_msg("cpufreq: open %s: %s", path, strerror(errno));
		return;
	}
	if (fstat(fd, &st) == 0 && st.st_size == 0 &&
	    write(fd, csv_header, sizeof(csv_header) - 1) < 0)
		log_msg("cpufreq: write %s: %s", path, strerror(errno));
	if (write(fd, rows, len) != (ssize_t)len)
		log_msg("cpufreq: write %s: %s", path, strerror(errno));
	fsync(fd);
	close(fd);
}

int cpuctl_game_end(void)
{
	static char rows[12288];
	static struct opp_acc tot;
	long tis[CPU_MAX_OPPS], trans = -1;
	char when[32], qrom[300], ks[256];
	int have, learned;
	size_t n = 0;

	if (!c.s.in_game)
		return 0;
	c.s.active = 0;
	apply();
	learned = learn();
	have = c.s.have_kstats && read_kstats(tis, &trans) == 0;
	if (have) {
		for (int i = 0; i < c.n; i++)
			tis[i] -= c.s.tis0[i];
		trans -= c.s.trans0;
		format_kstats(ks, sizeof(ks), tis, trans, "+");
		log_msg("cpufreq stats this game (incl. menus): %s", ks);
	} else {
		log_msg("cpufreq stats: not available (CONFIG_CPU_FREQ_STAT)");
	}

	timestamp(when, sizeof(when));
	csv_quote(qrom, sizeof(qrom), c.s.rom);
	log_msg("perf summary: %s, %s, grid %s dim %d%% boost %d%%, color %s, "
		"start %d MHz, learned %d MHz", c.s.rom, when,
		settings.grid ? "on" : "off", settings.grid_dim,
		settings.grid_boost, color_mode_names[rom_settings.color],
		c.s.start_mhz, learned);
	for (int k = 0; k < CLS_COUNT; k++) {
		int used = 0;

		memset(&tot, 0, sizeof(tot));
		for (int i = 0; i < c.n; i++) {
			const struct opp_acc *a = &c.s.acc[k][i];

			if (!a->frames)
				continue;
			used = 1;
			acc_add(&tot, a);
			n += emit(rows + n, sizeof(rows) - n, when, qrom, k, i, a,
				  have ? tis[i] / 100.0 : -1, -1, learned);
		}
		if (used)
			n += emit(rows + n, sizeof(rows) - n, when, qrom, k, -1, &tot,
				  -1, have ? trans : -1, learned);
	}
	if (n)
		csv_append(rows, n);
	c.s.in_game = 0;
	return learned;
}

/* ------------------------------------------------------------- reporting */

void cpuctl_format(char *buf, int size)
{
	char mode[24], hold[24] = "";
	int64_t left = c.s.hold_until - (c.s.last_ns ? c.s.last_ns : now_ns());

	mode_name(c.mode, mode, sizeof(mode));
	if (!c.s.in_game) {
		snprintf(buf, size, "cpu %s, no floor", mode);
		return;
	}
	if (c.mode == CPU_MODE_ADAPTIVE && left > 0)
		snprintf(hold, sizeof(hold), ", hold %lld s",
			 (long long)(left / NS));
	snprintf(buf, size, "cpu %s floor %ld MHz (util_min %u, %s), "
		 "%u changes%s", mode, c.khz[c.s.opp] / 1000,
		 opp_util(c.s.opp), act_names[c.act], c.s.changes, hold);
}

void cpuctl_counters(struct cpuctl_counters *k)
{
	unsigned missed = 0, cpu = 0;

	for (int j = 0; j < CLS_COUNT; j++)
		for (int i = 0; i < c.n; i++) {
			missed += c.s.acc[j][i].missed;
			cpu += c.s.acc[j][i].missed_cpu;
		}
	k->changes = c.s.changes;
	k->ups = c.s.ups;
	k->downs = c.s.downs;
	k->jumps = c.s.jumps;
	k->missed = missed;
	k->missed_cpu = cpu;
	k->hold_ns = c.s.hold;
}
