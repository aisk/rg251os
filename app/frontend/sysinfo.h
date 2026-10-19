#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdint.h>

#include "draw.h"

#define SYSINFO_MAX_CPUS 8

/* Jiffies from one /proc/stat line. */
struct cpu_times {
	unsigned long long busy, total;
};

/* /proc/stat: the "cpu" line into t[0], "cpuN" into t[1 + N] (N below
 * max - 1); returns 1 + the number of cores, 0 without a "cpu" line. */
int sysinfo_parse_stat(const char *text, struct cpu_times *t, int max);
/* Busy percent between two samples, -1 when no time passed. */
int sysinfo_usage(const struct cpu_times *a, const struct cpu_times *b);
/* A /proc/meminfo value in kB ("MemAvailable"), -1 if missing. */
long sysinfo_meminfo_kb(const char *text, const char *key);

/* Where /proc and /sys are, "" on the device; a fake tree for tests. */
void sysinfo_set_root(const char *root);
/* Opens the page: first sample (CPU usage since boot until the next one)
 * and the values logged, with the sensors found. from names the screen
 * it was opened from, for the log. */
void sysinfo_enter(const char *from, int64_t now);
/* Refreshes about once per second; returns UI_BACK on B. */
int sysinfo_update(uint32_t pressed, int64_t now);
void sysinfo_draw(struct canvas *c);

#endif
