# Rack SDK is an external build dependency, intentionally kept outside this
# repository. With the recommended checkout layout, it lives alongside
# VCVRackER-301 as ../Rack-SDK. An explicit RACK_DIR always takes precedence.
RACK_DIR ?= $(abspath ../Rack-SDK)

ifeq ($(wildcard $(RACK_DIR)/arch.mk),)
  $(error Rack SDK not found at '$(RACK_DIR)'; set RACK_DIR to a Rack 2 SDK)
endif

# Load Rack's target variables before making any ER-301 Sound Computer platform decisions.
# arch.mk derives ARCH_OS/ARCH_CPU/ARCH_NAME from the selected compiler, so this
# works for both native builds and the official cross-compilation toolchain.
include $(RACK_DIR)/arch.mk

ER301SoundComputer_TARGET ?= $(ARCH_NAME)
include third-party.mk

# ---------------------------------------------------------------------------
# Native ER-301 engine integration
# ---------------------------------------------------------------------------
ER301_DIR ?= er-301-native
ER301_PROFILE ?= release

ifdef ARCH_MAC
  ER301_ARCH := darwin
else ifdef ARCH_LIN
  ER301_ARCH := linux
else ifdef ARCH_WIN
  # Windows uses its own ER-301 platform layer alongside Darwin and Linux.
  ER301_ARCH := windows
else
  $(error Unsupported Rack target '$(ARCH_NAME)')
endif

# Rack 2 uses 10.9 for Intel macOS and 11.0 for Apple Silicon. The recursive
# ER-301 build must use the same deployment floor; otherwise its static archives
# inherit the SDK host default (14.0 on our validation machines) and Apple ld
# emits hundreds of "built for newer macOS" warnings at the final link.
ifdef ARCH_MAC
  ifeq ($(ARCH_CPU),arm64)
    ER301_MACOS_DEPLOYMENT_TARGET := 11.0
  else
    ER301_MACOS_DEPLOYMENT_TARGET := 10.9
  endif
endif

FLAGS += -I$(THIRD_PARTY_INCLUDE)
FLAGS += -I$(THIRD_PARTY_INCLUDE)/SDL2
# Some validation machines intentionally rely on a native static-library search
# path when a prepared target prefix has not yet been generated. Never emit a
# nonexistent -L directory: Apple ld warns for it and it obscures real issues.
LDFLAGS += -L$(THIRD_PARTY_LIB)

ifdef ARCH_LIN
  # The primary Rack plugin can be visible before copied engine images load.
  # Bind definitions inside each Linux image to that image so exported Lua and
  # engine symbols cannot be preempted by the primary plugin.
  LDFLAGS += -Wl,-Bsymbolic
endif

# Preserve the compiler selected by Rack/toolchain in the recursive ER-301
# build. GNU Make's built-in CXX on a native Mac is g++, which is Apple's clang
# driver; retain the previously proven explicit libc++ form in that one case.
ER301_CPP := $(CXX)
ifdef ARCH_MAC
  ifeq ($(origin CXX),default)
    ER301_CPP := clang++ -std=gnu++11 -stdlib=libc++
  endif
endif

# Derive a matching cross-archiver when CC is a target-prefixed GCC/Clang.
# Native cc/clang builds simply use Make's normal AR.
ER301_CC_WORD := $(firstword $(CC))
ER301_AR := $(AR)
ifneq ($(filter %-clang,$(ER301_CC_WORD)),)
  ER301_AR := $(patsubst %-clang,%-ar,$(ER301_CC_WORD))
else ifneq ($(filter %-gcc,$(ER301_CC_WORD)),)
  ER301_AR := $(patsubst %-gcc,%-ar,$(ER301_CC_WORD))
endif

