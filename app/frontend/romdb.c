/*
 * ROM identification: the cartridge header and the No-Intro table in
 * romdb_data.c (generated, see mk/romdb-gen.py), for the names in the ROM
 * list and the automatic color correction.
 *
 * Matching. The game code (0xAC) and the software version (0xBC) come
 * from the header, so most ROMs are identified from 192 bytes; that also
 * keeps the name for translations and hacks that leave the header alone.
 * When several entries share the code and version (revisions No-Intro
 * tells apart only by content, Virtual Console and collection builds,
 * unrelated prototypes reusing a code), the CRC32 of the whole file
 * decides. It is computed once and kept in config/romdb-cache.txt, keyed
 * by the path relative to <data>/roms (romdb_key: the bare file name for
 * ROMs directly in roms/, as before folders existed, so older caches
 * still match), size and modification time; the ROM list computes missing
 * ones a slice at a time while it shows their folder (romlist.c), a game
 * start computes its own. A CRC matching none of the code's entries means a
 * modified ROM: it gets the most likely entry's name with " *" after it.
 * ROMs whose code is not in the table can still be found by CRC among the
 * entries without a code (homebrew, cheat devices) when one of them has
 * the same size; anything else shows its file name.
 *
 * Automatic color correction (color=auto, the per game default), from the
 * first release year of the title (the generator takes the earliest year
 * of the entry, its other regions and its re-releases):
 *
 *   before 2005   gba     made for the unlit reflective screen of the GBA
 *                         and the frontlit SP (AGS-001): bright, strongly
 *                         saturated palettes, fx.c's full GBA model
 *   2005 on       sp101   the backlit SP (AGS-101, 2005) and the DS
 *                         were the screens of the time; their milder
 *                         model
 *   unknown       off     no year (homebrew, unidentified ROMs, entries
 *                         without data): the picture as the core makes
 *                         it, as before color=auto existed
 *
 * with the exceptions in color_exceptions[] (per game, by the first three
 * letters of the header's game code, all regions) taking precedence.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ARM_FEATURE_CRC32
#include <arm_acle.h>
#endif

#include "fe.h"
#include "romdb.h"

#define BACKLIT_YEAR 2005

const char *const romdb_how_names[ROMDB_HOW_COUNT] = {
	"unknown", "code", "crc", "modified", "pending",
};

/*
 * Games whose automatic choice differs from the year rule. Only entries
 * with a documented reason, cited in a comment, belong here; none has
 * been found yet, so the year rule decides for every game.
 */
static const struct color_exception *const color_exceptions = NULL;
static const int color_exception_count = 0;

/* ------------------------------------------------------------ header */

