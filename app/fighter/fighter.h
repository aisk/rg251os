/* Portable fighter state, FIGHTP11 content (system profiles with walls,
 * effect sets, throw and run/backdash rules, a roster of characters with stats, frames, per-step boxes and
 * motion, normals, named specials and projectiles, spark animations and parallax
 * stages), bounded draw commands, the per-tick sound events and the RGB565
 * software renderer interface. Rules (fighter.c) read the pack's steps,
 * boxes, moves and each character's resolved stats, keep the camera, the
 * spark pool and the sound events in the state and only emit draw commands
 * (the HUD, banners and command lists included);
 * render.c owns the pack decoder (which fills stats and class defaults from
 * the character's system and applies its damage scale), the render cache
 * (the fighters and stage the state shows, expanded to RGB565 lazily and in
 * bounded slices) and the pixel loops. */
#ifndef FIGHTER_H
#define FIGHTER_H
#include <stddef.h>
#include <stdint.h>
#define FW 480
#define FH 320
#define COMMANDS 2048
#define STATE_WORDS 256
/* Ground line in screen rows; fighters' foot anchors stand on it (a stage's
 * ground row is drawn on it). */
#define GROUND 260
/* World: fighters and projectiles live in stage pixels, x in 0..width; the
 * camera cam is the stage column at the screen's left edge, 0..width - FW.
 * Without stages the world is FW wide. Walls sit margin pixels inside the
 * rule window, play_width wide and centred on the screen (both from the
 * stage's system); at the camera's ends they are the stage walls. */
#define MIN_STAGE_WIDTH FW
#define MAX_STAGE_WIDTH 2048
#define MIN_PLAY_WIDTH 320
#define MIN_MARGIN 8
#define MAX_MARGIN 48
/* Pack limits; packs that need more are rejected. Rule data and the
 * palette-index frames of the whole roster stay resident (sized for about
 * 30 characters); RGB565 pixels only for the two cast fighters and the
 * effects. */
#define MAX_CHARACTERS 32
#define MAX_PALETTES 64
#define MAX_FRAMES 4096
#define MAX_ROWS (1u << 19)
#define MAX_SPANS (1u << 20)
#define MAX_INDICES (1u << 24)
#define MAX_IMAGES 1024
#define MAX_PIXELS (1u << 22)
#define MAX_STEPS 16384
#define MAX_BOXES 32768
#define MAX_EFFECTS 64
#define MAX_SIDE 1024
#define MAX_NAME 12
/* Special move names in the command lists. */
#define MAX_MOVE_NAME 16
#define MAX_SYSTEMS 8
/* Spark animations (all systems' effect sets) and their steps. */
#define MAX_SPARKS 32
#define MAX_SPARK_STEPS 64
/* Stages: layers per stage, anims per layer, steps per anim; totals. A
 * layer's expanded image may be at most MAX_COVER pixels wide. */
#define MAX_STAGES 16
#define MAX_LAYERS 8
#define MAX_LAYER_ANIMS 8
#define MAX_ANIM_STEPS 64
#define MAX_STAGE_LAYERS (MAX_STAGES * MAX_LAYERS)
#define MAX_STAGE_ANIMS 256
#define MAX_STAGE_STEPS 2048
#define MAX_COVER 4096
/* Live sparks in the state; a new one replaces the oldest when full. */
#define SPARK_SLOTS 8
/* Screen shake: at most SHAKE_MAX pixels up or down for shake ticks. */
#define SHAKE_MAX 4
#define SHAKE_HEAVY 10
#define SHAKE_KO 16
/* KO white flash ticks. */
#define FLASH_TICKS 3
/* Per-character, per-step and per-move limits, mirrored in pack.py. */
#define MAX_SPECIALS 8
#define MAX_MOVES (8 + MAX_SPECIALS)
#define MAX_PROJECTILE_TYPES 4
#define MAX_CHARACTER_STEPS 1024
#define MAX_STEP_BOXES 15
#define MAX_MOVE_TICKS 255
/* Damage as written (thousandths of the reference life 1000), before the
 * system's damage scale. */
