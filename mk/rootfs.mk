# Root filesystem: the static frontend as /sbin/init plus the hwtest
# binary as /sbin/hwtest (boot it with init=/sbin/hwtest on the kernel
# command line), packed into a small read-only ext4 image.

HWTEST_SRC := $(TOP)/app/hwtest/hwtest.c
HWTEST_DEPS := $(HWTEST_SRC) $(TOP)/app/hwtest/font8x8.h
HWTEST_BIN := $(BUILD)/hwtest/hwtest
HWTEST_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wshadow -static

ROOTFS_DIR := $(BUILD)/rootfs
ROOTFS_IMG := $(BUILD)/rootfs.ext4
ROOTFS_SIZE ?= 16M

MKE2FS ?= /usr/sbin/mke2fs
FAKEROOT ?= fakeroot

$(HWTEST_BIN): $(HWTEST_DEPS) $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(HWTEST_CFLAGS) -o $@ $(HWTEST_SRC)
	$(CROSS_COMPILE)strip $@

# The journal is dropped: the root is mounted read-only and this saves a
# little work at mount time. fakeroot makes every file owned by root.
$(ROOTFS_IMG): $(FRONTEND_BIN) $(HWTEST_BIN) $(TOP)/mk/rootfs.mk
	rm -rf $(ROOTFS_DIR) $@
	mkdir -p $(addprefix $(ROOTFS_DIR)/,dev proc sys tmp sbin mnt/data)
	chmod 1777 $(ROOTFS_DIR)/tmp
	install -m 0755 $(FRONTEND_BIN) $(ROOTFS_DIR)/sbin/init
	install -m 0755 $(HWTEST_BIN) $(ROOTFS_DIR)/sbin/hwtest
	$(FAKEROOT) -- sh -c 'chown -R 0:0 $(ROOTFS_DIR) && \
		$(MKE2FS) -q -F -t ext4 -O ^has_journal -L rootfs \
		-d $(ROOTFS_DIR) $@ $(ROOTFS_SIZE)'

.PHONY: hwtest rootfs
hwtest: $(HWTEST_BIN)
rootfs: $(ROOTFS_IMG)
