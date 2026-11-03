/* Deterministic fighter rules and the game modes (arcade ladder, survival,
 * practice): movement, run and
 * backdash, prejump,
 * jumps, crouching, data-driven normals and specials with an input buffer,
 * motion and charge commands, cancels, projectiles with clashes, hurt/hit
 * boxes from the pack, hitstop, hitstun, blockstun, high/low/overhead guard,
 * pushback with corner transfer, knockdown and wake-up, normal throws with
 * techs, KO, combo counts,
 * AI (with specials), the
 * camera and walls, hit sparks, screen shake, the KO flash, sound events,
 * the select, result and clear screens, the READY/FIGHT round start and the
 * stress scenes, plus the draw-command list (parallax stage or the flat
 * fallback, the attacking fighter drawn on top, sparks, L overlays the real
 * boxes, "N HITS", the pause and select command lists with the specials'
 * names; a frame or stage
 * the render cache has not expanded yet is left out and reported in the
 * scene). Rules pick an
 * action or move and its elapsed ticks; the pack's steps map that onto
 * frames, boxes and motion, so what is drawn is what collides. Feel comes
 * from each character's resolved stats (life, defense, weight, walk speeds,
 * jump, prejump, input windows, body push distance and guard range; see
 * struct stats), never from constants here. A fighter's x is in quarter
 * pixels (boxes, projectiles and drawing use x / 4), y in pixels and vy in
 * quarter pixels per tick; step motion and pushback move whole pixels.
 *
 * Timing, in ticks (one fight_step each): a move started on tick T shows
 * elapsed 0 on T and ends on tick T + total, when the fighter may act
 * again. A contact on tick C freezes both fighters for hitstop ticks
 * (C+1 .. C+hitstop); the defender is then stunned and acts again on tick
 * C + hitstop + hitstun (or blockstun). Damage and chip are the move's
 * (already scaled by its system) times the defender's defense / 1000,
 * rounded, at least 1 when nonzero. Pushback, times 1000 / the defender's
 * weight, slides the defender ceil(rest / 4) pixels per unfrozen tick; what
 * a wall stops moves the attacker back instead (not for projectile hits).
 *
 * Camera and walls: fighters and projectiles live in world (stage) pixels.
 * Every tick ends by setting cam = clamp(midpoint - FW / 2, 0, width - FW)
 * (the midpoint of the two anchors, floored) and clamping both fighters to
 * the walls of that camera: margin pixels inside the play_width rule window
 * centred on the screen (margin and play_width from the stage's system).
 * During the tick the walls of the previous camera apply to every move,
 * slide and push. With the camera at a stage end the window's wall is the
 * stage wall, so both fighters are always on screen and the corner transfer
 * works at stage and screen walls alike; fighters further apart than the
 * window cannot separate (the screen holds the one moving back).
 *
 * Effects: a strike or projectile contact spawns its system's spark at the
 * centre of the first overlapping hitbox/hurtbox pair (hit, guard; weapon
 * moves the slash spark plus blood on a hit) into SPARK_SLOTS state slots,
 * replacing the oldest when full; sparks age every fight tick (also in
 * hitstop) and vanish after their animation. An unguarded heavy or special
 * hit shakes the screen SHAKE_HEAVY ticks, a KO SHAKE_KO ticks and flashes
 * it white for FLASH_TICKS. Strength: class light is light; heavy, command
 * and throw heavy; special and super special; a move without a class goes
 * by its button (LP/LK light, HP/HK heavy, specials and projectiles
 * special). Sound events (enum sound_id) are appended to the state as the
 * rules act.
 * CPU difficulty (the state's level, set by R on the select screen or the
 * core option): the levels table sets the AI's guard
 * rate, wrong-height guards and guard reaction, how often it starts
 * specials and normals, its follow-ups, Hard's cancels of connected
 * normals into motion specials, how many of its plans walk in to throw,
 * how often and how soon it breaks a throw, and on Easy the ladder CPU's
 * damage (75%).
 * Normal is the original AI. Everything reads the state's rng and tick, so
 * replays and states stay deterministic.
 * Game modes (L on the select screen): Arcade is the ladder. Survival
 * fights random opponents (never the previous one) one round each with
 * P1's life carried over, SURVIVAL_HEAL per mille back per win; the streak
 * ends on a loss and the session's best is kept. Practice picks P1, then
 * the dummy, which stands, crouches, guards everything (high or low) or
 * fights as the CPU (R in the fight cycles it); life never drops under 1
 * and both bars refill when a combo ends, and each side's last combo damage
 * is shown. B in a paused fight quits to the select screen.
 * Combos: an unguarded hit (strike or projectile) on a defender still in
 * hitstun as the tick's contacts land continues the attacker's combo,
 * any other starts it at 1; the combo ends at the end of the first tick
 * the defender is out of hitstun. From two hits on "N HITS" shows under the
 * attacker's name until COMBO_TICKS after the combo ended; each round keeps
 * both sides' longest (best).
 * A step's motion moves
 * the fighter on every unfrozen tick it shows, from the move's first tick;
 * a projectile launches on its step's first tick at the owner's anchor plus
 * its offset, moves speed pixels on every later tick, never freezes,
 * freezes only the defender on a hit and vanishes on a hit, a clash with
 * the other side's projectile, after lifetime ticks or 64 pixels off
 * screen. Up starts a prejump of jump.prejump ticks, then the fighter takes
 * off with jump.vy and gains jump.gravity each airborne tick (y moves vy / 4
 * pixels); held forward/back drifts it jump.vx/vx_back while airborne, walk/
 * walk_back on the ground. Any move started in the prejump replaces the
 * jump. Grounded bodies closer than push - 1 pixels (the larger of the two
 * fighters' push) are pushed apart to push pixels.
 *
 * Input: every tick (frozen or not) records the raw direction in a
 * HISTORY-tick history and updates the back/down charge counters. A button
 * press is resolved at once against the history (specials first, in pack
 * order, then the normal for the pressed button, crouching with down held)
 * and buffered: it starts as soon as the fighter can act, within buffer
 * unfrozen ticks of the press (frozen ticks do not count, so a press in
 * hitstop survives it; a press in the last buffer recovery ticks comes out
 * on the first free tick). A newer press replaces the buffer unless that
 * would trade a special for a normal. Motions are matched backwards from
 * the press, relative to the current facing: 236 and 214 within quarter
 * ticks, 623, 421 and the half circles within dragon (a half circle needs
 * its 4, 2 and 6 in order, 41236 back to forward and 63214 forward to back,
 * so it also completes a 236 or 214: pack order decides, half circles
 * first); [4]6 and [2]8 need charge ticks of back/down,
 * released at most grace ticks ago (or still held), and forward/up in the
 * last release ticks (all from the fighter's stats). A projectile special
 * is not recognised while its owner has one alive.
 * Follow-ups (rekkas): a special with after set is only looked for while
 * its parent runs, before any other special, up to the end of its window,
 * and its motion must lie inside the parent's clock (ticks since it began,
 * frozen ones included). The [5]/[8]/[2]/[6]/[4] commands and "any" read
 * the direction held on the press. It starts on the first unfrozen tick
 * the parent shows an elapsed inside open..close (with a contact when the
 * move requires one), from the buffer like a cancel; a buffered follow-up
 * whose parent is no longer running is dropped. The AI takes a follow-up
 * at most once per move (how often depends on the difficulty).
 * Knockdowns: an unguarded hit of a knockdown move's last hit group (the
 * pack flags the sweep, most specials and some projectiles), when the
 * defender survives it, puts the defender down instead of in hitstun:
 * FALLING shows the fall animation (still sliding, and falling if airborne)
 * until it is over on the ground, then LYING shows the down pose for
 * wakeup - getup ticks and the get-up animation (wakeup and getup the
 * longer one in all); on the tick LYING has run that long the fighter acts
 * again. Nothing hits a downed fighter (projectiles pass through) except
 * OTG moves while it lies before the get-up; presses are buffered as usual,
 * so one in the last buffer ticks of the get-up is a reversal. The AI does
 * not attack a downed opponent, walks in and (above Easy) presses its
 * planned normal to land as the opponent rises. A KO plays the KO
 * animation instead.
 * Throws: forward or back (numpad 6 or 4, not diagonal) with HP or HK,
 * when the buffered stand HP/HK would start from neutral on the ground and
 * the opponent was within body + range pixels (body the larger push, range
 * the thrower's system) and throwable as the tick began, grabs instead on
 * that tick, before any strike lands (two grabs on one tick break each
 * other). Throwable: on the ground, not in a prejump, hitstun, blockstun,
 * a knockdown or a throw, and past its safe ticks (wakeup_safe after rising
 * from a knockdown, stun_safe after hitstun or blockstun ends; both from
 * the fighter's own system). The one held faces the thrower body pixels in
 * front of it; both show THROW/THROWN, nothing hits or pushes them, and
 * their elapsed counts together. HP or HK by the one held in the first tech
 * ticks after the grab breaks it: both recover tech_stun ticks in the guard
 * pose, sliding tech_push pixels apart. Otherwise, on the tick the
 * thrower's elapsed reaches its THROW total, the one held takes the
 * system's throw hit (class throw, unguardable, it knocks down; a back
 * throw first moves it behind the thrower and sends it the other way).
 * Run and backdash (the fighter's system's dash rule): on the ground and
 * free, 6 released and 6 again within window ticks runs, 4 likewise
 * backdashes. A run moves walk speed for run_startup ticks, then run speed
 * (walk times run_scale); once it has lasted run_startup + run_min ticks,
 * letting go of forward coasts run_grace ticks (forward again runs on) and
 * skids run_stop ticks at a falling speed. A buffered move or UP ends a
 * run at once (the move starts, the jump's prejump begins); a run never
 * guards or throws. A backdash waits back_startup ticks, flies back_air
 * ticks back back_distance pixels on a parabola back_height high (no
 * gravity) and lands for back_stop ticks; it cannot guard or be thrown,
 * strikes and projectiles miss it for its first back_invuln ticks, and a
 * hit ends it (an airborne one falls). The AI runs in from afar in some
 * plan windows and backdashes from some close swings it does not guard.
 * Cancels: a normal whose hit connected (hit or block) can be cancelled
 * into a special (cancel "special") or another normal (cancel "chain")
 * while its elapsed is inside startup + active; the buffered move starts on
 * the first unfrozen tick, so a cancel from hitstop starts on
 * C + hitstop + 1.
 *
 * Ladder: the select screen picks P1 (d-pad, any attack confirms; R
 * cycles the CPU level, shown top right, since the handheld has no core
 * option rows); each
 * stage is a match of ROUNDS_TO_WIN won rounds against a random character
 * not yet beaten in this run (a mirror with the alternate palette only for
 * a one-character roster), fought on that opponent's home stage, or on
 * pack stage (opponent index % stages) when it has none. Every ladder round
 * opens with a versus card of INTRO_TICKS ticks (stage number, both
 * fighters, names and stage name), then READY_TICKS of a big READY over the
 * stage; nothing moves through both and held keys are only recorded, so a
 * button held over them is no press. The round begins with FIGHT (shown
 * FIGHT_TICKS while the round runs) and ROUND_START (fight_start has
 * neither and sounds it at once). A loss
 * shows the result screen, beating everyone the clear screen; both return
 * to select. Auto skips select and rotates P1 through the roster each run.
 * START pauses a fight (the pause screen dims it under P1's command list);
 * on the select screen it toggles the highlighted character's command list
 * instead (fight_move_list builds both from the pack; the HUD font has
 * arrow glyphs for the motions). */
#include "fighter.h"
#include "libretro.h"
#include <stdlib.h>
#include <string.h>
#define KEY(k) (1u << RETRO_DEVICE_ID_JOYPAD_##k)
#define ATTACKS (KEY(A) | KEY(B) | KEY(X) | KEY(Y))
/* Normals by strength, strongest first; see libretro.c for the layout. */
static const uint32_t buttons[4] = {KEY(A), KEY(X), KEY(B), KEY(Y)};
static const int strength[4] = {STAND_HK, STAND_HP, STAND_LK, STAND_LP};
static const unsigned button_bit[4] = {BUTTON_HK, BUTTON_HP, BUTTON_LK, BUTTON_LP};
/* Projectiles vanish this far beyond the camera's screen edges. */
#define SHOT_MARGIN 64
/* KO, result and clear screens; Auto confirms them after AUTO_SCREEN. */
#define KO_TICKS 180
#define SCREEN_TICKS 180
#define AUTO_SCREEN 60
#define SCREEN_SKIP 20
/* The CPU per difficulty (Normal is the original AI): of 32 swings it
 * guards guard, at the wrong height wrong of those, once the swing (or a
 * projectile) has been out react ticks; an idle tick starts a special one
 * time in special and a tick in range presses its normal one time in press;
 * a follow-up comes after a contact one time in hit_follow and after a
 * whiff one time in whiff_follow (0 = never); a normal that connects is
 * cancelled into a motion special cancel times in 4; its hits deal damage
 * per mille (the ladder's CPU only). Of 8 plan windows run runs in from
 * afar, and of 32 close swings it does not guard backdash backdashes
 * away. */
static const struct level_ai {
	uint8_t guard, wrong, react, hit_follow, whiff_follow, cancel, tech, tech_at, grab, run,
	    backdash;
	uint16_t special, press, damage;
} levels[LEVELS] = {
    {14, 6, 4, 2, 4, 0, 6, 4, 1, 1, 2, 128, 16, 750},
    {22, 4, 0, 1, 2, 0, 12, 3, 2, 3, 4, 64, 8, 1000},
    {28, 1, 0, 1, 0, 3, 20, 2, 3, 5, 3, 24, 4, 1000},
};
static int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static uint32_t random_next(struct fight_state *s)
{
	s->rng ^= s->rng << 13;
	s->rng ^= s->rng >> 17;
	s->rng ^= s->rng << 5;
	return s->rng;
}
/* Quarter-pixel position from pixels and back. */
#define QUARTER(px) ((px) * 4)
static int px(const struct fighter *p) { return p->x / 4; }
/* ---- sound events, world and camera ---- */
static void sound(struct fight_state *s, const struct assets *a, int id, int side, int move)
{
	if (s->sounds >= MAX_SOUND_EVENTS)
		return;
	unsigned c = s->chr[side] < a->characters ? s->chr[side] : 0;
	s->sound[s->sounds++] = (struct sound_event){(uint8_t)id, (uint8_t)side, (uint8_t)c,
						     (uint8_t)a->chr[c].system, (uint8_t)move};
}
static const struct system *arena_system(const struct fight_state *s, const struct assets *a)
{
	return &a->sys[a->stages && s->arena < a->stages ? a->stage[s->arena].system : 0];
}
int fight_width(const struct fight_state *s, const struct assets *a)
{
	return a->stages && s->arena < a->stages ? a->stage[s->arena].width : FW;
}
void fight_walls(const struct fight_state *s, const struct assets *a, int *left, int *right)
{
	const struct system *sy = arena_system(s, a);
	int edge = (FW - sy->play_width) / 2;
	*left = (int)s->cam + edge + sy->margin;
	*right = (int)s->cam + FW - edge - sy->margin;
}
static int wall(const struct fight_state *s, const struct assets *a, int x)
{
	int lo, hi;
	fight_walls(s, a, &lo, &hi);
	return clamp(x, QUARTER(lo), QUARTER(hi));
}
/* End of a tick: the camera follows the midpoint, then the walls hold. */
static void camera(struct fight_state *s, const struct assets *a)
{
	int mid = (s->p[0].x + s->p[1].x) / 8;
	s->cam = (uint32_t)clamp(mid - FW / 2, 0, fight_width(s, a) - FW);
	for (int i = 0; i < 2; i++)
		s->p[i].x = wall(s, a, s->p[i].x);
}
/* Starting positions; life is only set for a fight (fight_start). */
static void place(struct fight_state *s)
{
	memset(s->p, 0, sizeof(s->p));
	memset(s->shot, 0, sizeof(s->shot));
	memset(s->spark, 0, sizeof(s->spark));
	s->p[0].x = QUARTER(140);
	s->p[1].x = QUARTER(340);
	s->p[0].face = 1;
	s->p[1].face = -1;
	s->ended = s->cam = s->arena = s->shake = s->flash = 0;
	for (int i = 0; i < 2; i++)
		s->combo[i] = s->shown[i] = s->show[i] = s->best[i] = s->damage[i] = s->last[i] = 0;
}
void fight_reset(struct fight_state *s, int automatic)
{
	memset(s, 0, sizeof(*s));
	s->rng = 0x351f123u;
	s->automatic = (uint32_t)automatic;
	s->level = LEVEL_NORMAL;
	s->mode = MODE_SELECT;
	s->pal[1] = 1;
	place(s);
}
/* A round between c0 and c1; with intro it opens with the versus card and
 * READY and ROUND_START sounds at FIGHT, otherwise at once (no banner). */
