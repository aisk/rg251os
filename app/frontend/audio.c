/*
 * Audio: RK817 codec through tinyalsa, card 0 device 0, always 48 kHz
 * S16 stereo with 8 periods of 512 frames (85 ms).
 *
 * tinyalsa on arm64. The kernel only lets x86/ppc/alpha mmap the PCM
 * status and control records, so tinyalsa falls back to the SYNC_PTR
 * ioctl with a cached copy of appl_ptr. pcm_state() (which pcm_writei()
 * itself calls before every write) issues SYNC_PTR without
 * SNDRV_PCM_SYNC_PTR_APPL, which makes the kernel take the cached
 * appl_ptr as the new one. Plain read/write I/O never refreshes that
 * cache, so every write would rewind appl_ptr to where it was at the
 * last prepare: the queue never grows, and behind a running hw_ptr the
 * rewinds show up as underruns and as the device "consuming twice the
 * rate". So after every write the cache is refreshed with
 * pcm_avail_update(), which syncs with SYNC_PTR_APPL (dev_sync()).
 *
 * Mixer. The mainline rk817 codec has two playback controls: "Master
 * Playback Volume" (0..255, 255 = 0 dB, 0.375 dB/step) and "Playback Mux"
 * (HP/SPK), a virtual enum without a register. With the mux at HP both
 * the headphones and the speaker play (as on ROCKNIX), and simple-card's
 * jack detection only gates the "Headphones" pin, so HP is set for both
 * jack states. The volume keys and the menus step settings.volume by
 * 3 dB down to -60 dB, below which it mutes (audio_volume_step). The mux
 * is reapplied on every jack event, and
 * <data>/mixer.txt can set any control for both states ("Name=value") or
 * for one ("speaker: Name=value", "headphones: Name=value").
 *
 * Modes (settings.audio):
 *   adaptive  the default: nonblocking writes, the stream primed with
 *             silence to a target fill, and dynamic rate control
 *   simple    blocking writes, the stream starting by itself at half a
 *             buffer, a fixed resampling ratio and half a buffer of
 *             silence after an underrun (what pid351 runs on this board)
 *
 * Timing. The display is the master clock: one retro_run per vblank
 * (60.02 Hz) although the GBA runs at 59.73 Hz. The resampling ratio is
 * corrected for that difference up front; in adaptive mode the rate
 * control adjusts it by up to +-0.5% to keep the ALSA buffer at its
 * target fill, which absorbs the drift between the panel and codec clocks.
 *
 * Sleep (main.c): audio_suspend() drops the stream and closes the PCM,
 * so nothing runs when the kernel suspends the sound card and the
 * hw_params are applied again on wake. audio_resume() reopens it and
 * restarts the rate control, so the gap is not mistaken for drift, and
 * holds the stream: the first frame after a wake takes hundreds of ms to
 * reach the screen (the panel coming back), and a stream started by
 * that frame's audio ran dry before the next one (an underrun right
 * after the wake). While held, audio_flush drops the game's audio and
 * leaves the PCM prepared but not started; once main.c sees the first
 * frame on screen, audio_wake_shown() lets the next flush start the
 * stream the way a game's first frame does (prime, or simple mode's
 * start threshold). A hold that lasts HOLD_MAX_FLUSHES frames is let go
 * by itself.
 *
 * Statistics: at game end audio_summary() logs the mode, the rate the
 * device was measured to play at between writes, underruns and the fill
 * range, and perf.csv gets the same as columns. With debug logging the
 * mixer controls, the ALSA hw/sw params and status (once per boot, 3 s
 * into a game) are logged and ALSA's xrun_debug reports to dmesg.
 *
 * Buffer status for a core's frame skipping (audio_buffer_status, the
 * libretro audio buffer status callback, see core.c): the frames queued
 * in the device right before retro_run. That is about the lowest point
 * of the cycle (the device has drained through the vblank wait, this
 * frame's audio is written after the run), so it is what an underrun
 * would come from. It is given in percent of twice the target fill, so
 * the fill the rate control aims for reads 50%, as with RetroArch's rate
 * control, which the cores' thresholds are made for (gpSP skips under
 * 33%: two thirds of the target, about 23 ms here). In percent of the
 * whole buffer the target is 40% in adaptive mode, and a little drift
 * puts the fill under 33% for good (0.1% drift does in the headless
 * test). Less than one video frame of audio left means the device runs
 * dry before the next write: underrun likely.
 *
 * A simulated device (audio_init_null) consumes a configurable rate per
 * video frame, for the selftest and headless runs; audio_null_idle plays
 * it for frames without a write, like the wait for the first flip after
 * a wake.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyalsa/asoundlib.h>

/* In libtinyalsa but missing from its public header. */
int pcm_state(struct pcm *pcm);
int pcm_avail_update(struct pcm *pcm);

