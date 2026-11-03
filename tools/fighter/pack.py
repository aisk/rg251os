#!/usr/bin/env python3
"""Build a FIGHTP03 content pack from a ripped sprite sheet and a manifest.

The manifest (assets/fighter/*.json) is the source of truth: every frame is a
hand-checked sheet rectangle plus a foot anchor, every action a list of
(frame, ticks) steps. Pixels stay 1:1; the sheet background is transparent and
each frame is trimmed and stored as per-row opaque spans of palette indices.
Frames must have a background border and may not touch the sheet's credit
box or its white animation underlines. Subcommands:

  build MANIFEST OUTPUT [--auto]   write the pack (Auto runs the AI scene cycle)
  detect MANIFEST                  print candidate frame boxes per sheet row
  preview MANIFEST OUTPUT.ppm      every action and move of the first
                                   character on its anchor, with hurtboxes
                                   (green) and hitboxes (red) outlined
  data MANIFEST                    frame data table of every move

Manifest schema (assets/fighter/terry.json is the reference):
  frames      name -> [x, y, w, h, anchor x, anchor y] in sheet pixels
  hurtboxes   frame -> [[x, y, w, h], ...] or the name of another frame whose
              boxes it shares. Boxes are frame pixels relative to the anchor
              (x forward for a right-facing sprite, y down, so heights above
              the feet are negative); the core mirrors them with facing.
              Every frame a character uses needs an entry ([] = invulnerable).
  characters  name -> {"actions": {action: [steps]}, "moves": {move: {...}}}
              A step is [frame, ticks] or [frame, ticks, {"hit": boxes,
              "hurt": boxes}]; "hurt" replaces the frame's hurtboxes for that
              step, "hit" is only allowed in moves. Contiguous steps with hit
              boxes form one hit (a move may have several).
              A move has "steps" plus damage, chip, hitstun, blockstun,
              hitstop (ticks), pushback and block_pushback (pixels), guard
              (high = stand or crouch block, low = crouch block only,
              overhead = stand block only) and cancel (list of chain,
              special, super; stored for later, no rules use it yet).
  players     two {"character", "palette"} slots: P1 and P2
  palettes    name -> {sheet RGB hex: replacement hex}
  effects     [frame, palette] pairs for the effect sprites

Frame data printed by `data` (all in 60 Hz ticks): startup = ticks before
the first hitbox (the press tick is elapsed 0, so the first active tick is
startup ticks after the press), active = ticks from the first to the last
hitbox step, recovery = the rest. A hit on the first active tick gives the
attacker an advantage of hitstun - (active + recovery), blockstun likewise;
hitstop freezes both fighters equally and does not change it.

Pack layout, all little endian (the C loader in app/fighter/render.c checks it):
  header     "FIGHTP03", u32 size, u32 FNV-1a of bytes 16..size, u32 flags
             (bit 0 automatic), u32 palettes, frames, characters, effects
  palette    u16 n (1..256), n x u16 RGB565
  frame      u16 w, h; i16 anchor x, y; u32 spans; h x u16 spans per row;
             spans x (u16 x, u16 len); then sum(len) u8 palette indices
  character  u16 steps, boxes, moves; 10 actions x (u16 first step, u16 steps);
             moves x 12 u16 (trigger, first step, steps, damage, chip,
             hitstun, blockstun, hitstop, pushback, block_pushback, guard,
             cancel bits); steps x (u16 frame, u16 ticks, u16 first box,
             u8 hurt boxes, u8 hit boxes: hurt first, then hit);
             boxes x (i16 x, i16 y, u16 w, u16 h). Step and box indices are
             local to the character.
  players    2 x (u16 character, u16 palette)
  effects    u16 frame, u16 palette
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

MAGIC = b'FIGHTP03'
# Order matters: these are the C enums in app/fighter/fighter.h.
ACTIONS = ['idle', 'walk_forward', 'walk_back', 'jump', 'guard', 'hurt', 'ko',
           'crouch', 'crouch_guard', 'crouch_hurt']
MOVES = ['stand_lp', 'stand_hp', 'stand_lk', 'stand_hk',
         'crouch_lp', 'crouch_hp', 'crouch_lk', 'crouch_hk']
GUARDS = ['high', 'low', 'overhead']
CANCELS = ['chain', 'special', 'super']
# Limits shared with fighter.h; the C loader rejects anything beyond them.
MAX_SIDE = 1024
MAX_STEP_BOXES = 15
MAX_MOVE_TICKS = 255
MAX_DAMAGE = 1000
MAX_STUN = 120
MAX_HITSTOP = 30
MAX_PUSH = 120


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
        if (len(b) != 4 or not all(isinstance(v, int) for v in b) or
                not -MAX_SIDE <= b[0] <= MAX_SIDE or not -MAX_SIDE <= b[1] <= MAX_SIDE or
                not 1 <= b[2] <= MAX_SIDE or not 1 <= b[3] <= MAX_SIDE):
            raise ValueError(f'{what}: bad box {b}')
    return [tuple(b) for b in boxes]


class Character:
    """One character's steps, boxes and moves, resolved from the manifest."""

    def __init__(self, m, name, frame_names):
        ch = m['characters'][name]
        self.name = name
        self.steps, self.boxes, self.actions, self.moves = [], [], [], []
        hurt = m['hurtboxes']

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

        def sequence(seq, what, allow_hit):
            first = len(self.steps)
            if not seq:
                raise ValueError(f'{what}: empty step list')
            for step in seq:
                f, ticks = step[0], step[1]
                extra = step[2] if len(step) > 2 else {}
                if f not in frame_names:
                    raise ValueError(f'{what}: unknown frame {f}')
                if not isinstance(ticks, int) or not 1 <= ticks <= 65535:
                    raise ValueError(f'{what}: step durations must be positive')
                if set(extra) - {'hit', 'hurt'}:
                    raise ValueError(f'{what}: unknown step keys {sorted(extra)}')
                if 'hit' in extra and not allow_hit:
                    raise ValueError(f'{what}: hitboxes are only allowed in moves')
                hb = check_boxes(extra['hurt'], what) if 'hurt' in extra else frame_hurt(f)
                xb = check_boxes(extra.get('hit', []), what)
                self.steps.append((frame_names.index(f), ticks, len(self.boxes), len(hb), len(xb)))
                self.boxes += hb + xb
            return first, len(seq)

        if sorted(ch['actions']) != sorted(ACTIONS):
            raise ValueError(f'{name}: needs exactly the actions {ACTIONS}')
        for action in ACTIONS:
            self.actions.append(sequence(ch['actions'][action], f'{name} {action}', False))
        if sorted(ch['moves']) != sorted(MOVES):
            raise ValueError(f'{name}: needs exactly the moves {MOVES}')
        for trigger, move in enumerate(MOVES):
            mv = ch['moves'][move]
            what = f'{name} {move}'
            known = {'steps', 'damage', 'chip', 'hitstun', 'blockstun', 'hitstop',
                     'pushback', 'block_pushback', 'guard', 'cancel'}
            if set(mv) != known:
                raise ValueError(f'{what}: fields must be exactly {sorted(known)}')
            first, count = sequence(mv['steps'], what, True)
            total = sum(t for t in (s[1] for s in self.steps[first:first + count]))
            limits = {'damage': MAX_DAMAGE, 'chip': mv['damage'], 'hitstun': MAX_STUN,
                      'blockstun': MAX_STUN, 'hitstop': MAX_HITSTOP, 'pushback': MAX_PUSH,
                      'block_pushback': MAX_PUSH}
            for k, top in limits.items():
                low = 1 if k in ('hitstun', 'blockstun') else 0
                if not isinstance(mv[k], int) or not low <= mv[k] <= top:
                    raise ValueError(f'{what}: {k} must be {low}..{top}')
            if total > MAX_MOVE_TICKS:
                raise ValueError(f'{what}: {total} ticks, at most {MAX_MOVE_TICKS}')
            if mv['guard'] not in GUARDS or not set(mv['cancel']) <= set(CANCELS):
                raise ValueError(f'{what}: guard must be one of {GUARDS}, cancel of {CANCELS}')
            cancel = sum(1 << CANCELS.index(c) for c in set(mv['cancel']))
            self.moves.append((trigger, first, count, mv['damage'], mv['chip'], mv['hitstun'],
                               mv['blockstun'], mv['hitstop'], mv['pushback'],
                               mv['block_pushback'], GUARDS.index(mv['guard']), cancel))

    def pack(self):
        out = struct.pack('<3H', len(self.steps), len(self.boxes), len(self.moves))
        for first, count in self.actions:
            out += struct.pack('<HH', first, count)
        for mv in self.moves:
            out += struct.pack('<12H', *mv)
        for s in self.steps:
            out += struct.pack('<3HBB', *s)
        for b in self.boxes:
            out += struct.pack('<hhHH', *b)
        return out

    def frame_data(self, trigger):
        """(startup, active, recovery, total, hits) of a move, as the core sees it."""
        _, first, count = self.moves[trigger][:3]
        t, startup, end, hits, previous = 0, None, None, 0, False
        for f, ticks, box, hurt, hit in self.steps[first:first + count]:
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


