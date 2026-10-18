/*
 * Audio: RK817 codec through tinyalsa, card 0 device 0.
 *
 * tinyalsa on arm64. The kernel only lets x86/ppc/alpha mmap the PCM
 * status and control records, so tinyalsa falls back to the SYNC_PTR
 * ioctl with a cached copy of appl_ptr. pcm_state() (which pcm_writei()
 * itself calls before every write) issues SYNC_PTR without
 * SNDRV_PCM_SYNC_PTR_APPL, which makes the kernel take the cached
 * appl_ptr as the new one. Plain read/write I/O never refreshes that
 * cache, so every write first rewound appl_ptr to where it was at the
 * last prepare: in simple mode the queue never grew past one write
 * (fill 798..801), never reached the start threshold, and nothing
 * played; in nonblocking mode the rewinds behind a running hw_ptr showed
 * up as underruns and as the device "consuming twice the rate". So after
 * every write the cache is refreshed with pcm_avail_update(), which
 * syncs with SYNC_PTR_APPL, and the PCM state is read the same way.
 *
 * Mixer. The mainline rk817 codec has two playback controls: "Master
 * Playback Volume" (0..255, inverted register, 255 = 0 dB, 0.375 dB/step)
 * and "Playback Mux" (HP/SPK), a virtual enum without a register that
 * boots as "HP". In DAPM "HP" powers DAC L/R and the headphone stage,
 * "SPK" powers SPK DAC with the class D amp; HPOL, HPOR and SPKO all hang
 * off the mux. simple-card's hp-det jack only gates the "Headphones"
 * pin, "Speaker" stays enabled, so the jack never changes the route by
 * itself. The first two hardware runs had the speaker audible (crackling
 * game sound, no headphones plugged) with the mux at HP, as on ROCKNIX
 * (pid351 probe), so HP is the default for both jack states. The mux is
 * reapplied on every jack event, and <data>/mixer.txt can set any control
 * for both states ("Name=value") or for one ("speaker: Name=value",
 * "headphones: Name=value"), e.g. "speaker: Playback Mux=SPK".
 *
 * Timing. The display is the master clock: one retro_run per vblank
 * (60.02 Hz) although the GBA runs at 59.73 Hz. The resampling ratio is
 * corrected for that difference up front, and dynamic rate control
 * adjusts it by up to +-0.5% to keep the ALSA buffer at its target fill,
 * which absorbs the remaining drift between the panel and codec clocks.
 *
 * Modes (settings.audio). Adaptive, the default: the calibrated,
 * nonblocking, rate controlled path below. Simple: 48 kHz S16 512x8 with
 * blocking writes, the stream starting by itself at half a buffer, a
 * fixed resampling ratio and half a buffer of silence after an underrun;
 * this is what pid351 runs on this board on mainline.
 *
 * Sleep (main.c): audio_suspend() drops the stream and closes the PCM,
 * so nothing is running when the kernel suspends the sound card and the
 * hw_params (I2S and codec clocks) are applied again on wake.
 * audio_resume() reopens the same config and restarts the rate control
 * from scratch, so the gap is not mistaken for a drifting device.
 *
 * Diagnostics in both modes: 3 s into each game the clock and I2S
 * registers (and once per boot clk_summary and /proc/asound) are logged;
 * at game end audio_summary() logs mode, config, calibration, the device
 * rate measured during play, underruns, fill, the mux and the DAPM power
 * state; perf.csv gets the same as columns; the settings screen has a
 * 440 Hz test tone that logs the same.
 *
 * Calibration (adaptive). Before the first game the open stream plays
 * silence for a moment and the frames it really consumed are fitted
 * against CLOCK_MONOTONIC. Within 2% of nominal it is kept as is. Only
 * if it is far off, a larger buffer is opened once and its measured rate
 * is used as the device rate for resampling. Nothing else is reopened,
 * since every reopen pops. Everything measured is logged ("audio:
 * calibrate ...") together with the clock and I2S registers.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

/* In libtinyalsa but missing from its public header. */
int pcm_state(struct pcm *pcm);
int pcm_avail_update(struct pcm *pcm);

#include "audio.h"
#include "fe.h"
#include "resample.h"

#define CARD 0
#define DEVICE 0
#define DRC_MAX 0.005
#define FILL_EMA 0.05
#define OUT_MAX 4096  /* frames per video frame, ~5x what is needed */

#define CAL_WARMUP_NS 60000000LL   /* let the stream start and settle */
#define CAL_WINDOW_NS 300000000LL  /* fitted part */
#define CAL_STEP_NS 2000000LL
#define CAL_TOL 0.02

struct cand {
	unsigned rate, period, periods;
	int s32;  /* S32_LE (one FIFO word per sample) instead of S16_LE */
};

/*
 * Adaptive mode opens the first of these that the driver accepts; pid351
 * runs 48 kHz S16 512x8 on mainline 6.12 on this board.
 */
static const struct cand cands[] = {
	{ 48000, 512, 8, 0 },   /* 85 ms */
	{ 48000, 1024, 4, 0 },  /* same buffer, more DMA refill slack */
	{ 48000, 512, 8, 1 },   /* S32: rules out a 16 bit packing problem */
	{ 44100, 512, 8, 0 },   /* other fractional divider setting */
};
/* Nothing matched: big enough to survive a 2x device rate. */
static const struct cand fallback = { 48000, 1024, 8, 0 };
/* AUDIO_SIMPLE: exactly what pid351 runs on this board (blocking writes,
 * start at half a buffer, re-prime half a buffer after an underrun). */
static const struct cand simple_cand = { 48000, 512, 8, 0 };

#define MAX_CAL 8

static struct {
	struct pcm *pcm;
	struct mixer *mixer;
	int null_dev;
	double null_ratio;  /* simulated device speed / nominal */
	double null_fill, null_hz, null_consume;  /* simulated device */
	int null_xrun;
	int null_running;   /* simulated start threshold reached */
	struct cand cand;   /* currently open */
	int mode;           /* AUDIO_SIMPLE / AUDIO_ADAPTIVE of the open PCM */
	struct { struct cand c; double rate; } cal[MAX_CAL];
	int ncal;
	double hw_mclk, hw_codec_mclk, hw_lrck;  /* from registers, 0: unknown */
	/* This game: underruns, fill range, rate measured between flushes
	 * (intervals without an underrun only). */
	unsigned sess_xruns;
	int sess_fill_min, sess_fill_max;
	uint64_t sess_frames;
	int64_t sess_ns;
	uint64_t prev_played;
	int64_t prev_ns;
	int prev_ok;
	unsigned flushes;
	int game_dumped;
	int jack;           /* headphones: 1 in, 0 out, -1 not reported yet */
	char mixer_text[2048];  /* <data>/mixer.txt, read once */
	int mixer_text_len; /* -1: no file */
	unsigned rate, buffer, period, target;
	double dev_rate;    /* frames/s the device really plays */
	int calibrated;
	int started;
	int suspended;      /* audio_suspend() closed the PCM */
	double base_step, adj, fill_ema;
	struct resampler rs;
	int16_t out[OUT_MAX * 2];
	int32_t conv[512 * 2];
	size_t out_n;
	struct audio_stats st;
	unsigned xruns_total;
	int64_t time_ns;
	int64_t next_init_ns;
	int dumped, hw_dumped;
	/* Measured consumption rate: frames written minus frames queued */
	uint64_t written;
	uint64_t rate_frames;
	int64_t rate_ns;
} a = { .adj = 1.0, .null_ratio = 1.0, .st = { .fill_min = -1 }, .jack = -1,
	.mixer_text_len = -1 };