#include "audio.h"
#include "fe.h"
#include "resample.h"
#include "settings.h"

#define CARD 0
#define DEVICE 0
#define RATE 48000
#define PERIOD 512
#define PERIODS 8
#define DRC_MAX 0.005
#define FILL_EMA 0.05
#define OUT_MAX 4096        /* frames per video frame, ~5x what is needed */
#define SILENCE_CHUNK 512
#define NULL_FRAME_NS 16661000LL  /* simulated vblank, for the rate */
#define KERNEL_DUMP_FLUSHES 180   /* 3 s into a game */
#define HOLD_MAX_FLUSHES 60       /* 1 s of frames without the wake shown */

static struct {
	struct pcm *pcm;
	struct mixer *mixer;
	int mode;           /* AUDIO_SIMPLE / AUDIO_ADAPTIVE of the open PCM */
	unsigned rate, buffer, period, target;
	int started;        /* primed (adaptive) or prepared (simple) */
	int suspended;      /* audio_suspend() closed the PCM */
	int held;           /* resumed, waiting for the first frame shown */
	unsigned held_flushes;
	int64_t held_ns;    /* when the hold started */
	int64_t next_init_ns;

	/* Simulated device: plays null_consume frames per flush. */
	int null_dev;
	double null_ratio;  /* simulated speed / nominal */
	double null_fill, null_consume;
	int null_running;   /* start threshold reached */
	int null_drained;   /* ran dry in audio_null_idle */

	int jack;           /* headphones: 1 in, 0 out, -1 not reported yet */
	char mixer_text[2048];  /* <data>/mixer.txt, read once */
	int mixer_text_len; /* -1: no file */
	int mixer_dumped, kernel_dumped;

	double base_step, adj, fill_ema;
	double frame_out;   /* device frames per video frame */
	struct resampler rs;
	int16_t out[OUT_MAX * 2];
	size_t out_n;
	int64_t time_ns;

	/* Frames written since the PCM was opened; written - delay is what
	 * the device played. */
	uint64_t written;
	/* Device rate between flushes (intervals with the stream running
	 * and no underrun only), this game and since the last stats call. */
	uint64_t prev_played;
	int64_t prev_ns;
	int prev_ok;
	unsigned flushes;
	uint64_t sess_frames, iv_frames;
	int64_t sess_ns, iv_ns;
	unsigned sess_xruns;
	int sess_fill_min, sess_fill_max;

	struct audio_stats st;
	unsigned xruns_total;
} a = { .null_ratio = 1.0, .adj = 1.0, .st = { .fill_min = -1 }, .jack = -1,
	.mixer_text_len = -1 };

static const char *mode_name(int mode)
{
	return mode == AUDIO_SIMPLE ? "simple" : "adaptive";
}

static const char *config_name(void)
{
	static char s[48];

	snprintf(s, sizeof(s), "%u Hz S16 %ux%u", a.rate, a.period,
		 a.period ? a.buffer / a.period : 0);
	return s;
}

/* ---------------------------------------------------------- diagnostics */

static void log_file(const char *prefix, const char *path)
{
	static char buf[16384];
	int n = read_file(path, buf, sizeof(buf) - 1);
	char *line, *save;

	if (n < 0) {
		log_verbose("%s%s: %s", prefix, path, strerror(errno));
		return;
	}
	buf[n] = 0;
	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save))
		log_verbose("%s%s", prefix, line);
}

/* ALSA's view of the running stream, once per boot (debug logging). */
static void kernel_dump(void)
{
	log_file("audio: hw_params: ", "/proc/asound/card0/pcm0p/sub0/hw_params");
	log_file("audio: sw_params: ", "/proc/asound/card0/pcm0p/sub0/sw_params");
	log_file("audio: status: ", "/proc/asound/card0/pcm0p/sub0/status");
}

/* Ask ALSA to report XRUNs and hw_ptr jumps in dmesg (SND_PCM_XRUN_DEBUG). */
static void xrun_debug_enable(void)
{
	if (write_sysfs("/proc/asound/card0/pcm0p/xrun_debug", "1\n") == 0)
		log_verbose("audio: ALSA xrun_debug enabled");
}

