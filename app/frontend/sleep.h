#ifndef SLEEP_H
#define SLEEP_H

#include <stdint.h>

/* What sleep_suspend() saw, for the caller's wake log line. */
struct sleep_result {
	int slept;            /* the system really suspended */
	int err;              /* errno of the last state write, 0 on success */
	int attempts;
	int64_t boot_ms;      /* CLOCK_BOOTTIME across the state write */
	int64_t mono_ms;      /* CLOCK_MONOTONIC across it (stops in suspend) */
	long success_delta;   /* suspend_stats/success, -1 if unreadable */
	char method[24];      /* "mem (deep)", "mem (s2idle)", ... */
};

/*
 * Suspend to RAM through <power_dir>/state (normally /sys/power), with
 * mem_sleep set to deep when offered and the wakeup_count handshake, and
 * log everything that shows whether it really slept: suspend_stats before
 * and after, pm_wakeup_irq, wakeup_count, both clocks. EBUSY (a wakeup
 * event raced the suspend) is retried twice. The log is synced before
 * the write. A fake power_dir (plain files) is accepted for tests; the
 * real one is only touched as PID 1.
 */
void sleep_suspend(const char *power_dir, unsigned n, struct sleep_result *r);

/* "a=1 b=2 ..." from <power_dir>/suspend_stats; returns success or -1. */
long sleep_stats(const char *power_dir, char *buf, int size);
/* A /sys/power that offers mem but never suspends, for the headless test. */
int sleep_fake_sysfs(const char *dir);

#endif