/* ---------------------------------------------------------- diagnostics */

/* Log a multi line text, optionally only lines containing one of the
 * comma separated words in filter. */
static void log_lines(const char *prefix, const char *text, const char *filter)
{
	while (*text) {
		const char *eol = strchr(text, '\n');
		size_t len = eol ? (size_t)(eol - text) : strlen(text);
		char line[200];
		int keep = !filter;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, text, len);
		line[len] = 0;
		for (const char *f = filter; f && *f && !keep;) {
			const char *c = strchr(f, ',');
			size_t fl = c ? (size_t)(c - f) : strlen(f);
			char w[32];

			if (fl < sizeof(w)) {
				memcpy(w, f, fl);
				w[fl] = 0;
				keep = strstr(line, w) != NULL;
			}
			f = c ? c + 1 : f + fl;
		}
		if (keep && len)
			log_verbose("%s%s", prefix, line);
		if (!eol)
			break;
		text = eol + 1;
	}
}

static void log_file(const char *prefix, const char *path, const char *filter)
{
	static char buf[65536];
	int n = read_file(path, buf, sizeof(buf) - 1);

	if (n < 0) {
		log_verbose("%s%s: %s", prefix, path, strerror(errno));
		return;
	}
	buf[n] = 0;
	log_lines(prefix, buf, filter);
}

/* Read n 32 bit registers at phys (same page) through /dev/mem. */
static int devmem_read(uint32_t phys, uint32_t *val, unsigned n)
{
	long pg = sysconf(_SC_PAGESIZE);
	uint32_t base = phys & ~(uint32_t)(pg - 1);
	volatile uint32_t *p;
	void *map;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);

	if (fd < 0)
		return -1;
	map = mmap(NULL, pg, PROT_READ, MAP_SHARED, fd, base);
	close(fd);
	if (map == MAP_FAILED)
		return -1;
	p = (volatile uint32_t *)((char *)map + (phys - base));
	for (unsigned i = 0; i < n; i++)
		val[i] = p[i];
	munmap(map, pg);
	return 0;
}

#define PX30_CRU 0xff2b0000u
#define PX30_PMUCRU 0xff2bc000u
#define PX30_I2S1 0xff070000u
#define PX30_GPIO2 0xff260000u

/* rk3328 style PLL: CON0..2 at con, mode bit selects normal vs xin24m. */
static double pll_hz(uint32_t con, uint32_t mode_reg, int mode_bit,
		     char *desc, size_t size)
{
	uint32_t c[3], mode;
	unsigned fb, post1, ref, post2, dsmpd, frac;
	double hz;

	if (devmem_read(con, c, 3) || devmem_read(mode_reg, &mode, 1))
		return 0;
	fb = c[0] & 0xfff;
	post1 = (c[0] >> 12) & 7;
	ref = c[1] & 0x3f;
	post2 = (c[1] >> 6) & 7;
	dsmpd = (c[1] >> 12) & 1;
	frac = c[2] & 0xffffff;
	if (!ref || !post1 || !post2)
		return 0;
	hz = 24e6 / ref * (fb + (dsmpd ? 0 : frac / 16777216.0)) / post1 / post2;
	if (!((mode >> mode_bit) & 1))
		hz = 24e6;  /* slow mode */
	snprintf(desc, size, "%.0f Hz (fb %u ref %u post %u/%u frac %s%u, %s)",
		 hz, fb, ref, post1, post2, dsmpd ? "off " : "", frac,
		 (mode >> mode_bit) & 1 ? "normal" : "slow");
	return hz;
}

/*
 * Clock tree and I2S dividers as the hardware has them, i.e. what really
 * sets LRCK: SCLK_I2S1 = mux(CLKSEL_CON30[11:10]) of i2s1_src
 * (gpll/npll / (div+1)), the fractional divider (CON31 num/den) or
 * xin12m; the codec gets clk_i2s1 or xin12m (CON30[15]); the I2S divides
 * MCLK by MDIV for BCLK and BCLK by TSD for LRCK. Only call while the
 * stream runs: the I2S registers need its hclk, which runtime PM gates.
 */
static void hw_dump(const char *when)
{
	uint32_t sel[2], gate, i2s[5], xfer;
	char gdesc[96], ndesc[96];
	double gpll, npll, src, frac = 0, mclk = 0, codec_mclk, bclk, lrck;
	unsigned div, num, den, mux, mdiv, tsd;

	if (!log_debug())
		return;
	gpll = pll_hz(PX30_PMUCRU, PX30_PMUCRU + 0x20, 0, gdesc, sizeof(gdesc));
	npll = pll_hz(PX30_CRU + 0x60, PX30_CRU + 0xa0, 6, ndesc, sizeof(ndesc));
	if (!gpll || devmem_read(PX30_CRU + 0x178, sel, 2) ||
	    devmem_read(PX30_CRU + 0x228, &gate, 1) ||
	    devmem_read(PX30_I2S1, i2s, 5) ||
	    devmem_read(PX30_I2S1 + 0x1c, &xfer, 1)) {
		log_msg("audio: hw regs (%s): /dev/mem not readable: %s", when,
			strerror(errno));
		return;
	}
	div = sel[0] & 0x7f;
	mux = (sel[0] >> 10) & 3;
	num = sel[1] >> 16;
	den = sel[1] & 0xffff;
	src = ((sel[0] >> 8) & 1 ? npll : gpll) / (div + 1);
	if (den)
		frac = src * num / den;
	mclk = mux == 0 ? src : mux == 1 ? frac : mux == 3 ? 12e6 : 0;
	codec_mclk = (sel[0] >> 15) & 1 ? 12e6 : mclk;
	mdiv = ((i2s[2] >> 16) & 0xff) + 1;
	tsd = (i2s[2] & 0xff) + 1;
	bclk = mclk / mdiv;
	lrck = bclk / tsd;
	log_msg("audio: hw regs (%s): gpll %s, npll %s", when, gdesc, ndesc);
	log_msg("audio: hw regs (%s): CLKSEL30 %08x CLKSEL31 %08x GATE10 %08x: "
		"i2s1_src %s/%u = %.0f Hz, frac %u/%u = %.0f Hz, mux %s -> "
		"MCLK %.0f Hz, codec MCLK %s %.0f Hz", when, sel[0], sel[1], gate,
		(sel[0] >> 8) & 1 ? "npll" : "gpll", div + 1, src, num, den, frac,
		mux == 0 ? "src" : mux == 1 ? "frac" : mux == 2 ? "clkin" : "xin12m",
		mclk, (sel[0] >> 15) & 1 ? "xin12m" : "clk_i2s1", codec_mclk);
	log_msg("audio: hw regs (%s): I2S TXCR %08x (VDW %u HWT %u CSR %u TFS %u) "
		"CKR %08x (MDIV %u TSD %u MSS %s TRCM %u) FIFOLR %08x DMACR %08x "
		"XFER %08x -> BCLK %.0f Hz, LRCK %.0f Hz", when, i2s[0],
		(i2s[0] & 0x1f) + 1, (i2s[0] >> 14) & 1, (i2s[0] >> 15) & 3,
		(i2s[0] >> 5) & 1, i2s[2], mdiv, tsd,
		(i2s[2] >> 27) & 1 ? "slave" : "master", (i2s[2] >> 28) & 3,
		i2s[3], i2s[4], xfer, bclk, lrck);
	a.hw_mclk = mclk;
	a.hw_codec_mclk = codec_mclk;
	a.hw_lrck = lrck;
}

