#ifndef PERF_H
#define PERF_H

#include <stdint.h>

/* Per-interval timing samples; formatted once per report. */
#define PERF_MAX_SAMPLES 1024

struct perf_series {
	float v[PERF_MAX_SAMPLES];
	int n;
	double sum;
	float max;
};

void perf_add(struct perf_series *s, int64_t ns);
/* "avg/p99/max" in ms, then resets the series. */
void perf_format(struct perf_series *s, char *buf, int size);

#endif