static void log_params(void)
{
	struct pcm_params *p = pcm_params_get(CARD, DEVICE, PCM_OUT);

	if (!p) {
		log_msg("audio: cannot query hw params of card %d device %d",
			CARD, DEVICE);
		return;
	}
	log_msg("audio: device accepts rate %u..%u, channels %u..%u, "
		"period %u..%u, periods %u..%u, S16_LE %s",
		pcm_params_get_min(p, PCM_PARAM_RATE),
		pcm_params_get_max(p, PCM_PARAM_RATE),
		pcm_params_get_min(p, PCM_PARAM_CHANNELS),
		pcm_params_get_max(p, PCM_PARAM_CHANNELS),
		pcm_params_get_min(p, PCM_PARAM_PERIOD_SIZE),
		pcm_params_get_max(p, PCM_PARAM_PERIOD_SIZE),
		pcm_params_get_min(p, PCM_PARAM_PERIODS),
		pcm_params_get_max(p, PCM_PARAM_PERIODS),
		pcm_params_format_test(p, PCM_FORMAT_S16_LE) ? "yes" : "no");
	pcm_params_free(p);
}

/* ---------------------------------------------------------------- mixer */

static void mixer_dump(const char *when)
{
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
static void apply_mixer_lines(const char *text, int jack)
{
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
			mixer_set(name, eq + 1);
		}
		if (!eol)
			break;
		text = eol + 1;
	}
}

/* Route for the current jack state, then the matching mixer.txt lines. */
static void mixer_route(void)
{
	mixer_set("Playback Mux", "HP");  /* see the top of this file */
	if (a.mixer_text_len >= 0)
		apply_mixer_lines(a.mixer_text, a.jack > 0);
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
	char path[300];
	int n;

	a.mixer = mixer_open(CARD);
	if (!a.mixer) {
		log_msg("mixer: cannot open card %d: %s", CARD, strerror(errno));
		return;
	}
	if (log_debug())
		mixer_dump("as found");
	audio_set_volume(settings.volume);

	snprintf(path, sizeof(path), "%s/mixer.txt", fe_data_dir);
	n = read_file(path, a.mixer_text, sizeof(a.mixer_text) - 1);
	a.mixer_text_len = n;
	if (n >= 0) {
		a.mixer_text[n] = 0;
		log_msg("mixer: applying %s", path);
		apply_mixer_lines(a.mixer_text, -1);
	}
	mixer_route();
	if (log_debug()) {
		mixer_dump("after setup");
		xrun_debug_enable();
	}
}

void audio_set_volume(int raw)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", raw);
	mixer_set("Master Playback Volume", buf);
}

void audio_volume_step(int d)
{
	/* 8 steps of 0.375 dB = 3 dB. */
	const int floor = 255 - 8 * 20;  /* -60 dB, below is mute */
	int v = settings.volume;

	if (d > 0)
		v = v < floor ? floor : v + 8 > 255 ? 255 : v + 8;
	else
		v = v - 8 < floor ? 0 : v - 8;
	settings.volume = v;
	audio_set_volume(v);
	log_verbose("volume %d (%+d)", v, d);
}

/* The board DTS sets the detect polarity (GPIO2_PC6, low when a plug is
 * in, unlike the mainline RG351M dtsi). */
void audio_jack(int inserted)
{
	a.jack = inserted ? 1 : 0;
	if (a.mixer)
		mixer_route();
	log_msg("audio: headphones %s, Playback Mux %s",
		inserted ? "inserted" : "removed",
		a.mixer ? mux_state() : "set when the mixer opens");
}

/* --------------------------------------------------------------- device */

