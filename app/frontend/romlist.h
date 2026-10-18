#ifndef ROMLIST_H
#define ROMLIST_H

#include <stdint.h>

#include "romdb.h"

struct rom_entry {
	char *file;         /* file name */
	struct rom_id id;   /* id.title is shown */
};

struct romlist {
	struct rom_entry *roms;  /* sorted by title, case-insensitively */
	int count;
	int pending;     /* entries still waiting for their CRC */
	/* The CRC being computed by romlist_step: entry job - 1, 0: none. */
	int job, job_fd;
	uint32_t job_crc;
	uint64_t job_bytes;
	int64_t job_ns, crc_ns;  /* this one, all of them */
	char dir[300];
};

/* List *.gba files in dir (no hidden or AppleDouble "._" files) and
 * identify them from their headers and the CRC cache (romdb.c). */
int romlist_scan(struct romlist *rl, const char *dir);
/* Compute missing CRCs for up to budget_ns; returns 1 when an entry got
 * its final name (the list is sorted again, indexes change). */
int romlist_step(struct romlist *rl, int64_t budget_ns);
void romlist_free(struct romlist *rl);

#endif