/* Kernel side view, once: clock rates, ALSA hw/sw params and status. */
static void kernel_dump(void)
{
	struct stat st;

	if (!log_debug())
		return;
	if (fe_is_init && stat("/sys/kernel/debug/clk", &st))
		mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
	log_file("audio: clk ", "/sys/kernel/debug/clk/clk_summary",
		 "i2s,pll,xin,clock ");
	log_file("audio: hw_params: ", "/proc/asound/card0/pcm0p/sub0/hw_params",
		 NULL);
	log_file("audio: sw_params: ", "/proc/asound/card0/pcm0p/sub0/sw_params",
		 NULL);
	log_file("audio: status: ", "/proc/asound/card0/pcm0p/sub0/status", NULL);
}

/* Ask ALSA to report XRUNs and hw_ptr jumps in dmesg (SND_PCM_XRUN_DEBUG). */
static void xrun_debug_enable(void)
{
	if (!log_debug())
		return;
	int fd = open("/proc/asound/card0/pcm0p/xrun_debug", O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, "1\n", 2) == 2)
		log_verbose("audio: ALSA xrun_debug enabled");
	close(fd);
}

/* ---------------------------------------------------------------- mixer */

static void mixer_dump(const char *when)
{
	if (!log_debug())
		return;
	unsigned n = mixer_get_num_ctls(a.mixer);

	log_verbose("mixer \"%s\" (%s): %u controls", mixer_get_name(a.mixer),
		    when, n);
	for (unsigned i = 0; i < n; i++) {
		struct mixer_ctl *ctl = mixer_get_ctl(a.mixer, i);
		enum mixer_ctl_type t = mixer_ctl_get_type(ctl);
		unsigned nv = mixer_ctl_get_num_values(ctl);
		char vals[256];
		int off = 0;

		vals[0] = 0;
		if (t == MIXER_CTL_TYPE_ENUM) {
			int cur = mixer_ctl_get_value(ctl, 0);

			for (unsigned e = 0; e < mixer_ctl_get_num_enums(ctl) &&
			     off < (int)sizeof(vals) - 40; e++)
				off += snprintf(vals + off, sizeof(vals) - off,
						(int)e == cur ? " >%s<" : " %s",
						mixer_ctl_get_enum_string(ctl, e));
		} else if (t == MIXER_CTL_TYPE_INT || t == MIXER_CTL_TYPE_BOOL) {
			for (unsigned v = 0; v < nv && off < (int)sizeof(vals) - 40; v++)
				off += snprintf(vals + off, sizeof(vals) - off, " %d",
						mixer_ctl_get_value(ctl, v));
			if (t == MIXER_CTL_TYPE_INT)
				snprintf(vals + off, sizeof(vals) - off,
					 " (range %d..%d)",
					 mixer_ctl_get_range_min(ctl),
					 mixer_ctl_get_range_max(ctl));
		}
		log_verbose("  ctl %u \"%s\" %s x%u:%s", i, mixer_ctl_get_name(ctl),
			    mixer_ctl_get_type_string(ctl), nv, vals);
	}
}

/* Set a control by name: enum by string, numbers for int/bool (one value
 * for all channels or a comma separated list). */
static int mixer_set(const char *name, const char *value)
{
	struct mixer_ctl *ctl;
	enum mixer_ctl_type t;
	int r = 0;

	if (!a.mixer)
		return -1;
	ctl = mixer_get_ctl_by_name(a.mixer, name);
	if (!ctl) {
		log_msg("mixer: no control \"%s\"", name);
		return -1;
	}
	t = mixer_ctl_get_type(ctl);
	if (t == MIXER_CTL_TYPE_ENUM) {
		r = mixer_ctl_set_enum_by_string(ctl, value);
	} else if (t == MIXER_CTL_TYPE_INT || t == MIXER_CTL_TYPE_BOOL) {
		unsigned nv = mixer_ctl_get_num_values(ctl);
		const char *p = value;
		long v = strtol(p, NULL, 0);

		for (unsigned i = 0; i < nv && r == 0; i++) {
			r = mixer_ctl_set_value(ctl, i, (int)v);
			p = strchr(p, ',');
			if (p)
				v = strtol(++p, NULL, 0);
			else
				p = "";
		}
	} else {
		log_msg("mixer: \"%s\" has unsupported type %s", name,
			mixer_ctl_get_type_string(ctl));
		return -1;
	}
	if (r)
		log_msg("mixer: set \"%s\" = %s failed (%d, %s)", name, value, r,
			strerror(errno));
	else
		log_msg("mixer: \"%s\" = %s", name, value);
	return r ? -1 : 0;
}

/*
 * Apply "Control Name=value" lines. A line may start with "speaker:" or
 * "headphones:" to apply only in that jack state; jack < 0 applies only
 * the lines without a prefix.
 */
static int apply_mixer_lines(const char *text, int jack)
{
	int n = 0;

	while (*text) {
		const char *eol = strchr(text, '\n'), *eq, *l;
		size_t len = eol ? (size_t)(eol - text) : strlen(text);
		char line[160];
		int want = -1;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, text, len);
		line[len] = 0;
		while (len && (line[len - 1] == '\r' || line[len - 1] == ' '))
			line[--len] = 0;
		l = line;
		if (!strncmp(l, "speaker:", 8)) {
			want = 0;
			l += 8;
		} else if (!strncmp(l, "headphones:", 11)) {
			want = 1;
			l += 11;
		}
		while (*l == ' ')
			l++;
		eq = strchr(l, '=');
		if (l[0] != '#' && eq && eq != l && want == jack) {
			char name[160];

			memcpy(name, l, (size_t)(eq - l));
			name[eq - l] = 0;
			if (mixer_set(name, eq + 1) == 0)
				n++;
		}
		if (!eol)
			break;
		text = eol + 1;
	}
	return n;
}

int audio_apply_mixer_text(const char *text)
{
	return apply_mixer_lines(text, -1);
}

