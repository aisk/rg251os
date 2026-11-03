/* Portable fighter state, FIGHTP02 content, bounded draw commands and the
 * RGB565 software renderer interface. Rules (fighter.c) only emit commands;
 * render.c owns the pack decoder and the pixel loops. */
#ifndef FIGHTER_H
#define FIGHTER_H
#include <stddef.h>
#include <stdint.h>
#define FW 480
#define FH 320
#define COMMANDS 512
#define STATE_WORDS 40
/* Ground line in screen rows; fighters' foot anchors stand on it. */
#define GROUND 260
/* Resident content limits; packs that need more are rejected. */
#define MAX_PALETTES 8
#define MAX_FRAMES 256
#define MAX_IMAGES 256
#define MAX_ROWS 32768
#define MAX_SPANS 65536
#define MAX_PIXELS (1u << 20)
#define MAX_STEPS 512
#define MAX_EFFECTS 64
#define MAX_SIDE 1024
enum action { IDLE, WALK_FORWARD, WALK_BACK, JUMP, ATTACK, GUARD, HURT, KO, ACTIONS };
struct fighter {
	int32_t x, y, vy, hp, attack, stun, face, hit, walk, block;
};
struct fight_state {
	uint32_t tick, rng, previous, paused, stress, boxes, automatic, round;
	struct fighter p[2];
	uint32_t tone, phase, ended, hits, guards;
};
/* One opaque run in an image row; pixels index the image's RGB565 data. */
struct span {
	uint16_t x, len;
	uint32_t pixel;
};
/* A frame expanded with one palette. Row r's spans are
 * span[row[first_row + r] .. row[first_row + r + 1]). */
struct image {
	int w, h, ax, ay;
	uint32_t first_row, pixels, count;
	uint16_t frame, palette;
};
struct step {
	uint16_t image, ticks;
};
struct anim {
	uint16_t first, count, total;
};
struct character {
	struct anim action[ACTIONS];
	uint32_t pixels; /* resident RGB565 pixels owned by this character */
};
struct assets {
	uint32_t hash;
	unsigned images, rows, spans, pixels, steps, effects;
	struct character chr[2];
	struct image image[MAX_IMAGES];
	struct step step[MAX_STEPS];
	uint16_t effect[MAX_EFFECTS];
	uint32_t row[MAX_ROWS + 1];
	struct span span[MAX_SPANS];
	uint16_t pixel[MAX_PIXELS];
};
enum operation { RECT, SPRITE };
enum blend { OPAQUE, ALPHA, ADD };
/* RECT fills x,y,w,h with color. SPRITE draws image with its top-left at x,y
 * (w,h are the image size), mirrored horizontally when flip is set. */
struct command {
	int op, x, y, w, h, image, flip, blend;
	uint16_t color;
	uint8_t alpha;
};
struct scene {
	struct command cmd[COMMANDS];
	unsigned count;
};
void fight_reset(struct fight_state *s, int automatic);
void fight_step(struct fight_state *s, uint32_t input, uint32_t second, int human);
void fight_scene(const struct fight_state *s, const struct assets *a, struct scene *scene);
void fight_render(const struct assets *a, const struct scene *scene, uint16_t *fb);
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic);
void fight_encode(const struct fight_state *s, uint32_t words[STATE_WORDS]);
int fight_decode(struct fight_state *s, const uint32_t words[STATE_WORDS]);
#endif
