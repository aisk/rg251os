/*
 * Audio: RK817 codec through tinyalsa, card 0 device 0.
 *
 * Mixer. The mainline rk817 codec has two playback controls: "Master
 * Playback Volume" (0..255, inverted register, 255 = 0 dB, 0.375 dB/step)
 * and "Playback Mux" (HP/SPK). The mux is a virtual enum without a
 * register, so it boots as "HP". The driver notes that the speaker output
 * and the left headphone pin are internally the same, and on the RG351
 * boards the speaker hangs off the headphone side: ROCKNIX runs with the
 * mux at HP and a working speaker (pid351 project, probe of a ROCKNIX
 * RG351P, mux numid=5 = 0, Master 255). There is no speaker amp enable
 * GPIO in the RG351M DT (no aux amp, no pin switches), and simple-card's
 * hp-det-gpios jack only enables/disables the "Headphones" DAPM pin, while
 * the "Speaker" pin stays enabled, so the HP route stays powered either
 * way. So: mux = HP always, the jack event is only logged. If the speaker
 * stays silent on hardware, <data>/mixer.txt can override any control
 * without a rebuild, e.g. "Playback Mux=SPK".
 *
 * Timing. The display is the master clock: one retro_run per vblank
 * (60.02 Hz) although the GBA runs at 59.73 Hz. The resampling ratio is
 * corrected for that difference up front, and dynamic rate control
 * adjusts it by up to +-0.5% to keep the ALSA buffer at its target fill,
 * which absorbs the remaining drift between the panel and codec clocks.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#include "audio.h"
#include "fe.h"
#include "resample.h"

#define CARD 0
#define DEVICE 0
#define DRC_MAX 0.005
#define FILL_EMA 0.05
#define OUT_MAX 4096  /* frames per video frame, ~5x what is needed */

static const struct { unsigned rate, period, periods; } configs[] = {
	{ 48000, 240, 12 },  /* 60 ms buffer */
	{ 48000, 256, 12 },
	{ 48000, 512, 6 },
	{ 44100, 256, 12 },
	{ 32000, 256, 8 },
};

static struct {
	struct pcm *pcm;
	struct mixer *mixer;
	int null_dev;
	double null_fill, null_hz, null_consume;  /* simulated device */
	unsigned rate, buffer, target;
	int started;
	double base_step, adj, fill_ema;
	struct resampler rs;
	int16_t out[OUT_MAX * 2];
	size_t out_n;
	struct audio_stats st;
	unsigned xruns_total;
	int64_t time_ns;
	int64_t next_init_ns;
	int dumped;
	/* Measured consumption rate: frames written minus frames queued */
	uint64_t written;
	uint64_t rate_frames;
	int64_t rate_ns;
} a = { .adj = 1.0, .st = { .fill_min = -1 } };

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

int audio_apply_mixer_text(const char *text)
{
	int n = 0;

	while (*text) {
		const char *eol = strchr(text, '\n'), *eq;
		size_t len = eol ? (size_t)(eol - text) : strlen(text);
		char line[160];

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, text, len);
		line[len] = 0;
		while (len && (line[len - 1] == '\r' || line[len - 1] == ' '))
			line[--len] = 0;
		eq = strchr(line, '=');
		if (line[0] != '#' && eq && eq != line) {
			line[eq - line] = 0;
			if (mixer_set(line, eq + 1) == 0)
				n++;
		}
		if (!eol)
			break;
		text = eol + 1;
	}
	return n;
}

static void mixer_setup(void)
{
	char path[300], buf[2048];
	int n;

	a.mixer = mixer_open(CARD);
	if (!a.mixer) {
		log_msg("mixer: cannot open card %d: %s", CARD, strerror(errno));
		return;
	}
	if (!a.dumped)
		mixer_dump("as found");
	mixer_set("Playback Mux", "HP");
	snprintf(buf, sizeof(buf), "%d", settings.volume);
	mixer_set("Master Playback Volume", buf);

	snprintf(path, sizeof(path), "%s/mixer.txt", fe_data_dir);
	n = read_file(path, buf, sizeof(buf) - 1);
	if (n >= 0) {
		buf[n] = 0;
		log_msg("mixer: applying %s", path);
		audio_apply_mixer_text(buf);
	}
	if (!a.dumped)
		mixer_dump("after setup");
	a.dumped = 1;
}

void audio_set_volume(int raw)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", raw);
	mixer_set("Master Playback Volume", buf);
}

