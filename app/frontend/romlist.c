/*
 * ROM list: one folder of <data>/roms at a time (the menu scans a folder
 * when it is entered, nothing deeper), its subfolders first by name, then
 * its *.gba files, identified (romdb.c) and sorted by the name shown.
 * Identification reads each file's header; ROMs that need their CRC and
 * are not in the cache show a provisional name until romlist_step has
 * read them, a slice at a time while the folder is shown. The cache is
 * keyed by the path relative to roms/ and pruned per folder: a scan
 * drops only the entries of its own folder that are gone. Up to MAX_ROMS
 * entries per folder, paths up to ROMLIST_PATH bytes (longer ones are
 * skipped and counted in the log).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fe.h"
#include "romlist.h"

#define MAX_ROMS 4096
#define STEP_READ (1 << 18)

static int cmp_roms(const void *a, const void *b)
{
	const struct rom_entry *x = a, *y = b;
	int r;

	if (x->dir != y->dir)
		return y->dir - x->dir;
	r = x->dir ? 0 : strcasecmp(x->id.title, y->id.title);

	if (!r)
		r = strcasecmp(x->file, y->file);
	return r ? r : strcmp(x->file, y->file);
}

static int is_rom(const char *name)
{
	return name[0] != '.' && (size_t)romdb_stem_len(name) < strlen(name);
}

static void log_summary(const struct romlist *rl, const char *what,
			int64_t ns)
{
	int n[ROMDB_HOW_COUNT] = { 0 }, cached = 0;

	for (int i = 0; i < rl->count; i++) {
		if (rl->roms[i].dir)
			continue;
		n[rl->roms[i].id.how]++;
		cached += rl->roms[i].id.have_crc;
	}
	log_msg("romdb: %s %d ROMs in %.1f ms: %d by code, %d by CRC, %d "
		"modified, %d unknown, %d waiting for their CRC (%d CRCs known)",
		what, rl->count - rl->dirs, ns / 1e6, n[ROMDB_CODE], n[ROMDB_CRC],
		n[ROMDB_MODIFIED], n[ROMDB_UNKNOWN], n[ROMDB_PENDING], cached);
}

/* An entry's path; the scan skipped those that do not fit. */
static const char *entry_path(const struct romlist *rl,
			      const struct rom_entry *r, char *buf, size_t size)
{
	if (snprintf(buf, size, "%s/%s", rl->dir, r->file) >= (int)size)
		buf[0] = 0;
	return buf;
}

/* The cache key prefix of the list's folder: "" or "a/b/". */
static const char *key_dir(const struct romlist *rl, char *buf, size_t size)
{
	snprintf(buf, size, "%s/", rl->dir);
	return romdb_key(buf);
}

int romlist_scan(struct romlist *rl, const char *dir)
{
	int64_t t0 = now_ns();
	DIR *d = opendir(dir);
	struct dirent *e;
	char kd[ROMLIST_PATH + 2];
	int cap = 0, too_long = 0, more = 0;

	romlist_free(rl);
	snprintf(rl->dir, sizeof(rl->dir), "%s", dir);
	if (!d)
		return -1;
	/* CRCs computed in a folder left before they were all done. */
	romdb_cache_save(NULL);
	romdb_cache_load();
	while ((e = readdir(d))) {
		struct rom_entry *r;
		char path[ROMLIST_PATH];
		int isdir = e->d_type == DT_DIR;

		if (e->d_name[0] == '.')
			continue;
		if (e->d_type != DT_REG && e->d_type != DT_DIR &&
		    e->d_type != DT_UNKNOWN)
			continue;
		if (!isdir && !is_rom(e->d_name) && e->d_type != DT_UNKNOWN)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >=
		    (int)sizeof(path)) {
			too_long++;
			continue;
		}
		if (e->d_type == DT_UNKNOWN) {
			struct stat st;

			if (stat(path, &st) || !(S_ISDIR(st.st_mode) ||
						 S_ISREG(st.st_mode)))
				continue;
			isdir = S_ISDIR(st.st_mode);
		}
		if (!isdir && !is_rom(e->d_name))
			continue;
		if (rl->count == MAX_ROMS) {
			more++;
			continue;
		}
		if (rl->count == cap) {
			struct rom_entry *nr;

			cap = cap ? cap * 2 : 64;
			nr = realloc(rl->roms, cap * sizeof(*nr));
			if (!nr)
				break;
			rl->roms = nr;
		}
		r = &rl->roms[rl->count];
		r->file = strdup(e->d_name);
		if (!r->file)
			continue;
		r->dir = isdir;
		if (isdir) {
			memset(&r->id, 0, sizeof(r->id));
			r->id.how = ROMDB_UNKNOWN;
			snprintf(r->id.title, sizeof(r->id.title), "%.*s/",
				 ROMDB_NAME_MAX, r->file);
			rl->dirs++;
		} else {
			romdb_identify(path, 0, &r->id);
			romdb_log(romdb_key(path), &r->id);
			rl->pending += r->id.how == ROMDB_PENDING;
		}
		rl->count++;
	}
	closedir(d);
	if (too_long || more)
		log_msg("romlist: %s: %d entries with a path over %d bytes and "
			"%d over the limit of %d not listed", dir, too_long,
			ROMLIST_PATH - 1, more, MAX_ROMS);
	if (rl->count)
		qsort(rl->roms, rl->count, sizeof(*rl->roms), cmp_roms);
	if (rl->count > rl->dirs)
		log_summary(rl, "scanned", now_ns() - t0);
	if (!rl->pending)
		romdb_cache_save(key_dir(rl, kd, sizeof(kd)));
	return rl->count;
}

