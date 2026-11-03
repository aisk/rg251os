#!/usr/bin/env python3
"""Build a FIGHTP09 content pack from ripped sprite sheets and their manifests.

Each manifest (assets/fighter/<name>.json) describes one sheet and is the
source of truth for it: every frame is a hand-checked sheet rectangle plus a
foot anchor, every action a list of (frame, ticks) steps. Pixels stay 1:1;
the sheet background is transparent and each frame is trimmed and stored as
per-row opaque spans of palette indices. Frames must have a background border
and may not touch the sheet's credit box or its white animation underlines.
Boxed sheets (kilburto's Spriters Resource exports: every frame in its own
background box, boxes separated by lines of a "separator" colour, which the
sprites may use too) have neither; a frame must lie inside one box, with
background or the box's edge around it.
A sheet may instead set "boxed" to the separator colour: then every frame
rectangle is one whole box, its border all separator or all background.
"mirror": true marks a sheet drawn facing left: frames are flipped as they
are read, so anchors, hurtboxes and hitboxes describe the right-facing frame
(the anchor is relative to the flipped rectangle).

A pack is built from a roster (assets/fighter/roster.json) or from a single
manifest, which then acts as a roster of its own characters:
  manifests   list of manifest paths relative to the roster; their characters
              appear in the select screen in this order
  effects     the manifest whose "effects" list the stress scenes draw
  stages      optional list of stage manifests (see below); without any the
              core draws its flat fallback background
  sparks      optional {id: spark manifest}; systems.json "effects" names
              their anims as "id.anim"
Either way systems.json next to the roster (or manifest) supplies the system
profiles; every profile goes into the pack (at most 8).
Subcommands:

  build MANIFEST|ROSTER OUTPUT [--auto]  write the pack (Auto: AI vs AI,
                                         rotating characters, scene cycle)
  detect MANIFEST                  print candidate frame boxes per sheet row
  preview MANIFEST OUTPUT.ppm      every action, move and projectile of the
                                   first character on its anchor, with
                                   hurtboxes (green) and hitboxes (red)
  data MANIFEST|ROSTER             each character's system and stats, the
                                   frame data table of every move and where
                                   its numbers come from (src)

Manifest schema (assets/fighter/terry.json is the reference; unknown
top-level keys such as "notes" are ignored):
  frames      name -> [x, y, w, h, anchor x, anchor y] in sheet pixels
  hurtboxes   frame -> [[x, y, w, h], ...] or the name of another frame whose
              boxes it shares. Boxes are frame pixels relative to the anchor
              (x forward for a right-facing sprite, y down, so heights above
              the feet are negative); the core mirrors them with facing.
              Every frame a character's actions and moves use needs an entry
              ([] = invulnerable); projectile frames need none.
  sources     optional: id -> {"url", "title", "fetched", "game"} (url
              required, the rest strings); "src" values may name these ids.
              Tools only, never packed.
  characters  name -> {"actions": {action: [steps]}, "moves": {move: {...}},
              "projectiles": {name: {...}}, "system": S, "stats": {...}}.
              The name, upper-cased with "_" as a space, is shown in the
              select screen and HUD (A-Z, 0-9, space and "-", at most 12
              characters). "system" names a profile in systems.json; without
              it the character uses the one systems.json "characters" assigns
              to its name, else kof2002 (giving both differently is an error).
              "stats" (optional) overrides the system's defaults: life
              (reference 1000), defense and weight (per mille: 1200 takes 20%
              more damage; weight divides the pushback received), walk and
              walk_back (quarter pixels per tick), jump {vy, gravity, vx,
              vx_back, prejump} (any subset; vy and gravity in quarter pixels
              per tick, vx/vx_back the air drift, vx_back follows vx unless
              given), prejump (same as jump.prejump), height (low, mid or high,
              AI and tools only) and src. hop, super_jump, hyper_hop, stun,
              guard and meters are accepted for later steps and not packed.
              A step is [frame, ticks] or [frame, ticks, {...}] with optional
              "hit" (hitboxes, moves and projectiles only), "hurt" (replaces
              the frame's hurtboxes), "move" ([forward, down] pixels per tick
              while the step shows, moves only) and "spawn" (a projectile
              name, launched on the step's first tick, moves only).
              Contiguous steps with hit boxes form one hit (a move may have
              several). A move's vertical motion must never take it below the
              ground and must end back on it.
              Actions: idle, walk_forward, walk_back, jump, guard, hurt, ko,
              crouch, crouch_guard, crouch_hurt, and optionally the
              knockdown's fall (knocked off the feet until landing), down
              (lying, loops) and getup; without them fall is the ko
              animation minus its last step, down that last step and getup
              the first crouch frame for 10 ticks.
              Moves: the eight normals (stand/crouch lp/hp/lk/hk) are fixed by
              name; any other name is a special. A special may have "name",
              shown in the command lists (A-Z, a-z, 0-9, space and "-", at
              most 16, upper-cased in the pack); without it the key is used
              the way character names are. Every special needs
              "trigger": {"motion": M, "buttons": B} with M one of 236, 214,
              623, [4]6, [2]8 (directions relative to facing; [d] = charge)
              and B "P", "K" or a list of lp/hp/lk/hk. Specials are checked
              in manifest order, the first match wins. A follow-up (the next
              stage of a rekka) adds "after": the special or normal it
              continues, "window": [open, close] (elapsed ticks of that move,
              0 <= open <= close < its length) and optional "contact" (true:
              only when that move hit or was guarded). Its motion may also be
              a direction held with the button: 5 (none), 8 (up), 2 (down), 6
              (forward), 4 (back) or "any". It never comes out on its own: a
              press while the parent move shows elapsed <= close is checked
              against the parent's follow-ups first, with the motion entered
              after the parent began, is buffered like any press and starts
              on the first unfrozen tick on which the parent has shown an
              elapsed in the window. Every move has
              "steps", guard (high = stand or crouch block, low = crouch block
              only, overhead = stand block only) and cancel (list of chain,
              special, super: chain and special are enforced, super is stored
              for later). Moves with hitboxes also need damage, chip, hitstun,
              blockstun, hitstop (ticks), pushback and block_pushback
              (pixels); moves without them may omit those. damage and chip
              are thousandths of the reference life 1000 as the original game
              has them; the core multiplies them by the system's damage_scale
              / 1000 at load (rounded, at least 1 when nonzero). Optional
              "class" (light, heavy, command, special, super, throw) lets
              hitstun/blockstun (stun_table) and hitstop (hitstop_table) come
              from the system when the move omits them; written values always
              win. Optional "src" maps a field name (or "steps", "*" for the
              rest) to where its value comes from: a "sources" id, estimate,
              sheet (from the sheet's frame counts), measured, design or
              mugen:<pack>; a field without one counts as estimate.
              Optional "knockdown" (true/false): an unguarded hit of the
              move's last hit group knocks the defender down. Without it the
              sweep (crouch_hk) and every special of class special or super
              with hitboxes that launches no projectile and has no
              follow-ups knock down. Optional "otg" (true): the move also hits
              a lying fighter (nothing else does).
              Projectiles: "steps" (looping animation, each step with "hit"
              boxes), "speed" (pixels per tick forward), "lifetime" (ticks),
              "offset" ([forward, down] from the owner's anchor at launch),
              "blend" (opaque, alpha or add), "alpha" and the same hit fields
              as moves (class, src and knockdown, default false, too).
              Blended projectiles share the
              effect pixel budget.
  players     two {"character", "palette"} slots: the palette of slot 1 is a
              character's normal palette, slot 2 its mirror-match alternate
              (characters not listed use the first and second palette)
  palettes    name -> {sheet RGB hex: replacement hex}
  effects     [frame, palette] pairs for the effect sprites
  home_stage  (in a character) optional stage name: the ladder fights this
              character on it; otherwise opponent c fights on stage
              c % stages
  slash       (in a character) optional list of lp/hp/lk/hk/special: the
              moves that are weapon strikes (slash sparks, blood and slash
              sounds); defaults to the system's "slash"

Spark manifest (assets/fighter/sparks-*.json): "sheet" (RGBA PNG, alpha 0 is
transparent), "sha256", optional "sources" and "notes", "frames" name -> [x,
y, w, h, anchor x, anchor y] and "anims" name -> {"steps": [[frame, ticks],
...], "blend" (opaque, alpha or add), "alpha" (1..255, blended only), "src"}.
Frames are trimmed to their opaque pixels; the anchor lands on the contact
point. One palette per sheet (at most 256 colours).

Stage manifest (assets/fighter/stage-*.json):
  name        shown in logs (A-Z, 0-9, space, -, at most 12 once upper-cased)
  system      the systems.json profile whose wall_margin and play_width the
              rules use on this stage
  width       world width W in pixels (480..2048); the camera's left edge
              moves in 0..W-480
  ground      the row in stage coordinates where feet stand; it is drawn on
              the core's ground line
  sheets      id -> {"file", "sha256", optional "background" RGB hex}: PNGs
              (RGB with a background key colour, or RGBA where alpha 0 is
              transparent)
  layers      back to front, at most 8: {"sheet", "rect" [x, y, w, h], "x"
              (screen x of the layer's left edge at camera 0), "y" (top in
              stage coordinates), "parallax" ("fit": the layer's two edges
              meet the screen's at the camera's two ends, i.e. it moves
              cam * (w - 480) / (W - 480); or [num, den]: moves
              cam * num / den), "edge" [horizontal, vertical] rules for
              screen pixels outside the layer (none: leave them to the layers
              behind, extend: repeat the outermost column/row, repeat: tile,
              color: fill with "color", mirror: reflect at the edge), optional
              "color" RGB hex, optional
              "anims"}. Each layer and its anims share one palette.
  anims       [{"steps": [[[x, y, w, h], ticks, dx, dy], ...], "diff": bool}]:
              sheet rectangles drawn over the layer with their top-left at
              (dx, dy) in layer pixels, cycling with the global tick. With
              "diff" each frame keeps only the pixels that differ from the
              layer, so only what changes is stored and drawn; where some
              frame is transparent over an opaque layer pixel, that pixel is
              taken out of the layer.
Stages are 1:1 pixels and scroll horizontally only.

Frame data printed by `data` (all in 60 Hz ticks): startup = ticks before
the first hitbox (the press tick is elapsed 0, so the first active tick is
startup ticks after the press), active = ticks from the first to the last
hitbox step, recovery = the rest. A hit on the first active tick gives the
attacker an advantage of hitstun - (active + recovery), blockstun likewise;
hitstop freezes both fighters equally and does not change it.

systems.json: {"systems": {name: profile}, "characters": {character:
system}, "notes": ...}. Profile names are a-z, 0-9 and "_" (at most 12). A
profile needs life, damage_scale (per mille, applied to its characters'
damage and chip), walk, walk_back, jump {vy, gravity, vx, prejump, optional
vx_back}, stun_table {class: [hitstun, blockstun]}, hitstop_table {class:
ticks} (classes may be left out; a move of such a class must give the
values itself), input {buffer, quarter (236/214 window), dragon (623),
charge, grace, optional release (default 4)}, push_distance (bodies closer
than this - 1 pixels are pushed apart to it) and guard_range (pixels).
Optional: defense, weight (1000), height (mid), wakeup (ticks from landing in
a knockdown until the fighter acts again, default 25), src, notes. hop, super_jump,
hyper_hop, chip, meters, mechanics, scaling, stun and guard are accepted for
later steps and not packed.

Pack layout, all little endian (the C loader in app/fighter/render.c checks it):
  header     "FIGHTP09", u32 size, u32 FNV-1a of bytes 16..size, u32 flags
             (bit 0 automatic), u32 palettes, frames, characters, effects,
             systems, sparks, stages
  palette    u16 sheet, u16 n (1..256), n x u16 RGB565
  frame      u16 sheet, u16 w, h; i16 anchor x, y; u32 spans; h x u16 spans
             per row; spans x (u16 x, u16 len); then sum(len) u8 indices
  spark      u8 blend, u8 alpha, u16 steps; steps x (u16 frame, u16 palette,
             u16 ticks)
  system     u8 name length, name bytes; u16 damage scale; 11 stats x u16
             (life, defense, walk, walk_back, i16 jump vy, gravity, vx,
             vx_back, prejump, weight, height 0 low 1 mid 2 high); u16 push
             distance, guard range, buffer, quarter, dragon, charge, grace,
             release; 6 classes (light, heavy, command, special, super,
             throw) x (u16 hitstun, blockstun, hitstop, has: bit 0 the stun
             pair, bit 1 hitstop; absent values 0); u16 wall margin, u16
             play width; 11 x u16 spark + 1 (0 none) for hit_light, hit_heavy,
             hit_special, guard, slash_light, slash_heavy, slash_special,
             slash_guard, blood_light, blood_heavy, blood_special; u16
             wakeup (ticks from landing in a knockdown to acting again)
  character  u8 name length, name bytes; u16 normal palette, alternate
             palette; u16 system, u16 override mask (bit per stat), 11 stats
             x u16 as above (0 where the mask bit is clear); u16 steps, boxes,
             moves, projectiles; 13 actions x (u16 first step, u16 steps);
             moves x 20 u16 (kind 0 normal or 1 special, trigger or motion
             (0 236, 1 214, 2 623, 3 [4]6, 4 [2]8, follow-ups also 5 any,
             6 5, 7 8, 8 2, 9 6, 10 4), buttons, first step, steps, damage,
             chip, hitstun, blockstun, hitstop, pushback, block_pushback,
             guard, cancel bits, class 0 none or 1 + class, defaults: bit 0
             hitstun, 1 blockstun, 2 hitstop from the class, packed as 0;
             after = parent move index + 1 or 0, window open, close, contact
             0 or 1, all 0 without a parent, flags: bit 0 knockdown, bit 1
             hits a lying fighter), u8 name length and the
             upper-case name (0 for normals, 1..16 for specials): the eight
             normals, then the specials in priority order; projectiles x 19 u16 (first step,
             steps, speed, lifetime, i16 offset x, i16 offset y, damage, chip,
             hitstun, blockstun, hitstop, pushback, block_pushback, guard,
             blend, alpha, class, defaults, flags: bit 0 knockdown); steps x (u16 frame, u16 ticks,
             u16 first box, u8 hurt boxes, u8 hit boxes, i8 forward, i8 down,
             u8 spawn = projectile + 1 or 0); boxes x (i16 x, i16 y, u16 w,
             u16 h); u16 slash mask (bit 0..3 lp, hp, lk, hk normals, bit 4
             specials), u16 home stage + 1 (0 none). Frame indices are
             global; step and box indices are local to the character. Damage
             is packed as written; src and sources are not packed.
  effects    u16 frame, u16 palette
  stage      u8 name length, name bytes; u16 width, ground, system, layers;
             layers x (u16 frame, palette, w, h; i16 x, y, num; u16 den; u8
             edge x, edge y (0 none, 1 extend, 2 repeat, 3 color, 4 mirror);
             u16 color
             RGB565; u16 anims; anims x (u16 steps; steps x (u16 frame or
             0xffff for nothing, i16 x, i16 y, u16 ticks))). A layer frame's
             anchor is minus its trimmed offset inside the layer rectangle;
             anim frames are drawn with their top-left at (x, y) - anchor.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

MAGIC = b'FIGHTP09'
# Order matters: these are the C enums in app/fighter/fighter.h.
ACTIONS = ['idle', 'walk_forward', 'walk_back', 'jump', 'guard', 'hurt', 'ko',
           'crouch', 'crouch_guard', 'crouch_hurt', 'fall', 'down', 'getup']
# Knockdown actions a manifest may leave out (see Character.knockdown_actions).
DERIVED_ACTIONS = ['fall', 'down', 'getup']
# Default get-up length (ticks of the crouch frame) when a character has no
# getup action.
GETUP_TICKS = 10
# Move flags (packed u16): knocks down on its last hit, hits a lying fighter.
FLAG_KNOCKDOWN, FLAG_OTG = 1, 2
MAX_WAKEUP = 120
MOVES = ['stand_lp', 'stand_hp', 'stand_lk', 'stand_hk',
         'crouch_lp', 'crouch_hp', 'crouch_lk', 'crouch_hk']
MOTIONS = ['236', '214', '623', '[4]6', '[2]8', 'any', '5', '8', '2', '6', '4']
# Motions from 'any' on are a direction held with the button: follow-ups only.
FOLLOW_ONLY = MOTIONS.index('any')
BUTTONS = ['lp', 'hp', 'lk', 'hk']
GUARDS = ['high', 'low', 'overhead']
CANCELS = ['chain', 'special', 'super']
BLENDS = ['opaque', 'alpha', 'add']
HIT_FIELDS = ['damage', 'chip', 'hitstun', 'blockstun', 'hitstop', 'pushback',
              'block_pushback']
NAME_CHARS = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -')
# Limits shared with fighter.h; the C loader rejects anything beyond them.
MAX_SIDE = 1024
MAX_STEP_BOXES = 15
MAX_MOVE_TICKS = 255
MAX_DAMAGE = 1500
MAX_STUN = 120
MAX_HITSTOP = 30
MAX_PUSH = 120
MAX_SPECIALS = 8
MAX_PROJECTILE_TYPES = 4
MAX_VELOCITY = 16
MAX_HEIGHT = 300
MAX_LIFETIME = 600
MAX_NAME = 12
MAX_MOVE_NAME = 16
MAX_CHARACTERS = 32
MAX_SYSTEMS = 8
MAX_LIFE = 4000
MIN_DEFENSE = 100
MIN_WEIGHT = 250
MIN_SCALE = 100
MAX_PERMILLE = 4000
MAX_WALK = 64
MAX_JUMP_VY = 255
MAX_GRAVITY = 32
MAX_PREJUMP = 15
MAX_BUFFER = 15
MAX_CHARGE = 200
MAX_GRACE = 60
MAX_PUSH_DISTANCE = 200
HISTORY = 32
SCREEN_WIDTH = 480
MAX_PALETTES = 64
MAX_FRAMES = 4096
EFFECT_KEYS = ['hit_light', 'hit_heavy', 'hit_special', 'guard', 'slash_light', 'slash_heavy',
               'slash_special', 'slash_guard', 'blood_light', 'blood_heavy', 'blood_special']
SLASH_KEYS = ['lp', 'hp', 'lk', 'hk', 'special']
EDGES = ['none', 'extend', 'repeat', 'color', 'mirror']
MAX_SPARKS = 32
MAX_SPARK_STEPS = 64
MAX_STAGES = 16
MAX_LAYERS = 8
MAX_LAYER_ANIMS = 8
MAX_ANIM_STEPS = 64
MIN_STAGE_WIDTH = SCREEN_WIDTH
MAX_STAGE_WIDTH = 2048
MIN_PLAY_WIDTH = 320
MIN_MARGIN = 8
MAX_MARGIN = 48
DEFAULT_MARGIN = 24
DEFAULT_SYSTEM = 'kof2002'
CLASSES = ['light', 'heavy', 'command', 'special', 'super', 'throw']
HEIGHTS = ['low', 'mid', 'high']
# Packed stat order (the character override mask has one bit per entry).
STATS = ['life', 'defense', 'walk', 'walk_back', 'jump_vy', 'gravity', 'vx', 'vx_back',
         'prejump', 'weight', 'height']
JUMP_KEYS = {'vy': 'jump_vy', 'gravity': 'gravity', 'vx': 'vx', 'vx_back': 'vx_back',
             'prejump': 'prejump'}
INPUT_KEYS = ['buffer', 'quarter', 'dragon', 'charge', 'grace', 'release']
# Accepted now, used by later steps of docs/fighter-stats.md, never packed.
LATER_SYSTEM_KEYS = {'hop', 'super_jump', 'hyper_hop', 'chip', 'meters', 'mechanics',
                     'scaling', 'stun', 'guard'}
LATER_STAT_KEYS = {'hop', 'super_jump', 'hyper_hop', 'stun', 'guard', 'meters'}
# src values besides "sources" ids and mugen:<pack>.
SOURCE_KINDS = ['estimate', 'sheet', 'measured', 'design']
# `data` warns when more than this share (percent) of a character's move
# fields are estimates.
ESTIMATE_WARNING = 50


def read_png(path, alpha=False):
    """Decode a non-interlaced 8-bit RGB or RGBA PNG, or a palette PNG of 1, 2,
    4 or 8 bits, to (w, h, RGB rows); with alpha, RGBA rows are returned as they are (RGB and palette rows
    get alpha 255; a palette image's tRNS is ignored)."""
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat, header, plte = 8, [], None, None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'PLTE':
            plte = body
        pos += 12 + n
    w, h, depth, color, _, _, interlace = header
    if (depth != 8 and not (color == 3 and depth in (1, 2, 4)) or color not in (2, 3, 6) or
            interlace or (color == 3 and plte is None)):
        raise ValueError(f'{path}: only non-interlaced 8-bit RGB/RGBA or 1/2/4/8-bit palette is supported')
    bpp = {2: 3, 3: 1, 6: 4}[color]
    stride = (w * depth + 7) // 8 if color == 3 else w * bpp
    raw = zlib.decompress(b''.join(idat))
    if len(raw) != h * (stride + 1):
        raise ValueError(f'{path}: truncated image data')
    rows, prev = [], bytearray(stride)
    for y in range(h):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif kind == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif kind == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif kind == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif kind:
            raise ValueError(f'{path}: bad filter {kind} on row {y}')
        prev = line
        if color == 3:
            if depth < 8:
                per = 8 // depth
                line = [b >> (8 - depth * (k + 1)) & (1 << depth) - 1
                        for b in line for k in range(per)][:w]
            if max(line) * 3 + 3 > len(plte):
                raise ValueError(f'{path}: palette index out of range on row {y}')
            line = b''.join(plte[i * 3:i * 3 + 3] for i in line)
            if alpha:
                line = b''.join(line[i:i + 3] + b'\xff' for i in range(0, w * 3, 3))
            rows.append(line)
        elif alpha:
            rows.append(bytes(line) if bpp == 4 else bytes(
                b for i in range(0, stride, 3) for b in line[i:i + 3] + b'\xff'))
        else:
            rows.append(bytes(line) if bpp == 3 else bytes(
                b for i in range(0, stride, 4) for b in line[i:i + 3]))
    return w, h, rows


class Sheet:
    def __init__(self, manifest_path):
        self.manifest_path = pathlib.Path(manifest_path)
        self.m = json.loads(self.manifest_path.read_text())
        path = self.manifest_path.parent / self.m['sheet']
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != self.m['sha256']:
            raise ValueError(f'{path}: SHA256 {digest} does not match the manifest')
        self.w, self.h, self.rows = read_png(path)
        self.bg = bytes.fromhex(self.m['background'])
        self.mirror = self.m.get('mirror', False) is True
        sep = self.m.get('separator')
        self.sep = bytes.fromhex(sep) if sep else None
        # "boxed" is the stricter form: every frame is one whole box, so its
        # border is all separator (or all background).
        self.boxed = bytes.fromhex(self.m['boxed']) if 'boxed' in self.m else None
        if self.sep and self.boxed:
            raise ValueError(f'{path}: use either separator or boxed')
        if self.boxed == self.bg:
            raise ValueError(f'{path}: the box separator is the background colour')
        if self.boxed:
            self.credit, self.underlines = (0, 0), []
        elif self.sep:
            # Boxed sheet: no credit box and no underlines.
            self.credit, self.underlines = (0, 0), []
            self.boxes = box_rects(self)
        else:
            self.bands = self._bands()
            # The credit text box is the sheet's first band; nothing may come from it.
            self.credit = self.bands[0]
            self.underlines = self._underlines()

    def px(self, x, y):
        return self.rows[y][x * 3:x * 3 + 3]

    def _bands(self):
        empty = self.bg * self.w
        bands, y = [], 0
        while y < self.h:
            if self.rows[y] != empty:
                top = y
                while y < self.h and self.rows[y] != empty:
                    y += 1
                bands.append((top, y))
            y += 1
        return bands

    def _underlines(self):
        """Thin single-colour runs (>= 20 px) with background above and below."""
        found = []
        for y in range(1, self.h - 1):
            x = 0
            while x < self.w:
                c = self.px(x, y)
                start = x
                while (x < self.w and self.px(x, y) == c and c != self.bg and
                       self.px(x, y - 1) == self.bg and self.px(x, y + 1) == self.bg):
                    x += 1
                if x - start >= 20:
                    found.append((start, y, x))
                x = max(x, start + 1)
        return found

    def segments(self, top, bottom):
        cols = []
        for x in range(self.w):
            cols.append(any(self.px(x, y) != self.bg for y in range(top, bottom)))
        out, x = [], 0
        while x < self.w:
            if cols[x]:
                a = x
                while x < self.w and cols[x]:
                    x += 1
                out.append((a, x))
            x += 1
        return out

    def frame(self, name):
        x, y, w, h, ax, ay = self.m['frames'][name]
        if x < 1 or y < 1 or x + w >= self.w or y + h >= self.h or w < 1 or h < 1:
            raise ValueError(f'{name}: rectangle outside the sheet')
        ct, cb = self.credit
        if y < cb and y + h > ct:
            raise ValueError(f'{name}: overlaps the credit box rows {ct}..{cb}')
        for ux0, uy, ux1 in self.underlines:
            if y <= uy < y + h and x < ux1 and ux0 < x + w:
                raise ValueError(f'{name}: contains the underline at y={uy} x={ux0}..{ux1}')
        # A background border proves the rectangle neither cuts a sprite nor
        # reaches into its neighbour. On a boxed sheet the rectangle must lie
        # in one box and the border is background or that box's edge.
        inbox = lambda xx, yy: True
        if self.boxed:
            ring = {self.px(xx, yy) for xx in range(x - 1, x + w + 1) for yy in (y - 1, y + h)}
            ring |= {self.px(xx, yy) for xx in (x - 1, x + w) for yy in range(y - 1, y + h + 1)}
            if ring not in ({self.bg}, {self.boxed}):
                raise ValueError(f'{name}: no background border and not one whole box')
            inbox = lambda xx, yy: False
        elif self.sep:
            box = next((b for b in self.boxes if b[0] <= x and b[1] <= y and
                        x + w <= b[0] + b[2] and y + h <= b[1] + b[3]), None)
            if box is None:
                raise ValueError(f'{name}: not inside one box')
            inbox = lambda xx, yy: (box[0] <= xx < box[0] + box[2] and
                                    box[1] <= yy < box[1] + box[3])
        for xx in range(x - 1, x + w + 1):
            for yy in (y - 1, y + h):
                if inbox(xx, yy) and self.px(xx, yy) != self.bg:
                    raise ValueError(f'{name}: no background border above/below')
        for yy in range(y - 1, y + h + 1):
            for xx in (x - 1, x + w):
                if inbox(xx, yy) and self.px(xx, yy) != self.bg:
                    raise ValueError(f'{name}: no background border left/right')
        pixels = [[None if self.px(x + i, y + j) == self.bg else self.px(x + i, y + j)
                   for i in range(w)] for j in range(h)]
        if self.mirror:
            pixels = [row[::-1] for row in pixels]
        used_rows = [j for j in range(h) if any(p is not None for p in pixels[j])]
        used_cols = [i for i in range(w) if any(pixels[j][i] is not None for j in range(h))]
        if not used_rows:
            raise ValueError(f'{name}: empty frame')
        t, b, l, r = used_rows[0], used_rows[-1] + 1, used_cols[0], used_cols[-1] + 1
        return [row[l:r] for row in pixels[t:b]], ax - l, ay - t


def rgb565(c):
    return (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3


def check_boxes(boxes, what):
    if not isinstance(boxes, list) or len(boxes) > MAX_STEP_BOXES:
        raise ValueError(f'{what}: box list must have at most {MAX_STEP_BOXES} boxes')
    for b in boxes:
        if (not isinstance(b, list) or len(b) != 4 or not all(isinstance(v, int) for v in b) or
                not -MAX_SIDE <= b[0] <= MAX_SIDE or not -MAX_SIDE <= b[1] <= MAX_SIDE or
                not 1 <= b[2] <= MAX_SIDE or not 1 <= b[3] <= MAX_SIDE):
            raise ValueError(f'{what}: bad box {b}')
    return [tuple(b) for b in boxes]


def check_int(v, low, high, what):
    if not isinstance(v, int) or isinstance(v, bool) or not low <= v <= high:
        raise ValueError(f'{what} must be an integer in {low}..{high}')
    return v


def scale(v, permille):
    """v * permille / 1000 rounded half up, at least 1 when v is nonzero (as
    the core does)."""
    r = (v * permille + 500) // 1000
    return 1 if v and not r else r


class Hit:
    """The hit fields of a move or projectile. packed: the 7 values as
    written (0 where the class supplies them); cls: 0 or 1 + class index;
    defaults: bits of hitstun/blockstun/hitstop taken from the class;
    resolved: what the core ends up with (class values filled, damage and
    chip scaled by the system)."""

    def __init__(self, d, what, required, system):
        cls = d.get('class')
        if cls is not None and cls not in CLASSES:
            raise ValueError(f'{what}: class must be one of {CLASSES}')
        self.cls = CLASSES.index(cls) + 1 if cls else 0
        row = system['classes'].get(cls, {})
        self.packed, self.resolved, self.defaults = [], [], 0
        for k in HIT_FIELDS:
            bit = {'hitstun': 1, 'blockstun': 2, 'hitstop': 4}.get(k, 0)
            if k not in d and k in row:
                self.defaults |= bit
                self.packed.append(0)
                self.resolved.append(row[k])
                continue
            if k not in d:
                if required:
                    raise ValueError(f'{what}: needs {k}' + (
                        f' (class {cls} has no default in {system["name"]})' if bit and cls
                        else ''))
                v = 1 if k in ('hitstun', 'blockstun') else 0
            else:
                top = {'damage': MAX_DAMAGE, 'chip': d.get('damage', 0), 'hitstun': MAX_STUN,
                       'blockstun': MAX_STUN, 'hitstop': MAX_HITSTOP, 'pushback': MAX_PUSH,
                       'block_pushback': MAX_PUSH}[k]
                v = check_int(d[k], 1 if k in ('hitstun', 'blockstun') else 0, top,
                              f'{what}: {k}')
            self.packed.append(v)
            self.resolved.append(v)
        for i in range(2):
            self.resolved[i] = scale(self.resolved[i], system['damage_scale'])

    def tail(self):
        return [self.cls, self.defaults]


def check_src(src, what, keys, sources):
    """A src map: field name (or "*") -> a sources id, a SOURCE_KINDS word or
    mugen:<pack>."""
    if not isinstance(src, dict):
        raise ValueError(f'{what}: src is {{field: source}}')
    for k, v in src.items():
        if k != '*' and k not in keys:
            raise ValueError(f'{what}: src names unknown field {k}')
        if not isinstance(v, str) or not (v in sources or v in SOURCE_KINDS or (
                v.startswith('mugen:') and len(v) > 6)):
            raise ValueError(f'{what}: src {k} must be a sources id, one of {SOURCE_KINDS} '
                             f'or mugen:<pack>, not {v!r}')
    return src


def source_kind(v, sources):
    return 'source' if v in sources else 'mugen' if v.startswith('mugen:') else v


def check_sources(m, what):
    sources = m.get('sources', {})
    if not isinstance(sources, dict):
        raise ValueError(f'{what}: sources is {{id: {{url, ...}}}}')
    for k, v in sources.items():
        if (k in SOURCE_KINDS or k.startswith('mugen:') or not isinstance(v, dict) or
                not isinstance(v.get('url'), str) or
                set(v) - {'url', 'title', 'fetched', 'game'} or
                not all(isinstance(x, str) for x in v.values())):
            raise ValueError(f'{what}: source {k} needs a url and only string title, '
                             f'fetched and game')
    return sources


def check_jump(j, what, partial):
    """A jump row; partial rows (character overrides) may give any subset."""
    if not isinstance(j, dict) or set(j) - set(JUMP_KEYS):
        raise ValueError(f'{what}: jump is {{{", ".join(JUMP_KEYS)}}}')
    need = set() if partial else {'vy', 'gravity', 'vx', 'prejump'}
    if need - set(j):
        raise ValueError(f'{what}: jump needs {sorted(need)}')
    bounds = {'vy': (-MAX_JUMP_VY, -1), 'gravity': (1, MAX_GRAVITY), 'vx': (0, MAX_WALK),
              'vx_back': (0, MAX_WALK), 'prejump': (1, MAX_PREJUMP)}
    out = {}
    for k, v in j.items():
        out[JUMP_KEYS[k]] = check_int(v, *bounds[k], f'{what}: jump {k}')
    if 'vx' in out and 'vx_back' not in out:
        out['vx_back'] = out['vx']
    return out


def jump_arc(vy, gravity):
    """(peak pixels, airborne ticks) of a jump, as the core moves it."""
    y, peak, ticks = 0, 0, 0
    while True:
        y += int(vy / 4)
        vy += gravity
        ticks += 1
        if y >= 0:
            return -peak, ticks
        peak = min(peak, y)


def stat_values(d, what, partial):
    """Stats from a profile or a character's "stats" as {STATS name: value}."""
    out = {}
    bounds = {'life': (1, MAX_LIFE), 'defense': (MIN_DEFENSE, MAX_PERMILLE),
              'weight': (MIN_WEIGHT, MAX_PERMILLE), 'walk': (0, MAX_WALK),
              'walk_back': (0, MAX_WALK)}
    for k, (low, high) in bounds.items():
        if k in d:
            out[k] = check_int(d[k], low, high, f'{what}: {k}')
    if 'height' in d:
        if d['height'] not in HEIGHTS:
            raise ValueError(f'{what}: height must be one of {HEIGHTS}')
        out['height'] = HEIGHTS.index(d['height'])
    if 'jump' in d:
        out.update(check_jump(d['jump'], what, partial))
    if 'prejump' in d:
        v = check_int(d['prejump'], 1, MAX_PREJUMP, f'{what}: prejump')
        if out.get('prejump', v) != v:
            raise ValueError(f'{what}: prejump and jump.prejump differ')
        out['prejump'] = v
    for k in ('hop', 'super_jump', 'hyper_hop'):
        if k in d:
            check_jump(d[k], f'{what} {k}', partial)
    for k in ('stun', 'guard'):
        if k in d:
            check_int(d[k], 0, 10000, f'{what}: {k}')
    if 'meters' in d and (not isinstance(d['meters'], list) or len(d['meters']) > 2 or
                          not all(isinstance(x, dict) and isinstance(x.get('name'), str)
                                  for x in d['meters'])):
        raise ValueError(f'{what}: meters is a list of at most two {{"name", ...}}')
    return out


def check_stats(st, what):
    """The resolved stats must also satisfy the core's jump bound."""
    peak, _ = jump_arc(st['jump_vy'], st['gravity'])
    if peak > MAX_HEIGHT:
        raise ValueError(f'{what}: the jump rises {peak} px, at most {MAX_HEIGHT}')
    return st


class Systems:
    """systems.json: the profiles in file order (all packed) and the
    character -> system assignments."""

    def __init__(self, path):
        path = pathlib.Path(path)
        if not path.exists():
            raise ValueError(f'{path}: no systems.json next to the roster or manifest')
        data = json.loads(path.read_text())
        if not isinstance(data, dict) or set(data) - {'systems', 'characters', 'notes'} or \
                not isinstance(data.get('systems'), dict) or not data['systems']:
            raise ValueError(f'{path}: needs "systems" (non-empty) and optional "characters", '
                             f'"notes"')
        if len(data['systems']) > MAX_SYSTEMS:
            raise ValueError(f'{path}: at most {MAX_SYSTEMS} systems')
        self.names = list(data['systems'])
        self.profile = {n: self._profile(n, p, path) for n, p in data['systems'].items()}
        self.assign = data.get('characters', {})
        if not isinstance(self.assign, dict) or not all(
                v in self.profile for v in self.assign.values()):
            raise ValueError(f'{path}: "characters" maps names to systems it defines')
        if DEFAULT_SYSTEM not in self.profile:
            raise ValueError(f'{path}: the default system {DEFAULT_SYSTEM} is missing')

    @staticmethod
    def _profile(name, p, path):
        what = f'{path.name} {name}'
        if not (1 <= len(name) <= MAX_NAME and set(name) <= set('abcdefghijklmnopqrstuvwxyz0123456789_')):
            raise ValueError(f'{what}: names are 1..{MAX_NAME} of a-z, 0-9 and _')
        need = {'life', 'damage_scale', 'walk', 'walk_back', 'jump', 'stun_table',
                'hitstop_table', 'input', 'push_distance', 'guard_range'}
        known = need | {'defense', 'weight', 'height', 'src', 'notes', 'wall_margin',
                        'play_width', 'slash', 'effects', 'wakeup'} | LATER_SYSTEM_KEYS
        if not isinstance(p, dict) or need - set(p) or set(p) - known:
            raise ValueError(f'{what}: needs {sorted(need)}, may have '
                             f'{sorted(known - need)}')
        st = {'defense': 1000, 'weight': 1000, 'height': HEIGHTS.index('mid')}
        st.update(stat_values(p, what, False))
        check_stats(st, what)
        if p.get('hop') is not None:
            check_jump(p['hop'], f'{what} hop', False)
        inp = p['input']
        if not isinstance(inp, dict) or set(inp) - set(INPUT_KEYS) or \
                set(INPUT_KEYS[:5]) - set(inp):
            raise ValueError(f'{what}: input is {{{", ".join(INPUT_KEYS)}}} (release optional)')
        bounds = {'buffer': (1, MAX_BUFFER), 'quarter': (3, HISTORY), 'dragon': (3, HISTORY),
                  'charge': (1, MAX_CHARGE), 'grace': (0, MAX_GRACE), 'release': (1, HISTORY)}
        inputs = [check_int(inp.get(k, 4), *bounds[k], f'{what}: input {k}') for k in INPUT_KEYS]
        classes = {}
        for table, keys in (('stun_table', ('hitstun', 'blockstun')),
                            ('hitstop_table', ('hitstop',))):
            t = p[table]
            if not isinstance(t, dict) or set(t) - set(CLASSES):
                raise ValueError(f'{what}: {table} keys must be among {CLASSES}')
            for cls, v in t.items():
                v = v if isinstance(v, list) else [v]
                if len(v) != len(keys):
                    raise ValueError(f'{what}: {table} {cls} needs {len(keys)} values')
                for k, x in zip(keys, v):
                    low, high = (1, MAX_STUN) if k != 'hitstop' else (0, MAX_HITSTOP)
                    classes.setdefault(cls, {})[k] = check_int(x, low, high,
                                                               f'{what}: {table} {cls}')
        if 'src' in p:
            check_src(p['src'], what, set(p) - {'src', 'notes'}, {})
        effects = p.get('effects', {})
        if not isinstance(effects, dict) or set(effects) - set(EFFECT_KEYS) or not all(
                isinstance(v, str) and v.count('.') == 1 for v in effects.values()):
            raise ValueError(f'{what}: effects is {{key: "sparks_id.anim"}} with keys among '
                             f'{EFFECT_KEYS}')
        return {'name': name,
                'margin': check_int(p.get('wall_margin', DEFAULT_MARGIN), MIN_MARGIN, MAX_MARGIN,
                                    f'{what}: wall_margin'),
                'play_width': check_int(p.get('play_width', SCREEN_WIDTH), MIN_PLAY_WIDTH,
                                        SCREEN_WIDTH, f'{what}: play_width'),
                'slash': slash_mask(p.get('slash', []), what),
                'effects': effects,
                'damage_scale': check_int(p['damage_scale'], MIN_SCALE, MAX_PERMILLE,
                                          f'{what}: damage_scale'),
                'stats': st, 'inputs': inputs,
                'push': check_int(p['push_distance'], 2, MAX_PUSH_DISTANCE,
                                  f'{what}: push_distance'),
                'guard_range': check_int(p['guard_range'], 0, SCREEN_WIDTH,
                                         f'{what}: guard_range'),
                'wakeup': check_int(p.get('wakeup', 25), 1, MAX_WAKEUP, f'{what}: wakeup'),
                'classes': classes}

    def of(self, character, written):
        """The system a character uses: its own "system", else the assigned
        one, else the default; the two may not disagree."""
        mapped = self.assign.get(character)
        if written is not None and written not in self.profile:
            raise ValueError(f'{character}: unknown system {written}')
        if written and mapped and written != mapped:
            raise ValueError(f'{character}: system {written} but systems.json assigns {mapped}')
        return self.profile[written or mapped or DEFAULT_SYSTEM]

    def pack(self, sparks=None):
        """The system records; sparks maps "id.anim" to its pack index (every
        name an effect table uses must be in it)."""
        sparks = sparks or {}
        out = b''
        for n in self.names:
            p = self.profile[n]
            out += struct.pack('<B', len(n)) + n.encode() + struct.pack('<H', p['damage_scale'])
            out += b''.join(struct.pack('<H', p['stats'][k] & 0xffff) for k in STATS)
            out += struct.pack('<8H', p['push'], p['guard_range'], *p['inputs'])
            for cls in CLASSES:
                row = p['classes'].get(cls, {})
                has = ('hitstun' in row) | ('hitstop' in row) << 1
                out += struct.pack('<4H', row.get('hitstun', 0), row.get('blockstun', 0),
                                   row.get('hitstop', 0), has)
            out += struct.pack('<2H', p['margin'], p['play_width'])
            for k in EFFECT_KEYS:
                ref = p['effects'].get(k)
                if ref is not None and ref not in sparks:
                    raise ValueError(f'systems.json {n}: effect {k} names unknown spark anim {ref}')
                out += struct.pack('<H', sparks[ref] + 1 if ref is not None else 0)
            out += struct.pack('<H', p['wakeup'])
        return out


def slash_mask(v, what):
    """Weapon moves: bits 0..3 the lp/hp/lk/hk normals, bit 4 the specials."""
    if not isinstance(v, list) or not set(v) <= set(SLASH_KEYS):
        raise ValueError(f'{what}: slash is a list of {SLASH_KEYS}')
    return sum(1 << SLASH_KEYS.index(k) for k in set(v))


def button_mask(b, what):
    if b == 'P':
        return 3
    if b == 'K':
        return 12
    if isinstance(b, list) and b and set(b) <= set(BUTTONS):
        return sum(1 << BUTTONS.index(x) for x in set(b))
    raise ValueError(f'{what}: buttons must be "P", "K" or a list of {BUTTONS}')


def display_name(name):
    text = name.upper().replace('_', ' ')
    if not 1 <= len(text) <= MAX_NAME or not set(text) <= NAME_CHARS:
        raise ValueError(f'{name}: character names must be 1..{MAX_NAME} of A-Z, 0-9, _ and -')
    return text


def flag(d, key, default, what):
    v = d.get(key, default)
    if not isinstance(v, bool):
        raise ValueError(f'{what}: {key} is true or false')
    return int(v)


def move_name(name, key, what):
    """The command-list name of a special: its "name", else its key."""
    if name is None:
        name = key.replace('_', ' ')
    if not isinstance(name, str):
        raise ValueError(f'{what}: name is a string')
    text = name.upper()
    if not 1 <= len(text) <= MAX_MOVE_NAME or not set(text) <= NAME_CHARS or \
            text != text.strip():
        raise ValueError(f'{what}: move names are 1..{MAX_MOVE_NAME} of A-Z, a-z, 0-9, '
                         f'space and -')
    return text


MOVE_SRC_KEYS = set(HIT_FIELDS) | {'steps', 'class', 'guard', 'cancel', 'trigger', 'speed',
                                   'lifetime', 'offset', 'blend', 'alpha'}
STAT_SRC_KEYS = {'life', 'defense', 'walk', 'walk_back', 'jump', 'prejump', 'weight', 'height'}


class Character:
    """One character's system, stats, steps, boxes, moves and projectiles
    from a manifest; frame indices are global (base + position in the
    manifest's frames)."""

    def __init__(self, m, name, frame_names, systems, base=0):
        ch = m['characters'][name]
        self.name = name
        self.display = display_name(name)
        self.steps, self.boxes, self.actions = [], [], []
        self.moves, self.move_names, self.projectiles, self.projectile_names = [], [], [], []
        # The upper-case command-list name of each move ('' for normals).
        self.labels = []
        # Per move: None or (parent name, [open, close], contact).
        self.follow = []
        self.hits, self.projectile_hits = [], []
        hurt = m['hurtboxes']
        index = {f: base + i for i, f in enumerate(frame_names)}
        known = {'actions', 'moves', 'projectiles', 'system', 'stats', 'home_stage', 'slash'}
        if set(ch) - known:
            raise ValueError(f'{name}: unknown keys {sorted(set(ch) - known)}')
        sources = check_sources(m, name)
        self.system = systems.of(name, ch.get('system'))
        self.system_index = systems.names.index(self.system['name'])
        self.slash = slash_mask(ch['slash'], name) if 'slash' in ch else self.system['slash']
        self.home_stage = ch.get('home_stage')
        if self.home_stage is not None and not isinstance(self.home_stage, str):
            raise ValueError(f'{name}: home_stage is a stage name')
        # Index + 1 of the home stage in the pack, set by build().
        self.home_index = 0
        given = ch.get('stats', {})
        if not isinstance(given, dict) or set(given) - STAT_SRC_KEYS - LATER_STAT_KEYS - {'src'}:
            raise ValueError(f'{name}: stats may have {sorted(STAT_SRC_KEYS | LATER_STAT_KEYS)} '
                             f'and src')
        self.overrides = stat_values(given, f'{name} stats', True)
        self.stats = check_stats({**self.system['stats'], **self.overrides}, f'{name} stats')
        if 'src' in given:
            check_src(given['src'], f'{name} stats', STAT_SRC_KEYS, sources)
        # Where each move field's value comes from: kind -> count.
        self.sources = {}

        def count_sources(d, what, hit):
            src = check_src(d.get('src', {}), what, MOVE_SRC_KEYS, sources)
            for k in HIT_FIELDS + ['steps']:
                if k in d:
                    kind = source_kind(src.get(k, src.get('*', 'estimate')), sources)
                elif hit.defaults & {'hitstun': 1, 'blockstun': 2, 'hitstop': 4}.get(k, 0):
                    kind = 'system'
                else:
                    continue
                self.sources[kind] = self.sources.get(kind, 0) + 1

        def frame_hurt(f):
            seen = set()
            while isinstance(hurt.get(f), str):
                if f in seen:
                    raise ValueError(f'{f}: hurtbox alias loop')
                seen.add(f)
                f = hurt[f]
            if f not in hurt:
                raise ValueError(f'{name}: frame {f} has no hurtboxes')
            return check_boxes(hurt[f], f'hurtboxes {f}')

        def sequence(seq, what, kind):
            first = len(self.steps)
            if not isinstance(seq, list) or not seq:
                raise ValueError(f'{what}: empty step list')
            for step in seq:
                if not isinstance(step, list) or not 2 <= len(step) <= 3:
                    raise ValueError(f'{what}: a step is [frame, ticks] or [frame, ticks, {{...}}]')
                f, ticks = step[0], step[1]
                extra = step[2] if len(step) > 2 else {}
                if f not in index:
                    raise ValueError(f'{what}: unknown frame {f}')
                check_int(ticks, 1, 65535, f'{what}: step duration')
                if not isinstance(extra, dict) or set(extra) - {'hit', 'hurt', 'move', 'spawn'}:
                    raise ValueError(f'{what}: unknown step keys {extra}')
                if kind == 'action' and set(extra) & {'hit', 'move', 'spawn'}:
                    raise ValueError(f'{what}: hit, move and spawn are only allowed in moves')
                if kind == 'projectile' and set(extra) & {'hurt', 'move', 'spawn'}:
                    raise ValueError(f'{what}: projectile steps only take hit boxes')
                if kind == 'projectile':
                    hb = []
                    if not extra.get('hit'):
                        raise ValueError(f'{what}: every projectile step needs hit boxes')
                else:
                    hb = check_boxes(extra['hurt'], what) if 'hurt' in extra else frame_hurt(f)
                xb = check_boxes(extra.get('hit', []), what)
                vx, vy = extra.get('move', [0, 0]) if isinstance(extra.get('move', [0, 0]), list) \
                    else (None, None)
                if 'move' in extra and len(extra['move']) != 2:
                    raise ValueError(f'{what}: move is [forward, down]')
                check_int(vx, -MAX_VELOCITY, MAX_VELOCITY, f'{what}: move forward')
                check_int(vy, -MAX_VELOCITY, MAX_VELOCITY, f'{what}: move down')
                spawn = 0
                if 'spawn' in extra:
                    if extra['spawn'] not in self.projectile_names:
                        raise ValueError(f'{what}: unknown projectile {extra["spawn"]}')
                    spawn = self.projectile_names.index(extra['spawn']) + 1
                self.steps.append((index[f], ticks, len(self.boxes), len(hb), len(xb), vx, vy, spawn))
                self.boxes += hb + xb
            return first, len(seq)

        given = ch['actions']
        if not set(ACTIONS) - set(DERIVED_ACTIONS) <= set(given) or set(given) - set(ACTIONS):
            raise ValueError(f'{name}: needs the actions {ACTIONS} ({DERIVED_ACTIONS} optional)')
        actions = {**self.knockdown_actions(given), **given}
        for action in ACTIONS:
            self.actions.append(sequence(actions[action], f'{name} {action}', 'action'))
        projectiles = ch.get('projectiles', {})
        if len(projectiles) > MAX_PROJECTILE_TYPES:
            raise ValueError(f'{name}: at most {MAX_PROJECTILE_TYPES} projectiles')
        self.projectile_names = list(projectiles)
        for pname, pr in projectiles.items():
            what = f'{name} projectile {pname}'
            need = {'steps', 'speed', 'lifetime', 'offset', 'guard', 'blend', 'alpha'}
            known = need | set(HIT_FIELDS) | {'class', 'src', 'knockdown'}
            if not isinstance(pr, dict) or need - set(pr) or set(pr) - known:
                raise ValueError(f'{what}: fields must be {sorted(need)} plus the hit fields '
                                 f'(class and src optional)')
            first, count = sequence(pr['steps'], what, 'projectile')
            hit = Hit(pr, what, True, self.system)
            count_sources(pr, what, hit)
            self.projectile_hits.append(hit)
            off = pr['offset']
            if not isinstance(off, list) or len(off) != 2:
                raise ValueError(f'{what}: offset is [forward, down]')
            check_int(off[0], -MAX_SIDE, MAX_SIDE, f'{what}: offset')
            check_int(off[1], -MAX_SIDE, MAX_SIDE, f'{what}: offset')
            if pr['guard'] not in GUARDS or pr['blend'] not in BLENDS:
                raise ValueError(f'{what}: guard must be one of {GUARDS}, blend of {BLENDS}')
            self.projectiles.append((first, count, check_int(pr['speed'], 1, MAX_VELOCITY, f'{what}: speed'),
                                     check_int(pr['lifetime'], 1, MAX_LIFETIME, f'{what}: lifetime'),
                                     off[0], off[1], *hit.packed,
                                     GUARDS.index(pr['guard']), BLENDS.index(pr['blend']),
                                     check_int(pr['alpha'], 1, 255, f'{what}: alpha'), *hit.tail(),
                                     FLAG_KNOCKDOWN * flag(pr, 'knockdown', False, what)))
        moves = ch['moves']
        if not set(MOVES) <= set(moves):
            raise ValueError(f'{name}: needs the moves {MOVES}')
        specials = [k for k in moves if k not in MOVES]
        if len(specials) > MAX_SPECIALS:
            raise ValueError(f'{name}: at most {MAX_SPECIALS} specials')
        for move in MOVES + specials:
            mv = moves[move]
            what = f'{name} {move}'
            special = move not in MOVES
            known = {'steps', 'guard', 'cancel', 'class', 'src', 'knockdown', 'otg'} | \
                set(HIT_FIELDS) | \
                ({'trigger', 'name'} if special else set())
            if set(mv) - known or not {'steps', 'guard', 'cancel'} <= set(mv) or \
                    (special and 'trigger' not in mv):
                raise ValueError(f'{what}: fields must be {sorted(known)} (hit fields optional '
                                 f'without hitboxes)')
            first, count = sequence(mv['steps'], what, 'move')
            steps = self.steps[first:first + count]
            total = sum(s[1] for s in steps)
            if total > MAX_MOVE_TICKS:
                raise ValueError(f'{what}: {total} ticks, at most {MAX_MOVE_TICKS}')
            height = 0
            for s in steps:
                height += s[1] * s[6]
                if height > 0 or height < -MAX_HEIGHT:
                    raise ValueError(f'{what}: vertical motion leaves 0..-{MAX_HEIGHT}')
            if height:
                raise ValueError(f'{what}: vertical motion must end on the ground')
            hit = Hit(mv, what, any(s[4] for s in steps), self.system)
            count_sources(mv, what, hit)
            self.hits.append(hit)
            if mv['guard'] not in GUARDS or not isinstance(mv['cancel'], list) or \
                    not set(mv['cancel']) <= set(CANCELS):
                raise ValueError(f'{what}: guard must be one of {GUARDS}, cancel of {CANCELS}')
            cancel = sum(1 << CANCELS.index(c) for c in set(mv['cancel']))
            follow = None
            if special:
                tr = mv['trigger']
                if not isinstance(tr, dict) or not {'motion', 'buttons'} <= set(tr) or \
                        set(tr) - {'motion', 'buttons', 'after', 'window', 'contact'} or \
                        tr['motion'] not in MOTIONS:
                    raise ValueError(f'{what}: trigger is {{"motion": one of {MOTIONS}, "buttons"}}'
                                     f' plus "after", "window" and "contact" for a follow-up')
                if 'after' in tr:
                    window = tr.get('window')
                    if not isinstance(tr['after'], str) or not isinstance(window, list) or \
                            len(window) != 2 or not isinstance(tr.get('contact', False), bool):
                        raise ValueError(f'{what}: a follow-up needs "after" (a move name), '
                                         f'"window": [open, close] and optionally "contact" '
                                         f'(true or false)')
                    follow = (tr['after'], window, tr.get('contact', False))
                elif set(tr) - {'motion', 'buttons'}:
                    raise ValueError(f'{what}: "window" and "contact" need "after"')
                elif MOTIONS.index(tr['motion']) >= FOLLOW_ONLY:
                    raise ValueError(f'{what}: motion {tr["motion"]} is only for follow-ups')
                head = (1, MOTIONS.index(tr['motion']), button_mask(tr['buttons'], what))
            else:
                head = (0, MOVES.index(move), 0)
            self.moves.append((*head, first, count, *hit.packed, GUARDS.index(mv['guard']), cancel,
                               *hit.tail(), 0, 0, 0, 0, FLAG_OTG * flag(mv, 'otg', False, what)))
            self.move_names.append(move)
            self.labels.append(move_name(mv.get('name'), move, what) if special else '')
            self.follow.append(follow)
        # Follow-ups name their parent; the window lies inside its length.
        for k, follow in enumerate(self.follow):
            if follow is None:
                continue
            what = f'{name} {self.move_names[k]}'
            parent, (open_, close), contact = follow
            if parent not in self.move_names or parent == self.move_names[k]:
                raise ValueError(f'{what}: after must name another move of {name}')
            p = self.move_names.index(parent)
            total = self.frame_data(p)[3]
            check_int(open_, 0, total - 1, f'{what}: window open')
            check_int(close, open_, total - 1, f'{what}: window close (open .. {total - 1})')
            self.moves[k] = self.moves[k][:16] + (p + 1, open_, close, int(contact)) + \
                self.moves[k][20:]
        # Knockdown: written, else the sweep (crouch HK) and every special
        # of class special/super that has hitboxes, launches nothing and is
        # the last stage of its rekka (nothing follows it).
        for k, move in enumerate(self.move_names):
            mv = moves[move]
            parent = any(f and f[0] == move for f in self.follow)
            default = move == 'crouch_hk' or (
                k >= len(MOVES) and mv.get('class') in ('special', 'super') and
                self.frame_data(k)[4] > 0 and not parent and
                not any(len(st) > 2 and 'spawn' in st[2] for st in mv['steps']))
            knock = flag(mv, 'knockdown', default, f'{name} {move}')
            self.moves[k] = self.moves[k][:20] + (self.moves[k][20] | FLAG_KNOCKDOWN * knock,)

    @staticmethod
    def knockdown_actions(given):
        """fall, down and getup when a manifest has none of its own: fall is
        the KO animation without its last step, down that last step (the
        lying frame) and getup the first crouch frame for GETUP_TICKS."""
        ko, crouch = given['ko'], given['crouch']
        lying = ko[-1][:1] + [1]
        return {'fall': ko[:-1] or [lying], 'down': [lying],
                'getup': [[crouch[0][0], GETUP_TICKS]]}

    def frames(self):
        return {s[0] for s in self.steps}

    def pack(self, palettes):
        name = self.display.encode()
        out = struct.pack('<B', len(name)) + name + struct.pack('<2H', *palettes)
        mask = sum(1 << i for i, k in enumerate(STATS) if k in self.overrides)
        out += struct.pack('<2H', self.system_index, mask)
        out += b''.join(struct.pack('<H', self.overrides.get(k, 0) & 0xffff) for k in STATS)
        out += struct.pack('<4H', len(self.steps), len(self.boxes), len(self.moves),
                           len(self.projectiles))
        for first, count in self.actions:
            out += struct.pack('<HH', first, count)
        for mv, label in zip(self.moves, self.labels):
            out += struct.pack('<21HB', *mv, len(label)) + label.encode()
        for pr in self.projectiles:
            out += struct.pack('<4H2h13H', *pr)
        for s in self.steps:
            out += struct.pack('<3H2B2bB', *s)
        for b in self.boxes:
            out += struct.pack('<hhHH', *b)
        return out + struct.pack('<2H', self.slash, self.home_index)

    def frame_data(self, k):
        """(startup, active, recovery, total, hits) of a move, as the core sees it."""
        first, count = self.moves[k][3:5]
        t, startup, end, hits, previous = 0, None, None, 0, False
        for s in self.steps[first:first + count]:
            ticks, hit = s[1], s[4]
            if hit:
                if startup is None:
                    startup = t
                end = t + ticks
                hits += not previous
            previous = bool(hit)
            t += ticks
        if startup is None:
            return 0, 0, t, t, 0
        return startup, end - startup, t - end, t, hits


def palettes_of(m, name):
    """(normal, alternate) palette names: players slot 1 and 2, else the first two."""
    names = list(m['palettes'])
    picks = []
    for slot in range(2):
        p = m['players'][slot] if len(m.get('players', [])) == 2 else None
        if p and p['character'] == name:
            picks.append(p['palette'])
        else:
            picks.append(names[min(slot, len(names) - 1)])
    for p in picks:
        if p not in names:
            raise ValueError(f'{name}: unknown palette {p}')
    return picks


def encode_frame(sid, pix, ax, ay, index):
    """A packed frame: rows of opaque spans of palette indices (None is
    transparent)."""
    h, w = len(pix), len(pix[0])
    counts, spans, data = [], [], bytearray()
    for row in pix:
        k, x = 0, 0
        while x < w:
            if row[x] is None:
                x += 1
                continue
            start = x
            while x < w and row[x] is not None:
                data.append(index[row[x]])
                x += 1
            spans.append((start, x - start))
            k += 1
        counts.append(k)
    return (struct.pack('<HHHhhI', sid, w, h, ax, ay, len(spans)) +
            struct.pack(f'<{h}H', *counts) + b''.join(struct.pack('<HH', *sp) for sp in spans) +
            data)


def trim(pix):
    """(trimmed rows, left, top) of a pixel block, or None if all transparent."""
    used_rows = [j for j, row in enumerate(pix) if any(p is not None for p in row)]
    if not used_rows:
        return None
    used_cols = [i for i in range(len(pix[0])) if any(row[i] is not None for row in pix)]
    t, b, l, r = used_rows[0], used_rows[-1] + 1, used_cols[0], used_cols[-1] + 1
    return [row[l:r] for row in pix[t:b]], l, t


class Image:
    """An RGB or RGBA PNG checked against its SHA256; pixels are transparent
    where alpha is 0 or the colour equals the background key."""

    def __init__(self, path, sha256, background=None):
        self.path = pathlib.Path(path)
        digest = hashlib.sha256(self.path.read_bytes()).hexdigest()
        if digest != sha256:
            raise ValueError(f'{path}: SHA256 {digest} does not match the manifest')
        self.w, self.h, self.rows = read_png(path, alpha=True)
        self.key = bytes.fromhex(background) if background else None

    def block(self, rect, what):
        x, y, w, h = rect
        if not all(isinstance(v, int) for v in rect) or w < 1 or h < 1 or x < 0 or y < 0 or \
                x + w > self.w or y + h > self.h or w > MAX_SIDE or h > MAX_SIDE:
            raise ValueError(f'{what}: rectangle {rect} outside {self.path.name} or too large')
        out = []
        for j in range(h):
            row = self.rows[y + j]
            line = []
            for i in range(x, x + w):
                p = row[i * 4:i * 4 + 4]
                line.append(None if p[3] == 0 or p[:3] == self.key else p[:3])
            out.append(line)
        return out


def palette_of(blocks, what):
    colors = sorted({c for pix in blocks for row in pix for c in row if c is not None})
    if not colors:
        raise ValueError(f'{what}: no opaque pixels')
    if len(colors) > 256:
        raise ValueError(f'{what}: {len(colors)} colours; 256 per sheet is the limit')
    return colors


class Sparks:
    """A spark manifest: hit/guard/slash/blood animations on one RGBA sheet."""

    def __init__(self, path, ident):
        path = pathlib.Path(path)
        m = json.loads(path.read_text())
        what = path.name
        if not isinstance(m, dict) or set(m) - {'sheet', 'sha256', 'frames', 'anims', 'sources',
                                                 'notes'} or not {'sheet', 'sha256', 'frames',
                                                                  'anims'} <= set(m):
            raise ValueError(f'{what}: needs sheet, sha256, frames and anims')
        self.ident = ident
        sources = check_sources(m, what)
        img = Image(path.parent / m['sheet'], m['sha256'])
        self.names = list(m['frames'])
        self.frames = []
        for n in self.names:
            f = m['frames'][n]
            if not isinstance(f, list) or len(f) != 6 or not all(isinstance(v, int) for v in f):
                raise ValueError(f'{what} {n}: a frame is [x, y, w, h, anchor x, anchor y]')
            t = trim(img.block(f[:4], f'{what} {n}'))
            if t is None:
                raise ValueError(f'{what} {n}: empty frame')
            pix, l, top = t
            ax, ay = f[4] - l, f[5] - top
            if not -MAX_SIDE <= ax <= MAX_SIDE or not -MAX_SIDE <= ay <= MAX_SIDE:
                raise ValueError(f'{what} {n}: anchor out of range')
            self.frames.append((pix, ax, ay))
        self.colors = palette_of([f[0] for f in self.frames], what)
        self.anims = {}
        for name, an in m['anims'].items():
            w = f'{what} {name}'
            if not isinstance(an, dict) or set(an) - {'steps', 'blend', 'alpha', 'src'} or \
                    not {'steps', 'blend'} <= set(an) or an['blend'] not in BLENDS:
                raise ValueError(f'{w}: an anim is {{steps, blend, alpha (blended), src}}')
            if 'src' in an:
                check_src(an['src'], w, {'steps'}, sources)
            blend = BLENDS.index(an['blend'])
            alpha = check_int(an.get('alpha', 255), 1, 255, f'{w}: alpha')
            steps = an['steps']
            if not isinstance(steps, list) or not 1 <= len(steps) <= MAX_SPARK_STEPS:
                raise ValueError(f'{w}: 1..{MAX_SPARK_STEPS} steps')
            out = []
            for st in steps:
                if not isinstance(st, list) or len(st) != 2 or st[0] not in self.names:
                    raise ValueError(f'{w}: a step is [frame, ticks]')
                out.append((self.names.index(st[0]), check_int(st[1], 1, 255, f'{w}: ticks')))
            self.anims[name] = (blend, alpha, out)


LAYER_KEYS = {'sheet', 'rect', 'x', 'y', 'parallax', 'edge', 'color', 'anims'}


class Stage:
    """A stage manifest: parallax layers and their animated sub-rectangles,
    each layer one palette (see the docstring above)."""

    def __init__(self, path, systems):
        path = pathlib.Path(path)
        m = json.loads(path.read_text())
        what = path.name
        need = {'name', 'system', 'width', 'ground', 'sheets', 'layers'}
        if not isinstance(m, dict) or need - set(m) or set(m) - need - {'sources', 'notes'}:
            raise ValueError(f'{what}: needs {sorted(need)}, may have sources and notes')
        check_sources(m, what)
        self.name = m['name']
        self.display = display_name(m['name'])
        if m['system'] not in systems.profile:
            raise ValueError(f'{what}: unknown system {m["system"]}')
        self.system = systems.names.index(m['system'])
        self.width = check_int(m['width'], MIN_STAGE_WIDTH, MAX_STAGE_WIDTH, f'{what}: width')
        self.ground = check_int(m['ground'], 0, MAX_SIDE, f'{what}: ground')
        sheets = m['sheets']
        if not isinstance(sheets, dict) or not sheets:
            raise ValueError(f'{what}: sheets is {{id: {{file, sha256, background}}}}')
        images = {}
        for k, v in sheets.items():
            if not isinstance(v, dict) or set(v) - {'file', 'sha256', 'background'} or \
                    not {'file', 'sha256'} <= set(v):
                raise ValueError(f'{what}: sheet {k} needs file and sha256')
            images[k] = Image(path.parent / v['file'], v['sha256'], v.get('background'))
        layers = m['layers']
        if not isinstance(layers, list) or not 1 <= len(layers) <= MAX_LAYERS:
            raise ValueError(f'{what}: 1..{MAX_LAYERS} layers')
        self.layers = []
        for n, ly in enumerate(layers):
            w = f'{what} layer {n}'
            if not isinstance(ly, dict) or set(ly) - LAYER_KEYS or \
                    not {'sheet', 'rect', 'x', 'y', 'edge'} <= set(ly) or ly['sheet'] not in images:
                raise ValueError(f'{w}: needs a known sheet, rect, x, y and edge')
            img = images[ly['sheet']]
            if not isinstance(ly['rect'], list) or len(ly['rect']) != 4:
                raise ValueError(f'{w}: rect is [x, y, w, h]')
            base = img.block(ly['rect'], w)
            lw, lh = ly['rect'][2], ly['rect'][3]
            x = check_int(ly['x'], -MAX_SIDE, MAX_SIDE, f'{w}: x')
            y = check_int(ly['y'], -MAX_SIDE, MAX_SIDE, f'{w}: y')
            par = ly.get('parallax', 'fit')
            if par == 'fit':
                num, den = (lw - SCREEN_WIDTH, self.width - SCREEN_WIDTH) \
                    if self.width > SCREEN_WIDTH else (0, 1)
            elif isinstance(par, list) and len(par) == 2:
                num = check_int(par[0], -MAX_SIDE, MAX_SIDE, f'{w}: parallax num')
                den = check_int(par[1], 1, MAX_STAGE_WIDTH, f'{w}: parallax den')
            else:
                raise ValueError(f'{w}: parallax is "fit" or [num, den]')
            edge = ly['edge']
            if not isinstance(edge, list) or len(edge) != 2 or not set(edge) <= set(EDGES):
                raise ValueError(f'{w}: edge is [horizontal, vertical] of {EDGES}')
            if 'color' in edge and 'color' not in ly:
                raise ValueError(f'{w}: edge color needs "color"')
            color = rgb565(bytes.fromhex(ly['color'])) if 'color' in ly else 0
            raw = []
            for k, an in enumerate(ly.get('anims', [])):
                aw = f'{w} anim {k}'
                if not isinstance(an, dict) or set(an) - {'steps', 'diff'} or \
                        not isinstance(an.get('steps'), list) or \
                        not 1 <= len(an['steps']) <= MAX_ANIM_STEPS:
                    raise ValueError(f'{aw}: an anim is {{steps (1..{MAX_ANIM_STEPS}), diff}}')
                steps = []
                for st in an['steps']:
                    if not isinstance(st, list) or len(st) != 4 or not isinstance(st[0], list) \
                            or len(st[0]) != 4:
                        raise ValueError(f'{aw}: a step is [[x, y, w, h], ticks, dx, dy]')
                    steps.append((img.block(st[0], aw), check_int(st[1], 1, 65535, f'{aw}: ticks'),
                                  check_int(st[2], -MAX_SIDE, MAX_SIDE, f'{aw}: dx'),
                                  check_int(st[3], -MAX_SIDE, MAX_SIDE, f'{aw}: dy')))
                raw.append((bool(an.get('diff')), steps))
            # A diff anim's frames are cropped to the pixels that differ from
            # the layer; where a frame is transparent over an opaque layer
            # pixel, that pixel is taken out of the layer (a hole every frame
            # fills or leaves open).
            orig = base
            base = [row[:] for row in base]
            moving = []
            for diff, steps in raw:
                mask, holes = set(), set()
                if diff:
                    for pix, _, dx, dy in steps:
                        for j, row in enumerate(pix):
                            for i, c in enumerate(row):
                                lx, ly_ = dx + i, dy + j
                                inside = 0 <= lx < lw and 0 <= ly_ < lh
                                under = orig[ly_][lx] if inside else None
                                if c != under:
                                    mask.add((lx, ly_))
                                    if c is None:
                                        holes.add((lx, ly_))
                for lx, ly_ in holes:
                    base[ly_][lx] = None
                moving.append((mask, holes))
            anims = []
            blocks = [base]
            for (diff, steps), (mask, holes) in zip(raw, moving):
                out = []
                for pix, ticks, dx, dy in steps:
                    if diff:
                        def keep(i, j, c):
                            p = (dx + i, dy + j)
                            if p not in mask or c is None:
                                return False
                            return p in holes or not (0 <= p[0] < lw and 0 <= p[1] < lh) or \
                                c != orig[p[1]][p[0]]
                        pix = [[c if keep(i, j, c) else None for i, c in enumerate(row)]
                               for j, row in enumerate(pix)]
                    t = trim(pix)
                    if t is None:
                        out.append((None, ticks, dx, dy))
                        continue
                    blocks.append(t[0])
                    out.append((t[0], ticks, dx + t[1], dy + t[2]))
                anims.append(out)
            if len(anims) > MAX_LAYER_ANIMS:
                raise ValueError(f'{w}: at most {MAX_LAYER_ANIMS} anims')
            colors = palette_of(blocks, w)
            t = trim(base)
            if t is None:
                raise ValueError(f'{w}: empty layer')
            self.layers.append({'pix': t[0], 'left': t[1], 'top': t[2], 'w': lw, 'h': lh,
                                'x': x, 'y': y, 'num': num, 'den': den,
                                'edge': [EDGES.index(e) for e in edge], 'color': color,
                                'anims': anims, 'colors': colors})

    def pixels(self):
        return sum(c is not None for ly in self.layers for row in ly['pix'] for c in row)


def load_extras(path):
    """({id: Sparks}, [Stage]) listed by a roster (none for a manifest)."""
    path = pathlib.Path(path)
    data = json.loads(path.read_text())
    if 'manifests' not in data:
        return {}, []
    systems = load_systems(path)
    sparks = data.get('sparks', {})
    stages = data.get('stages', [])
    if not isinstance(sparks, dict) or not isinstance(stages, list):
        raise ValueError(f'{path}: sparks is {{id: manifest}}, stages a list of manifests')
    if len(stages) > MAX_STAGES:
        raise ValueError(f'{path}: at most {MAX_STAGES} stages')
    out = [Stage(path.parent / s, systems) for s in stages]
    if len({s.display for s in out}) != len(out):
        raise ValueError(f'{path}: two stages share a name')
    return {k: Sparks(path.parent / v, k) for k, v in sparks.items()}, out


def load_systems(path):
    """The systems.json next to a roster or manifest."""
    return Systems(pathlib.Path(path).parent / 'systems.json')


def load_roster(path):
    """[(Sheet, manifest path)], effects manifest index, from a roster or manifest."""
    path = pathlib.Path(path)
    data = json.loads(path.read_text())
    if 'manifests' in data:
        if not {'manifests', 'effects'} <= set(data) or \
                set(data) - {'manifests', 'effects', 'stages', 'sparks'} or not data['manifests']:
            raise ValueError(f'{path}: a roster has "manifests" (non-empty) and "effects", '
                             f'optional "stages" and "sparks"')
        paths = [path.parent / p for p in data['manifests']]
        if len(set(paths)) != len(paths):
            raise ValueError(f'{path}: a manifest is listed twice')
        if data['effects'] not in data['manifests']:
            raise ValueError(f'{path}: effects must name one of the manifests')
        return [Sheet(p) for p in paths], data['manifests'].index(data['effects'])
    return [Sheet(path)], 0


def build(sheets, effects_from, automatic, systems, sparks=None, stages=None):
    """The pack bytes, its hash, the colour count, per-frame (w, h, opaque)
    stats and the characters. sparks {id: Sparks} and stages [Stage] are the
    roster's (none for a single manifest)."""
    sparks, stages = sparks or {}, stages or []
    out = bytearray()
    palette_count, frame_base, chars, char_palettes, all_frames = 0, 0, [], [], []
    stats, ncolors = {}, 0
    names_seen = set()
    palette_base = []
    for sid, sheet in enumerate(sheets):
        m = sheet.m
        names = list(m['frames'])
        frames = {n: sheet.frame(n) for n in names}
        colors = sorted({c for pix, _, _ in frames.values() for row in pix for c in row if c})
        if len(colors) > 256:
            raise ValueError(f'{sheet.manifest_path}: {len(colors)} colours; 256 per sheet is the limit')
        ncolors += len(colors)
        index = {c: i for i, c in enumerate(colors)}
        pal_names = list(m['palettes'])
        palette_base.append(palette_count)
        for name in pal_names:
            remap = {bytes.fromhex(k): bytes.fromhex(v) for k, v in m['palettes'][name].items()}
            pal = [rgb565(remap.get(c, c)) for c in colors]
            out += struct.pack('<HH', sid, len(pal)) + struct.pack(f'<{len(pal)}H', *pal)
        palette_count += len(pal_names)
        for n in names:
            pix, ax, ay = frames[n]
            all_frames.append(encode_frame(sid, pix, ax, ay, index))
            stats[(sid, n)] = (len(pix[0]), len(pix), sum(c is not None for row in pix for c in row))
        for cname in m['characters']:
            c = Character(m, cname, names, systems, frame_base)
            if c.display in names_seen:
                raise ValueError(f'{cname}: another character has the name {c.display}')
            names_seen.add(c.display)
            chars.append(c)
            char_palettes.append(tuple(palette_base[sid] + pal_names.index(p)
                                       for p in palettes_of(m, cname)))
        frame_base += len(names)
    if len(chars) > MAX_CHARACTERS:
        raise ValueError(f'{len(chars)} characters, at most {MAX_CHARACTERS}')
    sid = len(sheets)

    def sheet(colors, blocks):
        """A new sheet with one palette; returns (palette, first frame)."""
        nonlocal sid, palette_count, frame_base, ncolors, out
        index = {c: i for i, c in enumerate(colors)}
        out += struct.pack('<HH', sid, len(colors)) + struct.pack(
            f'<{len(colors)}H', *[rgb565(c) for c in colors])
        first = frame_base
        for pix, ax, ay in blocks:
            all_frames.append(encode_frame(sid, pix, ax, ay, index))
        ncolors += len(colors)
        frame_base += len(blocks)
        sid += 1
        palette_count += 1
        return palette_count - 1, first

    # Sparks: one sheet per manifest; anims indexed in roster then manifest order.
    spark_index, spark_out = {}, b''
    for ident, sp in sparks.items():
        pal, first = sheet(sp.colors, sp.frames)
        for name, (blend, alpha, steps) in sp.anims.items():
            spark_index[f'{ident}.{name}'] = len(spark_index)
            spark_out += struct.pack('<BBH', blend, alpha, len(steps)) + b''.join(
                struct.pack('<3H', first + f, pal, t) for f, t in steps)
    if len(spark_index) > MAX_SPARKS:
        raise ValueError(f'{len(spark_index)} spark anims, at most {MAX_SPARKS}')
    # Stages: one sheet per layer (the layer then its anim frames).
    stage_out = b''
    for st in stages:
        name = st.display.encode()
        stage_out += struct.pack('<B', len(name)) + name + struct.pack(
            '<4H', st.width, st.ground, st.system, len(st.layers))
        for ly in st.layers:
            blocks = [(ly['pix'], -ly['left'], -ly['top'])]
            for steps in ly['anims']:
                blocks += [(pix, 0, 0) for pix, _, _, _ in steps if pix is not None]
            pal, first = sheet(ly['colors'], blocks)
            stage_out += struct.pack('<4H3hH2BHH', first, pal, ly['w'], ly['h'], ly['x'], ly['y'],
                                     ly['num'], ly['den'], ly['edge'][0], ly['edge'][1],
                                     ly['color'], len(ly['anims']))
            f = first + 1
            for steps in ly['anims']:
                stage_out += struct.pack('<H', len(steps))
                for pix, ticks, dx, dy in steps:
                    stage_out += struct.pack('<H2hH', 0xffff if pix is None else f, dx, dy, ticks)
                    f += pix is not None
    if palette_count > MAX_PALETTES or frame_base > MAX_FRAMES:
        raise ValueError(f'{palette_count} palettes and {frame_base} frames, at most '
                         f'{MAX_PALETTES} and {MAX_FRAMES}')
    names = [st.name for st in stages]
    for c in chars:
        if c.home_stage is not None:
            if c.home_stage not in names:
                raise ValueError(f'{c.name}: unknown home_stage {c.home_stage}')
            c.home_index = names.index(c.home_stage) + 1
    out += b''.join(all_frames)
    out += spark_out
    out += systems.pack(spark_index)
    for c, pal in zip(chars, char_palettes):
        out += c.pack(pal)
    em = sheets[effects_from].m
    enames, epals = list(em['frames']), list(em['palettes'])
    ebase = sum(len(s.m['frames']) for s in sheets[:effects_from])
    for f, p in em['effects']:
        out += struct.pack('<HH', ebase + enames.index(f), palette_base[effects_from] + epals.index(p))
    out += stage_out
    body = struct.pack('<8I', int(automatic), palette_count, frame_base, len(chars),
                       len(em['effects']), len(systems.names), len(spark_index),
                       len(stages)) + out
    checksum = 2166136261
    for b in body:
        checksum = ((checksum ^ b) * 16777619) & 0xffffffff
    size = 16 + len(body)
    return MAGIC + struct.pack('<II', size, checksum) + body, checksum, ncolors, stats, chars


def describe_stats(c):
    st, sy = c.stats, c.system
    peak, ticks = jump_arc(st['jump_vy'], st['gravity'])
    own = ', '.join(sorted(c.overrides)) or 'none'
    return (f'system {sy["name"]} (damage scale {sy["damage_scale"]}), life {st["life"]} '
            f'defense {st["defense"]} weight {st["weight"]} height {HEIGHTS[st["height"]]}, '
            f'walk {st["walk"]}/{st["walk_back"]} (1/4 px), jump vy {st["jump_vy"]} gravity '
            f'{st["gravity"]} drift {st["vx"]}/{st["vx_back"]} prejump {st["prejump"]} '
            f'(peak {peak} px, {ticks} ticks airborne); own stats: {own}')


def frame_data(sheets, systems):
    """Stats, frame data (damage after the damage scale, class defaults
    filled) and the source summary of every character."""
    for sheet in sheets:
        m = sheet.m
        names = list(m['frames'])
        for cname in m['characters']:
            c = Character(m, cname, names, systems)
            print(f'{cname}: {describe_stats(c)}')
            print(f'{cname}: move startup/active/recovery total hits, damage/chip, '
                  f'hitstun/blockstun/hitstop, adv hit/block, guard, class, trigger')
            for k, move in enumerate(c.move_names):
                s, a, r, t, hits = c.frame_data(k)
                mv, h = c.moves[k], c.hits[k].resolved
                cls = CLASSES[c.hits[k].cls - 1] if c.hits[k].cls else '-'
                trig = f'{MOTIONS[mv[1]]}+' + '/'.join(b for i, b in enumerate(BUTTONS)
                                                       if mv[2] >> i & 1) if mv[0] else 'normal'
                if mv[16]:
                    trig += (f' after {c.move_names[mv[16] - 1]} {mv[17]}..{mv[18]}'
                             f'{" on contact" if mv[19] else ""}')
                print(f'  {move:12} {s:2}/{a:2}/{r:2} {t:3} {hits}  {h[0]:4}/{h[1]:3}  '
                      f'{h[2]:3}/{h[3]:3}/{h[4]:2}  {h[2] - a - r:+3}/{h[3] - a - r:+3}  '
                      f'{GUARDS[mv[12]]:8} {cls:8} {trig}')
                spawn = next(((i, st) for i, st in enumerate(c.steps[mv[3]:mv[3] + mv[4]])
                              if st[7]), None)
                if spawn:
                    tick = sum(st[1] for st in c.steps[mv[3]:mv[3] + spawn[0]])
                    pr = c.projectiles[spawn[1][7] - 1]
                    ph = c.projectile_hits[spawn[1][7] - 1].resolved
                    print(f'  {"":12} launches {c.projectile_names[spawn[1][7] - 1]} at elapsed '
                          f'{tick}: speed {pr[2]} lifetime {pr[3]} damage/chip {ph[0]}/{ph[1]} '
                          f'hitstun/blockstun/hitstop {ph[2]}/{ph[3]}/{ph[4]} {BLENDS[pr[14]]}')
            total = sum(c.sources.values())
            kinds = ', '.join(f'{k} {n}' for k, n in sorted(c.sources.items(),
                                                            key=lambda x: (-x[1], x[0])))
            print(f'{cname}: sources of {total} move fields: {kinds}')
            estimate = c.sources.get('estimate', 0)
            if total and estimate * 100 > ESTIMATE_WARNING * total:
                print(f'{cname}: note: {estimate * 100 // total}% of the move fields are '
                      f'estimates (more than {ESTIMATE_WARNING}%)')


def box_rects(sheet):
    """The boxes [x, y, w, h] of a boxed sheet. A box starts at a pixel that
    is not separator and not inside a known box, and runs right along its top
    row and down its left column to the next separator; its inside is never
    looked at, so sprites may use the separator colour as long as they keep
    off the box's top row and left column."""
    inside, out = {}, []
    for y in range(sheet.h):
        x = 0
        while x < sheet.w:
            if (x, y) in inside:
                x = inside[x, y]
                continue
            if sheet.px(x, y) == sheet.sep:
                x += 1
                continue
            x1, y1 = x, y
            while x1 < sheet.w and sheet.px(x1, y) != sheet.sep:
                x1 += 1
            while y1 < sheet.h and sheet.px(x, y1) != sheet.sep:
                y1 += 1
            inside.update(((x, yy), x1) for yy in range(y, y1))
            out.append([x, y, x1 - x, y1 - y])
            x = x1
    return out


def sheet_boxes(sheet):
    """[(box, trimmed sprite [x, y, w, h] or None)] of a boxed sheet."""
    out = []
    for x, y, w, h in sheet.boxes:
        used = [(i, j) for j in range(y, y + h) for i in range(x, x + w) if sheet.px(i, j) != sheet.bg]
        sprite = None
        if used:
            xs, ys = [u[0] for u in used], [u[1] for u in used]
            sprite = [min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1]
        out.append(([x, y, w, h], sprite))
    return out


def detect(sheet):
    if sheet.sep:
        print(f'background {sheet.m["background"]}, boxed by {sheet.m["separator"]}; '
              'rows of boxes: [x, y, w, h] of the sprite inside each box')
        by_top = {}
        for box, sprite in sheet_boxes(sheet):
            by_top.setdefault(box[1], []).append(sprite)
        for top, sprites in sorted(by_top.items()):
            print(f'boxes at y={top}: ' + ' '.join(str(r) for r in sprites if r))
        return
    print(f'background {sheet.m["background"]}, credit box rows {sheet.credit}')
    for x0, y, x1 in sheet.underlines:
        print(f'underline y={y} x={x0}..{x1}')
    for top, bottom in sheet.bands[1:]:
        print(f'rows {top}..{bottom}: ' + ' '.join(f'{a}..{b}' for a, b in sheet.segments(top, bottom)))


def preview(sheet, path):
    """One strip per action, move and projectile, frames on a shared baseline
    and anchor column grid, hurtboxes outlined green and hitboxes red."""
    m = sheet.m
    names = list(m['frames'])
    c = Character(m, list(m['characters'])[0], names, load_systems(sheet.manifest_path))
    strips = [c.steps[f:f + n] for f, n in c.actions]
    strips += [c.steps[mv[3]:mv[3] + mv[4]] for mv in c.moves]
    strips += [c.steps[pr[0]:pr[0] + pr[1]] for pr in c.projectiles]
    cache = {}
    cell_w, left, up, down = 200, 90, 150, 12
    width = cell_w * max(len(s) for s in strips)
    height = (up + down) * len(strips)
    img = bytearray(b'\x30\x30\x40' * width * height)

    def put(x, y, col):
        if 0 <= x < width and 0 <= y < height:
            img[(y * width + x) * 3:(y * width + x) * 3 + 3] = col

    def outline(ox, base, b, col):
        x0, y0, x1, y1 = ox + b[0], base + b[1], ox + b[0] + b[2] - 1, base + b[1] + b[3] - 1
        for x in range(x0, x1 + 1):
            put(x, y0, col)
            put(x, y1, col)
        for y in range(y0, y1 + 1):
            put(x0, y, col)
            put(x1, y, col)

    for row, strip in enumerate(strips):
        base = row * (up + down) + up
        for col, (f, ticks, box, hurt, hit, vx, vy, spawn) in enumerate(strip):
            if f not in cache:
                cache[f] = sheet.frame(names[f])
            pix, ax, ay = cache[f]
            ox = col * cell_w + left
            for x in range(col * cell_w, (col + 1) * cell_w):
                put(x, base, b'\x80\x80\x80')
            for j, line in enumerate(pix):
                for i, px in enumerate(line):
                    if px:
                        put(ox - ax + i, base - ay + j, px)
            for y in range(base - up, base + down):
                p = (y * width + ox) * 3
                if img[p:p + 3] == b'\x30\x30\x40':
                    img[p:p + 3] = b'\x00\x60\x00'
            for k in range(hurt + hit):
                outline(ox, base, c.boxes[box + k], b'\x00\xff\x40' if k < hurt else b'\xff\x20\x20')
    pathlib.Path(path).write_bytes(b'P6\n%d %d\n255\n' % (width, height) + img)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('roster')
    b.add_argument('output', type=pathlib.Path)
    b.add_argument('--auto', action='store_true')
    d = sub.add_parser('detect')
    d.add_argument('manifest')
    p = sub.add_parser('preview')
    p.add_argument('manifest')
    p.add_argument('output')
    t = sub.add_parser('data')
    t.add_argument('roster')
    args = ap.parse_args()
    if args.cmd == 'detect':
        detect(Sheet(args.manifest))
    elif args.cmd == 'preview':
        preview(Sheet(args.manifest), args.output)
    elif args.cmd == 'data':
        frame_data(load_roster(args.roster)[0], load_systems(args.roster))
    else:
        sheets, effects_from = load_roster(args.roster)
        sparks, stages = load_extras(args.roster)
        data, checksum, ncolors, stats, chars = build(sheets, effects_from, args.auto,
                                                      load_systems(args.roster), sparks, stages)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        opaque = sum(s[2] for s in stats.values())
        print(f'{args.output}: {len(data)} bytes, hash {checksum:08x}, {len(chars)} characters '
              f'({", ".join(c.display for c in chars)}), {len(stats)} frames, {opaque} opaque '
              f'pixels, {ncolors} colours')
        for st in stages:
            print(f'  stage {st.display}: {st.width} px wide, {len(st.layers)} layers, '
                  f'{st.pixels()} layer pixels, '
                  f'{sum(len(a) for ly in st.layers for a in ly["anims"])} anim steps')
        print(f'  {sum(len(sp.anims) for sp in sparks.values())} spark anims')


if __name__ == '__main__':
    sys.exit(main())
