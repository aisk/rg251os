/*
 * Persistent settings on the GBA partition, as key=value text that can be
 * edited on a PC:
 *
 *   config/settings.txt          global: volume, brightness, LCD grid
 *   config/roms/<rom name>.txt   per game: color correction
 *
 * <rom name> is the ROM file name without ".gba". Unknown keys, bad values,
 * comments (#), blank lines and CRLF line ends are ignored, so a missing or
 * damaged file just leaves the defaults. Files are replaced atomically.
 * Before config/ existed the global settings lived in <data>/settings.txt;
 * that file is still read when config/settings.txt is missing.
 *
 * A per ROM file is only written once something was changed for that
 * game, so a future default source (rom_settings_defaults(), e.g. a ROM
 * database turning color correction on for known titles) still applies to
 * every game the user has not touched.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fe.h"

const char *const color_mode_names[COLOR_COUNT] = {
	"off", "gba", "gba-light", "sp101",
};
const char *const color_mode_labels[COLOR_COUNT] = {
	"Off", "GBA", "GBA light", "SP101",
};

const struct settings settings_defaults = {
	.volume = 255,
	.brightness = 50,
	.grid = 0,
	.grid_dim = 50,
	.grid_boost = 0,
};
struct settings settings = settings_defaults;
struct rom_settings rom_settings;

static char rom_base[128];  /* current game, "" when none */

static const char *const on_off[] = { "off", "on" };

struct field {
	const char *key;
	size_t off;
	int min, max;
	const char *const *names;  /* value i is written as names[i] */
};

