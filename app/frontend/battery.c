/*
 * Battery state from the power supply class. On the RG351P the RK817
 * charger driver (rk817_charger) registers "rk817-battery" (capacity,
 * status, charge_type, voltage_avg, current_avg, charge_now, ...) and
 * "rk817-charger" (online, voltage_avg of the input). The driver itself
 * samples the PMIC every 8 s and sysfs reads only return its cached
 * values, so reading every few seconds costs next to nothing. Its online
 * is read from the PMIC once at probe and then only follows the plug-in
 * and plug-out interrupts.
 *
 * Charger diagnostics (always, not only with debug logging): plugging
 * in often leaves the RK817 "Not charging". On every poll the charger's
 * online, the battery's status and charge_type, the plug IRQ counts from
 * /proc/interrupts ("rk817_plug_in", "rk817_plug_out") and two PMIC
 * registers (CHRG_STS 0xeb, SYS_STS 0xf0) are compared with the last
 * poll; a change logs the whole state, and for 30 s after it a compact
 * sample every 1 s, then back to every 5 s. The registers are read live
 * from the regmap debugfs file of the rk8xx I2C device
 * (/sys/kernel/debug/regmap/<dev>/registers, CONFIG_DEBUG_FS from
 * debug.config; the regmap has no cache, so every read goes to the
 * chip): 0xe4 CHRG_OUT, 0xe5 CHRG_IN (input limits, which the driver
 * writes at probe and on unplug only), 0xe6 CHRG_TERM (charge enable),
 * 0xeb CHRG_STS, 0xf0 SYS_STS (PLUG_IN_STS), decoded. Only the needed
 * lines are read (pread at the offset found once), a few I2C reads per
 * sample, so the 1 s samples cost well under a millisecond in game; the
 * time is logged.
 *
 * Log lines: "power_supply <name>: ..." with every property once when the
 * supplies are first found; "charger: start: ..." and "charger: regs:
 * ..." then, on each change "charger: change (<what>): ..." with the
 * same two lines, "charger: +N s: ..." for the 1 s samples and
 * "charger: samples done ..."; battery_charger_log for sleep. With debug
 * logging also "battery: ..." once a minute (and a short form in the
 * perf line).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "battery.h"
#include "fe.h"

#define POLL_NS (5 * 1000000000LL)
#define LOG_NS (60 * 1000000000LL)
#define BURST_NS 1000000000LL           /* sample interval after a change */
#define BURST_LEN_NS (30 * 1000000000LL)

/* The RK817 charger registers logged (include/linux/mfd/rk808.h). */
#define REG_FIRST 0xe4  /* PMIC_CHRG_OUT */
#define REG_IN 0xe5     /* PMIC_CHRG_IN */
#define REG_TERM 0xe6   /* PMIC_CHRG_TERM */
#define REG_STS 0xeb    /* PMIC_CHRG_STS */
#define REG_SYS 0xf0    /* SYS_STS */
#define NREGS (REG_SYS - REG_FIRST + 1)

/* What the change detection compares between polls. */
struct chg_state {
	int online, status;
	char type[16];        /* charge_type */
	long irq_in, irq_out; /* -1: not in /proc/interrupts */
	int sts, sys;         /* CHRG_STS, SYS_STS; -1 unread */
};

static struct {
	char root[200];
	char bat[300], chg[300];
	int scanned, found_logged;
	struct battery b;
	int64_t next_poll, next_log;
	unsigned seq;

	/* Charger diagnostics */
	char irq_path[200], regmap_root[200];
	int reg_state;        /* 0 not looked for, 1 found, -1 none */
	int reg_fd, reg_len;  /* registers file, bytes per line */
	long reg_off;         /* offset of REG_FIRST's line */
	struct chg_state prev;
	int have_prev;
	int64_t burst_start, burst_until;
	double burst_ms_sum, burst_ms_max;
	unsigned burst_n;
} s = {
	.irq_path = "/proc/interrupts",
	.regmap_root = "/sys/kernel/debug/regmap",
	.reg_fd = -1,
};

