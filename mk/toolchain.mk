# Bootlin prebuilt aarch64 musl toolchain (gcc 15.3, binutils 2.45.1, musl 1.2.6)

TOOLCHAIN_NAME := aarch64--musl--stable-2026.08-1
TOOLCHAIN_URL := https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/$(TOOLCHAIN_NAME).tar.xz
TOOLCHAIN_SHA256 := b388c480a48e8e9f9b99e3d14e69219c4d61e5a2424a82faecb88a015b781a60
TOOLCHAIN_DIR := $(BUILD)/toolchain/$(TOOLCHAIN_NAME)
TOOLCHAIN_STAMP := $(TOOLCHAIN_DIR)/.stamp

CROSS_COMPILE := $(TOOLCHAIN_DIR)/bin/aarch64-linux-
export CROSS_COMPILE

$(DL)/$(TOOLCHAIN_NAME).tar.xz:
	mkdir -p $(DL)
	curl -fL -o $@.tmp $(TOOLCHAIN_URL)
	echo "$(TOOLCHAIN_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

$(TOOLCHAIN_STAMP): $(DL)/$(TOOLCHAIN_NAME).tar.xz
	rm -rf $(TOOLCHAIN_DIR)
	mkdir -p $(BUILD)/toolchain
	tar -xJf $< -C $(BUILD)/toolchain
	$(TOOLCHAIN_DIR)/relocate-sdk.sh >/dev/null 2>&1
	touch $@

.PHONY: toolchain
toolchain: $(TOOLCHAIN_STAMP)
