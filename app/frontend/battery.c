/*
 * Battery state from the power supply class. On the RG351P the RK817
 * charger driver (rk817_charger) registers "rk817-battery" (capacity,
 * status, voltage_avg, current_avg, ...) and "rk817-charger" (online). The
 * driver itself samples the PMIC every 8 s and sysfs reads only return its
 * cached values, so reading every few seconds costs next to nothing.
 *
 * Log lines: "power_supply <name>: ..." with every property once when the
 * supplies are first found, then "battery: ..." once a minute (and a
 * short form in the perf line), for calibration from the logs.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "battery.h"
#include "fe.h"

#define POLL_NS (5 * 1000000000LL)
#define LOG_NS (60 * 1000000000LL)

static struct {
	char root[200];
	char bat[300], chg[300];
	int scanned, found_logged;
	struct battery b;
	int64_t next_poll, next_log;
	unsigned seq;
} s;

static void log_uevent(const char *path, const char *name)
{
	char file[400], buf[2048], out[1024];
	size_t n = 0;
	char *line, *save;

	snprintf(file, sizeof(file), "%s/uevent", path);
	if (read_sysfs(file, buf, sizeof(buf)) <= 0) {
		log_msg("power_supply %s: no uevent", name);
		return;
	}
	out[0] = 0;
	for (line = strtok_r(buf, "\n", &save); line && n < sizeof(out);
	     line = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(line, "POWER_SUPPLY_", 13))
			line += 13;
		if (!strncmp(line, "NAME=", 5))
			continue;
		n += snprintf(out + n, sizeof(out) - n, "%s%s", n ? " " : "", line);
	}
	log_msg("power_supply %s: %s", name, out);
}

void battery_init(const char *dir)
{
	DIR *d;
	struct dirent *e;
	char path[400], type[32];

	snprintf(s.root, sizeof(s.root), "%s", dir);
	s.bat[0] = s.chg[0] = 0;
	s.scanned = 1;
	d = opendir(dir);
	if (!d)
		return;
	while ((e = readdir(d))) {
		char sub[300];

		if (e->d_name[0] == '.')
			continue;
		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
		snprintf(path, sizeof(path), "%s/type", sub);
		if (read_sysfs(path, type, sizeof(type)) <= 0)
			continue;
		if (!strcmp(type, "Battery") && !s.bat[0])
			snprintf(s.bat, sizeof(s.bat), "%s", sub);
		else if ((!strcmp(type, "Mains") || !strcmp(type, "USB")) &&
			 !s.chg[0])
			snprintf(s.chg, sizeof(s.chg), "%s", sub);
		else
			continue;
		log_msg("power_supply %s: type %s", e->d_name, type);
		log_uevent(sub, e->d_name);
	}
	closedir(d);
	if (!s.bat[0] && !s.found_logged) {
		log_msg("battery: no battery in %s", dir);
		s.found_logged = 1;
	}
}

static long read_long(const char *dir, const char *name, long fallback)
{
	char path[400];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return read_sysfs_long(path, fallback);
}

static int parse_status(const char *v)
{
	if (!strcmp(v, "Charging"))
		return BAT_CHARGING;
	if (!strcmp(v, "Discharging"))
		return BAT_DISCHARGING;
	if (!strcmp(v, "Full"))
		return BAT_FULL;
	if (!strcmp(v, "Not charging"))
		return BAT_NOT_CHARGING;
	return BAT_UNKNOWN;
}

int battery_read(struct battery *b)
{
	char path[400], buf[32];

	memset(b, 0, sizeof(*b));
	b->capacity = -1;
	b->uv = -1;
	if (!s.bat[0])
		return -1;
	b->capacity = (int)read_long(s.bat, "capacity", -1);
	if (b->capacity > 100)
		b->capacity = 100;
	b->uv = read_long(s.bat, "voltage_now", -1);
	if (b->uv < 0)
		b->uv = read_long(s.bat, "voltage_avg", -1);
	b->ua = read_long(s.bat, "current_now", 0x7fffffff);
	if (b->ua == 0x7fffffff)
		b->ua = read_long(s.bat, "current_avg", 0);
	snprintf(path, sizeof(path), "%s/status", s.bat);
	read_sysfs(path, buf, sizeof(buf));
	b->status = parse_status(buf);
	if (s.chg[0])
		b->online = read_long(s.chg, "online", 0) > 0;
	b->valid = b->capacity >= 0 || b->uv >= 0;
	return b->valid ? 0 : -1;
}

void battery_poll(int64_t now)
{
	char buf[128];

	if (now < s.next_poll)
		return;
	s.next_poll = now + POLL_NS;
	if (!s.bat[0])
		battery_init(s.scanned ? s.root : "/sys/class/power_supply");
	battery_read(&s.b);
	s.b.seq = ++s.seq;
	if (s.b.valid && now >= s.next_log && log_debug()) {
		s.next_log = now + LOG_NS;
		battery_format(&s.b, buf, sizeof(buf));
		log_msg("battery: %s", buf);
	}
}

const struct battery *battery_get(void)
{
	return &s.b;
}

int battery_low(void)
{
	return s.b.valid && s.b.capacity >= 0 && s.b.capacity < BAT_LOW_PCT &&
	       !s.b.online && s.b.status != BAT_CHARGING;
}

void battery_format(const struct battery *b, char *buf, int size)
{
	static const char *const names[] = {
		"Unknown", "Discharging", "Charging", "Full", "Not charging",
	};

	if (!b->valid) {
		snprintf(buf, size, "n/a");
		return;
	}
	snprintf(buf, size, "%d%% %s %.3f V %+.3f A, charger %s", b->capacity,
		 names[b->status], b->uv / 1e6, b->ua / 1e6,
		 b->online ? "online" : "offline");
}

void battery_label(const struct battery *b, char *buf, int size)
{
	if (!b->valid || b->capacity < 0)
		buf[0] = 0;
	else
		snprintf(buf, size, "%s%d%%", b->status == BAT_CHARGING ? "+" : "",
			 b->capacity);
}