static const char *const status_names[] = {
	"Unknown", "Discharging", "Charging", "Full", "Not charging",
};

/* CHRG_STS bits 6:4, enum rk817_charge_status in the driver. */
static const char *const chg_sts_names[] = {
	"CHRG_OFF", "DEAD_CHRG", "TRICKLE_CHRG", "CC_OR_CV_CHRG",
	"CHARGE_FINISH", "USB_OVER_VOL", "BAT_TMP_ERR", "BAT_TIM_ERR",
};
/* CHRG_IN ILIM_SEL (BSP driver's table; the mainline driver writes 3
 * for 1.5 A) and CHRG_OUT CUR_SEL (enum rk817_chg_cur), in mA. */
static const int ilim_ma[] = { 80, 450, 850, 1500, 1750, 2000, 2500, 3000 };
static const int chg_cur_ma[] = { 1000, 1500, 2000, 2500, 2750, 3000, 3500,
				  500 };

void battery_diag_paths(const char *interrupts, const char *regmap_root)
{
	snprintf(s.irq_path, sizeof(s.irq_path), "%s", interrupts);
	snprintf(s.regmap_root, sizeof(s.regmap_root), "%s", regmap_root);
	if (s.reg_fd >= 0)
		close(s.reg_fd);
	s.reg_fd = -1;
	s.reg_state = 0;
	s.have_prev = 0;
}

/* Summed over the CPUs; -1 when the line is not there. */
static void irq_counts(long *in, long *out)
{
	static char buf[16384];
	char *line, *save;
	int n = read_file(s.irq_path, buf, sizeof(buf) - 1);

	*in = *out = -1;
	if (n <= 0)
		return;
	buf[n] = 0;
	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *p = strchr(line, ':'), *end;
		long sum = 0, *dst;
		size_t len = strlen(line);

		/* The action name is the last field. */
		while (len && line[len - 1] == ' ')
			line[--len] = 0;
		while (len && line[len - 1] != ' ')
			len--;
		if (!p || !len)
			continue;
		if (!strcmp(line + len, "rk817_plug_in"))
			dst = in;
		else if (!strcmp(line + len, "rk817_plug_out"))
			dst = out;
		else
			continue;
		for (p++;; p = end) {
			long v = strtol(p, &end, 10);

			if (end == p)
				break;
			sum += v;
		}
		*dst = sum;
	}
}

/* Registers first..first+n-1 into v (-1 when not read); returns the
 * number read. */
static int regs_read(int first, int n, int *v)
{
	char buf[NREGS * 16 + 1], *p;
	ssize_t got;
	int found = 0;

	for (int i = 0; i < n; i++)
		v[i] = -1;
	if (s.reg_fd < 0)
		return 0;
	got = pread(s.reg_fd, buf, (size_t)(n * s.reg_len),
		    s.reg_off + (long)(first - REG_FIRST) * s.reg_len);
	if (got <= 0)
		return 0;
	buf[got] = 0;
	for (p = buf; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		unsigned r, val;

		if (sscanf(p, "%x: %x", &r, &val) == 2 && (int)r >= first &&
		    (int)r < first + n) {
			v[r - first] = (int)val;
			found++;
		}
	}
	return found;
}

/* The byte offset of REG_FIRST's line in an opened registers file: one
 * line per register ("e4: 4a"), all the same length, so at REG_FIRST
 * lines in if every register is listed, else found by reading it all. */
