/*
 * Suspend to RAM for the power key's sleep (see main.c for the frontend
 * side: what is paused before and restored after).
 *
 * Kernel side on the RG351P: CONFIG_SUSPEND with PSCI SYSTEM_SUSPEND
 * from the rkbin BL31 (PSCI 1.1), which registers "mem" in
 * /sys/power/state with mem_sleep "deep". The RK817 PMIC interrupt
 * (GPIO0_PB2) is the wakeup IRQ (wakeup-source in the RG351M dtsi), so the
 * power key (and the charger) wake the system; the BL31 itself logs the
 * wakeup cause on the serial console only.
 *
 * Proof of a real suspend, all in frontend.txt (log_msg, kept without
 * debug logging): CLOCK_MONOTONIC stops while suspended and
 * CLOCK_BOOTTIME does not, so "suspended N ms" (the difference across the
 * state write) is the time really spent in suspend; suspend_stats/success
 * counts up by one. A write that fails, or returns with neither, did not
 * suspend; the caller then falls back to a light sleep.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "fe.h"
#include "sleep.h"

#define SUSPEND_ATTEMPTS 3
#define RETRY_US 200000
/* Less than this in suspend (and no success count) is no suspend. */
#define MIN_SUSPENDED_MS 100

static int64_t clock_ms(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Returns 0 or an errno. */
static int write_attr(const char *dir, const char *name, const char *val)
{
	char path[640];
	size_t len = strlen(val);
	ssize_t n;
	int fd, err = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return errno;
	n = write(fd, val, len);
	if (n < 0)
		err = errno;
	else if ((size_t)n != len)
		err = EIO;
	if (close(fd) && !err)
		err = errno;
	return err;
}

static int read_attr(const char *dir, const char *name, char *buf, size_t size)
{
	char path[640];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return read_sysfs(path, buf, size);
}

/*
 * Reading wakeup_count blocks while a wakeup event is in progress, which
 * would leave the device dark and stuck; read it in a child with a
 * timeout. Returns the length, or -1 (buf says why).
 */
static int read_wakeup_count(const char *dir, char *buf, size_t size)
{
	struct pollfd p;
	int fd[2], n = -1;
	pid_t pid;

	if (pipe(fd))
		return read_attr(dir, "wakeup_count", buf, size);
	pid = fork();
	if (pid == 0) {
		char v[32];
		int len = read_attr(dir, "wakeup_count", v, sizeof(v));

		if (len > 0 && write(fd[1], v, len) < 0)
			_exit(1);
		_exit(0);
	}
	close(fd[1]);
	if (pid < 0) {
		close(fd[0]);
		return read_attr(dir, "wakeup_count", buf, size);
	}
	p.fd = fd[0];
	p.events = POLLIN;
	buf[0] = 0;
	if (poll(&p, 1, 500) > 0) {
		n = (int)read(fd[0], buf, size - 1);
		if (n > 0)
			buf[n] = 0;
		else
			n = -1;
	} else {
		snprintf(buf, size, "blocked");
		kill(pid, SIGKILL);
	}
	close(fd[0]);
	waitpid(pid, NULL, 0);
	return n < 0 ? -1 : n;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

long sleep_stats(const char *power_dir, char *buf, int size)
{
	char path[320], val[64], *names[48];
	int n = 0, len = 0;
	long success = -1;
	struct dirent *e;
	DIR *d;

	buf[0] = 0;
	snprintf(path, sizeof(path), "%s/suspend_stats", power_dir);
	d = opendir(path);
	if (!d) {
		snprintf(buf, size, "no %s", path);
		return -1;
	}
	while ((e = readdir(d)) && n < (int)ARRAY_SIZE(names))
		if (e->d_name[0] != '.')
			names[n++] = strdup(e->d_name);
	closedir(d);
	qsort(names, n, sizeof(names[0]), cmp_str);
	for (int i = 0; i < n; i++) {
		if (!names[i])
			continue;
		if (read_attr(path, names[i], val, sizeof(val)) < 0)
			snprintf(val, sizeof(val), "?");
		/* last_failed_dev is often empty */
		if (len < size)
			len += snprintf(buf + len, size - len, "%s%s=%s",
					len ? " " : "", names[i], val[0] ? val : "-");
		if (!strcmp(names[i], "success"))
			success = strtol(val, NULL, 10);
		free(names[i]);
	}
	return success;
}

void sleep_suspend(const char *power_dir, unsigned n, struct sleep_result *r)
{
	char state[64], mem_sleep[64], stats[640], wc[32], irq[32];
	long before, after;
	int64_t b0, m0;
	int err, deep = 0;

	memset(r, 0, sizeof(*r));
	r->success_delta = -1;
	snprintf(r->method, sizeof(r->method), "none");
	if (!fe_is_init && !strcmp(power_dir, "/sys/power")) {
		log_msg("sleep #%u: not PID 1, no suspend to RAM", n);
		r->err = EPERM;
		return;
	}
	read_attr(power_dir, "state", state, sizeof(state));
	read_attr(power_dir, "mem_sleep", mem_sleep, sizeof(mem_sleep));
	/* deep is the default when offered; make sure, it is what saves power */
	if (strstr(mem_sleep, "deep") && !strstr(mem_sleep, "[deep]")) {
		err = write_attr(power_dir, "mem_sleep", "deep");
		log_msg("sleep #%u: mem_sleep deep: %s", n, err ? strerror(err) : "set");
		deep = !err;
		read_attr(power_dir, "mem_sleep", mem_sleep, sizeof(mem_sleep));
	}
	snprintf(r->method, sizeof(r->method), "mem (%s)",
		 deep || strstr(mem_sleep, "[deep]") ? "deep" :
		 strstr(mem_sleep, "[s2idle]") ? "s2idle" : "?");
	log_msg("sleep #%u: state '%s', mem_sleep '%s', trying %s", n,
		state[0] ? state : "-", mem_sleep[0] ? mem_sleep : "-", r->method);
	if (!strstr(state, "mem")) {
		log_msg("sleep #%u: no \"mem\" in %s/state, no suspend to RAM", n,
			power_dir);
		r->err = ENOTSUP;
		return;
	}
	before = sleep_stats(power_dir, stats, sizeof(stats));
	log_msg("sleep #%u: suspend_stats before: %s", n, stats);
	if (log_debug()) {
		/* Kernel side detail for the dmesg dump after resume. */
		write_attr(power_dir, "pm_debug_messages", "1");
		write_attr(power_dir, "pm_print_times", "1");
	}

	for (r->attempts = 1; ; r->attempts++) {
		/*
		 * wakeup_count handshake: writing back the count just read
		 * makes the kernel abort the suspend (EBUSY) if a wakeup event
		 * arrives in between, instead of sleeping through it.
		 */
		if (read_wakeup_count(power_dir, wc, sizeof(wc)) > 0) {
			err = write_attr(power_dir, "wakeup_count", wc);
			log_msg("sleep #%u: wakeup_count %s (%s)", n, wc,
				err ? strerror(err) : "armed");
		} else {
			log_msg("sleep #%u: wakeup_count %s, not armed", n,
				wc[0] ? wc : "unreadable");
		}
		log_msg("sleep #%u: writing mem to %s/state (attempt %d)", n,
			power_dir, r->attempts);
		log_sync();
		sync();
		b0 = clock_ms(CLOCK_BOOTTIME);
		m0 = clock_ms(CLOCK_MONOTONIC);
		r->err = write_attr(power_dir, "state", "mem");
		r->boot_ms = clock_ms(CLOCK_BOOTTIME) - b0;
		r->mono_ms = clock_ms(CLOCK_MONOTONIC) - m0;
		log_msg("sleep #%u: state write returned %s (errno %d) after "
			"CLOCK_BOOTTIME +%lld ms, CLOCK_MONOTONIC +%lld ms, "
			"suspended %lld ms", n, r->err ? strerror(r->err) : "ok",
			r->err, (long long)r->boot_ms, (long long)r->mono_ms,
			(long long)(r->boot_ms - r->mono_ms));
		if (r->err != EBUSY || r->attempts >= SUSPEND_ATTEMPTS)
			break;
		usleep(RETRY_US);
	}

	after = sleep_stats(power_dir, stats, sizeof(stats));
	log_msg("sleep #%u: suspend_stats after: %s", n, stats);
	if (read_attr(power_dir, "pm_wakeup_irq", irq, sizeof(irq)) < 0)
		snprintf(irq, sizeof(irq), "n/a");
	read_wakeup_count(power_dir, wc, sizeof(wc));
	log_msg("sleep #%u: pm_wakeup_irq %s, wakeup_count now %s", n,
		irq[0] ? irq : "-", wc[0] ? wc : "-");
	if (before >= 0 && after >= 0)
		r->success_delta = after - before;
	r->slept = !r->err && (r->success_delta > 0 ||
			       r->boot_ms - r->mono_ms >= MIN_SUSPENDED_MS);
	if (r->slept)
		log_msg("sleep #%u: result: suspended to RAM, %s, %lld ms in "
			"suspend (suspend_stats success %+ld)", n, r->method,
			(long long)(r->boot_ms - r->mono_ms), r->success_delta);
	else
		log_msg("sleep #%u: result: no suspend to RAM (%s, %lld ms in "
			"suspend, suspend_stats success %+ld)", n,
			r->err ? strerror(r->err) : "write returned without "
			"suspending", (long long)(r->boot_ms - r->mono_ms),
			r->success_delta);
}
