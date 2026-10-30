/*
 * Video player as a libretro core (CORE_VIDEO, cores.c): plays the *.mpg
 * files of the ROM list (MPEG-PS with MPEG-1 video and MP2 audio, decoded
 * by pl_mpeg, see plm.c), streamed from the card (need_fullpath, never
 * read whole). Linked into every build, the stub build too, so the tests
 * run it. Its entry points are named video_retro_* here, no renaming.
 *
 * Threads: decoding a picture takes 13 to 18 ms on the device at 1296
 * MHz, which does not fit in one 16.7 ms vblank, but a 30 fps video has
 * 33 ms per picture. So all pl_mpeg work (demux, video and MP2 decode)
 * and the conversion to RGB565 run in one worker thread, the only user
 * of the plm_t once the load is done (pl_mpeg is not thread safe). The
 * worker runs ahead into NSLOT picture slots (one shown by retro_run, the
 * others queued with their time) and an audio FIFO (stereo S16 with the
 * time of its first frame, filled AUDIO_LEAD past the newest picture),
 * and blocks on a condition variable when both are full. retro_run only
 * takes the picture whose time has come and its audio, under one mutex,
 * well under 1 ms; it never waits for the worker (but see lockstep
 * below). Every slot keeps a copy of its picture's YUV planes, so a
 * mapping change can convert the shown picture again.
 *
 * Requests go through the mutex: a seek (also restart, replay after the
 * end and resume) bumps the generation, drops the queued pictures and the
 * FIFO and asks the worker for plm_seek; whatever the worker finishes for
 * an older generation is thrown away. Until the seek's picture is queued
 * (and its audio, at most BUFFER_MAX_RUNS runs), retro_run keeps the old
 * picture on screen, plays silence and holds the clock: no stall, just a
 * few runs of the old picture as the seek takes (about 35 ms). Resume is
 * a seek from retro_unserialize, during the load, which waits up to
 * RESUME_WAIT_NS for it so the first frame is already the resumed one.
 * Pause needs nothing from the worker: it fills up and blocks. Unload
 * sets quit, wakes the worker and joins it (it finishes the picture it is
 * on first).
 *
 * Picture: the decoder's YUV 4:2:0 converted to RGB565 (BT.601, limited
 * range, table driven) into a slot of at most 480x320, the logical
 * screen, mapped by the global setting video_scale (core option
 * "video_scale", read at load and after a GET_VARIABLE_UPDATE): Fit keeps
 * a picture that fits as it is (main.c scales it by the largest integer
 * that fits and centers it with black bars: 480x320 at 1x, 240x160 at 2x,
 * 480x270 with bars top and bottom) and shrinks a larger one to fit,
 * keeping its aspect; Crop scales the picture to cover the whole screen
 * and cuts off the overflow evenly on both sides. Scaling is nearest
 * neighbour; the pixel aspect ratio of the stream is logged and ignored
 * (square pixels). A change takes effect on the next run, paused or not:
 * retro_run converts the shown picture again from its YUV copy, the
 * worker uses the new mapping from its next picture on, and the queued
 * pictures converted with the old one (at most NSLOT - 1) are converted
 * again by retro_run when they are taken.
 *
 * Timing: the frontend runs retro_run once per vblank (about 60.02 Hz)
 * and resamples the audio from the fps and rate declared here (FPS, the
 * stream's sample rate), so each run is exactly 1/FPS of the video: the
 * play clock advances by that, the newest queued picture whose time has
 * come is shown (older due ones are dropped; a 30 fps video gets a new
 * picture every other run, the others repeat it with a NULL
 * video_refresh, which core.c counts as skipped and cpuctl.c does not
 * judge), and rate/FPS stereo samples (fractions carried) are taken from
 * the FIFO. Audio and picture both follow the run count, so they cannot
 * drift apart; the FIFO head's time is compared with the play clock, and
 * more than SYNC_SLACK apart (the start, after a seek, after it ran dry)
 * the FIFO is cut or padded with silence until they match. If the worker
 * falls behind, the last picture repeats (a late run; the picture shown
 * a run or more after its time is a late picture) and a FIFO that runs
 * dry plays silence (a dry run); the clock goes on. A mono stream comes
 * out of pl_mpeg on both channels; a file without audio plays silence at
 * NOAUDIO_RATE.
 *
 * CPU clock (cpuctl.c): the worker clears the uclamp_min it inherits
 * (cpuctl_thread_uclamp_min), main.c starts cpuctl's floor at the lowest
 * OPP (CPU_START_LOWEST) for a video, and the clock follows schedutil's
 * view of the worker's load: one picture per picture period at the
 * clock that fits it with schedutil's 25% headroom, and running without
 * pause (so a rising clock) when it falls behind.
 *
 * Controls (the joypad as retro_run sees it; SELECT+START is still the
 * frontend's in-game menu, B its OSD, see osd.c): A pauses and resumes
 * (after the end: plays again from the start), Left/Right seek 10 s
 * back/forward, Down/Up 60 s; a seek key held repeats its seek after
 * REPEAT_FIRST_RUNS runs (400 ms), then every REPEAT_NEXT_RUNS (250 ms),
 * each a real seek from where the last one aimed, so the picture follows.
 * Runs are counted rather than time so the tests are exact; one run is a
 * vblank. A seek goes to the intra frame at or before the target (fast,
 * plm_seek without seek_exact). While paused the picture stays and
 * silence plays. L/R ask the frontend for the previous/next video of the
 * folder (video_core_take_request, main.c switches); the end of the file
 * asks it too, for the next video if there is one. Every control bumps a
 * counter the frontend's OSD watches (video_core_status), along with the
 * position, the duration and whether it is paused or ended.
 *
 * End of the file: once the worker found no more pictures and the last
 * one was shown, it stays, paused, until A plays it again from the start,
 * a seek goes back, the menu quits or the frontend starts the next video.
 * No save RAM (memory size 0). The save state is the
 * play position (0 after the end; the seek target while a seek is under
 * way) and whether it was paused, so the frontend's resume state brings
 * the video back where it was after a quit, a power off or the battery
 * running out asleep.
 *
 * Log lines "video: ...": at load the stream (size, frame rate, pixel
 * aspect, audio rate and channels, duration), the mapping and the worker
 * (slots, FIFO, its uclamp_min), every mapping change (mode, scaled size,
 * crop offsets), every seek (why: the key and its repeat count, or
 * resume, restart, replay; target, where it landed, the worker's time
 * for it, runs until shown), pause and play, the end; with debug logging every 600
 * played runs a line for those 10 s (pictures, decode avg/max, ring
 * occupancy, late, dry, worker CPU share, cpu0 clock); at the end and at
 * unload the statistics: pictures decoded, decode+convert avg and max per
 * picture (worker wall time) and the conversion's share, audio decode
 * time, pictures shown, dropped and late, late and dry runs, ring
 * occupancy min and avg at retro_run, repeated runs, seeks, pictures
 * converted again on the main thread, the worker's CPU time against its
 * wall time and the time it was blocked.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"
#include "cpuctl.h"
#include "fe.h"
#include "libretro.h"
#include "plm.h"
#include "settings.h"

#define MAX_W 480
#define MAX_H 320
#define FPS 60.0
#define NOAUDIO_RATE 44100
#define NSLOT 4                   /* pictures: one shown, the rest queued */
#define FIFO_FRAMES 16384         /* stereo frames, 341 ms at 48 kHz */
#define AUDIO_LEAD 0.15           /* s of audio past the newest picture */
#define AUDIO_MIN 4096            /* frames kept in the FIFO regardless */
#define SYNC_SLACK 0.02           /* s between the FIFO head and the clock */
#define SEEK_SHORT 10.0
#define SEEK_LONG 60.0
#define REPEAT_FIRST_RUNS 24      /* a held seek key repeats: 400 ms */
#define REPEAT_NEXT_RUNS 15       /* then every 250 ms */
#define BUFFER_MAX_RUNS 30        /* runs a seek's picture waits for audio */
#define RESUME_WAIT_NS (500 * 1000000LL)
#define LOCKSTEP_WAIT_NS (5000 * 1000000LL)
#define WINDOW_RUNS 600           /* played runs per debug line, 10 s */
#define WORKER_STACK (256 << 10)
#define STATE_MAGIC "vidst01"

