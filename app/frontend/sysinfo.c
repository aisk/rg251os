/*
 * System info page, opened from the ROM list's settings and from the
 * in-game menu (the game stays paused meanwhile, as in every menu). Read
 * only, refreshed once per second (and drawn then and as the seconds of
 * the clock and the uptime tick), B goes back. Shows what the RG351P
 * kernel exposes: CPU usage (total and per core, /proc/stat deltas; the
 * first second shows the average since boot), the current frequency of
 * each cpufreq policy and the CPU mode (cpuctl.c), memory used of total
 * (MemTotal - MemAvailable), every thermal zone (the PX30 tsadc gives
 * soc-thermal and gpu-thermal) and hwmon temperature, the battery
 * (battery.c), the date and time (the system clock as UTC, "Not set"
 * before 2024, see clock.c), uptime (CLOCK_BOOTTIME), free of total space
 * on the GBA partition and the kernel release. Rows are one font height
 * apart with the B:back hint in the title bar, which leaves room for 11
 * (what the device shows); rows past the bottom are left out.
 *
 * Log: when the page opens, "sysinfo: ..." lines with all the values and
 * which sensors were found, so one run on the device shows them.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#include "battery.h"
#include "cpuctl.h"
#include "fe.h"
#include "input.h"
#include "menu.h"
#include "settings.h"
#include "sysinfo.h"

#define REFRESH_NS 1000000000LL
#define MAX_TEMPS 6
#define MAX_POLICIES 4
#define VAL_COLS(c) (((c)->w - SYSINFO_VAL_X - 8) / FONT_W)

struct temp {
	char name[32];
	long mc;
};

static struct {
	char root[200];
	struct cpu_times prev[SYSINFO_MAX_CPUS + 1], cur[SYSINFO_MAX_CPUS + 1];
	int ncpu;      /* entries in cur: the total and the cores */
	int usage[SYSINFO_MAX_CPUS + 1];
	long khz[MAX_POLICIES];
	int npol;
	long mem_total, mem_avail;  /* kB, -1 unknown */
	struct temp temps[MAX_TEMPS];
	int ntemps;
	struct battery bat;
	unsigned long long data_free, data_size;  /* bytes, 0 unknown */
	char kernel[65];
	int64_t next;
} si;

/* ------------------------------------------------------------- parsing */

int sysinfo_parse_stat(const char *text, struct cpu_times *t, int max)
{
	int n = 0, total = 0;

	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : p) {
		unsigned long long v[10] = { 0 };
		int idx, k;
		char *end;

		if (strncmp(p, "cpu", 3))
			continue;
		if (p[3] == ' ') {
			idx = 0;
			end = (char *)p + 3;
		} else {
			idx = (int)strtol(p + 3, &end, 10) + 1;
			if (end == p + 3 || *end != ' ' || idx < 1 || idx >= max)
				continue;
		}
		for (k = 0; k < 10; k++) {
			char *q;

			v[k] = strtoull(end, &q, 10);
			if (q == end)
				break;
			end = q;
		}
		if (k < 4)
			continue;
		/* user nice system idle iowait irq softirq steal (guest time
		 * is counted in user already) */
		t[idx].busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
		t[idx].total = t[idx].busy + v[3] + v[4];
		total |= idx == 0;
		if (idx >= n)
			n = idx + 1;
	}
	return total ? n : 0;
}

int sysinfo_usage(const struct cpu_times *a, const struct cpu_times *b)
{
	unsigned long long dt = b->total - a->total, db = b->busy - a->busy;

	if (b->total <= a->total || b->busy < a->busy)
		return -1;
	return (int)((db * 100 + dt / 2) / dt);
}

long sysinfo_meminfo_kb(const char *text, const char *key)
{
	size_t n = strlen(key);

	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : p)
		if (!strncmp(p, key, n) && p[n] == ':')
			return strtol(p + n + 1, NULL, 10);
	return -1;
}

/* ------------------------------------------------------------- reading */

void sysinfo_set_root(const char *root)
{
	snprintf(si.root, sizeof(si.root), "%s", root ? root : "");
}