/* Route for the current jack state, then the matching mixer.txt lines. */
static void mixer_route(void)
{
	int jack = a.jack > 0;

	mixer_set("Playback Mux", "HP");  /* see the top of this file */
	if (a.mixer_text_len >= 0)
		apply_mixer_lines(a.mixer_text, jack);
}

static const char *mux_state(void)
{
	struct mixer_ctl *ctl = a.mixer ?
		mixer_get_ctl_by_name(a.mixer, "Playback Mux") : NULL;
	int v;

	if (!ctl)
		return "unknown";
	v = mixer_ctl_get_value(ctl, 0);
	if (v < 0 || (unsigned)v >= mixer_ctl_get_num_enums(ctl))
		return "unknown";
	return mixer_ctl_get_enum_string(ctl, (unsigned)v);
}

static void mixer_setup(void)
{
	char path[300], buf[16];
	int n;

	a.mixer = mixer_open(CARD);
	if (!a.mixer) {
		log_msg("mixer: cannot open card %d: %s", CARD, strerror(errno));
		return;
	}
	if (!a.dumped)
		mixer_dump("as found");
	snprintf(buf, sizeof(buf), "%d", settings.volume);
	mixer_set("Master Playback Volume", buf);

	snprintf(path, sizeof(path), "%s/mixer.txt", fe_data_dir);
	n = read_file(path, a.mixer_text, sizeof(a.mixer_text) - 1);
	a.mixer_text_len = n;
	if (n >= 0) {
		a.mixer_text[n] = 0;
		log_msg("mixer: applying %s", path);
		apply_mixer_lines(a.mixer_text, -1);
	}
	mixer_route();
	if (!a.dumped) {
		mixer_dump("after setup");
		xrun_debug_enable();
	}
	a.dumped = 1;
}

void audio_set_volume(int raw)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", raw);
	mixer_set("Master Playback Volume", buf);
}

/*
 * The RG351P detect switch pulls GPIO2_PC6 low when a plug is in (vendor
 * BSP DT: GPIO_ACTIVE_LOW; the mainline RG351M dtsi says ACTIVE_HIGH and
 * reported "inserted" with nothing plugged in). The board DTS overrides
 * the polarity; the raw pin level is logged so each run confirms it.
 */
void audio_jack(int inserted)
{
	uint32_t v;
	char raw[48] = "pin level unknown";

	if (fe_is_init && devmem_read(PX30_GPIO2 + 0x50, &v, 1) == 0)
		snprintf(raw, sizeof(raw), "GPIO2_PC6 level %s",
			 (v >> 22) & 1 ? "high" : "low");
	a.jack = inserted ? 1 : 0;
	if (a.mixer)
		mixer_route();
	log_msg("audio: headphones %s (jack switch %d, %s, DT says ACTIVE_LOW), "
		"Playback Mux %s", inserted ? "inserted" : "removed", inserted, raw,
		a.mixer ? mux_state() : "set when the mixer opens");
}

/*
 * DAPM power state of the playback path from debugfs, one line:
 * /sys/kernel/debug/asoc/<card>/dapm/<widget> for the board widgets and
 * <card>/<component>/dapm/<widget> for the codec ones. Each file starts
 * with "<widget>: On|Off ...".
 */
static void dapm_dump(const char *when)
{
	static const char *const names[] = {
		"Playback Mux", "DAC L", "DAC R", "SPK DAC", "Enable Class D",
		"HPOL", "HPOR", "SPKO", "Headphones", "Speaker", "bias_level",
	};
	char line[700], path[800], buf[256];
	int off = 0, found = 0;
	struct stat st;
	DIR *cards, *comps;
	struct dirent *c, *d;

	if (!log_debug())
		return;
	if (fe_is_init && stat("/sys/kernel/debug/asoc", &st))
		mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
	cards = opendir("/sys/kernel/debug/asoc");
	if (!cards) {
		log_msg("audio: dapm (%s): no debugfs: %s", when, strerror(errno));
		return;
	}
	line[0] = 0;
	while ((c = readdir(cards))) {
		if (c->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/kernel/debug/asoc/%s", c->d_name);
		comps = opendir(path);
		if (!comps)
			continue;
		/* "" is the card itself, then each component directory. */
		for (d = NULL;; ) {
			const char *sub = d ? d->d_name : "";

			if (!d || (sub[0] != '.' && strcmp(sub, "dapm"))) {
				for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
					char *nl, *p;
					int n;

					snprintf(path, sizeof(path),
						 "/sys/kernel/debug/asoc/%s%s%s/dapm/%s",
						 c->d_name, d ? "/" : "", sub, names[i]);
					n = read_file(path, buf, sizeof(buf) - 1);
					if (n <= 0)
						continue;
					buf[n] = 0;
					nl = strchr(buf, '\n');
					if (nl)
						*nl = 0;
					/* "Name: On  in 1 out 2" -> "Name On" */
					p = strstr(buf, ": ");
					if (p) {
						char *e;

						memmove(p, p + 1, strlen(p));
						e = strstr(p + 1, "  ");
						if (e)
							*e = 0;
					}
					found++;
					if (off < (int)sizeof(line) - 2)
						off += snprintf(line + off, sizeof(line) - off,
								"%s%s%s", off ? ", " : "",
								p ? "" : "bias ", buf);
				}
			}
			d = readdir(comps);
			if (!d)
				break;
		}
		closedir(comps);
	}
	closedir(cards);
	log_msg("audio: dapm (%s): Playback Mux = %s; %s", when, mux_state(),
		found ? line : "no widget files found");
}

/* ------------------------------------------------------------------ PCM */

static void log_params(void)
{
	struct pcm_params *p = pcm_params_get(CARD, DEVICE, PCM_OUT);

	if (!p) {
		log_msg("audio: cannot query hw params of card %d device %d",
			CARD, DEVICE);
		return;
	}
	log_msg("audio: device accepts rate %u..%u, channels %u..%u, "
		"period %u..%u, periods %u..%u, S16_LE %s, S32_LE %s",
		pcm_params_get_min(p, PCM_PARAM_RATE),
		pcm_params_get_max(p, PCM_PARAM_RATE),
		pcm_params_get_min(p, PCM_PARAM_CHANNELS),
		pcm_params_get_max(p, PCM_PARAM_CHANNELS),
		pcm_params_get_min(p, PCM_PARAM_PERIOD_SIZE),
		pcm_params_get_max(p, PCM_PARAM_PERIOD_SIZE),
		pcm_params_get_min(p, PCM_PARAM_PERIODS),
		pcm_params_get_max(p, PCM_PARAM_PERIODS),
		pcm_params_format_test(p, PCM_FORMAT_S16_LE) ? "yes" : "no",
		pcm_params_format_test(p, PCM_FORMAT_S32_LE) ? "yes" : "no");
	pcm_params_free(p);
}

static const char *cand_name(const struct cand *c)
{
	static char s[48];

	snprintf(s, sizeof(s), "%u Hz %s %ux%u", c->rate, c->s32 ? "S32" : "S16",
		 c->period, c->periods);
	return s;
}