#define MAX_DAMAGE 1500
#define MAX_STUN 120
#define MAX_HITSTOP 30
#define MAX_PUSH 120
/* Stat bounds: life, per-mille factors (defense, weight, damage scale),
 * walk speed in quarter pixels per tick, jump velocity and gravity in
 * quarter pixels, prejump ticks. A defender's weight scales the pushback it
 * receives, so a slide can reach MAX_SLIDE pixels. */
#define MAX_LIFE 4000
#define MIN_DEFENSE 100
#define MIN_WEIGHT 250
#define MIN_SCALE 100
#define MAX_PERMILLE 4000
#define MAX_WALK 64
#define MAX_JUMP_VY 255
#define MAX_GRAVITY 32
#define MAX_PREJUMP 15
#define MAX_SLIDE (MAX_PUSH * 4)
#define MAX_BUFFER 15
#define MAX_CHARGE 200
#define MAX_GRACE 60
#define MAX_PUSH_DISTANCE 200
#define MAX_VELOCITY 16
#define MAX_HEIGHT 300
#define MAX_LIFETIME 600
/* Live projectiles; each fighter may own one at a time. */
#define MAX_PROJECTILES 4
/* Blended effect sprites (stress effects and blended projectiles) draw at
 * most this many opaque pixels per frame (about 12 Power Geyser flames);
 * later ones in the frame are dropped. */
#define EFFECT_PIXEL_BUDGET 76800u
/* Direction history in ticks; the input windows (struct stats) fit in it. */
#define HISTORY 32
/* Rounds a fighter must win to take a match (1 = single round). */
#define ROUNDS_TO_WIN 1
/* A ladder round opens with the versus card (INTRO_TICKS, while the core
 * fills the render cache), then READY over the frozen stage (READY_TICKS);
 * the rules hold still through both (no input, no AI). The round runs from
 * ROUND_TICKS on, when ROUND_START sounds, and FIGHT stays on screen for
 * FIGHT_TICKS more (the timer stops at BANNER_END). */
#define INTRO_TICKS 45
#define READY_TICKS 36
#define FIGHT_TICKS 24
#define ROUND_TICKS (INTRO_TICKS + READY_TICKS)
#define BANNER_END (ROUND_TICKS + FIGHT_TICKS)
/* Combo counter: "N HITS" stays this many ticks after a combo of at least
 * two hits ends; counts stop at MAX_COMBO. */
#define COMBO_TICKS 60
#define MAX_COMBO 99
/* Cast frames a scene can report missing (see struct scene). */
#define SCENE_MISSES 16
/* FALL, DOWN and GETUP are the knockdown (see fighter.c): knocked off the
 * feet until landing, lying, getting up. THROW is a normal throw's
 * thrower, THROWN the fighter it holds. RUN loops while running (and
 * through the skid), BACKDASH plays from the backdash's first tick. */
enum action {
	IDLE, WALK_FORWARD, WALK_BACK, JUMP, GUARD, HURT, KO, CROUCH, CROUCH_GUARD,
	CROUCH_HURT, FALL, DOWN, GETUP, THROW, THROWN, RUN, BACKDASH, ACTIONS
};
/* Normals; specials follow them in the move table. */
enum trigger {
	STAND_LP, STAND_HP, STAND_LK, STAND_HK, CROUCH_LP, CROUCH_HP, CROUCH_LK,
	CROUCH_HK, MOVES
};
/* Special move motions, directions relative to facing; [d] holds d. From
 * MOTION_ANY to HOLD_4 a follow-up's command is the button with a direction
 * held on the press (any, none, up, down, forward or back; numpad 7/8/9
 * count as up, 1/2/3 as down, 3/6/9 forward, 1/4/7 back). The half circles
 * 41236 and 63214 and 421 come after them so older motion numbers keep
 * their meaning. */
enum motion {
	MOTION_236, MOTION_214, MOTION_623, CHARGE_46, CHARGE_28, MOTION_ANY, HOLD_5, HOLD_8,
	HOLD_2, HOLD_6, HOLD_4, MOTION_41236, MOTION_63214, MOTION_421, MOTIONS
};
/* Button bits of a special's trigger. */
enum button { BUTTON_LP = 1, BUTTON_HP = 2, BUTTON_LK = 4, BUTTON_HK = 8 };
/* high: stand or crouch block; low: crouch block only; overhead: stand only. */
enum guard_type { GUARD_HIGH, GUARD_LOW, GUARD_OVERHEAD, GUARD_TYPES };
/* chain: a connected normal can be cancelled into a normal; special: into
 * a special. super is stored for later. */
