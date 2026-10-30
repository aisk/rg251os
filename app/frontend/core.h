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
/* The core a ROM file runs with: the video player for a video (by its
 * extension, romdb_is_video), otherwise rom_settings.core (so load the
 * ROM's settings first). */
int core_for_rom(const char *rom_name);
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
/* A setting behind a core option changed (core.c, rom_options[], and
 * the global video_scale for the video player): one the running core
 * takes live is read again after the next retro_run, the others wait for
 * the next start or are kept for the other core; each logged. */
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
 * and wait for it (why goes into the log); 0 when the state was written.
 * core_load() resumes from it. */
int core_state_save(const char *why);
/* The payload of the state file core_load would resume a ROM from with
 * a core, into buf when it is at most size bytes and passes the checks
 * that do not need the game loaded (magic, core, length, payload CRC, the
 * ROM's size); returns its length or -1. */
int core_state_peek(const char *rom_path, int core, void *buf, size_t size);
/* The game loaded resumed from its state. */
int core_state_resumed(void);
/* The state file core_load would use for a ROM with a core (enum
 * core_id); -1 when it does not fit. */
int core_state_path(const char *rom_path, int core, char *buf, size_t size);
/* Main loop reaped a child. */
void core_child_exited(pid_t pid, int status);

/* The video player's decoder thread (video_core.c), for tests: lockstep
 * makes retro_run wait (up to 5 s) for what it is about to show and play,
 * so runs back to back give the same result as paced ones; delay_ms
 * slows every picture the worker decodes. Kept across loads. */
void video_core_test(int lockstep, int delay_ms);
struct video_counters {
	unsigned decoded, shown, dropped, late_pics, late_runs, dry_runs;
	unsigned reconverted, discarded, seeks;
	int waiting;  /* the worker is blocked on its condition variable */
};
void video_core_counters(struct video_counters *k);

/* The video player as the frontend's OSD shows it (all 0 when no video
 * is loaded): the position (where a seek under way goes), the duration
 * (0: unknown), paused, ended, a seek under way, and a count of the
 * controls acted on (A, seeks with their repeats, L/R, restart) that
 * changes whenever the OSD should show. */
struct video_status {
	int loaded;
	double pos, duration;
	int paused, ended, buffering;
	unsigned events;
};
void video_core_status(struct video_status *s);
/* What the player asks the frontend for, taken once: L, R, or the end of
 * the video (main.c starts the previous/next video of the folder). */
enum video_request {
	VIDEO_REQ_NONE, VIDEO_REQ_PREV, VIDEO_REQ_NEXT, VIDEO_REQ_END,
};
int video_core_take_request(void);
/* The play position a video's resume state holds (0 after its end), or
 * -1 without a usable state (for the ROM list). */
double video_core_saved_pos(const char *path);

#endif