static int dev_open(int mode)
{
	struct pcm_config cfg = {
		.channels = 2,
		.rate = RATE,
		.period_size = PERIOD,
		.period_count = PERIODS,
		.format = PCM_FORMAT_S16_LE,
		.start_threshold = PERIOD * PERIODS /
			(mode == AUDIO_SIMPLE ? 2 : 4),
		.avail_min = PERIOD,
	};
	const struct pcm_config *got;
	struct pcm *pcm;

	a.mode = mode;
	a.rate = RATE;
	a.period = PERIOD;
	a.buffer = PERIOD * PERIODS;
	a.target = a.buffer / 2;
	a.started = 0;
	a.written = 0;
	a.prev_ok = 0;
	if (a.null_dev) {
		a.null_fill = 0;
		a.null_running = 0;
		return 0;
	}
	pcm = pcm_open(CARD, DEVICE, PCM_OUT | PCM_NORESTART |
		       (mode == AUDIO_SIMPLE ? 0 : PCM_NONBLOCK), &cfg);
	if (!pcm || !pcm_is_ready(pcm)) {
		log_msg("audio: open %s failed: %s", config_name(),
			pcm ? pcm_get_error(pcm) : "no memory");
		if (pcm)
			pcm_close(pcm);
		return -1;
	}
	a.pcm = pcm;
	a.rate = pcm_get_rate(pcm);
	a.buffer = pcm_get_buffer_size(pcm);
	a.target = a.buffer / 2;
	got = pcm_get_config(pcm);
	log_msg("audio: opened %s (%s, %s writes), buffer %u frames (%.1f ms), "
		"kernel period %u x %u", config_name(), mode_name(mode),
		mode == AUDIO_SIMPLE ? "blocking" : "nonblocking", a.buffer,
		a.buffer * 1000.0 / a.rate, got ? got->period_size : 0,
		got ? got->period_count : 0);
	return 0;
}

static void dev_close(void)
{
	if (a.pcm)
		pcm_close(a.pcm);
	a.pcm = NULL;
	a.started = 0;
}

static int dev_ready(void)
{
	return a.pcm || a.null_dev;
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
	int n;

	if (a.null_dev) {
		double room = a.buffer - a.null_fill;

		n = room < frames ? (int)room : (int)frames;
		a.null_fill += n;
		a.written += n;
		if (a.null_fill >= (a.mode == AUDIO_SIMPLE ? a.buffer / 2 :
				    a.buffer / 4))
			a.null_running = 1;
		return n;
	}
	n = pcm_writei(a.pcm, buf, frames);
	dev_sync();
	if (n > 0)
		a.written += (unsigned)n;
	return n;
}

/* Frames queued, or -1 with errno (EPIPE after an underrun). */
static long dev_delay(void)
{
	if (a.null_dev)
		return (long)a.null_fill;
	return pcm_get_delay(a.pcm);
}

static void dev_prepare(void)
{
	if (a.pcm)
		pcm_prepare(a.pcm);
}

static void dev_stop(void)
{
	if (a.pcm)
		pcm_stop(a.pcm);
	a.null_fill = 0;
	a.null_running = 0;
}

/* The simulated device plays one video frame of audio per flush. */
static int null_tick(void)
{
	if (!a.null_dev || !a.null_running)
		return 0;
	a.null_fill -= a.null_consume;
	if (a.null_fill >= 0)
		return 0;
	a.null_fill = 0;
	a.null_running = 0;
	return 1;
}

int audio_init(void)
{
	int64_t now = now_ns();

	if (dev_ready())
		return 0;
	if (now < a.next_init_ns)
		return -1;
	a.next_init_ns = now + 2000000000LL;

	if (!a.mixer)
		mixer_setup();
	if (!a.mixer)
		return -1;
	log_params();
	return dev_open(settings.audio);
}

void audio_init_null(double drift)
{
	memset(&a, 0, sizeof(a));
	a.null_dev = 1;
	a.null_ratio = 1.0 + drift;
	a.adj = 1.0;
	a.st.fill_min = -1;
	a.jack = -1;
	a.mixer_text_len = -1;
	dev_open(settings.audio);
}

void audio_null_idle(unsigned frames)
{
	while (a.null_dev && frames--)
		if (null_tick() && a.started)
			a.null_drained = 1;
}

unsigned audio_rate(void)
{
	return a.rate;
}

unsigned audio_target(void)
{
	return a.target;
}

unsigned audio_buffer(void)
{
	return a.buffer;
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
	log_msg("audio: switching to %s mode", mode_name(settings.audio));
	dev_close();
	dev_open(settings.audio);
}

