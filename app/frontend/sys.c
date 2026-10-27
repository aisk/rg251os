/*
 * PID 1 duties and small system helpers: mounts, per boot log
 * directories, status LED, kernel log dumps, child reaping, power off
 * (with the GBA partition unmounted first), file and sysfs access, atomic
 * file writes, backlight, CPU info and temperature.
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

#define DATA_DEV "/dev/mmcblk0p3"
#define DATA_FLAGS (MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME)
#define DATA_OPTS "utf8,errors=remount-ro"
/* What the FAT driver says when it mounts a volume whose dirty bit is
 * set: it was not unmounted (or remounted read-only) at some point. */
#define FAT_DIRTY_MSG "Volume was not properly unmounted"

int fe_is_init;
char fe_data_dir[256] = "/mnt/data";
char fe_log_dir[300] = "/mnt/data/log";

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

/* The whole kernel log buffer, NUL terminated (free it); NULL on error. */
static char *klog_read(int *len)
{
	int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
	char *buf;
	int n;

	if (size <= 0)
		size = 1 << 17;
	buf = malloc(size + 1);
	if (!buf)
		return NULL;
	n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, size);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	*len = n;
	return buf;
}

/*
 * The GBA partition holds ROMs, saves and logs, so it is mounted read-write
 * right away. The card was already probed when the root fs was found
 * (rootwait), so the partition is normally there; retry briefly anyway.
 * Whether the FAT dirty bit was set is logged: it is set while mounted
 * and cleared by a clean unmount (sys_poweroff), but the kernel leaves a
 * bit it found set at mount alone, so once set it stays until a repair
 * (Windows, fsck.fat).
 */
int sys_mount_data(void)
{
	int err = 0, len;
	char *klog;

	if (!fe_is_init)
		return 0;
	for (int i = 0; i < 20; i++) {
		if (mount(DATA_DEV, fe_data_dir, "vfat", DATA_FLAGS, DATA_OPTS) == 0)
			break;
		err = errno;
		if (err == EBUSY)
			return 0;
		if (i == 19) {
			log_msg("mount %s: %s", DATA_DEV, strerror(err));
			return -1;
		}
		usleep(50000);
	}
	klog = klog_read(&len);
	log_msg("mounted %s on %s (vfat, rw), %s", DATA_DEV, fe_data_dir,
		!klog ? "dirty bit unknown" : strstr(klog, FAT_DIRTY_MSG) ?
		"dirty bit set: not cleanly unmounted at some point (power "
		"lost, card pulled or a failed unmount; Linux never clears "
		"it, a repair on a PC does)" :
		"clean (unmounted at the last power off)");
	free(klog);
	return 0;
}

void sys_make_dirs(void)
{
	static const char *const dirs[] = {
		"log", "roms", "saves", "config", "config/roms",
	};
	char path[300];

	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		snprintf(path, sizeof(path), "%s/%s", fe_data_dir, dirs[i]);
		if (mkdir(path, 0755) && errno != EEXIST)
			log_msg("mkdir %s: %s", path, strerror(errno));
	}
	snprintf(fe_log_dir, sizeof(fe_log_dir), "%s/log", fe_data_dir);
}

/* Remove a directory and the plain files in it. */
static void rm_flat_dir(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	char f[900];

	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(f, sizeof(f), "%s/%s", path, e->d_name);
		unlink(f);
	}
	closedir(d);
	rmdir(path);
}

/*
 * One directory per boot: <data>/log/boot-NNNN holds that boot's
 * frontend.txt and dmesg dumps, so a session is never overwritten by the
 * boots after it. The counter lives in log/boot-counter.txt; the newest
 * LOG_KEEP_BOOTS directories are kept; log/LATEST.txt names the current
 * one. perf.csv stays in log/ (appended across boots).
 */
#define LOG_KEEP_BOOTS 20

unsigned sys_log_init(void)
{
	char path[300], buf[32], name[16];
	unsigned n = 0;
	DIR *d;
	struct dirent *e;
	int len;

	snprintf(path, sizeof(path), "%s/log/boot-counter.txt", fe_data_dir);
	len = read_file(path, buf, sizeof(buf) - 1);
	if (len > 0) {
		buf[len] = 0;
		n = (unsigned)strtoul(buf, NULL, 10);
	}
	n++;
	len = snprintf(buf, sizeof(buf), "%u\n", n);
	write_file_atomic(path, buf, (size_t)len);

	snprintf(name, sizeof(name), "boot-%04u", n % 10000);
	snprintf(fe_log_dir, sizeof(fe_log_dir), "%s/log/%s", fe_data_dir, name);
	if (mkdir(fe_log_dir, 0755) && errno != EEXIST) {
		log_msg("mkdir %s: %s", fe_log_dir, strerror(errno));
		snprintf(fe_log_dir, sizeof(fe_log_dir), "%s/log", fe_data_dir);
		return n;
	}
	snprintf(path, sizeof(path), "%s/log/LATEST.txt", fe_data_dir);
	len = snprintf(buf, sizeof(buf), "%s\n", name);
	write_file_atomic(path, buf, (size_t)len);

	snprintf(path, sizeof(path), "%s/log", fe_data_dir);
	d = opendir(path);
	while (d && (e = readdir(d))) {
		unsigned k;
		char *end;

		if (strncmp(e->d_name, "boot-", 5))
			continue;
		k = (unsigned)strtoul(e->d_name + 5, &end, 10);
		if (*end || end == e->d_name + 5)
			continue;
		/* Numbers wrap at 10000: compare modulo, keep the newest. */
		if (k >= 10000 || (n % 10000 + 10000 - k) % 10000 >= LOG_KEEP_BOOTS) {
			char old[600];

			snprintf(old, sizeof(old), "%s/%s", path, e->d_name);
			rm_flat_dir(old);
		}
	}
	if (d)
		closedir(d);
	return n;
}

