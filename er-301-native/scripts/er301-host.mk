include scripts/env.mk
include scripts/utils.mk
include ../third-party.mk

# Build the ER-301 emulator core as a static library for an external host
# such as VCV Rack. This mirrors scripts/emu.mk, but does not link an
# executable and compiles the alternate host-driven audio backend.
program_name := er301-host
program_dir := emu
out_dir := $(build_dir)/$(program_name)

src_dirs := $(program_dir) $(hal_dir) $(arch_dir)/$(ARCH) $(od_dir) $(ti_dir)

includes += $(program_dir) $(lua_dir) $(lodepng_dir) $(miniz_dir) $(libs_dir)/SDL_FontCache
# Phase 10: link the official Core native module into every engine image.
# This avoids process-global dlopen/SWIG symbol collisions between instances.
includes += $(mods_dir) $(libs_dir)/ne10/inc
# Present on newer ER-301 trees; harmless as an include path on older trees.
includes += emu/od/glue

# Recursive search for the same source set used by the standalone emulator.
cpp_sources := $(foreach D,$(src_dirs),$(call rwildcard,$D,*.cpp))
c_sources := $(foreach D,$(src_dirs),$(call rwildcard,$D,*.c))

# Compile Core DSP/graphics objects directly into the host archive. Lua still
# installs the package assets/toc normally, but core.libcore is preloaded from
# this image instead of opening a shared libcore.so.
core_src_dir := $(mods_dir)/core
cpp_sources += $(call rwildcard,$(core_src_dir),*.cpp)
c_sources += $(call rwildcard,$(core_src_dir),*.c)
objects := $(addprefix $(out_dir)/,$(c_sources:%.c=%.o) $(cpp_sources:%.cpp=%.o))

# Generated/manual objects also used by scripts/emu.mk.
objects += $(out_dir)/od/glue/app_swig.o
objects += $(out_dir)/mods/core/core_swig.o
objects += $(out_dir)/libs/SDL_FontCache/SDL_FontCache.o

# External host headers come from the workspace-local dependency prefix.
CFLAGS += -I$(THIRD_PARTY_INCLUDE) -I$(THIRD_PARTY_INCLUDE)/SDL2

CFLAGS += -DER301_VCV_HOST
CFLAGS += -DFIRMWARE_VERSION=\"$(FIRMWARE_VERSION)\"
# Keep the application SWIG module global (`app.*` is the firmware API), but
# match normal mod builds for the statically embedded Core module.
$(out_dir)/mods/core/core_swig.cpp: SWIGFLAGS += -nomoduleglobal -small -fvirtual

# Older source trees define these; newer ones do not. Empty definitions are
# unnecessary, so add them only when the variables exist.
ifneq ($(strip $(FIRMWARE_NAME)),)
  CFLAGS += -DFIRMWARE_NAME=\"$(FIRMWARE_NAME)\"
endif
ifneq ($(strip $(FIRMWARE_STATUS)),)
  CFLAGS += -DFIRMWARE_STATUS=\"$(FIRMWARE_STATUS)\"
endif
ifneq ($(strip $(PROFILE)),)
  CFLAGS += -DBUILD_PROFILE=\"$(PROFILE)\"
endif

host_library := $(out_dir)/liber301host.a

all: $(host_library)

$(objects): scripts/env.mk scripts/er301-host.mk

$(host_library): $(objects)
	@mkdir -p $(@D)
	@echo $(describe_env) ARCHIVE $(describe_target)
	@rm -f $@
	@$(AR) rcs $@ $(objects)

clean:
	rm -rf $(out_dir)

include scripts/rules.mk
