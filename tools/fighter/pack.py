#!/usr/bin/env python3
"""Generate original synthetic POC sprites, or pack exactly 16 P6 PPM frames.

No commercial artwork is bundled. Magenta pixels are transparent in PPM input.
The fixed layout is two fighters, each: idle A/B, jump, windup, strike, recovery,
hurt, KO. All frames are 128x160 with feet at (64,160). Real asset imports must
preserve those anchors; this intentionally small POC format is not a final SDK.
"""
import argparse
import pathlib
import struct

W, H, N = 128, 160, 16


def synthetic(index):
    pixels = [(255, 0, 255)] * (W * H)
    who, pose = divmod(index, 8)
    shift = 2 if pose == 1 else 0
    def box(x, y, w, h, c):
        for yy in range(max(0, y), min(H, y+h)):
            for xx in range(max(0, x), min(W, x+w)):
                pixels[yy*W+xx] = c
    skin = (232, 184, 128)
    cloth = (184, 40, 48) if who == 0 else (224, 224, 208)
    dark = (40, 48, 72)
    if pose == 7:
        box(16, 133, 82, 20, cloth)
        box(98, 130, 23, 23, skin)
    else:
        # Shaded silhouettes exercise transparent spans, clipping and mirroring.
        box(44, 10+shift, 29, 31, dark)
        box(48, 20+shift, 27, 27, skin)
        box(36, 48+shift, 43, 57, cloth)
        box(36, 48+shift, 8, 57, (96, 64, 80))
        box(36, 101, 44, 8, dark)
        box(35, 109, 18, 44, dark)
        box(66, 109, 18, 44, dark)
        box(28, 150, 26, 10, (112, 96, 72))
        box(65, 150, 28, 10, (112, 96, 72))
        if pose == 4:
            box(72, 50, 49, 15, skin)
            if who:
                box(91, 43, 37, 5, (232, 240, 248))
                box(89, 36, 4, 24, (200, 160, 48))
        else:
            box(77, 58+shift, 14, 34, skin)
            box(83, 45+shift, 16, 17, skin)
            if who:
                box(94, 3, 4, 70, (232, 240, 248))
                box(87, 70, 18, 4, (200, 160, 48))
        if pose == 6:
            box(22, 56, 14, 32, skin)
    return pixels


def ppm(path):
    data = pathlib.Path(path).read_bytes()
    # Narrow, explicit import format: binary RGB, no comments in the header.
    parts = data.split(b'\n', 3)
    if len(parts) != 4 or parts[:3] != [b'P6', b'128 160', b'255'] or len(parts[3]) != W*H*3:
        raise ValueError(f'{path}: expected P6\\n128 160\\n255\\n and RGB bytes')
    return list(zip(parts[3][::3], parts[3][1::3], parts[3][2::3]))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=pathlib.Path)
    ap.add_argument('--auto', action='store_true')
    ap.add_argument('--frames', nargs=16)
    args = ap.parse_args()
    payload = bytearray()
    for i in range(N):
        for r, g, b in ppm(args.frames[i]) if args.frames else synthetic(i):
            color = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
            payload += struct.pack('<HB', color, 0 if (r, g, b) == (255, 0, 255) else 255)
    checksum = 2166136261
    for b in payload:
        checksum = ((checksum ^ b) * 16777619) & 0xffffffff
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(b'FIGHTP01'+struct.pack('<6I',W,H,N,int(args.auto),checksum,0)+payload)
    print(f'{args.output}: {len(payload)+32} bytes, hash {checksum:08x}, '
          f'{"imported PPM frames" if args.frames else "synthetic fixtures"}')


if __name__ == '__main__':
    main()
