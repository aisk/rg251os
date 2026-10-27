/*
 * Date and time: the system clock, the RK817 hardware clock (rtc-rk808,
 * /dev/rtc0 from devtmpfs) and the Date & time page of the ROM list's
 * settings.
 *
 * There are no time zones: TZ is never set, so localtime() is gmtime(),
 * and the time the user enters is kept as UTC in the system clock and in
 * the RTC. The cores give the cartridge RTC (Pokemon Ruby, Sapphire,
 * Emerald and others) time(NULL) through localtime(), so the games see
 * exactly what was entered.
 *
 * At boot the kernel has already copied the RTC into the system clock
 * (RTC_HCTOSYS); clock_boot_log reads the RTC once more (RTC_RD_TIME)
 * and logs both, flagging an RTC that looks never set (before 2024).
 *
 * The page starts from the current time (2024-01-01 00:00 when the clock
 * is unset) with YYYY-MM-DD HH:MM fields: left/right pick a field, up/down
 * change it (with ui_repeat's key repeat; wrapping, the day clamped to the
 * month and leap years), A saves, B cancels. Saving sets the system clock
 * (settimeofday) and then the RTC (RTC_SET_TIME, seconds 0), reads the RTC
 * back and logs each step with the old and new time ("clock: ..."). A
 * save that worked goes back to the settings; a failure stays on the page
 * with a message in red.
 *
 * The rk808 driver keeps dates in Rockchip's calendar, which has a
 * November 31, and converts on every read and write; a time set across
 * the end of November is worth a look in the read back line.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "clock.h"
#include "fe.h"
#include "input.h"
#include "menu.h"

enum { F_YEAR, F_MONTH, F_DAY, F_HOUR, F_MIN, F_COUNT };

static struct {
	const char *rtc_path;  /* NULL: no RTC */
	int (*settime)(const struct timeval *tv);
	int custom;            /* set by clock_set_backend */
} cb;

static struct {
	int v[F_COUNT];
	int field;
	char msg[96];
} dt;

static int sys_settime(const struct timeval *tv)
{
	return settimeofday(tv, NULL);
}

void clock_set_backend(const char *rtc_path,
		       int (*settime)(const struct timeval *tv))
{
	cb.rtc_path = rtc_path;
	cb.settime = settime;
	cb.custom = rtc_path || settime;
}

/* The defaults only as PID 1: the PC running a test keeps its clock. */
static void backend(const char **path, int (**settime)(const struct timeval *))
{
	if (cb.custom) {
		*path = cb.rtc_path;
		*settime = cb.settime;
	} else {
		*path = fe_is_init ? "/dev/rtc0" : NULL;
		*settime = fe_is_init ? sys_settime : NULL;
	}
}

int clock_days_in_month(int year, int month)
{
	static const unsigned char days[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};

	if (month < 1 || month > 12)
		return 31;
	if (month == 2 && ((year % 4 == 0 && year % 100) || year % 400 == 0))
		return 29;
	return days[month - 1];
}

static void fmt_tm(char *buf, size_t size, const struct tm *tm)
{
	strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm);
}

/* RTC_RD_TIME into tm; 0 or an errno value (ENODEV: no RTC). */
static int rtc_read(const char *path, struct tm *tm)
{
	struct rtc_time rt;
	int fd, err = 0;

	memset(tm, 0, sizeof(*tm));
	if (!path)
		return ENODEV;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	memset(&rt, 0, sizeof(rt));
	if (ioctl(fd, RTC_RD_TIME, &rt) < 0)
		err = errno;
	close(fd);
	if (err)
		return err;
	tm->tm_sec = rt.tm_sec;
	tm->tm_min = rt.tm_min;
	tm->tm_hour = rt.tm_hour;
	tm->tm_mday = rt.tm_mday;
	tm->tm_mon = rt.tm_mon;
	tm->tm_year = rt.tm_year;
	return 0;
}

/* The RTC's time for the log, or why it could not be read. */
static int rtc_describe(const char *path, char *buf, size_t size,
			struct tm *tm)
{
	int err = rtc_read(path, tm);

	if (!path)
		snprintf(buf, size, "none (not PID 1)");
	else if (err)
		snprintf(buf, size, "unreadable (%s: %s)", path, strerror(err));
	else
		fmt_tm(buf, size, tm);
	return err;
}

