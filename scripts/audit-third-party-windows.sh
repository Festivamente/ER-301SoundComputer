#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-windows-x64}"

if [[ "$PLATFORM" != "windows-x64" ]]; then
  echo "error: ER301_THIRD_PARTY_PLATFORM must be windows-x64" >&2
  exit 1
fi

PREFIX="$ROOT/third-party/install/$PLATFORM"
required=(
  "$PREFIX/lib/libSDL2.a"
  "$PREFIX/lib/libSDL2_ttf.a"
  "$PREFIX/lib/libfreetype.a"
  "$PREFIX/lib/libfftw3f.a"
  "$PREFIX/lib/libfftw3f_threads.a"
  "$PREFIX/include/fftw3.h"
  "$ROOT/third-party/tools/swigwin-4.4.1/swig.exe"
)

for f in "${required[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "error: missing pinned Windows build dependency: $f" >&2
    echo "The validated windows-x64 dependency prefix and SWIG tool must be present in the checkout." >&2
    exit 1
  fi
done

echo "Pinned dependencies: PASS (windows-x64)"
