include scripts/env.mk
include scripts/utils.mk
include ../third-party.mk

program_name := libemu
program_dir := $(program_name)
out_dir := $(build_dir)/$(program_name)

src_dirs := $(program_dir) $(hal_dir) $(arch_dir)/$(ARCH) $(od_dir) $(ti_dir)
includes += $(program_dir) $(lua_dir) $(lodepng_dir) $(miniz_dir) $(libs_dir)/SDL_FontCache

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
objects += $(out_dir)/emu/emu_swig.o
objects += $(out_dir)/libs/SDL_FontCache/SDL_FontCache.o

ifeq ($(ARCH),linux)
LFLAGS += --gc-sections
endif

ifeq ($(ARCH),darwin)
CFLAGS += -I$(THIRD_PARTY_INCLUDE) -I$(THIRD_PARTY_INCLUDE)/SDL2
LFLAGS += -L$(THIRD_PARTY_LIB)
endif

CFLAGS += -fpic
ifeq ($(shell uname -s),Darwin)
LFLAGS += -lSDL2_ttf -lfreetype -lfftw3f $(SDL2_STATIC_LDFLAGS) -lm -ldl -lstdc++
else
LFLAGS += -lSDL2_ttf -lfreetype -lfftw3f -lSDL2 -lm -ldl -lstdc++
endif

all: $(out_dir)/$(program_name).so

$(out_dir)/$(program_name).so: $(objects) $(libraries)
	@mkdir -p $(@D)	
	@echo $(describe_env) LINK $(describe_target)
	@$(CC) $(CFLAGS) -o $@ $(objects) $(libraries) $(LFLAGS)

clean:
	rm -rf $(out_dir)	

include scripts/rules.mk