static void begin_round(struct fight_state *s, const struct assets *a, unsigned c0, unsigned c1,
			int intro)
{
	place(s);
	const struct character *opponent = &a->chr[c1 < a->characters ? c1 : 0];
	if (a->stages)
		s->arena = opponent->home ? opponent->home - 1u : (c1 % a->stages);
	int centre = fight_width(s, a) / 2;
	s->p[0].x = QUARTER(centre - 100);
	s->p[1].x = QUARTER(centre + 100);
	s->cam = (uint32_t)(centre - FW / 2);
	s->p[0].hp = a->chr[c0 < a->characters ? c0 : 0].stats.life;
	s->p[1].hp = opponent->stats.life;
	s->mode = MODE_FIGHT;
	s->timer = intro ? 0 : BANNER_END;
	s->chr[0] = c0;
	s->chr[1] = c1;
	s->pal[0] = 0;
	s->pal[1] = c0 == c1;
	if (!intro)
		sound(s, a, SOUND_ROUND_START, 0, 0);
}
void fight_start(struct fight_state *s, const struct assets *a, unsigned c0, unsigned c1)
{
	begin_round(s, a, c0, c1, 0);
}
static int grounded(const struct fighter *p) { return !p->y && !p->vy; }
static uint32_t back_key(const struct fighter *p)
{
	return p->face > 0 ? KEY(LEFT) : KEY(RIGHT);
}
/* ---- input history, charge and motions ---- */
static unsigned direction(uint32_t keys) { return (keys >> RETRO_DEVICE_ID_JOYPAD_UP) & 15u; }
static unsigned history_at(const struct fighter *p, unsigned i)
{
	return (p->history[i / 8] >> (4 * (i % 8))) & 15u;
}
/* Numpad direction of raw bits (1 up, 2 down, 4 left, 8 right) for a facing. */
static int numpad(unsigned d, int face)
{
	int up = d & 1, down = (d >> 1) & 1, fwd = face > 0 ? (d >> 3) & 1 : (d >> 2) & 1,
	    back = face > 0 ? (d >> 2) & 1 : (d >> 3) & 1;
	if (up && down)
		up = down = 0;
	if (fwd && back)
		fwd = back = 0;
	return 5 + 3 * up - 3 * down + fwd - back;
}
/* Charge counters: byte 0 back held, 1 back grace, 2 down held, 3 down grace. */
static unsigned charge_byte(uint32_t c, int k) { return (c >> (8 * k)) & 255u; }
static uint32_t charge_update(uint32_t c, const struct stats *st, int which, int held)
{
	unsigned t = charge_byte(c, 2 * which), grace = charge_byte(c, 2 * which + 1);
	if (held) {
		t = t < 255 ? t + 1 : 255;
		grace = 0;
	} else {
		grace = t >= st->charge ? st->grace : grace ? grace - 1 : 0;
		t = 0;
	}
	c &= ~(0xffffu << (16 * which));
	return c | (t | grace << 8) << (16 * which);
}
static void record(struct fighter *p, const struct stats *st, uint32_t keys)
{
	unsigned d = direction(keys);
	for (int w = HISTORY / 8 - 1; w > 0; w--)
		p->history[w] = p->history[w] << 4 | p->history[w - 1] >> 28;
	p->history[0] = p->history[0] << 4 | d;
	int n = numpad(d, p->face);
	p->charge = charge_update(p->charge, st, 0, n == 1 || n == 4 || n == 7);
	p->charge = charge_update(p->charge, st, 1, n == 1 || n == 2 || n == 3);
}
static int charged(uint32_t c, const struct stats *st, int which)
{
	return charge_byte(c, 2 * which) >= st->charge || charge_byte(c, 2 * which + 1);
}
/* Key directions of each motion, matched in order (in-between directions
 * are allowed); charges and held directions have none. */
static const int motion_keys[MOTIONS][3] = {
	[MOTION_236] = {2, 3, 6},   [MOTION_214] = {2, 1, 4},   [MOTION_623] = {6, 2, 3},
	[MOTION_41236] = {4, 2, 6}, [MOTION_63214] = {6, 2, 4}, [MOTION_421] = {4, 2, 1}};
/* Motion m in the last limit ticks of history at most (a follow-up's
 * motion must come after its parent began); the held-direction commands
 * look at this tick's direction only. */
static int motion_in(const struct fighter *p, const struct stats *st, int motion, unsigned limit)
{
	if (motion >= MOTION_ANY && motion <= HOLD_4) {
		int n = numpad(history_at(p, 0), p->face);
		switch (motion) {
		case HOLD_5:
			return n == 5;
		case HOLD_8:
			return n >= 7;
		case HOLD_2:
			return n <= 3;
		case HOLD_6:
			return n % 3 == 0;
		case HOLD_4:
			return n % 3 == 1;
		default:
			return 1;
		}
	}
	if (motion == CHARGE_46 || motion == CHARGE_28) {
		if (!charged(p->charge, st, motion == CHARGE_28))
			return 0;
		for (unsigned i = 0; i < st->release && i < limit; i++) {
			int n = numpad(history_at(p, i), p->face);
			if (motion == CHARGE_46 ? n % 3 == 0 : n >= 7)
				return 1;
		}
		return 0;
	}
	if (motion < 0 || motion >= MOTIONS)
		return 0;
	unsigned window = motion <= MOTION_214 ? st->quarter : st->dragon;
	int k = 2;
	for (unsigned i = 0; i < window && i < limit && k >= 0; i++)
		if (numpad(history_at(p, i), p->face) == motion_keys[motion][k])
			k--;
	return k < 0;
}
int fight_motion(const struct fighter *p, const struct stats *st, int motion)
{
	return motion_in(p, st, motion, HISTORY);
}
/* ---- poses ---- */
/* Step index for an animation after t ticks; looping wraps, others hold.
 * start receives the tick the step began on. */
static unsigned step_at(const struct assets *a, const struct anim *an, uint32_t t, int loop,
			uint32_t *start)
{
	t = loop ? t % an->total : t >= an->total ? an->total - 1u : t;
	uint32_t at = 0;
	for (unsigned i = 0; i < an->count; i++) {
		const struct step *st = &a->step[an->first + i];
		if (t < at + st->ticks) {
			if (start)
				*start = at;
			return an->first + i;
		}
		at += st->ticks;
	}
	if (start)
		*start = at - a->step[an->first + an->count - 1u].ticks;
	return an->first + an->count - 1u;
}
static const struct character *who(const struct fight_state *s, const struct assets *a, int i)
{
	return &a->chr[s->chr[i] < a->characters ? s->chr[i] : 0];
}
/* A knockdown after landing: lying for lie_ticks, then the get-up
 * animation; the fighter acts again once LYING has run down_ticks. */
static int lie_ticks(const struct character *c)
{
	int g = c->action[GETUP].total;
	return c->stats.wakeup > g ? c->stats.wakeup - g : 0;
}
static int down_ticks(const struct character *c)
{
	return lie_ticks(c) + c->action[GETUP].total;
}
/* Lying on the ground, before the get-up starts (only OTG moves hit). */
static int lying(const struct fighter *p, const struct character *c)
{
	return p->down == LYING && p->elapsed < lie_ticks(c);
}
/* ---- normal throws ---- */
static const struct throw_rule *rule(const struct fight_state *s, const struct assets *a, int i)
{
	return &a->sys[who(s, a, i)->system].throw;
}
static int throwing(const struct fighter *p)
{
	return p->grab == GRAB_FORWARD || p->grab == GRAB_BACK;
}
/* Can q be thrown: standing or crouching on the ground (not in a prejump
 * or a backdash), out of hitstun, blockstun, knockdowns and throws, and
 * past its safe ticks. */
static int throwable(const struct fighter *q)
{
	return q->hp && grounded(q) && !q->squat && !q->stun && !q->down && !q->grab && !q->safe &&
	       q->dash != DASH_BACK;
}
/* Body distance: the larger push of the two fighters. */
static int body(const struct fight_state *s, const struct assets *a)
{
	int p0 = who(s, a, 0)->stats.push, p1 = who(s, a, 1)->stats.push;
	return p0 > p1 ? p0 : p1;
}
/* Is fighter i's opponent within its throw range and throwable? */
static int in_throw_range(const struct fight_state *s, const struct assets *a, int i)
{
	const struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	return p->hp && throwable(q) && abs(px(q) - px(p)) <= body(s, a) + rule(s, a, i)->range;
}
/* ---- run and backdash ---- */
static const struct dash_rule *dash_rule(const struct fight_state *s, const struct assets *a,
					 int i)
{
	return &a->sys[who(s, a, i)->system].dash;
}
static int back_total(const struct dash_rule *d)
{
	return d->back_startup + d->back_air + d->back_stop;
}
/* A backdash's height after its tick e: a parabola over the airborne
 * ticks, back on the ground on the last of them. */
static int back_y(const struct dash_rule *d, int e)
{
	int t = e - d->back_startup + 1, n = d->back_air;
	return t <= 0 || t >= n ? 0 : -(4 * d->back_height * t * (n - t)) / (n * n);
}
/* Strikes cannot touch fighter i in the first back_invuln ticks of a
 * backdash. */
static int evading(const struct fight_state *s, const struct assets *a, int i)
{
	const struct fighter *q = &s->p[i];
	return q->dash == DASH_BACK && q->elapsed < dash_rule(s, a, i)->back_invuln;
}
/* A tap of numpad direction n (6 or 4) now: n this tick, the tick before
 * without that direction, and n again before it within window ticks
 * (the in-between ticks without it too). */
static int tapped(const struct fighter *p, int n, unsigned window)
{
	if (numpad(history_at(p, 0), p->face) != n)
		return 0;
	for (unsigned k = 1; k <= window && k < HISTORY; k++) {
		int m = numpad(history_at(p, k), p->face);
		if (m % 3 == n % 3)
			return k >= 2 && m == n;
	}
	return 0;
}
unsigned fight_pose(const struct fight_state *s, const struct assets *a, int i)
{
	const struct fighter *p = &s->p[i];
	const struct character *c = who(s, a, i);
	if (!p->hp)
		return step_at(a, &c->action[KO], s->ended, 0, NULL);
	if (p->down == FALLING)
		return step_at(a, &c->action[FALL], (uint32_t)p->elapsed, 0, NULL);
	if (p->down == LYING)
		return lying(p, c) ? step_at(a, &c->action[DOWN], (uint32_t)p->elapsed, 1, NULL)
				   : step_at(a, &c->action[GETUP],
					     (uint32_t)(p->elapsed - lie_ticks(c)), 0, NULL);
	if (p->grab)
		return step_at(a, &c->action[throwing(p) ? THROW : THROWN], (uint32_t)p->elapsed, 0,
			       NULL);
	if (p->move)
		return step_at(a, &c->move[p->move - 1].anim, (uint32_t)p->elapsed, 0, NULL);
	int low = p->crouch && grounded(p);
	if (p->stun)
		return step_at(a,
			       &c->action[p->block ? (low ? CROUCH_GUARD : GUARD)
						   : (low ? CROUCH_HURT : HURT)],
			       (uint32_t)p->elapsed, 0, NULL);
	if (p->dash == DASH_BACK)
		return step_at(a, &c->action[BACKDASH], (uint32_t)p->elapsed, 0, NULL);
	if (p->dash)
		return step_at(a, &c->action[RUN], s->tick, 1, NULL);
	if (p->squat)
		return step_at(a, &c->action[JUMP], 0, 0, NULL);
	if (!grounded(p))
		return step_at(a, &c->action[JUMP],
			       (uint32_t)(p->vy - c->stats.jump.vy) / (uint32_t)c->stats.jump.gravity,
			       0, NULL);
	if (p->block)
		return step_at(a, &c->action[low ? CROUCH_GUARD : GUARD],
			       (uint32_t)p->elapsed, 0, NULL);
	if (p->crouch)
		return step_at(a, &c->action[CROUCH], s->tick, 1, NULL);
	if (p->walk)
		return step_at(a, &c->action[p->walk > 0 ? WALK_FORWARD : WALK_BACK],
			       s->tick, 1, NULL);
	return step_at(a, &c->action[IDLE], s->tick + (unsigned)i * 17, 1, NULL);
}
/* Height of a move's motion after showing elapsed 0..t. */
static int height(const struct assets *a, const struct move *m, uint32_t t)
{
	int y = 0;
	uint32_t at = 0;
	for (unsigned i = 0; i < m->anim.count && at <= t; i++) {
		const struct step *st = &a->step[m->anim.first + i];
		uint32_t n = t + 1 - at < st->ticks ? t + 1 - at : st->ticks;
		y += st->vy * (int)n;
		at += st->ticks;
	}
	return y;
}
struct area fight_box(const struct box *b, int x, int y, int face)
{
	return (struct area){face > 0 ? x + b->x : x - b->x - b->w, GROUND + y + b->y,
			     b->w, b->h};
}
static int overlap(struct area p, struct area q)
{
	return p.x < q.x + q.w && q.x < p.x + p.w && p.y < q.y + q.h && q.y < p.y + p.h;
}
/* Centre of the overlap of two overlapping boxes: the contact point. */
static void middle(struct area p, struct area q, int *x, int *y)
{
	int x0 = p.x > q.x ? p.x : q.x, x1 = p.x + p.w < q.x + q.w ? p.x + p.w : q.x + q.w,
	    y0 = p.y > q.y ? p.y : q.y, y1 = p.y + p.h < q.y + q.h ? p.y + p.h : q.y + q.h;
	*x = (x0 + x1) / 2;
	*y = (y0 + y1) / 2;
}
static const struct projectile *shot_type(const struct fight_state *s, const struct assets *a,
					  const struct shot *sh)
{
	return &who(s, a, sh->owner)->projectile[sh->kind - 1];
}
static unsigned shot_step(const struct fight_state *s, const struct assets *a,
			  const struct shot *sh)
{
	return step_at(a, &shot_type(s, a, sh)->anim, (uint32_t)sh->age, 1, NULL);
}
static int owns_shot(const struct fight_state *s, int i)
{
	for (int k = 0; k < MAX_PROJECTILES; k++)
		if (s->shot[k].kind && s->shot[k].owner == i)
			return 1;
	return 0;
}
/* ---- AI ---- */
static uint32_t button_key(unsigned bits)
{
	for (int k = 3; k >= 0; k--)
		if (bits & button_bit[k])
			return buttons[k];
	return 0;
}
/* The AI's input for tick t of a special's motion, or 0 when it is done:
 * each key direction of a motion for two ticks, the button with the last;
 * charges hold for charge + 2 ticks. */
static uint32_t macro(int motion, uint32_t charge, uint32_t t, int face, uint32_t button)
{
	uint32_t fwd = face > 0 ? KEY(RIGHT) : KEY(LEFT), back = face > 0 ? KEY(LEFT) : KEY(RIGHT),
		 down = KEY(DOWN);
	if (motion == CHARGE_46 || motion == CHARGE_28) {
		uint32_t hold = motion == CHARGE_46 ? back : down,
			 release = motion == CHARGE_46 ? fwd : KEY(UP);
		return t < charge + 2 ? hold : t == charge + 2 ? release | button : 0;
	}
	if (motion >= MOTION_ANY && motion <= HOLD_4) {
		static const int held[] = {5, 5, 8, 2, 6, 4};
		int n = held[motion - MOTION_ANY];
		uint32_t key = n == 8 ? KEY(UP) : n == 2 ? down : n == 6 ? fwd : n == 4 ? back : 0;
		return t ? 0 : key | button;
	}
	if (t >= 5)
		return 0;
	int n = motion_keys[motion][t / 2];
	uint32_t key = (n <= 3 ? down : 0) | (n % 3 == 0 ? fwd : n % 3 == 1 ? back : 0);
	return t == 4 ? key | button : key;
}
/* The AI's half of a 66 or 44: key now unless the last tick held it, so
 * held keys turn into taps. */
