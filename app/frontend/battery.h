#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

enum bat_status {
	BAT_UNKNOWN,
	BAT_DISCHARGING,
	BAT_CHARGING,
	BAT_FULL,
	BAT_NOT_CHARGING,
};

struct battery {
	int valid;       /* a battery supply was found and read */
	int capacity;    /* percent, -1 if not reported */
	int status;      /* enum bat_status */
	long uv;         /* voltage (voltage_now or voltage_avg), -1 if none */
	long ua;         /* current (current_now or current_avg), 0 if none */
	int online;      /* a charger supply reports online */
	unsigned seq;    /* incremented on every poll */
};

#define BAT_LOW_PCT 10

/*
 * Find the supplies in dir (normally /sys/class/power_supply) and log all
 * their properties once. Safe to call again (rescans).
 */
void battery_init(const char *dir);
/* Rereads every 5 s (every 1 s for 30 s after a charger change, see
 * battery.c); rescans while nothing was found. Returns when it is due
 * again. */
int64_t battery_poll(int64_t now);
/* The charger diagnostics' full state as "charger: <why>: ..." lines
 * (for sleep entry and exit). */
void battery_charger_log(const char *why);
/* For the selftest: other /proc/interrupts and regmap debugfs roots. */
void battery_diag_paths(const char *interrupts, const char *regmap_root);
/* Reads now; returns 0 when a battery was read. */
int battery_read(struct battery *b);
const struct battery *battery_get(void);
/* Below BAT_LOW_PCT and not on a charger. */
int battery_low(void);
/* "87% Discharging 3.912 V -0.412 A 1612 mW, charger offline" for the
 * log (the RK817 current is negative while discharging; the power is
 * left out without a voltage). */
void battery_format(const struct battery *b, char *buf, int size);
/* "87%", "+87%" while charging, "" when unknown. */
void battery_label(const struct battery *b, char *buf, int size);

#endif
