#!/usr/bin/env python3
"""Read MUGEN SFF v1 (PCX) and v2 (raw/RLE8) sprite files and export sprites
as PNG for the fighter's stage and effect manifests. Build machine tool
only; nothing here is packed directly. The exported PNGs keep the sprite's
palette colours exactly; palette index 0 becomes transparent (alpha 0)
unless --opaque is given.

  list FILE.sff                    group, image, size, axis and format of
                                   every sprite
  export FILE.sff OUT.png SPEC...  lay the sprites out left to right on one
                                   RGBA sheet with a one-pixel transparent
                                   border around each; SPEC is group,image
                                   (or group for all its images). Prints
                                   each sprite's rectangle and axis so a
                                   manifest can name them.
  [--opaque]                       keep index 0 as a colour (opaque layers)
  [--palette G,I]                  colour v1 sprites with sprite G,I's
                                   palette instead of their own

SFF v1: 512-byte header ("ElecbyteSpr", version 1.x, image count, first
subfile offset); each subfile: next offset, length, axis x/y, group, image,
linked index, same-palette flag, then an 8-bit PCX with a 768-byte palette
at its end (absent when the palette is shared with the previous sprite).
SFF v2: sprite nodes (group, image, w, h, axis, linked, format, depth,
offset, length, palette, flags) and palette nodes in the ldata/tdata
blocks; formats 0 (raw) and 2 (RLE8) are supported, others are reported.
"""
import argparse
import pathlib
import struct
import sys
import zlib


class Sprite:
    def __init__(self, group, image, w, h, ax, ay, pixels, palette, fmt):
        self.group, self.image, self.w, self.h = group, image, w, h
        self.ax, self.ay, self.pixels, self.palette, self.fmt = ax, ay, pixels, palette, fmt


def pcx(data):
    """(w, h, index bytes, palette or None) of an 8-bit single-plane PCX."""
    if len(data) < 128 or data[0] != 10 or data[3] != 8 or data[65] != 1:
        raise ValueError('only 8-bit single-plane PCX is supported')
    x0, y0, x1, y1 = struct.unpack('<4H', data[4:12])
    w, h = x1 - x0 + 1, y1 - y0 + 1
    stride = struct.unpack('<H', data[66:68])[0]
    out, pos = bytearray(), 128
    need = stride * h
    while len(out) < need and pos < len(data):
        b = data[pos]
        pos += 1
        if b >= 0xc0:
            out += bytes([data[pos]]) * (b & 0x3f)
            pos += 1
        else:
            out.append(b)
    if len(out) < need:
        raise ValueError('truncated PCX')
    pixels = b''.join(bytes(out[y * stride:y * stride + w]) for y in range(h))
    palette = None
    if len(data) >= 769 and data[-769] == 12:
        p = data[-768:]
        palette = [tuple(p[i * 3:i * 3 + 3]) for i in range(256)]
    return w, h, pixels, palette


def read_v1(data):
    count, first = struct.unpack('<II', data[20:28])
    sprites, pos, palette = [], first, None
    by_index = []
    for _ in range(count):
        nxt, length, ax, ay, group, image, linked, same = struct.unpack(
            '<IIhhHHHB', data[pos:pos + 19])
        if length == 0:
            ref = by_index[linked]
            spr = Sprite(group, image, ref.w, ref.h, ax, ay, ref.pixels, ref.palette, 'linked')
        else:
            w, h, pixels, pal = pcx(data[pos + 32:pos + 32 + length])
            if same or pal is None:
                pal = palette
            palette = pal
            spr = Sprite(group, image, w, h, ax, ay, pixels, pal, 'pcx')
        sprites.append(spr)
        by_index.append(spr)
        if not nxt:
            break
        pos = nxt
    return sprites


def rle8(src, n):
    out, pos = bytearray(), 0
    while len(out) < n and pos < len(src):
        b = src[pos]
        pos += 1
        if b & 0xc0 == 0x40:
            out += bytes([src[pos]]) * (b & 0x3f)
            pos += 1
        else:
            out.append(b)
    return bytes(out[:n])


