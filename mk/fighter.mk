# Portable fighter (P0): static renamed libretro core for the handheld,
# native shared libretro core and callback harness for desktop verification.
# Content packs are built from the committed sprite sheet and manifest in
# assets/fighter; POC is player vs CPU, Auto runs the four timing scenes.
FIGHTER_DIR := $(TOP)/app/fighter
FIGHTER_BUILD := $(BUILD)/fighter
FIGHTER_INCLUDE := $(EXTERNAL)/mgba/src/platform/libretro
FIGHTER_SRCS := fighter.c render.c libretro.c
FIGHTER_OBJS := $(addprefix $(FIGHTER_BUILD)/,$(FIGHTER_SRCS:.c=.o))
FIGHTER_LIB := $(FIGHTER_BUILD)/libfighter.a
# LeakSanitizer cannot enumerate threads under the sandbox ptrace monitor;
# ASan bounds/use-after-free and UBSan remain enabled in fighter-test.
FIGHTER_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wshadow -Werror -I$(FIGHTER_INCLUDE)
FIGHTER_CONTENT := $(FIGHTER_BUILD)/Fighter-POC.fight $(FIGHTER_BUILD)/Fighter-Auto.fight
HOST_CC ?= cc

$(FIGHTER_BUILD)/%.o: $(FIGHTER_DIR)/%.c $(FIGHTER_DIR)/fighter.h $(TOP)/mk/fighter.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(FIGHTER_FLAGS) -mcpu=cortex-a35 -c -o $@ $<

$(FIGHTER_LIB): $(FIGHTER_OBJS) $(TOP)/mk/core-archive.sh
	$(TOP)/mk/core-archive.sh fighter $@ $(FIGHTER_OBJS)

FIGHTER_ASSETS := $(TOP)/assets/fighter/terry.json $(TOP)/assets/fighter/KOF02_Terry.png
$(FIGHTER_BUILD)/Fighter-POC.fight: $(TOP)/tools/fighter/pack.py $(FIGHTER_ASSETS)
	python3 $< build $(TOP)/assets/fighter/terry.json $@
$(FIGHTER_BUILD)/Fighter-Auto.fight: $(TOP)/tools/fighter/pack.py $(FIGHTER_ASSETS)
	python3 $< build $(TOP)/assets/fighter/terry.json $@ --auto

$(FIGHTER_BUILD)/fighter_libretro.so: $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS)) $(FIGHTER_DIR)/fighter.h $(TOP)/mk/fighter.mk
	mkdir -p $(@D)
	$(HOST_CC) $(FIGHTER_FLAGS) -fPIC -shared -o $@ $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS))

FIGHTER_TEST_SRCS := $(FIGHTER_DIR)/test.c $(FIGHTER_DIR)/test-render.c
FIGHTER_TEST_DEPS := $(FIGHTER_TEST_SRCS) $(FIGHTER_DIR)/test-render.h $(FIGHTER_DIR)/fighter.h $(TOP)/mk/fighter.mk
$(FIGHTER_BUILD)/test: $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS)) $(FIGHTER_TEST_DEPS)
	mkdir -p $(@D)
	$(HOST_CC) $(FIGHTER_FLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(addprefix $(FIGHTER_DIR)/,$(FIGHTER_SRCS)) $(FIGHTER_TEST_SRCS)

# The same callbacks also exercise the actual ELF shared library via the
# system dynamic loader; the source-linked build above instruments the core.
$(FIGHTER_BUILD)/test-shared: $(FIGHTER_TEST_DEPS) $(FIGHTER_BUILD)/fighter_libretro.so
	$(HOST_CC) $(FIGHTER_FLAGS) -o $@ $(FIGHTER_TEST_SRCS) -L$(FIGHTER_BUILD) -l:$(notdir $(FIGHTER_BUILD)/fighter_libretro.so) -Wl,-rpath,'$$ORIGIN'

.PHONY: fighter fighter-test
fighter: $(FIGHTER_LIB) $(FIGHTER_BUILD)/fighter_libretro.so $(FIGHTER_CONTENT)
fighter-test: $(FIGHTER_BUILD)/test $(FIGHTER_BUILD)/test-shared $(FIGHTER_BUILD)/fighter_libretro.so $(FIGHTER_CONTENT)
	ASAN_OPTIONS=detect_leaks=0 $(FIGHTER_BUILD)/test $(FIGHTER_BUILD)/Fighter-POC.fight $(FIGHTER_BUILD)/Fighter-Auto.fight $(FIGHTER_BUILD)/preview.ppm
	$(FIGHTER_BUILD)/test-shared $(FIGHTER_BUILD)/Fighter-POC.fight $(FIGHTER_BUILD)/Fighter-Auto.fight $(FIGHTER_BUILD)/preview-shared.ppm
