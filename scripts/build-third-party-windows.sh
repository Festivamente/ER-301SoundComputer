#!/usr/bin/env bash
set -euo pipefail

# Rebuild the pinned Windows x86_64 static dependencies used by ER-301 Sound
# Computer with the compiler selected by Rack's build environment. This is
# important for the official rack-plugin-toolchain: prebuilt MinGW archives from
# a different CRT/toolchain can compile successfully but fail at the final DLL
# link with unresolved __imp_* CRT symbols.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_ROOT="$ROOT/third-party/src"

PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-windows-x64}"
JOBS="${ER301_BUILD_JOBS:-1}"
CC="${ER301_WINDOWS_CC:-x86_64-w64-mingw32-gcc}"
CXX="${ER301_WINDOWS_CXX:-x86_64-w64-mingw32-g++}"
AR="${ER301_WINDOWS_AR:-$($CC -print-prog-name=ar 2>/dev/null || true)}"
RANLIB="${ER301_WINDOWS_RANLIB:-$($CC -print-prog-name=ranlib 2>/dev/null || true)}"

if [[ "$PLATFORM" != "windows-x64" ]]; then
  echo "error: ER301_THIRD_PARTY_PLATFORM must be windows-x64" >&2
  exit 1
fi

for tool in cmake make "$CC" "$CXX" "$AR" "$RANLIB"; do
  if [[ -z "$tool" ]] || ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required Windows dependency build tool not found: ${tool:-<empty>}" >&2
    exit 1
  fi
done

# CMake treats non-absolute compiler/binutils names passed in CMAKE_* variables
# as paths relative to its source/build context in some cross-compile checks.
# Resolve every MinGW tool to its absolute path before handing it to CMake.
CC="$(command -v "$CC")"
CXX="$(command -v "$CXX")"
AR="$(command -v "$AR")"
RANLIB="$(command -v "$RANLIB")"

TARGET_TRIPLE="$($CC -dumpmachine)"
case "$TARGET_TRIPLE" in
  x86_64-w64-mingw32*) ;;
  *)
    echo "error: Windows dependency compiler targets '$TARGET_TRIPLE', expected x86_64-w64-mingw32" >&2
    exit 1
    ;;
esac

# GCC's -print-prog-name=windres may return the bare string "windres" even
# though crosstool-ng installs the prefixed MinGW resource compiler. Rack's
# Windows build puts that target bin directory on PATH, so prefer the tool
# derived directly from the compiler target triple.
if [[ -n "${ER301_WINDOWS_RC:-}" ]]; then
  RC="$ER301_WINDOWS_RC"
elif command -v "${TARGET_TRIPLE}-windres" >/dev/null 2>&1; then
  RC="${TARGET_TRIPLE}-windres"
else
  RC="$($CC -print-prog-name=windres 2>/dev/null || true)"
fi
if [[ -z "$RC" ]] || ! command -v "$RC" >/dev/null 2>&1; then
  echo "error: required Windows resource compiler not found (tried ${TARGET_TRIPLE}-windres and ${RC:-<empty>})" >&2
  exit 1
fi
RC="$(command -v "$RC")"

COMPILER_VERSION="$($CC -dumpfullversion 2>/dev/null || $CC -dumpversion)"
FINAL_PREFIX="$ROOT/third-party/install/$PLATFORM"
BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/er301-third-party-${PLATFORM}.XXXXXX")"
PREFIX="$BUILD_ROOT/install/$PLATFORM"

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
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_AR="$AR" \
    -DCMAKE_RANLIB="$RANLIB" \
    -DCMAKE_RC_COMPILER="$RC"
  cmake --build "$build" --parallel "$JOBS"
  cmake --install "$build"
}

echo "Rebuilding pinned Windows dependencies"
echo "  platform: $PLATFORM"
echo "  target:   $TARGET_TRIPLE"
echo "  compiler: $CC $COMPILER_VERSION"
echo "  jobs:     $JOBS"
echo "  prefix:   $FINAL_PREFIX"

