# The GBA ROM database (app/frontend/romdb_data.c), generated from two
# DAT files of libretro-database (No-Intro names, CRCs and serials, and
# release years) at a pinned commit, and committed: the full repository
# is large and the build needs no network for it. To update, set
# ROMDB_COMMIT to a newer commit, run `make romdb` (it prints the hashes
# of the new files when they do not match), check the files, put the new
# hashes here and run it again. mk/romdb-gen.py has the details; the
# data is CC BY-SA 4.0 (libretro-database's license).
#
#   make romdb    regenerate app/frontend/romdb_data.c (needs python3)

ROMDB_COMMIT := 12c21cf79f1e20eff6ac68b80875816c717d0a7a
ROMDB_URL := https://raw.githubusercontent.com/libretro/libretro-database/$(ROMDB_COMMIT)/metadat
ROMDB_DAT := Nintendo%20-%20Game%20Boy%20Advance.dat
ROMDB_DL := $(DL)/libretro-database-$(ROMDB_COMMIT)
ROMDB_SHA256_no-intro := 0b8bd3a414276e9c31137b0a245fcdaf917d34fd035e02499c965a290ac8fce4
ROMDB_SHA256_releaseyear := 80a10952a808809cb998953eeecac45c4f10c2a12fe4d355a18858a681967022
ROMDB_OUT := $(TOP)/app/frontend/romdb_data.c

$(ROMDB_DL)/%.dat:
	mkdir -p $(@D)
	curl -fL -o $@.tmp $(ROMDB_URL)/$*/$(ROMDB_DAT)
	echo "$(ROMDB_SHA256_$*)  $@.tmp" | sha256sum -c - || \
		{ sha256sum $@.tmp; exit 1; }
	mv $@.tmp $@

.PHONY: romdb
romdb: $(ROMDB_DL)/no-intro.dat $(ROMDB_DL)/releaseyear.dat \
		$(TOP)/mk/romdb-gen.py
	python3 -I $(TOP)/mk/romdb-gen.py $(ROMDB_COMMIT) \
		$(ROMDB_DL)/no-intro.dat $(ROMDB_DL)/releaseyear.dat > $(ROMDB_OUT).tmp
	mv $(ROMDB_OUT).tmp $(ROMDB_OUT)
	@echo "romdb: $$(grep -c '^	{ 0x' $(ROMDB_OUT)) entries in $(ROMDB_OUT)"