static uint32_t tap(const struct fighter *p, uint32_t key)
{
	return history_at(p, 0) & (key >> RETRO_DEVICE_ID_JOYPAD_UP) ? 0 : key;
}
static uint32_t ai(struct fight_state *s, const struct assets *a, int i)
{
	struct fighter *p = &s->p[i];
	const struct fighter *q = &s->p[1 - i];
	const struct character *c = who(s, a, i), *oc = who(s, a, 1 - i);
	int d = px(q) - px(p), dist = abs(d), range = c->stats.guard_range;
	uint32_t toward = d > 0 ? KEY(RIGHT) : KEY(LEFT),
		 away = d > 0 ? KEY(LEFT) : KEY(RIGHT);
	uint32_t r = random_next(s);
	const struct level_ai *lv = &levels[s->level];
	if (p->stun || p->down || p->grab || !q->hp)
		p->plan = p->plan_tick = 0;
	if (p->down)
		return 0;
	/* Held by a throw: break it lv->tech times in 32 (decided once per
	 * throw), pressing HP lv->tech_at ticks after the grab. */
	if (p->grab) {
		uint32_t h = ((s->tick - (uint32_t)p->elapsed) * 2654435761u + (unsigned)i) >> 27;
		return p->grab == GRAB_HELD && h < lv->tech && p->elapsed == lv->tech_at &&
				       lv->tech_at < rule(s, a, 1 - i)->tech
			   ? KEY(X)
			   : 0;
	}
	/* Ticks until a knocked-down opponent acts again (-1: it is up). */
	int waking = q->down == LYING ? down_ticks(oc) - q->elapsed : q->down ? 255 : -1;
	if (p->plan) {
		const struct move *m = &c->move[p->plan - 1];
		uint32_t key = macro(m->motion, c->stats.charge, (uint32_t)p->plan_tick, p->face,
				     button_key(m->buttons));
		if (++p->plan_tick > c->stats.charge + 3 || !key)
			p->plan = p->plan_tick = 0;
		if (key)
			return key;
	}
	/* Guard lv->guard swings of 32 (Normal about two in three), at the
	 * wrong height lv->wrong of 32 (Normal one time in five), once the
	 * swing has been out lv->react ticks; the choice is fixed for the whole
	 * swing. Projectiles in range are guarded the same way. */
	if (q->move && dist < range && q->elapsed >= lv->react) {
		const struct move *m = &oc->move[q->move - 1];
		uint32_t h = ((s->tick - (uint32_t)q->elapsed) * 2654435761u + (unsigned)i) >> 27;
		if (h < lv->guard) {
			int low = m->hit.guard == GUARD_LOW;
			if (h < lv->wrong)
				low = !low;
			return away | (low ? KEY(DOWN) : 0u);
		}
		/* Some swings it does not guard, up close, it backdashes from. */
		if (h - lv->guard < lv->backdash && dist < 120 && !p->move && !p->stun &&
		    grounded(p) && !p->dash)
			return tap(p, away);
	}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		if (sh->kind && sh->owner != i && abs(sh->x - px(p)) < range &&
		    (px(p) - sh->x) * sh->face > 0 && sh->age >= lv->react &&
		    (((s->tick - (uint32_t)sh->age) * 2654435761u + (unsigned)i) >> 27) < lv->guard)
			return away;
	}
	/* In a move with follow-ups: once per move, go on after a hit or a
	 * guard one time in lv->hit_follow (Normal always), after a whiff one
	 * time in lv->whiff_follow (Normal every other time, Hard never; never
	 * without the contact a follow-up needs), timed so the press lands in
	 * its window. */
	if (p->move && !p->plan && !p->buffer) {
		unsigned parent = (unsigned)p->move, options = 0, pick = 0;
		uint32_t h = (s->tick - (uint32_t)p->clock) * 2654435761u + (unsigned)i * 977u;
		for (unsigned k = MOVES; k < c->moves; k++) {
			const struct move *m = &c->move[k];
			if (m->after != parent || (m->contact && !p->hit))
				continue;
			unsigned lead = m->motion >= MOTION_ANY && m->motion <= HOLD_4 ? 0 : 4;
			int from = (int)m->open - (int)lead, to = (int)m->close - (int)lead;
			if (p->elapsed >= from && p->elapsed <= to && h % (options + 1) == 0)
				pick = k;
			options += p->elapsed >= from && p->elapsed <= to;
		}
		uint32_t roll = h >> 16;
		if (options && (p->hit ? roll % lv->hit_follow == 0
				       : lv->whiff_follow && (roll & 1) &&
					     (roll >> 1) % (lv->whiff_follow / 2u) == 0)) {
			const struct move *m = &c->move[pick];
			p->plan = (int32_t)pick + 1;
			p->plan_tick = 1;
			return macro(m->motion, c->stats.charge, 0, p->face, button_key(m->buttons));
		}
	}
	/* Hard: a connected normal that allows it is cancelled into a motion
	 * special lv->cancel times in 4, decided once per move and entered
	 * while the hitstop holds both fighters (the press is buffered). */
	if (lv->cancel && p->move && p->move <= MOVES && p->hit && p->freeze && !p->plan &&
	    !p->buffer) {
		const struct move *n = &c->move[p->move - 1];
		uint32_t h = ((s->tick - (uint32_t)p->clock) * 2654435761u + (unsigned)i * 977u) >> 20;
		unsigned pick[MAX_SPECIALS], count = 0;
		for (unsigned k = MOVES; k < c->moves; k++) {
			const struct move *m = &c->move[k];
			if (!m->after && m->motion < CHARGE_46 && !(m->spawn && owns_shot(s, i)))
				pick[count++] = k;
		}
		if (count && (n->cancel & CANCEL_SPECIAL) && p->elapsed < (int)n->startup + n->active &&
		    h % 4 < lv->cancel) {
			unsigned k = pick[(h >> 2) % count];
			p->plan = (int32_t)k + 1;
			p->plan_tick = 1;
			return macro(c->move[k].motion, c->stats.charge, 0, p->face,
				     button_key(c->move[k].buttons));
		}
	}
	/* Now and then a special that starts on its own: the projectile from
	 * afar, else any. */
	unsigned specials = 0, own[MAX_SPECIALS];
	for (unsigned k = MOVES; k < c->moves; k++)
		if (!c->move[k].after)
			own[specials++] = k;
	if (specials && !p->move && !p->stun && grounded(p) && waking < 0 &&
	    (r >> 8) % lv->special == 0) {
		unsigned k = own[(r >> 16) % specials];
		for (unsigned j = 0; dist > 180 && j < specials; j++)
			if (c->move[own[j]].spawn)
				k = own[j];
		if (!(c->move[k].spawn && owns_shot(s, i))) {
			p->plan = (int32_t)k + 1;
			p->plan_tick = 1;
			return macro(c->move[k].motion, c->stats.charge, 0, p->face,
				     button_key(c->move[k].buttons));
		}
	}
	/* Each 29-tick window plans one normal: walk in until its hitbox can
	 * reach the opponent's body, then press it once. A knocked-down
	 * opponent cannot be hit: walk in and wait, and above Easy press so
	 * the hit lands as it gets up (a meaty). lv->grab windows in 8 (with
	 * the opponent up) plan a throw instead: walk into throw range and
	 * throw forward, or now and then back, with HP or HK. */
	unsigned window = (s->tick + (unsigned)i * 17) / 29, plan = window * 2654435761u >> 24;
	int button = (int)(plan & 3), low = (int)(plan >> 2) & 1;
	const struct move *m = &c->move[strength[button] + (low ? CROUCH_LP : 0)];
	/* lv->run windows in 8 run in (66, held until the target is near). */
	int grab = waking < 0 && ((plan >> 3) & 7) < lv->grab,
	    near = grab ? body(s, a) + rule(s, a, i)->range : m->reach + 20,
	    run = ((window * 2246822519u) >> 29) < lv->run;
	if (p->dash == DASH_RUN || p->dash == DASH_COAST)
		return dist > near + 40 ? toward : 0;
	if (run && dist > 180 && !p->move && !p->stun && !p->dash && grounded(p))
		return tap(p, toward);
	if (grab) {
		if (!p->move && !p->stun && grounded(p) && in_throw_range(s, a, i))
			return (plan & 0x40 && plan & 0x80 ? away : toward) |
			       (plan & 1 ? KEY(A) : KEY(X));
		return dist > near ? toward : 0;
	}
	uint32_t key = dist > near ? toward : 0;
	int press = waking < 0 ? r % lv->press == 0
			       : s->level != LEVEL_EASY && waking <= m->startup + 1;
	if (!key && !p->move && !p->stun && press)
		key = buttons[button] | (low ? KEY(DOWN) : 0u);
	if ((r & 255) == 0)
		key |= KEY(UP);
	return key;
}
/* ---- fighter update ---- */
/* One airborne tick: y moves vy / 4 pixels, then gravity; landing stops
 * (and sounds). */
static void fall(struct fight_state *s, const struct assets *a, int i)
{
	struct fighter *p = &s->p[i];
	p->y += p->vy / 4;
	p->vy += who(s, a, i)->stats.jump.gravity;
	if (p->y >= 0) {
		p->y = p->vy = 0;
		sound(s, a, SOUND_LAND, i, 0);
	}
}
static void age(struct fighter *p)
{
	if (p->elapsed < 255)
		p->elapsed++;
}
/* Pushback: ceil(rest / 4) pixels this tick; with corner set a wall passes
 * the remainder to the other fighter (the attacker) the opposite way. */
static void slide(const struct fight_state *s, const struct assets *a, struct fighter *p,
		  struct fighter *q)
{
	int dir = p->slide > 0 ? 1 : -1, d = (abs(p->slide) + 3) / 4;
	p->slide -= dir * d;
	int x = wall(s, a, p->x + dir * QUARTER(d)), rest = QUARTER(d) - abs(x - p->x);
	p->x = x;
	if (rest && p->corner)
		q->x = wall(s, a, q->x - dir * rest);
	if (!p->slide)
		p->corner = 0;
}
/* One unfrozen tick of a knockdown: the slide and the fall go on, FALLING
 * turns LYING on the tick the fall animation is over on the ground, and
 * LYING ends down_ticks later. Returns 1 while the fighter is still down
 * (0: it acts this tick). */
static int knocked(struct fight_state *s, const struct assets *a, int i)
{
	struct fighter *p = &s->p[i];
	const struct character *c = who(s, a, i);
	if (p->slide)
		slide(s, a, p, &s->p[1 - i]);
	if (!grounded(p))
		fall(s, a, i);
	age(p);
	if (p->down == FALLING && grounded(p) && p->elapsed >= c->action[FALL].total) {
		p->down = LYING;
		p->elapsed = 0;
	} else if (p->down == LYING && p->elapsed >= down_ticks(c)) {
		p->down = UP;
		p->elapsed = 0;
		return 0;
	}
	return 1;
}
/* The move a press of edge buttons (with keys held) asks for, or -1. */
static int resolve(const struct fight_state *s, const struct assets *a, int i, uint32_t edge,
		   uint32_t keys)
{
	const struct character *c = who(s, a, i);
	unsigned pressed = 0;
	for (int k = 0; k < 4; k++)
		if (edge & buttons[k])
			pressed |= button_bit[k];
	if (!pressed)
		return -1;
	const struct fighter *p = &s->p[i];
	/* The current move's follow-ups first, up to the end of their window,
	 * with a motion entered after the move began. */
	for (unsigned k = MOVES; p->move && k < c->moves; k++) {
		const struct move *m = &c->move[k];
		if (m->after == (unsigned)p->move && p->elapsed <= m->close && (m->buttons & pressed) &&
		    motion_in(p, &c->stats, m->motion, (unsigned)p->clock) &&
		    !(m->spawn && owns_shot(s, i)))
			return (int)k;
	}
	for (unsigned k = MOVES; k < c->moves; k++) {
		const struct move *m = &c->move[k];
		if (!m->after && (m->buttons & pressed) && fight_motion(p, &c->stats, m->motion) &&
		    !(m->spawn && owns_shot(s, i)))
			return (int)k;
	}
	for (int k = 0; k < 4; k++)
		if (edge & buttons[k])
			return strength[k] + (keys & KEY(DOWN) ? CROUCH_LP : 0);
	return -1;
}
/* Strength of a hit (0 light, 1 heavy, 2 special) from its class, else
 * from move index k (-1 for a projectile). */
static int tier(const struct hit *h, int k)
{
	switch (h->cls) {
	case CLASS_LIGHT:
		return 0;
	case CLASS_HEAVY:
	case CLASS_COMMAND:
	case CLASS_THROW:
		return 1;
	case CLASS_SPECIAL:
	case CLASS_SUPER:
		return 2;
	default:
		return k < 0 || k >= MOVES ? 2 : k % 2;
	}
}
static void start(struct fight_state *s, const struct assets *a, int i, int k)
{
	const struct character *c = who(s, a, i);
	struct fighter *p = &s->p[i];
	p->move = k + 1;
	p->elapsed = p->hit = p->block = p->squat = p->walk = p->clock = p->dash = 0;
	p->buffer = p->age = 0;
	p->crouch = k >= CROUCH_LP && k <= CROUCH_HK;
	if (c->move[k].special)
		s->specials++;
	sound(s, a, SOUND_WHOOSH_LIGHT + tier(&c->move[k].hit, k), i, k + 1);
}
/* Can the current move be cancelled into move k now? A follow-up: its
 * parent has shown an elapsed in its window (and connected if it must);
 * otherwise the current move is a connected normal that allows it. */
static int cancellable(const struct character *c, const struct fighter *p, int k)
{
	const struct move *m = &c->move[p->move - 1], *f = &c->move[k];
	if (f->after)
		return f->after == (unsigned)p->move && p->elapsed >= f->open &&
		       p->elapsed <= f->close && (!f->contact || p->hit);
	unsigned need = c->move[k].special ? CANCEL_SPECIAL : CANCEL_CHAIN;
	return p->hit && !m->special && (m->cancel & need) &&
	       p->elapsed < (int)m->startup + m->active;
}
static void launch(struct fight_state *s, const struct assets *a, int i, unsigned kind)
{
	const struct fighter *p = &s->p[i];
	const struct projectile *t = &who(s, a, i)->projectile[kind - 1];
	for (int k = 0; k < MAX_PROJECTILES; k++)
		if (!s->shot[k].kind) {
			s->shot[k] = (struct shot){(int32_t)kind, i, px(p) + p->face * t->x,
						   p->y + t->y, p->face, 0};
			sound(s, a, SOUND_LAUNCH, i, p->move);
			return;
		}
}
/* A normal throw begins (forward or back): it takes the opponent once
 * combat() has updated both fighters. */
static void begin_throw(struct fight_state *s, const struct assets *a, int i, int back)
{
	struct fighter *p = &s->p[i];
	p->grab = back ? GRAB_BACK : GRAB_FORWARD;
	p->elapsed = p->hit = p->block = p->squat = p->walk = p->crouch = p->clock = p->dash = 0;
	p->buffer = p->age = 0;
	sound(s, a, SOUND_WHOOSH_HEAVY, i, 0);
}
/* Backdash motion of its tick elapsed: back the next share of
 * back_distance on each airborne tick, up and down the arc; JUMP on the
 * first of them, LAND on the last. */
static void back_move(struct fight_state *s, const struct assets *a, int i)
{
	struct fighter *p = &s->p[i];
	const struct dash_rule *d = dash_rule(s, a, i);
	int j = p->elapsed - d->back_startup, n = d->back_air, far = QUARTER(d->back_distance);
	if (j < 0 || j >= n)
		return;
	p->x -= p->face * (far * (j + 1) / n - far * j / n);
	p->y = back_y(d, p->elapsed);
	if (!j)
		sound(s, a, SOUND_JUMP, i, 0);
	if (j == n - 1)
		sound(s, a, SOUND_LAND, i, 0);
}
/* One tick of a run or backdash (the tick it began did its own). Returns 1
 * while it holds the fighter, 0 once it is over or a buffered move or UP
 * cancels a run (the caller then acts as usual; a run never throws). */
static int dashing(struct fight_state *s, const struct assets *a, int i, uint32_t keys)
{
	struct fighter *p = &s->p[i];
	const struct dash_rule *d = dash_rule(s, a, i);
	const struct stats *st = &who(s, a, i)->stats;
	age(p);
	if (p->dash == DASH_BACK) {
		if (p->elapsed >= back_total(d)) {
			p->dash = p->elapsed = 0;
			return 0;
		}
		back_move(s, a, i);
		return 1;
	}
	if (p->dash != DASH_STOP && (p->buffer || (keys & KEY(UP)))) {
		if (keys & KEY(UP))
			p->dash = p->elapsed = 0;
		return 0;
	}
	int fwd = numpad(direction(keys), p->face) % 3 == 0;
	if (p->dash == DASH_RUN && !fwd && p->elapsed >= d->run_startup + d->run_min) {
		p->dash = d->run_grace ? DASH_COAST : DASH_STOP;
		p->elapsed = 0;
	} else if (p->dash == DASH_COAST && fwd) {
		p->dash = DASH_RUN;
		p->elapsed = d->run_startup + d->run_min;
	} else if (p->dash == DASH_COAST && p->elapsed >= d->run_grace) {
		p->dash = DASH_STOP;
		p->elapsed = 0;
	}
	if (p->dash == DASH_STOP && p->elapsed >= d->run_stop) {
		p->dash = p->elapsed = 0;
		return 0;
	}
	int v = p->dash == DASH_STOP ? st->run * (d->run_stop - p->elapsed) / (d->run_stop + 1)
		: p->dash == DASH_RUN && p->elapsed < d->run_startup ? st->walk
								     : st->run;
	p->x += p->face * v;
	return 1;
}
/* One fighter's tick before contacts; returns 1 if it was frozen. reach:
 * the opponent was in throw range and throwable as the tick began. */