mkdir -p "$PREFIX"

# SDL2: static-only. Building it with Rack's MinGW compiler keeps its CRT imports
# consistent with the compiler used for plugin.dll.
run_cmake_build sdl2 \
  -S "$SRC_ROOT/SDL2-2.32.10" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL_SHARED=OFF \
  -DSDL_STATIC=ON \
  -DSDL_TEST=OFF \
  -DSDL_TESTS=OFF \
  -DSDL_INSTALL_TESTS=OFF

# SDL2_ttf: static-only, with the pinned vendored FreeType and no HarfBuzz.
run_cmake_build sdl2_ttf \
  -S "$SRC_ROOT/SDL2_ttf-2.24.0" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL2TTF_INSTALL=ON \
  -DSDL2TTF_SAMPLES=OFF \
  -DSDL2TTF_VENDORED=ON \
  -DSDL2TTF_HARFBUZZ=OFF

# FFTW: cross-compiled for MinGW x86_64, single precision + pthreads, static-only.
# --host is required so configure never tries to execute a Windows test program
# on the Linux host used by rack-plugin-toolchain.
FFTW_BUILD="$BUILD_ROOT/fftw"
mkdir -p "$FFTW_BUILD"
(
  cd "$FFTW_BUILD"
  if ! env \
    CC="$CC" \
    CPP="$CC -E" \
    AR="$AR" \
    RANLIB="$RANLIB" \
    CFLAGS="-O3" \
    "$SRC_ROOT/fftw-3.3.11/configure" \
      --host="$TARGET_TRIPLE" \
      --prefix="$PREFIX" \
      --disable-shared \
      --enable-static \
      --enable-float \
      --enable-threads \
      --disable-fortran \
      --enable-sse2 \
      --with-our-malloc; then
    echo "error: FFTW configure failed; tail of config.log follows" >&2
    tail -n 100 config.log >&2 || true
    exit 1
  fi
  make -j"$JOBS"
  make install
)

# Keep the dependency prefix static and relocatable. Import/shared-library and
# package-manager metadata are not consumed by the plugin build.
find "$PREFIX" -type f \( -name '*.dll' -o -name '*.dll.a' -o -name '*.la' \) -delete 2>/dev/null || true
rm -rf "$PREFIX/lib/cmake" "$PREFIX/lib/pkgconfig" "$PREFIX/share" 2>/dev/null || true

required=(
  "$PREFIX/lib/libSDL2.a"
  "$PREFIX/lib/libSDL2main.a"
  "$PREFIX/lib/libSDL2_ttf.a"
  "$PREFIX/lib/libfreetype.a"
  "$PREFIX/lib/libfftw3f.a"
  "$PREFIX/lib/libfftw3f_threads.a"
  "$PREFIX/include/fftw3.h"
  "$PREFIX/include/SDL2/SDL.h"
  "$PREFIX/include/SDL2/SDL_ttf.h"
)
for f in "${required[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "error: expected Windows dependency artifact was not produced: $f" >&2
    exit 1
  fi
done

cat > "$PREFIX/.er301-build-meta" <<META
platform=$PLATFORM
target=$TARGET_TRIPLE
compiler=$CC
compiler_version=$COMPILER_VERSION
sdl2=2.32.10
sdl2_ttf=2.24.0
fftw=3.3.11
META

# Validate the staged prefix before replacing the checkout's existing prefix.
ER301_THIRD_PARTY_PLATFORM="$PLATFORM" \
ER301_THIRD_PARTY_PREFIX="$PREFIX" \
ER301_WINDOWS_CC="$CC" \
ER301_WINDOWS_AR="$AR" \
"$SCRIPT_DIR/audit-third-party-windows.sh"

rm -rf "$FINAL_PREFIX"
mkdir -p "$(dirname "$FINAL_PREFIX")"
mv "$PREFIX" "$FINAL_PREFIX"

echo "Pinned Windows dependency rebuild: PASS"