def build(sheet, automatic):
    m = sheet.m
    names = list(m['frames'])
    frames = {n: sheet.frame(n) for n in names}
    colors = sorted({c for pix, _, _ in frames.values() for row in pix for c in row if c})
    if len(colors) > 256:
        raise ValueError(f'{len(colors)} colours; one 256-entry index space is the limit')
    index = {c: i for i, c in enumerate(colors)}
    pal_names = list(m['palettes'])
    palettes = []
    for name in pal_names:
        remap = {bytes.fromhex(k): bytes.fromhex(v) for k, v in m['palettes'][name].items()}
        palettes.append([rgb565(remap.get(c, c)) for c in colors])
    out = bytearray()
    for pal in palettes:
        out += struct.pack('<H', len(pal)) + struct.pack(f'<{len(pal)}H', *pal)
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
        out += struct.pack('<HHhhI', w, h, ax, ay, len(spans))
        out += struct.pack(f'<{h}H', *counts)
        for s in spans:
            out += struct.pack('<HH', *s)
        out += data
    char_names = list(m['characters'])
    chars = [Character(m, c, names) for c in char_names]
    for c in chars:
        out += c.pack()
    if len(m['players']) != 2:
        raise ValueError('exactly two players are required')
    for p in m['players']:
        out += struct.pack('<HH', char_names.index(p['character']), pal_names.index(p['palette']))
    for f, p in m['effects']:
        out += struct.pack('<HH', names.index(f), pal_names.index(p))
    body = struct.pack('<5I', int(automatic), len(palettes), len(names), len(chars),
                       len(m['effects'])) + out
    checksum = 2166136261
    for b in body:
        checksum = ((checksum ^ b) * 16777619) & 0xffffffff
    size = 16 + len(body)
    stats = {n: (len(frames[n][0][0]), len(frames[n][0]),
                 sum(c is not None for row in frames[n][0] for c in row)) for n in names}
    return MAGIC + struct.pack('<II', size, checksum) + body, checksum, len(colors), stats


