# Root filesystem: the static frontend as /sbin/init, packed into a small
# read-only ext4 image.

ROOTFS_DIR := $(BUILD)/rootfs
ROOTFS_IMG := $(BUILD)/rootfs.ext4
ROOTFS_SIZE ?= 16M

MKE2FS ?= /usr/sbin/mke2fs
FAKEROOT ?= fakeroot

# The journal is dropped: the root is mounted read-only and this saves a
# little work at mount time. fakeroot makes every file owned by root.
$(ROOTFS_IMG): $(FRONTEND_BIN) $(TOP)/mk/rootfs.mk
	rm -rf $(ROOTFS_DIR) $@
	mkdir -p $(addprefix $(ROOTFS_DIR)/,dev proc sys tmp sbin mnt/data)
	chmod 1777 $(ROOTFS_DIR)/tmp
	install -m 0755 $(FRONTEND_BIN) $(ROOTFS_DIR)/sbin/init
	$(FAKEROOT) -- sh -c 'chown -R 0:0 $(ROOTFS_DIR) && \
		$(MKE2FS) -q -F -t ext4 -O ^has_journal -L rootfs \
		-d $(ROOTFS_DIR) $@ $(ROOTFS_SIZE)'

.PHONY: rootfs
rootfs: $(ROOTFS_IMG)