static int update(struct fight_state *s, const struct assets *a, int i, uint32_t keys,
		  int threat, int reach)
{
	struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	const struct character *c = who(s, a, i);
	uint32_t edge = keys & ~(uint32_t)p->keys;
	p->keys = (int32_t)(keys & 0xffffu);
	record(p, &c->stats, keys);
	if (p->move && p->clock < 255)
		p->clock++;
	int k = edge & ATTACKS ? resolve(s, a, i, edge, keys) : -1;
	if (k >= 0 && !(p->buffer && c->move[p->buffer - 1].special && !c->move[k].special)) {
		p->buffer = k + 1;
		p->age = 0;
	}
	if (p->freeze) {
		p->freeze--;
		return 1;
	}
	if (p->safe)
		p->safe--;
	/* In a throw: the one held breaks it with HP or HK in the thrower's
	 * tech window (combat() resolves it); nothing is buffered. */
	if (p->grab) {
		if (p->grab == GRAB_HELD && (edge & (KEY(A) | KEY(X))) &&
		    p->elapsed < rule(s, a, 1 - i)->tech)
			p->grab = GRAB_TECH;
		p->buffer = p->age = 0;
		age(p);
		return 0;
	}
	/* Knocked down: presses stay buffered (a press in the last buffer
	 * ticks of the get-up comes out as the fighter rises, which starts
	 * its throw-safe ticks). */
	if (p->down) {
		if (knocked(s, a, i)) {
			if (p->buffer && ++p->age > c->stats.buffer)
				p->buffer = p->age = 0;
			p->x = wall(s, a, p->x);
			return 0;
		}
		p->safe = rule(s, a, i)->wakeup_safe;
	}
	if (p->move && p->buffer && cancellable(c, p, p->buffer - 1))
		start(s, a, i, p->buffer - 1);
	else if (p->move && ++p->elapsed >= c->move[p->move - 1].anim.total)
		p->move = p->elapsed = p->hit = 0;
	/* A follow-up only ever starts from its parent. */
	if (p->buffer && c->move[p->buffer - 1].after &&
	    c->move[p->buffer - 1].after != (unsigned)p->move)
		p->buffer = p->age = 0;
	if (p->stun) {
		age(p);
		if (!--p->stun) {
			p->block = p->elapsed = 0;
			p->safe = rule(s, a, i)->stun_safe;
		}
	}
	if (p->slide)
		slide(s, a, p, q);
	if (!grounded(p) && !p->move && p->dash != DASH_BACK)
		fall(s, a, i);
	p->walk = 0;
	if (!p->move && !p->stun && p->dash && dashing(s, a, i, keys)) {
		/* Running, skidding or backdashing: nothing else this tick. */
	} else if (!p->move && !p->stun) {
		int ground = grounded(p);
		if (ground)
			p->face = q->x >= p->x ? 1 : -1;
		p->crouch = ground && !p->squat && (keys & KEY(DOWN));
		/* Forward or back with HP or HK near a throwable opponent
		 * throws (on this first tick, never out of a run); otherwise it
		 * is the normal. 66 runs, 44 backdashes. */
		int n = numpad(direction(keys), p->face);
		const struct dash_rule *d = dash_rule(s, a, i);
		if (p->buffer && ground && reach && !p->dash && (n == 4 || n == 6) &&
		    (p->buffer - 1 == STAND_HP || p->buffer - 1 == STAND_HK)) {
			begin_throw(s, a, i, n == 4);
		} else if (p->buffer && ground) {
			start(s, a, i, p->buffer - 1);
		} else if (ground && !p->squat && tapped(p, 6, d->window)) {
			p->dash = DASH_RUN;
			p->elapsed = p->block = 0;
			s->runs++;
			p->x += p->face * c->stats.walk;
		} else if (ground && !p->squat && tapped(p, 4, d->window)) {
			p->dash = DASH_BACK;
			p->elapsed = p->block = 0;
			s->backdashes++;
			back_move(s, a, i);
		} else if (p->squat) {
			if (!--p->squat) {
				p->vy = c->stats.jump.vy;
				sound(s, a, SOUND_JUMP, i, 0);
			}
		} else if (ground && threat && (keys & back_key(p))) {
			if (p->block != 2)
				p->block = 2, p->elapsed = 0;
			else
				age(p);
		} else {
			if (p->block)
				p->block = p->elapsed = 0;
			if (!p->crouch) {
				/* Forward and back together cancel out. */
				const struct stats *st = &c->stats;
				int fwd = (keys & (p->face > 0 ? KEY(RIGHT) : KEY(LEFT))) != 0,
				    back = (keys & back_key(p)) != 0;
				int dx = fwd == back ? 0
					 : fwd	     ? (ground ? st->walk : st->jump.vx)
						     : -(ground ? st->walk_back : st->jump.vx_back);
				p->x += p->face * dx;
				p->walk = dx > 0 ? 1 : dx ? -1 : 0;
				if ((keys & KEY(UP)) && ground)
					p->squat = st->jump.prejump;
			}
		}
	}
	if (p->buffer && ++p->age > c->stats.buffer)
		p->buffer = p->age = 0;
	if (p->move) {
		uint32_t begin;
		const struct move *m = &c->move[p->move - 1];
		const struct step *st =
		    &a->step[step_at(a, &m->anim, (uint32_t)p->elapsed, 0, &begin)];
		int airborne = p->y < 0;
		p->x += p->face * QUARTER(st->vx);
		p->y += st->vy;
		if (airborne && !p->y)
			sound(s, a, SOUND_LAND, i, 0);
		if (st->spawn && begin == (uint32_t)p->elapsed)
			launch(s, a, i, st->spawn);
	}
	p->x = wall(s, a, p->x);
	return 0;
}
/* Does fighter i's current hitbox touch the other's hurtboxes? at gets the
 * contact point (world x, screen y). */
static int contact(const struct fight_state *s, const struct assets *a, int i, int at[2])
{
	const struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	if (!p->move || !q->hp || q->grab || evading(s, a, 1 - i))
		return 0;
	/* A fighter knocked down is out of reach, except lying for OTG moves. */
	if (q->down && !(lying(q, who(s, a, 1 - i)) &&
			 (who(s, a, i)->move[p->move - 1].hit.flags & FLAG_OTG)))
		return 0;
	const struct step *hit = &a->step[fight_pose(s, a, i)],
			  *body = &a->step[fight_pose(s, a, 1 - i)];
	if (!hit->group || hit->group == p->hit)
		return 0;
	for (unsigned h = 0; h < hit->hit; h++) {
		struct area x = fight_box(&a->box[hit->box + hit->hurt + h], px(p), p->y, p->face);
		for (unsigned k = 0; k < body->hurt; k++) {
			struct area y = fight_box(&a->box[body->box + k], px(q), q->y, q->face);
			if (overlap(x, y)) {
				middle(x, y, &at[0], &at[1]);
				return 1;
			}
		}
	}
	return 0;
}
static int shot_hits_body(const struct fight_state *s, const struct assets *a,
			  const struct shot *sh, int i, int at[2])
{
	const struct fighter *q = &s->p[i];
	if (q->down || q->grab || evading(s, a, i))
		return 0;
	const struct step *hit = &a->step[shot_step(s, a, sh)],
			  *body = &a->step[fight_pose(s, a, i)];
	for (unsigned h = 0; h < hit->hit; h++) {
		struct area x = fight_box(&a->box[hit->box + h], sh->x, sh->y, sh->face);
		for (unsigned k = 0; k < body->hurt; k++) {
			struct area y = fight_box(&a->box[body->box + k], px(q), q->y, q->face);
			if (overlap(x, y)) {
				middle(x, y, &at[0], &at[1]);
				return 1;
			}
		}
	}
	return 0;
}
static int shots_clash(const struct fight_state *s, const struct assets *a,
		       const struct shot *u, const struct shot *v)
{
	const struct step *x = &a->step[shot_step(s, a, u)], *y = &a->step[shot_step(s, a, v)];
	for (unsigned h = 0; h < x->hit; h++)
		for (unsigned k = 0; k < y->hit; k++)
			if (overlap(fight_box(&a->box[x->box + h], u->x, u->y, u->face),
				    fight_box(&a->box[y->box + k], v->x, v->y, v->face)))
				return 1;
	return 0;
}
/* Can the defender block this hit with these keys? */
static int blocks(const struct fighter *q, uint32_t keys, const struct hit *h)
{
	if (q->move || q->down || q->dash || (q->stun && q->block != 1) || !grounded(q) ||
	    !(keys & back_key(q)))
		return 0;
	int low = (keys & KEY(DOWN)) != 0;
	return h->guard == GUARD_HIGH || (h->guard == GUARD_LOW) == low;
}
/* v * factor / 1000 rounded half up; at least 1 when v is nonzero. */
static int permille(int v, int factor)
{
	int r = (v * factor + 500) / 1000;
	return v && !r ? 1 : r;
}
/* Damage, stun and pushback on fighter q (character qc); a strike also
 * freezes its attacker p (projectiles pass p = NULL). knock: an unguarded
 * knockdown hit that leaves q alive puts it down (FALLING) instead of in
 * hitstun. Practice never takes the last point of life. Returns the
 * damage dealt (before the life bar's floor). */
static int struck(struct fight_state *s, struct fighter *p, struct fighter *q,
		  const struct character *qc, const struct hit *h, int guarded, int face,
		  uint32_t keys, int scale, int knock)
{
	int v = guarded ? h->chip : h->damage;
	if (scale != 1000)
		v = permille(v, scale);
	int dealt = permille(v, qc->stats.defense);
	q->hp = clamp(q->hp - dealt, 0, qc->stats.life);
	if (s->game == GAME_PRACTICE && !q->hp)
		q->hp = 1;
	q->move = q->hit = q->squat = q->dash = 0;
	q->stun = guarded ? h->blockstun : h->hitstun;
	q->block = guarded;
	q->elapsed = 0;
	if (guarded)
		q->crouch = (keys & KEY(DOWN)) != 0;
	if (q->y)
		q->crouch = 0;
	q->down = UP;
	if (knock && !guarded && q->hp) {
		q->down = FALLING;
		q->stun = q->block = q->crouch = q->walk = 0;
	}
	int push = guarded ? h->block_pushback : h->pushback;
	q->slide = face * clamp((push * 1000 + qc->stats.weight / 2) / qc->stats.weight, 0,
				MAX_SLIDE);
	q->corner = p != NULL && q->slide;
	if (p && p->freeze < h->hitstop)
		p->freeze = h->hitstop;
	if (q->freeze < h->hitstop)
		q->freeze = h->hitstop;
	s->tone = 2400;
	s->hits++;
	s->guards += (unsigned)guarded;
	return dealt;
}
/* A spark of system sy's effect key at world x, screen y: the first free
 * slot, else the oldest (lowest slot on ties). */
static void spark(struct fight_state *s, const struct assets *a, const struct system *sy,
		  int key, int x, int y, int flip)
{
	unsigned kind = sy->effect[key];
	if (!kind || kind > a->sparks)
		return;
	int k = 0;
	for (int j = 0; j < SPARK_SLOTS; j++) {
		if (!s->spark[j].kind) {
			k = j;
			break;
		}
		if (s->spark[j].age > s->spark[k].age)
			k = j;
	}
	s->spark[k] = (struct spark_slot){(int32_t)kind, x, y, 0, flip};
}
/* Sparks, shake and sounds of a contact by side i (move k + 1, or 0 for a
 * projectile) at point at; ko when it emptied the defender's life. */
static void react(struct fight_state *s, const struct assets *a, int i, const struct hit *h,
		  int k, int slash, int guarded, const int at[2], int face, int ko)
{
	const struct system *sy = &a->sys[who(s, a, i)->system];
	int t = tier(h, k - 1), flip = face < 0;
	if (guarded) {
		spark(s, a, sy, slash ? FX_SLASH_GUARD : FX_GUARD, at[0], at[1], flip);
		sound(s, a, slash ? SOUND_SLASH_GUARD : SOUND_GUARD, i, k);
	} else {
		spark(s, a, sy, (slash ? FX_SLASH_LIGHT : FX_HIT_LIGHT) + t, at[0], at[1], flip);
		if (slash)
			spark(s, a, sy, FX_BLOOD_LIGHT + t, at[0], at[1], flip);
		sound(s, a, (slash ? SOUND_SLASH_LIGHT : SOUND_HIT_LIGHT) + t, i, k);
		if (t && s->shake < SHAKE_HEAVY)
			s->shake = SHAKE_HEAVY;
	}
	if (ko) {
		sound(s, a, SOUND_KO, 1 - i, 0);
		s->shake = SHAKE_KO;
		s->flash = FLASH_TICKS;
	}
}
/* An unguarded hit by side i dealing dealt: the next hit of a combo when
 * the defender was still in hitstun, else the first. From the second hit
 * on the count is on screen and its timer waits until the combo ends.
 * damage sums the combo's hits, last keeps it after the combo ends. */
#define MAX_COMBO_DAMAGE 65535
static void count_hit(struct fight_state *s, int i, int stunned, int dealt)
{
	uint32_t n = stunned && s->combo[i] ? s->combo[i] + 1 : 1;
	uint32_t total = (n > 1 ? s->damage[i] : 0) + (uint32_t)dealt;
	s->damage[i] = s->last[i] = total < MAX_COMBO_DAMAGE ? total : MAX_COMBO_DAMAGE;
	s->combo[i] = n < MAX_COMBO ? n : MAX_COMBO;
	if (s->combo[i] >= 2) {
		s->shown[i] = s->combo[i];
		s->show[i] = COMBO_TICKS;
	}
	if (s->combo[i] > s->best[i])
		s->best[i] = s->combo[i];
}
static int in_hitstun(const struct fighter *p) { return p->stun && p->block != 1; }
/* Fighter i's throw takes the other: held facing it, body distance in
 * front, whatever it was doing dropped. */
static void seize(struct fight_state *s, const struct assets *a, int i)
{
	struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	q->grab = GRAB_HELD;
	q->elapsed = q->move = q->hit = q->stun = q->block = q->squat = q->walk = q->crouch = 0;
	q->dash = p->dash = 0;
	q->buffer = q->age = q->clock = q->freeze = q->slide = q->corner = q->y = q->vy = 0;
	p->freeze = p->slide = p->corner = 0;
	q->face = -p->face;
	q->x = wall(s, a, p->x + p->face * QUARTER(body(s, a)));
}
/* A broken throw (or two at once, rule of side i): both fighters recover
 * in the guard pose for tech_stun ticks, sliding tech_push pixels apart. */
static void tech(struct fight_state *s, const struct assets *a, int i)
{
	const struct throw_rule *t = rule(s, a, i);
	for (int j = 0; j < 2; j++) {
		struct fighter *f = &s->p[j];
		f->grab = GRAB_NONE;
		f->move = f->hit = f->squat = f->walk = f->crouch = f->buffer = f->age = f->dash = 0;
		f->stun = t->tech_stun;
		f->block = 1;
		f->elapsed = 0;
		f->slide = -f->face * t->tech_push;
		f->corner = 0;
	}
	s->techs++;
	sound(s, a, SOUND_GUARD, i, 0);
}
/* Fighter i's throw ends: the one held takes the throw hit (it knocks
 * down) at the centre of its first hurtbox, thrown forward, or from behind
 * the thrower the other way. */
