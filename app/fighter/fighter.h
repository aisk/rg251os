/* Portable fighter state, FIGHTP04 content (a roster of characters with
 * frames, per-step boxes and motion, normals, specials and projectiles),
 * bounded draw commands and the RGB565 software renderer interface.
 * Rules (fighter.c) read the pack's steps, boxes and moves and only emit
 * draw commands; render.c owns the pack decoder, the per-match sprite cast
 * and the pixel loops. */
#ifndef FIGHTER_H
#define FIGHTER_H
#include <stddef.h>
#include <stdint.h>
#define FW 480
#define FH 320
#define COMMANDS 2048
#define STATE_WORDS 112
/* Ground line in screen rows; fighters' foot anchors stand on it. */
#define GROUND 260
/* Stage walls for fighter anchors. */
#define WALL_LEFT 50
#define WALL_RIGHT 430
/* Pack limits; packs that need more are rejected. Rule data and the
 * palette-index frames of the whole roster stay resident (sized for about
 * 30 characters); RGB565 pixels only for the two cast fighters and the
 * effects. */
#define MAX_CHARACTERS 32
#define MAX_PALETTES 64
#define MAX_FRAMES 4096
#define MAX_ROWS (1u << 19)
#define MAX_SPANS (1u << 20)
#define MAX_INDICES (1u << 23)
#define MAX_IMAGES 1024
#define MAX_PIXELS (1u << 21)
#define MAX_STEPS 16384
#define MAX_BOXES 32768
#define MAX_EFFECTS 64
#define MAX_SIDE 1024
#define MAX_NAME 12
/* Per-character, per-step and per-move limits, mirrored in pack.py. */
#define MAX_SPECIALS 8
#define MAX_MOVES (8 + MAX_SPECIALS)
#define MAX_PROJECTILE_TYPES 4
#define MAX_CHARACTER_STEPS 1024
#define MAX_STEP_BOXES 15
#define MAX_MOVE_TICKS 255
#define MAX_DAMAGE 1000
#define MAX_STUN 120
#define MAX_HITSTOP 30
#define MAX_PUSH 120
#define MAX_VELOCITY 16
#define MAX_HEIGHT 300
#define MAX_LIFETIME 600
/* Live projectiles; each fighter may own one at a time. */
#define MAX_PROJECTILES 4
/* Blended effect sprites (stress effects and blended projectiles) draw at
 * most this many opaque pixels per frame (about 12 Power Geyser flames);
 * later ones in the frame are dropped. */
#define EFFECT_PIXEL_BUDGET 76800u
/* Input handling, in ticks (see fighter.c): a press is buffered for
 * BUFFER_TICKS unfrozen ticks; motions must fit their windows in the
 * HISTORY-tick direction history; a charge needs CHARGE_TICKS and the
 * release direction within CHARGE_GRACE ticks of letting go. */
#define HISTORY 32
#define BUFFER_TICKS 4
#define WINDOW_QUARTER 15
#define WINDOW_DRAGON 20
#define CHARGE_TICKS 30
#define CHARGE_GRACE 10
#define CHARGE_RELEASE 4
/* Ticks between pressing up and leaving the ground. */
#define JUMP_SQUAT 3
/* Rounds a fighter must win to take a match (1 = single round). */
#define ROUNDS_TO_WIN 1
enum action {
	IDLE, WALK_FORWARD, WALK_BACK, JUMP, GUARD, HURT, KO, CROUCH, CROUCH_GUARD,
	CROUCH_HURT, ACTIONS
};
/* Normals; specials follow them in the move table. */
enum trigger {
	STAND_LP, STAND_HP, STAND_LK, STAND_HK, CROUCH_LP, CROUCH_HP, CROUCH_LK,
	CROUCH_HK, MOVES
};
/* Special move motions, directions relative to facing; [d] holds d. */
enum motion { MOTION_236, MOTION_214, MOTION_623, CHARGE_46, CHARGE_28, MOTIONS };
/* Button bits of a special's trigger. */
enum button { BUTTON_LP = 1, BUTTON_HP = 2, BUTTON_LK = 4, BUTTON_HK = 8 };
/* high: stand or crouch block; low: crouch block only; overhead: stand only. */
enum guard_type { GUARD_HIGH, GUARD_LOW, GUARD_OVERHEAD, GUARD_TYPES };
/* chain: a connected normal can be cancelled into a normal; special: into
 * a special. super is stored for later. */
enum cancel { CANCEL_CHAIN = 1, CANCEL_SPECIAL = 2, CANCEL_SUPER = 4, CANCEL_MASK = 7 };
enum blend { OPAQUE, ALPHA, ADD, BLENDS };
/* Game flow: the arcade ladder (select, fights, loss result, clear). */
enum mode { MODE_SELECT, MODE_FIGHT, MODE_RESULT, MODE_CLEAR, MODES };
/* One fighter. move is the move index + 1 (0 = none); elapsed counts
 * unfrozen ticks in the current move, stun or guard stance; hit is the last
 * hit group of the move that connected; stun counts down hitstun/blockstun
 * and freeze the hitstop; slide is the remaining pushback in pixels, signed
 * by direction, passed on to the opponent at a wall when corner is set.
 * block: 1 in blockstun, 2 in the proximity guard stance. keys holds the
 * previous tick's buttons for press edges. squat counts the jump start-up.
 * buffer is a pressed move index + 1 waiting up to BUFFER_TICKS unfrozen
 * ticks (age). charge packs back held/grace and down held/grace ticks, one
 * byte each; history holds the last HISTORY raw directions (4 bits each,
 * newest in the low bits of history[0]). plan/plan_tick drive the AI's
 * special inputs. */
