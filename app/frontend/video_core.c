/*
 * Video player as a libretro core (CORE_VIDEO, cores.c): plays the *.mpg
 * files of the ROM list (MPEG-PS with MPEG-1 video and MP2 audio, decoded
 * by pl_mpeg, see plm.c), streamed from the card (need_fullpath, never
 * read whole). Linked into every build, the stub build too, so the tests
 * run it. Its entry points are named video_retro_* here, no renaming.
 *
 * Picture: the decoder's YUV 4:2:0 converted to RGB565 (BT.601, limited
 * range) into a buffer of at most 480x320, the logical screen, mapped by
 * the global setting video_scale (core option "video_scale", read at load
 * and after a GET_VARIABLE_UPDATE, so a change shows on the next frame,
 * paused or not): Fit keeps a picture that fits as it is (main.c scales
 * it by the largest integer that fits and centers it with black bars:
 * 480x320 at 1x, 240x160 at 2x, 480x270 with bars top and bottom) and
 * shrinks a larger one to fit, keeping its aspect; Crop scales the
 * picture to cover the whole screen and cuts off the overflow evenly on
 * both sides. Scaling is nearest neighbour; the pixel aspect ratio of the
 * stream is logged and ignored (square pixels).
 *
 * Timing: the frontend runs retro_run once per vblank (about 60.02 Hz)
 * and resamples the audio from the fps and rate declared here (FPS, the
 * stream's sample rate), so each run is exactly 1/FPS of the video: the
 * play clock advances by that, the picture whose time has come is
 * decoded and shown (a 30 fps video gets a new picture every other run,
 * the others repeat it with a NULL video_refresh, which core.c counts as
 * skipped and cpuctl.c does not judge, so the clock is set by the runs
 * that decode), and rate/FPS stereo samples (fractions carried) are
 * taken from a FIFO the MP2 decoder fills. Audio and picture both follow
 * the run count, so they cannot drift apart; the FIFO head's own time is
 * kept and compared with the play clock, and more than SYNC_SLACK apart
 * (the start, after a seek) the FIFO is cut or padded with silence until
 * they match. A mono stream comes out of pl_mpeg on both channels; a
 * file without audio plays silence at NOAUDIO_RATE.
 *
 * Controls (joypad edges; SELECT+START is still the frontend's in-game
 * menu): A pauses and resumes (after the end: plays again from the
 * start), Left/Right seek 10 s back/forward, L/R 60 s. A seek goes to
 * the intra frame at or before the target (fast, plm_seek without
 * seek_exact). While paused the picture stays and silence plays.
 *
 * End of the file: the last picture stays, paused, until A plays it again
 * from the start or the menu quits. No save RAM (memory size 0). The
 * save state is the play position (0 after the end) and whether it was
 * paused, so the frontend's resume state brings the video back where it
 * was after a quit, a power off or the battery running out asleep.
 *
 * Log lines "video: ...": at load the stream (size, frame rate, pixel
 * aspect, audio rate and channels, duration) and the mapping, every
 * mapping change (mode, scaled size, crop offsets), every seek (target,
 * where it landed, time taken), the end, and at the end and at unload
 * the decode statistics: pictures decoded, avg and max ms for decoding
 * and converting a picture, audio decode time, pictures dropped because
 * a run was late, and repeated runs.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"
#include "libretro.h"
#include "plm.h"
#include "settings.h"

#define MAX_W 480
#define MAX_H 320
#define FPS 60.0
#define NOAUDIO_RATE 44100
#define FIFO_FRAMES 8192          /* stereo frames, a few MP2 frames */
#define SYNC_SLACK 0.02           /* s between the FIFO head and the clock */
#define SEEK_SHORT 10.0
#define SEEK_LONG 60.0
#define STATE_MAGIC "vidst01"

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t batch_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t state_cb;

static struct {
	plm_t *plm;
	char name[128];          /* file name, for the log */
	int sw, sh;              /* the stream's picture */
	double fps, duration;
	int has_audio, rate, channels;
	int scale;               /* VIDEO_FIT or VIDEO_CROP, as set */
	int ow, oh;              /* the picture made, at most MAX_W x MAX_H */
	int xmap[MAX_W], ymap[MAX_H];  /* source pixel per output pixel */
	plm_frame_t *last;       /* the picture shown, valid until a decode */
	double clock;            /* play position of this run */
	double next_pic;         /* time of the next picture */
	int paused, ended;
	int dirty;               /* a picture to send this run */
	uint16_t prev_pad;
	/* Audio FIFO, interleaved stereo; head_time is its first frame's. */
	int16_t fifo[FIFO_FRAMES * 2];
	int fifo_n;
	double head_time;
	int audio_eof;
	double audio_acc;
	/* Statistics since the load. */
	unsigned pics, dropped, repeated, seeks, runs;
	int64_t pic_ns, pic_max_ns, audio_ns;
} v;

