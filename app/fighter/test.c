/* Native libretro callback harness: malformed content, deterministic state,
 * combat, clipping, output formats, audio and a complete automated scene cycle.
 * ASan/UBSan run this without a window or device; no host speed claims. */
#include "fighter.h"
#include "libretro.h"
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t keys[2], image_hash, sound_hash;
static unsigned video_count, audio_count, formats;
static bool use32, reject_formats;
static unsigned benchmark_complete;
static void log_line(enum retro_log_level level, const char *fmt, ...)
{
	(void)level;
	if (strstr(fmt, "benchmark complete:"))
		benchmark_complete++;
}
static uint16_t last_frame[FW * FH];
static const char *players = "cpu";
static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_line;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		((struct retro_variable *)data)->value = players;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		formats++;
		return !reject_formats && *(enum retro_pixel_format *)data ==
					      (use32 ? RETRO_PIXEL_FORMAT_XRGB8888
						     : RETRO_PIXEL_FORMAT_RGB565);
	default:
		return false;
	}
}
static uint32_t hash(const void *data, size_t n)
{
	const uint8_t *p = data;
	uint32_t h = 2166136261;
	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}
static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	assert(data && w == FW && h == FH && pitch == FW * (use32 ? 4u : 2u));
	image_hash = hash(data, pitch * h);
	video_count++;
	if (!use32)
		memcpy(last_frame, data, sizeof(last_frame));
}
static size_t audio(const int16_t *data, size_t n)
{
	assert(n == 800);
	sound_hash = hash(data, n * 4);
	audio_count++;
	return n;
}
static void poll_input(void) {}
static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id)
{
	assert(port < 2 && device == RETRO_DEVICE_JOYPAD && !index && id < 16);
	return (keys[port] >> id) & 1;
}
static uint8_t *read_all(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	assert(f);
	assert(!fseek(f, 0, SEEK_END));
	long len = ftell(f);
	assert(len > 0);
	rewind(f);
	uint8_t *p = malloc((size_t)len);
	assert(p);
	assert(fread(p, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	*n = (size_t)len;
	return p;
}
static void save(void *buf, size_t n) { assert(retro_serialize(buf, n)); }
int main(int argc, char **argv)
{
	assert(argc == 4);
	retro_set_environment(environment);
	retro_set_video_refresh(video);
	retro_set_audio_sample_batch(audio);
	retro_set_input_poll(poll_input);
	retro_set_input_state(input);
	retro_init();
	assert(retro_api_version() == RETRO_API_VERSION);
	struct retro_system_info si;
	struct retro_system_av_info av;
	retro_get_system_info(&si);
	retro_get_system_av_info(&av);
	assert(!strcmp(si.valid_extensions, "fight") && !si.need_fullpath &&
	       av.timing.fps == 60 && av.timing.sample_rate == 48000);
	size_t len;
	uint8_t *data = read_all(argv[1], &len);
	struct retro_game_info g = {argv[1], data, len, NULL};
	assert(!retro_load_game(NULL));
	for (size_t i = 0; i < 32; i++) {
		g.size = i;
		assert(!retro_load_game(&g));
	}
	g.size = len - 1;
	assert(!retro_load_game(&g));
	g.size = len;
	data[100] ^= 1;
	assert(!retro_load_game(&g));
	data[100] ^= 1;
	data[8] ^= 1;
	assert(!retro_load_game(&g));
	data[8] ^= 1;
	reject_formats = true;
	assert(!retro_load_game(&g));
	reject_formats = false;
	assert(retro_load_game(&g));
	assert(formats >= 3);
	size_t n = retro_serialize_size();
	uint8_t *start = malloc(n), *end = malloc(n), *copy = malloc(n);
	assert(start && end && copy);
	assert(!retro_serialize(start, n - 1));
	save(start, n);
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	for (int i = 0; i < 60; i++)
		retro_run();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_A;
	for (int i = 0; i < 120; i++)
		retro_run();
	save(end, n);
	assert(memcmp(start, end, n));
	uint32_t picture = image_hash, sound = sound_hash;
	assert(retro_unserialize(start, n));
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	for (int i = 0; i < 60; i++)
		retro_run();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_A;
	for (int i = 0; i < 120; i++)
		retro_run();
	save(copy, n);
	assert(!memcmp(end, copy, n) && picture == image_hash && sound == sound_hash);
	memcpy(copy, end, n);
	copy[8] ^= 1;
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memset(copy + 16 + 8 * 4, 0xff, 4);
	assert(!retro_unserialize(copy, n));
	assert(!retro_unserialize(end, n - 1));
	save(copy, n);
	assert(!memcmp(end, copy, n));
	/* Pause freezes simulation and audio state, while rendering continues. */
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_START;
	retro_run();
	keys[0] = 0;
	retro_run();
	save(start, n);
	for (int i = 0; i < 10; i++)
		retro_run();
	save(copy, n);
	assert(!memcmp(start, copy, n));
	retro_reset();
	keys[0] = 0;
	retro_run();
	/* Snapshot a frame for human review without running the handheld. */
	FILE *f = fopen(argv[3], "wb");
	assert(f);
	fprintf(f, "P6\n480 320\n255\n");
	for (int i = 0; i < FW * FH; i++) {
		unsigned c = last_frame[i];
		uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31),
				  (uint8_t)(((c >> 5) & 63) * 255 / 63),
				  (uint8_t)((c & 31) * 255 / 31)};
		assert(fwrite(rgb, 1, 3, f) == 3);
	}
	fclose(f);
	retro_unload_game();
	assert(!retro_serialize(start, n));
	use32 = true;
	assert(retro_load_game(&g));
	retro_run();
	retro_unload_game();
	use32 = false;
	players = "two";
	assert(retro_load_game(&g));
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_run();
	save(start, n);
	keys[1] = 0;
	retro_reset();
	retro_run();
	save(end, n);
	assert(memcmp(start, end, n));
	/* A disconnected port must neither move nor attack. */
	retro_set_controller_port_device(1, RETRO_DEVICE_NONE);
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_reset();
	retro_run();
	save(copy, n);
	assert(!memcmp(end, copy, n));
	retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	keys[1] = 0;
	retro_unload_game();
	players = "cpu";
	free(data);
	data = read_all(argv[2], &len);
	g.data = data;
	g.size = len;
	assert(retro_load_game(&g));
	unsigned runs = getenv("FIGHTER_LONG_TEST") ? 36010 : 2500;
	for (unsigned i = 0; i < runs; i++)
		retro_run();
	assert(benchmark_complete == (runs >= 36000 ? 1u : 0u));
	save(start, n);
	assert(video_count == audio_count);
	assert(video_count > 2800);
	/* Direct logic edge cases and every stress mode's clipped rendering. */
	struct fight_state s;
	fight_reset(&s, 0);
	s.p[0].x = 200;
	s.p[1].x = 280;
	s.p[0].attack = s.p[1].attack = 18;
	fight_step(&s, 0, 0, 1);
	assert(s.p[0].hp == 935 && s.p[1].hp == 935);
	fight_reset(&s, 0);
	s.p[0].x = 200;
	s.p[1].x = 280;
	s.p[0].attack = 18;
	fight_step(&s, 0, 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT, 1);
	assert(s.p[1].hp == 992 && s.guards == 1);
	fight_reset(&s, 0);
	for (int i = 0; i < 500; i++)
		fight_step(&s, 1u << RETRO_DEVICE_ID_JOYPAD_UP, 0, 1);
	uint32_t words[STATE_WORDS];
	fight_encode(&s, words);
	assert(fight_decode(&s, words));
	retro_unload_game();
	retro_deinit();
	free(start);
	free(end);
	free(copy);
	free(data);
	puts("fighter-test: content, RGB565/XRGB8888, audio, ports, replay, pause, "
	     "state validation, simultaneous hits, guard, jumps and stress "
	     "cycle passed");
	return 0;
}
