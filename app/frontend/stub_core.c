/*
 * Stand-in libretro core for building and testing without the real cores: a moving
 * test pattern at 240x160 RGB565, a 440 Hz tone at 32768 Hz in batches
 * like mGBA's, and a 32KiB "SRAM" that counts A presses so the save path
 * can be exercised. Buttons are drawn as boxes at the bottom. Its save
 * state is the frame counter and the tone's phase (not the SRAM, like
 * the real cores' loads). Frame skipping as gpSP does it, from gpSP's
 * keys (gpsp_frameskip, _threshold, _interval, read at load and after a
 * GET_VARIABLE_UPDATE): auto_threshold registers the audio buffer status
 * callback and skips while the occupancy is under the threshold (at most
 * 30 in a row), fixed_interval skips that many frames after each drawn
 * one; a skipped frame is not drawn and goes to video_refresh as NULL.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#define W 240
#define H 160
#define RATE 32768
#define FPS (16777216.0 / 280896.0)

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t batch_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t state_cb;

static uint16_t fb[W * H];
static uint8_t sram[32768];
static unsigned frame;
static double audio_acc, phase;
static int prev_a;

/* Frame skipping, as in gpSP. */
enum { SKIP_OFF, SKIP_AUTO, SKIP_FIXED };
static int skip_mode, skip_threshold, skip_interval, skip_count;
static bool buf_active;
static unsigned buf_occupancy;

static void buffer_status(bool active, unsigned occupancy, bool underrun)
{
	(void)underrun;
	buf_active = active;
	buf_occupancy = occupancy;
}

static const char *variable(const char *key)
{
	struct retro_variable v = { key, NULL };

	return env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) ? v.value : NULL;
}

static void read_options(void)
{
	const char *m = variable("gpsp_frameskip");
	const char *t = variable("gpsp_frameskip_threshold");
	const char *i = variable("gpsp_frameskip_interval");
	int prev = skip_mode;

	skip_mode = !m ? SKIP_OFF : !strcmp(m, "auto_threshold") ? SKIP_AUTO :
		    !strcmp(m, "fixed_interval") ? SKIP_FIXED : SKIP_OFF;
	skip_threshold = t ? atoi(t) : 33;
	skip_interval = i ? atoi(i) : 1;
	if (skip_mode != prev) {
		struct retro_audio_buffer_status_callback cb = { buffer_status };

		env_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,
		       skip_mode == SKIP_AUTO ? &cb : NULL);
		buf_active = false;
		skip_count = 0;
	}
}

static int skip_frame(void)
{
	int skip = 0;

	if (skip_mode == SKIP_AUTO)
		skip = buf_active && buf_occupancy < (unsigned)skip_threshold &&
		       skip_count < 30;
	else if (skip_mode == SKIP_FIXED)
		skip = skip_count < skip_interval;
	skip_count = skip ? skip_count + 1 : 0;
	return skip;
}

void retro_set_environment(retro_environment_t cb) { env_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { state_cb = cb; }

void retro_init(void)
{
	enum retro_pixel_format f = RETRO_PIXEL_FORMAT_RGB565;

	env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
}

void retro_deinit(void)
{
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "stub";
	info->library_version = "1";
	info->valid_extensions = "gba";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = info->geometry.max_width = W;
	info->geometry.base_height = info->geometry.max_height = H;
	info->geometry.aspect_ratio = 1.5f;
	info->timing.fps = FPS;
	info->timing.sample_rate = RATE;
}

bool retro_load_game(const struct retro_game_info *game)
{
	frame = 0;
	audio_acc = phase = 0;
	memset(sram, 0xff, sizeof(sram));
	skip_mode = SKIP_OFF;
	read_options();
	return game && game->size > 0;
}

void retro_unload_game(void)
{
}

/* Like a console reset: the pattern and the tone start over, the SRAM
 * stays. */
void retro_reset(void)
{
	frame = 0;
	audio_acc = phase = 0;
	prev_a = 0;
}

void retro_run(void)
{
	static int16_t audio[2048];
	int16_t pad;
	unsigned n;
	bool updated = false;
	int skip;

	poll_cb();
	pad = state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
	skip = skip_frame();

	for (int y = 0; y < H && !skip; y++)
		for (int x = 0; x < W; x++) {
			unsigned v = (x + frame) & 0xff;

			fb[y * W + x] = (uint16_t)((v >> 3) << 11 | (y >> 1) << 5 |
						   ((x ^ y) & 0x1f));
		}
	/* Orientation: white top-left, red bottom-right. */
	for (int i = 0; i < 8 && !skip; i++) {
		fb[i] = fb[i * W] = 0xffff;
		fb[(H - 1) * W + W - 1 - i] = fb[(H - 1 - i) * W + W - 1] = 0xf800;
	}
	for (int b = 0; b < 12 && !skip; b++)
		for (int y = H - 12; y < H - 4; y++)
			for (int x = 8 + b * 12; x < 16 + b * 12; x++)
				fb[y * W + x] = (pad >> b) & 1 ? 0xffff : 0x0000;

	if ((pad & (1 << RETRO_DEVICE_ID_JOYPAD_A)) && !prev_a) {
		sram[0] = sram[0] == 0xff ? 1 : sram[0] + 1;
		sram[1] = 0x5a;
	}
	prev_a = pad & (1 << RETRO_DEVICE_ID_JOYPAD_A);

	video_cb(skip ? NULL : fb, W, H, W * 2);

	audio_acc += RATE / FPS;
	n = (unsigned)audio_acc;
	audio_acc -= n;
	for (unsigned i = 0; i < n && i < 1024; i++) {
		int16_t s = (int16_t)(3000 * sin(phase));

		phase += 2 * M_PI * 440 / RATE;
		if (phase > 2 * M_PI)
			phase -= 2 * M_PI;
		audio[2 * i] = audio[2 * i + 1] = s;
	}
	batch_cb(audio, n);
	frame++;
	if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		read_options();
}

void *retro_get_memory_data(unsigned id)
{
	return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	return id == RETRO_MEMORY_SAVE_RAM ? sizeof(sram) : 0;
}

struct stub_state {
	char magic[8];
	unsigned frame;
	int prev_a;
	double audio_acc, phase;
};

size_t retro_serialize_size(void)
{
	return sizeof(struct stub_state);
}

bool retro_serialize(void *data, size_t size)
{
	struct stub_state st = { "stubst1", frame, prev_a, audio_acc, phase };

	if (size != sizeof(st))
		return false;
	memcpy(data, &st, sizeof(st));
	return true;
}

bool retro_unserialize(const void *data, size_t size)
{
	struct stub_state st;

	if (size != sizeof(st))
		return false;
	memcpy(&st, data, sizeof(st));
	if (memcmp(st.magic, "stubst1", 8))
		return false;
	frame = st.frame;
	prev_a = st.prev_a;
	audio_acc = st.audio_acc;
	phase = st.phase;
	return true;
}
