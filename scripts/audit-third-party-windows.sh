#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-windows-x64}"
PREFIX="${ER301_THIRD_PARTY_PREFIX:-$ROOT/third-party/install/$PLATFORM}"
CC="${ER301_WINDOWS_CC:-x86_64-w64-mingw32-gcc}"
AR="${ER301_WINDOWS_AR:-$($CC -print-prog-name=ar 2>/dev/null || true)}"

if [[ "$PLATFORM" != "windows-x64" ]]; then
  echo "error: ER301_THIRD_PARTY_PLATFORM must be windows-x64" >&2
  exit 1
fi
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "error: Windows dependency compiler not found: $CC" >&2
  exit 1
fi
if [[ -z "$AR" ]] || ! command -v "$AR" >/dev/null 2>&1; then
  echo "error: Windows archive tool not found: ${AR:-<empty>}" >&2
  exit 1
fi

TARGET_TRIPLE="$($CC -dumpmachine)"
COMPILER_VERSION="$($CC -dumpfullversion 2>/dev/null || $CC -dumpversion)"

libs=(
  "$PREFIX/lib/libSDL2.a"
  "$PREFIX/lib/libSDL2main.a"
  "$PREFIX/lib/libSDL2_ttf.a"
  "$PREFIX/lib/libfreetype.a"
  "$PREFIX/lib/libfftw3f.a"
  "$PREFIX/lib/libfftw3f_threads.a"
)
required=(
  "${libs[@]}"
  "$PREFIX/include/fftw3.h"
  "$PREFIX/include/SDL2/SDL.h"
  "$PREFIX/include/SDL2/SDL_ttf.h"
  "$PREFIX/.er301-build-meta"
  "$ROOT/third-party/tools/swigwin-4.4.1/swig.exe"
)

for f in "${required[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "error: missing pinned Windows build dependency: $f" >&2
    echo "Run: make third-party-windows-rebuild" >&2
    exit 1
  fi
done

for lib in "${libs[@]}"; do
  members="$($AR t "$lib" 2>/dev/null || true)"
  if [[ -z "$members" ]]; then
    echo "error: Windows static archive is empty or unreadable: $lib" >&2
    exit 1
  fi
done

shared="$(find "$PREFIX" -type f \( -name '*.dll' -o -name '*.dll.a' \) -print -quit)"
if [[ -n "$shared" ]]; then
  echo "error: shared/import library found in pinned Windows prefix" >&2
  find "$PREFIX" -type f \( -name '*.dll' -o -name '*.dll.a' \) -print >&2
  exit 1
fi

if ! grep -qx 'platform=windows-x64' "$PREFIX/.er301-build-meta" || \
   ! grep -qx "target=$TARGET_TRIPLE" "$PREFIX/.er301-build-meta" || \
   ! grep -qx "compiler_version=$COMPILER_VERSION" "$PREFIX/.er301-build-meta"; then
  echo "error: Windows dependency metadata does not match the active MinGW toolchain" >&2
  echo "  expected target:   $TARGET_TRIPLE" >&2
  echo "  expected compiler: $COMPILER_VERSION" >&2
  echo "Run: make third-party-windows-rebuild" >&2
  exit 1
fi

if ! grep -qx 'sdl2=2.32.10' "$PREFIX/.er301-build-meta" || \
   ! grep -qx 'sdl2_ttf=2.24.0' "$PREFIX/.er301-build-meta" || \
   ! grep -qx 'fftw=3.3.11' "$PREFIX/.er301-build-meta"; then
  echo "error: Windows dependency metadata does not match the pinned source versions" >&2
  exit 1
fi

echo "Windows static dependency audit: PASS (toolchain-matched windows-x64 prefix)"
