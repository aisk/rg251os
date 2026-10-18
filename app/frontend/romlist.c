/*
 * ROM list: *.gba files in <data>/roms, identified (romdb.c) and sorted
 * by the name shown. Identification reads each file's header; ROMs that
 * need their CRC and are not in the cache show a provisional name until
 * romlist_step has read them, a slice at a time while the list is shown.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fe.h"
#include "romlist.h"

#define MAX_ROMS 4096
#define STEP_READ (1 << 18)

static int cmp_roms(const void *a, const void *b)
{
	const struct rom_entry *x = a, *y = b;
	int r = strcasecmp(x->id.title, y->id.title);

	if (!r)
		r = strcasecmp(x->file, y->file);
	return r ? r : strcmp(x->file, y->file);
}

static int is_rom(const char *name)
{
	size_t n = strlen(name);

	if (name[0] == '.' || n < 5)
		return 0;
	return !strcasecmp(name + n - 4, ".gba");
}

static void log_summary(const struct romlist *rl, const char *what,
			int64_t ns)
{
	int n[ROMDB_HOW_COUNT] = { 0 }, cached = 0;

	for (int i = 0; i < rl->count; i++) {
		n[rl->roms[i].id.how]++;
		cached += rl->roms[i].id.have_crc;
	}
	log_msg("romdb: %s %d ROMs in %.1f ms: %d by code, %d by CRC, %d "
		"modified, %d unknown, %d waiting for their CRC (%d CRCs known)",
		what, rl->count, ns / 1e6, n[ROMDB_CODE], n[ROMDB_CRC],
		n[ROMDB_MODIFIED], n[ROMDB_UNKNOWN], n[ROMDB_PENDING], cached);
}

int romlist_scan(struct romlist *rl, const char *dir)
{
	int64_t t0 = now_ns();
	DIR *d = opendir(dir);
	struct dirent *e;
	int cap = 0;

	romlist_free(rl);
	snprintf(rl->dir, sizeof(rl->dir), "%s", dir);
	if (!d)
		return -1;
	romdb_cache_load();
	while ((e = readdir(d)) && rl->count < MAX_ROMS) {
		struct rom_entry *r;
		char path[600];

		if (e->d_type != DT_REG && e->d_type != DT_UNKNOWN)
			continue;
		if (!is_rom(e->d_name))
			continue;
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
		snprintf(path, sizeof(path), "%s/%s", dir, r->file);
		romdb_identify(path, 0, &r->id);
		romdb_log(r->file, &r->id);
		rl->pending += r->id.how == ROMDB_PENDING;
		rl->count++;
	}
	closedir(d);
	if (rl->count)
		qsort(rl->roms, rl->count, sizeof(*rl->roms), cmp_roms);
	if (rl->count)
		log_summary(rl, "scanned", now_ns() - t0);
	if (!rl->pending)
		romdb_cache_save(1);
	return rl->count;
}

/* The next pending entry's CRC, or the end of the current one. */
int romlist_step(struct romlist *rl, int64_t budget_ns)
{
	static uint8_t buf[STEP_READ];
	int64_t t0 = now_ns(), dt;
	struct rom_entry *r;
	ssize_t n;

	while (rl->pending && now_ns() - t0 < budget_ns) {
		if (!rl->job) {
			char path[600];

			for (int i = 0; i < rl->count && !rl->job; i++)
				if (rl->roms[i].id.how == ROMDB_PENDING)
					rl->job = i + 1;
			if (!rl->job) {
				rl->pending = 0;
				break;
			}
			snprintf(path, sizeof(path), "%s/%s", rl->dir,
				 rl->roms[rl->job - 1].file);
			rl->job_fd = open(path, O_RDONLY | O_CLOEXEC);
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
		if (n == 0)
			romdb_resolve(r->file, &r->id, rl->job_crc);
		else {
			log_msg("romdb: %s: cannot read for its CRC (%s)", r->file,
				strerror(errno));
			r->id.how = ROMDB_UNKNOWN;
		}
		if (rl->job_fd >= 0)
			close(rl->job_fd);
		dt = now_ns() - t0;
		rl->job_ns += dt;
		rl->crc_ns += dt;
		romdb_log(r->file, &r->id);
		if (r->id.how == ROMDB_MODIFIED)
			log_msg("romdb: %s: modified ROM (crc %08x), shown as %s",
				r->file, r->id.crc, r->id.title);
		log_verbose("romdb: %s: CRC of %llu KiB in %.1f ms", r->file,
			    (unsigned long long)rl->job_bytes / 1024,
			    rl->job_ns / 1e6);
		rl->job = 0;
		rl->pending--;
		qsort(rl->roms, rl->count, sizeof(*rl->roms), cmp_roms);
		if (!rl->pending) {
			romdb_cache_save(1);
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
	rl->pending = 0;
	rl->job = 0;
	rl->crc_ns = 0;
}