static int rtc_write(const char *path, const struct tm *tm)
{
	struct rtc_time rt;
	int fd, err = 0;

	if (!path)
		return ENODEV;
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return errno;
	memset(&rt, 0, sizeof(rt));
	rt.tm_sec = tm->tm_sec;
	rt.tm_min = tm->tm_min;
	rt.tm_hour = tm->tm_hour;
	rt.tm_mday = tm->tm_mday;
	rt.tm_mon = tm->tm_mon;
	rt.tm_year = tm->tm_year;
	rt.tm_wday = tm->tm_wday;
	rt.tm_yday = tm->tm_yday;
	if (ioctl(fd, RTC_SET_TIME, &rt) < 0)
		err = errno;
	close(fd);
	return err;
}

void clock_boot_log(void)
{
	const char *path;
	int (*settime)(const struct timeval *);
	struct tm sys, rtc;
	char sbuf[24], rbuf[96];
	int valid = realtime_utc(&sys), err;

	backend(&path, &settime);
	fmt_tm(sbuf, sizeof(sbuf), &sys);
	err = rtc_describe(path, rbuf, sizeof(rbuf), &rtc);
	log_msg("clock: system %s UTC%s, RTC %s%s", sbuf,
		valid ? "" : " (before 2024, perf.csv uses boot time)", rbuf,
		!err && rtc.tm_year + 1900 < 2024 ?
		" (looks never set: set it in Settings, Date & time)" : "");
}

int clock_set(const struct tm *in, char *msg, size_t size)
{
	const char *path;
	int (*settime)(const struct timeval *);
	struct tm tm = *in, old, rtc;
	struct timeval tv;
	char nbuf[24], obuf[24], rbuf[96];
	int sys_err = 0, rtc_err;

	backend(&path, &settime);
	/* Normalized, with the weekday and day of the year for the RTC. */
	tm.tm_sec = 0;
	tm.tm_isdst = 0;
	tv.tv_sec = timegm(&tm);
	tv.tv_usec = 0;
	gmtime_r(&tv.tv_sec, &tm);
	fmt_tm(nbuf, sizeof(nbuf), &tm);
	realtime_utc(&old);
	fmt_tm(obuf, sizeof(obuf), &old);
	rtc_describe(path, rbuf, sizeof(rbuf), &rtc);
	log_msg("clock: setting %s UTC; system clock was %s, RTC %s", nbuf,
		obuf, rbuf);

	if (!settime)
		sys_err = EPERM;
	else if (settime(&tv) < 0)
		sys_err = errno ? errno : EIO;
	log_msg("clock: settimeofday(%lld): %s", (long long)tv.tv_sec,
		!settime ? "skipped, not PID 1" : sys_err ? strerror(sys_err) : "ok");

	rtc_err = rtc_write(path, &tm);
	log_msg("clock: RTC_SET_TIME on %s: %s", path ? path : "no RTC",
		rtc_err ? strerror(rtc_err) : "ok");
	if (!rtc_err) {
		rtc_describe(path, rbuf, sizeof(rbuf), &rtc);
		log_msg("clock: RTC reads back %s", rbuf);
	}

	if (sys_err && rtc_err)
		snprintf(msg, size, "Clock and RTC not set:\n%s",
			 strerror(sys_err));
	else if (sys_err)
		snprintf(msg, size, "System clock not set:\n%s",
			 strerror(sys_err));
	else if (rtc_err)
		snprintf(msg, size, "Not saved to the RTC:\n%s",
			 strerror(rtc_err));
	else
		snprintf(msg, size, "%s", "");
	log_sync();
	return sys_err ? sys_err : rtc_err;
}

/* ---------------------------------------------------------------- page */

static void day_clamp(void)
{
	int n = clock_days_in_month(dt.v[F_YEAR], dt.v[F_MONTH]);

	if (dt.v[F_DAY] > n)
		dt.v[F_DAY] = n;
}

static void field_step(int f, int d)
{
	static const int lo[F_COUNT] = { CLOCK_YEAR_MIN, 1, 1, 0, 0 };
	int hi[F_COUNT] = {
		CLOCK_YEAR_MAX, 12,
		clock_days_in_month(dt.v[F_YEAR], dt.v[F_MONTH]), 23, 59,
	};
	int v = dt.v[f] + d;

	if (v > hi[f])
		v = lo[f];
	if (v < lo[f])
		v = hi[f];
	dt.v[f] = v;
	day_clamp();
}