void audio_jack(int inserted)
{
	log_msg("audio: headphones %s (mux stays HP)",
		inserted ? "inserted" : "removed");
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

	for (size_t i = 0; i < ARRAY_SIZE(configs); i++) {
		struct pcm_config cfg = {
			.channels = 2,
			.rate = configs[i].rate,
			.period_size = configs[i].period,
			.period_count = configs[i].periods,
			.format = PCM_FORMAT_S16_LE,
			.start_threshold = configs[i].period * configs[i].periods / 4,
			.avail_min = configs[i].period,
		};
		struct pcm *pcm = pcm_open(CARD, DEVICE,
					   PCM_OUT | PCM_NONBLOCK | PCM_NORESTART,
					   &cfg);

		if (!pcm || !pcm_is_ready(pcm)) {
			log_msg("audio: open %u Hz %ux%u failed: %s", cfg.rate,
				cfg.period_size, cfg.period_count,
				pcm ? pcm_get_error(pcm) : "no memory");
			if (pcm)
				pcm_close(pcm);
			continue;
		}
		a.pcm = pcm;
		a.rate = pcm_get_rate(pcm);
		a.buffer = pcm_get_buffer_size(pcm);
		a.target = a.buffer / 2;
		a.started = 0;
		log_msg("audio: opened %u Hz, period %u x %u, buffer %u frames "
			"(%.1f ms)", a.rate, cfg.period_size, cfg.period_count,
			a.buffer, a.buffer * 1000.0 / a.rate);
		return 0;
	}
	return -1;
}

void audio_init_null(unsigned rate, double drift)
{
	memset(&a, 0, sizeof(a));
	a.null_dev = 1;
	a.rate = rate;
	a.buffer = 2880;
	a.target = a.buffer / 2;
	a.adj = 1.0;
	a.st.fill_min = -1;
	a.null_hz = rate * (1.0 + drift);
}

int audio_ready(void)
{
	return a.pcm || a.null_dev;
}

unsigned audio_rate(void)
{
	return a.rate;
}

unsigned audio_target(void)
{
	return a.target;
}

void audio_set_source(double in_rate, double core_fps, double display_hz)
{
	if (!a.rate)
		return;
	/* Input frames per output frame, with the core sped up to the panel. */
	a.base_step = in_rate / a.rate * display_hz / core_fps;
	/*
	 * The fill is sampled right before each write of about one video
	 * frame of audio, so it can range from 0 to buffer - frame. Aim for
	 * the middle; the controller saturates at the edges.
	 */
	a.target = (unsigned)((a.buffer - a.rate / display_hz) / 2);
	if (a.target < a.buffer / 4)
		a.target = a.buffer / 4;
	a.adj = 1.0;
	a.fill_ema = a.target;
	rs_init(&a.rs, a.base_step);
	a.out_n = 0;
	a.started = 0;
	if (a.null_dev)
		a.null_consume = a.null_hz / display_hz;  /* per video frame */
	log_msg("audio: source %.0f Hz at %.4f fps, display %.4f Hz, "
		"step %.6f (%.2f out frames per video frame), target fill %u",
		in_rate, core_fps, display_hz, a.base_step,
		in_rate / core_fps / a.base_step, a.target);
}

void audio_push(const int16_t *samples, size_t frames)
{
	int64_t t0 = now_ns();

	if (a.base_step > 0 && frames)
		a.out_n += rs_process(&a.rs, samples, frames, a.out + 2 * a.out_n,
				      OUT_MAX - a.out_n);
	a.time_ns += now_ns() - t0;
}

/* Write to the device; returns frames accepted, or -1 with errno. */
static int dev_write(const int16_t *buf, unsigned frames)
{
	if (a.null_dev) {
		double room = a.buffer - a.null_fill;
		unsigned n = room < frames ? (unsigned)room : frames;

		a.null_fill += n;
		return (int)n;
	}
	{
		int n = pcm_writei(a.pcm, buf, frames);

		if (n > 0)
			a.written += (unsigned)n;
		return n;
	}
}

static long dev_delay(void)
{
	if (a.null_dev)
		return (long)a.null_fill;
	return pcm_get_delay(a.pcm);
}

static void prime(void)
{
	static const int16_t silence[512 * 2];
	unsigned left = a.target;

	while (left) {
		unsigned n = left < 512 ? left : 512;

		if (dev_write(silence, n) <= 0)
			break;
		left -= n;
	}
	a.started = 1;
	a.fill_ema = a.target;
}

static void on_xrun(const char *what)
{
	a.st.xruns++;
	a.xruns_total++;
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
	if (a.null_dev) {
		/* The simulated codec plays one video frame worth of audio. */
		a.null_fill -= a.null_consume;
		if (a.null_fill < 0) {
			a.null_fill = 0;
			if (a.started)
				on_xrun("simulated");
		}
	}
	if (!a.started)
		prime();

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

	/* Proportional control on the smoothed fill level. */
	a.fill_ema += FILL_EMA * (fill - a.fill_ema);
	err = ((double)a.target - a.fill_ema) / a.target;
	if (err > 1)
		err = 1;
	if (err < -1)
		err = -1;
	a.adj = 1.0 + DRC_MAX * err;
	rs_set_ratio(&a.rs, a.base_step / a.adj);

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
	if (a.pcm && a.started)
		pcm_stop(a.pcm);
	a.null_fill = 0;
	a.started = 0;
	a.out_n = 0;
}

/*
 * Frames the codec actually played since the last call, per second. On an
 * underrun the queue drains completely, so the count stays right across
 * xruns as long as the delay can be read.
 */
static double measure_rate(void)
{
	int64_t now = now_ns();
	long fill = a.pcm ? pcm_get_delay(a.pcm) : -1;
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

int64_t audio_take_time_ns(void)
{
	int64_t t = a.time_ns;

	a.time_ns = 0;
	return t;
}