enum cancel { CANCEL_CHAIN = 1, CANCEL_SPECIAL = 2, CANCEL_SUPER = 4, CANCEL_MASK = 7 };
/* Move class: picks the system's default hitstun, blockstun and hitstop
 * (CLASS_NONE: the move gives its own). */
enum move_class {
	CLASS_NONE, CLASS_LIGHT, CLASS_HEAVY, CLASS_COMMAND, CLASS_SPECIAL, CLASS_SUPER,
	CLASS_THROW, CLASSES
};
/* Hit flags: an unguarded hit (a move's last hit group) knocks down; the
 * move also hits a lying fighter. */
enum hit_flag { FLAG_KNOCKDOWN = 1, FLAG_OTG = 2, FLAG_MASK = 3 };
/* Knockdown phases (struct fighter down). */
enum down_phase { UP, FALLING, LYING, DOWN_PHASES };
#define MAX_WAKEUP 120
/* Normal throws (struct fighter grab): throwing forward or back, held, and
 * held after a tech press (only inside a tick). */
enum grab { GRAB_NONE, GRAB_FORWARD, GRAB_BACK, GRAB_HELD, GRAB_TECH, GRABS };
#define MAX_THROW_RANGE 64
#define MAX_TECH 15
#define MAX_THROW_SAFE 60
/* Run and backdash (struct fighter dash; elapsed counts the phase's
 * ticks): running (66, held forward), coasting the grace ticks after
 * forward is let go, the skid to a stop, and the backdash (44). */
enum dash_phase { DASH_NONE, DASH_RUN, DASH_COAST, DASH_STOP, DASH_BACK, DASH_PHASES };
#define MAX_DASH_TICKS 60
#define MAX_RUN 128
#define MAX_BACKDASH 160
/* Bits of a packed move's defaults: the field comes from its class. */
enum hit_default { DEFAULT_HITSTUN = 1, DEFAULT_BLOCKSTUN = 2, DEFAULT_HITSTOP = 4 };
/* Standing hurt height group, for the AI and tools only. */
enum height_class { HEIGHT_LOW, HEIGHT_MID, HEIGHT_HIGH, HEIGHTS };
enum blend { OPAQUE, ALPHA, ADD, BLENDS };
/* Stage layer edge rules for pixels outside a layer: leave them, repeat the
 * outermost column/row, tile, fill with the layer's colour, reflect. */
enum edge { EDGE_NONE, EDGE_EXTEND, EDGE_REPEAT, EDGE_COLOR, EDGE_MIRROR, EDGES };
/* A system's effect set: the spark each kind of contact spawns. Strikes
 * pick light/heavy/special by class (see fighter.c), weapon moves the slash
 * set plus blood. */
enum effect_key {
	FX_HIT_LIGHT, FX_HIT_HEAVY, FX_HIT_SPECIAL, FX_GUARD, FX_SLASH_LIGHT, FX_SLASH_HEAVY,
	FX_SLASH_SPECIAL, FX_SLASH_GUARD, FX_BLOOD_LIGHT, FX_BLOOD_HEAVY, FX_BLOOD_SPECIAL,
	FX_KEYS
};
/* CPU difficulty (struct fight_state level; see the AI in fighter.c). */
enum level { LEVEL_EASY, LEVEL_NORMAL, LEVEL_HARD, LEVELS };
/* Game flow: the select screen, fights, the result screen (a lost ladder
 * or survival run) and the clear screen (a finished ladder). */
enum mode { MODE_SELECT, MODE_FIGHT, MODE_RESULT, MODE_CLEAR, MODES };
/* Game modes, picked on the select screen (L cycles them): the arcade
 * ladder; survival, one life bar carried through endless random
 * opponents with SURVIVAL_HEAL per mille of life back after each win; and
 * practice against a dummy whose life refills. */
enum game { GAME_ARCADE, GAME_SURVIVAL, GAME_PRACTICE, GAMES };
#define SURVIVAL_HEAL 150
#define MAX_STREAK 9999
/* The practice dummy (R cycles it in a practice fight): standing,
 * crouching, guarding everything, or the CPU. */