static void release(struct fight_state *s, const struct assets *a, int i, int scale)
{
	struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	const struct hit *h = &rule(s, a, i)->hit;
	const struct step *b = &a->step[fight_pose(s, a, 1 - i)];
	int back = p->grab == GRAB_BACK, alive = q->hp > 0, at[2] = {px(q), GROUND + q->y - 64};
	if (b->hurt) {
		struct area r = fight_box(&a->box[b->box], px(q), q->y, q->face);
		at[0] = r.x + r.w / 2;
		at[1] = r.y + r.h / 2;
	}
	p->grab = q->grab = GRAB_NONE;
	p->elapsed = q->elapsed = 0;
	if (back)
		q->x = wall(s, a, p->x - p->face * QUARTER(body(s, a)));
	int dir = back ? -p->face : p->face;
	count_hit(s, i, 0, struck(s, p, q, who(s, a, 1 - i), h, 0, dir, 0, scale, 1));
	react(s, a, i, h, 0, 0, 0, at, dir, alive && !q->hp);
	s->throws++;
}
static void shots_move(struct fight_state *s, const struct assets *a)
{
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		struct shot *sh = &s->shot[k];
		if (!sh->kind)
			continue;
		const struct projectile *t = shot_type(s, a, sh);
		sh->age++;
		sh->x += sh->face * t->speed;
		if (sh->age >= t->lifetime || sh->x < (int)s->cam - SHOT_MARGIN ||
		    sh->x > (int)s->cam + FW + SHOT_MARGIN)
			memset(sh, 0, sizeof(*sh));
	}
}
/* cpu: side 1 is the ladder's CPU opponent (its hits scale by the level). */
static void combat(struct fight_state *s, const struct assets *a, const uint32_t keys[2], int cpu)
{
	const int scale[2] = {1000, cpu ? levels[s->level].damage : 1000};
	int threat[2], frozen[2], reach[2] = {in_throw_range(s, a, 0), in_throw_range(s, a, 1)};
	/* Both fighters see the other's state from the start of the tick. */
	for (int i = 0; i < 2; i++) {
		const struct fighter *p = &s->p[i], *q = &s->p[1 - i];
		int range = who(s, a, i)->stats.guard_range;
		threat[i] = q->move && abs(px(q) - px(p)) < range;
		for (int k = 0; k < MAX_PROJECTILES; k++)
			if (s->shot[k].kind && s->shot[k].owner != i &&
			    abs(s->shot[k].x - px(p)) < range)
				threat[i] = 1;
	}
	shots_move(s, a);
	for (int i = 0; i < 2; i++)
		frozen[i] = update(s, a, i, keys[i], threat[i], reach[i]);
	/* Throws, before any strike: a throw begun this tick (elapsed 0) takes
	 * the opponent, two at once break each other; a held fighter's tech
	 * press breaks the throw, else it is released once the thrower's THROW
	 * animation is over. */
	int fresh[2] = {throwing(&s->p[0]) && !s->p[0].elapsed,
			throwing(&s->p[1]) && !s->p[1].elapsed};
	if (fresh[0] && fresh[1])
		tech(s, a, 0);
	for (int i = 0; i < 2; i++) {
		struct fighter *p = &s->p[i];
		if (fresh[i] && !fresh[1 - i])
			seize(s, a, i);
		else if (throwing(p) && s->p[1 - i].grab == GRAB_TECH)
			tech(s, a, i);
		else if (throwing(p) && p->elapsed >= who(s, a, i)->action[THROW].total)
			release(s, a, i, scale[i]);
	}
	/* Who is still in hitstun as contacts land (a combo goes on). */
	int stunned[2] = {in_hitstun(&s->p[0]), in_hitstun(&s->p[1])};
	/* Resolve every contact from the same state before changing any. */
	const struct move *move[2] = {NULL, NULL};
	int guarded[2] = {0, 0}, group[2] = {0, 0}, struck_now[2] = {0, 0}, at[2][2],
	    shot_at[MAX_PROJECTILES][2];
	for (int i = 0; i < 2; i++)
		if (!frozen[i] && contact(s, a, i, at[i])) {
			move[i] = &who(s, a, i)->move[s->p[i].move - 1];
			group[i] = a->step[fight_pose(s, a, i)].group;
			guarded[i] = blocks(&s->p[1 - i], keys[1 - i], &move[i]->hit);
			struck_now[1 - i] = 1;
		}
	int gone[MAX_PROJECTILES] = {0}, landed[MAX_PROJECTILES] = {0},
	    shot_guard[MAX_PROJECTILES] = {0};
	for (int k = 0; k < MAX_PROJECTILES; k++)
		for (int j = k + 1; j < MAX_PROJECTILES; j++)
			if (s->shot[k].kind && s->shot[j].kind &&
			    s->shot[k].owner != s->shot[j].owner &&
			    shots_clash(s, a, &s->shot[k], &s->shot[j])) {
				gone[k] = gone[j] = 1;
				s->clashes++;
				sound(s, a, SOUND_CLASH, s->shot[k].owner, 0);
			}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		int d = 1 - sh->owner;
		if (!sh->kind || gone[k] || !s->p[d].hp || struck_now[d] ||
		    !shot_hits_body(s, a, sh, d, shot_at[k]))
			continue;
		landed[k] = 1;
		struck_now[d] = 1;
		shot_guard[k] = blocks(&s->p[d], keys[d], &shot_type(s, a, sh)->hit);
	}
	for (int i = 0; i < 2; i++) {
		if (!move[i])
			continue;
		struct fighter *p = &s->p[i];
		int alive = s->p[1 - i].hp > 0;
		/* Only a move's last hit group knocks down. */
		int dealt = struck(s, p, &s->p[1 - i], who(s, a, 1 - i), &move[i]->hit, guarded[i],
				   p->face, keys[1 - i], scale[i],
				   (move[i]->hit.flags & FLAG_KNOCKDOWN) &&
				       group[i] == move[i]->groups);
		if (!guarded[i])
			count_hit(s, i, stunned[1 - i], dealt);
		react(s, a, i, &move[i]->hit, p->move, move[i]->slash, guarded[i], at[i], p->face,
		      alive && !s->p[1 - i].hp);
		if (p->move)
			p->hit = group[i];
	}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		struct shot *sh = &s->shot[k];
		if (landed[k]) {
			int d = 1 - sh->owner, alive = s->p[d].hp > 0;
			const struct hit *h = &shot_type(s, a, sh)->hit;
			int dealt = struck(s, NULL, &s->p[d], who(s, a, d), h, shot_guard[k],
					   sh->face, keys[d], scale[sh->owner],
					   h->flags & FLAG_KNOCKDOWN);
			if (!shot_guard[k])
				count_hit(s, sh->owner, stunned[d], dealt);
			react(s, a, sh->owner, h, 0, 0, shot_guard[k], shot_at[k], sh->face,
			      alive && !s->p[d].hp);
		}
		if (landed[k] || gone[k])
			memset(sh, 0, sizeof(*sh));
	}
	struct fighter *p0 = &s->p[0], *p1 = &s->p[1];
	int push0 = who(s, a, 0)->stats.push, push1 = who(s, a, 1)->stats.push,
	    push = push0 > push1 ? push0 : push1;
	if (grounded(p0) && grounded(p1) && !p0->grab && abs(p0->x - p1->x) < QUARTER(push - 1)) {
		int mid = (p0->x + p1->x) / 2, f = p0->x <= p1->x ? 1 : -1;
		p0->x = wall(s, a, mid - f * 2 * push);
		p1->x = wall(s, a, mid + f * 2 * push);
	}
	camera(s, a);
	/* A combo ends once its defender is out of hitstun. */
	for (int i = 0; i < 2; i++)
		if (!in_hitstun(&s->p[1 - i]))
			s->combo[i] = s->damage[i] = 0;
	/* Practice: life refills once no combo runs on a fighter that is up
	 * and free of throws. */
	for (int i = 0; s->game == GAME_PRACTICE && i < 2; i++) {
		struct fighter *f = &s->p[i];
		if (!s->combo[1 - i] && !in_hitstun(f) && !f->down && !f->grab)
			f->hp = who(s, a, i)->stats.life;
	}
	if (!p0->hp || !p1->hp)
		s->ended = 1;
}
/* The practice dummy's keys: nothing, down, the guard each incoming
 * strike or projectile needs (crouching for lows), or the CPU. */
static uint32_t dummy(struct fight_state *s, const struct assets *a)
{
	const struct fighter *p = &s->p[1], *q = &s->p[0];
	if (s->dummy == DUMMY_CROUCH)
		return KEY(DOWN);
	if (s->dummy == DUMMY_CPU)
		return ai(s, a, 1);
	if (s->dummy != DUMMY_GUARD)
		return 0;
	int range = who(s, a, 1)->stats.guard_range, guard = -1;
	if (q->move && abs(px(q) - px(p)) < range)
		guard = who(s, a, 0)->move[q->move - 1].hit.guard;
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		if (sh->kind && !sh->owner && abs(sh->x - px(p)) < range)
			guard = guard == GUARD_LOW ? guard : shot_type(s, a, sh)->hit.guard;
	}
	return guard < 0 ? 0 : back_key(p) | (guard == GUARD_LOW ? KEY(DOWN) : 0u);
}
/* ---- ladder ---- */
/* Characters P1 meets in a run: everyone else, or itself in a one-character
 * roster. */
static uint32_t opponents(const struct assets *a, unsigned self)
{
	uint32_t all = a->characters >= 32 ? 0xffffffffu : (1u << a->characters) - 1u;
	return a->characters == 1 ? 1u : all & ~(1u << self);
}
static unsigned count(uint32_t m)
{
	unsigned n = 0;
	for (; m; m &= m - 1)
		n++;
	return n;
}
static void next_stage(struct fight_state *s, const struct assets *a)
{
	uint32_t left = opponents(a, s->chr[0]) & ~s->beaten;
	unsigned pick = random_next(s) % count(left), c = 0;
	for (;; c++)
		if ((left >> c) & 1 && !pick--)
			break;
	s->stage = count(s->beaten) + 1;
	s->wins[0] = s->wins[1] = 0;
	begin_round(s, a, s->chr[0], c, 1);
}
static void begin_run(struct fight_state *s, const struct assets *a, unsigned c0)
{
	s->chr[0] = c0;
	s->beaten = 0;
	next_stage(s, a);
}
/* The next survival fight: a random opponent (not the last one when there
 * is a choice), P1 at its carried life. */
static void survive(struct fight_state *s, const struct assets *a)
{
	uint32_t left = opponents(a, s->chr[0]);
	if (count(left) > 1)
		left &= ~(1u << s->chr[1]);
	unsigned pick = random_next(s) % count(left), c = 0;
	for (;; c++)
		if ((left >> c) & 1 && !pick--)
			break;
	s->stage = 1;
	s->wins[0] = s->wins[1] = 0;
	begin_round(s, a, s->chr[0], c, 1);
	s->p[0].hp = (int32_t)s->carry;
}
static void to_select(struct fight_state *s, const struct assets *a)
{
	place(s);
	s->mode = MODE_SELECT;
	s->timer = s->beaten = s->stage = s->wins[0] = s->wins[1] = 0;
	s->run++;
	s->chr[1] = s->chr[0];
	s->pal[0] = 0;
	s->pal[1] = 1;
	s->picking = s->carry = 0;
	if (s->automatic)
		begin_run(s, a, s->run % a->characters);
}
static void round_over(struct fight_state *s, const struct assets *a)
{
	int alive0 = s->p[0].hp > 0, alive1 = s->p[1].hp > 0;
	s->round++;
	if (alive0 != alive1)
		s->wins[alive0 ? 0 : 1]++;
	if (s->game == GAME_SURVIVAL && s->wins[0] >= ROUNDS_TO_WIN) {
		/* A survival win: count it, heal a little, on to the next. */
		int life = who(s, a, 0)->stats.life, hp = s->p[0].hp + life * SURVIVAL_HEAL / 1000;
		s->streak += s->streak < MAX_STREAK;
		s->record = s->streak > s->record ? s->streak : s->record;
		s->carry = (uint32_t)(hp < life ? hp : life);
		survive(s, a);
	} else if (s->game == GAME_SURVIVAL && s->wins[1] < ROUNDS_TO_WIN) {
		/* A double KO: the fight again from the life P1 brought in. */
		s->wins[0] = s->wins[1] = 0;
		begin_round(s, a, s->chr[0], s->chr[1], 1);
		s->p[0].hp = (int32_t)s->carry;
	} else if (s->game == GAME_SURVIVAL) {
		place(s);
		s->mode = MODE_RESULT;
		s->timer = s->carry = 0;
		s->wins[0] = s->wins[1] = 0;
	} else if (s->game == GAME_PRACTICE) {
		/* Practice never takes the last point of life; just in case. */
		begin_round(s, a, s->chr[0], s->chr[1], 1);
	} else if (s->wins[0] >= ROUNDS_TO_WIN) {
		s->beaten |= 1u << s->chr[1];
		uint32_t all = opponents(a, s->chr[0]);
		if ((s->beaten & all) == all) {
			place(s);
			s->mode = MODE_CLEAR;
			s->timer = 0;
			s->wins[0] = s->wins[1] = 0;
		} else
			next_stage(s, a);
	} else if (s->wins[1] >= ROUNDS_TO_WIN) {
		place(s);
		s->mode = MODE_RESULT;
		s->timer = 0;
		s->wins[0] = s->wins[1] = 0;
	} else {
		/* Another round of the same match, both at full life. */
		begin_round(s, a, s->chr[0], s->chr[1], 1);
	}
}
/* The select screen: the cursor is P1 (chr[0], chr[1] follows it), or the
 * dummy (chr[1]) once a practice P1 is picked. An attack button picks:
 * the arcade ladder, a survival run or, in practice, P1 and then the
 * dummy. */
static void select_screen(struct fight_state *s, const struct assets *a, uint32_t pressed)
{
	int n = (int)a->characters, c = (int)s->chr[s->picking];
	if (s->automatic) {
		begin_run(s, a, s->run % a->characters);
		return;
	}
	if (s->timer < 255)
		s->timer++;
	if (pressed & KEY(LEFT))
		c = (c + n - 1) % n;
	if (pressed & KEY(RIGHT))
		c = (c + 1) % n;
	if ((pressed & KEY(UP)) && c >= 4)
		c -= 4;
	if ((pressed & KEY(DOWN)) && c + 4 < n)
		c += 4;
	if (s->picking) {
		s->chr[1] = (uint32_t)c;
		s->pal[1] = s->chr[1] == s->chr[0];
	} else
		s->chr[0] = s->chr[1] = (uint32_t)c;
	if (!(pressed & ATTACKS))
		return;
	if (s->game == GAME_ARCADE) {
		begin_run(s, a, (unsigned)c);
	} else if (s->game == GAME_SURVIVAL) {
		s->streak = 0;
		s->carry = (uint32_t)a->chr[c].stats.life;
		survive(s, a);
	} else if (!s->picking) {
		/* The dummy cursor starts on the next character. */
		s->picking = 1;
		s->chr[1] = (uint32_t)(n > 1 ? (c + 1) % n : c);
		s->pal[1] = s->chr[1] == s->chr[0];
	} else {
		s->picking = 0;
		s->stage = 1;
		begin_round(s, a, s->chr[0], s->chr[1], 1);
	}
}
void fight_step(struct fight_state *s, const struct assets *a, uint32_t input,
		uint32_t second, int human)
{
	uint32_t pressed = input & ~s->previous;
	s->previous = input;
	s->sounds = 0;
	memset(s->sound, 0, sizeof(s->sound));
	/* START pauses, or on the select screen toggles the command list. */
	if (pressed & KEY(START)) {
		if (s->mode == MODE_SELECT && !s->automatic && !s->paused)
			s->list ^= 1;
		else
			s->paused ^= 1;
	}
	/* L toggles the box overlay, or on the select screen cycles the game
	 * mode (back to picking P1). */
	int choosing = s->mode == MODE_SELECT && !s->automatic && !s->paused;
	if ((pressed & KEY(L)) && choosing) {
		s->game = (s->game + 1) % GAMES;
		s->chr[1] = s->chr[0];
		s->pal[1] = 1;
		s->picking = 0;
	} else if (pressed & KEY(L))
		s->boxes ^= 1;
	/* R cycles the stress scene, on the select screen the CPU level (Easy,
	 * Normal, Hard) and in a practice fight the dummy. */
	if ((pressed & KEY(R)) && choosing)
		s->level = (s->level + 1) % LEVELS;
	else if ((pressed & KEY(R)) && s->mode == MODE_FIGHT && s->game == GAME_PRACTICE)
		s->dummy = (s->dummy + 1) % DUMMIES;
	else if (pressed & KEY(R))
		s->stress = (s->stress + 1) % 4;
	/* B in a paused fight quits to the select screen. */
	if (s->paused && (pressed & KEY(B)) && s->mode == MODE_FIGHT && !s->automatic) {
		s->paused = 0;
		to_select(s, a);
		return;
	}
	if (s->paused)
		return;
	if (s->automatic)
		s->stress = (s->tick / 600) % 4;
	s->tick++;
	if (s->mode == MODE_SELECT) {
		select_screen(s, a, pressed);
		return;
	}
	if (s->mode != MODE_FIGHT) {
		if (s->timer < 255)
			s->timer++;
	} else {
		/* Sparks, shake and flash run on every fight tick, hitstop too. */
		for (int k = 0; k < SPARK_SLOTS; k++) {
			struct spark_slot *sp = &s->spark[k];
			if (sp->kind && ++sp->age >= a->spark[sp->kind - 1].total)
				memset(sp, 0, sizeof(*sp));
		}
		if (s->shake)
			s->shake--;
		if (s->flash)
			s->flash--;
		/* A count fades COMBO_TICKS after its combo ended (a single hit
		 * since does not hold it). */
		for (int i = 0; i < 2; i++)
			if (s->combo[i] < 2 && s->show[i] && !--s->show[i])
				s->shown[i] = 0;
	}
	if (s->mode != MODE_FIGHT) {
		if (s->timer > (s->automatic ? AUTO_SCREEN : SCREEN_TICKS) ||
		    (s->timer > SCREEN_SKIP && (pressed & ATTACKS)))
			to_select(s, a);
		return;
	}
	if (s->timer < ROUND_TICKS) {
		/* The versus card and READY: nothing moves and the AI waits;
		 * buttons held now are no press when FIGHT appears. */
		s->p[0].keys = (int32_t)((s->automatic ? 0 : input) & 0xffffu);
		s->p[1].keys = (int32_t)((human ? second : 0) & 0xffffu);
		if (++s->timer == ROUND_TICKS)
			sound(s, a, SOUND_ROUND_START, 0, 0);
		return;
	}
	/* FIGHT stays up for a while as the round runs. */
	if (s->timer < BANNER_END)
		s->timer++;
	if (s->ended) {
		/* Airborne fighters still land and projectiles fly while the KO
		 * plays; nothing hits. */
		for (int i = 0; i < 2; i++) {
			struct fighter *p = &s->p[i];
			if (p->move || p->dash)
				p->move = p->elapsed = p->hit = p->dash = 0;
			if (p->down)
				knocked(s, a, i);
			else if (!grounded(p))
				fall(s, a, i);
			p->walk = 0;
		}
		shots_move(s, a);
		camera(s, a);
		if (++s->ended > KO_TICKS || (pressed & (KEY(A) | KEY(B))))
			round_over(s, a);
		return;
	}
	uint32_t keys[2];
	keys[0] = s->automatic ? ai(s, a, 0) : input;
	keys[1] = human				? second
		  : s->game == GAME_PRACTICE ? dummy(s, a)
					     : ai(s, a, 1);
	combat(s, a, keys, !human && !s->automatic);
}
/* ---- drawing ---- */
static void rect(struct scene *s, int x, int y, int w, int h, uint16_t color, int blend,
		 int alpha)
{
	if (s->count < COMMANDS)
		s->cmd[s->count++] = (struct command){
		    RECT, x, y, w, h, 0, 0, blend, color, (uint8_t)alpha};
}
/* v in decimal (at most four digits). */
static void number(char *out, unsigned v)
{
	for (unsigned d = 1000; d > 1; d /= 10)
		if (v >= d)
			*out++ = (char)('0' + v / d % 10);
	*out++ = (char)('0' + v % 10);
	*out = 0;
}
/* Compact original 3x5 glyphs: digits, then the limited HUD alphabet. */
static const char alphabet[] = "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ:-/+[]>.";
static const uint16_t glyph[] = {31599, 9362, 29671, 29647, 23497, 31183, 31215, 29257,
				 31727, 31695, 0, 11245, 27566, 31015, 27502, 31207,
				 31204, 31087, 23533, 29847, 4687, 23469, 18727, 24557,
				 31597, 31599, 31716, 31611, 27565, 31183, 29842, 23407,
				 23402, 23423, 23213, 23186, 29351, 1040, 448, 4772,
				 1488, 26918, 12875, 17492, 2};
