#!/usr/bin/env python3
"""Build a FIGHTP02 content pack from a ripped sprite sheet and a manifest.

The manifest (assets/fighter/*.json) is the source of truth: every frame is a
hand-checked sheet rectangle plus a foot anchor, every action a list of
(frame, ticks) steps. Pixels stay 1:1; the sheet background is transparent and
each frame is trimmed and stored as per-row opaque spans of palette indices.
Frames must have a background border and may not touch the sheet's credit
box or its white animation underlines. Subcommands:

  build MANIFEST OUTPUT [--auto]   write the pack (Auto runs the AI scene cycle)
  detect MANIFEST                  print candidate frame boxes per sheet row
  preview MANIFEST OUTPUT.ppm      actions side by side on their anchors

Pack layout, all little endian (the C loader in app/fighter/render.c checks it):
  header   "FIGHTP02", u32 size, u32 FNV-1a of bytes 16..size, u32 flags
           (bit 0 automatic), u32 palettes, frames, steps, effects
  palette  u16 n (1..256), n x u16 RGB565
  frame    u16 w, h; i16 anchor x, y; u32 spans; h x u16 spans per row;
           spans x (u16 x, u16 len); then sum(len) u8 palette indices
  2 chars  u16 palette, 8 actions x (u16 first step, u16 steps)
  steps    u16 frame, u16 ticks (>0)
  effects  u16 frame, u16 palette
Actions, in order: idle walk_forward walk_back jump attack guard hurt ko.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

ACTIONS = ['idle', 'walk_forward', 'walk_back', 'jump', 'attack', 'guard', 'hurt', 'ko']
MAGIC = b'FIGHTP02'


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
    steps = []
    for ch in m['characters']:
        out += struct.pack('<H', pal_names.index(ch['palette']))
        if sorted(ch['actions']) != sorted(ACTIONS):
            raise ValueError(f'{ch["name"]}: needs exactly the actions {ACTIONS}')
        for action in ACTIONS:
            seq = ch['actions'][action]
            out += struct.pack('<HH', len(steps), len(seq))
            steps += [(names.index(f), t) for f, t in seq]
    if len(m['characters']) != 2:
        raise ValueError('exactly two characters are required')
    for f, t in steps:
        if t < 1:
            raise ValueError('step durations must be positive')
        out += struct.pack('<HH', f, t)
    for f, p in m['effects']:
        out += struct.pack('<HH', names.index(f), pal_names.index(p))
    body = struct.pack('<5I', int(automatic), len(palettes), len(names), len(steps),
                       len(m['effects'])) + out
    checksum = 2166136261
    for b in body:
        checksum = ((checksum ^ b) * 16777619) & 0xffffffff
    size = 16 + len(body)
    stats = {n: (len(frames[n][0][0]), len(frames[n][0]),
                 sum(c is not None for row in frames[n][0] for c in row)) for n in names}
    return MAGIC + struct.pack('<II', size, checksum) + body, checksum, len(colors), stats


def detect(sheet):
    print(f'background {sheet.m["background"]}, credit box rows {sheet.credit}')
    for x0, y, x1 in sheet.underlines:
        print(f'underline y={y} x={x0}..{x1}')
    for top, bottom in sheet.bands[1:]:
        print(f'rows {top}..{bottom}: ' + ' '.join(f'{a}..{b}' for a, b in sheet.segments(top, bottom)))


def preview(sheet, path):
    """One strip per action, frames on a shared baseline and anchor column grid."""
    m = sheet.m
    ch = m['characters'][0]
    strips = []
    for action in ACTIONS:
        strips.append([sheet.frame(f) for f, _ in ch['actions'][action]])
    cell_w, up, down = 160, 170, 10
    width = cell_w * max(len(s) for s in strips)
    height = (up + down) * len(strips)
    img = bytearray(b'\x30\x30\x40' * width * height)
    for row, strip in enumerate(strips):
        base = row * (up + down) + up
        for col, (pix, ax, ay) in enumerate(strip):
            ox = col * cell_w + cell_w // 2
            for x in range(col * cell_w, (col + 1) * cell_w):
                img[(base * width + x) * 3:(base * width + x) * 3 + 3] = b'\x80\x80\x80'
            for j, line in enumerate(pix):
                for i, c in enumerate(line):
                    X, Y = ox - ax + i, base - ay + j
                    if c and 0 <= X < width and 0 <= Y < height:
                        img[(Y * width + X) * 3:(Y * width + X) * 3 + 3] = c
            for y in range(base - up, base + down):
                p = (y * width + ox) * 3
                if img[p:p + 3] == b'\x30\x30\x40':
                    img[p:p + 3] = b'\x00\xc0\x00'
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
    args = ap.parse_args()
    sheet = Sheet(args.manifest)
    if args.cmd == 'detect':
        detect(sheet)
    elif args.cmd == 'preview':
        preview(sheet, args.output)
    else:
        data, checksum, ncolors, stats = build(sheet, args.auto)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        opaque = sum(s[2] for s in stats.values())
        print(f'{args.output}: {len(data)} bytes, hash {checksum:08x}, {len(stats)} frames, '
              f'{opaque} opaque pixels, {ncolors} colours')


if __name__ == '__main__':
    sys.exit(main())