void audio_set_source(double in_rate, double core_fps, double display_hz)
{
	apply_mode();
	a.sess_xruns = 0;
	a.sess_fill_min = -1;
	a.sess_fill_max = 0;
	a.sess_frames = a.iv_frames = 0;
	a.sess_ns = a.iv_ns = 0;
	a.prev_ok = 0;
	a.flushes = 0;
	if (!a.rate || !dev_ready())
		return;
	/* Input frames per output frame, with the core sped up to the panel. */
	a.base_step = in_rate / a.rate * display_hz / core_fps;
	/*
	 * The fill is sampled right before each write of about one video
	 * frame of audio, so it can range from 0 to buffer - frame. Aim for
	 * the middle; the controller saturates at the edges. Simple mode
	 * re-primes to half a buffer after an underrun.
	 */
	a.target = (unsigned)((a.buffer - a.rate / display_hz) / 2);
	if (a.target < a.buffer / 4)
		a.target = a.buffer / 4;
	if (a.mode == AUDIO_SIMPLE)
		a.target = a.buffer / 2;
	a.adj = 1.0;
	a.fill_ema = a.target;
	rs_init(&a.rs, a.base_step);
	a.out_n = 0;
	a.started = 0;
	a.frame_out = a.rate / display_hz;
	if (a.null_dev)
		a.null_consume = a.rate * a.null_ratio / display_hz;
	log_msg("audio: source %.0f Hz at %.4f fps, display %.4f Hz, %s mode, "
		"device %u Hz, step %.6f (%.2f out frames per video frame), "
		"buffer %u, target fill %u", in_rate, core_fps, display_hz,
		mode_name(a.mode), a.rate, a.base_step,
		in_rate / core_fps / a.base_step, a.buffer, a.target);
}

void audio_push(const int16_t *samples, size_t frames)
{
	int64_t t0 = now_ns();

	if (a.base_step > 0 && frames)
		a.out_n += rs_process(&a.rs, samples, frames, a.out + 2 * a.out_n,
				      OUT_MAX - a.out_n);
	a.time_ns += now_ns() - t0;
}

int audio_buffer_status(unsigned *occupancy, int *underrun_likely)
{
	long fill;

	*occupancy = 0;
	*underrun_likely = 0;
	if (!dev_ready() || a.base_step <= 0 || !a.started || !a.target)
		return 0;
	if (a.null_dev) {
		/* Simulated time stands still between flushes: take off what
		 * the device plays until the next write, like the vblank wait
		 * does on the device. */
		fill = (long)(a.null_running ? a.null_fill - a.null_consume :
			      a.null_fill);
	} else {
		fill = dev_delay();
	}
	if (fill < 0)
		fill = 0;  /* an underrun (EPIPE), or drained in the simulation */
	*occupancy = (unsigned)(fill >= 2 * (long)a.target ? 100 :
				fill * 50 / a.target);
	*underrun_likely = fill < a.frame_out;
	return 1;
}

/*
 * Device rate during play: frames played between two flushes over the
 * time between them, only for intervals with the stream running and no
 * underrun in between (an underrun stops the clock for an unknown time).
 */
static void rate_sample(long fill)
{
	int64_t now = a.null_dev ? (int64_t)a.flushes * NULL_FRAME_NS : now_ns();
	uint64_t played = a.written > (uint64_t)fill ? a.written - (uint64_t)fill : 0;
	int running = a.null_dev || dev_running();

	if (running && a.prev_ok && played >= a.prev_played && now > a.prev_ns) {
		a.sess_frames += played - a.prev_played;
		a.sess_ns += now - a.prev_ns;
		a.iv_frames += played - a.prev_played;
		a.iv_ns += now - a.prev_ns;
	}
	a.prev_played = played;
	a.prev_ns = now;
	a.prev_ok = running;
}

