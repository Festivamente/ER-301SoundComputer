# Build tools for macOS.
#
# The VCV Rack host uses Apple Clang + libc++. All dynamically loaded ER-301
# packages must use that same ABI; Homebrew GCC produces std::__cxx11 symbols
# that cannot resolve inside Rack.
CC := clang -fdiagnostics-color
CPP := clang++ -std=gnu++11 -stdlib=libc++ -fdiagnostics-color
OBJCOPY := objcopy
OBJDUMP := objdump
ADDR2LINE := addr2line
LD := ld
AR := ar
SIZE := size
STRIP := strip
READELF := readelf
NM := nm
SWIG := swig
PYTHON := python3
ZIP := zip