static int regs_locate(void)
{
	char buf[4096 + 1], *nl;
	long pos = 0;
	ssize_t got;
	int v;

	got = pread(s.reg_fd, buf, 64, 0);
	if (got <= 0)
		return -1;
	buf[got] = 0;
	nl = strchr(buf, '\n');
	if (!nl || nl - buf < 3)
		return -1;
	s.reg_len = (int)(nl - buf + 1);
	s.reg_off = (long)REG_FIRST * s.reg_len;
	if (regs_read(REG_FIRST, 1, &v) == 1)
		return 0;
	for (;;) {
		size_t chunk = (sizeof(buf) - 1) / (size_t)s.reg_len *
			       (size_t)s.reg_len;
		char *p;

		got = pread(s.reg_fd, buf, chunk, pos);
		if (got <= 0)
			return -1;
		buf[got] = 0;
		for (p = buf; p < buf + got; p += s.reg_len) {
			unsigned r;

			if (sscanf(p, "%x:", &r) == 1 && r == REG_FIRST) {
				s.reg_off = pos + (p - buf);
				return 0;
			}
		}
		pos += got;
	}
}

/* The rk8xx regmap in debugfs, once. */
static void regs_find(void)
{
	char names[300] = "", sub[500], nm[64], path[520];
	size_t n = 0;
	DIR *d;
	struct dirent *e;

	if (s.reg_state)
		return;
	s.reg_state = -1;
	d = opendir(s.regmap_root);
	if (!d) {
		log_msg("charger: no %s (%s), PMIC registers not logged "
			"(needs debugfs, CONFIG_DEBUG_FS in debug.config)",
			s.regmap_root, strerror(errno));
		return;
	}
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(sub, sizeof(sub), "%s/%s", s.regmap_root, e->d_name);
		if (read_attr(sub, "name", nm, sizeof(nm)) < 0)
			snprintf(nm, sizeof(nm), "?");
		if (n < sizeof(names))
			n += (size_t)snprintf(names + n, sizeof(names) - n,
					      "%s%s (%s)", n ? ", " : "",
					      e->d_name, nm);
		if (s.reg_fd >= 0 || strncmp(nm, "rk8", 3))
			continue;
		snprintf(path, sizeof(path), "%s/registers", sub);
		s.reg_fd = open(path, O_RDONLY | O_CLOEXEC);
		if (s.reg_fd < 0) {
			log_msg("charger: open %s: %s", path, strerror(errno));
			continue;
		}
		if (regs_locate()) {
			log_msg("charger: 0x%x not found in %s", REG_FIRST, path);
			close(s.reg_fd);
			s.reg_fd = -1;
			continue;
		}
		s.reg_state = 1;
		log_msg("charger: PMIC registers from %s, %d bytes per line, "
			"0x%x at offset %ld", path, s.reg_len, REG_FIRST,
			s.reg_off);
	}
	closedir(d);
	if (s.reg_state < 0)
		log_msg("charger: no rk8xx regmap in %s (%s), PMIC registers "
			"not logged", s.regmap_root, names[0] ? names : "empty");
}

static int field(int v, int shift, int mask)
{
	return v < 0 ? -1 : (v >> shift) & mask;
}

static const char *sts_name(int sts)
{
	return sts < 0 ? "?" : chg_sts_names[(sts >> 4) & 7];
}

/* Current state; with regs (NREGS entries), all registers, otherwise
 * only CHRG_STS and SYS_STS. */
static void chg_read(struct chg_state *c, int *regs)
{
	int v[NREGS];

	battery_read(&s.b);
	c->online = s.b.online;
	c->status = s.b.status;
	if (!s.bat[0] || read_attr(s.bat, "charge_type", c->type,
				   sizeof(c->type)) < 0)
		snprintf(c->type, sizeof(c->type), "n/a");
	irq_counts(&c->irq_in, &c->irq_out);
	if (!regs)
		regs = v;
	if (regs == v) {
		regs_read(REG_STS, 1, &v[REG_STS - REG_FIRST]);
		regs_read(REG_SYS, 1, &v[REG_SYS - REG_FIRST]);
	} else {
		regs_read(REG_FIRST, NREGS, regs);
	}
	c->sts = regs[REG_STS - REG_FIRST];
	c->sys = regs[REG_SYS - REG_FIRST];
}

