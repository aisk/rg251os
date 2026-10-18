/*
 * The source stream is seen as s[-1] = prev, s[0..n-1] = in. Output frame
 * k lies at pos_k (in input frames from s[-1]) and is interpolated between
 * s[floor(pos_k) - 1] and s[floor(pos_k)].
 */
#include <string.h>

#include "resample.h"

void rs_set_ratio(struct resampler *r, double in_per_out)
{
	if (in_per_out < 0.01)
		in_per_out = 0.01;
	r->step = (uint64_t)(in_per_out * 4294967296.0 + 0.5);
}

void rs_init(struct resampler *r, double in_per_out)
{
	memset(r, 0, sizeof(*r));
	rs_set_ratio(r, in_per_out);
}

size_t rs_process(struct resampler *r, const int16_t *in, size_t in_frames,
		  int16_t *out, size_t out_max)
{
	uint64_t end = (uint64_t)in_frames << 32;
	size_t n = 0;

	if (!in_frames)
		return 0;
	while (r->pos < end && n < out_max) {
		size_t i = (size_t)(r->pos >> 32);
		int32_t f = (int32_t)((r->pos >> 17) & 0x7fff);  /* 15 bit */
		const int16_t *a = i ? in + 2 * (i - 1) : r->prev;
		const int16_t *b = in + 2 * i;

		out[2 * n] = (int16_t)(a[0] + (((b[0] - a[0]) * f) >> 15));
		out[2 * n + 1] = (int16_t)(a[1] + (((b[1] - a[1]) * f) >> 15));
		n++;
		r->pos += r->step;
	}
	/* Output full: drop the rest of the input rather than stall. */
	r->pos = r->pos >= end ? r->pos - end : 0;
	r->prev[0] = in[2 * (in_frames - 1)];
	r->prev[1] = in[2 * (in_frames - 1) + 1];
	return n;
}
