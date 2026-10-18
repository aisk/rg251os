/*
 * Logging and clocks. Every line gets a CLOCK_BOOTTIME timestamp so it
 * lines up with dmesg. Until the GBA partition is mounted, lines are kept
 * in memory and written out when the log file opens. Important lines also
 * go to /dev/kmsg (stderr when not PID 1), so they show up in the dmesg
 * dumps as well.
 *
 * Debug logging (settings "debug", off by default, switchable live in
 * the settings menus): when off, log_verbose() lines are dropped, and the
 * other modules skip what only serves investigations (log_debug()): the
 * 10 s perf lines, battery and cpufreq change lines, kernel log dumps
 * other than at power off and fatal errors, input key and axis lists,
 * mixer and ALSA dumps, ALSA xrun_debug and the kernel's suspend debug
 * messages. Startup, errors, game start/stop, saves, sleep and the per
 * game summaries stay.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fe.h"

static int kmsg_fd = -1;
static int file_fd = -1;
static int debug = 1;
static char backlog[32768];
static size_t backlog_len;

int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int64_t boot_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int realtime_utc(struct tm *tm)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	if (!gmtime_r(&ts.tv_sec, tm))
		memset(tm, 0, sizeof(*tm));
	return ts.tv_sec > 1704067200;  /* 2024-01-01 */
}

void log_set_kmsg(int fd)
{
	kmsg_fd = fd;
}

static void out_file(const char *buf, size_t n)
{
	if (file_fd >= 0) {
		if (write(file_fd, buf, n) != (ssize_t)n) {
			/* Card pulled or full: stop trying, keep the kernel log. */
			close(file_fd);
			file_fd = -1;
		}
		return;
	}
	if (backlog_len + n <= sizeof(backlog)) {
		memcpy(backlog + backlog_len, buf, n);
		backlog_len += n;
	}
}

static void vlog(int important, const char *fmt, va_list ap)
{
	char buf[512];
	int64_t t = boot_ms();
	int n, m, hl;

	n = snprintf(buf, sizeof(buf), "[%5lld.%03lld] ",
		     (long long)(t / 1000), (long long)(t % 1000));
	hl = n;
	m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
	if (m < 0)
		m = 0;
	n += m;
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	out_file(buf, n);

	if (!fe_is_init) {
		if (write(STDERR_FILENO, buf, n) < 0)
			return;
	} else if (important && kmsg_fd >= 0) {
		/* Skip our timestamp, the kernel adds its own. */
		char k[540];
		int kn;

		kn = snprintf(k, sizeof(k), "<6>frontend: %.*s", n - hl, buf + hl);
		if (kn > (int)sizeof(k) - 1)
			kn = sizeof(k) - 1;
		if (write(kmsg_fd, k, kn) < 0)
			return;
	}
}

void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(1, fmt, ap);
	va_end(ap);
}

void log_verbose(const char *fmt, ...)
{
	va_list ap;

	if (!debug)
		return;
	va_start(ap, fmt);
	vlog(0, fmt, ap);
	va_end(ap);
}

void log_set_debug(int on)
{
	on = !!on;
	if (on != debug)
		log_msg("log: debug logging %s", on ? "on" : "off");
	debug = on;
}

int log_debug(void)
{
	return debug;
}

/* Replaces the file; earlier boots have their own directory. */
void log_open_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC,
		      0644);

	if (fd < 0) {
		log_msg("open %s: %s", path, strerror(errno));
		return;
	}
	file_fd = fd;
	if (backlog_len)
		out_file(backlog, backlog_len);
	backlog_len = 0;
}

void log_sync(void)
{
	if (file_fd >= 0)
		fsync(file_fd);
}

/* fsync in a child, for use while a game runs: a slow card must not
 * stall a frame. The child is reaped by the main loop. */
void log_sync_async(void)
{
	pid_t pid;

	if (file_fd < 0)
		return;
	pid = fork();
	if (pid == 0) {
		fsync(file_fd);
		_exit(0);
	}
	if (pid < 0)
		fsync(file_fd);
}