ER301_PATH_MAP_FLAGS := -ffile-prefix-map=$(abspath $(ER301_DIR))=ER301_SOURCE
ER301_PATH_MAP_FLAGS += -fdebug-prefix-map=$(abspath $(ER301_DIR))=ER301_SOURCE
ER301_BUILD_ARGS := PROFILE="$(ER301_PROFILE)" ARCH="$(ER301_ARCH)" ER301SoundComputer_TARGET="$(ER301SoundComputer_TARGET)"
ER301_BUILD_ARGS += ER301_TARGET_OS="$(ARCH_OS)" ER301_TARGET_CPU="$(ARCH_CPU)"
ER301_BUILD_ARGS += CC="$(CC)" CPP="$(ER301_CPP)" AR="$(ER301_AR)"
ER301_BUILD_ARGS += THIRD_PARTY_PREFIX="$(abspath $(THIRD_PARTY_PREFIX))"

ER301_HOST_EXTRA_CFLAGS := -DER301_VCV_HOST $(ER301_PATH_MAP_FLAGS)
ifdef ARCH_MAC
  ER301_HOST_EXTRA_CFLAGS += -mmacosx-version-min=$(ER301_MACOS_DEPLOYMENT_TARGET)
endif

# Brian's upstream tree is intentionally treated as a sanctuary. Normal release
# builds compile that untouched recursive engine boundary with warnings muted,
# while Festivamente's Rack-facing sources keep Rack's normal warning policy. This
# is deliberately presentation-only: errors remain fatal and `make
# upstream-warning-audit` rebuilds the same upstream sources with diagnostics
# visible for maintenance. Using the compiler-standard -w here is portable
# across Apple Clang and GCC/MinGW and avoids chasing toolchain-specific warning
# spellings in upstream/generated SWIG code.
ifneq ($(ER301_AUDIT_WARNINGS),1)
  ER301_HOST_EXTRA_CFLAGS += -w
endif
ER301_BUILD_ARGS += ER301_EXTRA_CFLAGS="$(ER301_HOST_EXTRA_CFLAGS)"

ifdef ARCH_WIN
  # Brian's historical windows.mk assumes a different workspace nesting. Pass
  # the pinned repo-local SWIG executable explicitly so a clean Windows build
  # never needs a one-off command-line override.
  ER301_SWIG := $(abspath third-party/tools/swigwin-4.4.1/swig.exe)
  ER301_BUILD_ARGS += SWIG="$(ER301_SWIG)"
endif

# ER301SoundComputer_TARGET is deliberately part of the output path. mac-arm64 and mac-x64
# must never share ER-301 object files merely because both use arch/darwin.
ER301_BUILD := $(ER301_DIR)/$(ER301_PROFILE)/$(ER301SoundComputer_TARGET)
ER301_HOST_LIB := $(ER301_BUILD)/er301-host/liber301host.a
ER301_SUPPORT_LIBS := $(ER301_BUILD)/libs/liblua54.a
ER301_SUPPORT_LIBS += $(ER301_BUILD)/libs/liblodepng.a
ER301_SUPPORT_LIBS += $(ER301_BUILD)/libs/libminiz.a
ER301_LIBS := $(ER301_HOST_LIB) $(ER301_SUPPORT_LIBS)

FLAGS += -DER301_VCV_HOST
FLAGS += -I$(ER301_DIR) -I$(ER301_DIR)/emu -I$(ER301_DIR)/arch/$(ER301_ARCH)
# Rack's default development flags include debug information. Rewrite source
# and SDK prefixes so the binary does not retain the developer's checkout or
# home-directory paths in DWARF data or __FILE__ strings.
FLAGS += -ffile-prefix-map=$(abspath .)=ER301SoundComputer_SOURCE
FLAGS += -fdebug-prefix-map=$(abspath .)=ER301SoundComputer_SOURCE
FLAGS += -ffile-prefix-map=$(abspath $(RACK_DIR))=RACK_SDK
FLAGS += -fdebug-prefix-map=$(abspath $(RACK_DIR))=RACK_SDK

