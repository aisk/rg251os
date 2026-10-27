#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdint.h>

#include "menu.h"

#define SYSINFO_MAX_CPUS 8

/* The page: labels at x 8, values from SYSINFO_VAL_X, rows from
 * SYSINFO_ROW_Y, SYSINFO_ROW_H px apart, down to the bottom (the key hint
 * is in the title bar), none starting below SYSINFO_ROW_MAX. */
#define SYSINFO_VAL_X   (8 + FONT_W * 8)
#define SYSINFO_ROW_Y   (HEAD_LINE_Y + 6)
#define SYSINFO_ROW_H   FONT_H
#define SYSINFO_ROW_MAX (CANVAS_H - 2 - FONT_H)

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
/* The page: entering takes the first sample (CPU usage since boot until
 * the next one) and logs the values with the sensors found and the page
 * it was opened from; refreshed about once per second, B goes back. */
extern const struct page sysinfo_page;

#endif
