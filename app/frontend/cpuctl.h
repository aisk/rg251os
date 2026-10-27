#ifndef CPUCTL_H
#define CPUCTL_H

#include <stddef.h>
#include <stdint.h>

#define CPU_MAX_OPPS 8

/*
 * In game CPU frequency control (see cpuctl.c). root is the cpufreq sysfs
 * directory of the policy, NULL for /sys/devices/system/cpu/cpu0/cpufreq.
 * A non-NULL root is a fake directory for tests (see cpuctl_fake_sysfs):
 * the actuator is then simulated and its effect written to
 * scaling_cur_freq there.
 */
void cpuctl_init(const char *root);
/* Creates a fake cpufreq directory with the RK3326 OPPs. */
int cpuctl_fake_sysfs(const char *dir);
int cpuctl_opp_count(void);
long cpuctl_opp_khz(int i);
long cpu_freq_khz(void);  /* scaling_cur_freq, -1 if unknown */

/* settings.cpu: CPU_MODE_ADAPTIVE, CPU_MODE_MAX or a fixed MHz. Live. */
void cpuctl_set_mode(int mode);
/* Next/previous mode in menu order: adaptive, max, then the lower OPPs. */
int cpuctl_mode_step(int mode, int d);
/* "Adaptive", "Max", "816MHz" */
void cpuctl_mode_label(int mode, char *buf, size_t size);

/* A game session: start (start_mhz: learned start OPP, 0 for the top),
 * pause/resume around menus, one call per game frame with its work time,
 * the vblanks missed since the previous call and whether the core skipped
 * it (frameskip: counted, not judged), end. */
void cpuctl_game_start(const char *rom_name, double refresh_hz, int start_mhz,
		       int64_t now);
void cpuctl_game_pause(void);
void cpuctl_game_resume(void);
void cpuctl_game_frame(int64_t work_ns, unsigned missed, int skipped,
		       int64_t now);
/* Logs the summary and appends to <data>/log/perf.csv; returns the start
 * OPP in MHz to remember for this ROM (0: the top). */
int cpuctl_game_end(void);
int cpuctl_in_game(void);
/* Light sleep: clock capped at the lowest OPP until called with 0. */
void cpuctl_sleep(int on);

/* State for the perf line. */
void cpuctl_format(char *buf, int size);

/* For tests. */
struct cpuctl_counters {
	unsigned changes, ups, downs, jumps;
	unsigned missed, missed_cpu;
	unsigned skipped;  /* frames skipped by the core */
	int64_t hold_ns;  /* current hold after a step up */
};
void cpuctl_counters(struct cpuctl_counters *k);

#endif