struct fighter {
	int32_t x, y, vy, hp, face, walk, block, crouch, move, elapsed, hit, stun,
	    freeze, slide, keys, corner, squat, buffer, age, plan, plan_tick;
	uint32_t charge, history[HISTORY / 8];
};
/* A live projectile: kind is the owner's projectile index + 1 (0 = free). */
struct shot {
	int32_t kind, owner, x, y, face, age;
};
/* chr/pal are the cast (character, 0 normal or 1 alternate palette) of each
 * side; in the select screen chr[0] is the cursor. beaten has a bit per
 * character beaten in this ladder run, stage counts from 1, wins counts
 * rounds in the current match, timer counts ticks in a select, result or
 * clear screen, run counts ladder runs (the Auto rotation). */
struct fight_state {
	uint32_t tick, rng, previous, paused, stress, boxes, automatic, round;
	uint32_t mode, timer, beaten, stage, run, wins[2], chr[2], pal[2];
	struct fighter p[2];
	struct shot shot[MAX_PROJECTILES];
	uint32_t tone, phase, ended, hits, guards, specials, clashes;
};
/* One opaque run in a frame row; pixels index the frame's data. */
struct span {
	uint16_t x, len;
	uint32_t pixel;
};
/* A frame as palette indices, shared by every palette of its sheet. Row r's
 * spans are span[row[first_row + r] .. row[first_row + r + 1]). */
struct frame {
	int w, h, ax, ay;
	uint32_t first_row, index, count;
	uint16_t sheet;
	uint8_t max_index;
};
/* A frame expanded with one palette into resident RGB565 pixels. */
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
 * group numbers runs of consecutive hitbox steps in a move (0 = none).
 * vx/vy move the fighter forward/down each tick the step shows; spawn
 * launches projectile spawn - 1 on the step's first tick. */
struct step {
	uint16_t frame, ticks, box;
	uint8_t hurt, hit, group, spawn;
	int8_t vx, vy;
};
struct anim {
	uint16_t first, count, total;
};
/* The hit fields shared by moves and projectiles. */
struct hit {
	uint16_t damage, chip, hitstun, blockstun, hitstop, pushback, block_pushback;
	uint8_t guard;
};
/* Startup, active, recovery and reach are derived from the steps at load.
 * Specials have a motion and button bits; spawn is the projectile + 1 the
 * move launches (0 = none). */
struct move {
	struct anim anim;
	struct hit hit;
	uint8_t cancel, groups, startup, active, recovery, special, motion, buttons,
	    spawn;
	int16_t reach; /* furthest hitbox edge in front of the anchor */
};
struct projectile {
	struct anim anim;
	struct hit hit;
	int16_t speed, x, y;
	uint16_t lifetime;
	uint8_t blend, alpha;
};
struct character {
	char name[MAX_NAME + 1];
	uint16_t palette[2];
	unsigned moves, projectiles;
	struct anim action[ACTIONS];
	struct move move[MAX_MOVES];
	struct projectile projectile[MAX_PROJECTILE_TYPES];
	unsigned first_step, steps;
	uint32_t pixels; /* RGB565 pixels when cast (all frames it uses) */
	unsigned images;
};
/* cast[i] is the (character, palette) whose frames image[] currently holds
 * for side i (-1 = none); look[i][frame] is that side's image or -1. */
struct assets {
	uint32_t hash;
	unsigned characters, palettes, frames, rows, spans, indices, steps, boxes,
	    effects, images, pixels, effect_images, effect_pixels;
	int cast[2][2];
	uint32_t cast_pixels[2];
	struct character chr[MAX_CHARACTERS];
	uint16_t palette_sheet[MAX_PALETTES], palette_size[MAX_PALETTES];
	uint16_t palette[MAX_PALETTES][256];
	struct frame frame[MAX_FRAMES];
	int16_t look[2][MAX_FRAMES];
	struct image image[MAX_IMAGES];
	struct step step[MAX_STEPS];
	struct box box[MAX_BOXES];
	uint16_t effect[MAX_EFFECTS];
	uint32_t row[MAX_ROWS + 1];
	struct span span[MAX_SPANS];
	uint8_t index[MAX_INDICES];
	uint16_t pixel[MAX_PIXELS];
};
enum operation { RECT, SPRITE };
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
/* Back to the select screen (Auto: straight into a rotated ladder run). */
void fight_reset(struct fight_state *s, int automatic);
/* Start a round between characters c0 and c1 (mirror: P2 alternate). */
void fight_start(struct fight_state *s, unsigned c0, unsigned c1);
void fight_step(struct fight_state *s, const struct assets *a, uint32_t input,
		uint32_t second, int human);
/* Does fighter p's input history and charge complete motion m now? */
int fight_motion(const struct fighter *p, int motion);
/* Index into a->step of what fighter i shows (and collides with) now. */
unsigned fight_pose(const struct fight_state *s, const struct assets *a, int i);
struct area fight_box(const struct box *b, int x, int y, int face);
void fight_scene(const struct fight_state *s, const struct assets *a, struct scene *scene);
void fight_render(const struct assets *a, const struct scene *scene, uint16_t *fb);
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic);
/* Expand the state's two cast fighters into resident RGB565 images if they
 * changed; returns 1 when it expanded. Never fails for a loaded pack. */
int fight_cast(struct assets *a, const struct fight_state *s);
void fight_encode(const struct fight_state *s, uint32_t words[STATE_WORDS]);
int fight_decode(struct fight_state *s, const struct assets *a,
		 const uint32_t words[STATE_WORDS]);
#endif
