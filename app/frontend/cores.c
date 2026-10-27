/*
 * The core tables. Built once per binary (see mk/frontend.mk) with
 * HAVE_CORE_MGBA / HAVE_CORE_GPSP for the cores linked in, or with
 * neither for the stub build, where every core id runs the stub core.
 */
#include <stdbool.h>
#include <stdint.h>

#include "core_api.h"
#include "fe.h"

#define CORE_PROTOS(p) \
	void p##_retro_set_environment(retro_environment_t); \
	void p##_retro_set_video_refresh(retro_video_refresh_t); \
	void p##_retro_set_audio_sample(retro_audio_sample_t); \
	void p##_retro_set_audio_sample_batch(retro_audio_sample_batch_t); \
	void p##_retro_set_input_poll(retro_input_poll_t); \
	void p##_retro_set_input_state(retro_input_state_t); \
	void p##_retro_init(void); \
	void p##_retro_deinit(void); \
	void p##_retro_get_system_info(struct retro_system_info *); \
	void p##_retro_get_system_av_info(struct retro_system_av_info *); \
	bool p##_retro_load_game(const struct retro_game_info *); \
	void p##_retro_unload_game(void); \
	void p##_retro_reset(void); \
	void p##_retro_run(void); \
	void *p##_retro_get_memory_data(unsigned); \
	size_t p##_retro_get_memory_size(unsigned); \
	size_t p##_retro_serialize_size(void); \
	bool p##_retro_serialize(void *, size_t); \
	bool p##_retro_unserialize(const void *, size_t);

#define CORE_TABLE(p, label, save) { \
	.name = label, \
	.set_environment = p##_retro_set_environment, \
	.set_video_refresh = p##_retro_set_video_refresh, \
	.set_audio_sample = p##_retro_set_audio_sample, \
	.set_audio_sample_batch = p##_retro_set_audio_sample_batch, \
	.set_input_poll = p##_retro_set_input_poll, \
	.set_input_state = p##_retro_set_input_state, \
	.init = p##_retro_init, \
	.deinit = p##_retro_deinit, \
	.get_system_info = p##_retro_get_system_info, \
	.get_system_av_info = p##_retro_get_system_av_info, \
	.load_game = p##_retro_load_game, \
	.unload_game = p##_retro_unload_game, \
	.reset = p##_retro_reset, \
	.run = p##_retro_run, \
	.get_memory_data = p##_retro_get_memory_data, \
	.get_memory_size = p##_retro_get_memory_size, \
	.serialize_size = p##_retro_serialize_size, \
	.serialize = p##_retro_serialize, \
	.unserialize = p##_retro_unserialize, \
	.save_size = save, \
}

#if defined(HAVE_CORE_MGBA) || defined(HAVE_CORE_GPSP)

#ifdef HAVE_CORE_MGBA
CORE_PROTOS(mgba)
static const struct core_api mgba = CORE_TABLE(mgba, "mgba", NULL);
#endif

#ifdef HAVE_CORE_GPSP
CORE_PROTOS(gpsp)

/* gpSP's internals (kept global by mk/gpsp.mk), see gba_memory.h. */
extern uint32_t gpsp_backup_type, gpsp_flash_bank_cnt, gpsp_eeprom_size;

/*
 * gpSP always reports its whole 128KiB backup buffer. The layout in it is
 * the same as mGBA's (SRAM, flash banks in order, EEPROM 8 byte blocks
 * with the first bit in bit 7), so write only the real size like mGBA
 * does; other emulators guess the save type from the file size.
 */
static size_t gpsp_save_size(void)
{
	switch (gpsp_backup_type) {
	case 0:  /* BACKUP_SRAM */
		return 32 << 10;
	case 1:  /* BACKUP_FLASH: FLASH_SIZE_128KB is 2 */
		return gpsp_flash_bank_cnt == 2 ? 128 << 10 : 64 << 10;
	case 2:  /* BACKUP_EEPROM: EEPROM_8_KBYTE is 16 */
		return gpsp_eeprom_size == 16 ? 8 << 10 : 512;
	default:  /* not accessed yet */
		return 128 << 10;
	}
}

static const struct core_api gpsp = CORE_TABLE(gpsp, "gpsp", gpsp_save_size);
#endif

const struct core_api *core_api_get(int id)
{
	switch (id) {
#ifdef HAVE_CORE_MGBA
	case CORE_MGBA:
		return &mgba;
#endif
#ifdef HAVE_CORE_GPSP
	case CORE_GPSP:
		return &gpsp;
#endif
	default:
		return NULL;
	}
}

#else

CORE_PROTOS(stub)
static const struct core_api stub = CORE_TABLE(stub, "stub", NULL);

const struct core_api *core_api_get(int id)
{
	return id >= 0 && id < CORE_COUNT ? &stub : NULL;
}

#endif