static void close_pcm(void)
{
	if (a.pcm)
		pcm_close(a.pcm);
	a.pcm = NULL;
	a.started = 0;
}

static int open_cand(const struct cand *c)
{
	struct pcm_config cfg = {
		.channels = 2,
		.rate = c->rate,
		.period_size = c->period,
		.period_count = c->periods,
		.format = c->s32 ? PCM_FORMAT_S32_LE : PCM_FORMAT_S16_LE,
		.start_threshold = c->period * c->periods / 4,
		.avail_min = c->period,
	};
	int simple = c == &simple_cand;
	const struct pcm_config *got;
	struct pcm *pcm;

	close_pcm();
	a.cand = *c;
	a.period = c->period;
	a.buffer = c->period * c->periods;
	a.started = 0;
	a.written = 0;
	a.rate_ns = 0;
	a.rate_frames = 0;
	a.prev_ok = 0;
	if (a.null_dev) {
		a.null_fill = 0;
		a.null_running = 0;
		a.null_hz = a.rate * a.null_ratio;
		a.dev_rate = a.rate;
		a.target = a.buffer / 2;
		return 0;
	}
	if (simple)
		cfg.start_threshold = c->period * c->periods / 2;
	pcm = pcm_open(CARD, DEVICE, PCM_OUT | PCM_NORESTART |
		       (simple ? 0 : PCM_NONBLOCK), &cfg);
	if (!pcm || !pcm_is_ready(pcm)) {
		log_msg("audio: open %s failed: %s", cand_name(c),
			pcm ? pcm_get_error(pcm) : "no memory");
		if (pcm)
			pcm_close(pcm);
		return -1;
	}
	a.pcm = pcm;
	a.rate = pcm_get_rate(pcm);
	a.dev_rate = a.rate;
	a.buffer = pcm_get_buffer_size(pcm);
	a.target = a.buffer / 2;
	got = pcm_get_config(pcm);
	a.mode = simple ? AUDIO_SIMPLE : AUDIO_ADAPTIVE;
	log_msg("audio: opened %s (%s, %s writes), buffer %u frames (%.1f ms), "
		"kernel period %u x %u", cand_name(c),
		simple ? "simple" : "adaptive", simple ? "blocking" : "nonblocking",
		a.buffer, a.buffer * 1000.0 / a.rate,
		got ? got->period_size : 0, got ? got->period_count : 0);
	return 0;
}

int audio_init(void)
{
	int64_t now = now_ns();

	if (a.pcm || a.null_dev)
		return 0;
	if (now < a.next_init_ns)
		return -1;
	a.next_init_ns = now + 2000000000LL;

	if (!a.mixer)
		mixer_setup();
	if (!a.mixer)
		return -1;
	log_params();

	if (settings.audio == AUDIO_SIMPLE)
		return open_cand(&simple_cand);
	/* After calibration reopen what it chose, else the first that opens. */
	if (a.calibrated && open_cand(&a.cand) == 0)
		return 0;
	for (size_t i = 0; i < ARRAY_SIZE(cands); i++)
		if (open_cand(&cands[i]) == 0)
			return 0;
	return -1;
}

void audio_init_null(unsigned rate, double drift)
{
	memset(&a, 0, sizeof(a));
	a.null_dev = 1;
	a.rate = rate;
	a.adj = 1.0;
	a.st.fill_min = -1;
	a.null_ratio = 1.0 + drift;
	open_cand(&cands[0]);
}

int audio_ready(void)
{
	return a.pcm || a.null_dev;
}

unsigned audio_rate(void)
{
	return a.rate;
}

double audio_device_rate(void)
{
	return a.dev_rate;
}

unsigned audio_target(void)
{
	return a.target;
}

unsigned audio_buffer(void)
{
	return a.buffer;
}

/*
 * Refresh tinyalsa's cached appl_ptr from the kernel (see the top of this
 * file). Must follow every write, or the next pcm_writei()/pcm_state()
 * rewinds the kernel's appl_ptr to the stale value.
 */
static void dev_sync(void)
{
	int e = errno;

	if (a.pcm)
		pcm_avail_update(a.pcm);
	errno = e;
}

/* The stream runs (has passed its start threshold, no underrun). */
static int dev_running(void)
{
	if (a.null_dev)
		return a.null_running;
	if (!a.pcm)
		return 0;
	dev_sync();
	return pcm_state(a.pcm) == PCM_STATE_RUNNING;
}

/* Write to the device; returns frames accepted, or -1 with errno. */
static int dev_write(const int16_t *buf, unsigned frames)
{
	unsigned done = 0;

	if (a.null_dev) {
		double room = a.buffer - a.null_fill;
		unsigned n = room < frames ? (unsigned)room : frames;

		a.null_fill += n;
		a.written += n;
		if (a.null_fill >= (a.mode == AUDIO_SIMPLE ? a.buffer / 2 : a.buffer / 4))
			a.null_running = 1;
		return (int)n;
	}
	if (!a.cand.s32) {
		int n = pcm_writei(a.pcm, buf, frames);

		dev_sync();
		if (n > 0)
			a.written += (unsigned)n;
		return n;
	}
	while (done < frames) {
		unsigned chunk = frames - done;
		int n;

		if (chunk > ARRAY_SIZE(a.conv) / 2)
			chunk = ARRAY_SIZE(a.conv) / 2;
		for (unsigned i = 0; i < chunk * 2; i++)
			a.conv[i] = (int32_t)((uint32_t)(uint16_t)buf[2 * done + i] << 16);
		n = pcm_writei(a.pcm, a.conv, chunk);
		dev_sync();
		if (n < 0) {
			if (done)
				break;
			return -1;
		}
		done += (unsigned)n;
		a.written += (unsigned)n;
		if ((unsigned)n < chunk)
			break;
	}
	return (int)done;
}

static long dev_delay(void)
{
	if (a.null_dev)
		return a.null_xrun ? (errno = EPIPE, -1) : (long)a.null_fill;
	return pcm_get_delay(a.pcm);
}

/* ---------------------------------------------------------- calibration */

static int64_t cal_vt;  /* virtual time for the simulated device */

static int64_t cal_now(void)
{
	return a.null_dev ? cal_vt : now_ns();
}

static void cal_sleep(int64_t ns)
{
	if (a.null_dev) {
		cal_vt += ns;
		a.null_fill -= a.null_hz * ns / 1e9;
		if (a.null_fill < 0) {
			a.null_fill = 0;
			a.null_xrun = 1;
		}
	} else {
		struct timespec ts = { 0, (long)ns };

		nanosleep(&ts, NULL);
	}
}

/*
 * Play silence with the buffer kept nearly full and fit frames played
 * (written - delay) against time. Returns frames/s, 0 on failure.
 */
