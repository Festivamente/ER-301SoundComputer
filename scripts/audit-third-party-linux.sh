#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-linux-x64}"
PREFIX="${ER301_THIRD_PARTY_PREFIX:-$ROOT/third-party/install/$PLATFORM}"
CC="${ER301_LINUX_CC:-${CC:-cc}}"
AR="${ER301_LINUX_AR:-$($CC -print-prog-name=ar 2>/dev/null || true)}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "error: Linux third-party audit must run on Linux" >&2
  exit 1
fi
if [[ "$PLATFORM" != "linux-x64" ]]; then
  echo "error: ER301_THIRD_PARTY_PLATFORM must be linux-x64" >&2
  exit 1
fi
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "error: Linux dependency compiler not found: $CC" >&2
  exit 1
fi
if [[ -z "$AR" ]] || ! command -v "$AR" >/dev/null 2>&1; then
  echo "error: Linux archive tool not found: ${AR:-<empty>}" >&2
  exit 1
fi

CC="$(command -v "$CC")"
AR="$(command -v "$AR")"
TARGET_TRIPLE="$($CC -dumpmachine)"
COMPILER_VERSION="$($CC -dumpfullversion 2>/dev/null || $CC -dumpversion)"

libs=(
  "$PREFIX/lib/libSDL2.a"
  "$PREFIX/lib/libSDL2_ttf.a"
  "$PREFIX/lib/libfreetype.a"
  "$PREFIX/lib/libfftw3f.a"
  "$PREFIX/lib/libfftw3f_threads.a"
)
required=(
  "${libs[@]}"
  "$PREFIX/bin/sdl2-config"
  "$PREFIX/include/fftw3.h"
  "$PREFIX/.er301-build-meta"
)

for f in "${required[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "error: missing pinned Linux dependency: $f" >&2
    echo "Run: make third-party-linux-rebuild" >&2
    exit 1
  fi
done

for lib in "${libs[@]}"; do
  members="$($AR t "$lib" 2>/dev/null || true)"
  if [[ -z "$members" ]]; then
    echo "error: static archive is empty or unreadable: $lib" >&2
    exit 1
  fi
done

shared="$(find "$PREFIX" -type f \( -name '*.so' -o -name '*.so.*' \) -print -quit)"
if [[ -n "$shared" ]]; then
  echo "error: shared library found in pinned Linux prefix" >&2
  find "$PREFIX" -type f \( -name '*.so' -o -name '*.so.*' \) -print >&2
  exit 1
fi

if ! grep -qx 'platform=linux-x64' "$PREFIX/.er301-build-meta" || \
   ! grep -qx 'arch=x86_64' "$PREFIX/.er301-build-meta" || \
   ! grep -qx "target=$TARGET_TRIPLE" "$PREFIX/.er301-build-meta" || \
   ! grep -qx "compiler_version=$COMPILER_VERSION" "$PREFIX/.er301-build-meta"; then
  echo "error: Linux dependency metadata does not match the active linux-x64 toolchain" >&2
  echo "  expected target:   $TARGET_TRIPLE" >&2
  echo "  expected compiler: $COMPILER_VERSION" >&2
  exit 1
fi

if ! grep -qx 'sdl2=2.32.10' "$PREFIX/.er301-build-meta" || \
   ! grep -qx 'sdl2_ttf=2.24.0' "$PREFIX/.er301-build-meta" || \
   ! grep -qx 'fftw=3.3.11' "$PREFIX/.er301-build-meta"; then
  echo "error: Linux dependency metadata does not match the pinned source versions" >&2
  exit 1
fi

sdl_flags="$("$PREFIX/bin/sdl2-config" --static-libs)"
if [[ "$sdl_flags" != *"$PREFIX/lib/libSDL2.a"* ]]; then
  echo "error: sdl2-config does not resolve to the pinned libSDL2.a" >&2
  echo "$sdl_flags" >&2
  exit 1
fi

echo "Linux static dependency audit: PASS (toolchain-matched linux-x64 prefix)"
