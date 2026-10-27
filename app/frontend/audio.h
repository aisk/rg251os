#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

struct audio_stats {
	unsigned xruns;      /* underruns since the last call */
	unsigned overflows;  /* frames dropped because the buffer was full */
	int fill_min, fill_max;  /* queued frames, sampled before each write */
	double adj;          /* current rate control factor */
	double hw_rate;      /* frames/s the device played, 0 if unknown */
};

/* Mixer setup and PCM open on card 0 in settings.audio's mode; cheap to
 * call again, retries at most every 2 s while it fails. */
int audio_init(void);
/* Simulated device for tests, in settings.audio's mode: plays at the
 * nominal rate * (1 + drift). */
void audio_init_null(double drift);
/* Open config: nominal rate, buffer and target fill, in frames. */
unsigned audio_rate(void);
unsigned audio_buffer(void);
unsigned audio_target(void);
/* Source format: core sample rate, core fps and display refresh; starts
 * a game session (statistics) and reopens the PCM if the mode changed. */
void audio_set_source(double in_rate, double core_fps, double display_hz);
/* Called from the core's audio callback. */
void audio_push(const int16_t *samples, size_t frames);
/* Once per video frame, after retro_run: rate control and PCM write. */
void audio_flush(void);
/* For a core's frame skipping, right before retro_run: how full the
 * device buffer is now, in percent of twice the target fill (the target
 * reads 50%, 0..100, see audio.c), and whether less than one
 * video frame of audio is left (after an underrun: 0%, likely). Returns
 * 0 (and 0%) when no stream runs: no PCM, before the first write of a
 * game, after a pause until the next write. */
int audio_buffer_status(unsigned *occupancy, int *underrun_likely);
/* Stop output (menus); the next flush restarts it. */
void audio_pause(void);
/* Sleep: stop and close the PCM; reopen it and restart the rate control
 * (the gap must not look like drift). The stream is held (game audio
 * dropped) until audio_wake_shown, then the next flush starts it. */
void audio_suspend(void);
void audio_resume(void);
/* The first frame after audio_resume is on screen: the held stream may
 * start with the next frame's audio. */
void audio_wake_shown(void);
/* The simulated device plays frames video frames without a write. */
void audio_null_idle(unsigned frames);
void audio_jack(int inserted);
void audio_set_volume(int raw);
/* settings.volume one 3 dB step up or down (d), muted below -60 dB, and
 * applied: for the volume keys and the menus. */
void audio_volume_step(int d);
void audio_take_stats(struct audio_stats *st);
/* Time spent resampling and writing since the last call. */
int64_t audio_take_time_ns(void);
/* Underruns since start, never reset. */
unsigned audio_xruns_total(void);

/* Log mode, config, measured rate, underruns and fill of this game (call
 * at game end, before audio_pause). */
void audio_summary(const char *when);
/* Device rate measured during play this game, 0 if unknown. */
double audio_session_rate(void);
/* perf.csv columns: audio_mode,audio_config,audio_measured_hz,
 * audio_fill_min,audio_fill_max */
int audio_csv_fields(char *buf, size_t size);

#endif