/* The next pending entry's CRC, or the end of the current one. */
int romlist_step(struct romlist *rl, int64_t budget_ns)
{
	static uint8_t buf[STEP_READ];
	int64_t t0 = now_ns(), dt;
	struct rom_entry *r;
	char path[ROMLIST_PATH], kd[ROMLIST_PATH + 2];
	const char *key;
	ssize_t n;

	while (rl->pending && now_ns() - t0 < budget_ns) {
		if (!rl->job) {
			for (int i = 0; i < rl->count && !rl->job; i++)
				if (rl->roms[i].id.how == ROMDB_PENDING)
					rl->job = i + 1;
			if (!rl->job) {
				rl->pending = 0;
				break;
			}
			rl->job_fd = open(entry_path(rl, &rl->roms[rl->job - 1],
						     path, sizeof(path)),
					  O_RDONLY | O_CLOEXEC);
			rl->job_crc = 0;
			rl->job_bytes = 0;
			rl->job_ns = 0;
		}
		r = &rl->roms[rl->job - 1];
		n = rl->job_fd < 0 ? -1 : read(rl->job_fd, buf, sizeof(buf));
		if (n > 0) {
			rl->job_crc = romdb_crc32(rl->job_crc, buf, n);
			rl->job_bytes += n;
			continue;
		}
		key = romdb_key(entry_path(rl, r, path, sizeof(path)));
		if (n == 0)
			romdb_resolve(key, &r->id, rl->job_crc);
		else {
			log_msg("romdb: %s: cannot read for its CRC (%s)", key,
				strerror(errno));
			r->id.how = ROMDB_UNKNOWN;
		}
		if (rl->job_fd >= 0)
			close(rl->job_fd);
		dt = now_ns() - t0;
		rl->job_ns += dt;
		rl->crc_ns += dt;
		romdb_log(key, &r->id);
		if (r->id.how == ROMDB_MODIFIED)
			log_msg("romdb: %s: modified ROM (crc %08x), shown as %s",
				key, r->id.crc, r->id.title);
		log_verbose("romdb: %s: CRC of %llu KiB in %.1f ms", key,
			    (unsigned long long)rl->job_bytes / 1024,
			    rl->job_ns / 1e6);
		rl->job = 0;
		rl->pending--;
		qsort(rl->roms, rl->count, sizeof(*rl->roms), cmp_roms);
		if (!rl->pending) {
			romdb_cache_save(key_dir(rl, kd, sizeof(kd)));
			log_summary(rl, "CRCs done,", rl->crc_ns);
		}
		return 1;
	}
	dt = now_ns() - t0;
	rl->job_ns += dt;
	rl->crc_ns += dt;
	return 0;
}

void romlist_free(struct romlist *rl)
{
	if (rl->job && rl->job_fd >= 0)
		close(rl->job_fd);
	for (int i = 0; i < rl->count; i++)
		free(rl->roms[i].file);
	free(rl->roms);
	rl->roms = NULL;
	rl->count = 0;
	rl->dirs = 0;
	rl->pending = 0;
	rl->job = 0;
	rl->crc_ns = 0;
}