#define F(type, member, lo, hi, n) \
	{ #member, offsetof(struct type, member), lo, hi, n }

static const struct field global_fields[] = {
	F(settings, volume, 0, 255, NULL),
	F(settings, brightness, 5, 100, NULL),
	F(settings, grid, 0, 1, on_off),
	F(settings, grid_dim, 0, 90, NULL),
	F(settings, grid_boost, 0, 100, NULL),
};

static const struct field rom_fields[] = {
	F(rom_settings, color, 0, COLOR_COUNT - 1, color_mode_names),
};

static int parse_value(const struct field *f, const char *v, int *out)
{
	char *end;
	long n;

	if (f->names)
		for (int i = 0; i <= f->max; i++)
			if (!strcasecmp(v, f->names[i])) {
				*out = i;
				return 0;
			}
	if (f->names == on_off) {
		if (!strcasecmp(v, "yes") || !strcasecmp(v, "true")) {
			*out = 1;
			return 0;
		}
		if (!strcasecmp(v, "no") || !strcasecmp(v, "false")) {
			*out = 0;
			return 0;
		}
	}
	if (!*v)
		return -1;
	n = strtol(v, &end, 10);
	if (*end || n < f->min || n > f->max)
		return -1;
	*out = (int)n;
	return 0;
}

/* Returns the number of fields set. */
static int parse_fields(void *obj, const struct field *fields, size_t nf,
			const char *text)
{
	int count = 0;

	while (text && *text) {
		const char *eol = strchr(text, '\n');
		size_t len = eol ? (size_t)(eol - text) : strlen(text);
		char line[128], *key, *val, *eq, *p;

		if (len < sizeof(line)) {
			memcpy(line, text, len);
			line[len] = 0;
			key = line;
			while (isspace((unsigned char)*key))
				key++;
			eq = strchr(key, '=');
			if (*key && *key != '#' && eq) {
				*eq = 0;
				val = eq + 1;
				for (p = eq; p > key && isspace((unsigned char)p[-1]); )
					*--p = 0;
				while (isspace((unsigned char)*val))
					val++;
				for (p = val + strlen(val);
				     p > val && isspace((unsigned char)p[-1]); )
					*--p = 0;
				for (size_t i = 0; i < nf; i++) {
					int v;

					if (strcmp(key, fields[i].key) ||
					    parse_value(&fields[i], val, &v))
						continue;
					*(int *)((char *)obj + fields[i].off) = v;
					count++;
				}
			}
		}
		text = eol ? eol + 1 : NULL;
	}
	return count;
}

static int format_fields(const void *obj, const struct field *fields,
			 size_t nf, char *buf, size_t size)
{
	size_t n = 0;

	buf[0] = 0;
	for (size_t i = 0; i < nf && n < size; i++) {
		int v = *(const int *)((const char *)obj + fields[i].off);

		if (fields[i].names && v >= 0 && v <= fields[i].max)
			n += snprintf(buf + n, size - n, "%s=%s\n", fields[i].key,
				      fields[i].names[v]);
		else
			n += snprintf(buf + n, size - n, "%s=%d\n", fields[i].key, v);
	}
	return n < size ? (int)n : -1;
}

static int load_file(const char *path, void *obj, const struct field *fields,
		     size_t nf)
{
	char buf[2048];
	int n = read_file(path, buf, sizeof(buf) - 1);

	if (n < 0)
		return -1;
	buf[n] = 0;
	return parse_fields(obj, fields, nf, buf);
}

/* ---------------------------------------------------------------- global */

int settings_parse(struct settings *s, const char *text)
{
	return parse_fields(s, global_fields, ARRAY_SIZE(global_fields), text);
}

int settings_format(const struct settings *s, char *buf, size_t size)
{
	return format_fields(s, global_fields, ARRAY_SIZE(global_fields), buf,
			     size);
}

void settings_load(void)
{
	char path[300];
	const char *from = "config/settings.txt";
	int n;

	settings = settings_defaults;
	snprintf(path, sizeof(path), "%s/config/settings.txt", fe_data_dir);
	n = load_file(path, &settings, global_fields, ARRAY_SIZE(global_fields));
	if (n < 0) {
		from = "settings.txt (old location)";
		snprintf(path, sizeof(path), "%s/settings.txt", fe_data_dir);
		n = load_file(path, &settings, global_fields,
			      ARRAY_SIZE(global_fields));
	}
	log_msg("settings: %s, %d values: volume %d, brightness %d%%, grid %s "
		"dim %d%% boost %d%%", n < 0 ? "defaults (no file)" : from,
		n < 0 ? 0 : n, settings.volume, settings.brightness,
		settings.grid ? "on" : "off", settings.grid_dim,
		settings.grid_boost);
}

void settings_save(void)
{
	char path[300], buf[512];
	int n = settings_format(&settings, buf, sizeof(buf));

	snprintf(path, sizeof(path), "%s/config/settings.txt", fe_data_dir);
	if (n > 0 && write_file_atomic(path, buf, n) == 0)
		log_msg("settings: saved to %s", path);
}

/* --------------------------------------------------------------- per ROM */

void rom_settings_defaults(const char *rom_name, struct rom_settings *r)
{
	/* Hook for per game defaults (e.g. color correction for titles
	 * known to be authored for the unlit screen); same for all today. */
	(void)rom_name;
	r->color = COLOR_OFF;
}

int rom_settings_parse(struct rom_settings *r, const char *text)
{
	return parse_fields(r, rom_fields, ARRAY_SIZE(rom_fields), text);
}

int rom_settings_format(const struct rom_settings *r, char *buf, size_t size)
{
	return format_fields(r, rom_fields, ARRAY_SIZE(rom_fields), buf, size);
}

static void rom_path(char *buf, size_t size)
{
	snprintf(buf, size, "%s/config/roms/%s.txt", fe_data_dir, rom_base);
}

void rom_settings_load(const char *rom_name)
{
	char path[400];
	size_t len;
	int n;

	rom_base[0] = 0;
	rom_settings_defaults(rom_name, &rom_settings);
	if (!rom_name)
		return;
	len = strlen(rom_name);
	if (len > 4 && !strcasecmp(rom_name + len - 4, ".gba"))
		len -= 4;
	snprintf(rom_base, sizeof(rom_base), "%.*s", (int)len, rom_name);
	rom_path(path, sizeof(path));
	n = load_file(path, &rom_settings, rom_fields, ARRAY_SIZE(rom_fields));
	log_msg("settings: %s: %s, color %s", rom_base,
		n < 0 ? "defaults (no file)" : "loaded",
		color_mode_names[rom_settings.color]);
}

void rom_settings_save(void)
{
	char path[400], buf[256];
	int n;

	if (!rom_base[0])
		return;
	n = rom_settings_format(&rom_settings, buf, sizeof(buf));
	rom_path(path, sizeof(path));
	if (n > 0 && write_file_atomic(path, buf, n) == 0)
		log_msg("settings: saved %s", path);
}