static double calibrate(int full_dump)
{
	static const int16_t silence[512 * 2];
	unsigned keep = a.buffer - a.period;
	double sx = 0, sy = 0, sxx = 0, sxy = 0, rate, dn;
	long step_min = -1, d;
	uint64_t written = 0, played, prev = 0;
	int64_t t0, t, ta, tb;
	int n = 0, dumped = 0;
	const char *fail = NULL;

	if (a.pcm && pcm_prepare(a.pcm)) {
		log_msg("audio: calibrate %s: prepare failed: %s",
			cand_name(&a.cand), pcm_get_error(a.pcm));
		return 0;
	}
	a.null_fill = 0;
	a.null_xrun = 0;
	t0 = cal_now();
	for (;;) {
		ta = cal_now();
		d = dev_delay();
		tb = cal_now();
		if (d < 0) {
			fail = errno == EPIPE ? "underrun" : strerror(errno);
			break;
		}
		played = written > (uint64_t)d ? written - (uint64_t)d : 0;
		t = (ta + tb) / 2 - t0;
		if (played > prev && (step_min < 0 || (long)(played - prev) < step_min))
			step_min = (long)(played - prev);
		prev = played;
		if (t >= CAL_WARMUP_NS) {
			double x = (t - CAL_WARMUP_NS) / 1e9, y = (double)played;

			sx += x;
			sy += y;
			sxx += x * x;
			sxy += x * y;
			n++;
		}
		if (t >= CAL_WARMUP_NS + CAL_WINDOW_NS)
			break;
		while ((unsigned long)d + 16 < keep) {
			unsigned want = keep - (unsigned)d;
			int w = dev_write(silence, want < 512 ? want : 512);

			if (w <= 0)
				break;
			written += (unsigned)w;
			d += w;
		}
		if (!dumped && t >= CAL_WARMUP_NS && !a.null_dev) {
			dumped = 1;
			hw_dump(cand_name(&a.cand));
			if (full_dump)
				kernel_dump();
		}
		cal_sleep(CAL_STEP_NS);
	}
	if (a.pcm)
		pcm_stop(a.pcm);
	a.null_fill = 0;
	a.null_running = 0;
	a.null_xrun = 0;
	a.started = 0;
	a.written = 0;
	a.rate_ns = 0;
	a.rate_frames = 0;
	dn = n * sxx - sx * sx;
	if (!fail && (n < 10 || dn <= 0))
		fail = "too few samples";
	if (fail) {
		log_msg("audio: calibrate %s: failed (%s) after %.0f ms", cand_name(&a.cand),
			fail, (cal_now() - t0) / 1e6);
		if (a.ncal < MAX_CAL) {
			a.cal[a.ncal].c = a.cand;
			a.cal[a.ncal].rate = 0;
			a.ncal++;
		}
		return 0;
	}
	rate = (n * sxy - sx * sy) / dn;
	if (a.ncal < MAX_CAL) {
		a.cal[a.ncal].c = a.cand;
		a.cal[a.ncal].rate = rate;
		a.ncal++;
	}
	if (log_debug())
		log_msg("audio: calibrate %s: device consumed %.0f frames/s = %.4fx "
		"nominal (%d samples over %.0f ms, pointer step >= %ld frames, "
		"%llu frames written)", cand_name(&a.cand), rate, rate / a.rate, n,
		CAL_WINDOW_NS / 1e6, step_min, (unsigned long long)written);
	return rate;
}

static int rate_ok(double r)
{
	return r > 0 && fabs(r / a.rate - 1) < CAL_TOL;
}

/*
 * Measure the open stream. Off by more than CAL_TOL (never seen once the
 * appl_ptr rewinds were fixed), reopen once with a bigger buffer and
 * resample for the measured rate. No other config is tried: every
 * reopen is an audible pop.
 */
static void choose_config(void)
{
	double r;

	a.calibrated = 1;
	if (!audio_ready() && open_cand(&cands[0]))
		return;
	r = calibrate(!a.hw_dumped);
	a.hw_dumped = 1;
	if (rate_ok(r)) {
		a.dev_rate = a.rate;
		log_msg("audio: using %s (measured %.0f Hz is within %.0f%%)",
			cand_name(&a.cand), r, CAL_TOL * 100);
		return;
	}
	if (r <= 0) {
		a.dev_rate = a.rate;
		log_msg("audio: calibration failed, using %s at its nominal rate",
			cand_name(&a.cand));
		return;
	}
	if (open_cand(&fallback) == 0) {
		double r2 = calibrate(0);

		if (r2 > 0)
			r = r2;
		a.dev_rate = rate_ok(r) ? a.rate : r;
		log_msg("audio: %s does not play at its nominal rate; using %s and "
			"resampling for a device rate of %.0f Hz", cand_name(&cands[0]),
			cand_name(&a.cand), a.dev_rate);
		return;
	}
	open_cand(&cands[0]);
	a.dev_rate = r;
	log_msg("audio: resampling %s for a measured device rate of %.0f Hz",
		cand_name(&a.cand), a.dev_rate);
}

/* --------------------------------------------------------------- stream */

/* Reopen the PCM if settings.audio changed since it was opened. */
static void apply_mode(void)
{
	if (a.null_dev) {
		a.mode = settings.audio;
		return;
	}
	if (!a.pcm || a.mode == settings.audio)
		return;
	log_msg("audio: switching to %s mode",
		settings.audio == AUDIO_SIMPLE ? "simple" : "adaptive");
	close_pcm();
	if (settings.audio == AUDIO_SIMPLE)
		open_cand(&simple_cand);
	else if (!(a.calibrated && open_cand(&a.cand) == 0))
		for (size_t i = 0; i < ARRAY_SIZE(cands); i++)
			if (open_cand(&cands[i]) == 0)
				break;
}

void audio_set_source(double in_rate, double core_fps, double display_hz)
{
	apply_mode();
	if (a.mode == AUDIO_ADAPTIVE && !a.calibrated && audio_ready())
		choose_config();
	if (a.mode == AUDIO_SIMPLE)
		a.dev_rate = a.rate;
	a.sess_xruns = 0;
	a.sess_fill_min = -1;
	a.sess_fill_max = 0;
	a.sess_frames = 0;
	a.sess_ns = 0;
	a.prev_ok = 0;
	a.flushes = 0;
	a.game_dumped = 0;
	if (!a.rate || !audio_ready())
		return;
	/* Input frames per output frame, with the core sped up to the panel. */
	a.base_step = in_rate / a.dev_rate * display_hz / core_fps;
	/*
	 * The fill is sampled right before each write of about one video
	 * frame of audio, so it can range from 0 to buffer - frame. Aim for
	 * the middle; the controller saturates at the edges.
	 */
	a.target = (unsigned)((a.buffer - a.dev_rate / display_hz) / 2);
	if (a.target < a.buffer / 4)
		a.target = a.buffer / 4;
	if (a.mode == AUDIO_SIMPLE)
		a.target = a.buffer / 2;  /* re-prime level after an underrun */
	a.adj = 1.0;
	a.fill_ema = a.target;
	rs_init(&a.rs, a.base_step);
	a.out_n = 0;
	a.started = 0;
	if (a.null_dev)
		a.null_consume = a.null_hz / display_hz;  /* per video frame */
	log_msg("audio: source %.0f Hz at %.4f fps, display %.4f Hz, %s mode, "
		"device %u Hz (plays %.0f), step %.6f (%.2f out frames per video "
		"frame), buffer %u, target fill %u", in_rate, core_fps, display_hz,
		a.mode == AUDIO_SIMPLE ? "simple" : "adaptive", a.rate, a.dev_rate,
		a.base_step, in_rate / core_fps / a.base_step, a.buffer, a.target);
}