/* Bytes 1..9 (not 5) are 5x5 arrows pointing like the numpad digit, as the
 * move list writes motions for a fighter facing right (FIGHT_ARROW). */
static const uint32_t arrow[9] = {1135388, 4347332, 17044583, 4488452, 0,
				  4291652, 30167105, 4674692, 7443728};
static int is_arrow(char c) { return c >= 1 && c <= 9 && c != 5; }
/* Each glyph row's lit run is one rectangle of cell x cell pixels per glyph
 * pixel; letters advance 4 cells, arrows 6. */
static void text_cell(struct scene *s, int x, int y, const char *str, uint16_t color, int cell)
{
	for (; *str; str++) {
		int wide = is_arrow(*str), w = wide ? 5 : 3;
		const char *p = wide ? NULL : strchr(alphabet, *str);
		uint32_t bits = wide ? arrow[*str - 1] : p && *str ? glyph[p - alphabet] : 0;
		for (int r = 0; r < 5; r++)
			for (int c = 0; c < w; c++) {
				int top = w * 5 - 1 - r * w;
				if (!(bits & (1u << (top - c))))
					continue;
				int n = 1;
				while (c + n < w && (bits & (1u << (top - c - n))))
					n++;
				rect(s, x + c * cell, y + r * cell, cell * n, cell, color, OPAQUE, 255);
				c += n - 1;
			}
		x += (w + 1) * cell;
	}
}
static void text(struct scene *s, int x, int y, const char *str, uint16_t color)
{
	text_cell(s, x, y, str, color, 2);
}
static int text_width(const char *str)
{
	int w = 0;
	for (; *str; str++)
		w += is_arrow(*str) ? 12 : 8;
	return w;
}
/* ---- the move list ---- */
static void append(char *out, const char *add)
{
	size_t n = strlen(out), k = strlen(add);
	if (n + k >= LIST_TEXT)
		k = LIST_TEXT - 1 - n;
	memcpy(out + n, add, k);
	out[n + k] = 0;
}
/* Button labels; weapon punches are slashes. */
static void button_names(const struct character *c, unsigned bits, char *out)
{
	static const char *const plain[4] = {"LP", "HP", "LK", "HK"},
				 *const weapon[4] = {"LS", "HS", "LK", "HK"};
	const char *sep = "";
	if ((bits & 3) == 3) {
		append(out, (c->slash & 3) == 3 ? "S" : "P");
		bits &= ~3u;
		sep = "/";
	}
	if ((bits & 12) == 12) {
		append(out, sep);
		append(out, "K");
		bits &= ~12u;
		sep = "/";
	}
	for (int k = 0; k < 4; k++)
		if (bits >> k & 1) {
			append(out, sep);
			append(out, (c->slash >> k & 1) ? weapon[k] : plain[k]);
			sep = "/";
		}
}
static void list_line(const struct character *c, unsigned k, char *out)
{
	static const char *const notation[MOTIONS] = {
	    "\2\3\6", "\2\1\4", "\6\2\3", "[\4]\6", "[\2]\10", "", "", "\10", "\2", "\6", "\4",
	    "\4\1\2\3\6", "\6\3\2\1\4", "\4\2\1"};
	const struct move *m = &c->move[k];
	out[0] = 0;
	if (k < MOVES) {
		/* A normal with follow-ups: its button, down for the crouching ones. */
		if (k >= CROUCH_LP)
			append(out, "\2 + ");
		button_names(c, 1u << (k % 4), out);
	} else {
		const char *n = m->motion < MOTIONS ? notation[m->motion] : "";
		append(out, n);
		if (*n)
			append(out, " + ");
		button_names(c, m->buttons, out);
	}
	if (m->spawn)
		append(out, "  PROJECTILE");
	if (m->groups && m->hit.guard == GUARD_LOW)
		append(out, "  LOW");
	if (m->groups && m->hit.guard == GUARD_OVERHEAD)
		append(out, "  OVERHEAD");
	if (m->after && m->contact)
		append(out, "  ON CONTACT");
}
struct list {
	char name[LIST_LINES][MAX_MOVE_NAME + 1], text[LIST_LINES][LIST_TEXT];
	uint8_t depth[LIST_LINES];
	unsigned n;
};
/* Move k at depth, then its follow-ups one level deeper. */
static void list_add(const struct character *c, unsigned k, unsigned depth, struct list *l)
{
	if (depth > 3 || l->n >= LIST_LINES)
		return;
	memcpy(l->name[l->n], c->move[k].name, sizeof(l->name[0]));
	list_line(c, k, l->text[l->n]);
	l->depth[l->n++] = (uint8_t)depth;
	for (unsigned j = MOVES; j < c->moves; j++)
		if (c->move[j].after == k + 1)
			list_add(c, j, depth + 1, l);
}
unsigned fight_move_list(const struct character *c, char name[LIST_LINES][MAX_MOVE_NAME + 1],
			 char text[LIST_LINES][LIST_TEXT], uint8_t depth[LIST_LINES])
{
	struct list l;
	l.n = 0;
	for (unsigned k = MOVES; k < c->moves; k++)
		if (!c->move[k].after)
			list_add(c, k, 0, &l);
	for (unsigned k = 0; k < MOVES; k++) {
		int parent = 0;
		for (unsigned j = MOVES; j < c->moves; j++)
			parent |= c->move[j].after == k + 1;
		if (parent)
			list_add(c, k, 0, &l);
	}
	memcpy(name, l.name, sizeof(l.name));
	memcpy(text, l.text, sizeof(l.text));
	memcpy(depth, l.depth, sizeof(l.depth));
	return l.n;
}
/* The list at x, y in lines of step pixels (follow-ups indented under a
 * ">"), then the normals legend; returns the y below it. Each move shows
 * its name (white) and its command (light green): in one row with the
 * commands in a column after the longest name, or with two_rows the
 * command in the row under the name (the narrow select panel). */
static int move_list(struct scene *out, const struct character *c, int x, int y, int step,
		     int two_rows)
{
	char names[LIST_LINES][MAX_MOVE_NAME + 1], lines[LIST_LINES][LIST_TEXT];
	char legend[LIST_TEXT] = "";
	uint8_t depth[LIST_LINES];
	unsigned n = fight_move_list(c, names, lines, depth);
	int column = 0;
	for (unsigned i = 0; i < n; i++) {
		int w = depth[i] * 16 + text_width(names[i]) + (names[i][0] ? 16 : 0);
		column = w > column ? w : column;
	}
	for (unsigned i = 0; i < n; i++, y += step) {
		int dx = depth[i] * 16;
		if (depth[i])
			text(out, x + dx - 10, y, ">", 0x8410);
		if (names[i][0]) {
			text(out, x + dx, y, names[i], 0xffff);
			if (two_rows)
				y += step;
		}
		text(out, x + (two_rows ? dx + (names[i][0] ? 16 : 0) : column), y, lines[i], 0xafe5);
	}
	if (!n) {
		text(out, x, y, "NO SPECIALS", 0x8410);
		y += step;
	}
	y += step / 2;
	static const char *const keys[4] = {"Y ", "  X ", "  B ", "  A "};
	for (int k = 0; k < 4; k++) {
		append(legend, keys[k]);
		button_names(c, 1u << k, legend);
	}
	text(out, x, y, legend, 0xbdf7);
	text(out, x, y + step, "\2 CROUCH  \10 JUMP", 0xbdf7);
	text(out, x, y + 2 * step, "\4 GUARD  \1 LOW GUARD", 0xbdf7);
	return y + 3 * step;
}
static void sprite(struct scene *s, const struct assets *a, int image, int x, int y,
		   int flip, int blend, int alpha)
{
	const struct image *im = &a->image[image];
	if (s->count < COMMANDS)
		s->cmd[s->count++] = (struct command){
		    SPRITE, flip ? x - (im->w - im->ax) : x - im->ax,
		    y - im->ay, im->w, im->h, image, flip, blend, 0, (uint8_t)alpha};
}
/* Blended effects share EFFECT_PIXEL_BUDGET opaque pixels per frame; an
 * effect that no longer fits is dropped and later smaller ones may still
 * draw. Rules never see effects, so dropping cannot change the fight. */
static void effect(struct scene *s, const struct assets *a, int image, int x, int y,
		   int flip, int blend, int alpha, uint32_t *budget)
{
	uint32_t cost = a->image[image].count;
	s->effects_requested++;
	if (cost > *budget || s->count >= COMMANDS)
		return;
	*budget -= cost;
	s->effects_drawn++;
	sprite(s, a, image, x, y, flip, blend, alpha);
}
/* Side i's image of a frame, or -1 when the cache does not hold it: not
 * expanded yet is reported in the scene (fight_demand fills it), a cache
 * that belongs to another cast draws nothing. */
static int cast_image(struct scene *out, const struct fight_state *s, const struct assets *a,
		      int i, unsigned f)
{
	if (a->cast[i][0] != (int)s->chr[i] || a->cast[i][1] != (int)s->pal[i])
		return -1;
	if (a->look[i][f] < 0) {
		if (out->misses < SCENE_MISSES)
			out->miss[out->misses] = (uint16_t)((unsigned)i << 15 | f);
		out->misses++;
	}
	return a->look[i][f];
}
/* Box overlay: a one-pixel opaque outline, hitboxes also tinted. */
static void outline(struct scene *s, struct area r, uint16_t color, int alpha, int width)
{
	if (alpha)
		rect(s, r.x, r.y, r.w, r.h, color, ALPHA, alpha);
	rect(s, r.x, r.y, r.w, width, color, OPAQUE, 255);
	rect(s, r.x, r.y + r.h - width, r.w, width, color, OPAQUE, 255);
	rect(s, r.x, r.y, width, r.h, color, OPAQUE, 255);
	rect(s, r.x + r.w - width, r.y, width, r.h, color, OPAQUE, 255);
}
/* The flat fallback background of a pack without stages (the world is then
 * FW wide, so the camera never moves). */
static void flat_stage(struct scene *out, int dy)
{
	rect(out, 0, 0, FW, FH, 0x10c6, OPAQUE, 255);
	for (int i = 0; i < 8; i++)
		rect(out, i * 64, 100 + (i % 3) * 15 + dy, 46, 140, 0x212b, OPAQUE, 255);
	rect(out, 0, GROUND + dy, FW, FH - GROUND, 0x39a8, OPAQUE, 255);
	rect(out, 0, GROUND + dy, FW, 2, 0x8c50, OPAQUE, 255);
}
/* The cast stage's layers back to front, each followed by its anims' current
 * frames; a layer image already covers every camera and shake. */
static void stage(struct scene *out, const struct fight_state *s, const struct assets *a, int dy)
{
	if (!a->stages || a->cast_stage != (int)s->arena ||
	    a->stage_layer < a->stage[s->arena].layers) {
		out->stage_missing = a->stages && a->cast_stage == (int)s->arena;
		flat_stage(out, dy);
		return;
	}
	const struct stage *st = &a->stage[s->arena];
	int oy = GROUND - st->ground + dy;
	for (unsigned i = 0; i < st->layers; i++) {
		const struct layer *l = &a->layer[st->first_layer + i];
		int lx = fight_layer_x(l, (int)s->cam), ly = oy + l->y;
		if (a->layer_image[i] >= 0) {
			sprite(out, a, a->layer_image[i], lx + a->layer_x0[i], ly + a->layer_y0[i], 0,
			       OPAQUE, 255);
			out->layers++;
		}
		for (unsigned j = 0; j < l->anims; j++) {
			const struct stage_anim *an = &a->anim[l->first_anim + j];
			uint32_t t = s->tick % an->total;
			unsigned k = an->first;
			while (t >= a->stage_step[k].ticks)
				t -= a->stage_step[k++].ticks;
			const struct stage_step *sp = &a->stage_step[k];
			if (a->stage_look[k] >= 0) {
				sprite(out, a, a->stage_look[k], lx + sp->x, ly + sp->y, 0, OPAQUE, 255);
				out->layers++;
			}
		}
	}
}
static void idle_sprite(struct scene *out, const struct fight_state *s, const struct assets *a,
			int i, int x, int y, int flip)
{
	const struct character *c = who(s, a, i);
	const struct step *st = &a->step[step_at(a, &c->action[IDLE], s->tick, 1, NULL)];
	int im = cast_image(out, s, a, i, st->frame);
	if (im >= 0)
		sprite(out, a, im, x, y, flip, OPAQUE, 255);
}
/* The difficulty at the top right of the select screen and on the versus
 * card. */
static const char *const level_names[LEVELS] = {"CPU EASY", "CPU NORMAL", "CPU HARD"};
static void difficulty(struct scene *out, const struct fight_state *s, int y)
{
	static const uint16_t colors[LEVELS] = {0x07e0, 0xffff, 0xf800};
	const char *name = level_names[s->level];
	text(out, 464 - text_width(name), y, name, colors[s->level]);
}
/* The select screen's difficulty, with the key that changes it in grey
 * on its left. */
static void select_difficulty(struct scene *out, const struct fight_state *s)
{
	difficulty(out, s, 12);
	text(out, 464 - text_width(level_names[s->level]) - text_width("R "), 12, "R", 0x8410);
}
/* The game modes' names and colours (select screen, versus card, HUD). */
static const char *const game_names[GAMES] = {"ARCADE", "SURVIVAL", "PRACTICE"};
static const uint16_t game_colors[GAMES] = {0xffff, 0xfd20, 0x07ff};
static const char *const dummy_names[DUMMIES] = {"STAND", "CROUCH", "GUARD ALL", "CPU"};
/* The select screen's game mode at the top left, with the key that
 * changes it in grey before it (and the session's survival record). */
static void select_game(struct scene *out, const struct fight_state *s)
{
	text(out, 16, 12, "L", 0x8410);
	text(out, 16 + text_width("L "), 12, game_names[s->game], game_colors[s->game]);
	if (s->game == GAME_SURVIVAL && s->record) {
		char line[16] = "BEST ";
		number(line + 5, s->record);
		text(out, 16 + text_width("L SURVIVAL  "), 12, line, 0xbdf7);
	}
}
/* The select screen's command list: the highlighted fighter on the left,
 * its list in a panel on the right. */
static void select_list(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	int i = (int)s->picking;
	const struct character *c = &a->chr[s->chr[i]];
	rect(out, 0, 262, 190, 2, 0x8c50, OPAQUE, 255);
	idle_sprite(out, s, a, i, 96, 262, 0);
	text(out, 96 - text_width(c->name) / 2, 270, c->name, 0xffff);
	text(out, 16, 290, "ATTACK TO FIGHT", 0x8410);
	text(out, 16, 304, "START BACK", 0x8410);
	rect(out, 196, 32, 276, 280, 0x2124, OPAQUE, 255);
	text(out, 206, 40, c->name, 0xffe0);
	move_list(out, c, 206, 60, 14, 1);
}
/* The select screen: the game mode (top left), the difficulty (top
 * right), the roster grid with the cursor and P1's pose. Picking a
 * practice dummy keeps P1 outlined in green, outlines the dummy cursor in
 * cyan and shows both poses facing each other. */