static uint16_t fb[MAX_W * MAX_H];
static int ytab[256], rv[256], gu[256], gv[256], bu[256];
static plm_frame_t *seek_frame;

/* ------------------------------------------------------------ picture */

static void tables(void)
{
	for (int i = 0; i < 256; i++) {
		ytab[i] = 298 * (i - 16) + 128;
		rv[i] = 409 * (i - 128);
		gu[i] = -100 * (i - 128);
		gv[i] = -208 * (i - 128);
		bu[i] = 516 * (i - 128);
	}
}

static inline int clamp8(int x)
{
	x >>= 8;
	return x < 0 ? 0 : x > 255 ? 255 : x;
}

/* The mapping for the stream and v.scale: output size and source maps. */
static void make_map(const char *why)
{
	int sw = v.sw, sh = v.sh, scaled_w, scaled_h, cx, cy;
	double s;

	if (v.scale == VIDEO_CROP) {
		s = fmax((double)MAX_W / sw, (double)MAX_H / sh);
	} else {
		s = fmin((double)MAX_W / sw, (double)MAX_H / sh);
		if (s > 1)
			s = 1;  /* fits: main.c scales by an integer */
	}
	scaled_w = (int)lround(sw * s);
	scaled_h = (int)lround(sh * s);
	scaled_w = scaled_w < 1 ? 1 : scaled_w;
	scaled_h = scaled_h < 1 ? 1 : scaled_h;
	v.ow = scaled_w < MAX_W ? scaled_w : MAX_W;
	v.oh = scaled_h < MAX_H ? scaled_h : MAX_H;
	cx = (scaled_w - v.ow) / 2;
	cy = (scaled_h - v.oh) / 2;
	for (int x = 0; x < v.ow; x++) {
		int sx = (int)((long)(x + cx) * sw / scaled_w);

		v.xmap[x] = sx < sw ? sx : sw - 1;
	}
	for (int y = 0; y < v.oh; y++) {
		int sy = (int)((long)(y + cy) * sh / scaled_h);

		v.ymap[y] = sy < sh ? sy : sh - 1;
	}
	log_msg("video: %s: %s, %dx%d scaled to %dx%d, crop %d,%d, picture "
		"%dx%d", why, video_scale_names[v.scale], sw, sh, scaled_w,
		scaled_h, cx, cy, v.ow, v.oh);
}