enum { SLOT_FREE, SLOT_BUSY, SLOT_QUEUED, SLOT_SHOWN };

struct map {
	unsigned gen;
	int ow, oh;              /* the picture made, at most MAX_W x MAX_H */
	int xmap[MAX_W], cxmap[MAX_W], ymap[MAX_H];  /* source per output */
};

struct slot {
	int state;
	double time;
	unsigned gen, map_gen;   /* seek generation, mapping converted with */
	int ow, oh;
	uint16_t *rgb;           /* MAX_W x MAX_H */
	uint8_t *y, *cb, *cr;    /* copy of the decoded planes */
};

/* Statistics since the load; the worker's under the mutex. */
struct counters {
	unsigned decoded, discarded, audio_frames, waits;
	int64_t dec_ns, conv_ns, audio_ns, wait_ns, pic_max_ns;
	unsigned shown, dropped, late_pics, late_runs, dry_runs, dry_frames;
	unsigned repeated, runs, played, seeks, reconverted;
	unsigned occ_n;
	uint64_t occ_sum;
	int occ_min;
};

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t batch_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t state_cb;

static struct {
	plm_t *plm;              /* the worker's once it runs */
	char name[128];          /* file name, for the log */
	int sw, sh;              /* the stream's picture */
	int yw, yh, cw, ch;      /* its decoded planes */
	double fps, duration;
	int has_audio, rate, channels;
	int scale;               /* VIDEO_FIT or VIDEO_CROP, as set */
	struct map map;          /* written by retro_run under the mutex */

	pthread_t thread;
	int thread_ok;
	pthread_mutex_t mu;
	pthread_cond_t wcond;    /* the worker waits on it */
	pthread_cond_t mcond;    /* waits for the worker (resume, lockstep) */
	int64_t worker_t0;

	/* Shared, under mu. */
	struct slot slots[NSLOT];
	int queue[NSLOT], qn;    /* queued slots, oldest first */
	unsigned gen;            /* seek generation */
	int seek_pending, quit, waiting, started, uclamp_err;
	double seek_target;
	struct {                 /* the worker's answer to seek gen */
		unsigned gen;
		int done, ok;
		double landed;
		int64_t ns;
	} seek_res;
	int video_eof, audio_eof;  /* for the current generation */
	int eof_file;            /* video_eof at the end of the file */
	int16_t fifo[FIFO_FRAMES * 2];
	int fifo_rd, fifo_n;
	double head_time;        /* time of the FIFO's first frame */
	struct counters k;

	/* retro_run's own. */
	int shown;               /* slot on screen, -1 none */
	double clock;            /* play position of this run */
	double next_pic;         /* time of the next picture */
	int paused, ended, dirty, buffering, buf_runs;
	char seek_why[48];
	double seek_from, seek_to;
	uint16_t prev_pad;
	int rep_key, rep_runs, rep_count;  /* held seek key, -1 none */
	int request;             /* enum video_request for the frontend */
	unsigned events;         /* controls acted on, for the OSD */
	double audio_acc;
	/* Debug window. */
	struct counters wk;
	int64_t win_t0, win_cpu0, win_max_ns;
	int win_occ_min;
} v;

static int lockstep, test_delay_ms;
static int ytab[256], rv[256], gu[256], gv[256], bu[256];
static uint16_t rtab[1024], gtab[1024], btab[1024];

