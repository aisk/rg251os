#!/usr/bin/env python3
"""Build a FIGHTP05 content pack from ripped sprite sheets and their manifests.

Each manifest (assets/fighter/<name>.json) describes one sheet and is the
source of truth for it: every frame is a hand-checked sheet rectangle plus a
foot anchor, every action a list of (frame, ticks) steps. Pixels stay 1:1;
the sheet background is transparent and each frame is trimmed and stored as
per-row opaque spans of palette indices. Frames must have a background border
and may not touch the sheet's credit box or its white animation underlines.

A pack is built from a roster (assets/fighter/roster.json) or from a single
manifest, which then acts as a roster of its own characters:
  manifests   list of manifest paths relative to the roster; their characters
              appear in the select screen in this order
  effects     the manifest whose "effects" list the stress scenes draw
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
              Moves: the eight normals (stand/crouch lp/hp/lk/hk) are fixed by
              name; any other name is a special and needs
              "trigger": {"motion": M, "buttons": B} with M one of 236, 214,
              623, [4]6, [2]8 (directions relative to facing; [d] = charge)
              and B "P", "K" or a list of lp/hp/lk/hk. Specials are checked
              in manifest order, the first match wins. Every move has
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
              Projectiles: "steps" (looping animation, each step with "hit"
              boxes), "speed" (pixels per tick forward), "lifetime" (ticks),
              "offset" ([forward, down] from the owner's anchor at launch),
              "blend" (opaque, alpha or add), "alpha" and the same hit fields
              as moves (class and src too). Blended projectiles share the
              effect pixel budget.
  players     two {"character", "palette"} slots: the palette of slot 1 is a
              character's normal palette, slot 2 its mirror-match alternate
              (characters not listed use the first and second palette)
  palettes    name -> {sheet RGB hex: replacement hex}
  effects     [frame, palette] pairs for the effect sprites

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
Optional: defense, weight (1000), height (mid), src, notes. hop, super_jump,
hyper_hop, chip, meters, mechanics, scaling, stun and guard are accepted for
later steps and not packed.

Pack layout, all little endian (the C loader in app/fighter/render.c checks it):
  header     "FIGHTP05", u32 size, u32 FNV-1a of bytes 16..size, u32 flags
             (bit 0 automatic), u32 palettes, frames, characters, effects,
             systems
  palette    u16 sheet, u16 n (1..256), n x u16 RGB565
  frame      u16 sheet, u16 w, h; i16 anchor x, y; u32 spans; h x u16 spans
             per row; spans x (u16 x, u16 len); then sum(len) u8 indices
  system     u8 name length, name bytes; u16 damage scale; 11 stats x u16
             (life, defense, walk, walk_back, i16 jump vy, gravity, vx,
             vx_back, prejump, weight, height 0 low 1 mid 2 high); u16 push
             distance, guard range, buffer, quarter, dragon, charge, grace,
             release; 6 classes (light, heavy, command, special, super,
             throw) x (u16 hitstun, blockstun, hitstop, has: bit 0 the stun
             pair, bit 1 hitstop; absent values 0)
  character  u8 name length, name bytes; u16 normal palette, alternate
             palette; u16 system, u16 override mask (bit per stat), 11 stats
             x u16 as above (0 where the mask bit is clear); u16 steps, boxes,
             moves, projectiles; 10 actions x (u16 first step, u16 steps);
             moves x 16 u16 (kind 0 normal or 1 special, trigger or motion,
             buttons, first step, steps, damage, chip, hitstun, blockstun,
             hitstop, pushback, block_pushback, guard, cancel bits, class 0
             none or 1 + class, defaults: bit 0 hitstun, 1 blockstun, 2
             hitstop from the class, packed as 0): the eight normals, then the
             specials in priority order; projectiles x 18 u16 (first step,
             steps, speed, lifetime, i16 offset x, i16 offset y, damage, chip,
             hitstun, blockstun, hitstop, pushback, block_pushback, guard,
             blend, alpha, class, defaults); steps x (u16 frame, u16 ticks,
             u16 first box, u8 hurt boxes, u8 hit boxes, i8 forward, i8 down,
             u8 spawn = projectile + 1 or 0); boxes x (i16 x, i16 y, u16 w,
             u16 h). Frame indices are global; step and box indices are local
             to the character. Damage is packed as written; src and sources
             are not packed.
  effects    u16 frame, u16 palette
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

MAGIC = b'FIGHTP05'
# Order matters: these are the C enums in app/fighter/fighter.h.
ACTIONS = ['idle', 'walk_forward', 'walk_back', 'jump', 'guard', 'hurt', 'ko',
           'crouch', 'crouch_guard', 'crouch_hurt']
MOVES = ['stand_lp', 'stand_hp', 'stand_lk', 'stand_hk',
         'crouch_lp', 'crouch_hp', 'crouch_lk', 'crouch_hk']
MOTIONS = ['236', '214', '623', '[4]6', '[2]8']
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


def read_png(path):
    """Decode an 8-bit, non-interlaced RGB or RGBA PNG to (w, h, RGB rows)."""
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat, header = 8, [], None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat.append(body)
        pos += 12 + n
    w, h, depth, color, _, _, interlace = header
    if depth != 8 or color not in (2, 6) or interlace:
        raise ValueError(f'{path}: only 8-bit non-interlaced RGB/RGBA is supported')
    bpp = 3 if color == 2 else 4
    stride = w * bpp
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
        # reaches into its neighbour.
        for xx in range(x - 1, x + w + 1):
            if self.px(xx, y - 1) != self.bg or self.px(xx, y + h) != self.bg:
                raise ValueError(f'{name}: no background border above/below')
        for yy in range(y - 1, y + h + 1):
            if self.px(x - 1, yy) != self.bg or self.px(x + w, yy) != self.bg:
                raise ValueError(f'{name}: no background border left/right')
        pixels = [[None if self.px(x + i, y + j) == self.bg else self.px(x + i, y + j)
                   for i in range(w)] for j in range(h)]
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
        known = need | {'defense', 'weight', 'height', 'src', 'notes'} | LATER_SYSTEM_KEYS
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
        return {'name': name,
                'damage_scale': check_int(p['damage_scale'], MIN_SCALE, MAX_PERMILLE,
                                          f'{what}: damage_scale'),
                'stats': st, 'inputs': inputs,
                'push': check_int(p['push_distance'], 2, MAX_PUSH_DISTANCE,
                                  f'{what}: push_distance'),
                'guard_range': check_int(p['guard_range'], 0, SCREEN_WIDTH,
                                         f'{what}: guard_range'),
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

    def pack(self):
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
        return out


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
        self.hits, self.projectile_hits = [], []
        hurt = m['hurtboxes']
        index = {f: base + i for i, f in enumerate(frame_names)}
        known = {'actions', 'moves', 'projectiles', 'system', 'stats'}
        if set(ch) - known:
            raise ValueError(f'{name}: unknown keys {sorted(set(ch) - known)}')
        sources = check_sources(m, name)
        self.system = systems.of(name, ch.get('system'))
        self.system_index = systems.names.index(self.system['name'])
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

        if sorted(ch['actions']) != sorted(ACTIONS):
            raise ValueError(f'{name}: needs exactly the actions {ACTIONS}')
        for action in ACTIONS:
            self.actions.append(sequence(ch['actions'][action], f'{name} {action}', 'action'))
        projectiles = ch.get('projectiles', {})
        if len(projectiles) > MAX_PROJECTILE_TYPES:
            raise ValueError(f'{name}: at most {MAX_PROJECTILE_TYPES} projectiles')
        self.projectile_names = list(projectiles)
        for pname, pr in projectiles.items():
            what = f'{name} projectile {pname}'
            need = {'steps', 'speed', 'lifetime', 'offset', 'guard', 'blend', 'alpha'}
            known = need | set(HIT_FIELDS) | {'class', 'src'}
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
                                     check_int(pr['alpha'], 1, 255, f'{what}: alpha'), *hit.tail()))
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
            known = {'steps', 'guard', 'cancel', 'class', 'src'} | set(HIT_FIELDS) | \
                ({'trigger'} if special else set())
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
            if special:
                tr = mv['trigger']
                if not isinstance(tr, dict) or set(tr) != {'motion', 'buttons'} or \
                        tr['motion'] not in MOTIONS:
                    raise ValueError(f'{what}: trigger is {{"motion": one of {MOTIONS}, "buttons"}}')
                head = (1, MOTIONS.index(tr['motion']), button_mask(tr['buttons'], what))
            else:
                head = (0, MOVES.index(move), 0)
            self.moves.append((*head, first, count, *hit.packed, GUARDS.index(mv['guard']), cancel,
                               *hit.tail()))
            self.move_names.append(move)

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
        for mv in self.moves:
            out += struct.pack('<16H', *mv)
        for pr in self.projectiles:
            out += struct.pack('<4H2h12H', *pr)
        for s in self.steps:
            out += struct.pack('<3H2B2bB', *s)
        for b in self.boxes:
            out += struct.pack('<hhHH', *b)
        return out

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


def load_systems(path):
    """The systems.json next to a roster or manifest."""
    return Systems(pathlib.Path(path).parent / 'systems.json')


def load_roster(path):
    """[(Sheet, manifest path)], effects manifest index, from a roster or manifest."""
    path = pathlib.Path(path)
    data = json.loads(path.read_text())
    if 'manifests' in data:
        if set(data) != {'manifests', 'effects'} or not data['manifests']:
            raise ValueError(f'{path}: a roster has exactly "manifests" (non-empty) and "effects"')
        paths = [path.parent / p for p in data['manifests']]
        if len(set(paths)) != len(paths):
            raise ValueError(f'{path}: a manifest is listed twice')
        if data['effects'] not in data['manifests']:
            raise ValueError(f'{path}: effects must name one of the manifests')
        return [Sheet(p) for p in paths], data['manifests'].index(data['effects'])
    return [Sheet(path)], 0


def build(sheets, effects_from, automatic, systems):
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
            all_frames.append(struct.pack('<HHHhhI', sid, w, h, ax, ay, len(spans)) +
                              struct.pack(f'<{h}H', *counts) +
                              b''.join(struct.pack('<HH', *s) for s in spans) + data)
            stats[(sid, n)] = (w, h, sum(c is not None for row in pix for c in row))
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
    out += b''.join(all_frames)
    out += systems.pack()
    for c, pal in zip(chars, char_palettes):
        out += c.pack(pal)
    em = sheets[effects_from].m
    enames, epals = list(em['frames']), list(em['palettes'])
    ebase = sum(len(s.m['frames']) for s in sheets[:effects_from])
    for f, p in em['effects']:
        out += struct.pack('<HH', ebase + enames.index(f), palette_base[effects_from] + epals.index(p))
    body = struct.pack('<6I', int(automatic), palette_count, frame_base, len(chars),
                       len(em['effects']), len(systems.names)) + out
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


def detect(sheet):
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
        data, checksum, ncolors, stats, chars = build(sheets, effects_from, args.auto,
                                                      load_systems(args.roster))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        opaque = sum(s[2] for s in stats.values())
        print(f'{args.output}: {len(data)} bytes, hash {checksum:08x}, {len(chars)} characters '
              f'({", ".join(c.display for c in chars)}), {len(stats)} frames, {opaque} opaque '
              f'pixels, {ncolors} colours')


if __name__ == '__main__':
    sys.exit(main())