void datetime_enter(void)
{
	struct tm tm;
	char buf[24];
	int valid = realtime_utc(&tm);

	if (!valid || tm.tm_year + 1900 > CLOCK_YEAR_MAX) {
		memset(&tm, 0, sizeof(tm));
		tm.tm_year = CLOCK_YEAR_MIN - 1900;
		tm.tm_mday = 1;
	}
	dt.v[F_YEAR] = tm.tm_year + 1900;
	dt.v[F_MONTH] = tm.tm_mon + 1;
	dt.v[F_DAY] = tm.tm_mday;
	dt.v[F_HOUR] = tm.tm_hour;
	dt.v[F_MIN] = tm.tm_min;
	dt.field = 0;
	dt.msg[0] = 0;
	realtime_utc(&tm);
	fmt_tm(buf, sizeof(buf), &tm);
	log_msg("clock: date & time page opened, system clock %s%s", buf,
		valid ? "" : " (unset, starting from 2024-01-01)");
}

void datetime_value(struct tm *tm)
{
	memset(tm, 0, sizeof(*tm));
	tm->tm_year = dt.v[F_YEAR] - 1900;
	tm->tm_mon = dt.v[F_MONTH] - 1;
	tm->tm_mday = dt.v[F_DAY];
	tm->tm_hour = dt.v[F_HOUR];
	tm->tm_min = dt.v[F_MIN];
}

int datetime_field(void)
{
	return dt.field;
}

const char *datetime_message(void)
{
	return dt.msg;
}

int datetime_update(uint32_t pressed)
{
	struct tm tm;

	if (pressed & JOY_BIT(B)) {
		log_msg("clock: date & time page closed without saving");
		return UI_BACK;
	}
	if (pressed & (JOY_BIT(LEFT) | JOY_BIT(RIGHT) | JOY_BIT(UP) |
		       JOY_BIT(DOWN)))
		dt.msg[0] = 0;
	if (pressed & JOY_BIT(LEFT))
		dt.field = (dt.field + F_COUNT - 1) % F_COUNT;
	if (pressed & JOY_BIT(RIGHT))
		dt.field = (dt.field + 1) % F_COUNT;
	if (pressed & JOY_BIT(UP))
		field_step(dt.field, 1);
	if (pressed & JOY_BIT(DOWN))
		field_step(dt.field, -1);
	if (pressed & (JOY_BIT(A) | JOY_BIT(START))) {
		datetime_value(&tm);
		if (!clock_set(&tm, dt.msg, sizeof(dt.msg)))
			return UI_BACK;
	}
	return UI_NONE;
}

void datetime_draw(struct canvas *c)
{
	/* Column and width of each field in "YYYY-MM-DD HH:MM" at 3x. */
	static const int col[F_COUNT] = { 0, 5, 8, 11, 14 };
	static const int len[F_COUNT] = { 4, 2, 2, 2, 2 };
	const int cw = 24, x0 = (c->w - 16 * cw) / 2, y = 110;
	struct tm now;
	char buf[24];
	const char *p;
	int ly = c->h - 76;

	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 8, 8, 2, COL_HI, "Date & time");
	draw_fill(c, 0, 30, c->w, 2, COL_SEL);

	draw_fill(c, x0 + col[dt.field] * cw - 4, y - 6, len[dt.field] * cw + 8,
		  8 * 3 + 12, COL_SEL);
	snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", dt.v[F_YEAR],
		 dt.v[F_MONTH], dt.v[F_DAY], dt.v[F_HOUR], dt.v[F_MIN]);
	draw_text(c, x0, y, 3, COL_FG, buf);
	draw_text(c, x0 + col[dt.field] * cw + (len[dt.field] * cw - 16) / 2,
		  y - 30, 2, COL_DIM, "+");
	draw_text(c, x0 + col[dt.field] * cw + (len[dt.field] * cw - 16) / 2,
		  y + 38, 2, COL_DIM, "-");

	if (realtime_utc(&now)) {
		fmt_tm(buf, sizeof(buf), &now);
		draw_textf(c, 8, 190, 2, COL_DIM, "Now %s", buf);
	} else {
		draw_text(c, 8, 190, 2, COL_DIM, "Now: not set");
	}

	/* The message, one line per '\n'. */
	for (p = dt.msg; *p && ly < c->h - 30; ly += 20) {
		size_t n = strcspn(p, "\n");

		draw_textf(c, 8, ly, 2, COL_ERR, "%.*s", n > 29 ? 29 : (int)n, p);
		p += n;
		if (*p)
			p++;
	}
	draw_text(c, 8, c->h - 18, 2, COL_DIM, "A:save B:cancel <>:field");
}
