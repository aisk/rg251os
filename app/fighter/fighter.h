/* Portable fighter state, FIGHTP03 content (frames, per-step boxes, move
 * data), bounded draw commands and the RGB565 software renderer interface.
 * Rules (fighter.c) read the pack's steps, boxes and moves and only emit
 * draw commands; render.c owns the pack decoder and the pixel loops. */
#ifndef FIGHTER_H
#define FIGHTER_H
#include <stddef.h>
#include <stdint.h>
#define FW 480
#define FH 320
#define COMMANDS 512
#define STATE_WORDS 48
/* Ground line in screen rows; fighters' foot anchors stand on it. */
#define GROUND 260
/* Stage walls for fighter anchors. */
#define WALL_LEFT 50
#define WALL_RIGHT 430
/* Resident content limits; packs that need more are rejected. */
#define MAX_PALETTES 8
#define MAX_FRAMES 256
#define MAX_IMAGES 256
#define MAX_ROWS 32768
#define MAX_SPANS 65536
#define MAX_PIXELS (1u << 20)
#define MAX_STEPS 512
#define MAX_BOXES 4096
#define MAX_EFFECTS 64
#define MAX_CHARACTERS 8
#define MAX_SIDE 1024
/* Per-step and per-move limits, mirrored in tools/fighter/pack.py. */
#define MAX_STEP_BOXES 15
#define MAX_MOVE_TICKS 255
#define MAX_DAMAGE 1000
#define MAX_STUN 120
#define MAX_HITSTOP 30
#define MAX_PUSH 120
/* Blended effect sprites draw at most this many opaque pixels per frame
 * (about 12 Power Geyser flames); later effects in the frame are dropped. */
#define EFFECT_PIXEL_BUDGET 76800u
enum action {
	IDLE, WALK_FORWARD, WALK_BACK, JUMP, GUARD, HURT, KO, CROUCH, CROUCH_GUARD,
	CROUCH_HURT, ACTIONS
};
/* Move triggers. Normals only so far; specials will add command triggers. */
enum trigger {
	STAND_LP, STAND_HP, STAND_LK, STAND_HK, CROUCH_LP, CROUCH_HP, CROUCH_LK,
	CROUCH_HK, MOVES
};
/* high: stand or crouch block; low: crouch block only; overhead: stand only. */
enum guard_type { GUARD_HIGH, GUARD_LOW, GUARD_OVERHEAD, GUARD_TYPES };
/* Cancel bits are validated and stored; no rule uses them yet. */
enum cancel { CANCEL_CHAIN = 1, CANCEL_SPECIAL = 2, CANCEL_SUPER = 4, CANCEL_MASK = 7 };
/* One fighter. move is trigger + 1 (0 = none); elapsed counts unfrozen ticks
 * in the current move, stun or guard stance; hit is the last hit group of
 * the move that connected; stun counts down hitstun/blockstun and freeze the
 * hitstop; slide is the remaining pushback in pixels, signed by direction.
 * block: 1 in blockstun, 2 in the proximity guard stance. keys holds the
 * previous tick's buttons for press edges. */
struct fighter {
	int32_t x, y, vy, hp, face, walk, block, crouch, move, elapsed, hit, stun,
	    freeze, slide, keys;
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
/* Anchor-relative box of a right-facing frame: x forward, y down (negative
 * above the feet). Facing left mirrors it to x' = -(x + w). */
struct box {
	int16_t x, y, w, h;
};
/* box[box .. box + hurt) are hurtboxes, the next hit boxes are hitboxes.
 * group numbers runs of consecutive hitbox steps in a move (0 = none). */
struct step {
	uint16_t image, ticks, box;
	uint8_t hurt, hit, group;
};
struct anim {
	uint16_t first, count, total;
};
/* Startup, active and recovery are derived from the steps at load time. */
struct move {
	struct anim anim;
	uint16_t damage, chip, hitstun, blockstun, hitstop, pushback, block_pushback;
	uint8_t guard, cancel, groups, startup, active, recovery;
	int16_t reach; /* furthest hitbox edge in front of the anchor */
};
struct character {
	struct anim action[ACTIONS];
	struct move move[MOVES];
	uint32_t pixels; /* resident RGB565 pixels owned by this player */
};
struct assets {
	uint32_t hash;
	unsigned images, rows, spans, pixels, steps, boxes, effects;
	struct character chr[2];
	struct image image[MAX_IMAGES];
	struct step step[MAX_STEPS];
	struct box box[MAX_BOXES];
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
/* effects_requested/effects_drawn count blended effect sprites this frame. */
struct scene {
	struct command cmd[COMMANDS];
	unsigned count, effects_requested, effects_drawn;
};
/* Screen rectangle of a box for a fighter anchored at x, GROUND + y. */
struct area {
	int x, y, w, h;
};
void fight_reset(struct fight_state *s, int automatic);
void fight_step(struct fight_state *s, const struct assets *a, uint32_t input,
		uint32_t second, int human);
/* Index into a->step of what fighter i shows (and collides with) now. */
unsigned fight_pose(const struct fight_state *s, const struct assets *a, int i);
struct area fight_box(const struct box *b, int x, int y, int face);
void fight_scene(const struct fight_state *s, const struct assets *a, struct scene *scene);
void fight_render(const struct assets *a, const struct scene *scene, uint16_t *fb);
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic);
void fight_encode(const struct fight_state *s, uint32_t words[STATE_WORDS]);
int fight_decode(struct fight_state *s, const struct assets *a,
		 const uint32_t words[STATE_WORDS]);
#endif
