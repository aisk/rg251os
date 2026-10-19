#ifndef ROMLIST_H
#define ROMLIST_H

#include <stdint.h>

#include "romdb.h"

/* Room for a folder or ROM path (main.c's ROM path has the same size). */
#define ROMLIST_PATH 512

struct rom_entry {
	char *file;         /* file or folder name */
	int dir;            /* a subfolder: id.title is "name/" */
	struct rom_id id;   /* id.title is shown */
};

struct romlist {
	/* Folders first, by name, then ROMs by title, case-insensitively. */
	struct rom_entry *roms;
	int count;
	int dirs;        /* folders among them */
	int pending;     /* entries still waiting for their CRC */
	/* The CRC being computed by romlist_step: entry job - 1, 0: none. */
	int job, job_fd;
	uint32_t job_crc;
	uint64_t job_bytes;
	int64_t job_ns, crc_ns;  /* this one, all of them */
	char dir[ROMLIST_PATH];
};

/* List the subfolders and *.gba files of dir (no hidden or AppleDouble
 * "._" entries), not recursing, and identify the ROMs from their headers
 * and the CRC cache (romdb.c). Returns the number of entries, -1 when dir
 * cannot be opened. */
int romlist_scan(struct romlist *rl, const char *dir);
/* Compute missing CRCs for up to budget_ns; returns 1 when an entry got
 * its final name (the list is sorted again, indexes change). */
int romlist_step(struct romlist *rl, int64_t budget_ns);
void romlist_free(struct romlist *rl);

#endif
