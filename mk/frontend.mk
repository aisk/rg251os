# The frontend (app/frontend), a static PID 1 linking the libretro cores
# and tinyalsa. It links mGBA and gpSP when mk/mgba.mk and mk/gpsp.mk
# provide MGBA_LIBS and GPSP_LIBS (each core's symbols are renamed to
# mgba_retro_* / gpsp_retro_* by mk/core-archive.sh, cores.c holds the
# tables) and falls back to a stub core otherwise; FRONTEND_CORE=stub
# forces the stub, FRONTEND_CORE=mgba or gpsp links only that core. The
# stub build (every core id runs the stub) is also available as
# frontend-stub for tests.
#
#   make frontend          build/frontend/frontend (stripped, installed)
#   make frontend-test     selftest and headless runs under qemu-aarch64

FRONTEND_DIR := $(TOP)/app/frontend
FRONTEND_BUILD := $(BUILD)/frontend
FRONTEND_BIN := $(FRONTEND_BUILD)/frontend
FRONTEND_ELF := $(FRONTEND_BUILD)/frontend.elf
FRONTEND_STUB_ELF := $(FRONTEND_BUILD)/frontend-stub.elf

FRONTEND_CORE ?= $(or $(subst $(eval) ,-,$(strip $(if $(MGBA_LIBS),mgba) \
	$(if $(GPSP_LIBS),gpsp))),stub)
LIBRETRO_INCLUDE := $(if $(MGBA_INCLUDE),$(MGBA_INCLUDE),$(EXTERNAL)/mgba/src/platform/libretro)

FRONTEND_SRCS := main.c log.c sys.c display.c blit.c draw.c audio.c \
	resample.c input.c romlist.c core.c menu.c perf.c selftest.c \
	settings.c fx.c battery.c cpuctl.c
FRONTEND_OBJS := $(addprefix $(FRONTEND_BUILD)/obj/,$(FRONTEND_SRCS:.c=.o))
FRONTEND_STUB_OBJ := $(FRONTEND_BUILD)/obj/stub_core.o
FRONTEND_STUB_LIB := $(FRONTEND_BUILD)/libstub_core.a
FRONTEND_CORES_OBJ := $(FRONTEND_BUILD)/obj/cores-$(FRONTEND_CORE).o
FRONTEND_STUB_CORES_OBJ := $(FRONTEND_BUILD)/obj/cores-stub.o

FRONTEND_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wshadow -D_GNU_SOURCE \
	-mcpu=cortex-a35 -isystem $(LIBRETRO_INCLUDE) -isystem $(TINYALSA_INCLUDE) \
	-I$(TOP)/app/hwtest -MMD -MP
FRONTEND_LDFLAGS := -static

ifeq ($(FRONTEND_CORE),stub)
FRONTEND_CORE_LIBS := $(FRONTEND_STUB_LIB)
FRONTEND_CORE_LDLIBS :=
else
FRONTEND_CORE_LIBS := $(if $(filter mgba,$(subst -, ,$(FRONTEND_CORE))),$(MGBA_LIBS)) \
	$(if $(filter gpsp,$(subst -, ,$(FRONTEND_CORE))),$(GPSP_LIBS))
FRONTEND_CORE_LDLIBS := $(sort $(MGBA_LDLIBS) $(GPSP_LDLIBS))
endif

# The selected core is recorded so switching it relinks
FRONTEND_CORE_STAMP := $(FRONTEND_BUILD)/.core-$(FRONTEND_CORE)
$(FRONTEND_BUILD)/.core-%:
	mkdir -p $(@D)
	rm -f $(FRONTEND_BUILD)/.core-*
	touch $@

$(FRONTEND_BUILD)/obj/%.o: $(FRONTEND_DIR)/%.c $(TOP)/mk/frontend.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(FRONTEND_CFLAGS) -c -o $@ $<