/* Restart after an underrun or pause: queue the target fill of silence. */
static void prime(void)
{
	static const int16_t silence[SILENCE_CHUNK * 2];
	unsigned left = a.target;

	while (left) {
		int w = dev_write(silence, left < SILENCE_CHUNK ? left :
				  SILENCE_CHUNK);

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
	dev_prepare();
	a.started = 0;
}

static void fill_stats(int fill)
{
	if (a.st.fill_min < 0 || fill < a.st.fill_min)
		a.st.fill_min = fill;
	if (fill > a.st.fill_max)
		a.st.fill_max = fill;
	if (a.sess_fill_min < 0 || fill < a.sess_fill_min)
		a.sess_fill_min = fill;
	if (fill > a.sess_fill_max)
		a.sess_fill_max = fill;
}

void audio_flush(void)
{
	int64_t t0 = now_ns();
	long fill;
	int n;

	if (!dev_ready() || a.base_step <= 0) {
		a.out_n = 0;
		return;
	}
	if (a.held) {
		a.out_n = 0;
		if (++a.held_flushes < HOLD_MAX_FLUSHES)
			return;
		log_msg("audio: no frame on screen after %u frames since the "
			"resume, starting the stream anyway", a.held_flushes);
		a.held = 0;
		return;
	}
	if ((null_tick() | a.null_drained) && a.started)
		on_xrun("simulated");
	a.null_drained = 0;
	if (!a.started) {
		/* Simple mode starts on its own at half a buffer of game audio
		 * (pid351: priming on open was its boot pop). After a pause
		 * the stream is in SETUP, where the delay cannot be read
		 * (that would count as an underrun). */
		if (a.mode == AUDIO_SIMPLE) {
			dev_prepare();
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
	fill_stats((int)fill);
	rate_sample(fill);
	if (++a.flushes == KERNEL_DUMP_FLUSHES && !a.kernel_dumped && a.pcm &&
	    log_debug() && dev_running()) {
		a.kernel_dumped = 1;
		kernel_dump();
	}

	if (a.mode == AUDIO_ADAPTIVE) {
		/* Proportional control on the smoothed fill level. */
		double err;

		a.fill_ema += FILL_EMA * (fill - a.fill_ema);
		err = ((double)a.target - a.fill_ema) / a.target;
		if (err > 1)
			err = 1;
		if (err < -1)
			err = -1;
		a.adj = 1.0 + DRC_MAX * err;
		rs_set_ratio(&a.rs, a.base_step / a.adj);
	}

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
	if (a.started)
		dev_stop();
	a.null_fill = 0;
	a.null_running = 0;
	a.started = 0;
	a.out_n = 0;
	a.prev_ok = 0;
}

void audio_suspend(void)
{
	int was_open = a.pcm != NULL;

	audio_pause();
	dev_close();
	a.suspended = 1;
	log_msg("audio: suspended (%s)", was_open ? "PCM closed" : "no PCM open");
}

void audio_resume(void)
{
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
	/* Keep the target audio_set_source chose; dev_open resets it. */
	if (a.base_step > 0 && a.mode == settings.audio)
		a.target = target;
	a.adj = 1.0;
	a.fill_ema = a.target;
	if (a.base_step > 0)
		rs_init(&a.rs, a.base_step);
	a.out_n = 0;
	a.started = 0;
	a.prev_ok = 0;
	a.held = 1;
	a.held_flushes = 0;
	a.held_ns = now_ns();
	log_msg("audio: resumed, %s %s mode reopened in %.1f ms, rate control "
		"reset, stream held until a frame is on screen",
		config_name(), mode_name(a.mode), (now_ns() - t0) / 1e6);
}

void audio_wake_shown(void)
{
	if (!a.held)
		return;
	a.held = 0;
	log_msg("audio: frame on screen %.0f ms after the resume, stream starts "
		"with the next frame (%s); %u frame%s of game audio dropped "
		"meanwhile", (now_ns() - a.held_ns) / 1e6,
		a.mode == AUDIO_SIMPLE ? "at half a buffer" : "primed with silence",
		a.held_flushes, a.held_flushes == 1 ? "" : "s");
}

void audio_take_stats(struct audio_stats *st)
{
	*st = a.st;
	st->adj = a.adj;
	st->hw_rate = a.iv_ns > 0 ? a.iv_frames * 1e9 / a.iv_ns : 0;
	a.iv_frames = 0;
	a.iv_ns = 0;
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

double audio_session_rate(void)
{
	return a.sess_ns > 0 ? a.sess_frames * 1e9 / a.sess_ns : 0;
}

void audio_summary(const char *when)
{
	log_msg("audio summary (%s): %s mode, %s%s, measured in play %.0f Hz "
		"over %.1f s, underruns %u this game / %u since boot, fill %d..%d "
		"of buffer %u", when, mode_name(a.mode),
		dev_ready() ? config_name() : "no PCM",
		a.null_dev ? " (simulated)" : "", audio_session_rate(),
		a.sess_ns / 1e9, a.sess_xruns, a.xruns_total, a.sess_fill_min,
		a.sess_fill_max, a.buffer);
	if (!a.null_dev)
		log_msg("audio summary (%s): stream %s, headphones %s, Playback "
			"Mux %s", when, dev_running() ? "running" : "not running",
			a.jack < 0 ? "not reported" : a.jack ? "in" : "out",
			mux_state());
}

int audio_csv_fields(char *buf, size_t size)
{
	int n = snprintf(buf, size, "%s,%s,%.0f,%d,%d", mode_name(a.mode),
			 dev_ready() ? config_name() : "", audio_session_rate(),
			 a.sess_fill_min, a.sess_fill_max);

	return n < (int)size ? n : (int)size - 1;
}
