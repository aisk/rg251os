#!/usr/bin/env python3
"""
Generate app/frontend/font.c, the glyphs of the frontend's UI font, from
JetBrains Mono Regular (https://github.com/JetBrains/JetBrainsMono, SIL
OFL 1.1; JetBrainsMono-Regular.ttf and OFL.txt from a release):

  pip install freetype-py
  font-gen.py JetBrainsMono-Regular.ttf OFL.txt > app/frontend/font.c

Each printable ASCII glyph (0x20..0x7e) is rendered by FreeType at SIZE
px, hinted, in grayscale, with its origin at the left of a FONT_W x
FONT_H cell and its baseline on row BASELINE, and kept as 4-bit alpha,
two pixels a byte, the left one in the high nibble. Fails if a glyph's
advance is not FONT_W or any of its ink above alpha 1 falls outside the
cell; fainter fringe there is clipped. Prints the clipped pixels and the
ink extents to stderr. font.h, written by hand, has the size; the
generated file checks it at compile time.
"""
import sys

import freetype

FIRST, LAST = 0x20, 0x7e
W, H, SIZE, BASELINE = 12, 24, 20, 19


def main():
    face = freetype.Face(sys.argv[1])
    face.set_pixel_sizes(0, SIZE)
    names = {n.name_id: n.string.decode('utf-16-be') for n in
             (face.get_sfnt_name(i) for i in range(face.sfnt_name_count))
             if n.platform_id == 3}
    version = names[5].split(';')[0].replace('Version ', '')
    ofl = open(sys.argv[2], encoding='utf-8').read().rstrip().splitlines()

    glyphs, ext = {}, [W, 0, H, 0]
    for c in range(FIRST, LAST + 1):
        face.load_char(chr(c), freetype.FT_LOAD_RENDER)
        g = face.glyph
        b = g.bitmap
        if g.advance.x != W * 64:
            sys.exit(f'{chr(c)!r}: advance {g.advance.x / 64} is not {W}')
        cell = [[0] * W for _ in range(H)]
        for r in range(b.rows):
            for i in range(b.width):
                a = (b.buffer[r * b.pitch + i] * 15 + 127) // 255
                x, y = g.bitmap_left + i, BASELINE - g.bitmap_top + r
                if not a:
                    continue
                if not (0 <= x < W and 0 <= y < H):
                    if a > 1:
                        sys.exit(f'{chr(c)!r}: ink {a} at ({x}, {y}) '
                                 'outside the cell')
                    print(f'{chr(c)!r}: clipped alpha {a} at ({x}, {y})',
                          file=sys.stderr)
                    continue
                cell[y][x] = a
                ext = [min(ext[0], x), max(ext[1], x),
                       min(ext[2], y), max(ext[3], y)]
        glyphs[c] = cell
    print(f'ink: columns {ext[0]}..{ext[1]}, rows {ext[2]}..{ext[3]}',
          file=sys.stderr)

    license = '\n'.join((' * ' + l).rstrip() for l in ofl)
    print(f'''/*
 * The glyphs of the UI font (font.h): JetBrains Mono Regular {version}
 * (https://github.com/JetBrains/JetBrainsMono) at {SIZE} px, hinted and
 * anti-aliased by FreeType, for printable ASCII (0x20..0x7e). Generated
 * by mk/font-gen.py from JetBrainsMono-Regular.ttf and OFL.txt; do not
 * edit. Each glyph is {W}x{H} px of 4-bit alpha (0 clear, 15 opaque),
 * {H} rows top to bottom, two pixels a byte with the left one in the high
 * nibble; the baseline is row {BASELINE}.
 *
{license}
 */
#include "font.h"

_Static_assert(FONT_W == {W} && FONT_H == {H} && FONT_FIRST == 0x{FIRST:02x} &&
	       FONT_LAST == 0x{LAST:02x}, "font.h does not match the glyphs");

const uint8_t font_glyphs[FONT_LAST - FONT_FIRST + 1][FONT_H][FONT_W / 2] = {{''')
    for c in range(FIRST, LAST + 1):
        print(f'\t/* {chr(c)!r} */ {{')
        for row in glyphs[c]:
            print('\t\t{ ' + ', '.join(f'0x{row[i] << 4 | row[i + 1]:02x}'
                                       for i in range(0, W, 2)) + ' },')
        print('\t},')
    print('};')


main()