/* The worker's own. */
static struct {
	struct map map;
	double last_pic, audio_next;
	plm_frame_t *seek_frame;
	int16_t abuf[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
} w;

/* The save state: the play position and whether it was paused. */
struct video_state {
	char magic[8];
	double pos;
	int paused;
};

/* ------------------------------------------------------------ picture */

static void tables(void)
{
	/* ytab carries a bias of 384 << 8, so the sums index the clamp
	 * tables (-384..639) without a sign; see convert. */
	for (int i = 0; i < 256; i++) {
		ytab[i] = 298 * (i - 16) + 128 + (384 << 8);
		rv[i] = 409 * (i - 128);
		gu[i] = -100 * (i - 128);
		gv[i] = -208 * (i - 128);
		bu[i] = 516 * (i - 128);
	}
	for (int i = 0; i < 1024; i++) {
		int c = i - 384;

		c = c < 0 ? 0 : c > 255 ? 255 : c;
		rtab[i] = (uint16_t)((c >> 3) << 11);
		gtab[i] = (uint16_t)((c >> 2) << 5);
		btab[i] = (uint16_t)(c >> 3);
	}
}

/* The mapping for the stream and v.scale into m (gen not touched). */
static void make_map(struct map *m, const char *why)
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
	m->ow = scaled_w < MAX_W ? scaled_w : MAX_W;
	m->oh = scaled_h < MAX_H ? scaled_h : MAX_H;
	cx = (scaled_w - m->ow) / 2;
	cy = (scaled_h - m->oh) / 2;
	for (int x = 0; x < m->ow; x++) {
		int sx = (int)((long)(x + cx) * sw / scaled_w);

		m->xmap[x] = sx < sw ? sx : sw - 1;
		m->cxmap[x] = m->xmap[x] >> 1;
	}
	for (int y = 0; y < m->oh; y++) {
		int sy = (int)((long)(y + cy) * sh / scaled_h);

		m->ymap[y] = sy < sh ? sy : sh - 1;
	}
	log_msg("video: %s: %s, %dx%d scaled to %dx%d, crop %d,%d, picture "
		"%dx%d", why, video_scale_names[v.scale], sw, sh, scaled_w,
		scaled_h, cx, cy, m->ow, m->oh);
}

/* A slot's YUV copy into its RGB565 with mapping m. */
static void convert(const struct map *m, struct slot *s)
{
	for (int y = 0; y < m->oh; y++) {
		int sy = m->ymap[y];
		const uint8_t *yr = s->y + (long)sy * v.yw;
		const uint8_t *br = s->cb + (long)(sy >> 1) * v.cw;
		const uint8_t *rr = s->cr + (long)(sy >> 1) * v.cw;
		uint16_t *d = s->rgb + (long)y * m->ow;

		for (int x = 0; x < m->ow; x++) {
			int c = ytab[yr[m->xmap[x]]], cx = m->cxmap[x];
			int u = br[cx], r = rr[cx];

			d[x] = (uint16_t)(rtab[(c + rv[r]) >> 8] |
					  gtab[(c + gu[u] + gv[r]) >> 8] |
					  btab[(c + bu[u]) >> 8]);
		}
	}
	s->ow = m->ow;
	s->oh = m->oh;
	s->map_gen = m->gen;
}

static void copy_planes(struct slot *s, const plm_frame_t *f)
{
	memcpy(s->y, f->y.data, (size_t)v.yw * v.yh);
	memcpy(s->cb, f->cb.data, (size_t)v.cw * v.ch);
	memcpy(s->cr, f->cr.data, (size_t)v.cw * v.ch);
	s->time = f->time;
}

/* ------------------------------------------------- shared, under v.mu */

static int free_slot(void)
{
	for (int i = 0; i < NSLOT; i++)
		if (v.slots[i].state == SLOT_FREE)
			return i;
	return -1;
}

static void enqueue(int i)
{
	v.slots[i].state = SLOT_QUEUED;
	v.queue[v.qn++] = i;
}

static int dequeue(void)
{
	int i = v.queue[0];

	memmove(v.queue, v.queue + 1, (size_t)--v.qn * sizeof(v.queue[0]));
	return i;
}

static void flush_queue(void)
{
	while (v.qn)
		v.slots[dequeue()].state = SLOT_FREE;
}

static double fifo_end(void)
{
	return v.head_time + (double)v.fifo_n / v.rate;
}

static void fifo_drop(int n)
{
	if (n > v.fifo_n)
		n = v.fifo_n;
	v.fifo_rd = (v.fifo_rd + n) % FIFO_FRAMES;
	v.fifo_n -= n;
	v.head_time += (double)n / v.rate;
}

static void fifo_take(int16_t *out, int n)
{
	int first = FIFO_FRAMES - v.fifo_rd;

	if (first > n)
		first = n;
	memcpy(out, v.fifo + 2 * v.fifo_rd, (size_t)first * 4);
	memcpy(out + 2 * first, v.fifo, (size_t)(n - first) * 4);
	fifo_drop(n);
}

static void fifo_put(const int16_t *in, int n, double time)
{
	int wr = (v.fifo_rd + v.fifo_n) % FIFO_FRAMES, first = FIFO_FRAMES - wr;

	if (!v.fifo_n)
		v.head_time = time;
	if (first > n)
		first = n;
	memcpy(v.fifo + 2 * wr, in, (size_t)first * 4);
	memcpy(v.fifo, in + 2 * first, (size_t)(n - first) * 4);
	v.fifo_n += n;
}

/* ------------------------------------------------------------- worker */

static int want_video(void)
{
	return !v.video_eof && free_slot() >= 0;
}

static int want_audio(void)
{
	return v.has_audio && !v.audio_eof &&
	       v.fifo_n + PLM_AUDIO_SAMPLES_PER_FRAME <= FIFO_FRAMES &&
	       (v.video_eof || v.fifo_n < AUDIO_MIN ||
		w.audio_next < w.last_pic + AUDIO_LEAD);
}

