#ifndef ROMDB_H
#define ROMDB_H

#include <stddef.h>
#include <stdint.h>

/* The cartridge header fields used for identification. */
struct gba_header {
	char title[13];   /* 0xA0, 12 bytes, trailing NULs and spaces cut */
	char code[5];     /* 0xAC, game code, "" when not 4 of A-Z 0-9 */
	char maker[3];    /* 0xB0 */
	uint8_t version;  /* 0xBC, software version */
};

/* Generated table (romdb_data.c, see mk/romdb-gen.py). */
#define YEAR_INHERITED 0x80  /* year is the title's, not this entry's */
struct romdb_entry {
	uint32_t crc;
	uint32_t size;
	uint32_t name;     /* offset in romdb_names */
	char code[5];      /* "" when No-Intro has no serial */
	uint8_t version;   /* from "(Rev N)" in the name */
	uint8_t year;      /* first release year - 2000, 0: unknown */
};
extern const char romdb_names[];
extern const struct romdb_entry romdb_entries[];
extern const int romdb_count;

#define ROMDB_NAME_MAX 160

enum romdb_how {
	ROMDB_UNKNOWN,   /* not in the database: the file name is shown */
	ROMDB_CODE,      /* game code and version, header only */
	ROMDB_CRC,       /* CRC32 of the file */
	ROMDB_MODIFIED,  /* code known, CRC matches none of its entries */
	ROMDB_PENDING,   /* the CRC is needed and not known yet */
	ROMDB_HOW_COUNT
};
extern const char *const romdb_how_names[ROMDB_HOW_COUNT];

/* One ROM file. */
struct rom_id {
	struct gba_header hdr;
	int hdr_ok;
	uint64_t size;
	int64_t mtime;
	uint32_t crc;
	int have_crc;
	enum romdb_how how;
	const struct romdb_entry *e;  /* the match or best guess, or NULL */
	char title[ROMDB_NAME_MAX + 2];  /* as shown: name, "name *", file */
};

/* 0 when buf (at least 0xC0 bytes) looks like a GBA header (0x96 at
 * 0xB2); h is filled either way. */
int romdb_parse_header(const uint8_t *buf, size_t len, struct gba_header *h);
const char *romdb_name(const struct romdb_entry *e);
int romdb_year(const struct romdb_entry *e);  /* 0: unknown */

/*
 * Match a header: by game code, narrowed by the version when that leaves
 * one entry; otherwise by crc if given (*crc), else ROMDB_PENDING. A code
 * not in the table is looked up by CRC among the entries without a code
 * when one of them has the file's size (PENDING without crc). Fills
 * id->how, id->e and id->title (file: the file name, shown if unknown).
 */
void romdb_match(const char *file, const struct gba_header *h, uint64_t size,
		 const uint32_t *crc, struct rom_id *id);

/* The length of a ROM file name without its ".gba" (any case): the one
 * rule for the name of its save, resume states and settings (core.c,
 * settings.c), the title of an unknown ROM and which files are ROMs
 * (romlist.c: those it shortens, and videos). A video keeps its ".mpg"
 * there, so x.mpg and x.gba never share a file. */
int romdb_stem_len(const char *file);
/* A video for video_core.c: the name ends in ".mpg" (any case). */
int romdb_is_video(const char *file);

/* Header (and the cache when the CRC is needed) for path; computes the
 * CRC when allow_crc and still needed, and caches it. Fills id->title.
 * A video is only stat'ed: ROMDB_UNKNOWN, its name without ".mpg".
 * Returns 0, or -1 when the file cannot be read (title: the file name). */
int romdb_identify(const char *path, int allow_crc, struct rom_id *id);
/* The cache key of a ROM path: relative to <data>/roms ("a/b/x.gba"), or
 * the file name for a path elsewhere. Points into path. */
const char *romdb_key(const char *path);
/* Finish a PENDING id with the file's CRC (also cached under key). */
void romdb_resolve(const char *key, struct rom_id *id, uint32_t crc);
void romdb_log(const char *file, const struct rom_id *id);

/* CRC32 (zlib's), incrementally: crc = romdb_crc32(crc, buf, len), 0 to
 * start. */
uint32_t romdb_crc32(uint32_t crc, const void *buf, size_t len);
int romdb_crc_file(const char *path, uint32_t *crc);

/* CRC cache, <data>/config/romdb-cache.txt: one "crc size mtime key"
 * line per ROM whose CRC was needed (key: see romdb_key). */
void romdb_cache_load(void);
int romdb_cache_get(const char *file, uint64_t size, int64_t mtime,
		    uint32_t *crc);
void romdb_cache_put(const char *file, uint64_t size, int64_t mtime,
		     uint32_t crc);
/* Writes the cache if it changed; prune (a key prefix, "" or "a/b/"):
 * drop the keys directly in that folder not looked up since the last
 * load (after a full scan of the folder), NULL: keep everything. */
void romdb_cache_save(const char *prune);
void romdb_cache_reset(void);  /* forget it (tests) */

/*
 * Automatic color correction (color=auto) for a ROM: see romdb.c.
 * Returns the mode (enum color_mode, not COLOR_AUTO) and its reason.
 */
struct color_exception {
	char code[4];  /* first 3 letters of the game code: all regions */
	int color;
	const char *why;
};
int romdb_auto_color(const struct rom_id *id, char *why, size_t size);
int romdb_auto_color_with(const struct rom_id *id,
			  const struct color_exception *ex, int nex,
			  char *why, size_t size);

#endif