/* ------------------------------------------------------------------ LED */

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
			write_sysfs(path, "none");
		}
		snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness",
			 e->d_name);
		write_sysfs(path, red ? "1" : "0");
	}
	triggers_cleared = 1;
	closedir(d);
}

/* ------------------------------------------------------------- klog dump */

/*
 * The kernel log buffer into <log dir>/name, from the last line containing
 * marker on (everything when marker is NULL or gone).
 */
void klog_dump_since(const char *name, const char *marker)
{
	char path[400], *buf, *from, *p;
	int n, fd;

	if (!fe_is_init)
		return;
	buf = klog_read(&n);
	if (!buf)
		return;
	from = buf;
	for (p = buf; marker && (p = strstr(p, marker)); p++)
		from = p;
	while (from > buf && from[-1] != '\n')
		from--;
	snprintf(path, sizeof(path), "%s/%s", fe_log_dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		ssize_t len = buf + n - from;

		if (len && write(fd, from, len) != len)
			log_msg("write %s: %s", path, strerror(errno));
		fsync(fd);
		close(fd);
	}
	free(buf);
}

void klog_dump(const char *name)
{
	klog_dump_since(name, NULL);
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

/*
 * Before the unmount: children (background saves, log fsyncs, kernel log
 * dumps) may still hold files on the GBA partition open.
 */
void sys_wait_children(int timeout_ms)
{
	int64_t end = now_ns() + timeout_ms * 1000000LL;
	unsigned reaped = 0;
	pid_t pid;
	int st;

	for (;;) {
		pid = waitpid(-1, &st, WNOHANG);
		if (pid > 0) {
			reaped++;
			continue;
		}
		if (pid < 0 || now_ns() >= end)
			break;
		usleep(10000);
	}
	if (pid < 0 && errno == ECHILD)
		log_msg("power off: no child processes left (%u reaped)", reaped);
	else
		log_msg("power off: child processes still running after %d ms "
			"(%u reaped)", timeout_ms, reaped);
}

/*
 * vfat keeps its dirty bit set on the card until a clean unmount (or a
 * remount read-only), and Windows offers to repair a card with it set.
 * So the log file is closed and the GBA partition unmounted, or if
 * something still has a file open there (EBUSY), remounted read-only.
 * The outcome only reaches the kernel log, which does not survive the
 * power off; the next boot's mount line tells whether it worked (see
 * sys_mount_data). Not PID 1: nothing is unmounted.
 */
void sys_poweroff(void)
{
	log_msg("powering off: unmounting %s", fe_data_dir);
	log_close_file();
	sync();
	if (!fe_is_init)
		exit(0);
	if (umount2(fe_data_dir, 0) == 0) {
		log_msg("power off: %s unmounted", fe_data_dir);
	} else {
		log_msg("power off: umount %s: %s, remounting read-only",
			fe_data_dir, strerror(errno));
		if (mount(DATA_DEV, fe_data_dir, "vfat",
			  MS_REMOUNT | MS_RDONLY | DATA_FLAGS, DATA_OPTS) == 0)
			log_msg("power off: %s remounted read-only", fe_data_dir);
		else
			log_msg("power off: remount %s read-only: %s",
				fe_data_dir, strerror(errno));
	}
	sync();
	reboot(RB_POWER_OFF);
	/* Still here: no pm_power_off. Halting is better than a PID 1 exit. */
	log_msg("power off failed: %s", strerror(errno));
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

int read_attr(const char *dir, const char *name, char *buf, size_t size)
{
	char path[640];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return read_sysfs(path, buf, size);
}

long read_attr_long(const char *dir, const char *name, long fallback)
{
	char path[640];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return read_sysfs_long(path, fallback);
}

int write_sysfs(const char *path, const char *val)
{
	size_t len = strlen(val);
	ssize_t n;
	int fd, err = 0;

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

int write_attr(const char *dir, const char *name, const char *val)
{
	char path[640];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return write_sysfs(path, val);
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
	log_msg("backlight %s: max_brightness %ld, brightness was %ld, "
		"user minimum %d%% = %ld", bl_dir, bl_max,
		read_sysfs_long(path, -1), BRIGHTNESS_MIN,
		(bl_max * BRIGHTNESS_MIN + 50) / 100);
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
	if (percent < BRIGHTNESS_MIN)
		percent = BRIGHTNESS_MIN;
	if (percent > 100)
		percent = 100;
	v = (bl_max * percent + 50) / 100;
	if (v < 1)
		v = 1;
	snprintf(path, sizeof(path), "%s/brightness", bl_dir);
	snprintf(val, sizeof(val), "%ld", v);
	write_sysfs(path, val);
	log_verbose("backlight %d%% = %ld/%ld", percent, v, bl_max);
}

/* Backlight fully off (sleep); backlight_set() turns it back on. */
void backlight_off(void)
{
	char path[400];

	if (!bl_dir[0] || !fe_is_init)
		return;
	snprintf(path, sizeof(path), "%s/brightness", bl_dir);
	write_sysfs(path, "0");
	log_verbose("backlight off");
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

long soc_temp_mc(void)
{
	return read_sysfs_long("/sys/class/thermal/thermal_zone0/temp", -1);
}