enum dummy { DUMMY_STAND, DUMMY_CROUCH, DUMMY_GUARD, DUMMY_CPU, DUMMIES };
/* One fighter. x is in quarter pixels (x / 4 is the anchor column), y in
 * pixels, vy in quarter pixels per tick; hp counts down from the
 * character's life. move is the move index + 1 (0 = none); elapsed counts
 * unfrozen ticks in the current move, stun or guard stance; hit is the last
 * hit group of the move that connected; stun counts down hitstun/blockstun
 * and freeze the hitstop; slide is the remaining pushback in pixels, signed
 * by direction, passed on to the opponent at a wall when corner is set.
 * block: 1 in blockstun, 2 in the proximity guard stance. keys holds the
 * previous tick's buttons for press edges. squat counts the prejump.
 * buffer is a pressed move index + 1 waiting up to stats.buffer unfrozen
 * ticks (age). charge packs back held/grace and down held/grace ticks, one
 * byte each; history holds the last HISTORY raw directions (4 bits each,
 * newest in the low bits of history[0]). plan/plan_tick drive the AI's
 * special inputs. clock counts every tick (frozen ones too, at most 255)
 * since the current move started: a follow-up's motion must lie within it.
 * down is the knockdown phase (enum down_phase; elapsed counts its ticks):
 * FALLING until the fall animation is over and the fighter is on the
 * ground, then LYING through lying and getting up. grab is the normal throw
 * state (enum grab; elapsed counts its ticks, equal on both sides); safe
 * counts down the ticks the fighter cannot be thrown after rising or
 * leaving hitstun or blockstun. dash is the run or backdash phase (enum
 * dash_phase; elapsed counts its ticks). */
struct fighter {
	int32_t x, y, vy, hp, face, walk, block, crouch, move, elapsed, hit, stun,
	    freeze, slide, keys, corner, squat, buffer, age, plan, plan_tick, clock, down, grab,
	    safe;
	uint32_t charge, history[HISTORY / 8];
	int32_t dash;
};
/* A live projectile: kind is the owner's projectile index + 1 (0 = free). */
struct shot {
	int32_t kind, owner, x, y, face, age;
};
/* A live spark: kind is the spark index + 1 (0 = free), x the world column
 * and y the screen row of its anchor, age its ticks shown, flip 1 when
 * mirrored. */
struct spark_slot {
	int32_t kind, x, y, age, flip;
};
/* Sound events. fight_step clears fight_state.sound and appends what the
 * rules emitted during that tick (at most MAX_SOUND_EVENTS, in emission
 * order; later ones are dropped). The list is
 * part of the rule state, so it is deterministic, saved and restored: a
 * consumer reads it after each fight_step and never needs to look at the
 * scene. A paused tick emits nothing. The ids are a stable interface (the
 * audio side maps them to clips); new ones are only ever appended before
 * SOUNDS.
 *   WHOOSH_*        a move starts (normal or special): LIGHT for the light
 *                   class or classless LP/LK normals, HEAVY for the heavy,
 *                   command and throw classes or classless HP/HK normals,
 *                   SPECIAL for the special and super classes and classless
 *                   specials. side/chr/system: the mover; move: its index
 *                   + 1.
 *   HIT_*           a strike or projectile connects unguarded: LIGHT/HEAVY/
 *                   SPECIAL by the hit's class as above. Weapon moves (the
 *                   character's slash buttons, see struct character) emit
 *                   SLASH_* instead. side: the attacker.
 *   GUARD           a strike or projectile is guarded (SLASH_GUARD for
 *                   weapon moves). side: the attacker.
 *   JUMP            a fighter takes off (end of the prejump).
 *   LAND            an airborne fighter lands.
 *   LAUNCH          a projectile is spawned. move: the projectile's move.
 *   CLASH           two projectiles cancel out. side: the lower slot's owner.
 *   KO              a fighter's life reaches 0. side: the fighter knocked
 *                   out.
 *   ROUND_START     a round begins: FIGHT after READY in the ladder, at once
 *                   in fight_start. side 0, P1's character.
 * side is 0 or 1, chr the character index in the pack, system its system
 * profile index (struct assets sys[]), move a move index + 1 or 0. */
