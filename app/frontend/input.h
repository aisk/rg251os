#ifndef INPUT_H
#define INPUT_H

#include <poll.h>
#include <stdint.h>

/* Extra bits above the libretro joypad ids, for frontend-only keys. */
#define BTN_BIT_MENU (1u << 20)

int input_init(void);
/* Fill pollfds for the inotify watch and all devices; returns the count. */
int input_pollfds(struct pollfd *p, int max);
/* Handle the revents of the pollfds filled by input_pollfds. */
void input_dispatch(const struct pollfd *p, int n);
/* Cheap fallback rescan of /dev/input, in case an inotify event is missed. */
void input_rescan(void);
/* Bitmask of (1 << RETRO_DEVICE_ID_JOYPAD_*) | BTN_BIT_MENU. */
uint32_t input_buttons(void);

/*
 * Power key: a short press (released before PWRKEY_LONG_NS) sleeps or
 * wakes, holding it PWRKEY_LONG_NS powers off (fired while still held,
 * well before the RK817 cuts power by itself after a long hold). After a
 * wake the press that woke the device is swallowed, so neither it nor
 * its release starts a new sleep.
 */
#define PWRKEY_LONG_NS 2000000000LL
#define PWRKEY_WAKE_NS 1500000000LL

enum pwr_action { PWR_NONE, PWR_SHORT, PWR_LONG, PWR_WAKE_PRESS };

struct pwrkey {
	int down, fired, swallow;
	int64_t down_ns, swallow_until;
};

/* Key went down (1) or up (0); returns an enum pwr_action. */
int pwrkey_event(struct pwrkey *k, int down, int64_t now);
/* Call regularly; PWR_LONG once the key is held long enough. */
int pwrkey_poll(struct pwrkey *k, int64_t now);
/* After sleep. held: 1 the waking press was seen and the key is still
 * down, 0 it was seen and released (light sleep), -1 it may still be
 * reported (suspend to RAM). */
void pwrkey_wake(struct pwrkey *k, int held, int64_t now);

/* Implemented by the main loop: KEY_POWER down (1) or up (0). */
void on_power_key(int down);
void on_volume_key(int dir);
void on_jack(int inserted);

#endif