static void worker_map(void)
{
	if (w.map.gen != v.map.gen)
		w.map = v.map;
}

static void on_seek_frame(plm_t *p, plm_frame_t *f, void *user)
{
	(void)p;
	(void)user;
	w.seek_frame = f;
}

/* The pending seek, called and returning with v.mu held. */
static void work_seek(void)
{
	unsigned gen = v.gen;
	double target = v.seek_target;
	int s = free_slot(), ok;
	int64_t t0 = now_ns();
	plm_frame_t *f;

	v.seek_pending = 0;
	v.slots[s].state = SLOT_BUSY;
	worker_map();
	pthread_mutex_unlock(&v.mu);
	w.seek_frame = NULL;
	plm_set_video_decode_callback(v.plm, on_seek_frame, NULL);
	ok = plm_seek(v.plm, target, 0) && w.seek_frame;
	plm_set_video_decode_callback(v.plm, NULL, NULL);
	f = w.seek_frame;
	if (!ok) {
		/* The decoder lost its place: back to the start. */
		plm_rewind(v.plm);
		f = plm_decode_video(v.plm);
	}
	if (f) {
		copy_planes(&v.slots[s], f);
		convert(&w.map, &v.slots[s]);
	}
	pthread_mutex_lock(&v.mu);
	w.last_pic = f ? f->time : 0;
	w.audio_next = w.last_pic;
	if (gen != v.gen) {
		v.slots[s].state = SLOT_FREE;
		v.k.discarded += f != NULL;
		return;
	}
	v.seek_res.gen = gen;
	v.seek_res.done = 1;
	v.seek_res.ok = ok;
	v.seek_res.landed = f ? f->time : 0;
	v.seek_res.ns = now_ns() - t0;
	if (f) {
		v.slots[s].gen = gen;
		enqueue(s);
	} else {
		v.slots[s].state = SLOT_FREE;
		v.video_eof = 1;
	}
	pthread_cond_broadcast(&v.mcond);
}

/* One picture into a free slot, with v.mu held. */
static void work_video(void)
{
	unsigned gen = v.gen;
	int s = free_slot();
	int64_t t0 = now_ns(), t1, t2;
	plm_frame_t *f;

	v.slots[s].state = SLOT_BUSY;
	worker_map();
	pthread_mutex_unlock(&v.mu);
	f = plm_decode_video(v.plm);
	t1 = now_ns();
	if (f) {
		copy_planes(&v.slots[s], f);
		convert(&w.map, &v.slots[s]);
		if (test_delay_ms) {
			struct timespec ts = { 0, test_delay_ms * 1000000L };

			nanosleep(&ts, NULL);
		}
	}
	t2 = now_ns();
	pthread_mutex_lock(&v.mu);
	if (f) {
		v.k.decoded++;
		v.k.dec_ns += t2 - t0;
		v.k.conv_ns += t2 - t1;
		v.k.pic_max_ns = t2 - t0 > v.k.pic_max_ns ? t2 - t0 :
				 v.k.pic_max_ns;
		v.win_max_ns = t2 - t0 > v.win_max_ns ? t2 - t0 : v.win_max_ns;
		w.last_pic = f->time;
	}
	if (gen != v.gen) {
		v.slots[s].state = SLOT_FREE;
		v.k.discarded += f != NULL;
		return;
	}
	if (f) {
		v.slots[s].gen = gen;
		enqueue(s);
	} else {
		v.slots[s].state = SLOT_FREE;
		v.video_eof = 1;
		v.eof_file = plm_has_ended(v.plm);
	}
	pthread_cond_broadcast(&v.mcond);
}

/* One MP2 frame into the FIFO, with v.mu held. */
static void work_audio(void)
{
	unsigned gen = v.gen;
	int64_t t0 = now_ns();
	plm_samples_t *s;

	pthread_mutex_unlock(&v.mu);
	s = plm_decode_audio(v.plm);
	if (s)
		for (unsigned i = 0; i < 2 * s->count; i++) {
			float f = s->interleaved[i] * 32767.0f;

			w.abuf[i] = (int16_t)(f > 32767.0f ? 32767 :
					      f < -32768.0f ? -32768 : (int)f);
		}
	pthread_mutex_lock(&v.mu);
	v.k.audio_ns += now_ns() - t0;
	if (gen != v.gen)
		return;
	if (!s) {
		v.audio_eof = 1;
	} else {
		v.k.audio_frames++;
		fifo_put(w.abuf, (int)s->count, s->time);
		w.audio_next = s->time + (double)s->count / v.rate;
	}
	pthread_cond_broadcast(&v.mcond);
}

static void *worker(void *arg)
{
	int err = cpuctl_thread_uclamp_min(0);

	(void)arg;
	pthread_mutex_lock(&v.mu);
	v.uclamp_err = err;
	v.started = 1;
	pthread_cond_broadcast(&v.mcond);
	for (;;) {
		while (!v.quit && !v.seek_pending && !want_audio() &&
		       !want_video()) {
			int64_t t0 = now_ns();

			v.waiting = 1;
			pthread_cond_wait(&v.wcond, &v.mu);
			v.waiting = 0;
			v.k.wait_ns += now_ns() - t0;
			v.k.waits++;
		}
		if (v.quit)
			break;
		if (v.seek_pending)
			work_seek();
		else if (want_audio())
			work_audio();
		else
			work_video();
	}
	pthread_mutex_unlock(&v.mu);
	return NULL;
}

