#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

struct audio_stats {
	unsigned xruns;      /* underruns since the last call */
	unsigned overflows;  /* frames dropped because the buffer was full */
	int fill_min, fill_max;  /* queued frames, sampled before each write */
	double adj;          /* current rate control factor */
};

/* Mixer setup and PCM open on card 0; safe to call again if it failed. */
int audio_init(void);
/* Simulated device for tests: consumes rate*(1+drift) frames per second. */
void audio_init_null(unsigned rate, double drift);
int audio_ready(void);
unsigned audio_rate(void);
unsigned audio_target(void);
/* Source format: core sample rate, core fps and display refresh. */
void audio_set_source(double in_rate, double core_fps, double display_hz);
/* Called from the core's audio callback. */
void audio_push(const int16_t *samples, size_t frames);
/* Once per video frame, after retro_run: rate control and PCM write. */
void audio_flush(void);
/* Stop output (menus); the next flush restarts it. */
void audio_pause(void);
void audio_jack(int inserted);
void audio_set_volume(int raw);
void audio_take_stats(struct audio_stats *st);
/* Time spent resampling and writing since the last call. */
int64_t audio_take_time_ns(void);

/* Apply "Control Name=value" lines (from <data>/mixer.txt). */
int audio_apply_mixer_text(const char *text);

#endif
