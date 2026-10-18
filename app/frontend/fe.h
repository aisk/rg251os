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
/* This boot's log directory, <data>/log/boot-NNNN (sys_log_init), or
 * <data>/log before that or when testing on a PC. */
extern char fe_log_dir[300];

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
/* Debug logging (settings.debug): log_verbose lines, periodic perf and
 * battery lines, cpufreq changes, kernel/register/mixer dumps. On until
 * the settings are loaded; log_set_debug logs a change. */
void log_set_debug(int on);
int log_debug(void);

/* ---- sys.c */
void sys_early_init(void);
int sys_mount_data(void);
void sys_make_dirs(void);
/* New per boot log directory (fe_log_dir); returns the boot number. */
unsigned sys_log_init(void);
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
long soc_temp_mc(void);
/* pm_genpd_summary from debugfs into the log (best effort). */
void genpd_log(const char *when);

/* ---- settings.c: key=value text files in <data>/config */
enum color_mode {
	COLOR_OFF,
	COLOR_GBA,        /* original GBA, unlit reflective screen */
	COLOR_GBA_LIGHT,  /* same colors without the extra gamma darkening */
	COLOR_SP101,      /* backlit GBA SP (AGS-101) */
	COLOR_COUNT
};
extern const char *const color_mode_names[COLOR_COUNT];   /* in files */
extern const char *const color_mode_labels[COLOR_COUNT];  /* in menus */

/* The libretro cores linked in (core.c), selectable per game. */
enum core_id {
	CORE_MGBA,
	CORE_GPSP,  /* gpSP, ARM64 dynarec: much faster, less accurate */
	CORE_COUNT
};
extern const char *const core_names[CORE_COUNT];   /* in files */
extern const char *const core_labels[CORE_COUNT];  /* in menus */

/* settings.cpu, otherwise a fixed frequency in MHz (see cpuctl.c) */
#define AUDIO_SIMPLE 0    /* 48 kHz S16 512x8, blocking writes, fixed ratio */
#define AUDIO_ADAPTIVE 1  /* calibrated, nonblocking, rate control */
#define CPU_MODE_ADAPTIVE 0
#define CPU_MODE_MAX 1

/* Global, in config/settings.txt. */
struct settings {
	int volume;      /* raw "Master Playback Volume", 0..255 */
	int brightness;  /* backlight percent, 5..100 */
	int grid;        /* LCD pixel grid effect on/off */
	int grid_dim;    /* darkening of the 3 unlit pixels of a cell, percent */
	int grid_boost;  /* brightening of the lit pixel, percent */
	int cpu;         /* CPU_MODE_ADAPTIVE, CPU_MODE_MAX or MHz */
	int audio;       /* AUDIO_SIMPLE or AUDIO_ADAPTIVE (audio.c) */
	int debug;       /* debug logging (see log.c), off for normal play */
};
/* Per game, in config/roms/<rom name>.txt. */
struct rom_settings {
	int color;       /* enum color_mode */
	int cpu_start;   /* learned start OPP in MHz, 0: the top */
	int core;        /* enum core_id, used when the game starts */
};
extern struct settings settings;
extern struct rom_settings rom_settings;  /* for the running game */
extern const struct settings settings_defaults;

void settings_load(void);
void settings_save(void);
int settings_parse(struct settings *s, const char *text);
int settings_format(const struct settings *s, char *buf, size_t size);
/* Defaults for a ROM, then its file. NULL: no game (defaults only). */
void rom_settings_load(const char *rom_name);
void rom_settings_save(void);
/* Where a per ROM default comes from (today the same for every ROM). */
void rom_settings_defaults(const char *rom_name, struct rom_settings *r);
int rom_settings_parse(struct rom_settings *r, const char *text);
int rom_settings_format(const struct rom_settings *r, char *buf, size_t size);

#endif