static int read_root(const char *path, char *buf, size_t size)
{
	char full[300];
	int n;

	snprintf(full, sizeof(full), "%s%s", si.root, path);
	n = read_file(full, buf, size - 1);
	buf[n > 0 ? n : 0] = 0;
	return n;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* The numbers N of the entries <prefix>N in dir, sorted; returns count. */
static int list_numbered(const char *dir, const char *prefix, int *nums,
			 int max)
{
	char full[300];
	size_t pl = strlen(prefix);
	struct dirent *e;
	DIR *d;
	int n = 0;

	snprintf(full, sizeof(full), "%s%s", si.root, dir);
	d = opendir(full);
	if (!d)
		return 0;
	while ((e = readdir(d)) && n < max) {
		char *end;
		long v;

		if (strncmp(e->d_name, prefix, pl))
			continue;
		v = strtol(e->d_name + pl, &end, 10);
		if (end != e->d_name + pl && !*end)
			nums[n++] = (int)v;
	}
	closedir(d);
	qsort(nums, n, sizeof(*nums), cmp_int);
	return n;
}

static void add_temp(const char *name, long mc)
{
	struct temp *t;

	if (si.ntemps >= MAX_TEMPS)
		return;
	t = &si.temps[si.ntemps++];
	snprintf(t->name, sizeof(t->name), "%s", name[0] ? name : "?");
	t->mc = mc;
}

static void read_temps(void)
{
	int nums[MAX_TEMPS];
	int n;

	si.ntemps = 0;
	n = list_numbered("/sys/class/thermal", "thermal_zone", nums, MAX_TEMPS);
	for (int i = 0; i < n; i++) {
		char path[80], type[32], temp[24];

		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type",
			 nums[i]);
		read_root(path, type, sizeof(type));
		type[strcspn(type, "\n")] = 0;
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp",
			 nums[i]);
		add_temp(type, read_root(path, temp, sizeof(temp)) > 0 ?
			 strtol(temp, NULL, 10) : -1);
	}
	n = list_numbered("/sys/class/hwmon", "hwmon", nums, MAX_TEMPS);
	for (int i = 0; i < n; i++) {
		char path[80], name[32], temp[24];

		snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp1_input",
			 nums[i]);
		if (read_root(path, temp, sizeof(temp)) <= 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", nums[i]);
		read_root(path, name, sizeof(name));
		name[strcspn(name, "\n")] = 0;
		add_temp(name, strtol(temp, NULL, 10));
	}
}

static void read_freqs(void)
{
	int nums[MAX_POLICIES];
	int n = list_numbered("/sys/devices/system/cpu/cpufreq", "policy", nums,
			      MAX_POLICIES);

	si.npol = 0;
	for (int i = 0; i < n; i++) {
		char path[80], buf[24];

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq",
			 nums[i]);
		si.khz[si.npol++] = read_root(path, buf, sizeof(buf)) > 0 ?
				    strtol(buf, NULL, 10) : -1;
	}
}

static void sample(void)
{
	static char buf[4096];
	struct statvfs vfs;

	memcpy(si.prev, si.cur, sizeof(si.prev));
	memset(si.cur, 0, sizeof(si.cur));
	read_root("/proc/stat", buf, sizeof(buf));
	si.ncpu = sysinfo_parse_stat(buf, si.cur, SYSINFO_MAX_CPUS + 1);
	for (int i = 0; i < si.ncpu; i++)
		si.usage[i] = sysinfo_usage(&si.prev[i], &si.cur[i]);
	read_root("/proc/meminfo", buf, sizeof(buf));
	si.mem_total = sysinfo_meminfo_kb(buf, "MemTotal");
	si.mem_avail = sysinfo_meminfo_kb(buf, "MemAvailable");
	read_freqs();
	read_temps();
	battery_read(&si.bat);
	si.data_free = si.data_size = 0;
	if (!statvfs(fe_data_dir, &vfs)) {
		si.data_free = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
		si.data_size = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
	}
}

/* ------------------------------------------------------------ the page */

