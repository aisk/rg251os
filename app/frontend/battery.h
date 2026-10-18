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
/* Rereads every few seconds; rescans while nothing was found. */
void battery_poll(int64_t now);
/* Reads now; returns 0 when a battery was read. */
int battery_read(struct battery *b);
const struct battery *battery_get(void);
/* Below BAT_LOW_PCT and not on a charger. */
int battery_low(void);
/* "87% Discharging 3.912 V -0.412 A, charger offline" for the log. */
void battery_format(const struct battery *b, char *buf, int size);
/* "87%", "+87%" while charging, "" when unknown. */
void battery_label(const struct battery *b, char *buf, int size);

#endif
