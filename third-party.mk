# Shared external dependency layout for the ER-301 Sound Computer workspace.
#
# Expected checkout layout:
#   GitHub/
#     Rack-SDK/                         # external, not committed here
#     VCVRackER-301/
#       src/
#       res/
#       er-301-native/
#       third-party/install/<platform>/
#
# Rack builds should provide ER301SoundComputer_TARGET using Rack's ARCH_NAME
# (mac-arm64, mac-x64, win-x64, lin-x64). The ER-301 recursive build receives
# the same value, so dependency selection follows the requested target rather
# than the machine running make.
#
# THIRD_PARTY_ROOT / THIRD_PARTY_PREFIX can still be overridden explicitly.

ER301SoundComputer_SOURCE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
# third-party is intentionally repository-local so pinned dependency sources
# and target prefixes remain isolated from system package-manager updates.
THIRD_PARTY_ROOT ?= $(ER301SoundComputer_SOURCE_ROOT)/third-party

# Standalone/non-Rack fallback. The VCV plugin Makefile sets ER301SoundComputer_TARGET from
# Rack's arch.mk before including this file, so cross-builds never use uname.
ifndef ER301SoundComputer_TARGET
  ER301SoundComputer_UNAME_S := $(shell uname -s)
  ER301SoundComputer_UNAME_M := $(shell uname -m)
  ifeq ($(ER301SoundComputer_UNAME_S),Darwin)
    ifeq ($(ER301SoundComputer_UNAME_M),arm64)
      ER301SoundComputer_TARGET := mac-arm64
    else
      ER301SoundComputer_TARGET := mac-x64
    endif
  else ifeq ($(ER301SoundComputer_UNAME_S),Linux)
    ER301SoundComputer_TARGET := lin-x64
  else
    ER301SoundComputer_TARGET := win-x64
  endif
endif

ifeq ($(ER301SoundComputer_TARGET),mac-arm64)
  THIRD_PARTY_PLATFORM ?= macos-arm64
else ifeq ($(ER301SoundComputer_TARGET),mac-x64)
  THIRD_PARTY_PLATFORM ?= macos-x86_64
else ifeq ($(ER301SoundComputer_TARGET),win-x64)
  THIRD_PARTY_PLATFORM ?= windows-x64
else ifeq ($(ER301SoundComputer_TARGET),lin-x64)
  THIRD_PARTY_PLATFORM ?= linux-x64
else
  $(error Unsupported ER301SoundComputer_TARGET '$(ER301SoundComputer_TARGET)')
endif

THIRD_PARTY_PREFIX ?= $(THIRD_PARTY_ROOT)/install/$(THIRD_PARTY_PLATFORM)
THIRD_PARTY_INCLUDE ?= $(THIRD_PARTY_PREFIX)/include
THIRD_PARTY_LIB ?= $(THIRD_PARTY_PREFIX)/lib
SDL2_CONFIG ?= $(THIRD_PARTY_PREFIX)/bin/sdl2-config

# sdl2-config is a host-runnable shell script emitted by our pinned SDL2 build.
# It supplies SDL2 plus the platform system-library flags required by a static
# SDL2 link on macOS and Linux. Keep this recursively expanded so a fresh clone
# can prepare the target prefix before the final link command requests the flags.
ifneq ($(filter mac-arm64 mac-x64 lin-x64,$(ER301SoundComputer_TARGET)),)
  SDL2_STATIC_LDFLAGS = $(shell "$(SDL2_CONFIG)" --static-libs 2>/dev/null)
endif
