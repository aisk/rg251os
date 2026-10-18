# rg251os

A tiny Linux distribution for the Anbernic RG351P that does one thing: play Game Boy Advance games.

## Why

The usual handheld firmwares (ROCKNIX, AmberELEC, ArkOS) ship EmulationStation, RetroArch, dozens of emulators, a full userland and a pile of services. That is great for a general purpose device, but it means slow boots, more moving parts and a lot of things to configure. This project goes the other way: a mainline kernel, a single static program as PID 1, and nothing else. Power on, pick a game, play.

## How it works

The SD card holds a raw bootloader, a FAT boot partition, a read-only ext4 rootfs and a FAT partition named `GBA` for ROMs, saves, settings and logs. The `GBA` partition grows to fill the card on first boot.

Boot chain: Rockchip's DDR init and miniloader, the ROCKNIX fork of U-Boot 2017.09, then a mainline Linux 6.18 kernel with a board DTS derived from the upstream RG351M one. Mainline U-Boot is kept buildable (`BOOTLOADER=mainline`) but does not boot on this hardware yet.

The rootfs is essentially one binary, `app/frontend`, running as init. It mounts the data partition, drives the panel through KMS dumb buffers (RGB565, 2x integer scale with 270° rotation done on the CPU), reads the built-in gamepad through evdev, and plays audio through tinyalsa with a small resampler and rate control that keeps audio locked to the 60.02 Hz panel. Two libretro cores are linked statically into the same binary, mGBA and gpSP (ARM64 dynarec), with their symbols prefixed so they can coexist. The GPU is not used at all.

Around that sit the things a handheld needs: a ROM list with No-Intro names, an in-game menu, per-game settings, battery readout, adaptive cpufreq through uclamp based on frame timing, suspend on the power key, an LCD grid effect, GBA screen color correction and gpSP's frame blending.

Everything is built from git submodules under `external/` with a Bootlin aarch64 musl toolchain, driven by plain Makefiles in `mk/`.

## Status

It works and is used to play games. Verified on hardware: boot, display, input, audio on speaker and headphones, saves, battery, power off and both cores. Sleep, the first boot partition growth and the ROM name database are newer and still need more testing on real cards. Boot time has not been optimized yet. There is no CJK font, so non-ASCII file names do not display.

Hardware notes, decisions and debugging history are in [docs/hardware-research.md](docs/hardware-research.md) (Chinese).

## Building

Host packages (Debian/Ubuntu names): `genimage mtools dosfstools fakeroot cmake ninja-build`, plus `qemu-user` for the tests. The toolchain is downloaded automatically.

```sh
git clone https://github.com/aisk/rg251os
cd rg251os
git submodule update --init --depth 1
make image          # build/sdcard.img
make frontend-test  # frontend tests under qemu
```

Write `build/sdcard.img` to an SD card, boot, then copy ROMs into `roms/` on the `GBA` partition. An optional GBA BIOS goes to `bios/gba_bios.bin`; without it the cores use their built-in replacements.

## Controls

SELECT in the ROM list opens settings. SELECT+START in game opens the menu (volume, brightness, CPU mode, core, frame blend, settings, quit). Color correction and the LCD grid are in settings. A short press on the power key sleeps or wakes, holding it for two seconds powers off.

## Logs

Enable "Debug log" in settings to get detailed per-boot logs under `log/` on the `GBA` partition. That directory is what to send along with a bug report.

## License

GPLv3, see [LICENSE](LICENSE). The submodules under `external/` keep their own licenses. The ROM name table in `app/frontend/romdb_data.c` is generated from No-Intro data via libretro-database and is CC BY-SA 4.0.