def read_v2(data):
    (sprite_at, nsprites, palette_at, npalettes, ldata, _llen, tdata, _tlen) = struct.unpack(
        '<8I', data[36:68])
    palettes = []
    for i in range(npalettes):
        _g, _i, ncol, linked, off, length = struct.unpack(
            '<4HII', data[palette_at + 16 * i:palette_at + 16 * i + 16])
        if length == 0:
            palettes.append(palettes[linked])
            continue
        raw = data[ldata + off:ldata + off + length]
        palettes.append([tuple(raw[k * 4:k * 4 + 3]) for k in range(ncol)])
    sprites = []
    for i in range(nsprites):
        (group, image, w, h, ax, ay, linked, fmt, _depth, off, length, pal,
         flags) = struct.unpack('<HHHHhhHBBIIHH', data[sprite_at + 28 * i:sprite_at + 28 * i + 28])
        if length == 0:
            ref = sprites[linked]
            sprites.append(Sprite(group, image, ref.w, ref.h, ax, ay, ref.pixels, ref.palette,
                                  ref.fmt))
            continue
        base = (tdata if flags & 1 else ldata) + off
        raw = data[base:base + length]
        if fmt == 0:
            pixels = raw[:w * h]
        elif fmt == 2:
            pixels = rle8(raw[4:], w * h)
        else:
            pixels = None
        sprites.append(Sprite(group, image, w, h, ax, ay, pixels, palettes[pal] if pal <
                              len(palettes) else None, fmt))
    return sprites


def read_sff(path):
    data = pathlib.Path(path).read_bytes()
    if not data.startswith(b'ElecbyteSpr'):
        raise ValueError(f'{path}: not an SFF')
    major = data[15]
    if major == 1:
        return read_v1(data)
    if major == 2:
        return read_v2(data)
    raise ValueError(f'{path}: SFF version {major} is not supported')


def write_png(path, w, h, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack(
            '>I', zlib.crc32(kind + body) & 0xffffffff)
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    pathlib.Path(path).write_bytes(png)


def select(sprites, specs):
    out = []
    for spec in specs:
        parts = [int(v) for v in spec.split(',')]
        hit = [s for s in sprites if s.group == parts[0] and (len(parts) == 1 or
                                                              s.image == parts[1])]
        if not hit:
            raise ValueError(f'no sprite {spec}')
        out += sorted(hit, key=lambda s: s.image)
    return out


def export(sprites, out, specs, opaque, palette):
    chosen = select(sprites, specs)
    pal_override = select(sprites, [palette])[0].palette if palette else None
    width = sum(s.w + 1 for s in chosen) + 1
    height = max(s.h for s in chosen) + 2
    rgba = bytearray(width * height * 4)
    x = 1
    for s in chosen:
        if s.pixels is None:
            raise ValueError(f'sprite {s.group},{s.image}: format {s.fmt} is not supported')
        pal = pal_override or s.palette
        for j in range(s.h):
            for i in range(s.w):
                k = s.pixels[j * s.w + i]
                if k == 0 and not opaque:
                    continue
                p = ((j + 1) * width + x + i) * 4
                rgba[p:p + 4] = bytes(pal[k]) + b'\xff'
        print(f'{s.group},{s.image}: rect [{x}, 1, {s.w}, {s.h}] axis [{s.ax}, {s.ay}]')
        x += s.w + 1
    write_png(out, width, height, rgba)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    li = sub.add_parser('list')
    li.add_argument('sff')
    ex = sub.add_parser('export')
    ex.add_argument('sff')
    ex.add_argument('output')
    ex.add_argument('spec', nargs='+')
    ex.add_argument('--opaque', action='store_true')
    ex.add_argument('--palette')
    args = ap.parse_args()
    sprites = read_sff(args.sff)
    if args.cmd == 'list':
        for s in sprites:
            print(f'{s.group},{s.image} {s.w}x{s.h} axis {s.ax},{s.ay} {s.fmt}')
    else:
        export(sprites, args.output, args.spec, args.opaque, args.palette)


if __name__ == '__main__':
    sys.exit(main())