# The core tables, built per core set: -DHAVE_CORE_MGBA etc.
$(FRONTEND_CORES_OBJ) $(FRONTEND_STUB_CORES_OBJ): $(FRONTEND_BUILD)/obj/cores-%.o: \
		$(FRONTEND_DIR)/cores.c $(TOP)/mk/frontend.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(FRONTEND_CFLAGS) \
		$(if $(filter mgba,$(subst -, ,$*)),-DHAVE_CORE_MGBA) \
		$(if $(filter gpsp,$(subst -, ,$*)),-DHAVE_CORE_GPSP) -c -o $@ $<

$(FRONTEND_STUB_LIB): $(FRONTEND_STUB_OBJ) $(TOP)/mk/core-archive.sh
	$(TOP)/mk/core-archive.sh stub $@ $(FRONTEND_STUB_OBJ)

-include $(FRONTEND_OBJS:.o=.d) $(FRONTEND_STUB_OBJ:.o=.d) \
	$(FRONTEND_CORES_OBJ:.o=.d) $(FRONTEND_STUB_CORES_OBJ:.o=.d)

$(FRONTEND_ELF): $(FRONTEND_OBJS) $(FRONTEND_CORES_OBJ) $(FRONTEND_CORE_LIBS) \
		$(TINYALSA_LIB) $(FRONTEND_CORE_STAMP)
	$(CROSS_COMPILE)gcc $(FRONTEND_LDFLAGS) -o $@ $(FRONTEND_OBJS) \
		$(FRONTEND_CORES_OBJ) $(FRONTEND_CORE_LIBS) $(TINYALSA_LIB) \
		$(FRONTEND_CORE_LDLIBS) -lm

$(FRONTEND_STUB_ELF): $(FRONTEND_OBJS) $(FRONTEND_STUB_CORES_OBJ) \
		$(FRONTEND_STUB_LIB) $(TINYALSA_LIB)
	$(CROSS_COMPILE)gcc $(FRONTEND_LDFLAGS) -o $@ $(FRONTEND_OBJS) \
		$(FRONTEND_STUB_CORES_OBJ) $(FRONTEND_STUB_LIB) $(TINYALSA_LIB) -lm

$(FRONTEND_BIN): $(FRONTEND_ELF)
	$(CROSS_COMPILE)strip -o $@ $<
	@echo "frontend ($(FRONTEND_CORE) cores): $$(stat -c %s $@) bytes"

QEMU_AARCH64 ?= qemu-aarch64
FRONTEND_TEST_DIR := $(FRONTEND_BUILD)/test
FRONTEND_TEST_ROM ?= $(EXTERNAL)/mgba/cinema/gba/irq/keyirq/test.gba
FRONTEND_GPSP_TEST_ROMS ?= $(wildcard $(EXTERNAL)/mgba/cinema/gba/*/*/test.gba)

.PHONY: frontend frontend-stub frontend-test
frontend: $(FRONTEND_BIN)
frontend-stub: $(FRONTEND_STUB_ELF)