enum sound_id {
	SOUND_WHOOSH_LIGHT, SOUND_WHOOSH_HEAVY, SOUND_WHOOSH_SPECIAL,
	SOUND_HIT_LIGHT, SOUND_HIT_HEAVY, SOUND_HIT_SPECIAL, SOUND_GUARD,
	SOUND_SLASH_LIGHT, SOUND_SLASH_HEAVY, SOUND_SLASH_SPECIAL, SOUND_SLASH_GUARD,
	SOUND_JUMP, SOUND_LAND, SOUND_LAUNCH, SOUND_CLASH, SOUND_KO, SOUND_ROUND_START,
	SOUNDS
};
#define MAX_SOUND_EVENTS 16
struct sound_event {
	uint8_t id, side, chr, system, move;
};
/* chr/pal are the cast (character, 0 normal or 1 alternate palette) of each
 * side; in the select screen chr[0] is the cursor. beaten has a bit per
 * character beaten in this ladder run, stage counts from 1, wins counts
 * rounds in the current match, timer counts ticks in a select, result or
 * clear screen and, in the fight mode, the versus card and READY (the round
 * runs once it reaches ROUND_TICKS) and the FIGHT banner (up to
 * BANNER_END), run counts ladder runs (the Auto rotation). arena is the
 * pack stage of the fight (0 without stages), cam the camera column, shake
 * and flash the remaining screen shake and KO flash ticks. Combos, per
 * attacking side: combo counts the unguarded hits since the defender last
 * left hitstun (0 = none running), shown the count on screen (at least 2,
 * 0 = none) for show more ticks once the combo ended, best the longest
 * combo of this round. list is set while the select screen shows the
 * highlighted character's command list (START toggles it there). level is
 * the CPU difficulty (enum level, LEVEL_NORMAL after fight_reset; R cycles
 * it on the select screen, the core sets its option at load, reset and when
 * the option changes while the select screen shows). throws and techs count
 * the normal throws that landed and those broken, runs and backdashes the
 * runs and backdashes started. game is the game mode (enum game, L cycles
 * it on the select screen); picking is set while a practice select picks
 * the dummy (chr[1] is then the cursor); dummy is the practice dummy's
 * behaviour (enum dummy, R in a practice fight); streak counts the wins of
 * the current survival run and record the most of this session; carry is
 * P1's life for the next survival fight (0 outside a survival fight).
 * damage is the life an attacking side's running combo took (0 = none),
 * last what its latest combo took (shown in practice). */