static int64_t worker_cpu_ns(void)
{
	clockid_t id;
	struct timespec ts;

	if (!v.thread_ok || pthread_getcpuclockid(v.thread, &id) ||
	    clock_gettime(id, &ts))
		return -1;
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* -------------------------------------------------------- retro_run's */

static void lock(void)
{
	pthread_mutex_lock(&v.mu);
}

static void unlock(void)
{
	pthread_mutex_unlock(&v.mu);
}

/* Audio frames of this run, before the accumulator moves. */
static int run_frames(void)
{
	return (int)(v.audio_acc + v.rate / FPS);
}

static int audio_ready(double from, int n)
{
	return !v.has_audio || v.audio_eof ||
	       (v.fifo_n && fifo_end() >= from + (double)n / v.rate);
}

/* Under v.mu: the seek's picture (and its audio) can be taken. */
static int seek_ready(int n)
{
	if (!v.seek_res.done || v.seek_res.gen != v.gen)
		return 0;
	if (!v.qn)
		return v.video_eof;
	return audio_ready(v.slots[v.queue[0]].time, n);
}

/* Under v.mu: this run has what it shows and plays (lockstep). */
static int run_ready(int n)
{
	if (v.buffering)
		return seek_ready(n);
	if (v.paused || v.ended)
		return 1;
	if (!v.qn && !v.video_eof && v.next_pic <= v.clock + 1e-6)
		return 0;
	return audio_ready(v.clock, n);
}

/* Under v.mu: wait up to ns for ready(n). */
static int wait_for(int (*ready)(int), int n, int64_t ns)
{
	struct timespec ts;
	int64_t deadline;

	if (ready(n))
		return 1;
	clock_gettime(CLOCK_REALTIME, &ts);
	deadline = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + ns;
	ts.tv_sec = (time_t)(deadline / 1000000000);
	ts.tv_nsec = (long)(deadline % 1000000000);
	while (!ready(n))
		if (pthread_cond_timedwait(&v.mcond, &v.mu, &ts) == ETIMEDOUT)
			return ready(n);
	return 1;
}

/* Under v.mu: slot i goes on screen, the one there is freed. */
static void show(int i)
{
	if (v.shown >= 0)
		v.slots[v.shown].state = SLOT_FREE;
	v.slots[i].state = SLOT_SHOWN;
	v.shown = i;
	v.next_pic = v.slots[i].time + 1.0 / v.fps;
	v.dirty = 1;
	v.k.shown++;
	pthread_cond_signal(&v.wcond);
}

static void log_end(const char *how);

/*
 * Under v.mu, while buffering: take the seek's picture when it is ready
 * (or after BUFFER_MAX_RUNS without its audio). Returns what to log
 * after unlocking: 0 nothing, 1 the seek, 2 the seek and the end.
 */
static int take_seek(int n)
{
	if (!v.seek_res.done || v.seek_res.gen != v.gen)
		return 0;
	if (!v.qn) {
		if (!v.video_eof)
			return 0;
		v.buffering = 0;
		v.ended = 1;
		return 2;
	}
	if (!seek_ready(n) && v.buf_runs < BUFFER_MAX_RUNS)
		return 0;
	show(dequeue());
	v.clock = v.slots[v.shown].time;
	v.buffering = 0;
	return 1;
}

static void log_seek(int what)
{
	if (!v.seek_res.ok)
		log_msg("video: seek (%s) from %.2f s to %.2f s failed, back to "
			"the start", v.seek_why, v.seek_from, v.seek_to);
	else
		log_msg("video: seek (%s) from %.2f s to %.2f s: at %.2f s in "
			"%.1f ms, shown after %d runs", v.seek_why, v.seek_from,
			v.seek_to, v.seek_res.landed, v.seek_res.ns / 1e6,
			v.buf_runs);
	if (what == 2)
		log_end("no picture after the seek");
}

/* To target (clamped), at the intra frame before it, by the worker; why
 * goes into the log line once it is shown. */
static void request_seek(double target, const char *why)
{
	if (target < 0)
		target = 0;
	if (v.duration > 0 && target > v.duration)
		target = v.duration;
	lock();
	v.gen++;
	v.seek_pending = 1;
	v.seek_target = target;
	flush_queue();
	v.fifo_n = 0;
	v.video_eof = v.audio_eof = 0;
	v.k.seeks++;
	pthread_cond_signal(&v.wcond);
	unlock();
	snprintf(v.seek_why, sizeof(v.seek_why), "%s", why);
	v.seek_from = v.clock;
	v.seek_to = target;
	v.buffering = 1;
	v.buf_runs = 0;
	v.ended = 0;
}

/* n frames for this run, from the play clock on; under v.mu. */
static void audio_run(int16_t *out, int n, int play)
{
	int got = 0;

	while (play && v.has_audio && got < n && v.fifo_n) {
		int lead, k;

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
		fifo_take(out + 2 * got, k);
		got += k;
	}
	if (play && v.has_audio && got < n && !v.audio_eof) {
		v.k.dry_runs++;
		v.k.dry_frames += (unsigned)(n - got);
	}
	memset(out + 2 * got, 0, (size_t)(n - got) * 4);
	pthread_cond_signal(&v.wcond);
}

/* ------------------------------------------------------------ logging */

static double pct(int64_t a, int64_t b)
{
	return b > 0 ? 100.0 * a / b : 0;
}

static void log_stats(const char *why)
{
	struct counters k;
	int64_t cpu = worker_cpu_ns(), wall = now_ns() - v.worker_t0;

	lock();
	k = v.k;
	unlock();
	log_msg("video: stats (%s): %u pictures decoded, decode+convert avg "
		"%.2f ms max %.2f ms (convert and copy avg %.2f ms), audio "
		"decode %.2f ms per MP2 frame; %u shown, %u dropped, %u late, "
		"%u runs late, %u runs dry (%u frames of silence); ring at "
		"retro_run min %d avg %.2f of %d; %u of %u runs repeated the "
		"picture, %u seeks, %u converted again by retro_run",
		why, k.decoded, k.decoded ? k.dec_ns / 1e6 / k.decoded : 0.0,
		k.pic_max_ns / 1e6, k.decoded ? k.conv_ns / 1e6 / k.decoded : 0.0,
		k.audio_frames ? k.audio_ns / 1e6 / k.audio_frames : 0.0,
		k.shown, k.dropped, k.late_pics, k.late_runs, k.dry_runs,
		k.dry_frames, k.occ_n ? k.occ_min : 0,
		k.occ_n ? (double)k.occ_sum / k.occ_n : 0.0, NSLOT - 1,
		k.repeated, k.runs, k.seeks, k.reconverted);
	log_msg("video: worker (%s): CPU %.2f s of %.2f s wall (%.0f%%), "
		"blocked %.2f s in %u waits, %u pictures thrown away by seeks",
		why, cpu / 1e9, wall / 1e9, pct(cpu, wall), k.wait_ns / 1e9,
		k.waits, k.discarded);
}

/* The 10 s line, debug logging only. */
static void log_window(void)
{
	struct counters k, b = v.wk;
	int64_t cpu = worker_cpu_ns(), t = now_ns(), max;
	int occ_min = v.win_occ_min;
	unsigned n;

	lock();
	k = v.k;
	max = v.win_max_ns;
	v.win_max_ns = 0;
	unlock();
	n = k.decoded - b.decoded;
	log_verbose("video: 10 s to %.1f s: %u pictures decoded, avg %.2f ms "
		    "max %.2f ms; ring min %d avg %.2f; %u late, %u runs late, "
		    "%u dry, %u dropped; worker CPU %.0f%%, cpu0 %ld MHz",
		    v.clock, n, n ? (k.dec_ns - b.dec_ns) / 1e6 / n : 0.0,
		    max / 1e6, occ_min, k.occ_n - b.occ_n ? (double)(k.occ_sum -
		    b.occ_sum) / (k.occ_n - b.occ_n) : 0.0,
		    k.late_pics - b.late_pics, k.late_runs - b.late_runs,
		    k.dry_runs - b.dry_runs, k.dropped - b.dropped,
		    cpu >= 0 && v.win_cpu0 >= 0 ? pct(cpu - v.win_cpu0,
		    t - v.win_t0) : -1.0, cpu_freq_khz() / 1000);
	v.wk = k;
	v.win_t0 = t;
	v.win_cpu0 = cpu;
	v.win_occ_min = NSLOT;
}

static void log_end(const char *how)
{
	log_msg("video: %s ended at %.2f s (%s), paused on the last picture; "
		"A plays it again", v.name, v.clock, how);
	log_stats("end");
	/* The frontend starts the next video of the folder, if any. */
	v.request = VIDEO_REQ_END;
}

/* ------------------------------------------------------------ control */

static void read_options(const char *why)
{
	struct retro_variable var = { "video_scale", NULL };
	int s = env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
		!strcmp(var.value, "crop") ? VIDEO_CROP : VIDEO_FIT;
	static struct map m;

	if (s == v.scale)
		return;
	v.scale = s;
	make_map(&m, why);
	m.gen = v.map.gen + 1;
	lock();
	v.map = m;
	unlock();
	/* The shown slot is retro_run's: no lock to convert it. */
	if (v.shown >= 0) {
		convert(&v.map, &v.slots[v.shown]);
		v.k.reconverted++;
		v.dirty = 1;
	}
}

/* ---------------------------------------------------------- libretro */

void video_core_test(int on, int delay_ms)
{
	lockstep = on;
	test_delay_ms = delay_ms;
}

void video_core_status(struct video_status *s)
{
	memset(s, 0, sizeof(*s));
	if (!v.plm)
		return;
	s->loaded = 1;
	s->duration = v.duration;
	s->pos = v.buffering ? v.seek_to : v.clock;
	if (s->pos < 0)
		s->pos = 0;
	if (v.duration > 0 && s->pos > v.duration)
		s->pos = v.duration;
	s->paused = v.paused;
	s->ended = v.ended;
	s->buffering = v.buffering;
	s->events = v.events;
}

int video_core_take_request(void)
{
	int r = v.plm ? v.request : VIDEO_REQ_NONE;

	v.request = VIDEO_REQ_NONE;
	return r;
}

double video_core_saved_pos(const char *path)
{
	struct video_state st;

	if (core_state_peek(path, CORE_VIDEO, &st, sizeof(st)) !=
	    (int)sizeof(st) || memcmp(st.magic, STATE_MAGIC, 8) ||
	    !(st.pos >= 0))
		return -1;
	return st.pos;
}

void video_core_counters(struct video_counters *c)
{
	memset(c, 0, sizeof(*c));
	if (!v.plm)
		return;
	lock();
	c->decoded = v.k.decoded;
	c->shown = v.k.shown;
	c->dropped = v.k.dropped;
	c->late_pics = v.k.late_pics;
	c->late_runs = v.k.late_runs;
	c->dry_runs = v.k.dry_runs;
	c->reconverted = v.k.reconverted;
	c->discarded = v.k.discarded;
	c->seeks = v.k.seeks;
	c->waiting = v.waiting;
	unlock();
}

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
	info->geometry.base_width = (unsigned)v.map.ow;
	info->geometry.base_height = (unsigned)v.map.oh;
	info->geometry.max_width = MAX_W;
	info->geometry.max_height = MAX_H;
	info->geometry.aspect_ratio = v.map.oh ? (float)v.map.ow / v.map.oh :
				      1.5f;
	info->timing.fps = FPS;
	info->timing.sample_rate = v.rate;
}

