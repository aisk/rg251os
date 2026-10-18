#ifndef CORE_H
#define CORE_H

#include <stdint.h>
#include <sys/types.h>

struct core_info {
	double fps, sample_rate;
	int core;  /* enum core_id actually loaded */
};

/* Load a ROM by path with the given core (enum core_id; one that is not
 * built in falls back to mGBA); on failure returns -1 with a message in
 * err. A core still loaded is unloaded first. */
int core_load(const char *rom_path, int core, char *err, size_t errlen);
/* Save if needed, then unload the game and deinit the core. */
void core_unload(void);
int core_loaded(void);
const struct core_info *core_info(void);
/* Write changed save data and wait for it, then retro_reset: the game
 * starts over like after the console's reset, with its save RAM kept. */
void core_reset(void);
/* One retro_run with the given joypad bitmask. */
void core_run(uint32_t buttons);
/* rom_settings.blend changed: gpSP reads it again after the next
 * retro_run (applied live), mGBA ignores it. */
void core_options_changed(void);
/* Last video frame (RGB565), or NULL before the first one. */
const uint16_t *core_frame(int *w, int *h, int *pitch_px);

/* Call about once per second in game: writes changed save data in a child. */
void core_save_tick(int64_t now);
/* Write changed save data now and wait for it (quit, power off). */
void core_save_sync(void);
/* Main loop reaped a child. */
void core_child_exited(pid_t pid, int status);

#endif
