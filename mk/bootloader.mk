# Bootloader for the RG351P: TF-A BL31 + mainline U-Boot (TPL/SPL/proper),
# packed by binman into u-boot-rockchip.bin, written at 32KiB (sector 64)
# of the SD card. The FIT with U-Boot proper and BL31 sits at
# CONFIG_SPL_PAD_TO (0x7f8000) inside the image, i.e. 8MiB on the card, so
# the first partition has to start after ~10MiB.
#
# Selectable pieces:
#   DDR_INIT=rkbin  Rockchip DDR blob as TPL (default, known to work, ROCKNIX)
#   DDR_INIT=open   U-Boot's open source TPL DRAM init (sdram_px30.c)
#   BL31=tfa        mainline TF-A, PLAT=px30 (default)
#   BL31=rkbin      Rockchip prebuilt BL31
#
# Host requirements for U-Boot: binman needs pylibfdt (built from U-Boot's
# dtc with swig + setuptools) and pyelftools; mkimage needs libssl. U-Boot
# builds its own dtc, and mkeficapsule (gnutls, libuuid) is disabled.
#   apt install swig python3-setuptools python3-pyelftools python3-dev \
#               libssl-dev bison flex
# TF-A on px30 has no Cortex-M coprocessor firmware, the aarch64 cross
# compiler is all it needs.
#
# Outputs are real files with explicit prerequisites. The submodules are
# pinned and never edited, so source changes are not tracked: after updating
# external/u-boot or external/trusted-firmware-a run `make bootloader-clean`.

DDR_INIT ?= rkbin
BL31 ?= tfa

ifeq ($(filter $(DDR_INIT),rkbin open),)
$(error DDR_INIT must be "rkbin" or "open", got "$(DDR_INIT)")
endif
ifeq ($(filter $(BL31),tfa rkbin),)
$(error BL31 must be "tfa" or "rkbin", got "$(BL31)")
endif

# Rockchip blobs, pinned to the newest RK3326 versions in external/rkbin
RKBIN_DIR := $(EXTERNAL)/rkbin/bin/rk33
RKBIN_DDR := $(RKBIN_DIR)/rk3326_ddr_333MHz_v2.12.bin
RKBIN_BL31 := $(RKBIN_DIR)/rk3326_bl31_v1.37.elf

# ---------------------------------------------------------------------------
# TF-A
# ---------------------------------------------------------------------------

TFA_SRC := $(EXTERNAL)/trusted-firmware-a
TFA_BUILD := $(BUILD)/tf-a
TFA_BL31 := $(TFA_BUILD)/px30/release/bl31/bl31.elf

# BL31= is passed empty on purpose: our BL31 selector would otherwise leak
# through MAKEFLAGS, and a non-empty BL31 tells TF-A to use a prebuilt image
# instead of building one.
$(TFA_BL31): | $(TOOLCHAIN_STAMP)
	$(MAKE) -C $(TFA_SRC) -j$(JOBS) \
		CROSS_COMPILE=$(CROSS_COMPILE) PLAT=px30 BUILD_BASE=$(TFA_BUILD) \
		BL31= bl31

.PHONY: tf-a
tf-a: $(TFA_BL31)

ifeq ($(BL31),tfa)
BL31_FILE := $(TFA_BL31)
else
BL31_FILE := $(RKBIN_BL31)
endif

ifeq ($(DDR_INIT),rkbin)
UBOOT_TPL_ARG := ROCKCHIP_TPL=$(RKBIN_DDR)
UBOOT_TPL_FILE := $(RKBIN_DDR)
endif

# ---------------------------------------------------------------------------
# U-Boot
# ---------------------------------------------------------------------------

UBOOT_SRC := $(EXTERNAL)/u-boot
UBOOT_BUILD := $(BUILD)/u-boot
UBOOT_CFG_DIR := $(BOARD_DIR)/u-boot
UBOOT_DEFCONFIG := odroid-go2_defconfig
UBOOT_DTSI := $(UBOOT_CFG_DIR)/rk3326-rg351p-u-boot.dtsi
UBOOT_FRAGMENTS := $(UBOOT_CFG_DIR)/rg351p.config \
	$(UBOOT_CFG_DIR)/ddr-$(DDR_INIT).config
# Fragment with absolute paths, generated next to the build
UBOOT_GEN_FRAGMENT := $(UBOOT_BUILD)/rg351p-generated.config
UBOOT_CONFIG := $(UBOOT_BUILD)/.config
UBOOT_BIN := $(UBOOT_BUILD)/u-boot-rockchip.bin

UBOOT_MAKE = $(MAKE) -C $(UBOOT_SRC) O=$(UBOOT_BUILD) \
	CROSS_COMPILE=$(CROSS_COMPILE)

# Marker files record the selected variant so that switching DDR_INIT
# regenerates .config and switching BL31 re-runs binman.
$(UBOOT_BUILD)/.ddr-init-%:
	mkdir -p $(UBOOT_BUILD)
	rm -f $(UBOOT_BUILD)/.ddr-init-*
	touch $@

$(UBOOT_BUILD)/.bl31-%:
	mkdir -p $(UBOOT_BUILD)
	rm -f $(UBOOT_BUILD)/.bl31-*
	touch $@

# odroid-go2_defconfig + our fragments, then verify nothing was dropped
$(UBOOT_CONFIG): $(UBOOT_SRC)/configs/$(UBOOT_DEFCONFIG) $(UBOOT_FRAGMENTS) \
		$(UBOOT_BUILD)/.ddr-init-$(DDR_INIT) | $(TOOLCHAIN_STAMP)
	mkdir -p $(UBOOT_BUILD)
	$(UBOOT_MAKE) $(UBOOT_DEFCONFIG)
	printf 'CONFIG_DEVICE_TREE_INCLUDES="%s"\n' '$(UBOOT_DTSI)' > $(UBOOT_GEN_FRAGMENT)
	cd $(UBOOT_BUILD) && $(UBOOT_SRC)/scripts/kconfig/merge_config.sh -m \
		.config $(UBOOT_FRAGMENTS) $(UBOOT_GEN_FRAGMENT)
	$(UBOOT_MAKE) olddefconfig
	$(UBOOT_CFG_DIR)/check-config.sh $@ $(UBOOT_FRAGMENTS) $(UBOOT_GEN_FRAGMENT)

# BL31 (and ROCKCHIP_TPL for DDR_INIT=rkbin) are consumed by binman.
$(UBOOT_BIN): $(UBOOT_CONFIG) $(UBOOT_DTSI) $(BL31_FILE) $(UBOOT_TPL_FILE) \
		$(UBOOT_BUILD)/.bl31-$(BL31)
	$(UBOOT_MAKE) -j$(JOBS) BL31=$(BL31_FILE) $(UBOOT_TPL_ARG)
	touch $@

.PHONY: u-boot u-boot-config u-boot-menuconfig bootloader-clean
u-boot: $(UBOOT_BIN)

u-boot-config: $(UBOOT_CONFIG)

# Interactive tweaking only; persist changes in board/rg351p/u-boot/*.config
u-boot-menuconfig: $(UBOOT_CONFIG)
	$(UBOOT_MAKE) menuconfig

bootloader-clean:
	rm -rf $(UBOOT_BUILD) $(TFA_BUILD)
