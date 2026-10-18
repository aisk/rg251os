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

/* Implemented by the main loop. */
void on_power_key(void);
void on_volume_key(int dir);
void on_jack(int inserted);

#endif
