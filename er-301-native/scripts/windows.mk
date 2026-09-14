# Build tools for Windows (MinGW-w64 / MSYS2 MINGW64).
CC := gcc -fdiagnostics-color -fmax-errors=5
CPP := g++ -fdiagnostics-color -fmax-errors=5
OBJCOPY := objcopy
OBJDUMP := objdump
ADDR2LINE := addr2line
LD := gcc -fdiagnostics-color
AR := gcc-ar
SIZE := size
STRIP := strip
READELF := readelf
NM := nm
SWIG := $(abspath ../../third-party/tools/swigwin-4.4.1/swig.exe)
PYTHON := python3
ZIP := zip