/* "online 0->1, status Discharging->Not charging", "" if nothing. */
static int chg_diff(const struct chg_state *a, const struct chg_state *b,
		    char *buf, size_t size)
{
	size_t n = 0;

	buf[0] = 0;
#define ADD(...) (n += (size_t)snprintf(buf + n, n < size ? size - n : 0, \
					 __VA_ARGS__), n >= size ? n = size - 1 : 0)
	if (a->online != b->online)
		ADD("%sonline %d->%d", n ? ", " : "", a->online, b->online);
	if (a->status != b->status)
		ADD("%sstatus %s->%s", n ? ", " : "", status_names[a->status],
		    status_names[b->status]);
	if (strcmp(a->type, b->type))
		ADD("%scharge_type %s->%s", n ? ", " : "", a->type, b->type);
	if (a->irq_in != b->irq_in || a->irq_out != b->irq_out)
		ADD("%sIRQs plug_in %ld->%ld plug_out %ld->%ld", n ? ", " : "",
		    a->irq_in, b->irq_in, a->irq_out, b->irq_out);
	if (field(a->sys, 6, 1) != field(b->sys, 6, 1))
		ADD("%sPLUG_IN_STS %d->%d", n ? ", " : "", field(a->sys, 6, 1),
		    field(b->sys, 6, 1));
	if (field(a->sts, 4, 7) != field(b->sts, 4, 7))
		ADD("%sCHRG_STS %s->%s", n ? ", " : "", sts_name(a->sts),
		    sts_name(b->sts));
#undef ADD
	return n > 0;
}

static void fmt_volts(char *buf, size_t size, long uv)
{
	if (uv < 0)
		snprintf(buf, size, "n/a");
	else
		snprintf(buf, size, "%.3f V", uv / 1e6);
}

/* The two full lines: values, then the registers decoded. */
static void chg_log_full(const char *why, const struct chg_state *c,
			 const int *r, double ms)
{
	char in[24], ccv[24], cc[32] = "n/a", charge[48] = "n/a", regs[160] = "",
	     dec[200] = "";
	long now_uah = -1, full_uah = -1, ccc = -1;
	const struct battery *b = &s.b;
	size_t n = 0;

	fmt_volts(in, sizeof(in),
		  s.chg[0] ? read_attr_long(s.chg, "voltage_avg", -1) : -1);
	fmt_volts(ccv, sizeof(ccv), s.bat[0] ?
		  read_attr_long(s.bat, "constant_charge_voltage_max", -1) : -1);
	if (s.bat[0]) {
		now_uah = read_attr_long(s.bat, "charge_now", -1);
		full_uah = read_attr_long(s.bat, "charge_full", -1);
		ccc = read_attr_long(s.bat, "constant_charge_current_max", -1);
	}
	if (now_uah >= 0 && full_uah >= 0)
		snprintf(charge, sizeof(charge), "%ld of %ld mAh",
			 now_uah / 1000, full_uah / 1000);
	if (ccc >= 0)
		snprintf(cc, sizeof(cc), "%.3f A", ccc / 1e6);
	log_msg("charger: %s: online %d, %s, charge_type %s, %d%% %.3f V "
		"%+.3f A, input %s, charge %s, max %s %s; IRQs plug_in %ld "
		"plug_out %ld", why, c->online, status_names[c->status], c->type,
		b->capacity, b->uv / 1e6, b->ua / 1e6, in, charge, cc, ccv,
		c->irq_in, c->irq_out);
	if (s.reg_fd < 0)
		return;
	for (int i = 0; i < NREGS; i++) {
		int reg = REG_FIRST + i;

		if (reg != REG_FIRST && reg != REG_IN && reg != REG_TERM &&
		    reg != REG_STS && reg != REG_SYS)
			continue;
		if (r[i] < 0)
			n += (size_t)snprintf(regs + n, sizeof(regs) - n,
					      "%s%02x=?", n ? " " : "", reg);
		else
			n += (size_t)snprintf(regs + n, sizeof(regs) - n,
					      "%s%02x=0x%02x", n ? " " : "", reg,
					      r[i]);
	}
	if (r[0] >= 0 && r[1] >= 0 && r[2] >= 0 && r[7] >= 0 && r[12] >= 0) {
		int e4 = r[0], e5 = r[1], e6 = r[2];

		snprintf(dec, sizeof(dec), "PLUG_IN_STS %d, CHRG_STS %s, "
			 "BAT_EXS %d, CHRG_EN %d, in: VLIM %s %.1f V ILIM %s "
			 "%.2f A, out: %.2f V %.2f A", field(r[12], 6, 1),
			 sts_name(r[7]), field(r[7], 7, 1), field(e6, 6, 1),
			 e5 & 0x80 ? "on" : "off", 4.0 + 0.1 * ((e5 >> 4) & 7),
			 e5 & 0x08 ? "on" : "off", ilim_ma[e5 & 7] / 1000.0,
			 4.1 + 0.05 * ((e4 >> 4) & 7),
			 chg_cur_ma[e4 & 7] / 1000.0);
	} else {
		snprintf(dec, sizeof(dec), "not all read");
	}
	log_msg("charger: %s: regs %s (%s), read in %.1f ms", why, regs, dec,
		ms);
}

