/*
 * Settings (settings.c): the global and per game settings kept as
 * key=value text files in <data>/config, and the enums they use (color
 * modes, cores, audio and CPU modes).
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stddef.h>

enum color_mode {
	COLOR_OFF,
	COLOR_GBA,        /* original GBA, unlit reflective screen */
	COLOR_GBA_LIGHT,  /* same colors without the extra gamma darkening */
	COLOR_SP101,      /* backlit GBA SP (AGS-101) */
	COLOR_MODES,      /* the modes fx.c builds */
	/* Per game only: one of the above from the release year (romdb.c),
	 * resolved when the game starts (rom_settings_set_auto). */
	COLOR_AUTO = COLOR_MODES,
	COLOR_COUNT
};
extern const char *const color_mode_names[COLOR_COUNT];   /* in files */
extern const char *const color_mode_labels[COLOR_COUNT];  /* in menus */

/* The libretro cores linked in (core.c), selectable per game. */
enum core_id {
	CORE_MGBA,
	CORE_GPSP,  /* gpSP, ARM64 dynarec: much faster, less accurate */
	CORE_COUNT
};
extern const char *const core_names[CORE_COUNT];   /* in files */
extern const char *const core_labels[CORE_COUNT];  /* in menus */

/* rom_settings.frameskip: Auto skips a frame when the audio buffer runs
 * low (gpSP only), N renders one frame and skips the next N (core.c). */
enum frameskip {
	FRAMESKIP_OFF,
	FRAMESKIP_AUTO,
	FRAMESKIP_1,
	FRAMESKIP_2,
	FRAMESKIP_3,
	FRAMESKIP_COUNT
};
extern const char *const frameskip_names[FRAMESKIP_COUNT];   /* in files */
extern const char *const frameskip_labels[FRAMESKIP_COUNT];  /* in menus */

/* settings.audio (see audio.c) */
#define AUDIO_SIMPLE 0    /* blocking writes, fixed resampling ratio */
#define AUDIO_ADAPTIVE 1  /* nonblocking writes, rate control */
/* settings.cpu, otherwise a fixed frequency in MHz (see cpuctl.c) */
#define CPU_MODE_ADAPTIVE 0
#define CPU_MODE_MAX 1

/* settings.brightness: the panel shows nothing at the lowest backlight
 * levels, so the user setting never goes below this percent (settings.c
 * raises a lower saved value, backlight_set clamps). */
#define BRIGHTNESS_MIN 25

/* Global, in config/settings.txt. */
struct settings {
	int volume;      /* raw "Master Playback Volume", 0..255 */
	int brightness;  /* backlight percent, BRIGHTNESS_MIN..100 */
	int grid;        /* LCD pixel grid effect on/off */
	int grid_dim;    /* darkening of the 3 unlit pixels of a cell, percent */
	int grid_boost;  /* brightening of the lit pixel, percent */
	int cpu;         /* CPU_MODE_ADAPTIVE, CPU_MODE_MAX or MHz */
	int audio;       /* AUDIO_SIMPLE or AUDIO_ADAPTIVE (audio.c) */
	int debug;       /* debug logging (see log.c), off for normal play */
};
/* Per game, in config/roms/<rom name>.txt. */
struct rom_settings {
	int color;       /* enum color_mode, COLOR_AUTO by default */
	int cpu_start;   /* learned start OPP in MHz, 0: the top */
	int core;        /* enum core_id, used when the game starts */
	int blend;       /* gpSP frame blending (core.c), mGBA ignores it */
	int frameskip;   /* enum frameskip (core.c), off by default */
};
extern struct settings settings;
extern struct rom_settings rom_settings;  /* for the running game */
extern const struct settings settings_defaults;

void settings_load(void);
void settings_save(void);
int settings_parse(struct settings *s, const char *text);
int settings_format(const struct settings *s, char *buf, size_t size);
/* Defaults for a ROM, then its file. NULL: no game (defaults only). */
void rom_settings_load(const char *rom_name);
void rom_settings_save(void);
/* The game's automatic color mode and its reason (romdb_auto_color),
 * logged; rom_settings_load resets it to off. */
void rom_settings_set_auto(int color, const char *why);
/* The mode in effect: rom_settings.color with auto resolved. */
int rom_settings_color(void);
/* The menu label: "GBA", or "Auto (GBA)" for auto. */
void rom_color_label(char *buf, size_t size);
/* The defaults for a ROM (today the same for every ROM). */
void rom_settings_defaults(const char *rom_name, struct rom_settings *r);
int rom_settings_parse(struct rom_settings *r, const char *text);

#endif
