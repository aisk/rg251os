# Top-level build for the RG351P GBA distro.
# All outputs go to $(BUILD); submodules under external/ are never modified.

TOP := $(abspath .)
BOARD ?= rg351p
BOARD_DIR := $(TOP)/board/$(BOARD)
BUILD := $(TOP)/build
DL := $(TOP)/dl
EXTERNAL := $(TOP)/external
JOBS ?= $(shell nproc)

.PHONY: all
all: image

include mk/toolchain.mk
-include mk/linux.mk
-include mk/bootloader.mk
-include mk/mgba.mk
-include mk/gpsp.mk
-include mk/tinyalsa.mk
-include mk/frontend.mk
-include mk/rootfs.mk
-include mk/image.mk

.PHONY: clean distclean
clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(DL)
