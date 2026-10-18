/*
 * ROM list: *.gba files in <data>/roms, sorted.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fe.h"
#include "romlist.h"

#define MAX_ROMS 4096

static int cmp_names(const void *a, const void *b)
{
	const char *x = *(char *const *)a, *y = *(char *const *)b;
	int r = strcasecmp(x, y);

	return r ? r : strcmp(x, y);
}

static int is_rom(const char *name)
{
	size_t n = strlen(name);

	if (name[0] == '.' || n < 5)
		return 0;
	return !strcasecmp(name + n - 4, ".gba");
}

int romlist_scan(struct romlist *rl, const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int cap = 0;

	romlist_free(rl);
	if (!d)
		return -1;
	while ((e = readdir(d)) && rl->count < MAX_ROMS) {
		if (e->d_type != DT_REG && e->d_type != DT_UNKNOWN)
			continue;
		if (!is_rom(e->d_name))
			continue;
		if (rl->count == cap) {
			char **nn;

			cap = cap ? cap * 2 : 64;
			nn = realloc(rl->names, cap * sizeof(*nn));
			if (!nn)
				break;
			rl->names = nn;
		}
		rl->names[rl->count] = strdup(e->d_name);
		if (rl->names[rl->count])
			rl->count++;
	}
	closedir(d);
	if (rl->count)
		qsort(rl->names, rl->count, sizeof(*rl->names), cmp_names);
	return rl->count;
}

void romlist_free(struct romlist *rl)
{
	for (int i = 0; i < rl->count; i++)
		free(rl->names[i]);
	free(rl->names);
	rl->names = NULL;
	rl->count = 0;
}