void audio_push(const int16_t *samples, size_t frames)
{
	int64_t t0 = now_ns();

	if (a.base_step > 0 && frames)
		a.out_n += rs_process(&a.rs, samples, frames, a.out + 2 * a.out_n,
				      OUT_MAX - a.out_n);
	a.time_ns += now_ns() - t0;
}

/*
 * Device rate during play: frames played between two flushes over the
 * time between them, only for intervals with the stream running and no
 * underrun in between (an underrun stops the clock for an unknown time).
 */
static void session_rate_sample(long fill)
{
	int64_t now = now_ns();
	uint64_t played = a.written > (uint64_t)fill ? a.written - (uint64_t)fill : 0;
	int running = a.null_dev || dev_running();

	if (a.null_dev)
		now = (int64_t)a.flushes * 16661000LL;  /* simulated frames */
	if (running && a.prev_ok && played >= a.prev_played && now > a.prev_ns) {
		a.sess_frames += played - a.prev_played;
		a.sess_ns += now - a.prev_ns;
	}
	a.prev_played = played;
	a.prev_ns = now;
	a.prev_ok = running;
}

/* Restart after an underrun or pause: queue the target fill of silence. */
static void prime(void)
{
	static const int16_t silence[512 * 2];
	unsigned left = a.target;

	while (left) {
		unsigned n = left < 512 ? left : 512;
		int w = dev_write(silence, n);

		if (w <= 0)
			break;
		left -= (unsigned)w;
	}
	a.started = 1;
	a.fill_ema = a.target;
}

static void on_xrun(const char *what)
{
	a.st.xruns++;
	a.xruns_total++;
	a.sess_xruns++;
	a.prev_ok = 0;
	if (a.xruns_total <= 20 || a.xruns_total % 100 == 0)
		log_msg("audio: underrun #%u (%s)", a.xruns_total, what);
	if (a.pcm)
		pcm_prepare(a.pcm);
	a.started = 0;
}

void audio_flush(void)
{
	int64_t t0 = now_ns();
	long fill;
	double err;
	int n;

	if (!audio_ready() || a.base_step <= 0) {
		a.out_n = 0;
		return;
	}
	if (a.null_dev && a.null_running) {
		/* The simulated codec plays one video frame worth of audio. */
		a.null_fill -= a.null_consume;
		if (a.null_fill < 0) {
			a.null_fill = 0;
			a.null_running = 0;
			if (a.started)
				on_xrun("simulated");
		}
	}
	if (!a.started) {
		/* Simple mode starts on its own at half a buffer of game audio
		 * (pid351: priming on open was its boot pop). */
		if (a.mode == AUDIO_SIMPLE) {
			/* After a pause the stream is in SETUP, where the delay
			 * cannot be read (that would count as an underrun). */
			if (a.pcm)
				pcm_prepare(a.pcm);
			a.started = 1;
		} else {
			prime();
		}
	}

	fill = dev_delay();
	if (fill < 0) {
		on_xrun(errno == EPIPE ? "delay" : strerror(errno));
		prime();
		fill = dev_delay();
		if (fill < 0)
			fill = a.target;
	}
	if (a.st.fill_min < 0 || (int)fill < a.st.fill_min)
		a.st.fill_min = (int)fill;
	if ((int)fill > a.st.fill_max)
		a.st.fill_max = (int)fill;
	if (a.sess_fill_min < 0 || (int)fill < a.sess_fill_min)
		a.sess_fill_min = (int)fill;
	if ((int)fill > a.sess_fill_max)
		a.sess_fill_max = (int)fill;
	session_rate_sample(fill);
	if (++a.flushes == 180 && !a.game_dumped && a.pcm && dev_running()) {
		/* 3 s into the game, with the stream running for real. */
		a.game_dumped = 1;
		hw_dump("in game");
		if (!a.hw_dumped)
			kernel_dump();
		a.hw_dumped = 1;
	}

	if (a.mode == AUDIO_SIMPLE)
		goto write;
	/* Proportional control on the smoothed fill level. */
	a.fill_ema += FILL_EMA * (fill - a.fill_ema);
	err = ((double)a.target - a.fill_ema) / a.target;
	if (err > 1)
		err = 1;
	if (err < -1)
		err = -1;
	a.adj = 1.0 + DRC_MAX * err;
	rs_set_ratio(&a.rs, a.base_step / a.adj);

write:
	if (a.out_n) {
		n = dev_write(a.out, (unsigned)a.out_n);
		if (n < 0 && errno == EPIPE) {
			on_xrun("write");
			prime();
			n = dev_write(a.out, (unsigned)a.out_n);
		}
		if (n < 0)
			n = 0;
		if ((size_t)n < a.out_n)
			a.st.overflows += (unsigned)(a.out_n - n);
	}
	a.out_n = 0;
	a.time_ns += now_ns() - t0;
}

void audio_pause(void)
{
	if (a.pcm && a.started) {
		long d = pcm_get_delay(a.pcm);

		/* Dropped, never played: keep the rate measurement honest. */
		if (d > 0 && a.written >= (uint64_t)d)
			a.written -= (uint64_t)d;
		pcm_stop(a.pcm);
	}
	a.null_fill = 0;
	a.null_running = 0;
	a.started = 0;
	a.out_n = 0;
	a.prev_ok = 0;
}

static const char *mode_name(void);

void audio_suspend(void)
{
	int was_open = a.pcm != NULL;

	audio_pause();
	close_pcm();
	a.suspended = 1;
	log_msg("audio: suspended (%s)", was_open ? "PCM closed" : "no PCM open");
}

void audio_resume(void)
{
	double dev_rate = a.dev_rate;
	unsigned target = a.target;
	int64_t t0 = now_ns();

	if (!a.suspended)
		return;
	a.suspended = 0;
	a.next_init_ns = 0;
	if (audio_init()) {
		log_msg("audio: resume: no PCM, retrying in the background");
		return;
	}
	/* Keep what the game's audio_set_source chose; open_cand resets it. */
	if (a.base_step > 0 && a.mode == settings.audio) {
		a.dev_rate = dev_rate;
		a.target = target;
	}
	a.adj = 1.0;
	a.fill_ema = a.target;
	if (a.base_step > 0)
		rs_init(&a.rs, a.base_step);
	a.out_n = 0;
	a.started = 0;
	a.prev_ok = 0;
	a.rate_ns = 0;
	a.rate_frames = 0;
	log_msg("audio: resumed, %s %s mode reopened in %.1f ms, rate control "
		"reset", cand_name(&a.cand), mode_name(), (now_ns() - t0) / 1e6);
}

/*
 * Frames the codec actually played since the last call, per second. On an
 * underrun the queue drains completely, so the count stays right across
 * xruns as long as the delay can be read.
 */
