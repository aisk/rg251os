/*
 * Logging. Every line gets a CLOCK_BOOTTIME timestamp so it lines up with
 * dmesg. Until the GBA partition is mounted, lines are kept in memory and
 * written out when the log file opens. Important lines also go to
 * /dev/kmsg, so they show up in the dmesg dumps as well.
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

	va_start(ap, fmt);
	vlog(0, fmt, ap);
	va_end(ap);
}

/*
 * Keep the previous run's log next to the new one: when something goes
 * wrong the user often reboots before copying the card.
 */
void log_open_file(const char *path)
{
	char prev[300];
	const char *dot = strrchr(path, '.');
	int fd;

	snprintf(prev, sizeof(prev), "%.*s-prev%s",
		 dot ? (int)(dot - path) : (int)strlen(path), path, dot ? dot : "");
	rename(path, prev);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
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
