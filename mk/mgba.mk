# mGBA libretro core as a static archive, linked into our frontend.
# mGBA's own CMake builds the core with BUILD_LIBRETRO=ON and every frontend,
# optional dependency and debugger turned off. Its libretro target is always
# a SHARED module, so mk/mgba-static.cmake is injected as a project include
# and adds a STATIC twin from the same sources and definitions (no -fPIC).
# Only that static target is built; external/mgba stays pristine.
#
# Consumers use MGBA_LIBS (also a valid prerequisite), MGBA_LDLIBS and
# MGBA_INCLUDE. The core is built with DISABLE_THREADING, GBA only (no GB).
# MGBA_LIBS is mGBA's archive joined by mk/core-archive.sh into one object
# that exports only mgba_retro_* (all else local), so it links next to
# other cores.

MGBA_SRC := $(EXTERNAL)/mgba
MGBA_BUILD := $(BUILD)/mgba
MGBA_TOOLCHAIN_FILE := $(MGBA_BUILD)/toolchain.cmake
MGBA_CONFIG_STAMP := $(MGBA_BUILD)/.configured
MGBA_STATIC_HOOK := $(TOP)/mk/mgba-static.cmake

MGBA_RAW_LIB := $(MGBA_BUILD)/libmgba_libretro.a
MGBA_LIBS := $(MGBA_BUILD)/libmgba_core.a
MGBA_LDLIBS := -lm
MGBA_INCLUDE := $(MGBA_SRC)/src/platform/libretro
MGBA_STAMP := $(MGBA_LIBS)

# CMake's Release type adds -O3 -DNDEBUG on top of these
MGBA_CFLAGS := -mcpu=cortex-a35

# GCC LTO objects would force the frontend to link with -flto and gcc-ar
MGBA_LTO ?= OFF

# mGBA still declares cmake_minimum_required(VERSION 3.1), which CMake 4
# rejects without a policy floor
MGBA_CMAKE_OPTS := \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DBUILD_LIBRETRO=ON -DSKIP_LIBRARY=ON \
	-DBUILD_STATIC=OFF -DBUILD_SHARED=OFF \
	-DBUILD_QT=OFF -DBUILD_SDL=OFF \
	-DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF \
	-DBUILD_LTO=$(MGBA_LTO) -DBUILD_PGO=OFF \
	-DM_CORE_GBA=ON -DM_CORE_GB=OFF \
	-DUSE_DEBUGGERS=OFF -DUSE_GDB_STUB=OFF -DUSE_EDITLINE=OFF \
	-DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF -DUSE_MINIZIP=OFF -DUSE_PNG=OFF \
	-DUSE_LIBZIP=OFF -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LZMA=OFF \
	-DUSE_LUA=OFF -DENABLE_SCRIPTING=OFF -DUSE_DISCORD_RPC=OFF \
	-DCMAKE_PROJECT_mGBA_INCLUDE=$(MGBA_STATIC_HOOK)

# Search only the toolchain sysroot so no host library is ever picked up
$(MGBA_TOOLCHAIN_FILE): $(TOP)/mk/mgba.mk $(TOOLCHAIN_STAMP)
	mkdir -p $(@D)
	printf '%s\n' \
		'set(CMAKE_SYSTEM_NAME Linux)' \
		'set(CMAKE_SYSTEM_PROCESSOR aarch64)' \
		'set(CMAKE_SYSROOT $(TOOLCHAIN_DIR)/aarch64-buildroot-linux-musl/sysroot)' \
		'set(CMAKE_C_COMPILER $(CROSS_COMPILE)gcc)' \
		'set(CMAKE_CXX_COMPILER $(CROSS_COMPILE)g++)' \
		'set(CMAKE_ASM_COMPILER $(CROSS_COMPILE)gcc)' \
		'set(CMAKE_AR $(CROSS_COMPILE)gcc-ar CACHE FILEPATH "")' \
		'set(CMAKE_RANLIB $(CROSS_COMPILE)gcc-ranlib CACHE FILEPATH "")' \
		'set(CMAKE_C_FLAGS_INIT "$(MGBA_CFLAGS)")' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)' \
		> $@

# Reconfigure from scratch whenever the options or the hook change
$(MGBA_CONFIG_STAMP): $(MGBA_TOOLCHAIN_FILE) $(MGBA_STATIC_HOOK)
	rm -rf $(MGBA_BUILD)/CMakeCache.txt $(MGBA_BUILD)/CMakeFiles
	cmake -S $(MGBA_SRC) -B $(MGBA_BUILD) -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE=$(MGBA_TOOLCHAIN_FILE) $(MGBA_CMAKE_OPTS) \
		> $(MGBA_BUILD)/configure.log
	touch $@

# Always defer to ninja, which leaves the archive alone when nothing changed
$(MGBA_RAW_LIB): $(MGBA_CONFIG_STAMP) FORCE
	ninja -C $(MGBA_BUILD) -j$(JOBS) mgba_libretro_static

$(MGBA_LIBS): $(MGBA_RAW_LIB) $(TOP)/mk/core-archive.sh
	$(TOP)/mk/core-archive.sh mgba $@ $(MGBA_RAW_LIB)

.PHONY: mgba FORCE
mgba: $(MGBA_LIBS)

FORCE:
