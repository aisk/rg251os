/*
 * Persistent settings on the GBA partition, as key=value text that can be
 * edited on a PC:
 *
 *   config/settings.txt          global: volume, brightness, LCD grid,
 *                                CPU mode (cpu=adaptive|max|<MHz>),
 *                                audio path (audio=simple|adaptive),
 *                                debug logging (debug=on|off),
 *                                resume on boot (resume=on|off, see
 *                                resume.c), how videos fill the screen
 *                                (video_scale=fit|crop, video_core.c)
 *   config/roms/<rom name>.txt   per game: color correction
 *                                (color=auto|off|gba|gba-light|sp101,
 *                                auto by default, see romdb.c), the
 *                                learned start OPP (cpu_start=<MHz>),
 *                                the core (core=mgba|gpsp), gpSP's
 *                                frame blending (blend=off|on) and
 *                                frame skipping
 *                                (frameskip=off|auto|1|2|3)
 *
 * <rom name> is the ROM file name without ".gba", whichever folder of
 * roms/ it is in: moving a ROM to another folder keeps its settings (and
 * its save, core.c), and two ROMs with the same file name in different
 * folders share them. A video keeps its ".mpg" (x.mpg.txt), so it never
 * shares a file with x.gba; of its settings only cpu_start is used. Unknown keys, bad values, comments (#), blank lines
 * and CRLF line ends are ignored, so a missing or damaged file just leaves
 * the defaults. Files are replaced atomically.
 *
 * A per ROM file is only written once something was changed for that
 * game, and only with the fields that were in the file or differ from the
 * defaults (rom_settings_defaults()), so a changed default still reaches
 * every game the user has not touched: core= appears only once a core
 * other than the default was picked for that game, color= only once a
 * mode other than auto was.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fe.h"
#include "romdb.h"
#include "settings.h"

const char *const color_mode_names[COLOR_COUNT] = {
	"off", "gba", "gba-light", "sp101", "auto",
};
const char *const color_mode_labels[COLOR_COUNT] = {
	"Off", "GBA", "GBA light", "SP101", "Auto",
};
const char *const core_names[CORE_COUNT] = { "mgba", "gpsp", "video" };
const char *const frameskip_names[FRAMESKIP_COUNT] = {
	"off", "auto", "1", "2", "3",
};
const char *const frameskip_labels[FRAMESKIP_COUNT] = {
	"Off", "Auto", "1 (30fps)", "2 (20fps)", "3 (15fps)",
};
const char *const video_scale_names[VIDEO_SCALE_COUNT] = { "fit", "crop" };
const char *const video_scale_labels[VIDEO_SCALE_COUNT] = { "Fit", "Crop" };
const char *const core_labels[CORE_COUNT] = { "mGBA", "gpSP", "Video" };

const struct settings settings_defaults = {
	.volume = 255,
	.brightness = 50,
	.grid = 1,
	.grid_dim = 25,
	.grid_boost = 15,
	.cpu = CPU_MODE_ADAPTIVE,
	.audio = AUDIO_ADAPTIVE,
	.debug = 0,
	.resume = 1,
};
struct settings settings = settings_defaults;
struct rom_settings rom_settings;

static char rom_base[128];  /* current game, "" when none */
static unsigned rom_in_file;  /* fields present in the ROM's file */
static int rom_auto = COLOR_OFF;  /* what auto means for this game */

static const char *const on_off[] = { "off", "on" };
static const char *const cpu_modes[] = { "adaptive", "max" };
static const char *const audio_modes[] = { "simple", "adaptive" };

/*
 * Value i < nnames is written as names[i]; nnames 0 means names cover
 * min..max. Other values are numbers in min..max.
 */
struct field {
	const char *key;
	size_t off;
	int min, max;
	const char *const *names;
	int nnames;
};

#define F(type, member, lo, hi, n) \
	{ #member, offsetof(struct type, member), lo, hi, n, 0 }
#define FN(type, member, lo, hi, n) \
	{ #member, offsetof(struct type, member), lo, hi, n, ARRAY_SIZE(n) }

static const struct field global_fields[] = {
	F(settings, volume, 0, 255, NULL),
	F(settings, brightness, 5, 100, NULL),
	F(settings, grid, 0, 1, on_off),
	F(settings, grid_dim, 0, 90, NULL),
	F(settings, grid_boost, 0, 100, NULL),
	FN(settings, cpu, 300, 5000, cpu_modes),
	FN(settings, audio, 0, 1, audio_modes),
	F(settings, debug, 0, 1, on_off),
	F(settings, resume, 0, 1, on_off),
	FN(settings, video_scale, 1, 0, video_scale_names),
};

static const struct field rom_fields[] = {
	F(rom_settings, color, 0, COLOR_COUNT - 1, color_mode_names),
	F(rom_settings, cpu_start, 0, 5000, NULL),
	F(rom_settings, core, 0, CORE_GBA_COUNT - 1, core_names),
	F(rom_settings, blend, 0, 1, on_off),
	/* Only the names: no number range besides them (min > max). */
	FN(rom_settings, frameskip, 1, 0, frameskip_names),
};

static int field_nnames(const struct field *f)
{
	return f->nnames ? f->nnames : f->max + 1;
}

static int parse_value(const struct field *f, const char *v, int *out)
{
	char *end;
	long n;

	if (f->names)
		for (int i = 0; i < field_nnames(f); i++)
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

/* Returns the number of fields set; marks them in *mask if not NULL. */
static int parse_fields(void *obj, const struct field *fields, size_t nf,
			const char *text, unsigned *mask)
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
					if (mask)
						*mask |= 1u << i;
					count++;
				}
			}
		}
		text = eol ? eol + 1 : NULL;
	}
	return count;
}

/* Writes the fields in mask. */
static int format_fields(const void *obj, const struct field *fields,
			 size_t nf, unsigned mask, char *buf, size_t size)
{
	size_t n = 0;

	buf[0] = 0;
	for (size_t i = 0; i < nf && n < size; i++) {
		int v = *(const int *)((const char *)obj + fields[i].off);

		if (!(mask & 1u << i))
			continue;
		if (fields[i].names && v >= 0 && v < field_nnames(&fields[i]))
			n += snprintf(buf + n, size - n, "%s=%s\n", fields[i].key,
				      fields[i].names[v]);
		else
			n += snprintf(buf + n, size - n, "%s=%d\n", fields[i].key, v);
	}
	return n < size ? (int)n : -1;
}

/* All the fields on one line for the log: "key=value key=value". */
static void fields_line(const void *obj, const struct field *fields,
			size_t nf, char *buf, size_t size)
{
	size_t n;

	format_fields(obj, fields, nf, ~0u, buf, size);
	for (char *p = buf; *p; p++)
		if (*p == '\n')
			*p = ' ';
	n = strlen(buf);
	if (n && buf[n - 1] == ' ')
		buf[n - 1] = 0;
}

static int load_file(const char *path, void *obj, const struct field *fields,
		     size_t nf, unsigned *mask)
{
	char buf[2048];
	int n = read_file(path, buf, sizeof(buf) - 1);

	if (n < 0)
		return -1;
	buf[n] = 0;
	return parse_fields(obj, fields, nf, buf, mask);
}

/* ---------------------------------------------------------------- global */

int settings_parse(struct settings *s, const char *text)
{
	return parse_fields(s, global_fields, ARRAY_SIZE(global_fields), text,
			    NULL);
}

int settings_format(const struct settings *s, char *buf, size_t size)
{
	return format_fields(s, global_fields, ARRAY_SIZE(global_fields), ~0u,
			     buf, size);
}