static void free_slots(void)
{
	for (int i = 0; i < NSLOT; i++) {
		free(v.slots[i].rgb);
		free(v.slots[i].y);
	}
}

static int alloc_slots(void)
{
	size_t ys = (size_t)v.yw * v.yh, cs = (size_t)v.cw * v.ch;

	for (int i = 0; i < NSLOT; i++) {
		struct slot *s = &v.slots[i];

		s->rgb = malloc((size_t)MAX_W * MAX_H * 2);
		s->y = malloc(ys + 2 * cs);
		if (!s->rgb || !s->y)
			return -1;
		s->cb = s->y + ys;
		s->cr = s->cb + cs;
		s->state = SLOT_FREE;
	}
	return 0;
}

static void stop_worker(void)
{
	if (!v.thread_ok)
		return;
	lock();
	v.quit = 1;
	pthread_cond_signal(&v.wcond);
	unlock();
	pthread_join(v.thread, NULL);
	v.thread_ok = 0;
}

static void destroy(void)
{
	stop_worker();
	if (v.plm)
		plm_destroy(v.plm);
	free_slots();
	pthread_cond_destroy(&v.wcond);
	pthread_cond_destroy(&v.mcond);
	pthread_mutex_destroy(&v.mu);
	memset(&v, 0, sizeof(v));
}