void battery_charger_log(const char *why)
{
	struct chg_state c;
	int r[NREGS];
	int64_t t0 = now_ns();

	if (!s.bat[0] && !s.chg[0])
		return;
	regs_find();
	chg_read(&c, r);
	chg_log_full(why, &c, r, (now_ns() - t0) / 1e6);
}

/* One compact line per BURST_NS after a change; ms is what chg_read
 * took. Returns the time with this line's own reads. */
static double chg_sample(int64_t now, const struct chg_state *c, double ms)
{
	char in[24], regs[96] = "";
	int64_t t0 = now_ns();
	int e5;

	fmt_volts(in, sizeof(in),
		  s.chg[0] ? read_attr_long(s.chg, "voltage_avg", -1) : -1);
	if (s.reg_fd >= 0) {
		regs_read(REG_IN, 1, &e5);
		snprintf(regs, sizeof(regs), "; e5=0x%02x eb=0x%02x f0=0x%02x "
			 "(PLUG_IN_STS %d, %s)", e5 & 0xff, c->sts & 0xff,
			 c->sys & 0xff, field(c->sys, 6, 1), sts_name(c->sts));
	}
	ms += (now_ns() - t0) / 1e6;
	log_msg("charger: +%lld s: online %d, %s, %s, %d%% %.3f V %+.3f A, "
		"input %s; IRQs %ld/%ld%s; read in %.2f ms",
		(long long)((now - s.burst_start + BURST_NS / 2) / BURST_NS),
		c->online, status_names[c->status], c->type, s.b.capacity,
		s.b.uv / 1e6, s.b.ua / 1e6, in, c->irq_in, c->irq_out, regs, ms);
	return ms;
}

/* The baseline and the startup lines, when the supplies are found. */
static void chg_start(void)
{
	int r[NREGS];
	int64_t t0 = now_ns();

	regs_find();
	chg_read(&s.prev, r);
	chg_log_full("start", &s.prev, r, (now_ns() - t0) / 1e6);
	s.have_prev = 1;
	s.burst_until = 0;
}