ifdef ARCH_MAC
  # Static libraries are ordered from dependents to dependencies. SDL2's own
  # platform frameworks/system libs come from our target-local sdl2-config.
  LDFLAGS += -lSDL2_ttf -lfreetype -lfftw3f_threads -lfftw3f
  LDFLAGS += $(SDL2_STATIC_LDFLAGS)
else ifdef ARCH_LIN
  # The pinned Linux SDL2 archive needs a small set of normal Linux system
  # libraries. Take those flags from the relocatable target-local sdl2-config.
  LDFLAGS += -lSDL2_ttf -lfreetype -lfftw3f_threads -lfftw3f
  LDFLAGS += $(SDL2_STATIC_LDFLAGS)
else
  LDFLAGS += -lSDL2_ttf -lfreetype -lfftw3f_threads -lfftw3f -lSDL2
endif

# Keep the entire engine archive in this plugin (not just objects referenced by
# the bridge). Additional Rack modules privately load copies of this image, so
# each copy needs its complete ER-301 engine and statically preloaded Core module.
ifdef ARCH_MAC
  LDFLAGS += -Wl,-force_load,$(abspath $(ER301_HOST_LIB))
else
  LDFLAGS += -Wl,--whole-archive $(ER301_HOST_LIB) -Wl,--no-whole-archive
endif
# GNU ld resolves static archives from left to right. The whole ER-301 archive
# introduces references to Lua, SDL, FreeType and FFTW, so GNU-linker targets
# must repeat those dependency archives *after* the engine archive and place
# them in a rescan group.
ifdef ARCH_LIN
  LDFLAGS += -Wl,--start-group
  LDFLAGS += $(ER301_SUPPORT_LIBS)
  LDFLAGS += -lSDL2_ttf -lfreetype
  LDFLAGS += -lfftw3f_threads -lfftw3f
  LDFLAGS += $(SDL2_STATIC_LDFLAGS)
  LDFLAGS += -Wl,--end-group
endif

ifdef ARCH_WIN
  LDFLAGS += -Wl,--start-group
  LDFLAGS += $(ER301_SUPPORT_LIBS)
  LDFLAGS += -lSDL2_ttf -lfreetype
  LDFLAGS += -lfftw3f_threads -lfftw3f
  LDFLAGS += -lSDL2
  LDFLAGS += -Wl,--end-group

  # System libraries required by our static Windows SDL2 build, plus the
  # MinGW pthread implementation used by FFTW's threads library.
  LDFLAGS += -static-libgcc
  WINPTHREAD_STATIC := $(shell gcc -print-file-name=libwinpthread.a)
  LDFLAGS += $(WINPTHREAD_STATIC) -lm
  LDFLAGS += -lkernel32 -luser32 -lgdi32 -lwinmm -limm32
  LDFLAGS += -lole32 -loleaut32 -lversion -luuid -ladvapi32
  LDFLAGS += -lsetupapi -lshell32 -ldinput8
endif

OBJECTS += $(ER301_SUPPORT_LIBS)

SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += README.md
DISTRIBUTABLES += ATTRIBUTION.md
DISTRIBUTABLES += THIRD_PARTY_NOTICES.md

include $(RACK_DIR)/plugin.mk

# Rack's all/dist/install targets ultimately build $(TARGET). Ensure the pinned
# dependency prefix exists before the plugin link recipe expands target-local
# SDL2 linker flags. This preserves one-command `make install` from a fresh clone
# without a second plugin directory or a preliminary build pass.
$(TARGET): | dependencies

# Rack 2.6.6 hard-codes a generic macOS 10.9 floor in compile.mk. Replace that
# single generic flag after plugin.mk has loaded, rather than appending a second
# contradictory deployment flag. CFLAGS/CXXFLAGS retain a recursive reference to
# FLAGS, so this becomes the one deployment target used by plugin compilation
# and final linking.
ifdef ARCH_MAC
  FLAGS := $(filter-out -mmacosx-version-min=%,$(FLAGS))
  FLAGS += -mmacosx-version-min=$(ER301_MACOS_DEPLOYMENT_TARGET)