frontend-test: $(FRONTEND_ELF) $(FRONTEND_STUB_ELF)
	rm -rf $(FRONTEND_TEST_DIR)
	mkdir -p $(FRONTEND_TEST_DIR)/stub $(FRONTEND_TEST_DIR)/core
	$(QEMU_AARCH64) $(FRONTEND_STUB_ELF) --selftest $(FRONTEND_TEST_DIR)/selftest
	$(QEMU_AARCH64) $(FRONTEND_STUB_ELF) --headless $(FRONTEND_TEST_DIR)/stub \
		$(FRONTEND_TEST_ROM) 3000
	test -s $(FRONTEND_TEST_DIR)/stub/saves/test.sav
	# Repeated background saves are all collected and logged, and the
	# next run loads the save back.
	test $$(grep -c 'save: wrote' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt) -ge 3
	grep -q 'core: unloaded ([1-9][0-9]* save writes)' \
		$(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	# Game statistics: one CSV header, a summary row per game, and the
	# simulated cpufreq floor was applied.
	test $$(grep -c '^time,' $(FRONTEND_TEST_DIR)/stub/log/perf.csv) -eq 1
	test $$(grep -c ',all,' $(FRONTEND_TEST_DIR)/stub/log/perf.csv) -eq 1
	grep -q 'perf summary: adaptive all' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	# Debug logging is off by default: a small log, no periodic lines,
	# perf.csv still written (checked above).
	test $$(stat -c %s $(FRONTEND_TEST_DIR)/stub/log/frontend.txt) -lt 16384
	! grep -q '\] perf: \|cpufreq: target' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	cp $(FRONTEND_TEST_DIR)/stub/log/frontend.txt \
		$(FRONTEND_TEST_DIR)/stub/log/frontend-nodebug.txt
	printf 'grid=on\ngrid_dim=50\ngrid_boost=15\ncpu=max\ndebug=on\n' > \
		$(FRONTEND_TEST_DIR)/stub/config/settings.txt
	printf 'color=gba\n' > $(FRONTEND_TEST_DIR)/stub/config/roms/test.txt
	$(QEMU_AARCH64) $(FRONTEND_STUB_ELF) --headless $(FRONTEND_TEST_DIR)/stub \
		$(FRONTEND_TEST_ROM) 600
	grep -q 'save: loaded' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	grep -q '\] perf: ' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	grep -q 'cpufreq: target .* observed' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	grep -q 'fx: color gba, grid on' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	test $$(grep -c '^time,' $(FRONTEND_TEST_DIR)/stub/log/perf.csv) -eq 1
	test $$(grep -c ',all,' $(FRONTEND_TEST_DIR)/stub/log/perf.csv) -eq 2
	grep -q ',max,1296000,' $(FRONTEND_TEST_DIR)/stub/log/perf.csv
	# Audio: summary line and the audio columns at the end of perf.csv.
	grep -q 'audio summary (headless): adaptive mode' \
		$(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	grep -q ',audio_lrck_hz$$' $(FRONTEND_TEST_DIR)/stub/log/perf.csv
	grep -q ',adaptive,48000 Hz S16 512x8,48000,' \
		$(FRONTEND_TEST_DIR)/stub/log/perf.csv
	# A perf.csv from an older build (other header) is moved aside.
	mkdir -p $(FRONTEND_TEST_DIR)/core/log
	printf 'time,old\n1,2\n' > $(FRONTEND_TEST_DIR)/core/log/perf.csv
	# The real core on the default (simple) audio path.
	FE_AUDIO=simple $(QEMU_AARCH64) $(FRONTEND_ELF) --headless \
		$(FRONTEND_TEST_DIR)/core $(FRONTEND_TEST_ROM) 3000
	grep -q 'audio summary (headless): simple mode, 48000 Hz S16 512x8' \
		$(FRONTEND_TEST_DIR)/core/log/frontend.txt
	grep -q '^time,old' $(FRONTEND_TEST_DIR)/core/log/perf-old.csv
	grep -q ',simple,48000 Hz S16 512x8,48000,' \
		$(FRONTEND_TEST_DIR)/core/log/perf.csv
	# gpSP (ARM64 dynarec, under qemu-user too) on mGBA's test ROMs,
	# picked by the per ROM setting.
	set -e; for r in $(FRONTEND_GPSP_TEST_ROMS); do \
		d=$(FRONTEND_TEST_DIR)/gpsp/$$(basename $$(dirname $$r)); \
		mkdir -p $$d/config/roms; \
		printf 'core=gpsp\n' > $$d/config/roms/test.txt; \
		$(QEMU_AARCH64) $(FRONTEND_ELF) --headless $$d $$r 1200; \
		grep -q 'core: gpSP .* loaded test.gba' $$d/log/frontend.txt; \
		grep -q 'core: options gpsp_drc=enabled' $$d/log/frontend.txt; \
		grep -q 'headless: 1200 frames, frame 240x160' $$d/log/frontend.txt; \
		! grep -q 'Failed to map the JIT' $$d/log/frontend.txt; \
	done
	# mGBA -> gpSP -> mGBA in one process on one save, and a .srm.
	$(QEMU_AARCH64) $(FRONTEND_ELF) --core-switch $(FRONTEND_TEST_DIR)/switch 120
	grep -q 'core switch: passed' $(FRONTEND_TEST_DIR)/switch/log/frontend.txt
