/*
 * The libretro entry points of one statically linked core. Every core's
 * retro_* symbols are renamed at build time (mk/core-archive.sh:
 * mgba_retro_run, gpsp_retro_run, ...), so several cores live in one
 * binary; the in-tree video player (video_core.c) has its entry points
 * named video_retro_* in its source. cores.c fills one table per core
 * and core.c calls through the table of the core that is loaded. Only one core is initialized at a
 * time.
 */
#ifndef CORE_API_H
#define CORE_API_H

#include <stddef.h>

#include "libretro.h"

struct core_api {
	const char *name;  /* as in core_names[], or "stub" */
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*init)(void);
	void (*deinit)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void (*reset)(void);
	void (*run)(void);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	/* Bytes of the save buffer worth writing; NULL: get_memory_size. */
	size_t (*save_size)(void);
};

/* The table for an enum core_id, or NULL if that core is not built in. */
const struct core_api *core_api_get(int id);

#endif