endif

# Rack's SDK adds -Wno-vla-extension for Clang. GCC does not recognize that
# switch and prints a diagnostic note even on otherwise clean Linux/Windows
# builds. Remove it only for non-Clang C++ drivers after plugin.mk has supplied
# Rack's flags.
ER301_CXX_VERSION := $(shell $(firstword $(CXX)) --version 2>/dev/null | head -n 1)
ifeq ($(findstring clang,$(ER301_CXX_VERSION)),)
  CFLAGS := $(filter-out -Wno-vla-extension,$(CFLAGS))
  CXXFLAGS := $(filter-out -Wno-vla-extension,$(CXXFLAGS))
endif

# The bridge object depends on the complete recursive ER-301 build. This makes
# interrupted clean builds recovery-safe without injecting liber301host.a into
# Rack's automatic `$^` link inputs (the host archive is deliberately linked via
# --whole-archive/-force_load below). A changed engine rebuilds the bridge, which
# in turn relinks the plugin.
build/src/er301_bridge.cpp.o: $(ER301_LIBS)

# A fresh checkout may not have its target-local static dependency prefix yet.
# Prepare/audit it before the recursive ER-301 engine build begins.
$(ER301_LIBS): | dependencies

$(ER301_LIBS):
	+$(MAKE) -C $(ER301_DIR) er301-host $(ER301_BUILD_ARGS)

# Rack's clean target removes plugin output. Add target-specific ER-301 output
# as a prerequisite so successive toolchain targets cannot reuse stale objects.
clean: er301-clean

.PHONY: platform-info binary-audit upstream-warning-audit dependencies third-party-macos-prepare third-party-macos-rebuild third-party-macos-audit third-party-linux-prepare third-party-linux-rebuild third-party-linux-audit third-party-windows-audit audio-test multi-instance-test package-test runtime-check runtime-sync

# Public dependency bootstrap. A fresh clone can use `make install` directly.
# macOS/Linux build pinned static dependencies from the repository-local source
# snapshots only when the target prefix is missing or invalid. Windows ships its
# validated static prefix and pinned SWIG tool, so preparation there is an
# audit-only operation.
ifdef ARCH_MAC
dependencies: third-party-macos-prepare
else ifdef ARCH_LIN
dependencies: third-party-linux-prepare
else ifdef ARCH_WIN
dependencies: third-party-windows-audit
else
dependencies:
	@echo "error: unsupported dependency target $(ARCH_NAME)" >&2; exit 1
endif

# Normal first-build output deliberately stays compact. Full vendor/CMake output
# is captured to a temporary log and is only surfaced when preparation fails.
# Explicit *-rebuild targets remain verbose maintenance tools.
third-party-macos-prepare:
	@set -e; \
	if ER301_MACOS_DEPLOYMENT_TARGET="$(ER301_MACOS_DEPLOYMENT_TARGET)" \
	   ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	   scripts/audit-third-party-macos.sh >/dev/null 2>&1; then \
	  echo "Pinned dependencies: PASS ($(THIRD_PARTY_PLATFORM))"; \
	else \
	  echo "Preparing pinned dependencies ($(THIRD_PARTY_PLATFORM))... This could take a while."; \
	  log="$${TMPDIR:-/tmp}/er301-dependencies-$(THIRD_PARTY_PLATFORM).log"; \
	  rm -f "$$log"; \
	  if ER301_MACOS_DEPLOYMENT_TARGET="$(ER301_MACOS_DEPLOYMENT_TARGET)" \
	     ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	     ER301_BUILD_JOBS="$${ER301_BUILD_JOBS:-1}" \
	     scripts/build-third-party-macos.sh >"$$log" 2>&1; then \
	    echo "Pinned dependencies: PASS ($(THIRD_PARTY_PLATFORM))"; \
	    rm -f "$$log"; \
	  else \
	    status=$$?; \
	    echo "Dependency preparation failed. Last 80 log lines:" >&2; \
	    tail -n 80 "$$log" >&2 || true; \
	    echo "Full dependency log: $$log" >&2; \
	    exit $$status; \
	  fi; \
	fi