static void convert(const plm_frame_t *f)
{
	const int yw = (int)f->y.width, cw = (int)f->cb.width;

	for (int y = 0; y < v.oh; y++) {
		int sy = v.ymap[y];
		const uint8_t *yr = f->y.data + (long)sy * yw;
		const uint8_t *br = f->cb.data + (long)(sy >> 1) * cw;
		const uint8_t *rr = f->cr.data + (long)(sy >> 1) * cw;
		uint16_t *d = fb + (long)y * v.ow;

		for (int x = 0; x < v.ow; x++) {
			int sx = v.xmap[x], c = ytab[yr[sx]];
			int u = br[sx >> 1], w = rr[sx >> 1];
			int r = clamp8(c + rv[w]), g = clamp8(c + gu[u] + gv[w]);
			int b = clamp8(c + bu[u]);

			d[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
		}
	}
}

/* -------------------------------------------------------------- audio */

static void fifo_drop(int n)
{
	if (n > v.fifo_n)
		n = v.fifo_n;
	memmove(v.fifo, v.fifo + 2 * n, (size_t)(v.fifo_n - n) * 4);
	v.fifo_n -= n;
	v.head_time += (double)n / v.rate;
}

static void fifo_reset(void)
{
	v.fifo_n = 0;
	v.audio_eof = 0;
}

/* Decode MP2 frames until the FIFO holds n frames or the audio ends. */
static void fifo_fill(int n)
{
	int64_t t0 = now_ns();

	while (v.has_audio && !v.audio_eof && v.fifo_n < n &&
	       v.fifo_n + PLM_AUDIO_SAMPLES_PER_FRAME <= FIFO_FRAMES) {
		plm_samples_t *s = plm_decode_audio(v.plm);
		int16_t *d;

		if (!s) {
			v.audio_eof = 1;
			break;
		}
		if (!v.fifo_n)
			v.head_time = s->time;
		d = v.fifo + 2 * v.fifo_n;
		for (unsigned i = 0; i < 2 * s->count; i++) {
			float f = s->interleaved[i] * 32767.0f;

			d[i] = (int16_t)(f > 32767.0f ? 32767 : f < -32768.0f ?
					 -32768 : (int)f);
		}
		v.fifo_n += (int)s->count;
	}
	v.audio_ns += now_ns() - t0;
}

/* n frames for this run, from the play clock on. */
static void audio_run(int n, int play)
{
	static int16_t out[2 * 2048];
	int got = 0;

	if (n > 2048)
		n = 2048;
	while (play && v.has_audio && got < n) {
		int lead, k;

		fifo_fill(n - got + 64);
		if (!v.fifo_n)
			break;
		/* Behind the clock: cut; ahead: silence until it is due. */
		lead = (int)lround((v.head_time - (v.clock + (double)got / v.rate)) *
				   v.rate);
		if (lead < -SYNC_SLACK * v.rate) {
			fifo_drop(-lead);
			continue;
		}
		if (lead > SYNC_SLACK * v.rate) {
			k = lead < n - got ? lead : n - got;
			memset(out + 2 * got, 0, (size_t)k * 4);
			got += k;
			continue;
		}
		k = v.fifo_n < n - got ? v.fifo_n : n - got;
		memcpy(out + 2 * got, v.fifo, (size_t)k * 4);
		got += k;
		fifo_drop(k);
	}
	memset(out + 2 * got, 0, (size_t)(n - got) * 4);
	batch_cb(out, (size_t)n);
}

/* ------------------------------------------------------------ control */

static void log_stats(const char *why)
{
	log_msg("video: stats (%s): %u pictures decoded, decode+convert avg "
		"%.2f ms max %.2f ms, audio decode %.2f ms per run, %u "
		"dropped (late), %u of %u runs repeated the picture, %u seeks",
		why, v.pics, v.pics ? v.pic_ns / 1e6 / v.pics : 0.0,
		v.pic_max_ns / 1e6, v.runs ? v.audio_ns / 1e6 / v.runs : 0.0,
		v.dropped, v.repeated, v.runs, v.seeks);
}

static void on_seek_frame(plm_t *p, plm_frame_t *f, void *user)
{
	(void)p;
	(void)user;
	seek_frame = f;
}

/* To target (clamped), at the intra frame before it. */
static void seek_to(double target, const char *why)
{
	int64_t t0 = now_ns();
	int ok;

	if (target < 0)
		target = 0;
	if (v.duration > 0 && target > v.duration)
		target = v.duration;
	seek_frame = NULL;
	plm_set_video_decode_callback(v.plm, on_seek_frame, NULL);
	ok = plm_seek(v.plm, target, 0);
	plm_set_video_decode_callback(v.plm, NULL, NULL);
	v.seeks++;
	if (!ok || !seek_frame) {
		log_msg("video: seek (%s) from %.2f s to %.2f s failed, back to "
			"the start", why, v.clock, target);
		/* The decoder lost its place: back to the start. */
		plm_rewind(v.plm);
		seek_frame = plm_decode_video(v.plm);
		if (!seek_frame) {
			v.ended = 1;
			return;
		}
	} else {
		log_msg("video: seek (%s) from %.2f s to %.2f s: at %.2f s in "
			"%.1f ms", why, v.clock, target, seek_frame->time,
			(now_ns() - t0) / 1e6);
	}
	v.last = seek_frame;
	v.clock = seek_frame->time;
	v.next_pic = v.clock + 1.0 / v.fps;
	v.ended = 0;
	fifo_reset();
	v.dirty = 1;
}

static void read_options(const char *why)
{
	struct retro_variable var = { "video_scale", NULL };
	int s = env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
		!strcmp(var.value, "crop") ? VIDEO_CROP : VIDEO_FIT;

	if (s == v.scale && v.ow)
		return;
	v.scale = s;
	make_map(why);
	if (v.last)
		convert(v.last);
	v.dirty = 1;
}

/* ---------------------------------------------------------- libretro */

void video_retro_set_environment(retro_environment_t cb) { env_cb = cb; }
void video_retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void video_retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void video_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	batch_cb = cb;
}
void video_retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void video_retro_set_input_state(retro_input_state_t cb) { state_cb = cb; }