struct fight_state {
	uint32_t tick, rng, previous, paused, stress, boxes, automatic, round;
	uint32_t mode, timer, beaten, stage, run, wins[2], chr[2], pal[2];
	struct fighter p[2];
	struct shot shot[MAX_PROJECTILES];
	uint32_t tone, phase, ended, hits, guards, specials, clashes;
	uint32_t arena, cam, shake, flash;
	struct spark_slot spark[SPARK_SLOTS];
	/* This tick's sound events (see enum sound_id). */
	uint32_t sounds;
	struct sound_event sound[MAX_SOUND_EVENTS];
	uint32_t combo[2], shown[2], show[2], best[2], list, level, throws, techs;
	uint32_t runs, backdashes;
	uint32_t game, picking, dummy, streak, record, carry, damage[2], last[2];
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
/* The hit fields shared by moves and projectiles; damage and chip are
 * already scaled by the owner's system (life points before the defender's
 * defense). flags: enum hit_flag (projectiles only knock down). */
struct hit {
	uint16_t damage, chip, hitstun, blockstun, hitstop, pushback, block_pushback;
	uint8_t guard, cls, flags;
};
/* Startup, active, recovery and reach are derived from the steps at load.
 * Specials have a motion and button bits; spawn is the projectile + 1 the
 * move launches (0 = none); slash is set for weapon moves (the character's
 * slash mask). A follow-up (the next stage of a rekka) has after = its
 * parent move + 1: it never starts on its own, only from the parent once
 * that has shown an elapsed in open..close (and connected when contact is
 * set); see fighter.c. name is a special's command-list name (empty for
 * normals). */
struct move {
	struct anim anim;
	struct hit hit;
	uint8_t cancel, groups, startup, active, recovery, special, motion, buttons,
	    spawn, slash, after, open, close, contact;
	int16_t reach; /* furthest hitbox edge in front of the anchor */
	char name[MAX_MOVE_NAME + 1];
};
struct projectile {
	struct anim anim;
	struct hit hit;
	int16_t speed, x, y;
	uint16_t lifetime;
	uint8_t blend, alpha;
};
/* A jump: takeoff vy (negative, quarter px/tick), gravity added each
 * airborne tick, air drift forward/back (quarter px/tick) and prejump ticks.
 * Derived at load: peak, the jump's height in pixels (at most MAX_HEIGHT),
 * and fall, the largest vy an airborne fighter can have (falling from
 * MAX_HEIGHT, as after a hit during a tall move). */
struct jump {
	int16_t vy, gravity, vx, vx_back, prejump, peak, fall;
};
/* A character's resolved stats (system defaults with its overrides). life
 * in points (reference 1000); defense and weight per mille; walk speeds in
 * quarter pixels per tick; height is an enum height_class. From the system
 * only: push, the body distance in pixels (closer than push - 1 is pushed
 * out to push), guard_range in pixels, and the input windows buffer,
 * quarter, dragon, charge, grace and release in ticks (see fighter.c), and
 * wakeup, the ticks from landing in a knockdown to acting again; run, the
 * running speed in quarter pixels per tick (walk times the system's run
 * scale, derived at load). */
struct stats {
	int16_t life, defense, walk, walk_back, weight, push, guard_range, run;
	uint8_t height, buffer, quarter, dragon, charge, grace, release, wakeup;
	struct jump jump;
};
/* Default hitstun/blockstun/hitstop of a move class; has bit 0 stun, bit 1
 * hitstop. */
struct class_row {
	uint8_t hitstun, blockstun, hitstop, has;
};
/* Normal throws of a system's characters: range, the body gap in pixels
 * beyond the push distance a throw reaches; tech, the ticks after the grab
 * in which the victim's HP or HK breaks it (0 none); tech_stun and
 * tech_push, both fighters' recovery (guard pose) and slide in pixels after
 * a break; wakeup_safe and stun_safe, the ticks a fighter cannot be thrown
 * after rising from a knockdown or leaving hitstun/blockstun. hit is the
 * throw's damage (scaled at load), pushback and hitstop, class throw, and
 * it knocks down. */
struct throw_rule {
	uint8_t range, tech, tech_stun, tech_push, wakeup_safe, stun_safe;
	struct hit hit;
};
/* Run and backdash of a system's characters (ticks unless noted): window,
 * the ticks within which the second tap of 66 or 44 must follow the first;
 * run_scale, the run speed per mille of the walk speed; a run moves at
 * walk speed for run_startup ticks, then at run speed, lasts at least
 * run_startup + run_min ticks, coasts run_grace ticks once forward is
 * let go (forward again keeps running) and skids run_stop ticks. A backdash
 * waits back_startup ticks on the ground, flies back_air ticks covering
 * back_distance pixels in an arc back_height pixels high, and lands for
 * back_stop ticks; it cannot be thrown and strikes miss it for its first
 * back_invuln ticks. */
struct dash_rule {
	uint8_t window, run_startup, run_min, run_grace, run_stop, back_startup, back_air,
	    back_stop, back_distance, back_height, back_invuln;
	uint16_t run_scale;
};
/* A system profile: the stat defaults, the damage scale applied to its
 * characters' moves at load, the class table, the wall margin and rule
 * window (play_width) of its stages, its effect set (spark + 1, 0 =
 * none, per enum effect_key), its normal throws and its run and backdash. */
struct system {
	char name[MAX_NAME + 1];
	uint16_t damage_scale, margin, play_width;
	uint16_t effect[FX_KEYS];
	struct stats stats;
	struct class_row cls[CLASSES];
	struct throw_rule throw;
	struct dash_rule dash;
};
struct character {
	char name[MAX_NAME + 1];
	unsigned system;
	struct stats stats;
	uint16_t palette[2];
	unsigned moves, projectiles;
	struct anim action[ACTIONS];
	struct move move[MAX_MOVES];
	struct projectile projectile[MAX_PROJECTILE_TYPES];
	unsigned first_step, steps;
	uint32_t pixels; /* RGB565 pixels when cast (all frames it uses) */
	unsigned images;
	/* slash: bits 0..3 the LP/HP/LK/HK normals, bit 4 the specials, are
	 * weapon moves; home: the ladder's stage for this opponent + 1 (0 =
	 * stage index % stages). */
	uint8_t slash;
	uint16_t home;
};
/* A spark animation: steps spark_step[first .. first + count), total ticks;
 * opaque ones draw as sprites, blended ones within the effect budget. */
struct spark {
	uint16_t first, count, total;
	uint8_t blend, alpha;
};
struct spark_step {
	uint16_t image, ticks;
};
/* A stage layer: frame (palette indices, anchor = minus its offset inside
 * the w x h layer) with palette, screen x at camera 0, top y in stage rows,
 * horizontal motion cam * num / den (floor), edge rules [x, y] and fill
 * colour, anims stage_anim[first_anim ..). */
struct layer {
	uint16_t frame, palette, w, h;
	int16_t x, y, num;
	uint16_t den;
	uint8_t edge[2];
	uint16_t color, first_anim, anims;
};
/* Steps stage_step[first .. first + count) cycle with the global tick. A
 * step draws frame (or nothing at 0xffff) with its top-left at layer pixel
 * (x, y) minus the frame's anchor. */
struct stage_anim {
	uint16_t first, count;
	uint32_t total;
};
struct stage_step {
	uint16_t frame, ticks;
	int16_t x, y;
};
/* width and ground (stage row drawn on GROUND); system gives the walls.
 * pixels/rows/spans/images: what expanding it costs (proven to fit). */
struct stage {
	char name[MAX_NAME + 1];
	uint16_t width, ground, system, first_layer, layers;
	uint32_t pixels, rows, spans;
	unsigned images;
};
/* The render cache (never part of the state). After the effects, image[]
 * and pixel[] hold three regions sized at load for the largest stage and
 * the largest character: the stage's (region 0), side 0's and side 1's;
 * images and pixels are their end, slots not yet filled have w 0.
 * cast[i] is the (character, palette) side i's region belongs to (-1 =
 * none); look[i][frame] is that side's image or -1 while not expanded,
 * cast_images/cast_pixels what it holds and cast_next the next character
 * step the prefetch looks at. cast_stage is the stage of region 0 (-1 =
 * none): its layers are expanded in order, stage_layer the one in progress
 * (layers = complete) and stage_row the next row of its image, then its
 * anim steps (-1: not begun); layer_image[k] is layer k's image (-1 =
 * nothing to draw) covering layer pixels from (layer_x0, layer_y0),
 * stage_look[step] each anim step's image or -1. Rows and spans past
 * pack_rows/pack_spans belong to the expanded layers. wanted has bit i for
 * side i and bit 2 for the stage when the state draws them. */
struct assets {
	uint32_t hash;
	unsigned systems, characters, palettes, frames, rows, spans, indices, steps, boxes,
	    effects, images, pixels, effect_images, effect_pixels, sparks, spark_steps, stages,
	    layers, anims, stage_steps, pack_rows, pack_spans;
	uint32_t region_image[3], region_pixel[3], char_images, char_pixels, stage_images,
	    stage_cap;
	int cast[2][2], cast_stage, stage_row;
	unsigned cast_images[2], cast_next[2], stage_layer, stage_used, wanted;
	uint32_t cast_pixels[2], stage_pixels;
	int layer_image[MAX_LAYERS];
	int16_t layer_x0[MAX_LAYERS], layer_y0[MAX_LAYERS];
	struct system sys[MAX_SYSTEMS];
	struct character chr[MAX_CHARACTERS];
	struct spark spark[MAX_SPARKS];
	struct spark_step spark_step[MAX_SPARKS * MAX_SPARK_STEPS];
	struct stage stage[MAX_STAGES];
	struct layer layer[MAX_STAGE_LAYERS];
	struct stage_anim anim[MAX_STAGE_ANIMS];
	struct stage_step stage_step[MAX_STAGE_STEPS];
	int16_t stage_look[MAX_STAGE_STEPS];
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
/* effects_requested/effects_drawn count blended effect sprites this frame,
 * sparks the spark sprites, layers the stage layer and anim sprites. The
 * cast frames the scene wanted but the cache has not expanded yet are in
 * miss (side << 15 | frame, at most SCENE_MISSES, misses counts all) and
 * were left out; stage_missing is set when the arena's stage is still
 * incomplete and the flat background stood in. fight_demand fills both. */
struct scene {
	struct command cmd[COMMANDS];
	unsigned count, effects_requested, effects_drawn, sparks, layers, misses, stage_missing;
	uint16_t miss[SCENE_MISSES];
};
/* Screen rectangle of a box for a fighter anchored at x, GROUND + y. */
struct area {
	int x, y, w, h;
};
/* World width and walls (pixels, for the camera in s) of the state's arena. */
int fight_width(const struct fight_state *s, const struct assets *a);
void fight_walls(const struct fight_state *s, const struct assets *a, int *left, int *right);
/* Back to the select screen (Auto: straight into a rotated ladder run). */
void fight_reset(struct fight_state *s, int automatic);
/* Start a round between characters c0 and c1 (mirror: P2 alternate), each
 * with its own life, on c1's stage, centred with the camera on them. */
void fight_start(struct fight_state *s, const struct assets *a, unsigned c0, unsigned c1);
void fight_step(struct fight_state *s, const struct assets *a, uint32_t input,
		uint32_t second, int human);
/* Does fighter p's input history and charge complete motion m now, with
 * the input windows of its stats? */
int fight_motion(const struct fighter *p, const struct stats *st, int motion);
/* Index into a->step of what fighter i shows (and collides with) now. */
unsigned fight_pose(const struct fight_state *s, const struct assets *a, int i);
struct area fight_box(const struct box *b, int x, int y, int face);
void fight_scene(const struct fight_state *s, const struct assets *a, struct scene *scene);
void fight_render(const struct assets *a, const struct scene *scene, uint16_t *fb);
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic);
/* Screen x of a stage layer's left edge with the camera at cam. */
int fight_layer_x(const struct layer *l, int cam);
/* The render cache (render.c). fight_want points it at what state s draws:
 * in the fight mode both sides' (character, palette) and the arena's
 * stage, both sides while a practice select picks the dummy, otherwise
 * side 0 only (the select, result and clear screens show P1; side 1 and
 * the stage keep what they hold). A region whose owner
 * changes is emptied; nothing is expanded. Returns bit i when side i
 * changed, bit 2 for the stage. fight_prefetch expands about budget pixels
 * of what is still missing (a stage row counts its width): the stage, then
 * side 0, then side 1; it returns the pixels done. fight_demand expands
 * what a scene reported missing (its frames, an incomplete stage) so the
 * scene can be built again; fight_ready tells whether everything wanted is
 * expanded. fight_cast is all of it at once (returns 1 if it expanded
 * anything); it never fails for a loaded pack. */
unsigned fight_want(struct assets *a, const struct fight_state *s);
uint32_t fight_prefetch(struct assets *a, uint32_t budget);
uint32_t fight_demand(struct assets *a, const struct scene *scene);
int fight_ready(const struct assets *a);
int fight_cast(struct assets *a, const struct fight_state *s);
/* The command list of the pause and select screens, from the pack: every
 * special without a parent in pack order, then every normal that has
 * follow-ups, each followed by its follow-ups one level deeper (depth, at
 * most 3). name is the move's name (empty for a normal); a line is the motion as the HUD font's arrows for a fighter
 * facing right (bytes 1..9 point like the numpad digit; [d] holds d), " + "
 * and the buttons (P, K, or LP/HP/LK/HK; S, LS, HS for weapon punches),
 * then the tags PROJECTILE, LOW, OVERHEAD and ON CONTACT (a follow-up that
 * needs its parent to connect). Returns the line count. */
#define LIST_LINES 16
#define LIST_TEXT 48
unsigned fight_move_list(const struct character *c, char name[LIST_LINES][MAX_MOVE_NAME + 1],
			 char text[LIST_LINES][LIST_TEXT], uint8_t depth[LIST_LINES]);
void fight_encode(const struct fight_state *s, uint32_t words[STATE_WORDS]);
int fight_decode(struct fight_state *s, const struct assets *a,
		 const uint32_t words[STATE_WORDS]);
#endif
