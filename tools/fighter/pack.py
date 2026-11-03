#!/usr/bin/env python3
"""Build a FIGHTP04 content pack from ripped sprite sheets and their manifests.

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
Subcommands:

  build MANIFEST|ROSTER OUTPUT [--auto]  write the pack (Auto: AI vs AI,
                                         rotating characters, scene cycle)
  detect MANIFEST                  print candidate frame boxes per sheet row
  preview MANIFEST OUTPUT.ppm      every action, move and projectile of the
                                   first character on its anchor, with
                                   hurtboxes (green) and hitboxes (red)
  data MANIFEST|ROSTER             frame data table of every move

Manifest schema (assets/fighter/terry.json is the reference; unknown
top-level keys such as "notes" are ignored):
  frames      name -> [x, y, w, h, anchor x, anchor y] in sheet pixels
  hurtboxes   frame -> [[x, y, w, h], ...] or the name of another frame whose
              boxes it shares. Boxes are frame pixels relative to the anchor
              (x forward for a right-facing sprite, y down, so heights above
              the feet are negative); the core mirrors them with facing.
              Every frame a character's actions and moves use needs an entry
              ([] = invulnerable); projectile frames need none.
  characters  name -> {"actions": {action: [steps]}, "moves": {move: {...}},
              "projectiles": {name: {...}}}. The name, upper-cased with "_"
              as a space, is shown in the select screen and HUD (A-Z, 0-9,
              space and "-", at most 12 characters).
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
              (pixels); moves without them may omit those.
              Projectiles: "steps" (looping animation, each step with "hit"
              boxes), "speed" (pixels per tick forward), "lifetime" (ticks),
              "offset" ([forward, down] from the owner's anchor at launch),
              "blend" (opaque, alpha or add), "alpha" and the same hit fields
              as moves. Blended projectiles share the effect pixel budget.
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

Pack layout, all little endian (the C loader in app/fighter/render.c checks it):
  header     "FIGHTP04", u32 size, u32 FNV-1a of bytes 16..size, u32 flags
             (bit 0 automatic), u32 palettes, frames, characters, effects
  palette    u16 sheet, u16 n (1..256), n x u16 RGB565
  frame      u16 sheet, u16 w, h; i16 anchor x, y; u32 spans; h x u16 spans
             per row; spans x (u16 x, u16 len); then sum(len) u8 indices
  character  u8 name length, name bytes; u16 normal palette, alternate
             palette; u16 steps, boxes, moves, projectiles; 10 actions x
             (u16 first step, u16 steps); moves x 14 u16 (kind 0 normal or 1
             special, trigger or motion, buttons, first step, steps, damage,
             chip, hitstun, blockstun, hitstop, pushback, block_pushback,
             guard, cancel bits): the eight normals, then the specials in
             priority order; projectiles x 16 u16 (first step, steps, speed,
             lifetime, i16 offset x, i16 offset y, damage, chip, hitstun,
             blockstun, hitstop, pushback, block_pushback, guard, blend,
             alpha); steps x (u16 frame, u16 ticks, u16 first box, u8 hurt
             boxes, u8 hit boxes, i8 forward, i8 down, u8 spawn = projectile
             + 1 or 0); boxes x (i16 x, i16 y, u16 w, u16 h). Frame indices
             are global; step and box indices are local to the character.
  effects    u16 frame, u16 palette
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

MAGIC = b'FIGHTP04'
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
MAX_DAMAGE = 1000
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


def hit_fields(d, what, required):
    """damage .. block_pushback; defaults (0, stun 1) only when not required."""
    out = []
    for k in HIT_FIELDS:
        if k not in d:
            if required:
                raise ValueError(f'{what}: needs {k}')
            out.append(1 if k in ('hitstun', 'blockstun') else 0)
            continue
        top = {'damage': MAX_DAMAGE, 'chip': d.get('damage', 0), 'hitstun': MAX_STUN,
               'blockstun': MAX_STUN, 'hitstop': MAX_HITSTOP, 'pushback': MAX_PUSH,
               'block_pushback': MAX_PUSH}[k]
        out.append(check_int(d[k], 1 if k in ('hitstun', 'blockstun') else 0, top,
                             f'{what}: {k}'))
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


class Character:
    """One character's steps, boxes, moves and projectiles from a manifest;
    frame indices are global (base + position in the manifest's frames)."""

    def __init__(self, m, name, frame_names, base=0):
        ch = m['characters'][name]
        self.name = name
        self.display = display_name(name)
        self.steps, self.boxes, self.actions = [], [], []
        self.moves, self.move_names, self.projectiles, self.projectile_names = [], [], [], []
        hurt = m['hurtboxes']
        index = {f: base + i for i, f in enumerate(frame_names)}
        if set(ch) - {'actions', 'moves', 'projectiles'}:
            raise ValueError(f'{name}: unknown keys {sorted(set(ch) - {"actions", "moves", "projectiles"})}')

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
            known = {'steps', 'speed', 'lifetime', 'offset', 'guard', 'blend', 'alpha'} | set(HIT_FIELDS)
            if set(pr) != known:
                raise ValueError(f'{what}: fields must be exactly {sorted(known)}')
            first, count = sequence(pr['steps'], what, 'projectile')
            off = pr['offset']
            if not isinstance(off, list) or len(off) != 2:
                raise ValueError(f'{what}: offset is [forward, down]')
            check_int(off[0], -MAX_SIDE, MAX_SIDE, f'{what}: offset')
            check_int(off[1], -MAX_SIDE, MAX_SIDE, f'{what}: offset')
            if pr['guard'] not in GUARDS or pr['blend'] not in BLENDS:
                raise ValueError(f'{what}: guard must be one of {GUARDS}, blend of {BLENDS}')
            self.projectiles.append((first, count, check_int(pr['speed'], 1, MAX_VELOCITY, f'{what}: speed'),
                                     check_int(pr['lifetime'], 1, MAX_LIFETIME, f'{what}: lifetime'),
                                     off[0], off[1], *hit_fields(pr, what, True),
                                     GUARDS.index(pr['guard']), BLENDS.index(pr['blend']),
                                     check_int(pr['alpha'], 1, 255, f'{what}: alpha')))
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
            known = {'steps', 'guard', 'cancel'} | set(HIT_FIELDS) | ({'trigger'} if special else set())
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
            fields = hit_fields(mv, what, any(s[4] for s in steps))
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
            self.moves.append((*head, first, count, *fields, GUARDS.index(mv['guard']), cancel))
            self.move_names.append(move)

    def frames(self):
        return {s[0] for s in self.steps}

    def pack(self, palettes):
        name = self.display.encode()
        out = struct.pack('<B', len(name)) + name + struct.pack('<2H', *palettes)
        out += struct.pack('<4H', len(self.steps), len(self.boxes), len(self.moves),
                           len(self.projectiles))
        for first, count in self.actions:
            out += struct.pack('<HH', first, count)
        for mv in self.moves:
            out += struct.pack('<14H', *mv)
        for pr in self.projectiles:
            out += struct.pack('<4H2h10H', *pr)
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


def build(sheets, effects_from, automatic):
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
            c = Character(m, cname, names, frame_base)
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
    for c, pal in zip(chars, char_palettes):
        out += c.pack(pal)
    em = sheets[effects_from].m
    enames, epals = list(em['frames']), list(em['palettes'])
    ebase = sum(len(s.m['frames']) for s in sheets[:effects_from])
    for f, p in em['effects']:
        out += struct.pack('<HH', ebase + enames.index(f), palette_base[effects_from] + epals.index(p))
    body = struct.pack('<5I', int(automatic), palette_count, frame_base, len(chars),
                       len(em['effects'])) + out
    checksum = 2166136261
    for b in body:
        checksum = ((checksum ^ b) * 16777619) & 0xffffffff
    size = 16 + len(body)
    return MAGIC + struct.pack('<II', size, checksum) + body, checksum, ncolors, stats, chars


def frame_data(sheets):
    for sheet in sheets:
        m = sheet.m
        names = list(m['frames'])
        for cname in m['characters']:
            c = Character(m, cname, names)
            print(f'{cname}: move startup/active/recovery total hits, damage/chip, '
                  f'hitstun/blockstun/hitstop, adv hit/block, guard, trigger')
            for k, move in enumerate(c.move_names):
                s, a, r, t, hits = c.frame_data(k)
                mv = c.moves[k]
                trig = f'{MOTIONS[mv[1]]}+' + '/'.join(b for i, b in enumerate(BUTTONS)
                                                       if mv[2] >> i & 1) if mv[0] else 'normal'
                print(f'  {move:12} {s:2}/{a:2}/{r:2} {t:3} {hits}  {mv[5]:4}/{mv[6]:3}  '
                      f'{mv[7]:3}/{mv[8]:3}/{mv[9]:2}  {mv[7] - a - r:+3}/{mv[8] - a - r:+3}  '
                      f'{GUARDS[mv[12]]:8} {trig}')
                spawn = next(((i, st) for i, st in enumerate(c.steps[mv[3]:mv[3] + mv[4]])
                              if st[7]), None)
                if spawn:
                    tick = sum(st[1] for st in c.steps[mv[3]:mv[3] + spawn[0]])
                    pr = c.projectiles[spawn[1][7] - 1]
                    print(f'  {"":12} launches {c.projectile_names[spawn[1][7] - 1]} at elapsed '
                          f'{tick}: speed {pr[2]} lifetime {pr[3]} damage/chip {pr[6]}/{pr[7]} '
                          f'hitstun/blockstun/hitstop {pr[8]}/{pr[9]}/{pr[10]} {BLENDS[pr[14]]}')


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
    c = Character(m, list(m['characters'])[0], names)
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
        frame_data(load_roster(args.roster)[0])
    else:
        sheets, effects_from = load_roster(args.roster)
        data, checksum, ncolors, stats, chars = build(sheets, effects_from, args.auto)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        opaque = sum(s[2] for s in stats.values())
        print(f'{args.output}: {len(data)} bytes, hash {checksum:08x}, {len(chars)} characters '
              f'({", ".join(c.display for c in chars)}), {len(stats)} frames, {opaque} opaque '
              f'pixels, {ncolors} colours')


if __name__ == '__main__':
    sys.exit(main())
