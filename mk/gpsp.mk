# gpSP (libretro fork) as a second GBA core, with its ARM64 dynarec.
# gpSP's own Makefile builds in its source tree, so the source lists come
# from its Makefile.common and the objects are built here into
# $(GPSP_BUILD); external/gpsp stays pristine. The flags are those of its
# "arm64" platform (HAVE_DYNAREC, ARM64_ARCH, MMAP_JIT_CACHE: the
# translation cache is mmapped RWX next to the text segment), plus
# -mcpu=cortex-a35. gpSP uses no threads.
#
# The objects are joined by mk/core-archive.sh into one object exporting
# only gpsp_retro_* (and gpsp_backup_type, gpsp_flash_bank_cnt,
# gpsp_eeprom_size for the save size, see core.c); everything else is
# local. Consumers use GPSP_LIBS (also a valid prerequisite) and
# GPSP_LDLIBS.

GPSP_SRC := $(EXTERNAL)/gpsp
GPSP_BUILD := $(BUILD)/gpsp
GPSP_LIBS := $(GPSP_BUILD)/libgpsp_core.a
GPSP_LDLIBS := -lm

GPSP_GIT_VERSION := $(shell git -C $(GPSP_SRC) rev-parse --short HEAD 2>/dev/null)

# Makefile.common sets these from CORE_DIR, HAVE_DYNAREC and CPU_ARCH;
# copy them and drop the generic names again.
CORE_DIR := $(GPSP_SRC)
HAVE_DYNAREC := 1
CPU_ARCH := arm64
include $(GPSP_SRC)/Makefile.common
GPSP_INCFLAGS := $(INCFLAGS)
GPSP_SOURCES := $(SOURCES_C) $(SOURCES_CC) $(SOURCES_ASM)
undefine CORE_DIR
undefine HAVE_DYNAREC
undefine CPU_ARCH
undefine INCFLAGS
undefine LIBRETRO_COMM_DIR
undefine SOURCES_C
undefine SOURCES_CC
undefine SOURCES_ASM

GPSP_OBJS := $(patsubst $(GPSP_SRC)/%,$(GPSP_BUILD)/obj/%.o,$(GPSP_SOURCES))

GPSP_CFLAGS := -O3 -DNDEBUG -mcpu=cortex-a35 -fomit-frame-pointer \
	-ffast-math -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H \
	-D__LIBRETRO__ -DINLINE=inline -DHAVE_DYNAREC -DARM64_ARCH \
	-DMMAP_JIT_CACHE -DFRONTEND_SUPPORTS_RGB565 \
	$(if $(GPSP_GIT_VERSION),-DGIT_VERSION=\"$(GPSP_GIT_VERSION)\") \
	$(GPSP_INCFLAGS) -Wall -MMD -MP
GPSP_CXXFLAGS := $(GPSP_CFLAGS) -fno-rtti -fno-exceptions -std=c++11

# Like upstream: the dynarec's generated code has many unused labels and
# variables by design.
$(GPSP_BUILD)/obj/cpu_threaded.c.o: GPSP_CFLAGS += \
	-Wno-unused-variable -Wno-unused-label

$(GPSP_BUILD)/obj/%.c.o: $(GPSP_SRC)/%.c $(TOP)/mk/gpsp.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(GPSP_CFLAGS) -c -o $@ $<

$(GPSP_BUILD)/obj/%.cc.o: $(GPSP_SRC)/%.cc $(TOP)/mk/gpsp.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)g++ $(GPSP_CXXFLAGS) -c -o $@ $<

$(GPSP_BUILD)/obj/%.S.o: $(GPSP_SRC)/%.S $(TOP)/mk/gpsp.mk | $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc $(GPSP_CFLAGS) -Wa,-I$(GPSP_SRC) -c -o $@ $<

-include $(GPSP_OBJS:.o=.d)

$(GPSP_LIBS): $(GPSP_OBJS) $(TOP)/mk/core-archive.sh
	$(TOP)/mk/core-archive.sh gpsp $@ -k backup_type -k flash_bank_cnt \
		-k eeprom_size $(GPSP_OBJS)

.PHONY: gpsp
gpsp: $(GPSP_LIBS)