static int start_worker(void)
{
	pthread_attr_t a;
	int err, uerr;

	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, WORKER_STACK);
	v.worker_t0 = now_ns();
	err = pthread_create(&v.thread, &a, worker, NULL);
	pthread_attr_destroy(&a);
	if (err) {
		log_msg("video: cannot start the worker thread: %s",
			strerror(err));
		return -1;
	}
	v.thread_ok = 1;
	lock();
	while (!v.started)
		pthread_cond_wait(&v.mcond, &v.mu);
	uerr = v.uclamp_err;
	unlock();
	v.win_t0 = now_ns();
	v.win_cpu0 = worker_cpu_ns();
	v.win_occ_min = NSLOT;
	log_msg("video: decoding in a worker thread: %d picture slots (1 shown, "
		"up to %d queued), audio FIFO %d frames (%.0f ms), %.2f s of "
		"audio past the newest picture; worker uclamp_min 0 (%s), its "
		"clock follows schedutil", NSLOT, NSLOT - 1, FIFO_FRAMES,
		1000.0 * FIFO_FRAMES / v.rate, AUDIO_LEAD,
		uerr == 0 ? "cleared" : uerr < 0 ? "the cpufreq actuator is not "
		"uclamp here, nothing to clear" : strerror(uerr));
	return 0;
}

bool video_retro_load_game(const struct retro_game_info *game)
{
	const char *base;
	int64_t t0 = now_ns();
	plm_frame_t *f;

	memset(&v, 0, sizeof(v));
	/* The worker's own too: its mapping is matched by generation,
	 * which starts over with every video (another size: out of
	 * bounds with the last one's mapping). */
	memset(&w, 0, sizeof(w));
	v.shown = -1;
	v.rep_key = -1;
	pthread_mutex_init(&v.mu, NULL);
	pthread_cond_init(&v.wcond, NULL);
	pthread_cond_init(&v.mcond, NULL);
	if (!game || !game->path)
		goto fail;
	base = strrchr(game->path, '/');
	snprintf(v.name, sizeof(v.name), "%s", base ? base + 1 : game->path);
	v.plm = plm_create_with_filename(game->path);
	if (!v.plm) {
		log_msg("video: %s: cannot open", v.name);
		goto fail;
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
	/* The first picture here, before the worker owns the decoder. */
	f = plm_decode_video(v.plm);
	if (!f) {
		log_msg("video: %s: the first picture does not decode", v.name);
		goto fail;
	}
	v.yw = (int)f->y.width;
	v.yh = (int)f->y.height;
	v.cw = (int)f->cb.width;
	v.ch = (int)f->cb.height;
	if (alloc_slots()) {
		log_msg("video: %s: no memory for %d pictures", v.name, NSLOT);
		goto fail;
	}
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
	v.scale = -1;
	read_options("start");
	copy_planes(&v.slots[0], f);
	convert(&v.map, &v.slots[0]);
	enqueue(0);
	v.clock = v.next_pic = f->time;
	w.last_pic = w.audio_next = f->time;
	if (start_worker())
		goto fail;
	return true;
fail:
	destroy();
	return false;
}

void video_retro_unload_game(void)
{
	if (!v.plm)
		return;
	log_stats(v.ended ? "unload after the end" : "unload");
	log_msg("video: %s closed at %.2f s of %.1f s", v.name, v.clock,
		v.duration);
	destroy();
}

void video_retro_reset(void)
{
	if (!v.plm)
		return;
	v.paused = 0;
	v.events++;
	request_seek(0, "restart");
}

/* Picture for a playing run, under v.mu: the newest due one. */
static int take_picture(void)
{
	int take = -1;

	v.k.occ_n++;
	v.k.occ_sum += (unsigned)v.qn;
	if (v.k.occ_n == 1 || v.qn < v.k.occ_min)
		v.k.occ_min = v.qn;
	v.win_occ_min = v.qn < v.win_occ_min ? v.qn : v.win_occ_min;
	while (v.qn && v.slots[v.queue[0]].time <= v.clock + 1e-6) {
		if (take >= 0) {
			v.slots[take].state = SLOT_FREE;
			v.k.dropped++;
		}
		take = dequeue();
	}
	if (take >= 0) {
		if (v.clock - v.slots[take].time >= 1.0 / FPS - 1e-6)
			v.k.late_pics++;
		show(take);
		return 1;
	}
	if (v.next_pic <= v.clock + 1e-6) {
		if (!v.video_eof) {
			v.k.late_runs++;
		} else if (!v.qn) {
			v.ended = 1;
			return 2;
		}
	}
	return 0;
}

/* The seek keys, in the order a press is looked for. */
static const struct {
	int id;
	double d;
	const char *key;
} seek_keys[] = {
	{ RETRO_DEVICE_ID_JOYPAD_LEFT, -SEEK_SHORT, "Left" },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT, SEEK_SHORT, "Right" },
	{ RETRO_DEVICE_ID_JOYPAD_DOWN, -SEEK_LONG, "Down" },
	{ RETRO_DEVICE_ID_JOYPAD_UP, SEEK_LONG, "Up" },
};

/* A seek by seek_keys[i], from where the last one aimed while one is
 * under way; repeat counts the repeats of a held key (0: the press). */
