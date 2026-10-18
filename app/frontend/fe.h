/*
 * Shared declarations for the frontend: time, logging, system helpers.
 */
#ifndef FE_H
#define FE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FE_PRINTF(a, b) __attribute__((format(printf, a, b)))

/* Set when running as PID 1; most system side effects depend on it. */
extern int fe_is_init;
/* Root of the GBA partition: /mnt/data, or a directory given on the
 * command line when testing on a PC. */
extern char fe_data_dir[256];

int64_t now_ns(void);   /* CLOCK_MONOTONIC */
int64_t boot_ms(void);  /* CLOCK_BOOTTIME, the clock dmesg uses */

/* ---- log.c: frontend.txt on the GBA partition plus /dev/kmsg */
void log_set_kmsg(int fd);
void log_open_file(const char *path);
/* Important lines: to the log file and the kernel log. */
void log_msg(const char *fmt, ...) FE_PRINTF(1, 2);
/* Chatty lines (input traces, mixer dumps): log file only. */
void log_verbose(const char *fmt, ...) FE_PRINTF(1, 2);
void log_sync(void);
void log_sync_async(void);

/* ---- sys.c */
void sys_early_init(void);
int sys_mount_data(void);
void sys_make_dirs(void);
void led_set_red(int red);
void klog_dump(const char *name);
void klog_dump_async(const char *name);
pid_t sys_reap(int *status);
void sys_poweroff(void) __attribute__((noreturn));
int read_file(const char *path, void *buf, size_t max);
int read_sysfs(const char *path, char *buf, size_t size);
long read_sysfs_long(const char *path, long fallback);
int write_file_atomic(const char *path, const void *data, size_t len);
void backlight_init(int percent);
void backlight_set(int percent);
void cpu_info_log(void);
/* Same governor on all CPUs (they share one clock on the RK3326). */
void cpu_set_governor(const char *gov);
long cpu_freq_khz(void);
long soc_temp_mc(void);

/* ---- settings kept in <data>/settings.txt */
struct settings {
	int volume;      /* raw "Master Playback Volume", 0..255 */
	int brightness;  /* backlight percent, 5..100 */
};
extern struct settings settings;
void settings_load(void);
void settings_save(void);
int settings_parse(struct settings *s, const char *text);

#endif