def frame_data(sheet):
    m = sheet.m
    names = list(m['frames'])
    for cname in m['characters']:
        c = Character(m, cname, names)
        print(f'{cname}: move startup/active/recovery total hits, damage, '
              f'hitstun/blockstun/hitstop, adv hit/block, guard')
        for trigger, move in enumerate(MOVES):
            s, a, r, t, hits = c.frame_data(trigger)
            mv = c.moves[trigger]
            print(f'  {move:10} {s:2}/{a:2}/{r:2} {t:3} {hits}  {mv[3]:4}  '
                  f'{mv[5]:3}/{mv[6]:3}/{mv[7]:2}  {mv[5] - a - r:+3}/{mv[6] - a - r:+3}  '
                  f'{GUARDS[mv[10]]}')


def detect(sheet):
    print(f'background {sheet.m["background"]}, credit box rows {sheet.credit}')
    for x0, y, x1 in sheet.underlines:
        print(f'underline y={y} x={x0}..{x1}')
    for top, bottom in sheet.bands[1:]:
        print(f'rows {top}..{bottom}: ' + ' '.join(f'{a}..{b}' for a, b in sheet.segments(top, bottom)))


def preview(sheet, path):
    """One strip per action and move, frames on a shared baseline and anchor
    column grid, hurtboxes outlined green and hitboxes red."""
    m = sheet.m
    names = list(m['frames'])
    c = Character(m, list(m['characters'])[0], names)
    strips = [c.steps[f:f + n] for f, n in c.actions]
    strips += [c.steps[mv[1]:mv[1] + mv[2]] for mv in c.moves]
    cache = {}
    cell_w, left, up, down = 200, 90, 150, 12
    width = cell_w * max(len(s) for s in strips)
    height = (up + down) * len(strips)
    img = bytearray(b'\x30\x30\x40' * width * height)

    def put(x, y, c):
        if 0 <= x < width and 0 <= y < height:
            img[(y * width + x) * 3:(y * width + x) * 3 + 3] = c

    def outline(ox, base, b, c):
        x0, y0, x1, y1 = ox + b[0], base + b[1], ox + b[0] + b[2] - 1, base + b[1] + b[3] - 1
        for x in range(x0, x1 + 1):
            put(x, y0, c)
            put(x, y1, c)
        for y in range(y0, y1 + 1):
            put(x0, y, c)
            put(x1, y, c)

    for row, strip in enumerate(strips):
        base = row * (up + down) + up
        for col, (f, ticks, box, hurt, hit) in enumerate(strip):
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
    b.add_argument('manifest')
    b.add_argument('output', type=pathlib.Path)
    b.add_argument('--auto', action='store_true')
    d = sub.add_parser('detect')
    d.add_argument('manifest')
    p = sub.add_parser('preview')
    p.add_argument('manifest')
    p.add_argument('output')
    t = sub.add_parser('data')
    t.add_argument('manifest')
    args = ap.parse_args()
    sheet = Sheet(args.manifest)
    if args.cmd == 'detect':
        detect(sheet)
    elif args.cmd == 'preview':
        preview(sheet, args.output)
    elif args.cmd == 'data':
        frame_data(sheet)
    else:
        data, checksum, ncolors, stats = build(sheet, args.auto)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        opaque = sum(s[2] for s in stats.values())
        print(f'{args.output}: {len(data)} bytes, hash {checksum:08x}, {len(stats)} frames, '
              f'{opaque} opaque pixels, {ncolors} colours')


if __name__ == '__main__':
    sys.exit(main())