# Rebuild the pinned macOS static dependency prefix from the repository-local
# sources using the same deployment floor as this Rack target. The helper is
# intentionally single-threaded by default; ER301_BUILD_JOBS may be raised on a
# disposable/CI machine.
third-party-macos-rebuild:
	@if [ "$(ARCH_OS)" != "mac" ]; then \
	  echo "third-party-macos-rebuild is only valid on macOS" >&2; exit 1; \
	fi
	ER301_MACOS_DEPLOYMENT_TARGET="$(ER301_MACOS_DEPLOYMENT_TARGET)" \
	ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	ER301_BUILD_JOBS="$${ER301_BUILD_JOBS:-1}" \
	scripts/build-third-party-macos.sh

third-party-macos-audit:
	@if [ "$(ARCH_OS)" != "mac" ]; then \
	  echo "third-party-macos-audit is only valid on macOS" >&2; exit 1; \
	fi
	ER301_MACOS_DEPLOYMENT_TARGET="$(ER301_MACOS_DEPLOYMENT_TARGET)" \
	ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	scripts/audit-third-party-macos.sh

third-party-linux-prepare:
	@set -e; \
	if ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	   scripts/audit-third-party-linux.sh >/dev/null 2>&1; then \
	  echo "Pinned dependencies: PASS ($(THIRD_PARTY_PLATFORM))"; \
	else \
	  echo "Preparing pinned dependencies ($(THIRD_PARTY_PLATFORM))... This could take a while."; \
	  log="$${TMPDIR:-/tmp}/er301-dependencies-$(THIRD_PARTY_PLATFORM).log"; \
	  rm -f "$$log"; \
	  if ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	     ER301_BUILD_JOBS="$${ER301_BUILD_JOBS:-1}" \
	     scripts/build-third-party-linux.sh >"$$log" 2>&1; then \
	    echo "Pinned dependencies: PASS ($(THIRD_PARTY_PLATFORM))"; \
	    rm -f "$$log"; \
	  else \
	    status=$$?; \
	    echo "Dependency preparation failed. Last 80 log lines:" >&2; \
	    tail -n 80 "$$log" >&2 || true; \
	    echo "Full dependency log: $$log" >&2; \
	    exit $$status; \
	  fi; \
	fi

# Rebuild and validate the pinned Linux x86_64 dependency prefix. Like the
# macOS helper, this is single-threaded by default to keep validation machines
# responsive and to make failures easy to read.
third-party-linux-rebuild:
	@if [ "$(ARCH_OS)" != "lin" ]; then \
	  echo "third-party-linux-rebuild is only valid on Linux" >&2; exit 1; \
	fi
	ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	ER301_BUILD_JOBS="$${ER301_BUILD_JOBS:-1}" \
	scripts/build-third-party-linux.sh

third-party-linux-audit:
	@if [ "$(ARCH_OS)" != "lin" ]; then \
	  echo "third-party-linux-audit is only valid on Linux" >&2; exit 1; \
	fi
	ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	scripts/audit-third-party-linux.sh

third-party-windows-audit:
	@if [ "$(ARCH_OS)" != "win" ]; then \
	  echo "third-party-windows-audit is only valid on Windows" >&2; exit 1; \
	fi
	@ER301_THIRD_PARTY_PLATFORM="$(THIRD_PARTY_PLATFORM)" \
	scripts/audit-third-party-windows.sh

