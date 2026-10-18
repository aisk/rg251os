# tinyalsa (external/tinyalsa, v2.0.0) as a static library for the
# frontend. Upstream's Makefile builds in the source tree, so the few
# sources needed are compiled here instead. Plugin support (dlopen based)
# is left out: without TINYALSA_USES_PLUGINS only the hw backends are used.
# snd_card_plugin.c is still needed because pcm_close() calls
# snd_utils_close_dev_node() unconditionally (a no-op on a NULL node).

TINYALSA_SRC := $(EXTERNAL)/tinyalsa
TINYALSA_BUILD := $(BUILD)/tinyalsa
TINYALSA_LIB := $(TINYALSA_BUILD)/libtinyalsa.a
TINYALSA_INCLUDE := $(TINYALSA_SRC)/include
TINYALSA_OBJS := $(addprefix $(TINYALSA_BUILD)/,limits.o mixer.o mixer_hw.o pcm.o pcm_hw.o \
	snd_card_plugin.o)
TINYALSA_CFLAGS := -std=gnu11 -O2 -Wall -mcpu=cortex-a35 -I$(TINYALSA_INCLUDE)

$(TINYALSA_BUILD)/%.o: $(TINYALSA_SRC)/src/%.c $(TOP)/mk/tinyalsa.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(TINYALSA_CFLAGS) -c -o $@ $<

$(TINYALSA_LIB): $(TINYALSA_OBJS)
	rm -f $@
	$(CROSS_COMPILE)ar rcs $@ $^

.PHONY: tinyalsa
tinyalsa: $(TINYALSA_LIB)
