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
 * err. A core still loaded is unloaded first. The save is loaded, then
 * the resume state if there is a usable one. */
int core_load(const char *rom_path, int core, char *err, size_t errlen);
/* Save if needed, then unload the game and deinit the core. */
void core_unload(void);
int core_loaded(void);
/* Whether a core (enum core_id) is built in. */
int core_available(int core);
const struct core_info *core_info(void);
/* Write changed save data and wait for it, remove this core's state
 * file, then retro_reset: the game starts over like after the console's
 * reset, with its save RAM kept. */
void core_reset(void);
/* One retro_run with the given joypad bitmask (the core's audio buffer
 * status callback, if any, first); 1 if the core skipped the frame (no
 * new picture, see core.c). */
int core_run(uint32_t buttons);
/* Frames run and skipped since the game was loaded. */
void core_frame_counts(unsigned *frames, unsigned *skipped);
/* A setting behind a core option changed (core.c, rom_options[]): one
 * the running core takes live is read again after the next retro_run,
 * the others wait for the next start or are kept for the other core;
 * each logged. */
void core_options_changed(void);
/* Whether the core (enum core_id) has the game setting at field (a
 * member of rom_settings behind core options, core.c's rom_options[])
 * with value v, or at all for v < 0. */
int core_setting_supported(const int *field, int core, int v);
/* What a core gets for an option key now, NULL: the core's default. */
const char *core_option_value(const char *key);
/* Last video frame (RGB565), or NULL before the first one. */
const uint16_t *core_frame(int *w, int *h, int *pitch_px);

/* Call about once per second in game: writes changed save data in a child. */
void core_save_tick(int64_t now);
/* Write changed save data now and wait for it (quit, power off). */
void core_save_sync(void);
/* Write the save, then the resume state <data>/saves/<rom>.<core>.state
 * and wait for it (why goes into the log). core_load() resumes from it. */
void core_state_save(const char *why);
/* Main loop reaped a child. */
void core_child_exited(pid_t pid, int status);

#endif
