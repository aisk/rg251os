/*
 * PID 1 duties and small system helpers: mounts, status LED, kernel log
 * dumps, child reaping, power off, sysfs access, atomic file writes,
 * backlight, CPU frequency and temperature, persistent settings.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fe.h"

int fe_is_init;
char fe_data_dir[256] = "/mnt/data";
struct settings settings = { .volume = 255, .brightness = 50 };

static void on_sigchld(int sig)
{
	/* Only here so poll() wakes up; children are reaped by sys_reap(). */
	(void)sig;
}

void sys_early_init(void)
{
	struct sigaction sa = { .sa_handler = on_sigchld, .sa_flags = SA_RESTART };
	int fd;

	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (!fe_is_init)
		return;

	mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	/* Normally already mounted by CONFIG_DEVTMPFS_MOUNT (EBUSY). */
	mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");

	/* Make sure fds 0..2 are valid so no device fd ends up as stderr. */
	do
		fd = open("/dev/console", O_RDWR | O_NOCTTY);
	while (fd >= 0 && fd < 2);
	if (fd > 2)
		close(fd);
	while ((fd = open("/dev/null", O_RDWR)) >= 0 && fd < 2)
		;
	if (fd > 2)
		close(fd);

	log_set_kmsg(open("/dev/kmsg", O_WRONLY | O_CLOEXEC));
}

/*
 * The GBA partition holds ROMs, saves and logs, so it is mounted read-write
 * right away. The card was already probed when the root fs was found
 * (rootwait), so the partition is normally there; retry briefly anyway.
 */
int sys_mount_data(void)
{
	static const char *const dev = "/dev/mmcblk0p3";
	int err = 0;

	if (!fe_is_init)
		return 0;
	for (int i = 0; i < 20; i++) {
		if (mount(dev, fe_data_dir, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC |
			  MS_NOATIME, "utf8,errors=remount-ro") == 0) {
			log_msg("mounted %s on %s (vfat, rw)", dev, fe_data_dir);
			return 0;
		}
		err = errno;
		if (err == EBUSY)
			return 0;
		usleep(50000);
	}
	log_msg("mount %s: %s", dev, strerror(err));
	return -1;
}

void sys_make_dirs(void)
{
	static const char *const dirs[] = { "log", "roms", "saves" };
	char path[300];

	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		snprintf(path, sizeof(path), "%s/%s", fe_data_dir, dirs[i]);
		if (mkdir(path, 0755) && errno != EEXIST)
			log_msg("mkdir %s: %s", path, strerror(errno));
	}
}

/* ------------------------------------------------------------------ LED */

static void write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, val, strlen(val)) < 0)
		log_verbose("write %s: %s", path, strerror(errno));
	close(fd);
}

/*
 * The RG351 status LED is one GPIO: driven high it is red, low it is
 * green. Only touched as PID 1 (on a PC this would poke real LEDs).
 */
void led_set_red(int red)
{
	static int triggers_cleared;
	DIR *d;
	struct dirent *e;
	char path[300];

	if (!fe_is_init)
		return;
	d = opendir("/sys/class/leds");
	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (!triggers_cleared) {
			snprintf(path, sizeof(path), "/sys/class/leds/%s/trigger",
				 e->d_name);
			write_str(path, "none");
		}
		snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness",
			 e->d_name);
		write_str(path, red ? "1" : "0");
	}
	triggers_cleared = 1;
	closedir(d);
}

/* ------------------------------------------------------------- klog dump */

void klog_dump(const char *name)
{
	int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
	char path[300], *buf;
	int n, fd;

	if (!fe_is_init)
		return;
	if (size <= 0)
		size = 1 << 17;
	buf = malloc(size);
	if (!buf)
		return;
	n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, size);
	snprintf(path, sizeof(path), "%s/log/%s", fe_data_dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0 && n > 0) {
		if (write(fd, buf, n) != n)
			log_msg("write %s: %s", path, strerror(errno));
		fsync(fd);
	}
	if (fd >= 0)
		close(fd);
	free(buf);
}

/* In a child, so a slow card never stalls a frame. */
void klog_dump_async(const char *name)
{
	pid_t pid;

	if (!fe_is_init)
		return;
	pid = fork();
	if (pid == 0) {
		klog_dump(name);
		_exit(0);
	}
	if (pid < 0)
		klog_dump(name);
}

/* As PID 1 we inherit every orphan, so reap anything that has exited. */
pid_t sys_reap(int *status)
{
	return waitpid(-1, status, WNOHANG);
}

void sys_poweroff(void)
{
	log_msg("powering off");
	log_sync();
	sync();
	if (!fe_is_init)
		exit(0);
	reboot(RB_POWER_OFF);
	/* Still here: no pm_power_off. Halting is better than a PID 1 exit. */
	log_msg("power off failed: %s", strerror(errno));
	log_sync();
	reboot(RB_HALT_SYSTEM);
	for (;;)
		pause();
}

/* ------------------------------------------------------------ file I/O */