static double measure_rate(void)
{
	int64_t now = now_ns();
	long fill = a.pcm && a.started ? pcm_get_delay(a.pcm) : -1;
	uint64_t played;
	double r = 0;

	if (fill < 0)
		fill = 0;
	played = a.written > (uint64_t)fill ? a.written - (uint64_t)fill : 0;
	if (a.rate_ns && now > a.rate_ns && played >= a.rate_frames)
		r = (played - a.rate_frames) * 1e9 / (double)(now - a.rate_ns);
	a.rate_frames = played;
	a.rate_ns = now;
	return r;
}

void audio_take_stats(struct audio_stats *st)
{
	*st = a.st;
	st->adj = a.adj;
	st->hw_rate = a.pcm ? measure_rate() : 0;
	memset(&a.st, 0, sizeof(a.st));
	a.st.fill_min = -1;
}

unsigned audio_xruns_total(void)
{
	return a.xruns_total;
}

int64_t audio_take_time_ns(void)
{
	int64_t t = a.time_ns;

	a.time_ns = 0;
	return t;
}

/* ------------------------------------------------------------- summary */

static const char *mode_name(void)
{
	return a.mode == AUDIO_SIMPLE ? "simple" : "adaptive";
}

double audio_session_rate(void)
{
	return a.sess_ns > 0 ? a.sess_frames * 1e9 / a.sess_ns : 0;
}

void audio_summary(const char *when)
{
	char cal[400] = "";
	int off = 0;

	for (int i = 0; i < a.ncal && off < (int)sizeof(cal) - 60; i++)
		off += snprintf(cal + off, sizeof(cal) - off, "%s%s = %.0f Hz",
				i ? ", " : "", cand_name(&a.cal[i].c), a.cal[i].rate);
	log_msg("audio summary (%s): %s mode, %s%s, device plays %.0f Hz "
		"(assumed), measured in play %.0f Hz over %.1f s, underruns %u "
		"this game / %u since boot, fill %d..%d of buffer %u",
		when, mode_name(), audio_ready() ? cand_name(&a.cand) : "no PCM",
		a.null_dev ? " (simulated)" : "", a.dev_rate, audio_session_rate(),
		a.sess_ns / 1e9, a.sess_xruns, a.xruns_total, a.sess_fill_min,
		a.sess_fill_max, a.buffer);
	log_msg("audio summary (%s): calibration %s; registers: MCLK %.0f Hz, "
		"codec MCLK %.0f Hz, LRCK %.0f Hz%s", when,
		a.ncal ? cal : a.mode == AUDIO_SIMPLE ? "not used (simple mode)" :
		"not run", a.hw_mclk, a.hw_codec_mclk, a.hw_lrck,
		a.hw_lrck > 0 ? "" : " (not read)");
	if (!a.null_dev) {
		log_msg("audio summary (%s): stream %s, headphones %s", when,
			dev_running() ? "running" : "not running",
			a.jack < 0 ? "not reported" : a.jack ? "in" : "out");
		dapm_dump(when);
	}
}

int audio_csv_fields(char *buf, size_t size)
{
	char cfg[48];
	int n;

	snprintf(cfg, sizeof(cfg), "%s", audio_ready() ? cand_name(&a.cand) : "");
	n = snprintf(buf, size, "%s,%s,%.0f,%.0f,%d,%d,%.0f", mode_name(), cfg,
		     a.dev_rate, audio_session_rate(), a.sess_fill_min,
		     a.sess_fill_max, a.hw_lrck);
	return n < (int)size ? n : (int)size - 1;
}

/* ---------------------------------------------------------- test tone */

/* Sine sample at frame i of a tone of len frames with 20 ms fades. */
int16_t audio_tone_sample(unsigned i, unsigned len, unsigned rate, double hz)
{
	double fade = rate * 0.02, g = 1.0;

	if (i < fade)
		g = i / fade;
	else if (len - i < fade)
		g = (len - i) / fade;
	return (int16_t)(sin(2 * M_PI * hz * i / rate) * 0.3 * 32767 * g);
}

/*
 * 440 Hz at the nominal rate through the open PCM, the same config and
 * write mode as in game, so pitch (an octave up means the device plays
 * twice the rate) and crackle can be judged without a game. Blocks for
 * about the tone's length.
 */
void audio_test_tone(double seconds)
{
	int16_t buf[512 * 2];
	unsigned len, done = 0, xr0 = a.xruns_total;
	int64_t t0, t_start = 0;
	int dumped = 0;
	uint64_t w0;
	long d;

	audio_init();
	apply_mode();
	if (!a.pcm) {
		log_msg("audio: test tone: no PCM");
		return;
	}
	audio_pause();
	pcm_prepare(a.pcm);
	len = (unsigned)(seconds * a.rate);
	w0 = a.written;
	t0 = now_ns();
	a.prev_ok = 0;
	a.sess_frames = 0;
	a.sess_ns = 0;
	while (done < len) {
		unsigned n = len - done < 512 ? len - done : 512;
		int w;

		for (unsigned i = 0; i < n; i++)
			buf[2 * i] = buf[2 * i + 1] =
				audio_tone_sample(done + i, len, a.rate, 440);
		w = dev_write(buf, n);
		if (w < 0 && errno == EPIPE) {
			on_xrun("tone");
			pcm_prepare(a.pcm);
			continue;
		}
		if (w < 0 && errno != EAGAIN) {
			log_msg("audio: test tone: write: %s", pcm_get_error(a.pcm));
			break;
		}
		if (w > 0)
			done += (unsigned)w;
		d = pcm_get_delay(a.pcm);
		if (d >= 0) {
			if (!t_start && dev_running())
				t_start = now_ns();
			session_rate_sample(d);
			/* Half way through: what the hardware and DAPM do. */
			if (t_start && !dumped && done >= len / 2) {
				dumped = 1;
				hw_dump("test tone");
				dapm_dump("test tone, playing");
			}
		}
		if (w <= 0 || (unsigned)w < n) {
			struct timespec ts = { 0, 2000000 };

			nanosleep(&ts, NULL);
		}
	}
	/* Let it drain, then stop. */
	for (int i = 0; i < 200; i++) {
		struct timespec ts = { 0, 5000000 };

		d = pcm_get_delay(a.pcm);
		if (d < 0 || d < (long)a.period)
			break;
		session_rate_sample(d);
		nanosleep(&ts, NULL);
	}
	if (!dumped)
		dapm_dump(t_start ? "test tone, end" : "test tone, never started");
	pcm_stop(a.pcm);
	a.started = 0;
	log_msg("audio: test tone 440 Hz %.1f s, %s %s mode: %u underruns, "
		"measured %.0f Hz, %llu frames written in %.2f s wall (%.2f s "
		"after start)", seconds, cand_name(&a.cand), mode_name(),
		a.xruns_total - xr0, audio_session_rate(),
		(unsigned long long)(a.written - w0), (now_ns() - t0) / 1e9,
		t_start ? (now_ns() - t_start) / 1e9 : 0);
	audio_summary("test tone");
}
