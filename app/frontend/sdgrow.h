#ifndef SDGROW_H
#define SDGROW_H

#include <stddef.h>
#include <stdint.h>

/*
 * First boot: the GBA partition (p3, 64 MiB in the image) is grown to
 * the end of the SD card and formatted FAT32, its files carried over
 * through a tmpfs. See sdgrow.c for the steps and power loss handling.
 */

/* Called with a short status text whenever a long step starts (only
 * when there is work to do), for a message on screen. */
typedef void (*sdgrow_show_fn)(const char *msg);

/* As PID 1, before the GBA partition is mounted: finds the card from the
 * root device, grows p3 if there is free space after it or finishes an
 * interrupted attempt. Quick when there is nothing to do. */
void sdgrow_boot(sdgrow_show_fn show);

/*
 * The same on a card image file, for tests on a PC: the file system is
 * read and written through `hook extract|inject IMG OFFSET DIR` (mtools)
 * instead of mounts, scratch holds the directories. FE_SDGROW_STOP=<step>
 * exits at that step as a power loss would, FE_SDGROW_FAIL=verify makes
 * the check after the copy fail. Returns 0 when grown or nothing to do.
 */
int sdgrow_image(const char *img, const char *scratch, const char *hook);

/* ---- parts with unit tests (selftest.c) */

struct sdgrow_plan {
	uint64_t card;      /* sectors of 512 bytes */
	uint32_t start;     /* p3 as in the MBR */
	uint32_t size;
	uint32_t end;       /* where p3 can end (exclusive), 4 MiB aligned */
	uint32_t hdr_lba;   /* the progress header, last 4 KiB before end */
};
/* Checks the layout (p3 FAT, last, after p2); 0 or -1 with why. */
int sdgrow_plan(const uint8_t *mbr, uint64_t card, struct sdgrow_plan *p,
		char *why, size_t n);

struct fat32_geom {
	uint32_t sectors;     /* of the partition */
	uint32_t reserved;    /* boot area, padded to align the data area */
	uint32_t fat_sectors; /* per FAT, two FATs */
	uint32_t spc;         /* sectors per cluster */
	uint32_t clusters;
	uint32_t data;        /* first data sector, from the partition start */
};
/* Windows' cluster sizes, smaller when needed for a valid FAT32; the
 * data area 4 MiB aligned on the card. 0 or -1 if too small. */
int fat32_geometry(uint32_t part_lba, uint32_t sectors, struct fat32_geom *g);
/* Writes the file system; fd is the whole card. 0 or an errno value. */
int fat32_format(int fd, uint32_t part_lba, const struct fat32_geom *g,
		 uint32_t volume_id, const char *label);

uint32_t sdgrow_crc32(uint32_t crc, const void *buf, size_t len);

struct sdgrow_tree {
	unsigned files, dirs, skipped;
	uint64_t bytes;
};
/* Files and directories of src into dst (existing), with times and the
 * read-only bit; 0 or -1 (logged). */
int sdgrow_copy_tree(const char *src, const char *dst, struct sdgrow_tree *t);
/* Same names, types, sizes, content and mtimes (FAT's 2 s) in b as in a,
 * nothing more; 0 or -1 (logged). */
int sdgrow_compare_tree(const char *a, const char *b, struct sdgrow_tree *t);

#endif
