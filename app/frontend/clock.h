#ifndef CLOCK_H
#define CLOCK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "draw.h"

/* Years the page offers; the RK817 keeps two BCD digits from 2000. */
#define CLOCK_YEAR_MIN 2024
#define CLOCK_YEAR_MAX 2099

/* The RTC device and the system clock setter, NULL for the defaults:
 * /dev/rtc0 and settimeofday() as PID 1, neither otherwise (a PC keeps
 * its clock). Tests pass a regular file and a fake. */
void clock_set_backend(const char *rtc_path,
		       int (*settime)(const struct timeval *tv));
/* Startup: the system clock and the RTC read with RTC_RD_TIME, logged,
 * flagged when the RTC looks never set. */
void clock_boot_log(void);
/* Both clocks to tm (UTC, seconds taken as 0), every step logged.
 * Returns 0, or the errno of the first failing step with a short
 * message for the screen in msg (lines split by '\n'). */
int clock_set(const struct tm *tm, char *msg, size_t size);
int clock_days_in_month(int year, int month);  /* month 1..12 */

/* The Date & time page (from the ROM list's settings). */
void datetime_enter(void);
/* UI_BACK after a save that worked or on B; a failed save stays. */
int datetime_update(uint32_t pressed);
void datetime_draw(struct canvas *c);
/* For tests: the time as edited, the selected field (0 year .. 4
 * minute) and the message ("" when none). */
void datetime_value(struct tm *tm);
int datetime_field(void);
const char *datetime_message(void);

#endif
