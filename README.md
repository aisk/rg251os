# rg251os

A tiny Linux distribution for the Anbernic RG351P that only plays Game Boy Advance games, built to boot fast.

There is no userland in the usual sense. A trimmed mainline kernel with every driver built in boots without an initramfs straight into our own PID 1, which is the whole system: a ROM list, an in-game menu and two GBA emulators (gpSP and mGBA), all statically linked into one binary. No shell, no services, no GPU stack. Powering off during a game works like sleep: its state is saved and the next boot goes straight back into it (hold B while booting for the ROM list).

On real hardware the ROM list is on screen about 2.5 s after the kernel starts (measured with debug logging on, not tuned yet). The bootloader is still Rockchip's closed DDR init plus the ROCKNIX fork of U-Boot 2017.09, which adds its own delay on top. That stage has not been optimized and is where most of the remaining gain is.

## Status

Used for playing. Display, input, audio, saves, battery, both cores and power off work on hardware. Sleep, growing the data partition on first boot and the ROM name database still need more testing. See [docs/roadmap.md](docs/roadmap.md) for open issues and plans, and [docs/hardware-research.md](docs/hardware-research.md) for hardware notes, design decisions and the card layout (both in Chinese).

It also plays videos. To bring back the feel of the cheap "MP4" players of the 2000s, only MPEG-1 is supported, so videos have to be converted first and copied into `roms/`, where the `.mpg` files show up next to the games. For example, to fit the 480x320 screen:

```sh
ffmpeg -i in.mp4 -vf scale=480:-2 -r 30 -c:v mpeg1video -q:v 2 -mbd rd -trellis 2 \
  -c:a mp2 -b:a 128k out.mpg
```

While a video plays, A pauses, Left/Right seek 10 s and Down/Up 60 s (hold to keep seeking), L/R go to the previous/next video in the same folder, and B toggles the on-screen display with the name, time and progress. When a video ends the next one in the folder starts by itself. SELECT+START opens the menu as in games, and each video resumes where it was left.

## Build

Host packages: `genimage mtools dosfstools fakeroot cmake ninja-build`, plus `qemu-user` and `ffmpeg` for tests. The toolchain is downloaded automatically.

```sh
git clone https://github.com/aisk/rg251os
cd rg251os
git submodule update --init --depth 1
make image          # build/sdcard.img
make frontend-test
```

Or take an image from the releases. Write it to an SD card, boot once, then copy ROMs into `roms/` on the `GBA` partition.

## License

GPLv3, see [LICENSE](LICENSE). Submodules under `external/` keep their own licenses. The ROM name table in `app/frontend/romdb_data.c` is generated from No-Intro data via libretro-database and is CC BY-SA 4.0. The UI font in `app/frontend/font.c` is rendered from JetBrains Mono by The JetBrains Mono Project Authors and is SIL OFL 1.1 (license text in the file).