void video_retro_init(void)
{
	enum retro_pixel_format f = RETRO_PIXEL_FORMAT_RGB565;

	env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
	tables();
}

void video_retro_deinit(void)
{
}

void video_retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "pl_mpeg";
	info->library_version = "video";
	info->valid_extensions = "mpg";
	info->need_fullpath = true;
}

void video_retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = (unsigned)v.ow;
	info->geometry.base_height = (unsigned)v.oh;
	info->geometry.max_width = MAX_W;
	info->geometry.max_height = MAX_H;
	info->geometry.aspect_ratio = v.oh ? (float)v.ow / v.oh : 1.5f;
	info->timing.fps = FPS;
	info->timing.sample_rate = v.rate;
}

bool video_retro_load_game(const struct retro_game_info *game)
{
	const char *base;
	int64_t t0 = now_ns();

	memset(&v, 0, sizeof(v));
	if (!game || !game->path)
		return false;
	base = strrchr(game->path, '/');
	snprintf(v.name, sizeof(v.name), "%s", base ? base + 1 : game->path);
	v.plm = plm_create_with_filename(game->path);
	if (!v.plm) {
		log_msg("video: %s: cannot open", v.name);
		return false;
	}
	if (!plm_has_headers(v.plm) && !plm_probe(v.plm, 5 << 20)) {
		log_msg("video: %s: no MPEG-PS headers or streams found", v.name);
		goto fail;
	}
	v.sw = plm_get_width(v.plm);
	v.sh = plm_get_height(v.plm);
	v.fps = plm_get_framerate(v.plm);
	if (plm_get_num_video_streams(v.plm) < 1 || v.sw <= 0 || v.sh <= 0 ||
	    v.fps <= 0) {
		log_msg("video: %s: no MPEG-1 video stream (%d streams, %dx%d, "
			"%.3f fps)", v.name, plm_get_num_video_streams(v.plm),
			v.sw, v.sh, v.fps);
		goto fail;
	}
	v.has_audio = plm_get_num_audio_streams(v.plm) > 0 &&
		      plm_get_samplerate(v.plm) > 0;
	v.rate = v.has_audio ? plm_get_samplerate(v.plm) : NOAUDIO_RATE;
	v.channels = v.has_audio ? plm_audio_channels(v.plm) : 0;
	plm_set_audio_enabled(v.plm, v.has_audio);
	v.duration = plm_get_duration(v.plm);
	v.last = plm_decode_video(v.plm);
	if (!v.last) {
		log_msg("video: %s: the first picture does not decode", v.name);
		goto fail;
	}
	v.clock = v.last->time;
	v.next_pic = v.clock + 1.0 / v.fps;
	v.scale = -1;
	log_msg("video: %s: %dx%d at %.3f fps (pixel aspect %.3f), %s, "
		"duration %.1f s, opened in %.1f ms", v.name, v.sw, v.sh, v.fps,
		plm_get_pixel_aspect_ratio(v.plm), v.has_audio ?
		(v.channels == 1 ? "MP2 mono" : "MP2 stereo") : "no audio",
		v.duration, (now_ns() - t0) / 1e6);
	if (v.has_audio)
		log_msg("video: audio %d Hz, %d channel%s", v.rate, v.channels,
			v.channels == 1 ? " (played on both)" : "s");
	else
		log_msg("video: no audio stream, playing silence at %d Hz",
			v.rate);
	read_options("start");
	return true;
fail:
	plm_destroy(v.plm);
	v.plm = NULL;
	return false;
}

void video_retro_unload_game(void)
{
	if (!v.plm)
		return;
	log_stats(v.ended ? "unload after the end" : "unload");
	log_msg("video: %s closed at %.2f s of %.1f s", v.name, v.clock,
		v.duration);
	plm_destroy(v.plm);
	memset(&v, 0, sizeof(v));
}

void video_retro_reset(void)
{
	if (!v.plm)
		return;
	v.paused = 0;
	seek_to(0, "restart");
}

