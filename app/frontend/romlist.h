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

/* List the subfolders, *.gba and *.mpg files of dir (no hidden or AppleDouble
 * "._" entries), not recursing, and identify the ROMs from their headers
 * and the CRC cache (romdb.c). Returns the number of entries, -1 when dir
 * cannot be opened. */
int romlist_scan(struct romlist *rl, const char *dir);
/* Compute missing CRCs for up to budget_ns; returns 1 when an entry got
 * its final name (the list is sorted again, indexes change). */
int romlist_step(struct romlist *rl, int64_t budget_ns);
void romlist_free(struct romlist *rl);
/* The video before (d < 0) or after (d > 0) the one at path among the
 * videos of its folder, in the list's order (by the name shown), as a
 * path into out: 0, or 1 when path is the first or last one, -1 when the
 * folder cannot be read or path is not a video in it. *index and *count
 * get path's place among them (from 1) and how many there are. */
int romlist_video_neighbor(const char *path, int d, char *out, size_t size,
			   int *index, int *count);

#endif
