# rg251os

A tiny Linux distribution for the Anbernic RG351P that only plays Game Boy Advance games, built to boot fast.

There is no userland in the usual sense. A trimmed mainline kernel with every driver built in boots without an initramfs straight into our own PID 1, which is the whole system: a ROM list, an in-game menu and two GBA emulators (gpSP and mGBA), all statically linked into one binary. No shell, no services, no GPU stack.

On real hardware the ROM list is on screen about 2.5 s after the kernel starts (measured with debug logging on, not tuned yet). The bootloader is still Rockchip's closed DDR init plus the ROCKNIX fork of U-Boot 2017.09, which adds its own delay on top. That stage has not been optimized and is where most of the remaining gain is.

## Status

Used for playing. Display, input, audio, saves, battery, both cores and power off work on hardware. Sleep, growing the data partition on first boot and the ROM name database still need more testing. See [docs/roadmap.md](docs/roadmap.md) for open issues and plans, and [docs/hardware-research.md](docs/hardware-research.md) for hardware notes, design decisions and the card layout (both in Chinese).

## Build

Host packages: `genimage mtools dosfstools fakeroot cmake ninja-build`, plus `qemu-user` for tests. The toolchain is downloaded automatically.

```sh
git clone https://github.com/aisk/rg251os
cd rg251os
git submodule update --init --depth 1
make image          # build/sdcard.img
make frontend-test
```

Or take an image from the releases. Write it to an SD card, boot once, then copy ROMs into `roms/` on the `GBA` partition.

## License

GPLv3, see [LICENSE](LICENSE). Submodules under `external/` keep their own licenses. The ROM name table in `app/frontend/romdb_data.c` is generated from No-Intro data via libretro-database and is CC BY-SA 4.0.
