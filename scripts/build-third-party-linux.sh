#!/usr/bin/env bash
set -euo pipefail

# Rebuild the pinned Linux x86_64 static dependencies used by ER-301 Sound
# Computer with the compiler selected by Rack's build environment. This works
# both natively on x86_64 Linux and under Rack's GNU/Linux cross toolchain on an
# arm64 Docker host.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_ROOT="$ROOT/third-party/src"

PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-linux-x64}"
JOBS="${ER301_BUILD_JOBS:-1}"
CC="${ER301_LINUX_CC:-${CC:-cc}}"
CXX="${ER301_LINUX_CXX:-${CXX:-c++}}"
AR="${ER301_LINUX_AR:-$($CC -print-prog-name=ar 2>/dev/null || true)}"
RANLIB="${ER301_LINUX_RANLIB:-$($CC -print-prog-name=ranlib 2>/dev/null || true)}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "error: Linux third-party rebuild must run on Linux" >&2
  exit 1
fi
if [[ "$PLATFORM" != "linux-x64" ]]; then
  echo "error: ER301_THIRD_PARTY_PLATFORM must be linux-x64" >&2
  exit 1
fi

for tool in cmake make "$CC" "$CXX" "$AR" "$RANLIB"; do
  if [[ -z "$tool" ]] || ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required Linux dependency build tool not found: ${tool:-<empty>}" >&2
    exit 1
  fi
done

# Use absolute tool paths. This matters under CMake cross-compilation, where a
# bare target-prefixed binutils name can otherwise be interpreted relative to
# the plugin source/build directory.
CC="$(command -v "$CC")"
CXX="$(command -v "$CXX")"
AR="$(command -v "$AR")"
RANLIB="$(command -v "$RANLIB")"

TARGET_TRIPLE="$($CC -dumpmachine)"
case "$TARGET_TRIPLE" in
  x86_64*linux*) ;;
  *)
    echo "error: Linux dependency compiler targets '$TARGET_TRIPLE', expected x86_64 Linux" >&2
    exit 1
    ;;
esac
COMPILER_VERSION="$($CC -dumpfullversion 2>/dev/null || $CC -dumpversion)"

PREFIX="$ROOT/third-party/install/$PLATFORM"
BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/er301-third-party-${PLATFORM}.XXXXXX")"
cleanup() {
  if [[ "${ER301_KEEP_THIRD_PARTY_BUILD:-0}" == "1" ]]; then
    echo "Keeping temporary build tree: $BUILD_ROOT"
  else
    rm -rf "$BUILD_ROOT"
  fi
}
trap cleanup EXIT

run_cmake_build() {
  local name="$1"
  shift
  local build="$BUILD_ROOT/$name"
  cmake "$@" -B "$build" \
    -G "Unix Makefiles" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_AR="$AR" \
    -DCMAKE_RANLIB="$RANLIB"
  cmake --build "$build" --parallel "$JOBS"
  cmake --install "$build"
}

echo "Rebuilding pinned Linux dependencies"
echo "  platform: $PLATFORM"
echo "  target:   $TARGET_TRIPLE"
echo "  compiler: $CC $COMPILER_VERSION"
echo "  host:     $(uname -m)"
echo "  jobs:     $JOBS"
echo "  prefix:   $PREFIX"

rm -rf "$PREFIX"
mkdir -p "$PREFIX"

