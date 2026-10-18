/*
 * Frame timing statistics for the periodic performance log line.
 */
#include <stdio.h>
#include <stdlib.h>

#include "perf.h"

void perf_add(struct perf_series *s, int64_t ns)
{
	float ms = (float)(ns / 1e6);

	if (s->n < PERF_MAX_SAMPLES)
		s->v[s->n] = ms;
	s->n++;
	s->sum += ms;
	if (ms > s->max)
		s->max = ms;
}

static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

void perf_format(struct perf_series *s, char *buf, int size)
{
	int kept = s->n < PERF_MAX_SAMPLES ? s->n : PERF_MAX_SAMPLES;
	float p99 = 0;

	if (kept) {
		int i = (kept * 99 + 99) / 100 - 1;

		qsort(s->v, kept, sizeof(s->v[0]), cmp_float);
		p99 = s->v[i < 0 ? 0 : i];
	}
	snprintf(buf, size, "%.2f/%.2f/%.2f", s->n ? s->sum / s->n : 0.0,
		 (double)p99, (double)s->max);
	perf_reset(s);
}

void perf_reset(struct perf_series *s)
{
	s->n = 0;
	s->sum = 0;
	s->max = 0;
}