/* Compare with the last poll; returns when it wants the next one. */
static int64_t chg_poll(int64_t now)
{
	struct chg_state c;
	char what[200], why[230];
	int r[NREGS];
	int64_t t0 = now_ns();
	double ms;

	regs_find();
	chg_read(&c, NULL);
	ms = (now_ns() - t0) / 1e6;
	if (!s.have_prev) {
		s.prev = c;
		s.have_prev = 1;
	}
	if (chg_diff(&s.prev, &c, what, sizeof(what))) {
		snprintf(why, sizeof(why), "change (%s)", what);
		t0 = now_ns();
		chg_read(&c, r);
		chg_log_full(why, &c, r, (now_ns() - t0) / 1e6);
		s.prev = c;
		s.burst_start = now;
		s.burst_until = now + BURST_LEN_NS;
		s.burst_n = 0;
		s.burst_ms_sum = s.burst_ms_max = 0;
		return now + BURST_NS;
	}
	if (!s.burst_until || now > s.burst_until)
		return now + POLL_NS;
	s.burst_n++;
	ms = chg_sample(now, &c, ms);
	s.burst_ms_sum += ms;
	if (ms > s.burst_ms_max)
		s.burst_ms_max = ms;
	if (now + BURST_NS <= s.burst_until)
		return now + BURST_NS;
	log_msg("charger: samples done, %u in %lld s, read time avg %.2f ms max "
		"%.2f ms, polling every %lld s again", s.burst_n,
		(long long)((now - s.burst_start) / 1000000000LL),
		s.burst_ms_sum / s.burst_n, s.burst_ms_max,
		POLL_NS / 1000000000LL);
	s.burst_until = 0;
	return now + POLL_NS;
}

static void log_uevent(const char *path, const char *name)
{
	char buf[2048], out[1024];
	size_t n = 0;
	char *line, *save;

	if (read_attr(path, "uevent", buf, sizeof(buf)) <= 0) {
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
	char type[32];

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
		if (read_attr(sub, "type", type, sizeof(type)) <= 0)
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
	if (s.bat[0] || s.chg[0])
		chg_start();
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
	char buf[32];

	memset(b, 0, sizeof(*b));
	b->capacity = -1;
	b->uv = -1;
	if (!s.bat[0])
		return -1;
	b->capacity = (int)read_attr_long(s.bat, "capacity", -1);
	if (b->capacity > 100)
		b->capacity = 100;
	b->uv = read_attr_long(s.bat, "voltage_now", -1);
	if (b->uv < 0)
		b->uv = read_attr_long(s.bat, "voltage_avg", -1);
	b->ua = read_attr_long(s.bat, "current_now", 0x7fffffff);
	if (b->ua == 0x7fffffff)
		b->ua = read_attr_long(s.bat, "current_avg", 0);
	read_attr(s.bat, "status", buf, sizeof(buf));
	b->status = parse_status(buf);
	if (s.chg[0])
		b->online = read_attr_long(s.chg, "online", 0) > 0;
	b->valid = b->capacity >= 0 || b->uv >= 0;
	return b->valid ? 0 : -1;
}

int64_t battery_poll(int64_t now)
{
	char buf[128];

	if (now < s.next_poll)
		return s.next_poll;
	s.next_poll = now + POLL_NS;
	if (!s.bat[0])
		battery_init(s.scanned ? s.root : "/sys/class/power_supply");
	if (s.bat[0] || s.chg[0])
		s.next_poll = chg_poll(now);  /* reads s.b */
	else
		battery_read(&s.b);
	s.b.seq = ++s.seq;
	if (s.b.valid && now >= s.next_log && log_debug()) {
		s.next_log = now + LOG_NS;
		battery_format(&s.b, buf, sizeof(buf));
		log_msg("battery: %s", buf);
	}
	return s.next_poll;
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
	char mw[24] = "";

	if (!b->valid) {
		snprintf(buf, size, "n/a");
		return;
	}
	if (b->uv > 0)
		snprintf(mw, sizeof(mw), " %.0f mW",
			 (b->ua < 0 ? -b->ua : b->ua) * (double)b->uv / 1e9);
	snprintf(buf, size, "%d%% %s %.3f V %+.3f A%s, charger %s", b->capacity,
		 status_names[b->status], b->uv / 1e6, b->ua / 1e6, mw,
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