int read_file(const char *path, void *buf, size_t max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t got = 0;

	if (fd < 0)
		return -1;
	while (got < max) {
		ssize_t n = read(fd, (char *)buf + got, max - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += n;
	}
	close(fd);
	return (int)got;
}

int read_sysfs(const char *path, char *buf, size_t size)
{
	int n = read_file(path, buf, size - 1);

	if (n < 0) {
		buf[0] = 0;
		return -1;
	}
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = 0;
	return n;
}

long read_sysfs_long(const char *path, long fallback)
{
	char buf[32];

	if (read_sysfs(path, buf, sizeof(buf)) <= 0)
		return fallback;
	return strtol(buf, NULL, 10);
}

/*
 * Write a file so that a pulled card or power loss leaves either the old
 * or the new version: temp file, fsync, rename, fsync the directory.
 */
int write_file_atomic(const char *path, const void *data, size_t len)
{
	char tmp[320], dir[320];
	const char *p = data;
	size_t done = 0;
	char *slash;
	int fd, dfd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		goto fail;
	while (done < len) {
		ssize_t n = write(fd, p + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n == 0)
				errno = ENOSPC;
			close(fd);
			goto fail_unlink;
		}
		done += n;
	}
	if (fsync(fd)) {
		close(fd);
		goto fail_unlink;
	}
	if (close(fd))
		goto fail_unlink;
	if (rename(tmp, path))
		goto fail_unlink;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dfd >= 0) {
			fsync(dfd);
			close(dfd);
		}
	}
	return 0;

fail_unlink:
	{
		int e = errno;

		unlink(tmp);
		errno = e;
	}
fail:
	log_msg("write %s: %s", path, strerror(errno));
	return -1;
}

/* ------------------------------------------------------------ backlight */

static char bl_dir[300];
static long bl_max;

void backlight_init(int percent)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	char path[400];

	if (!d) {
		log_msg("backlight: no /sys/class/backlight");
		return;
	}
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(bl_dir, sizeof(bl_dir), "/sys/class/backlight/%s",
			 e->d_name);
		break;
	}
	closedir(d);
	if (!bl_dir[0]) {
		log_msg("backlight: no device");
		return;
	}
	snprintf(path, sizeof(path), "%s/max_brightness", bl_dir);
	bl_max = read_sysfs_long(path, 0);
	snprintf(path, sizeof(path), "%s/brightness", bl_dir);
	log_msg("backlight %s: max_brightness %ld, brightness was %ld",
		bl_dir, bl_max, read_sysfs_long(path, -1));
	if (bl_max <= 0) {
		bl_dir[0] = 0;
		return;
	}
	backlight_set(percent);
}

void backlight_set(int percent)
{
	char path[400], val[24];
	long v;

	if (!bl_dir[0] || !fe_is_init)
		return;
	if (percent < 5)
		percent = 5;
	if (percent > 100)
		percent = 100;
	v = (bl_max * percent + 50) / 100;
	if (v < 1)
		v = 1;
	snprintf(path, sizeof(path), "%s/brightness", bl_dir);
	snprintf(val, sizeof(val), "%ld", v);
	write_str(path, val);
	log_verbose("backlight %d%% = %ld/%ld", percent, v, bl_max);
}

/* ---------------------------------------------------- CPU and thermals */

void cpu_info_log(void)
{
	static const char *const files[] = {
		"scaling_governor", "scaling_available_governors",
		"scaling_available_frequencies", "scaling_min_freq",
		"scaling_max_freq", "scaling_cur_freq",
	};
	char path[200], buf[256];

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cpufreq/%s", files[i]);
		read_sysfs(path, buf, sizeof(buf));
		log_msg("cpu0 %s: %s", files[i], buf[0] ? buf : "-");
	}
	read_sysfs("/sys/class/thermal/thermal_zone0/type", buf, sizeof(buf));
	log_msg("thermal_zone0 %s: %ld mC", buf[0] ? buf : "-", soc_temp_mc());
}

long cpu_freq_khz(void)
{
	return read_sysfs_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", -1);
}

long soc_temp_mc(void)
{
	return read_sysfs_long("/sys/class/thermal/thermal_zone0/temp", -1);
}

/* ------------------------------------------------------------- settings */

int settings_parse(struct settings *s, const char *text)
{
	int n = 0;

	while (*text) {
		char key[32];
		long v;
		int len = 0;

		if (sscanf(text, "%31[a-z_]=%ld%n", key, &v, &len) == 2 && len) {
			if (!strcmp(key, "volume") && v >= 0 && v <= 255) {
				s->volume = (int)v;
				n++;
			} else if (!strcmp(key, "brightness") && v >= 5 && v <= 100) {
				s->brightness = (int)v;
				n++;
			}
		}
		text = strchr(text, '\n');
		if (!text)
			break;
		text++;
	}
	return n;
}

static void settings_path(char *buf, size_t size)
{
	snprintf(buf, size, "%s/settings.txt", fe_data_dir);
}

void settings_load(void)
{
	char path[300], buf[512];
	int n;

	settings_path(path, sizeof(path));
	n = read_file(path, buf, sizeof(buf) - 1);
	if (n < 0)
		return;
	buf[n] = 0;
	settings_parse(&settings, buf);
	log_msg("settings: volume %d, brightness %d%%", settings.volume,
		settings.brightness);
}

void settings_save(void)
{
	char path[300], buf[128];
	int n;

	settings_path(path, sizeof(path));
	n = snprintf(buf, sizeof(buf), "volume=%d\nbrightness=%d\n",
		     settings.volume, settings.brightness);
	write_file_atomic(path, buf, n);
}