static void log_values(const char *from)
{
	char cpu[160], temps[300], bat[96], mode[16], when[24];
	size_t n = 0;
	struct tm tm;
	int valid;

	n += snprintf(cpu + n, sizeof(cpu) - n, "total %d%%",
		      si.ncpu ? si.usage[0] : -1);
	for (int i = 1; i < si.ncpu && n < sizeof(cpu); i++)
		n += snprintf(cpu + n, sizeof(cpu) - n, ", cpu%d %d%%", i - 1,
			      si.usage[i]);
	cpuctl_mode_label(settings.cpu, mode, sizeof(mode));
	log_msg("sysinfo: opened from %s; cpu since boot %s", from,
		si.ncpu ? cpu : "unknown (no /proc/stat)");
	n = 0;
	temps[0] = 0;
	for (int i = 0; i < si.npol && n < sizeof(temps); i++)
		n += snprintf(temps + n, sizeof(temps) - n, "%s%ld", i ? "/" : "",
			      si.khz[i]);
	log_msg("sysinfo: cpufreq %d policies, %s kHz, mode %s; memory "
		"MemTotal %ld kB MemAvailable %ld kB", si.npol,
		si.npol ? temps : "-", mode, si.mem_total, si.mem_avail);
	n = 0;
	temps[0] = 0;
	for (int i = 0; i < si.ntemps && n < sizeof(temps); i++)
		n += snprintf(temps + n, sizeof(temps) - n, "%s%s %ld mC",
			      i ? ", " : "", si.temps[i].name, si.temps[i].mc);
	log_msg("sysinfo: %d temperature sensors (thermal zones, hwmon): %s",
		si.ntemps, si.ntemps ? temps : "none");
	battery_format(&si.bat, bat, sizeof(bat));
	valid = realtime_utc(&tm);
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
	log_msg("sysinfo: battery %s; clock %s UTC%s; uptime %lld s; data %llu "
		"of %llu MiB free; kernel %s", bat, when, valid ? "" : " (not set)",
		(long long)(boot_ms() / 1000),
		si.data_free >> 20, si.data_size >> 20, si.kernel);
}

static void sysinfo_enter(const char *from, int64_t now)
{
	struct utsname u;

	memset(si.cur, 0, sizeof(si.cur));
	if (!uname(&u))
		snprintf(si.kernel, sizeof(si.kernel), "%s", u.release);
	/* From zero: the first usage is the average since boot. */
	sample();
	log_values(from);
	si.next = now + REFRESH_NS;
}

/* Drawn after each sample and as the seconds of the clock and the
 * uptime tick (two clocks, so up to three times a second). */
static struct ui_result sysinfo_update(uint32_t pressed, int64_t now)
{
	if (pressed & JOY_BIT(B))
		return UI_ACT(UI_BACK);
	if (now >= si.next) {
		sample();
		si.next = now + REFRESH_NS;
		ui_redraw();
	}
	ui_redraw_at(si.next);
	ui_redraw_at(ui_clock_tick(now, CLOCK_REALTIME, 1));
	ui_redraw_at(ui_clock_tick(now, CLOCK_BOOTTIME, 1));
	return UI_STAY;
}

/* Label in the left column, value from VAL_X. */
static void row(struct canvas *c, int *y, const char *label, const char *val)
{
	if (*y > SYSINFO_ROW_MAX)
		return;
	draw_text(c, 8, *y, COL_DIM, label);
	draw_textf(c, SYSINFO_VAL_X, *y, COL_FG, "%.*s", VAL_COLS(c), val);
	*y += SYSINFO_ROW_H;
}

/* Items joined by two spaces, wrapped onto further rows (no label). */
static void row_list(struct canvas *c, int *y, const char *label,
		     char items[][28], int n)
{
	char line[CANVAS_W / FONT_W + 1] = "";
	int first = 1, cols = VAL_COLS(c);

	if (cols > (int)sizeof(line) - 1)
		cols = sizeof(line) - 1;

	for (int i = 0; i < n; i++) {
		size_t len = strlen(line);

		if (len && len + 2 + strlen(items[i]) > (size_t)cols) {
			row(c, y, first ? label : "", line);
			first = 0;
			line[0] = 0;
			len = 0;
		}
		snprintf(line + len, sizeof(line) - len, "%s%s", len ? "  " : "",
			 items[i]);
	}
	if (line[0])
		row(c, y, first ? label : "", line);
}