void video_retro_run(void)
{
	bool updated = false;
	uint16_t pad, pressed;
	int n, play;

	if (!v.plm)
		return;
	if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		read_options("changed");
	poll_cb();
	pad = (uint16_t)state_cb(0, RETRO_DEVICE_JOYPAD, 0,
				 RETRO_DEVICE_ID_JOYPAD_MASK);
	pressed = (uint16_t)(pad & ~v.prev_pad);
	v.prev_pad = pad;
	if (pressed & (1 << RETRO_DEVICE_ID_JOYPAD_A)) {
		if (v.ended) {
			log_msg("video: A after the end: playing from the start");
			v.paused = 0;
			seek_to(0, "replay");
		} else {
			v.paused = !v.paused;
			log_msg("video: %s at %.2f s", v.paused ? "paused" :
				"playing", v.clock);
		}
	}
	if (!v.ended) {
		static const struct { int id; double d; } keys[] = {
			{ RETRO_DEVICE_ID_JOYPAD_LEFT, -SEEK_SHORT },
			{ RETRO_DEVICE_ID_JOYPAD_RIGHT, SEEK_SHORT },
			{ RETRO_DEVICE_ID_JOYPAD_L, -SEEK_LONG },
			{ RETRO_DEVICE_ID_JOYPAD_R, SEEK_LONG },
		};
		static const char *const names[] = {
			"-10 s", "+10 s", "-60 s", "+60 s",
		};

		for (size_t i = 0; i < ARRAY_SIZE(keys); i++)
			if (pressed & (1 << keys[i].id)) {
				seek_to(v.clock + keys[i].d, names[i]);
				break;
			}
	}

	play = !v.paused && !v.ended;
	if (play && !v.dirty && v.next_pic <= v.clock + 1e-6) {
		int64_t t0 = now_ns(), dt;
		plm_frame_t *f = NULL;
		int decoded = 0;

		/* Pictures that are due; only the newest is converted. */
		while (v.next_pic <= v.clock + 1e-6) {
			f = plm_decode_video(v.plm);
			if (!f)
				break;
			decoded++;
			v.last = f;
			v.next_pic = f->time + 1.0 / v.fps;
		}
		if (decoded) {
			convert(v.last);
			v.dropped += (unsigned)decoded - 1;
			v.pics += (unsigned)decoded;
			dt = now_ns() - t0;
			v.pic_ns += dt;
			v.pic_max_ns = dt > v.pic_max_ns ? dt : v.pic_max_ns;
			v.dirty = 1;
		}
		if (!f) {
			v.ended = 1;
			play = 0;
			log_msg("video: %s ended at %.2f s (%s), paused on the last "
				"picture; A plays it again", v.name, v.clock,
				plm_has_ended(v.plm) ? "end of the file" :
				"no more pictures decode");
			log_stats("end");
		}
	}
	if (v.dirty) {
		video_cb(fb, (unsigned)v.ow, (unsigned)v.oh, (size_t)v.ow * 2);
		v.dirty = 0;
	} else {
		video_cb(NULL, (unsigned)v.ow, (unsigned)v.oh, (size_t)v.ow * 2);
		v.repeated++;
	}
	v.audio_acc += v.rate / FPS;
	n = (int)v.audio_acc;
	v.audio_acc -= n;
	audio_run(n, play);
	if (play)
		v.clock += 1.0 / FPS;
	v.runs++;
}

void *video_retro_get_memory_data(unsigned id)
{
	(void)id;
	return NULL;
}

size_t video_retro_get_memory_size(unsigned id)
{
	(void)id;
	return 0;
}

struct video_state {
	char magic[8];
	double pos;
	int paused;
};

size_t video_retro_serialize_size(void)
{
	return sizeof(struct video_state);
}

bool video_retro_serialize(void *data, size_t size)
{
	struct video_state st = { STATE_MAGIC, v.ended ? 0 : v.clock,
				  v.ended ? 0 : v.paused };

	if (size != sizeof(st) || !v.plm)
		return false;
	memcpy(data, &st, sizeof(st));
	return true;
}

bool video_retro_unserialize(const void *data, size_t size)
{
	struct video_state st;

	if (size != sizeof(st) || !v.plm)
		return false;
	memcpy(&st, data, sizeof(st));
	if (memcmp(st.magic, STATE_MAGIC, 8) || !(st.pos >= 0))
		return false;
	v.paused = st.paused != 0;
	if (st.pos > 0)
		seek_to(st.pos, "resume");
	log_msg("video: resuming at %.2f s%s", v.clock,
		v.paused ? ", paused" : "");
	return true;
}
