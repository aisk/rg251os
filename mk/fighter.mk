# Portable fighter (P0): static renamed libretro core for the handheld,
# native shared libretro core and callback harness for desktop verification.
# Content packs are built from the roster (assets/fighter/roster.json), the
# system profiles (assets/fighter/systems.json) and the sprite sheets and
# manifests it lists; POC is the arcade ladder vs CPU, Auto runs AI vs AI
# through the roster and the four timing scenes. fighter-test also checks
# pack.py's stats/system validation and the manifest conversion
# (tools/fighter/pack-test.py). Each pack gets a sound bank next to it
# (<pack>.fightsnd: ADPCM effects, MP2 music, event and music cues) built by
# tools/fighter/sound.py from assets/fighter/sound; the core links its own
# pl_mpeg copy (mp2.c, -O3 like the frontend's) for the music. test-audio
# checks the mixer, ADPCM, MP2 seeking and loops, the bank checks and the
# audio state block against that bank.
FIGHTER_DIR := $(TOP)/app/fighter
FIGHTER_BUILD := $(BUILD)/fighter
FIGHTER_INCLUDE := $(EXTERNAL)/mgba/src/platform/libretro
FIGHTER_SRCS := fighter.c render.c libretro.c audio.c mp2.c
FIGHTER_OBJS := $(addprefix $(FIGHTER_BUILD)/,$(FIGHTER_SRCS:.c=.o))
FIGHTER_LIB := $(FIGHTER_BUILD)/libfighter.a
# LeakSanitizer cannot enumerate threads under the sandbox ptrace monitor;
# ASan bounds/use-after-free and UBSan remain enabled in fighter-test.
FIGHTER_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wshadow -Werror -I$(FIGHTER_INCLUDE) -isystem $(EXTERNAL)/pl_mpeg
FIGHTER_CONTENT := $(FIGHTER_BUILD)/Fighter-POC.fight $(FIGHTER_BUILD)/Fighter-Auto.fight
FIGHTER_SOUND := $(FIGHTER_CONTENT:.fight=.fightsnd)
FIGHTER_HEADERS := $(addprefix $(FIGHTER_DIR)/,fighter.h audio.h mp2.h)
HOST_CC ?= cc

$(FIGHTER_BUILD)/%.o: $(FIGHTER_DIR)/%.c $(FIGHTER_HEADERS) $(TOP)/mk/fighter.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(FIGHTER_FLAGS) -mcpu=cortex-a35 -c -o $@ $<
$(FIGHTER_BUILD)/mp2.o: FIGHTER_FLAGS += -O3

$(FIGHTER_LIB): $(FIGHTER_OBJS) $(TOP)/mk/core-archive.sh
	$(TOP)/mk/core-archive.sh fighter $@ $(FIGHTER_OBJS)

FIGHTER_ROSTER := $(TOP)/assets/fighter/roster.json
FIGHTER_ASSETS := $(FIGHTER_ROSTER) $(wildcard $(TOP)/assets/fighter/*.json $(TOP)/assets/fighter/*.png)
$(FIGHTER_BUILD)/Fighter-POC.fight: $(TOP)/tools/fighter/pack.py $(FIGHTER_ASSETS)
	python3 $< build $(FIGHTER_ROSTER) $@
$(FIGHTER_BUILD)/Fighter-Auto.fight: $(TOP)/tools/fighter/pack.py $(FIGHTER_ASSETS)
	python3 $< build $(FIGHTER_ROSTER) $@ --auto

# One bank serves both packs (same roster); Auto's is a copy.
FIGHTER_SOUND_ASSETS := $(TOP)/assets/fighter/sound/sound.json $(wildcard $(TOP)/assets/fighter/sound/sfx/*.wav \
	$(TOP)/assets/fighter/sound/music/*.mp2 $(TOP)/assets/fighter/sound/music/tracks.json)
$(FIGHTER_BUILD)/Fighter-POC.fightsnd: $(TOP)/tools/fighter/sound.py $(TOP)/tools/fighter/pack.py \
		$(FIGHTER_SOUND_ASSETS) $(FIGHTER_ASSETS) $(FIGHTER_DIR)/fighter.h
	mkdir -p $(@D)
	python3 $< build $(FIGHTER_ROSTER) $@
$(FIGHTER_BUILD)/Fighter-Auto.fightsnd: $(FIGHTER_BUILD)/Fighter-POC.fightsnd
	cp $< $@

$(FIGHTER_BUILD)/fighter_libretro.so: $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS)) $(FIGHTER_HEADERS) $(TOP)/mk/fighter.mk
	mkdir -p $(@D)
	$(HOST_CC) $(FIGHTER_FLAGS) -fPIC -shared -o $@ $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS))

FIGHTER_TEST_SRCS := $(FIGHTER_DIR)/test.c $(FIGHTER_DIR)/test-render.c
FIGHTER_TEST_DEPS := $(FIGHTER_TEST_SRCS) $(FIGHTER_DIR)/test-render.h $(FIGHTER_HEADERS) $(TOP)/mk/fighter.mk
$(FIGHTER_BUILD)/test: $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS)) $(FIGHTER_TEST_DEPS)
	mkdir -p $(@D)
	$(HOST_CC) $(FIGHTER_FLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS)) $(FIGHTER_TEST_SRCS)

FIGHTER_AUDIO_TEST_SRCS := $(addprefix $(FIGHTER_DIR)/,test-audio.c audio.c mp2.c)
$(FIGHTER_BUILD)/test-audio: $(FIGHTER_AUDIO_TEST_SRCS) $(FIGHTER_HEADERS) $(TOP)/mk/fighter.mk
	mkdir -p $(@D)
	$(HOST_CC) $(FIGHTER_FLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(FIGHTER_AUDIO_TEST_SRCS)

# The same callbacks also exercise the actual ELF shared library via the
# system dynamic loader; the source-linked build above instruments the core.
$(FIGHTER_BUILD)/test-shared: $(FIGHTER_TEST_DEPS) $(FIGHTER_BUILD)/fighter_libretro.so
	$(HOST_CC) $(FIGHTER_FLAGS) -o $@ $(FIGHTER_TEST_SRCS) -L$(FIGHTER_BUILD) -l:$(notdir $(FIGHTER_BUILD)/fighter_libretro.so) -Wl,-rpath,'$$ORIGIN'

.PHONY: fighter fighter-test
fighter: $(FIGHTER_LIB) $(FIGHTER_BUILD)/fighter_libretro.so $(FIGHTER_CONTENT) $(FIGHTER_SOUND)
fighter-test: $(FIGHTER_BUILD)/test $(FIGHTER_BUILD)/test-shared $(FIGHTER_BUILD)/test-audio \
		$(FIGHTER_BUILD)/fighter_libretro.so $(FIGHTER_CONTENT) $(FIGHTER_SOUND)
	python3 $(TOP)/tools/fighter/pack-test.py
	ASAN_OPTIONS=detect_leaks=0 $(FIGHTER_BUILD)/test-audio $(FIGHTER_BUILD)/Fighter-POC.fightsnd
	ASAN_OPTIONS=detect_leaks=0 $(FIGHTER_BUILD)/test $(FIGHTER_BUILD)/Fighter-POC.fight $(FIGHTER_BUILD)/Fighter-Auto.fight $(FIGHTER_BUILD)/preview.ppm
	$(FIGHTER_BUILD)/test-shared $(FIGHTER_BUILD)/Fighter-POC.fight $(FIGHTER_BUILD)/Fighter-Auto.fight $(FIGHTER_BUILD)/preview-shared.ppm