static const char *const bat_status[] = {
	"", "Discharging", "Charging", "Full", "Not charging",
};

static void sysinfo_draw(struct canvas *c, const struct game_view *g)
{
	char v[64], mode[16], items[MAX_TEMPS > SYSINFO_MAX_CPUS ?
				    MAX_TEMPS : SYSINFO_MAX_CPUS][28];
	int y = SYSINFO_ROW_Y, n = 0;
	int64_t up = boot_ms() / 1000;
	struct tm tm;

	(void)g;
	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 8, HEAD_Y, COL_HI, "System info");
	draw_fill(c, 0, HEAD_LINE_Y, c->w, 2, COL_SEL);

	cpuctl_mode_label(settings.cpu, mode, sizeof(mode));
	if (si.ncpu && si.usage[0] >= 0)
		snprintf(v, sizeof(v), "%3d%%  %s", si.usage[0], mode);
	else
		snprintf(v, sizeof(v), "  -   %s", mode);
	row(c, &y, "CPU", v);
	for (int i = 1; i < si.ncpu; i++)
		snprintf(items[n++], sizeof(items[0]), "%3d%%", si.usage[i]);
	if (n)
		row_list(c, &y, "Cores", items, n);
	n = 0;
	for (int i = 0; i < si.npol; i++)
		snprintf(items[n++], sizeof(items[0]), "%ld MHz",
			 si.khz[i] > 0 ? si.khz[i] / 1000 : -1);
	if (n)
		row_list(c, &y, "Freq", items, n);
	if (si.mem_total > 0 && si.mem_avail >= 0)
		snprintf(v, sizeof(v), "%ld / %ld MB",
			 (si.mem_total - si.mem_avail) / 1024, si.mem_total / 1024);
	else
		snprintf(v, sizeof(v), "-");
	row(c, &y, "Memory", v);
	n = 0;
	for (int i = 0; i < si.ntemps; i++) {
		char name[12];

		/* "soc-thermal" reads "soc". */
		snprintf(name, sizeof(name), "%.*s",
			 (int)strcspn(si.temps[i].name, "-_"), si.temps[i].name);
		if (si.temps[i].mc == -1)
			snprintf(items[n++], sizeof(items[0]), "%s -", name);
		else
			snprintf(items[n++], sizeof(items[0]), "%s %.1fC", name,
				 si.temps[i].mc / 1000.0);
	}
	if (n)
		row_list(c, &y, "Temp", items, n);
	else
		row(c, &y, "Temp", "no sensors");
	if (si.bat.valid) {
		snprintf(v, sizeof(v), "%d%%  %s", si.bat.capacity,
			 bat_status[si.bat.status]);
		row(c, &y, "Battery", v);
		/* RK817 current is negative while discharging. */
		snprintf(v, sizeof(v), "%.3f V  %+ld mA%s", si.bat.uv / 1e6,
			 si.bat.ua / 1000, si.bat.online ? "  USB" : "");
		row(c, &y, "", v);
	} else {
		row(c, &y, "Battery", "-");
	}
	if (realtime_utc(&tm))
		snprintf(v, sizeof(v), "%04d-%02d-%02d %02d:%02d:%02d",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
			 tm.tm_min, tm.tm_sec);
	else
		snprintf(v, sizeof(v), "Not set");
	row(c, &y, "Clock", v);
	snprintf(v, sizeof(v), "%lld:%02lld:%02lld", (long long)(up / 3600),
		 (long long)(up / 60 % 60), (long long)(up % 60));
	row(c, &y, "Uptime", v);
	if (si.data_size)
		snprintf(v, sizeof(v), "%.1f / %.1f GB", si.data_free / 1e9,
			 si.data_size / 1e9);
	else
		snprintf(v, sizeof(v), "-");
	row(c, &y, "Free", v);
	row(c, &y, "Kernel", si.kernel);
	/* Every row is needed (11 on the device), so the hint goes up. */
	draw_text(c, c->w - 8 - draw_text_width("B:back"), HEAD_Y, COL_DIM,
		  "B:back");
}

const struct page sysinfo_page = {
	"system info", sysinfo_enter, sysinfo_update, sysinfo_draw, 0,
};