static void select_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	rect(out, 0, 0, FW, FH, 0x0841, OPAQUE, 255);
	const char *title = s->picking ? "DUMMY" : "SELECT";
	text(out, 240 - text_width(title) / 2, 12, title, 0xffff);
	select_game(out, s);
	select_difficulty(out, s);
	if (s->list) {
		select_list(s, a, out);
		return;
	}
	for (unsigned k = 0; k < a->characters; k++) {
		int x = 16 + (int)(k % 4) * 114, y = 40 + (int)(k / 4) * 20;
		int cursor = k == s->chr[s->picking];
		rect(out, x, y, 108, 18, 0x2124, OPAQUE, 255);
		if (s->picking && k == s->chr[0])
			outline(out, (struct area){x, y, 108, 18}, 0x07e0, 0, 2);
		if (cursor)
			outline(out, (struct area){x + (s->picking ? 3 : 0), y + (s->picking ? 3 : 0),
						   108 - (s->picking ? 6 : 0), 18 - (s->picking ? 6 : 0)},
				s->picking ? 0x07ff : 0x07e0, 0, 2);
		text(out, x + 6, y + 4, a->chr[k].name, cursor ? 0xffe0 : 0xffff);
	}
	rect(out, 0, 300, FW, 2, 0x8c50, OPAQUE, 255);
	if (s->picking) {
		idle_sprite(out, s, a, 0, 100, 300, 0);
		idle_sprite(out, s, a, 1, 380, 300, 1);
		text(out, 180, 240, "P1", 0x07e0);
		text(out, 180, 254, a->chr[s->chr[0]].name, 0xffff);
		text(out, 180, 272, "DUMMY", 0x07ff);
		text(out, 180, 286, a->chr[s->chr[1]].name, 0xffff);
		return;
	}
	idle_sprite(out, s, a, 0, 120, 300, 0);
	text(out, 220, 240, "P1", 0x07e0);
	text(out, 220, 254, a->chr[s->chr[0]].name, 0xffff);
	text(out, 220, 272, s->game == GAME_PRACTICE ? "ATTACK TO PICK DUMMY" : "ATTACK TO FIGHT",
	     0x8410);
	text(out, 220, 286, "START COMMANDS", 0x8410);
}
static void screen_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	char line[24];
	unsigned all = count(opponents(a, s->chr[0]));
	int clear = s->mode == MODE_CLEAR, survival = s->game == GAME_SURVIVAL;
	rect(out, 0, 0, FW, FH, clear ? 0x0110 : 0x2000, OPAQUE, 255);
	const char *title = clear ? "ALL CLEAR" : survival ? "SURVIVAL OVER" : "YOU LOSE";
	text(out, 240 - text_width(title) / 2, 60, title, clear ? 0xffe0 : 0xf800);
	text(out, 240 - text_width(a->chr[s->chr[0]].name) / 2, 90, a->chr[s->chr[0]].name,
	     0xffff);
	if (survival) {
		/* Wins of this run and the session's best. */
		strcpy(line, "WINS ");
		number(line + 5, s->streak);
		text(out, 240 - text_width(line) / 2, 110, line, 0xffff);
		strcpy(line, "BEST ");
		number(line + 5, s->record);
		text(out, 240 - text_width(line) / 2, 128, line, 0xfd20);
	} else {
		unsigned beaten = count(s->beaten);
		strcpy(line, "BEATEN ");
		size_t n = strlen(line);
		line[n++] = (char)('0' + beaten / 10 % 10);
		line[n++] = (char)('0' + beaten % 10);
		line[n++] = '/';
		line[n++] = (char)('0' + all / 10 % 10);
		line[n++] = (char)('0' + all % 10);
		line[n] = 0;
		text(out, 240 - text_width(line) / 2, 110, line, 0xffff);
	}
	rect(out, 0, 300, FW, 2, 0x8c50, OPAQUE, 255);
	idle_sprite(out, s, a, 0, 240, 300, 0);
}

/* Which fighter is drawn on top: the thrower in a throw, the one in a move
 * (the more recent move if both are), else the one not stunned, else player 2 as before, so an attack is never
 * hidden behind the defender. Derived from the state, nothing is stored. */
static int front_fighter(const struct fight_state *s)
{
	const struct fighter *p = s->p, *q = s->p + 1;
	if (p->grab)
		return throwing(q);
	if (p->move && q->move)
		return q->elapsed < p->elapsed;
	if (p->move || q->move)
		return q->move != 0;
	if (!p->stun != !q->stun)
		return p->stun != 0;
	return 1;
}
/* The versus card of a ladder round: both idle poses facing each other,
 * the names, the ladder stage and the arena; no stage art, so it never
 * waits for the stage cache. */
static void intro_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	char line[24];
	rect(out, 0, 0, FW, FH, 0x0841, OPAQUE, 255);
	if (s->game == GAME_SURVIVAL) {
		strcpy(line, "SURVIVAL  WINS ");
		number(line + strlen(line), s->streak);
	} else if (s->game == GAME_PRACTICE) {
		strcpy(line, "PRACTICE");
	} else {
		strcpy(line, "STAGE ");
		number(line + 6, s->stage);
		size_t len = strlen(line);
		line[len++] = '/';
		number(line + len, count(opponents(a, s->chr[0])));
	}
	text(out, 240 - text_width(line) / 2, 40, line, game_colors[s->game]);
	difficulty(out, s, 12);
	rect(out, 0, 262, FW, 2, 0x8c50, OPAQUE, 255);
	idle_sprite(out, s, a, 0, 140, 262, 0);
	idle_sprite(out, s, a, 1, 340, 262, 1);
	text(out, 240 - text_width("VS") / 2, 160, "VS", 0xffe0);
	for (int i = 0; i < 2; i++) {
		const char *name = who(s, a, i)->name;
		text(out, (i ? 340 : 140) - text_width(name) / 2, 276, name, 0xffff);
	}
	if (a->stages && s->arena < a->stages)
		text(out, 240 - text_width(a->stage[s->arena].name) / 2, 300,
		     a->stage[s->arena].name, 0x8410);
}
/* Pause: the round dimmed under P1's command list. */
static void pause_panel(struct scene *out, const struct fight_state *s, const struct assets *a)
{
	const struct character *c = who(s, a, 0);
	rect(out, 0, 0, FW, FH, 0, ALPHA, 128);
	rect(out, 44, 54, FW - 88, 250, 0x0841, ALPHA, 224);
	text_cell(out, (FW - 19 * 4) / 2, 28, "PAUSE", 0xffff, 4);
	text(out, 60, 62, c->name, 0xffe0);
	int y = move_list(out, c, 60, 82, 14, 0);
	text(out, 60, y + 6, "START RESUME  B QUIT", 0x8410);
}
/* READY over the frozen round, then FIGHT as it begins: big glyphs (cell
 * BANNER_CELL) with a dark drop shadow, centred above the fighters. */
#define BANNER_CELL 10
static void banner(struct scene *out, const struct fight_state *s)
{
	if (s->timer < INTRO_TICKS || s->timer >= BANNER_END)
		return;
	int ready = s->timer < ROUND_TICKS;
	const char *word = ready ? "READY" : "FIGHT";
	int w = (4 * (int)strlen(word) - 1) * BANNER_CELL, x = (FW - w) / 2, y = 96;
	text_cell(out, x + 4, y + 4, word, 0x0000, BANNER_CELL);
	text_cell(out, x, y, word, ready ? 0xffe0 : 0xfa00, BANNER_CELL);
}
void fight_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	out->count = out->effects_requested = out->effects_drawn = out->sparks = out->layers = 0;
	out->misses = out->stage_missing = 0;
	if (s->mode == MODE_SELECT) {
		select_scene(s, a, out);
		return;
	}
	if (s->mode != MODE_FIGHT) {
		screen_scene(s, a, out);
		return;
	}
	if (s->timer < INTRO_TICKS) {
		intro_scene(s, a, out);
		return;
	}
	/* World commands shift by the camera (x) and the shake (y); a KO flash
	 * replaces the world with white. */
	int cx = -(int)s->cam,
	    dy = !s->shake ? 0 : (s->shake & 2 ? 1 : -1) * (s->shake > 8 ? SHAKE_MAX : 2);
	uint32_t budget = EFFECT_PIXEL_BUDGET;
	if (s->flash) {
		rect(out, 0, 0, FW, FH, 0xffff, OPAQUE, 255);
		goto hud;
	}
	stage(out, s, a, dy);
	for (int i = 0; i < 2; i++)
		rect(out, px(&s->p[i]) - 30 + cx, GROUND - 4 + dy, 60, 6, 0, ALPHA, 120);
	int front = front_fighter(s);
	for (int n = 0; n < 2; n++) {
		int i = n ? front : !front;
		const struct fighter *p = &s->p[i];
		/* Sheet art faces right; the foot anchor stands on the ground. */
		const struct step *st = &a->step[fight_pose(s, a, i)];
		int im = cast_image(out, s, a, i, st->frame);
		if (im >= 0)
			sprite(out, a, im, px(p) + cx, GROUND + p->y + dy, p->face < 0, OPAQUE, 255);
	}
	/* Projectiles: opaque ones as sprites, blended ones within the effect
	 * budget (ahead of the sparks and stress effects). */
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		if (!sh->kind)
			continue;
		const struct projectile *t = shot_type(s, a, sh);
		int im = cast_image(out, s, a, sh->owner, a->step[shot_step(s, a, sh)].frame);
		if (im < 0)
			continue;
		if (t->blend == OPAQUE)
			sprite(out, a, im, sh->x + cx, GROUND + sh->y + dy, sh->face < 0, OPAQUE, 255);
		else
			effect(out, a, im, sh->x + cx, GROUND + sh->y + dy, sh->face < 0, t->blend,
			       t->alpha, &budget);
	}
	/* Sparks over the fighters, the current step of each. */
	for (int k = 0; k < SPARK_SLOTS; k++) {
		const struct spark_slot *sp = &s->spark[k];
		if (!sp->kind)
			continue;
		const struct spark *an = &a->spark[sp->kind - 1];
		int32_t t = sp->age;
		unsigned j = an->first;
		while (t >= a->spark_step[j].ticks)
			t -= a->spark_step[j++].ticks;
		int im = a->spark_step[j].image;
		out->sparks++;
		if (an->blend == OPAQUE)
			sprite(out, a, im, sp->x + cx, sp->y + dy, sp->flip, OPAQUE, 255);
		else
			effect(out, a, im, sp->x + cx, sp->y + dy, sp->flip, an->blend, an->alpha,
			       &budget);
	}
	/* Drawn after the sprites so none hides the boxes. */
	for (int i = 0; s->boxes && i < 2; i++) {
		const struct fighter *p = &s->p[i];
		const struct step *st = &a->step[fight_pose(s, a, i)];
		for (unsigned k = 0; k < (unsigned)st->hurt + st->hit; k++)
			outline(out, fight_box(&a->box[st->box + k], px(p) + cx, p->y + dy, p->face),
				k < st->hurt ? 0x07e0 : 0xf800, k < st->hurt ? 0 : 96, 1);
		rect(out, px(p) - 3 + cx, GROUND + p->y + dy, 7, 1, 0xffff, OPAQUE, 255);
	}
	for (int k = 0; s->boxes && k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		if (!sh->kind)
			continue;
		const struct step *st = &a->step[shot_step(s, a, sh)];
		for (unsigned h = 0; h < st->hit; h++)
			outline(out, fight_box(&a->box[st->box + h], sh->x + cx, sh->y + dy, sh->face),
				0xf800, 96, 1);
	}
	/* Stress modes: 8, 24 or 48 overlapping animated alpha/additive effects. */
	int n = s->stress == 1 ? 8 : s->stress == 2 ? 24 : s->stress == 3 ? 48 : 0;
	for (int i = 0; i < n && a->effects; i++) {
		int x = (int)((s->tick * 3 + (unsigned)i * 71) % 560) - 80;
		int y = 200 + (i * 23) % 120;
		int image = a->effect[(s->tick / 4 + (unsigned)i) % a->effects];
		effect(out, a, image, x, y, i % 2, i % 3 ? ALPHA : ADD, 160, &budget);
	}
hud:
	rect(out, 16, 16, 204, 13, 0, OPAQUE, 255);
	rect(out, 260, 16, 204, 13, 0, OPAQUE, 255);
	/* Life bars are 200 pixels at full life, whatever the character's life. */
	const struct character *c0 = who(s, a, 0), *c1 = who(s, a, 1);
	rect(out, 18, 18, s->p[0].hp * 200 / c0->stats.life, 9, 0x07ed, OPAQUE, 255);
	rect(out, 262, 18, s->p[1].hp * 200 / c1->stats.life, 9, 0xfbe0, OPAQUE, 255);
	const char *n0 = c0->name, *n1 = c1->name;
	text(out, 16, 34, n0, 0xffff);
	text(out, 464 - text_width(n1), 34, n1, 0xffff);
	/* Under the mode indicator: the ladder's stage x/y, the survival wins,
	 * or PRACTICE with the dummy (R cycles it) and each side's last combo
	 * damage. */
	char line[24];
	if (s->game == GAME_SURVIVAL) {
		strcpy(line, "WINS ");
		number(line + 5, s->streak);
	} else if (s->game == GAME_PRACTICE) {
		strcpy(line, "PRACTICE");
		char dummy_line[24] = "R DUMMY ";
		strcat(dummy_line, dummy_names[s->dummy]);
		text(out, 240 - text_width(dummy_line) / 2, 48, dummy_line, 0x07ff);
		for (int i = 0; i < 2; i++) {
			if (!s->last[i])
				continue;
			char damage[24] = "DAMAGE ";
			number(damage + 7, s->last[i]);
			text(out, i ? 464 - text_width(damage) : 16, 76, damage, 0xffff);
		}
	} else {
		number(line, s->stage);
		size_t len = strlen(line);
		line[len++] = '/';
		number(line + len, count(opponents(a, s->chr[0])));
	}
	text(out, 240 - text_width(line) / 2, 34, line, game_colors[s->game]);
	if (s->automatic)
		text(out, 224, 48, "AUTO", 0x8410);
	/* Keep labels short: each glyph row run is a bounded command. */
	/* Combo counts under each attacker's name. */
	for (int i = 0; i < 2; i++) {
		if (!s->shown[i])
			continue;
		char hits[12];
		number(hits, s->shown[i]);
		strcat(hits, " HITS");
		int w = (4 * (int)strlen(hits) - 1) * 4, x = i ? 464 - w : 16;
		text_cell(out, x + 2, 52, hits, 0, 4);
		text_cell(out, x, 50, hits, 0xffe0, 4);
	}
	if (s->ended && !s->paused)
		text(out, 232, 80, "KO", 0xffff);
	/* Mode indicators don't depend on the text command budget. */
	rect(out, 220, 16, 40, 13, (uint16_t)(0x001f + s->stress * 0x3800), OPAQUE,
	     255);
	banner(out, s);
	if (s->paused)
		pause_panel(out, s, a);
}
/* ---- state words ---- */
#define FIGHTER_WORDS 31
#define SHOT_WORDS 6
#define GLOBAL_WORDS 19
#define SHOTS_AT (GLOBAL_WORDS + 2 * FIGHTER_WORDS)
#define TAIL_AT (SHOTS_AT + MAX_PROJECTILES * SHOT_WORDS)
/* Then arena, cam, shake, flash, the spark slots, the sound count and one
 * word per sound event (id | side << 8 | chr << 9 | system << 16 | move <<
 * 24). */
#define EXTRA_AT (TAIL_AT + 7)
#define SPARK_WORDS 5
#define SPARKS_AT (EXTRA_AT + 4)
#define SOUNDS_AT (SPARKS_AT + SPARK_SLOTS * SPARK_WORDS)
/* Then combo, shown, show and best per side, the select list flag, the
 * difficulty, the throw and tech counts and the run and backdash counts. */
#define COMBO_AT (SOUNDS_AT + 1 + MAX_SOUND_EVENTS)
#define LIST_AT (COMBO_AT + 8)
#define LEVEL_AT (LIST_AT + 1)
#define THROWS_AT (LEVEL_AT + 1)
#define DASHES_AT (THROWS_AT + 2)
/* Then game, picking, dummy, streak, record, carry and damage and last per
 * side. */
