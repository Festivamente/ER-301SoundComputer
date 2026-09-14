#!/usr/bin/env bash
set -euo pipefail

# Rebuild the pinned macOS static dependencies used by ER-301 Sound Computer.
# This script intentionally uses repository-local sources and a target-local
# install prefix. It does not use Homebrew libraries and never edits upstream
# ER-301 source.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_ROOT="$ROOT/third-party/src"

PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-}"
DEPLOYMENT="${ER301_MACOS_DEPLOYMENT_TARGET:-}"
JOBS="${ER301_BUILD_JOBS:-1}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: macOS third-party rebuild must run on macOS" >&2
  exit 1
fi

case "$PLATFORM" in
  macos-arm64)
    CMAKE_ARCH="arm64"
    FFTW_SIMD="--enable-neon"
    : "${DEPLOYMENT:=11.0}"
    ;;
  macos-x86_64)
    CMAKE_ARCH="x86_64"
    FFTW_SIMD="--enable-sse2"
    : "${DEPLOYMENT:=10.9}"
    ;;
  *)
    echo "error: ER301_THIRD_PARTY_PLATFORM must be macos-arm64 or macos-x86_64" >&2
    exit 1
    ;;
esac

PREFIX="$ROOT/third-party/install/$PLATFORM"

if ! command -v cmake >/dev/null 2>&1; then
  echo "error: cmake is required to rebuild pinned macOS dependencies" >&2
  exit 1
fi

# Prefer Apple's standalone Command Line Tools when installed. This keeps the
# dependency build independent of the Xcode GUI/application bundle.
if [[ -x /Library/Developer/CommandLineTools/usr/bin/clang ]]; then
  export DEVELOPER_DIR=/Library/Developer/CommandLineTools
  CC=/Library/Developer/CommandLineTools/usr/bin/clang
  CXX=/Library/Developer/CommandLineTools/usr/bin/clang++
else
  CC=/usr/bin/clang
  CXX=/usr/bin/clang++
fi

SDK="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
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
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_OSX_ARCHITECTURES="$CMAKE_ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT" \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX"
  cmake --build "$build" --parallel "$JOBS"
  cmake --install "$build"
}

echo "Rebuilding pinned macOS dependencies"
echo "  platform:   $PLATFORM"
echo "  arch:       $CMAKE_ARCH"
echo "  deployment: macOS $DEPLOYMENT"
echo "  jobs:       $JOBS"
echo "  prefix:     $PREFIX"

rm -rf "$PREFIX"
mkdir -p "$PREFIX"

# SDL2: static-only. Disable test libraries/programs; the ER-301 host needs the
# normal macOS audio/video/input backends and SDL2main metadata.
run_cmake_build sdl2 \
  -S "$SRC_ROOT/SDL2-2.32.10" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL_SHARED=OFF \
  -DSDL_STATIC=ON \
  -DSDL_TEST=OFF \
  -DSDL_TESTS=OFF \
  -DSDL_INSTALL_TESTS=OFF

# SDL2_ttf: static-only with its pinned vendored FreeType and no HarfBuzz.
# CMAKE_PREFIX_PATH points it at the SDL2 prefix built immediately above.
run_cmake_build sdl2_ttf \
  -S "$SRC_ROOT/SDL2_ttf-2.24.0" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL2TTF_INSTALL=ON \
  -DSDL2TTF_SAMPLES=OFF \
  -DSDL2TTF_VENDORED=ON \
  -DSDL2TTF_HARFBUZZ=OFF

# FFTW: single precision + pthreads, static-only. This is a native build on
# each validation Mac, so do not force an Autoconf --host triplet. Passing an
# incomplete host triplet (for example aarch64-apple-darwin) can make modern
# Apple Clang get treated as a cross compiler and fail the initial executable
# probe. Keep the architecture and SDK explicit in the compiler/linker flags.
FFTW_BUILD="$BUILD_ROOT/fftw"
mkdir -p "$FFTW_BUILD"
(
  cd "$FFTW_BUILD"
  if ! env \
    CC="$CC" \
    CPP="$CC -E" \
    CFLAGS="-O3 -fPIC -arch $CMAKE_ARCH -isysroot $SDK -mmacosx-version-min=$DEPLOYMENT" \
    CPPFLAGS="-isysroot $SDK -mmacosx-version-min=$DEPLOYMENT" \
    LDFLAGS="-arch $CMAKE_ARCH -isysroot $SDK -mmacosx-version-min=$DEPLOYMENT" \
    MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT" \
    "$SRC_ROOT/fftw-3.3.11/configure" \
      --prefix="$PREFIX" \
      --disable-shared \
      --enable-static \
      --enable-float \
      --enable-threads \
      --disable-fortran \
      "$FFTW_SIMD"; then
    echo "error: FFTW configure failed; tail of config.log follows" >&2
    tail -n 100 config.log >&2 || true
    exit 1
  fi
  make -j"$JOBS"
  make install
)

# Keep only the relocatable build inputs the plugin actually consumes. CMake,
# pkg-config, libtool and manpage metadata frequently capture absolute paths and
# are deliberately not part of the canonical source archive.
find "$PREFIX" -type f \( -name '*.dylib' -o -name '*.la' \) -delete 2>/dev/null || true
rm -rf "$PREFIX/lib/cmake" "$PREFIX/lib/pkgconfig" "$PREFIX/share" 2>/dev/null || true
find "$PREFIX/bin" -type f ! -name 'sdl2-config' -delete 2>/dev/null || true

required=(
  "$PREFIX/lib/libSDL2.a"
  "$PREFIX/lib/libSDL2main.a"
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
arch=$CMAKE_ARCH
macos_deployment_target=$DEPLOYMENT
sdl2=2.32.10
sdl2_ttf=2.24.0
fftw=3.3.11
META

ER301_THIRD_PARTY_PLATFORM="$PLATFORM" \
ER301_MACOS_DEPLOYMENT_TARGET="$DEPLOYMENT" \
"$SCRIPT_DIR/audit-third-party-macos.sh"

echo "Pinned macOS dependency rebuild: PASS"