static void key_seek(int i, int repeat)
{
	char why[48];

	if (repeat)
		snprintf(why, sizeof(why), "%+.0f s, %s repeat %d",
			 seek_keys[i].d, seek_keys[i].key, repeat);
	else
		snprintf(why, sizeof(why), "%+.0f s, %s", seek_keys[i].d,
			 seek_keys[i].key);
	v.events++;
	request_seek((v.buffering ? v.seek_to : v.clock) + seek_keys[i].d, why);
}

/*
 * The buttons of this run: A, the seek keys with their hold repeat (a
 * newly pressed one takes over from one held), L/R for the frontend.
 * Seeking also works after the end (back into the video).
 */
static void controls(uint16_t pad, uint16_t pressed)
{
	int seeked = 0;

	if (pressed & (1 << RETRO_DEVICE_ID_JOYPAD_A)) {
		v.events++;
		if (v.ended) {
			log_msg("video: A after the end: playing from the start");
			v.paused = 0;
			request_seek(0, "replay");
		} else {
			v.paused = !v.paused;
			log_msg("video: %s at %.2f s", v.paused ? "paused" :
				"playing", v.clock);
		}
	}
	for (int i = 0; i < (int)ARRAY_SIZE(seek_keys); i++)
		if (pressed & (1 << seek_keys[i].id)) {
			v.rep_key = i;
			v.rep_runs = v.rep_count = 0;
			key_seek(i, 0);
			seeked = 1;
			break;
		}
	if (!seeked && v.rep_key >= 0) {
		if (!(pad & (1 << seek_keys[v.rep_key].id))) {
			v.rep_key = -1;
		} else {
			/* Runs held since the press. */
			int r = ++v.rep_runs;

			if (r == REPEAT_FIRST_RUNS || (r > REPEAT_FIRST_RUNS &&
			    (r - REPEAT_FIRST_RUNS) % REPEAT_NEXT_RUNS == 0))
				key_seek(v.rep_key, ++v.rep_count);
		}
	}
	if (pressed & (1 << RETRO_DEVICE_ID_JOYPAD_L)) {
		v.events++;
		v.request = VIDEO_REQ_PREV;
	}
	if (pressed & (1 << RETRO_DEVICE_ID_JOYPAD_R)) {
		v.events++;
		v.request = VIDEO_REQ_NEXT;
	}
}

void video_retro_run(void)
{
	static int16_t out[2 * 2048];
	bool updated = false;
	uint16_t pad, pressed;
	int n, play, seek_log = 0, end_log = 0;

	if (!v.plm)
		return;
	if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		read_options("changed");
	poll_cb();
	pad = (uint16_t)state_cb(0, RETRO_DEVICE_JOYPAD, 0,
				 RETRO_DEVICE_ID_JOYPAD_MASK);
	pressed = (uint16_t)(pad & ~v.prev_pad);
	v.prev_pad = pad;
	controls(pad, pressed);

	n = run_frames();
	if (n > 2048)
		n = 2048;
	lock();
	if (lockstep && !wait_for(run_ready, n, LOCKSTEP_WAIT_NS))
		log_msg("video: lockstep: the worker did not deliver in 5 s");
	if (v.buffering) {
		seek_log = take_seek(n);
		if (v.buffering)
			v.buf_runs++;
	}
	play = !v.paused && !v.ended && !v.buffering;
	if (play && !seek_log && take_picture() == 2)
		end_log = 1;
	play = play && !v.ended;
	audio_run(out, n, play);
	unlock();

	if (seek_log)
		log_seek(seek_log);
	if (end_log)
		log_end(v.eof_file ? "end of the file" :
			"no more pictures decode");
	if (v.dirty) {
		struct slot *s = &v.slots[v.shown];

		/* Converted before a mapping change: again, here. */
		if (s->map_gen != v.map.gen) {
			convert(&v.map, s);
			v.k.reconverted++;
		}
		video_cb(s->rgb, (unsigned)s->ow, (unsigned)s->oh,
			 (size_t)s->ow * 2);
		v.dirty = 0;
	} else {
		video_cb(NULL, (unsigned)v.map.ow, (unsigned)v.map.oh,
			 (size_t)v.map.ow * 2);
		v.k.repeated++;
	}
	v.audio_acc += v.rate / FPS - n;
	batch_cb(out, (size_t)n);
	if (play) {
		v.clock += 1.0 / FPS;
		if (++v.k.played % WINDOW_RUNS == 0 && log_debug())
			log_window();
	}
	v.k.runs++;
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

size_t video_retro_serialize_size(void)
{
	return sizeof(struct video_state);
}

bool video_retro_serialize(void *data, size_t size)
{
	struct video_state st = { STATE_MAGIC, v.ended ? 0 : v.buffering ?
				  v.seek_to : v.clock, v.ended ? 0 : v.paused };

	if (size != sizeof(st) || !v.plm)
		return false;
	memcpy(data, &st, sizeof(st));
	return true;
}

bool video_retro_unserialize(const void *data, size_t size)
{
	struct video_state st;
	int what = 0, n = run_frames();

	if (size != sizeof(st) || !v.plm)
		return false;
	memcpy(&st, data, sizeof(st));
	if (memcmp(st.magic, STATE_MAGIC, 8) || !(st.pos >= 0))
		return false;
	v.paused = st.paused != 0;
	if (st.pos > 0) {
		/* Still loading: wait for it, so the first frame is the
		 * resumed one rather than the start. */
		request_seek(st.pos, "resume");
		lock();
		wait_for(seek_ready, n, RESUME_WAIT_NS);
		what = take_seek(n);
		unlock();
		if (what)
			log_seek(what);
	}
	log_msg("video: resuming at %.2f s%s%s", v.buffering ? v.seek_to :
		v.clock, v.paused ? ", paused" : "", v.buffering ?
		" (the seek is still under way)" : "");
	return true;
}
