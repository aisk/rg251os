# Legacy bootloader for the RG351P: ROCKNIX's Hardkernel U-Boot 2017.09
# (external/u-boot-legacy) with the Rockchip miniloader and BL31 from rkbin,
# as shipped by ROCKNIX "A" images on the RG351 family. Mainline U-Boot does
# not get past SPL on the device yet, see mk/bootloader.mk.
#
# Card layout, all relative to the start of the SD card:
#   sector    64 (32KiB)  idbloader.img: rkbin DDR init + miniloader
#   sector 16384 (8MiB)   uboot.img: U-Boot proper, 4 copies of 1MiB
#   sector 24576 (12MiB)  trust.img: BL31, 2 copies of 2MiB
# They are packed into one blob from sector 64 up to exactly 16MiB, so the
# first partition can start at 16MiB.
#
# U-Boot runs boot.ini from the first FAT partition (cfgload), which hands
# over to extlinux.conf via sysboot.

LEGACY_SRC := $(EXTERNAL)/u-boot-legacy
LEGACY_BUILD := $(BUILD)/u-boot-legacy
LEGACY_DEFCONFIG := odroidgoa_defconfig
LEGACY_FRAGMENT := $(BOARD_DIR)/u-boot-legacy/rg351p.config
LEGACY_CONFIG := $(LEGACY_BUILD)/.config
LEGACY_UBOOT := $(LEGACY_BUILD)/u-boot-dtb.bin
LEGACY_OUT := $(LEGACY_BUILD)/pack
LEGACY_BIN := $(LEGACY_OUT)/u-boot-legacy.bin

# Same blob family ROCKNIX uses (it pins ddr v2.11 and bl31 v1.34)
LEGACY_DDR := $(EXTERNAL)/rkbin/bin/rk33/rk3326_ddr_333MHz_v2.12.bin
LEGACY_MINILOADER := $(EXTERNAL)/rkbin/bin/rk33/rk3326_miniloader_v1.40.bin
LEGACY_BL31 := $(EXTERNAL)/rkbin/bin/rk33/rk3326_bl31_v1.37.elf
RKBIN_TOOLS := $(EXTERNAL)/rkbin/tools

# gcc 15 defaults to C23, whose <stddef.h> defines unreachable() and
# clashes with include/linux/compiler-gcc.h under -Werror.
LEGACY_MAKE = $(MAKE) -C $(LEGACY_SRC) O=$(LEGACY_BUILD) ARCH=arm \
	CROSS_COMPILE=$(CROSS_COMPILE) KCFLAGS=-std=gnu11

# The defconfig is a full .config; merge our fragment and verify it held.
$(LEGACY_CONFIG): $(LEGACY_SRC)/configs/$(LEGACY_DEFCONFIG) $(LEGACY_FRAGMENT) \
		| $(TOOLCHAIN_STAMP)
	mkdir -p $(LEGACY_BUILD)
	cp $< $@
	cd $(LEGACY_BUILD) && $(LEGACY_SRC)/scripts/kconfig/merge_config.sh -m \
		.config $(LEGACY_FRAGMENT)
	$(LEGACY_MAKE) olddefconfig
	$(BOARD_DIR)/u-boot/check-config.sh $@ $(LEGACY_FRAGMENT)

$(LEGACY_UBOOT): $(LEGACY_CONFIG)
	$(LEGACY_MAKE) -j$(JOBS)
	touch $@

$(LEGACY_BIN): $(LEGACY_UBOOT) $(LEGACY_DDR) $(LEGACY_MINILOADER) $(LEGACY_BL31)
	rm -rf $(LEGACY_OUT)
	mkdir -p $(LEGACY_OUT)
	$(LEGACY_BUILD)/tools/mkimage -n px30 -T rksd -d $(LEGACY_DDR) \
		$(LEGACY_OUT)/idbloader.img
	cat $(LEGACY_MINILOADER) >> $(LEGACY_OUT)/idbloader.img
	cd $(LEGACY_OUT) && $(RKBIN_TOOLS)/loaderimage --pack --uboot \
		$(LEGACY_UBOOT) uboot.img 0x00200000
	printf '[BL30_OPTION]\nSEC=0\n[BL31_OPTION]\nSEC=1\nPATH=%s\nADDR=0x00010000\n[BL32_OPTION]\nSEC=0\n[BL33_OPTION]\nSEC=0\n[OUTPUT]\nPATH=trust.img\n' \
		'$(LEGACY_BL31)' > $(LEGACY_OUT)/trust.ini
	cd $(LEGACY_OUT) && $(RKBIN_TOOLS)/trust_merger trust.ini
	# Offsets are in sectors relative to sector 64, where the blob is written
	truncate -s $$(( (32768 - 64) * 512 )) $@
	dd if=$(LEGACY_OUT)/idbloader.img of=$@ conv=notrunc status=none
	dd if=$(LEGACY_OUT)/uboot.img of=$@ bs=512 seek=$$((16384 - 64)) conv=notrunc status=none
	dd if=$(LEGACY_OUT)/trust.img of=$@ bs=512 seek=$$((24576 - 64)) conv=notrunc status=none
	@test $$(stat -c %s $@) -eq $$(( (32768 - 64) * 512 )) || \
		{ echo "error: $@ grew past 16MiB" >&2; exit 1; }

.PHONY: u-boot-legacy u-boot-legacy-clean
u-boot-legacy: $(LEGACY_BIN)

u-boot-legacy-clean:
	rm -rf $(LEGACY_BUILD)
