/*
 * Resume on boot (resume.c): the record config/resume.txt that makes the
 * next boot start the game that was running when the device was powered
 * off, from its resume state.
 */
#ifndef RESUME_H
#define RESUME_H

#include <stddef.h>

#include "romlist.h"

/* Seconds a resumed game has to run before its record is removed (until
 * then a crash or a pulled battery shows the ROM list on the next boot). */
#define RESUME_OK_S 5

struct resume_rec {
	char rom[ROMLIST_PATH];  /* relative to roms/, with its folders */
	int core;                /* enum core_id the state was written with */
	int attempted;           /* a boot started it and it did not run on */
	char why[64];            /* how the device was powered off */
};

/* Why a record is not used at boot; RESUME_GO: start the game. */
enum resume_check {
	RESUME_GO,
	RESUME_NONE,       /* no record: the last session did not end in game */
	RESUME_BAD,        /* unreadable or damaged */
	RESUME_OFF,        /* the setting is off */
	RESUME_ATTEMPTED,  /* the last boot's resume did not run RESUME_OK_S */
	RESUME_NO_ROM,     /* the ROM is gone */
	RESUME_CORE,       /* the game's core setting changed since */
	RESUME_NO_STATE,   /* no state file for that core */
	RESUME_ESCAPE,     /* B held at boot (main.c) */
	RESUME_FAILED,     /* the game did not start or its state not load */
};
extern const char *const resume_check_names[];

int resume_parse(struct resume_rec *r, const char *text);
int resume_format(const struct resume_rec *r, char *buf, size_t size);
/* config/resume.txt: 0, -1 when there is none, -2 unreadable or damaged. */
int resume_read(struct resume_rec *r);
/* Atomically; 0 or -1. */
int resume_write(const struct resume_rec *r);
/* Remove the record if there is one, logged with why. */
void resume_clear(const char *why);
/* <data>/roms/<r->rom> into buf; -1 when it does not fit. */
int resume_rom_path(const struct resume_rec *r, char *buf, size_t size);
/* The ROM's path relative to roms/ (rom_path under <data>/roms/), or -1. */
int resume_rom_rel(const char *rom_path, char *buf, size_t size);
/* Everything but the escape key and the state's contents (checked when
 * the game loads): the record, the setting (on), the attempted mark, the
 * ROM, the core its settings pick and the state file. Loads and resets
 * rom_settings. */
enum resume_check resume_check(int read_result, const struct resume_rec *r,
			       int on);

#endif