static int code_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int romdb_parse_header(const uint8_t *buf, size_t len, struct gba_header *h)
{
	int n;

	memset(h, 0, sizeof(*h));
	if (len < 0xc0)
		return -1;
	memcpy(h->title, buf + 0xa0, 12);
	for (n = 12; n > 0 && (h->title[n - 1] == ' ' || !h->title[n - 1]); n--)
		h->title[n - 1] = 0;
	for (int i = 0; i < n; i++)
		if ((unsigned char)h->title[i] < 32 || (unsigned char)h->title[i] > 126)
			h->title[i] = '?';
	memcpy(h->code, buf + 0xac, 4);
	for (int i = 0; i < 4; i++)
		if (!code_char(h->code[i]))
			h->code[0] = 0;
	memcpy(h->maker, buf + 0xb0, 2);
	for (int i = 0; i < 2; i++)
		if (!code_char(h->maker[i]))
			h->maker[0] = 0;
	h->version = buf[0xbc];
	if (buf[0xb2] != 0x96) {
		h->code[0] = 0;
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------- table */

const char *romdb_name(const struct romdb_entry *e)
{
	return romdb_names + e->name;
}

int romdb_year(const struct romdb_entry *e)
{
	int y = e->year & ~YEAR_INHERITED;

	return y ? 2000 + y : 0;
}

/* The entries with this code: first index, count. */
static int code_range(const char *code, int *first)
{
	int lo = 0, hi = romdb_count, end;

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (strcmp(romdb_entries[mid].code, code) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (end = lo; end < romdb_count && !strcmp(romdb_entries[end].code, code);
	     end++)
		;
	*first = lo;
	return end - lo;
}

static void file_title(const char *file, char *buf, size_t size)
{
	size_t n = strlen(file);

	if (n > 4 && !strcasecmp(file + n - 4, ".gba"))
		n -= 4;
	snprintf(buf, size, "%.*s", (int)n, file);
}

/* What candidates have in common, cut back to whole words; "" if
 * nothing. */
static void common_title(const struct romdb_entry *c, int n, char *buf,
			 size_t size)
{
	const char *first = romdb_name(c);
	size_t len = strlen(first);

	for (int i = 1; i < n; i++) {
		const char *s = romdb_name(&c[i]);
		size_t k = 0;

		while (k < len && s[k] == first[k])
			k++;
		if (k < len && k < strlen(s)) {
			/* Inside a word or a parenthesis: back to the last
			 * complete one. */
			while (k > 0 && first[k - 1] != ' ')
				k--;
		}
		len = k;
	}
	while (len > 0 && (first[len - 1] == ' ' || first[len - 1] == '('))
		len--;
	snprintf(buf, size, "%.*s", (int)len, first);
}

void romdb_match(const char *file, const struct gba_header *h, uint64_t size,
		 const uint32_t *crc, struct rom_id *id)
{
	const struct romdb_entry *c = NULL;
	int lo = 0, n = 0, cn = 0;

	if (h != &id->hdr)
		id->hdr = *h;
	id->how = ROMDB_UNKNOWN;
	id->e = NULL;
	if (h->code[0])
		n = code_range(h->code, &lo);
	if (n) {
		/* Sorted by version within a code. */
		for (int i = 0; i < n; i++) {
			const struct romdb_entry *e = &romdb_entries[lo + i];

			if (e->version != h->version)
				continue;
			if (!cn)
				c = e;
			cn++;
		}
		/* A version No-Intro does not list: only the CRC can tell
		 * a known dump (Rev numbering differs) from a modified one. */
		int any = !cn;

		if (any) {
			c = &romdb_entries[lo];
			cn = n;
		}
		id->e = c;
		if (cn == 1 && !any) {
			id->how = ROMDB_CODE;
		} else if (!crc) {
			id->how = ROMDB_PENDING;
		} else {
			id->how = ROMDB_MODIFIED;
			for (int i = 0; i < n; i++)
				if (romdb_entries[lo + i].crc == *crc) {
					id->how = ROMDB_CRC;
					id->e = &romdb_entries[lo + i];
					break;
				}
			/* Best guess for a modified ROM: one of the same size. */
			for (int i = 0; id->how == ROMDB_MODIFIED && i < cn; i++)
				if (c[i].size == size) {
					id->e = &c[i];
					break;
				}
		}
	} else {
		/* Unknown code: the entries without one, if the size fits. */
		n = code_range("", &lo);
		for (int i = 0; i < n; i++) {
			const struct romdb_entry *e = &romdb_entries[lo + i];

			if (e->size != size)
				continue;
			if (!crc) {
				id->how = ROMDB_PENDING;
				break;
			}
			if (e->crc == *crc) {
				id->how = ROMDB_CRC;
				id->e = e;
				break;
			}
		}
	}

	switch (id->how) {
	case ROMDB_CODE:
	case ROMDB_CRC:
		snprintf(id->title, sizeof(id->title), "%s", romdb_name(id->e));
		break;
	case ROMDB_MODIFIED:
		snprintf(id->title, sizeof(id->title), "%s *", romdb_name(id->e));
		break;
	case ROMDB_PENDING:
		id->title[0] = 0;
		if (c)
			common_title(c, cn, id->title, sizeof(id->title));
		if (!id->title[0])
			file_title(file, id->title, sizeof(id->title));
		break;
	default:
		file_title(file, id->title, sizeof(id->title));
	}
}

/* -------------------------------------------------------------- CRC32 */

#ifndef __ARM_FEATURE_CRC32
static uint32_t crc_table[256];

static void crc_table_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}
#endif

/* The ARMv8 CRC32 instructions use zlib's polynomial: several GB/s on
 * the A35, so reading the file is all the cost. */
uint32_t romdb_crc32(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
#ifdef __ARM_FEATURE_CRC32
	while (len && ((uintptr_t)p & 7)) {
		crc = __crc32b(crc, *p++);
		len--;
	}
	for (; len >= 8; len -= 8, p += 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = __crc32d(crc, v);
	}
	while (len--)
		crc = __crc32b(crc, *p++);
#else
	if (!crc_table[1])
		crc_table_init();
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
#endif
	return ~crc;
}

int romdb_crc_file(const char *path, uint32_t *crc)
{
	static uint8_t buf[1 << 18];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	*crc = 0;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		*crc = romdb_crc32(*crc, buf, n);
	close(fd);
	return n < 0 ? -1 : 0;
}

/* -------------------------------------------------------------- cache */

struct cache_ent {
	char *file;
	uint64_t size;
	int64_t mtime;
	uint32_t crc;
	int seen;
};

static struct {
	struct cache_ent *e;
	int count, cap;
	int loaded, dirty;
} cache;

static void cache_path(char *buf, size_t size)
{
	snprintf(buf, size, "%s/config/romdb-cache.txt", fe_data_dir);
}

void romdb_cache_reset(void)
{
	for (int i = 0; i < cache.count; i++)
		free(cache.e[i].file);
	free(cache.e);
	memset(&cache, 0, sizeof(cache));
}

static void cache_add(const char *file, uint64_t size, int64_t mtime,
		      uint32_t crc, int seen)
{
	struct cache_ent *e;

	for (int i = 0; i < cache.count; i++) {
		e = &cache.e[i];
		if (!strcmp(e->file, file)) {
			e->size = size;
			e->mtime = mtime;
			e->crc = crc;
			e->seen |= seen;
			return;
		}
	}
	if (cache.count == cache.cap) {
		int cap = cache.cap ? cache.cap * 2 : 64;
		struct cache_ent *ne = realloc(cache.e, cap * sizeof(*ne));

		if (!ne)
			return;
		cache.e = ne;
		cache.cap = cap;
	}
	e = &cache.e[cache.count];
	e->file = strdup(file);
	if (!e->file)
		return;
	e->size = size;
	e->mtime = mtime;
	e->crc = crc;
	e->seen = seen;
	cache.count++;
}

void romdb_cache_load(void)
{
	char path[300], *line = NULL;
	size_t cap = 0;
	ssize_t len;
	FILE *f;

	romdb_cache_reset();
	cache.loaded = 1;
	cache_path(path, sizeof(path));
	f = fopen(path, "re");
	if (!f)
		return;
	while ((len = getline(&line, &cap, f)) > 0) {
		unsigned long long size;
		long long mtime;
		unsigned crc;
		int off = 0;

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		if (line[0] == '#')
			continue;
		if (sscanf(line, "%8x %llu %lld %n", &crc, &size, &mtime, &off) == 3 &&
		    off > 0 && line[off])
			cache_add(line + off, size, mtime, crc, 0);
	}
	free(line);
	fclose(f);
}

int romdb_cache_get(const char *file, uint64_t size, int64_t mtime,
		    uint32_t *crc)
{
	if (!cache.loaded)
		romdb_cache_load();
	for (int i = 0; i < cache.count; i++) {
		struct cache_ent *e = &cache.e[i];

		if (strcmp(e->file, file))
			continue;
		if (e->size != size || e->mtime != mtime)
			return -1;
		e->seen = 1;
		*crc = e->crc;
		return 0;
	}
	return -1;
}

void romdb_cache_put(const char *file, uint64_t size, int64_t mtime,
		     uint32_t crc)
{
	if (!cache.loaded)
		romdb_cache_load();
	cache_add(file, size, mtime, crc, 1);
	cache.dirty = 1;
}

/* A key directly in the folder prefix ("" or "a/b/"), not deeper. */
static int in_folder(const char *file, const char *prefix)
{
	size_t n = strlen(prefix);

	return !strncmp(file, prefix, n) && !strchr(file + n, '/');
}

void romdb_cache_save(const char *prune)
{
	char path[300], *buf;
	size_t n = 0, cap = 64;
	int kept = 0;

	if (!cache.loaded)
		return;
	for (int i = 0; i < cache.count; i++) {
		if (prune && !cache.e[i].seen && in_folder(cache.e[i].file, prune))
			cache.dirty = 1;
		cap += strlen(cache.e[i].file) + 48;
	}
	if (!cache.dirty)
		return;
	buf = malloc(cap);
	if (!buf)
		return;
	n += snprintf(buf, cap, "# ROM CRC32 cache: crc size mtime file\n");
	for (int i = 0; i < cache.count; i++) {
		const struct cache_ent *e = &cache.e[i];

		if (prune && !e->seen && in_folder(e->file, prune))
			continue;
		n += snprintf(buf + n, cap - n, "%08x %llu %lld %s\n", e->crc,
			      (unsigned long long)e->size, (long long)e->mtime,
			      e->file);
		kept++;
	}
	cache_path(path, sizeof(path));
	if (write_file_atomic(path, buf, n) == 0) {
		cache.dirty = 0;
		log_verbose("romdb: cache saved, %d entries", kept);
	}
	free(buf);
}

/* ----------------------------------------------------------- identify */

const char *romdb_key(const char *path)
{
	size_t n = strlen(fe_data_dir);
	const char *s = strrchr(path, '/');

	if (!strncmp(path, fe_data_dir, n) && !strncmp(path + n, "/roms/", 6))
		return path + n + 6;
	return s ? s + 1 : path;
}

int romdb_identify(const char *path, int allow_crc, struct rom_id *id)
{
	const char *file = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	const char *key = romdb_key(path);
	uint8_t buf[0xc0];
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC), err = 0;
	ssize_t n = -1;

	memset(id, 0, sizeof(*id));
	if (fd >= 0) {
		if (fstat(fd, &st) == 0) {
			id->size = (uint64_t)st.st_size;
			id->mtime = (int64_t)st.st_mtime;
			n = read(fd, buf, sizeof(buf));
		}
		close(fd);
	}
	if (n < 0)
		err = -1;
	id->hdr_ok = n > 0 && romdb_parse_header(buf, n, &id->hdr) == 0;
	if (!id->hdr_ok)
		memset(&id->hdr, 0, sizeof(id->hdr));
	romdb_match(file, &id->hdr, id->size, NULL, id);
	if (id->how != ROMDB_PENDING)
		return err;
	if (romdb_cache_get(key, id->size, id->mtime, &id->crc) == 0) {
		id->have_crc = 1;
		romdb_match(file, &id->hdr, id->size, &id->crc, id);
	} else if (allow_crc && romdb_crc_file(path, &id->crc) == 0) {
		romdb_resolve(key, id, id->crc);
		romdb_cache_save(NULL);
	}
	return err;
}

void romdb_resolve(const char *key, struct rom_id *id, uint32_t crc)
{
	const char *file = strrchr(key, '/') ? strrchr(key, '/') + 1 : key;

	id->crc = crc;
	id->have_crc = 1;
	romdb_cache_put(key, id->size, id->mtime, crc);
	romdb_match(file, &id->hdr, id->size, &id->crc, id);
}

void romdb_log(const char *file, const struct rom_id *id)
{
	char crc[16] = "";

	if (id->have_crc)
		snprintf(crc, sizeof(crc), " crc %08x", id->crc);
	log_verbose("romdb: %s: header \"%s\" %s %s v%u%s: %s, \"%s\"%s", file,
		    id->hdr.title, id->hdr.code[0] ? id->hdr.code : "-",
		    id->hdr.maker[0] ? id->hdr.maker : "-", id->hdr.version, crc,
		    romdb_how_names[id->how],
		    id->title, id->e && romdb_year(id->e) ? "" : ", no year");
}

/* --------------------------------------------------------- auto color */

int romdb_auto_color_with(const struct rom_id *id,
			  const struct color_exception *ex, int nex,
			  char *why, size_t size)
{
	const char *mod = id->how == ROMDB_MODIFIED ? ", modified ROM" : "";
	int year = id->e ? romdb_year(id->e) : 0;

	for (int i = 0; i < nex && id->hdr.code[0]; i++)
		if (!strncmp(ex[i].code, id->hdr.code, 3)) {
			snprintf(why, size, "exception: %s", ex[i].why);
			return ex[i].color;
		}
	if (!year) {
		snprintf(why, size, "release year unknown");
		return COLOR_OFF;
	}
	snprintf(why, size, "first released %d%s%s", year,
		 id->e->year & YEAR_INHERITED ? " (title)" : "", mod);
	return year < BACKLIT_YEAR ? COLOR_GBA : COLOR_SP101;
}

int romdb_auto_color(const struct rom_id *id, char *why, size_t size)
{
	return romdb_auto_color_with(id, color_exceptions, color_exception_count,
				     why, size);
}
