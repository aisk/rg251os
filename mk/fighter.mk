# The fighting game core, external/fighter (a submodule with its own
# Makefile, tests and content), is optional. FIGHTER=1 builds it with the
# cross toolchain as a static renamed core (fighter_retro_*), builds its
# packs, compiles the frontend with -DHAVE_CORE_FIGHTER so cores.c links it
# and romlist.c lists *.fight, ships the packs in the image as
# roms/Fighter.fight and roms/Fighter.fightsnd (the core finds the bank by
# the pack's name) and runs the engine's own tests plus the qemu frontend
# integration test (tools/fighter/frontend-test.sh) in frontend-test. With
# FIGHTER=0 (the default) nothing of it is built or linked and the image
# has no packs. The engine's build lives under build/fighter/engine; the
# dependencies below are its sources and content, so a touched manifest or
# source rebuilds the pack or the core.
FIGHTER ?= 0
FIGHTER_SRC := $(EXTERNAL)/fighter
FIGHTER_BUILD := $(BUILD)/fighter
FIGHTER_ENGINE := $(FIGHTER_BUILD)/engine

ifeq ($(FIGHTER),1)
ifeq ($(wildcard $(FIGHTER_SRC)/Makefile),)
$(error FIGHTER=1 needs the external/fighter submodule: git submodule update --init external/fighter)
endif
FIGHTER_LIB := $(FIGHTER_BUILD)/libfighter.a
FIGHTER_RAW_LIB := $(FIGHTER_ENGINE)/libfighter.a
FIGHTER_CONTENT := $(FIGHTER_ENGINE)/Fighter.fight $(FIGHTER_ENGINE)/Fighter-Auto.fight
FIGHTER_SOUND := $(FIGHTER_CONTENT:.fight=.fightsnd)
FIGHTER_CFLAGS := -DHAVE_CORE_FIGHTER
FIGHTER_SOURCES := $(wildcard $(FIGHTER_SRC)/src/*.c $(FIGHTER_SRC)/src/*.h \
	$(FIGHTER_SRC)/third_party/*.h) $(FIGHTER_SRC)/Makefile
FIGHTER_ASSETS := $(wildcard $(FIGHTER_SRC)/assets/*.json $(FIGHTER_SRC)/assets/*.png \
	$(FIGHTER_SRC)/assets/sound/*.json $(FIGHTER_SRC)/assets/sound/sfx/*.wav \
	$(FIGHTER_SRC)/assets/sound/music/*) $(FIGHTER_SRC)/tools/pack.py $(FIGHTER_SRC)/tools/sound.py
FIGHTER_MAKE := $(MAKE) -C $(FIGHTER_SRC) BUILD=$(FIGHTER_ENGINE)

$(FIGHTER_RAW_LIB): $(FIGHTER_SOURCES) $(TOP)/mk/fighter.mk | $(TOOLCHAIN_STAMP)
	$(FIGHTER_MAKE) static CROSS_COMPILE=$(CROSS_COMPILE) ARCH_FLAGS=-mcpu=cortex-a35

$(FIGHTER_LIB): $(FIGHTER_RAW_LIB) $(TOP)/mk/core-archive.sh
	$(TOP)/mk/core-archive.sh fighter $@ $<

$(FIGHTER_CONTENT) $(FIGHTER_SOUND) &: $(FIGHTER_ASSETS) $(FIGHTER_SRC)/src/fighter.h
	$(FIGHTER_MAKE) packs

.PHONY: fighter fighter-test
fighter: $(FIGHTER_LIB) $(FIGHTER_CONTENT) $(FIGHTER_SOUND)
fighter-test: $(FIGHTER_CONTENT) $(FIGHTER_SOUND)
	$(FIGHTER_MAKE) test
else
FIGHTER_LIB :=
FIGHTER_CONTENT :=
FIGHTER_SOUND :=
FIGHTER_CFLAGS :=
.PHONY: fighter fighter-test
fighter fighter-test:
	@echo "fighter: not built (FIGHTER=1 builds external/fighter)"
endif