# SDL2: static-only and intentionally headless/portable for the Rack plugin.
# VCV provides the actual window/audio host; the ER-301 engine only needs SDL core,
# software rendering, timers, threads/events, and dummy/offscreen backends. Disabling
# X11/Wayland/ALSA/etc. avoids requiring distribution-specific development packages.
run_cmake_build sdl2 \
  -S "$SRC_ROOT/SDL2-2.32.10" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL_SHARED=OFF \
  -DSDL_STATIC=ON \
  -DSDL_TEST=OFF \
  -DSDL_TESTS=OFF \
  -DSDL_INSTALL_TESTS=OFF \
  -DSDL_X11=OFF \
  -DSDL_WAYLAND=OFF \
  -DSDL_KMSDRM=OFF \
  -DSDL_RPI=OFF \
  -DSDL_ALSA=OFF \
  -DSDL_JACK=OFF \
  -DSDL_PIPEWIRE=OFF \
  -DSDL_PULSEAUDIO=OFF \
  -DSDL_ESD=OFF \
  -DSDL_SNDIO=OFF \
  -DSDL_LIBSAMPLERATE=OFF \
  -DSDL_OFFSCREEN=ON \
  -DSDL_DUMMYVIDEO=ON \
  -DSDL_DUMMYAUDIO=ON

# SDL2_ttf: static-only with its pinned vendored FreeType and no HarfBuzz.
run_cmake_build sdl2_ttf \
  -S "$SRC_ROOT/SDL2_ttf-2.24.0" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL2TTF_INSTALL=ON \
  -DSDL2TTF_SAMPLES=OFF \
  -DSDL2TTF_VENDORED=ON \
  -DSDL2TTF_HARFBUZZ=OFF

# FFTW: cross-compile for Rack's linux-x64 target, single precision + pthreads,
# static-only. --host prevents configure from trying to execute an x86_64 test
# binary when the Docker host itself is arm64.
FFTW_BUILD="$BUILD_ROOT/fftw"
mkdir -p "$FFTW_BUILD"
(
  cd "$FFTW_BUILD"
  if ! env \
    CC="$CC" \
    CPP="$CC -E" \
    AR="$AR" \
    RANLIB="$RANLIB" \
    CFLAGS="-O3 -fPIC" \
    "$SRC_ROOT/fftw-3.3.11/configure" \
      --host="$TARGET_TRIPLE" \
      --prefix="$PREFIX" \
      --disable-shared \
      --enable-static \
      --enable-float \
      --enable-threads \
      --disable-fortran \
      --enable-sse2; then
    echo "error: FFTW configure failed; tail of config.log follows" >&2
    tail -n 100 config.log >&2 || true
    exit 1
  fi
  make -j"$JOBS"
  make install
)

# Keep only inputs consumed by the plugin build. Remove shared libraries and
# machine-specific package metadata so the canonical prefix stays static and
# relocatable.
find "$PREFIX" -type f \( -name '*.so' -o -name '*.so.*' -o -name '*.la' \) -delete 2>/dev/null || true
rm -rf "$PREFIX/lib/cmake" "$PREFIX/lib/pkgconfig" "$PREFIX/share" 2>/dev/null || true
if [[ -d "$PREFIX/bin" ]]; then
  find "$PREFIX/bin" -type f ! -name 'sdl2-config' -delete 2>/dev/null || true
fi

required=(
  "$PREFIX/lib/libSDL2.a"
  "$PREFIX/lib/libSDL2_ttf.a"
  "$PREFIX/lib/libfreetype.a"
  "$PREFIX/lib/libfftw3f.a"
  "$PREFIX/lib/libfftw3f_threads.a"
  "$PREFIX/bin/sdl2-config"
  "$PREFIX/include/fftw3.h"
)
for f in "${required[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "error: expected dependency artifact was not produced: $f" >&2
    exit 1
  fi
done

cat > "$PREFIX/.er301-build-meta" <<META
platform=$PLATFORM
arch=x86_64
target=$TARGET_TRIPLE
compiler=$CC
compiler_version=$COMPILER_VERSION
sdl2=2.32.10
sdl2_ttf=2.24.0
fftw=3.3.11
META

ER301_THIRD_PARTY_PLATFORM="$PLATFORM" \
ER301_THIRD_PARTY_PREFIX="$PREFIX" \
ER301_LINUX_CC="$CC" \
ER301_LINUX_AR="$AR" \
"$SCRIPT_DIR/audit-third-party-linux.sh"

echo "Pinned Linux dependency rebuild: PASS"
