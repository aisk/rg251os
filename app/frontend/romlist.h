#ifndef ROMLIST_H
#define ROMLIST_H

struct romlist {
	char **names;  /* file names, sorted case-insensitively */
	int count;
};

/* List *.gba files in dir (no hidden or AppleDouble "._" files). */
int romlist_scan(struct romlist *rl, const char *dir);
void romlist_free(struct romlist *rl);

#endif
