/* Portable POC state, bounded draw commands and RGB565 software renderer. */
#ifndef FIGHTER_H
#define FIGHTER_H
#include <stddef.h>
#include <stdint.h>
#define FW 480
#define FH 320
#define SW 128
#define SH 160
#define POSES 8
#define SPRITES (2 * POSES)
#define PIXELS (SPRITES * SW * SH)
#define COMMANDS 512
#define STATE_WORDS 32
struct fighter {
	int32_t x, y, vy, hp, attack, stun, face, hit;
};
struct fight_state {
	uint32_t tick, rng, previous, paused, stress, boxes, automatic, round;
	struct fighter p[2];
	uint32_t tone, phase, ended, hits, guards, reserved[3];
};
struct assets {
	uint16_t color[PIXELS];
	uint8_t alpha[PIXELS];
	uint32_t hash;
};
enum operation { RECT, SPRITE };
enum blend { OPAQUE, ALPHA, ADD };
struct command {
	int op, x, y, w, h, sprite, flip, blend;
	uint16_t color;
	uint8_t alpha;
};
struct scene {
	struct command cmd[COMMANDS];
	unsigned count;
};
void fight_reset(struct fight_state *s, int automatic);
void fight_step(struct fight_state *s, uint32_t input, uint32_t second, int human);
void fight_scene(const struct fight_state *s, struct scene *scene);
void fight_render(const struct assets *a, const struct scene *scene, uint16_t *fb);
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic);
void fight_encode(const struct fight_state *s, uint32_t words[STATE_WORDS]);
int fight_decode(struct fight_state *s, const uint32_t words[STATE_WORDS]);
#endif
