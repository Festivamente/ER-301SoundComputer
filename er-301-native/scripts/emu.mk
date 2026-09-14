include scripts/env.mk
include scripts/utils.mk
include ../third-party.mk

program_name := emu
program_dir := $(program_name)
out_dir := $(build_dir)/$(program_name)

src_dirs := $(program_dir) $(hal_dir) $(arch_dir)/$(ARCH) $(od_dir) $(ti_dir)
includes += $(program_dir) $(lua_dir) $(lodepng_dir) $(miniz_dir) $(libs_dir)/SDL_FontCache
includes += emu/od/glue

libraries :=
libraries += $(libs_build_dir)/lib$(lua_name).a
libraries += $(libs_build_dir)/liblodepng.a
libraries += $(libs_build_dir)/libminiz.a

# Recursive search for source files
cpp_sources := $(foreach D,$(src_dirs),$(call rwildcard,$D,*.cpp)) 
c_sources := $(foreach D,$(src_dirs),$(call rwildcard,$D,*.c)) 

objects := $(addprefix $(out_dir)/,$(c_sources:%.c=%.o) $(cpp_sources:%.cpp=%.o)) 

# Manually add objects 
objects += $(out_dir)/od/glue/app_swig.o
objects += $(out_dir)/libs/SDL_FontCache/SDL_FontCache.o

ifeq ($(ARCH),linux)
LFLAGS += -Wl,--export-dynamic -Wl,--gc-sections
endif

ifeq ($(shell uname -s),Darwin)
ifeq ($(shell uname -m),arm64)
ARCH_FLAGS=-march=armv8.2-a
else
ARCH_FLAGS=-march=native
endif

CFLAGS += -rdynamic
CFLAGS += -I$(THIRD_PARTY_INCLUDE) -I$(THIRD_PARTY_INCLUDE)/SDL2
CFLAGS += $(ARCH_FLAGS)
LFLAGS += -L$(THIRD_PARTY_LIB)
endif

CFLAGS += -DFIRMWARE_VERSION=\"$(FIRMWARE_VERSION)\"
CFLAGS += -DBUILD_PROFILE=\"$(PROFILE)\"
ifeq ($(shell uname -s),Darwin)
LFLAGS += -lSDL2_ttf -lfreetype -lfftw3f $(SDL2_STATIC_LDFLAGS) -lm -ldl -lstdc++
else
LFLAGS += -lSDL2_ttf -lfreetype -lfftw3f -lSDL2 -lm -ldl -lstdc++
endif

all: $(out_dir)/$(program_name).elf

$(objects): scripts/env.mk scripts/emu.mk

$(out_dir)/$(program_name).elf: $(objects) $(libraries)
	@mkdir -p $(@D)	
	@echo $(describe_env) LINK $(describe_target)
	@$(CC) $(CFLAGS) -o $@ $(objects) $(libraries) $(LFLAGS)

clean:
	rm -rf $(out_dir)

addr2line: $(out_dir)/$(program_name).elf
	@echo $(describe_env) Find ${ADDRESS} in $(out_dir)/$(program_name).elf
	@$(ADDR2LINE) -p -f -i -C -e $(out_dir)/$(program_name).elf -a $(ADDRESS)

missing: $(objects) $(libraries)
	@echo $(describe_env) Generating list of missing references...
	-@$(CC) $(CFLAGS) -o $@ $(objects) $(libraries) $(LFLAGS) 2> $(out_dir)/error.log
	@$(PYTHON) list-undefined.py $(out_dir)/error.log > $(out_dir)/missing.log	

include scripts/rules.mk