# Diagnostic-only target: rebuild Brian's untouched engine with the audited
# upstream warnings visible. Normal release builds suppress those warning
# families at the host boundary instead of modifying upstream source.
upstream-warning-audit:
	+$(MAKE) er301-clean ER301_AUDIT_WARNINGS=1
	+$(MAKE) er301 ER301_AUDIT_WARNINGS=1

platform-info:
	@echo "Rack target:          $(ARCH_NAME)"
	@echo "Rack OS / CPU:        $(ARCH_OS) / $(ARCH_CPU)"
	@echo "ER-301 architecture:  $(ER301_ARCH)"
	@echo "ER-301 profile:       $(ER301_PROFILE)"
	@echo "ER-301 build dir:      $(abspath $(ER301_BUILD))"
	@echo "Third-party platform: $(THIRD_PARTY_PLATFORM)"
	@echo "Third-party prefix:   $(abspath $(THIRD_PARTY_PREFIX))"
	@echo "CC:                   $(CC)"
	@echo "CXX/CPP:              $(ER301_CPP)"
	@echo "AR:                   $(ER301_AR)"
	@if [ "$(ARCH_OS)" = "mac" ]; then \
	  echo "macOS deployment:     $(ER301_MACOS_DEPLOYMENT_TARGET)"; \
	fi
	@if [ "$(ARCH_OS)" = "win" ]; then \
	  echo "Windows engine layer: enabled (arch/windows)"; \
	  echo "Windows SWIG:         $(ER301_SWIG)"; \
	fi

# Optional release audit. This does not alter the build; it reports accidental
# runtime dependencies on SDL/FreeType/FFTW that should be statically absorbed
# by the packaged plugin. Rack itself and normal OS libraries are expected.
binary-audit: $(TARGET)
	@set -e; \
	if [ "$(ARCH_OS)" = "mac" ]; then \
	  bad=`otool -L "$(TARGET)" | grep -Ei '(homebrew|/usr/local|SDL2|SDL2_ttf|fftw|freetype)' || true`; \
	elif [ "$(ARCH_OS)" = "lin" ]; then \
	  bad=`ldd "$(TARGET)" | grep -Ei '(SDL2|SDL2_ttf|fftw|freetype)' || true`; \
	elif [ "$(ARCH_OS)" = "win" ]; then \
	  bad=`objdump -p "$(TARGET)" | grep -Ei 'DLL Name:.*(SDL2|SDL2_ttf|fftw|freetype)' || true`; \
	else \
	  bad=""; \
	fi; \
	if [ -n "$$bad" ]; then \
	  echo "Unexpected third-party runtime dependency:" >&2; \
	  echo "$$bad" >&2; \
	  exit 1; \
	fi; \
	echo "Binary dependency audit: PASS"

audio-test:
	ER301_DIR="$(abspath $(ER301_DIR))" RACK_DIR="$(abspath $(RACK_DIR))" ./scripts/run-audio-verification.sh

multi-instance-test:
	./scripts/run-multi-instance-verification.sh

package-test:
	PLUCK_PKG="$(PLUCK_PKG)" ./scripts/run-package-verification.sh

runtime-check:
	@set -e; \
	tmp=`mktemp -d`; \
	trap 'rm -rf "$$tmp"' EXIT; \
	cp -R "$(ER301_DIR)/xroot/." "$$tmp/"; \
	cp res/er301/xroot/Package/Manager.lua "$$tmp/Package/Manager.lua"; \
	diff -qr "$$tmp" res/er301/xroot; \
	grep -q 'installBundledCoreOnce' res/er301/xroot/Package/Manager.lua; \
	echo "Runtime mirror + VCV Core bootstrap: PASS"

runtime-sync:
	./scripts/sync-runtime.sh "$(abspath $(ER301_DIR))"

.PHONY: er301 er301-clean

er301:
	+$(MAKE) -C $(ER301_DIR) er301-host $(ER301_BUILD_ARGS)

er301-clean:
	+$(MAKE) -C $(ER301_DIR) er301-host-clean $(ER301_BUILD_ARGS)

