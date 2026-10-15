# SD card image assembled with genimage from the kernel, dtb, U-Boot and
# rootfs. Needs genimage, mtools and dosfstools on the host.

IMAGES_DIR := $(BUILD)/images
SDCARD_IMG := $(BUILD)/sdcard.img
GENIMAGE_CFG := $(BOARD_DIR)/genimage.cfg
GENIMAGE_TMP := $(BUILD)/genimage.tmp
EXTLINUX_CONF := $(BOARD_DIR)/extlinux.conf

GENIMAGE ?= genimage
# mkdosfs and friends live in /usr/sbin, which may not be in PATH.
IMAGE_PATH := /usr/sbin:/sbin:$(PATH)

$(SDCARD_IMG): $(LINUX_IMAGE) $(LINUX_DTB) $(UBOOT_BIN) $(ROOTFS_IMG) \
		$(GENIMAGE_CFG) $(EXTLINUX_CONF)
	@if [ -z "$(LINUX_IMAGE)" ] || [ -z "$(LINUX_DTB)" ] || [ -z "$(UBOOT_BIN)" ]; then \
		echo "error: LINUX_IMAGE, LINUX_DTB or UBOOT_BIN undefined (mk/linux.mk, mk/bootloader.mk)" >&2; \
		exit 1; \
	fi
	@missing=; \
	for t in $(GENIMAGE):genimage mcopy:mtools mmd:mtools mkdosfs:dosfstools; do \
		PATH=$(IMAGE_PATH) command -v $${t%%:*} >/dev/null 2>&1 || missing="$$missing $${t#*:}"; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "error: missing host tools, install with:" >&2; \
		echo "  sudo apt install genimage mtools dosfstools" >&2; \
		exit 1; \
	fi
	rm -rf $(IMAGES_DIR) $(GENIMAGE_TMP)
	mkdir -p $(IMAGES_DIR)/extlinux $(GENIMAGE_TMP)/root
	cp $(LINUX_IMAGE) $(IMAGES_DIR)/Image
	cp $(LINUX_DTB) $(IMAGES_DIR)/rk3326-anbernic-rg351p.dtb
	cp $(EXTLINUX_CONF) $(IMAGES_DIR)/extlinux/extlinux.conf
	cp $(UBOOT_BIN) $(IMAGES_DIR)/u-boot-rockchip.bin
	cp $(ROOTFS_IMG) $(IMAGES_DIR)/rootfs.ext4
	PATH=$(IMAGE_PATH) $(GENIMAGE) \
		--config $(GENIMAGE_CFG) \
		--rootpath $(GENIMAGE_TMP)/root \
		--tmppath $(GENIMAGE_TMP)/tmp \
		--inputpath $(IMAGES_DIR) \
		--outputpath $(IMAGES_DIR)
	mv $(IMAGES_DIR)/sdcard.img $@
	@echo "SD card image: $@"

.PHONY: image
image: $(SDCARD_IMG)
