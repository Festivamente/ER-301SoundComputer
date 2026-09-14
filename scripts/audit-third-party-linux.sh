#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-linux-x64}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "error: Linux third-party audit must run on Linux" >&2
  exit 1
fi
if [[ "$PLATFORM" != "linux-x64" ]]; then
  echo "error: ER301_THIRD_PARTY_PLATFORM must be linux-x64" >&2
  exit 1
fi

PREFIX="$ROOT/third-party/install/$PLATFORM"
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
  members="$(ar t "$lib" 2>/dev/null || true)"
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
   ! grep -qx 'arch=x86_64' "$PREFIX/.er301-build-meta"; then
  echo "error: Linux dependency metadata does not describe linux-x64/x86_64" >&2
  exit 1
fi

sdl_flags="$("$PREFIX/bin/sdl2-config" --static-libs)"
if [[ "$sdl_flags" != *"$PREFIX/lib/libSDL2.a"* ]]; then
  echo "error: sdl2-config does not resolve to the pinned libSDL2.a" >&2
  echo "$sdl_flags" >&2
  exit 1
fi

echo "Linux static dependency audit: PASS (repo-local linux-x64 prefix)"
