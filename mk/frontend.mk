# The frontend (app/frontend), a static PID 1 linking the libretro core and
# tinyalsa. It links mGBA when mk/mgba.mk provides MGBA_LIBS and falls back
# to a stub core otherwise; FRONTEND_CORE=stub forces the stub. The stub
# build is also available as frontend-stub for tests.
#
#   make frontend          build/frontend/frontend (stripped, installed)
#   make frontend-test     selftest and headless runs under qemu-aarch64

FRONTEND_DIR := $(TOP)/app/frontend
FRONTEND_BUILD := $(BUILD)/frontend
FRONTEND_BIN := $(FRONTEND_BUILD)/frontend
FRONTEND_ELF := $(FRONTEND_BUILD)/frontend.elf
FRONTEND_STUB_ELF := $(FRONTEND_BUILD)/frontend-stub.elf

FRONTEND_CORE ?= $(if $(MGBA_LIBS),mgba,stub)
LIBRETRO_INCLUDE := $(if $(MGBA_INCLUDE),$(MGBA_INCLUDE),$(EXTERNAL)/mgba/src/platform/libretro)

FRONTEND_SRCS := main.c log.c sys.c display.c blit.c draw.c audio.c \
	resample.c input.c romlist.c core.c menu.c perf.c selftest.c \
	settings.c fx.c battery.c cpuctl.c
FRONTEND_OBJS := $(addprefix $(FRONTEND_BUILD)/obj/,$(FRONTEND_SRCS:.c=.o))
FRONTEND_STUB_OBJ := $(FRONTEND_BUILD)/obj/stub_core.o

FRONTEND_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wshadow -D_GNU_SOURCE \
	-mcpu=cortex-a35 -isystem $(LIBRETRO_INCLUDE) -isystem $(TINYALSA_INCLUDE) \
	-I$(TOP)/app/hwtest -MMD -MP
FRONTEND_LDFLAGS := -static

ifeq ($(FRONTEND_CORE),mgba)
FRONTEND_CORE_LIBS := $(MGBA_LIBS)
FRONTEND_CORE_LDLIBS := $(MGBA_LDLIBS)
else
FRONTEND_CORE_LIBS := $(FRONTEND_STUB_OBJ)
FRONTEND_CORE_LDLIBS :=
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

-include $(FRONTEND_OBJS:.o=.d) $(FRONTEND_STUB_OBJ:.o=.d)

$(FRONTEND_ELF): $(FRONTEND_OBJS) $(FRONTEND_CORE_LIBS) $(TINYALSA_LIB) \
		$(FRONTEND_CORE_STAMP)
	$(CROSS_COMPILE)gcc $(FRONTEND_LDFLAGS) -o $@ $(FRONTEND_OBJS) \
		$(FRONTEND_CORE_LIBS) $(TINYALSA_LIB) $(FRONTEND_CORE_LDLIBS) -lm

$(FRONTEND_STUB_ELF): $(FRONTEND_OBJS) $(FRONTEND_STUB_OBJ) $(TINYALSA_LIB)
	$(CROSS_COMPILE)gcc $(FRONTEND_LDFLAGS) -o $@ $(FRONTEND_OBJS) \
		$(FRONTEND_STUB_OBJ) $(TINYALSA_LIB) -lm

$(FRONTEND_BIN): $(FRONTEND_ELF)
	$(CROSS_COMPILE)strip -o $@ $<
	@echo "frontend ($(FRONTEND_CORE) core): $$(stat -c %s $@) bytes"

QEMU_AARCH64 ?= qemu-aarch64
FRONTEND_TEST_DIR := $(FRONTEND_BUILD)/test
FRONTEND_TEST_ROM ?= $(EXTERNAL)/mgba/cinema/gba/irq/keyirq/test.gba

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
	grep -q 'cpufreq: target .* observed' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
	printf 'grid=on\ngrid_dim=50\ngrid_boost=15\ncpu=max\n' > \
		$(FRONTEND_TEST_DIR)/stub/config/settings.txt
	printf 'color=gba\n' > $(FRONTEND_TEST_DIR)/stub/config/roms/test.txt
	$(QEMU_AARCH64) $(FRONTEND_STUB_ELF) --headless $(FRONTEND_TEST_DIR)/stub \
		$(FRONTEND_TEST_ROM) 600
	grep -q 'save: loaded' $(FRONTEND_TEST_DIR)/stub/log/frontend.txt
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