void settings_load(void)
{
	char path[300], line[256];
	int n;

	settings = settings_defaults;
	snprintf(path, sizeof(path), "%s/config/settings.txt", fe_data_dir);
	n = load_file(path, &settings, global_fields, ARRAY_SIZE(global_fields),
		      NULL);
	if (settings.brightness < BRIGHTNESS_MIN) {
		log_msg("settings: brightness %d%% raised to the minimum %d%%",
			settings.brightness, BRIGHTNESS_MIN);
		settings.brightness = BRIGHTNESS_MIN;
	}
	fields_line(&settings, global_fields, ARRAY_SIZE(global_fields), line,
		    sizeof(line));
	log_msg("settings: %s, %d values: %s",
		n < 0 ? "defaults (no file)" : "config/settings.txt",
		n < 0 ? 0 : n, line);
	log_set_debug(settings.debug);
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

/* The same for every game today, 0 unless listed. gpSP is the default
 * core: mGBA barely keeps up with demanding games on the RK3326. */
static const struct rom_settings rom_defaults = {
	.color = COLOR_AUTO,
	.core = CORE_GPSP,
};

void rom_settings_defaults(const char *rom_name, struct rom_settings *r)
{
	(void)rom_name;
	*r = rom_defaults;
}

int rom_settings_parse(struct rom_settings *r, const char *text)
{
	return parse_fields(r, rom_fields, ARRAY_SIZE(rom_fields), text, NULL);
}

static void rom_path(char *buf, size_t size)
{
	snprintf(buf, size, "%s/config/roms/%s.txt", fe_data_dir, rom_base);
}

void rom_settings_load(const char *rom_name)
{
	char path[400], line[256];
	int n;

	rom_base[0] = 0;
	rom_in_file = 0;
	rom_auto = COLOR_OFF;
	rom_settings_defaults(rom_name, &rom_settings);
	if (!rom_name)
		return;
	snprintf(rom_base, sizeof(rom_base), "%.*s", romdb_stem_len(rom_name),
		 rom_name);
	rom_path(path, sizeof(path));
	n = load_file(path, &rom_settings, rom_fields, ARRAY_SIZE(rom_fields),
		      &rom_in_file);
	fields_line(&rom_settings, rom_fields, ARRAY_SIZE(rom_fields), line,
		    sizeof(line));
	log_msg("settings: %s: %s, %s", rom_base,
		n < 0 ? "defaults (no file)" : "loaded", line);
}

void rom_settings_set_auto(int color, const char *why)
{
	rom_auto = color >= 0 && color < COLOR_MODES ? color : COLOR_OFF;
	log_msg("color: auto is %s for %s (%s)%s", color_mode_names[rom_auto],
		rom_base[0] ? rom_base : "no game", why,
		rom_settings.color == COLOR_AUTO ? "" : ", not used: the game "
		"has an explicit mode");
}

int rom_settings_color(void)
{
	int c = rom_settings.color;

	return c == COLOR_AUTO ? rom_auto : c >= 0 && c < COLOR_MODES ? c :
	       COLOR_OFF;
}

void rom_color_label(char *buf, size_t size)
{
	if (rom_settings.color == COLOR_AUTO)
		snprintf(buf, size, "Auto (%s)", color_mode_labels[rom_auto]);
	else
		snprintf(buf, size, "%s", color_mode_labels[rom_settings.color]);
}

void rom_settings_save(void)
{
	struct rom_settings def;
	unsigned mask = rom_in_file;
	char path[400], buf[256];
	int n;

	if (!rom_base[0])
		return;
	rom_settings_defaults(rom_base, &def);
	for (size_t i = 0; i < ARRAY_SIZE(rom_fields); i++)
		if (*(int *)((char *)&rom_settings + rom_fields[i].off) !=
		    *(int *)((char *)&def + rom_fields[i].off))
			mask |= 1u << i;
	if (!mask)
		return;
	n = format_fields(&rom_settings, rom_fields, ARRAY_SIZE(rom_fields),
			  mask, buf, sizeof(buf));
	rom_path(path, sizeof(path));
	if (n > 0 && write_file_atomic(path, buf, n) == 0) {
		rom_in_file = mask;
		log_msg("settings: saved %s", path);
	}
}
