# Mainline Linux kernel and the out-of-tree RG351P device tree.
# The kernel is built with O=$(BUILD)/linux so external/linux stays pristine.

LINUX_SRC := $(EXTERNAL)/linux
LINUX_BUILD := $(BUILD)/linux
LINUX_CONFIG := $(LINUX_BUILD)/.config
LINUX_IMAGE := $(LINUX_BUILD)/arch/arm64/boot/Image
LINUX_DTC := $(LINUX_BUILD)/scripts/dtc/dtc

# Debug fragment (fbcon on the panel, the frontend's debug log sources) is
# merged unless LINUX_DEBUG=0
LINUX_DEBUG ?= 1
LINUX_FRAGMENTS := $(BOARD_DIR)/linux/rg351p.config
ifeq ($(LINUX_DEBUG),1)
LINUX_FRAGMENTS += $(BOARD_DIR)/linux/debug.config
endif

LINUX_MAKE := $(MAKE) -C $(LINUX_SRC) O=$(LINUX_BUILD) ARCH=arm64 \
	CROSS_COMPILE=$(CROSS_COMPILE) -j$(JOBS)

# The fragment list and the kernel version are recorded so toggling
# LINUX_DEBUG or moving the submodule to another release regenerates
# .config (otherwise kbuild's syncconfig would silently take defaults)
LINUX_VERSION := $(shell sed -n 's/^\(VERSION\|PATCHLEVEL\|SUBLEVEL\) = //p' \
	$(LINUX_SRC)/Makefile | paste -sd.)
LINUX_FRAGMENTS_STAMP := $(LINUX_BUILD)/.fragments
$(shell mkdir -p $(LINUX_BUILD); \
	echo '$(LINUX_FRAGMENTS) $(LINUX_VERSION)' | \
	cmp -s - $(LINUX_FRAGMENTS_STAMP) || \
	echo '$(LINUX_FRAGMENTS) $(LINUX_VERSION)' > $(LINUX_FRAGMENTS_STAMP))

# allnoconfig first creates the O= wrapper Makefile, then merge_config.sh
# runs from the build dir so its temp files never land in external/linux.
# Fail if any requested symbol was dropped by Kconfig dependencies.
$(LINUX_CONFIG): $(LINUX_FRAGMENTS) $(LINUX_FRAGMENTS_STAMP) $(TOOLCHAIN_STAMP)
	$(LINUX_MAKE) allnoconfig
	cd $(LINUX_BUILD) && ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) \
		$(LINUX_SRC)/scripts/kconfig/merge_config.sh -n $(LINUX_FRAGMENTS) \
		> $(LINUX_BUILD)/merge_config.log
	@if grep -E -A2 'Value requested|differs:' $(LINUX_BUILD)/merge_config.log; then \
		rm -f $@; echo "linux: config fragment symbols did not stick"; exit 1; fi
	touch $@

# Always defer to kbuild, which only touches Image when something changed
$(LINUX_IMAGE): $(LINUX_CONFIG) FORCE
	$(LINUX_MAKE) Image

.PHONY: linux linux-menuconfig linux-savedefconfig
linux: $(LINUX_IMAGE)

linux-menuconfig: $(LINUX_CONFIG)
	$(LINUX_MAKE) menuconfig

linux-savedefconfig: $(LINUX_CONFIG)
	$(LINUX_MAKE) savedefconfig


# Device tree, preprocessed and compiled out of tree with the kernel's dtc
DTB_DIR := $(BUILD)/dtb
LINUX_DTS := $(BOARD_DIR)/linux/rk3326-anbernic-rg351p.dts
LINUX_DTB := $(DTB_DIR)/rk3326-anbernic-rg351p.dtb

DTC_CPP_FLAGS := -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I $(LINUX_SRC)/include \
	-I $(LINUX_SRC)/arch/arm64/boot/dts/rockchip \
	-I $(LINUX_SRC)/scripts/dtc/include-prefixes

# Same warning set as a default (W=0) kernel dtbs build
DTC_FLAGS := -I dts -O dtb -b 0 \
	-i $(LINUX_SRC)/arch/arm64/boot/dts/rockchip \
	-Wno-unique_unit_address -Wno-unit_address_vs_reg \
	-Wno-avoid_unnecessary_addr_size -Wno-alias_paths \
	-Wno-graph_child_address -Wno-interrupt_map -Wno-simple_bus_reg \
	-Wunique_unit_address_if_enabled

$(LINUX_DTC): | $(LINUX_CONFIG)
	$(LINUX_MAKE) scripts_dtc

$(LINUX_DTB): $(LINUX_DTS) $(LINUX_DTC)
	mkdir -p $(DTB_DIR)
	$(CROSS_COMPILE)gcc $(DTC_CPP_FLAGS) -MD -MT $@ -MF $@.d \
		-o $@.dts.tmp $<
	$(LINUX_DTC) $(DTC_FLAGS) -o $@ $@.dts.tmp

-include $(LINUX_DTB).d

.PHONY: dtb
dtb: $(LINUX_DTB)
