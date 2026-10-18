#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Linear interpolating resampler for interleaved stereo int16, with a
 * ratio that may change between calls without clicks (the phase carries
 * over). step is input frames per output frame in 32.32 fixed point.
 */
struct resampler {
	uint64_t pos;    /* 32.32, relative to prev */
	uint64_t step;
	int16_t prev[2];
};

void rs_init(struct resampler *r, double in_per_out);
void rs_set_ratio(struct resampler *r, double in_per_out);
/* Returns output frames written (at most out_max). */
size_t rs_process(struct resampler *r, const int16_t *in, size_t in_frames,
		  int16_t *out, size_t out_max);

#endif