#define GAME_AT (DASHES_AT + 2)
#define GAME_WORDS 10
#define USED_WORDS (GAME_AT + GAME_WORDS)
static void fighter_fields(struct fighter *p, int32_t *v[FIGHTER_WORDS])
{
	int32_t *f[] = {&p->x,	   &p->y,     &p->vy,	     &p->hp,	&p->face,  &p->walk,
			&p->block, &p->crouch, &p->move,     &p->elapsed, &p->hit,   &p->stun,
			&p->freeze, &p->slide, &p->keys,     &p->corner, &p->squat, &p->buffer,
			&p->age,   &p->plan,   &p->plan_tick, &p->clock, &p->down,  &p->grab,
			&p->safe};
	for (int k = 0; k < 25; k++)
		v[k] = f[k];
	v[25] = (int32_t *)(void *)&p->charge;
	for (int k = 0; k < HISTORY / 8; k++)
		v[26 + k] = (int32_t *)(void *)&p->history[k];
	v[26 + HISTORY / 8] = &p->dash;
}
static void global_fields(struct fight_state *s, uint32_t *v[GLOBAL_WORDS])
{
	uint32_t *f[GLOBAL_WORDS] = {&s->tick,  &s->rng,    &s->previous, &s->paused, &s->stress,
				     &s->boxes, &s->automatic, &s->round, &s->mode,	&s->timer,
				     &s->beaten, &s->stage, &s->run,	  &s->wins[0], &s->wins[1],
				     &s->chr[0], &s->chr[1], &s->pal[0],   &s->pal[1]};
	memcpy(v, f, sizeof(f));
}
static void tail_fields(struct fight_state *s, uint32_t *v[7])
{
	uint32_t *f[7] = {&s->tone, &s->phase, &s->ended, &s->hits, &s->guards, &s->specials,
			  &s->clashes};
	memcpy(v, f, sizeof(f));
}
static void game_fields(struct fight_state *s, uint32_t *v[GAME_WORDS])
{
	uint32_t *f[GAME_WORDS] = {&s->game,	&s->picking, &s->dummy, &s->streak, &s->record,
				   &s->carry,	&s->damage[0], &s->damage[1], &s->last[0],
				   &s->last[1]};
	memcpy(v, f, sizeof(f));
}
void fight_encode(const struct fight_state *s, uint32_t w[STATE_WORDS])
{
	struct fight_state t = *s;
	uint32_t *g[GLOBAL_WORDS], *e[7];
	memset(w, 0, STATE_WORDS * sizeof(*w));
	global_fields(&t, g);
	for (int k = 0; k < GLOBAL_WORDS; k++)
		w[k] = *g[k];
	for (int i = 0; i < 2; i++) {
		int32_t *v[FIGHTER_WORDS];
		fighter_fields(&t.p[i], v);
		for (int k = 0; k < FIGHTER_WORDS; k++)
			w[GLOBAL_WORDS + i * FIGHTER_WORDS + k] = (uint32_t)*v[k];
	}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &t.shot[k];
		const int32_t v[SHOT_WORDS] = {sh->kind, sh->owner, sh->x, sh->y, sh->face, sh->age};
		for (int j = 0; j < SHOT_WORDS; j++)
			w[SHOTS_AT + k * SHOT_WORDS + j] = (uint32_t)v[j];
	}
	tail_fields(&t, e);
	for (int k = 0; k < 7; k++)
		w[TAIL_AT + k] = *e[k];
	w[EXTRA_AT] = t.arena;
	w[EXTRA_AT + 1] = t.cam;
	w[EXTRA_AT + 2] = t.shake;
	w[EXTRA_AT + 3] = t.flash;
	for (int k = 0; k < SPARK_SLOTS; k++) {
		const struct spark_slot *sp = &t.spark[k];
		const int32_t v[SPARK_WORDS] = {sp->kind, sp->x, sp->y, sp->age, sp->flip};
		for (int j = 0; j < SPARK_WORDS; j++)
			w[SPARKS_AT + k * SPARK_WORDS + j] = (uint32_t)v[j];
	}
	w[SOUNDS_AT] = t.sounds;
	for (unsigned k = 0; k < t.sounds && k < MAX_SOUND_EVENTS; k++) {
		const struct sound_event *e2 = &t.sound[k];
		w[SOUNDS_AT + 1 + k] = e2->id | (uint32_t)e2->side << 8 | (uint32_t)e2->chr << 9 |
				       (uint32_t)e2->system << 16 | (uint32_t)e2->move << 24;
	}
	for (int i = 0; i < 2; i++) {
		w[COMBO_AT + i] = t.combo[i];
		w[COMBO_AT + 2 + i] = t.shown[i];
		w[COMBO_AT + 4 + i] = t.show[i];
		w[COMBO_AT + 6 + i] = t.best[i];
	}
	w[LIST_AT] = t.list;
	w[LEVEL_AT] = t.level;
	w[THROWS_AT] = t.throws;
	w[THROWS_AT + 1] = t.techs;
	w[DASHES_AT] = t.runs;
	w[DASHES_AT + 1] = t.backdashes;
	uint32_t *g2[GAME_WORDS];
	game_fields(&t, g2);
	for (int k = 0; k < GAME_WORDS; k++)
		w[GAME_AT + k] = *g2[k];
}
/* Bounds come from the fighter's own character (life, jump velocities,
 * prejump, buffer, charge and grace) and the walls of the state's camera. */
static int valid(const struct fight_state *s, const struct fighter *p, const struct assets *a,
		 const struct character *c)
{
	const struct stats *st = &c->stats;
	int lo, hi;
	fight_walls(s, a, &lo, &hi);
	if (p->x < QUARTER(lo) || p->x > QUARTER(hi) || p->y < -MAX_HEIGHT ||
	    p->y > 0 || p->vy < st->jump.vy || p->vy > st->jump.fall || p->hp < 0 ||
	    p->hp > st->life ||
	    (p->face != 1 && p->face != -1) || p->walk < -1 || p->walk > 1 ||
	    p->block < 0 || p->block > 2 || p->crouch < 0 || p->crouch > 1 ||
	    (p->crouch && p->y) || p->move < 0 || p->move > (int)c->moves || p->elapsed < 0 ||
	    p->hit < 0 || p->stun < 0 || p->stun > MAX_STUN || p->freeze < 0 ||
	    p->freeze > MAX_HITSTOP || p->slide < -MAX_SLIDE || p->slide > MAX_SLIDE ||
	    p->keys < 0 || p->keys > 0xffff || p->corner < 0 || p->corner > 1 ||
	    (p->corner && !p->slide) || p->squat < 0 || p->squat > st->jump.prejump ||
	    p->buffer < 0 || p->buffer > (int)c->moves || p->age < 0 ||
	    p->age > st->buffer || (!p->buffer && p->age) || p->plan < 0 ||
	    p->plan > (int)c->moves || (p->plan && p->plan <= MOVES) || p->plan_tick < 0 ||
	    p->plan_tick > st->charge + 3 || (!p->plan && p->plan_tick) || p->clock < 0 ||
	    p->clock > 255)
		return 0;
	for (int k = 0; k < 2; k++) {
		unsigned held = charge_byte(p->charge, 2 * k), grace = charge_byte(p->charge, 2 * k + 1);
		if (grace > st->grace || (grace && held))
			return 0;
	}
	if (p->squat && (!grounded(p) || p->move || p->stun))
		return 0;
	/* Down: nothing else going on; lying ends within down_ticks. */
	if (p->down < UP || p->down >= DOWN_PHASES ||
	    (p->down && (p->move || p->stun || p->block || p->crouch || p->squat || p->walk)) ||
	    (p->down == LYING && (!grounded(p) || p->elapsed >= down_ticks(c))))
		return 0;
	/* A throw: nothing else going on, on the ground; the thrower is
	 * released before its THROW animation is over. */
	if (p->grab < GRAB_NONE || p->grab > GRAB_HELD || p->safe < 0 ||
	    p->safe > MAX_THROW_SAFE ||
	    (p->grab && (p->move || p->stun || p->block || p->crouch || p->squat || p->walk ||
			 p->down || !grounded(p) || p->freeze || p->buffer || p->slide)) ||
	    (throwing(p) && p->elapsed >= c->action[THROW].total))
		return 0;
	/* Running, skidding or backdashing: nothing else going on; a run on
	 * the ground within its phase, a backdash on its arc. */
	const struct dash_rule *d = &a->sys[c->system].dash;
	if (p->dash < DASH_NONE || p->dash >= DASH_PHASES ||
	    (p->dash && (p->move || p->stun || p->block || p->crouch || p->squat || p->walk ||
			 p->down || p->grab || p->vy)) ||
	    (p->dash == DASH_BACK &&
	     (p->elapsed >= back_total(d) || p->y != back_y(d, p->elapsed))) ||
	    (p->dash && p->dash != DASH_BACK && p->y) ||
	    (p->dash == DASH_COAST && p->elapsed >= d->run_grace) ||
	    (p->dash == DASH_STOP && p->elapsed >= d->run_stop))
		return 0;
	if (p->move) {
		const struct move *m = &c->move[p->move - 1];
		if (p->elapsed >= m->anim.total || p->hit > m->groups || p->stun || p->block ||
		    p->vy || p->y != height(a, m, (uint32_t)p->elapsed))
			return 0;
	} else if (p->elapsed > 255 || p->hit)
		return 0;
	return (p->block != 1 || p->stun) && (p->block != 2 || !p->stun);
}
int fight_decode(struct fight_state *s, const struct assets *a,
		 const uint32_t w[STATE_WORDS])
{
	struct fight_state t;
	memset(&t, 0, sizeof(t));
	uint32_t *g[GLOBAL_WORDS], *e[7];
	global_fields(&t, g);
	for (int k = 0; k < GLOBAL_WORDS; k++)
		*g[k] = w[k];
	tail_fields(&t, e);
	for (int k = 0; k < 7; k++)
		*e[k] = w[TAIL_AT + k];
	t.arena = w[EXTRA_AT];
	t.cam = w[EXTRA_AT + 1];
	t.shake = w[EXTRA_AT + 2];
	t.flash = w[EXTRA_AT + 3];
	for (int i = USED_WORDS; i < STATE_WORDS; i++)
		if (w[i])
			return 0;
	if (t.chr[0] >= a->characters || t.chr[1] >= a->characters ||
	    (a->stages ? t.arena >= a->stages : t.arena != 0))
		return 0;
	int width = fight_width(&t, a);
	if (t.cam > (uint32_t)(width - FW) || t.shake > SHAKE_KO || t.flash > FLASH_TICKS ||
	    (t.mode != MODE_FIGHT && (t.arena || t.cam || t.shake || t.flash)))
		return 0;
	for (int k = 0; k < SPARK_SLOTS; k++) {
		struct spark_slot *sp = &t.spark[k];
		const uint32_t *d = w + SPARKS_AT + k * SPARK_WORDS;
		*sp = (struct spark_slot){(int32_t)d[0], (int32_t)d[1], (int32_t)d[2], (int32_t)d[3],
					  (int32_t)d[4]};
		if (!sp->kind) {
			if (sp->x || sp->y || sp->age || sp->flip)
				return 0;
			continue;
		}
		if (t.mode != MODE_FIGHT || sp->kind < 0 || sp->kind > (int)a->sparks ||
		    sp->age < 0 || sp->age >= a->spark[sp->kind - 1].total || sp->flip < 0 ||
		    sp->flip > 1 || sp->x < -MAX_SIDE || sp->x > width + MAX_SIDE ||
		    sp->y < -MAX_SIDE || sp->y > FH + MAX_SIDE)
			return 0;
	}
	t.sounds = w[SOUNDS_AT];
	if (t.sounds > MAX_SOUND_EVENTS)
		return 0;
	for (unsigned k = 0; k < MAX_SOUND_EVENTS; k++) {
		uint32_t v = w[SOUNDS_AT + 1 + k];
		if (k >= t.sounds) {
			if (v)
				return 0;
			continue;
		}
		struct sound_event e2 = {(uint8_t)(v & 255), (uint8_t)(v >> 8 & 1),
					 (uint8_t)(v >> 9 & 127), (uint8_t)(v >> 16 & 255),
					 (uint8_t)(v >> 24)};
		if (e2.id >= SOUNDS || e2.chr >= a->characters ||
		    e2.system != a->chr[e2.chr].system || e2.move > a->chr[e2.chr].moves)
			return 0;
		t.sound[k] = e2;
	}
	/* Combos: a running one of two or more is on screen at full time, a
	 * shown count has time left, nothing outside a running round. */
	for (int i = 0; i < 2; i++) {
		t.combo[i] = w[COMBO_AT + i];
		t.shown[i] = w[COMBO_AT + 2 + i];
		t.show[i] = w[COMBO_AT + 4 + i];
		t.best[i] = w[COMBO_AT + 6 + i];
		if (t.best[i] > MAX_COMBO || t.combo[i] > t.best[i] || t.shown[i] > t.best[i] ||
		    t.shown[i] == 1 || t.show[i] > COMBO_TICKS || !t.shown[i] != !t.show[i] ||
		    (t.combo[i] >= 2 && (t.shown[i] != t.combo[i] || t.show[i] != COMBO_TICKS)) ||
		    ((t.mode != MODE_FIGHT || t.timer < ROUND_TICKS) && t.best[i]))
			return 0;
	}
	t.list = w[LIST_AT];
	t.level = w[LEVEL_AT];
	t.throws = w[THROWS_AT];
	t.techs = w[THROWS_AT + 1];
	t.runs = w[DASHES_AT];
	t.backdashes = w[DASHES_AT + 1];
	uint32_t *g2[GAME_WORDS];
	game_fields(&t, g2);
	for (int k = 0; k < GAME_WORDS; k++)
		*g2[k] = w[GAME_AT + k];
	/* The game mode: picking a dummy only on a practice select screen; a
	 * running combo's damage is its last; nothing outside a running round;
	 * carry only in a survival fight. */
	if (t.game >= GAMES || t.picking > 1 || t.dummy >= DUMMIES || t.record > MAX_STREAK ||
	    t.streak > t.record ||
	    (t.picking && (t.mode != MODE_SELECT || t.game != GAME_PRACTICE)) ||
	    (!!t.carry != (t.game == GAME_SURVIVAL && t.mode == MODE_FIGHT)) ||
	    t.carry > (uint32_t)a->chr[t.chr[0] < a->characters ? t.chr[0] : 0].stats.life)
		return 0;
	for (int i = 0; i < 2; i++)
		if (t.damage[i] > MAX_COMBO_DAMAGE || t.last[i] > MAX_COMBO_DAMAGE ||
		    (!t.combo[i] && t.damage[i]) || (t.combo[i] && t.damage[i] != t.last[i]) ||
		    ((t.mode != MODE_FIGHT || t.timer < ROUND_TICKS) && t.last[i]))
			return 0;
	if (t.list > 1 || t.level >= LEVELS)
		return 0;
	unsigned n = a->characters;
	uint32_t all = n >= 32 ? 0xffffffffu : (1u << n) - 1u;
	if (t.paused > 1 || t.stress > 3 || t.boxes > 1 || t.automatic > 1 || t.mode >= MODES ||
	    t.timer > 255 || (t.beaten & ~all) || t.stage > n || t.wins[0] >= ROUNDS_TO_WIN ||
	    t.wins[1] >= ROUNDS_TO_WIN || t.chr[0] >= n || t.chr[1] >= n || t.pal[0] > 1 ||
	    t.pal[1] > 1 || t.tone > 2400 || t.phase >= 48000 || t.ended > KO_TICKS ||
	    (t.ended && t.mode != MODE_FIGHT))
		return 0;
	if (t.mode == MODE_FIGHT) {
		uint32_t opp = opponents(a, t.chr[0]);
		if (t.timer > BANNER_END || (t.timer < ROUND_TICKS && (t.ended || t.shake || t.flash)))
			return 0;
		/* A mirror is always a valid fight (tests and versus set one up);
		 * practice takes any dummy, only the ladder beats anyone. */
		if (t.pal[0] || t.pal[1] != (t.chr[0] == t.chr[1]) ||
		    (t.game != GAME_PRACTICE && !((opp >> t.chr[1]) & 1) &&
		     t.chr[0] != t.chr[1]) ||
		    (t.game == GAME_ARCADE ? (t.beaten & ~opp) || ((t.beaten >> t.chr[1]) & 1) ||
						     t.stage != count(t.beaten) + 1
					   : t.beaten || t.stage != 1))
			return 0;
	}
	for (int i = 0; i < 2; i++) {
		int32_t *v[FIGHTER_WORDS];
		fighter_fields(&t.p[i], v);
		for (int k = 0; k < FIGHTER_WORDS; k++)
			*v[k] = (int32_t)w[GLOBAL_WORDS + i * FIGHTER_WORDS + k];
		if (!valid(&t, &t.p[i], a, &a->chr[t.chr[i]]))
			return 0;
	}
	for (int i = 0; i < 2; i++)
		if (t.combo[i] && !in_hitstun(&t.p[1 - i]))
			return 0;
	/* A survival fight starts from carry; practice never empties a bar. */
	if ((t.carry && t.p[0].hp > (int32_t)t.carry) ||
	    (t.game == GAME_PRACTICE && t.mode == MODE_FIGHT &&
	     (t.p[0].hp < 1 || t.p[1].hp < 1 || t.ended)))
		return 0;
	/* A thrower holds the other fighter, facing it, for as long. */
	for (int i = 0; i < 2; i++)
		if (throwing(&t.p[i]) != (t.p[1 - i].grab == GRAB_HELD) ||
		    (throwing(&t.p[i]) && (t.p[i].elapsed != t.p[1 - i].elapsed ||
					   t.p[i].face != -t.p[1 - i].face)))
			return 0;
	int owned[2] = {0, 0};
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		struct shot *sh = &t.shot[k];
		const uint32_t *d = w + SHOTS_AT + k * SHOT_WORDS;
		*sh = (struct shot){(int32_t)d[0], (int32_t)d[1], (int32_t)d[2],
				    (int32_t)d[3], (int32_t)d[4], (int32_t)d[5]};
		if (!sh->kind) {
			if (sh->owner || sh->x || sh->y || sh->face || sh->age)
				return 0;
			continue;
		}
		if (sh->owner < 0 || sh->owner > 1 || owned[sh->owner]++ || t.mode != MODE_FIGHT ||
		    sh->kind < 0 || sh->kind > (int)a->chr[t.chr[sh->owner]].projectiles ||
		    (sh->face != 1 && sh->face != -1) || sh->x < -SHOT_MARGIN - FW ||
		    sh->x > width + FW + SHOT_MARGIN || sh->y < -MAX_SIDE - MAX_HEIGHT ||
		    sh->y > MAX_SIDE ||
		    sh->age < 0 ||
		    sh->age >= a->chr[t.chr[sh->owner]].projectile[sh->kind - 1].lifetime)
			return 0;
	}
	/* Nothing moves during the versus card and READY. */
	for (int i = 0; t.mode == MODE_FIGHT && t.timer < ROUND_TICKS && i < 2; i++)
		if (t.p[i].move || t.p[i].stun || t.p[i].down || t.p[i].grab || t.p[i].buffer || t.p[i].dash ||
		    owned[i])
			return 0;
	*s = t;
	return 1;
}
