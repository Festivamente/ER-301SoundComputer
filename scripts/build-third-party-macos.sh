#!/usr/bin/env bash
set -euo pipefail

# Rebuild the pinned macOS static dependencies used by ER-301 Sound Computer.
# Sources and install prefixes stay repository-local. The build may run either
# natively on macOS or under Rack's GNU/Linux -> macOS cross toolchain.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_ROOT="$ROOT/third-party/src"

PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-}"
DEPLOYMENT="${ER301_MACOS_DEPLOYMENT_TARGET:-}"
JOBS="${ER301_BUILD_JOBS:-1}"
HOST_OS="$(uname -s)"

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

CROSS_BUILD=0
TARGET_TRIPLE=""
AR=""
RANLIB=""

if [[ "$HOST_OS" == "Darwin" ]]; then
  # Preserve the previously proven native-Mac path exactly: prefer Apple's
  # standalone Command Line Tools and obtain the active SDK with Apple xcrun.
  if [[ -x /Library/Developer/CommandLineTools/usr/bin/clang ]]; then
    export DEVELOPER_DIR=/Library/Developer/CommandLineTools
    CC=/Library/Developer/CommandLineTools/usr/bin/clang
    CXX=/Library/Developer/CommandLineTools/usr/bin/clang++
  else
    CC=/usr/bin/clang
    CXX=/usr/bin/clang++
  fi
  XCRUN=/usr/bin/xcrun
  SDK="$($XCRUN --sdk macosx --show-sdk-path)"
else
  # Rack's Plugin Toolchain builds macOS targets from GNU/Linux. In that case,
  # the target compiler and OSXCross xcrun must come from Rack's environment;
  # never substitute the Linux host compiler.
  CROSS_BUILD=1
  CC="${ER301_MACOS_CC:-${CC:-}}"
  CXX="${ER301_MACOS_CXX:-${CXX:-}}"
  TARGET_TRIPLE="${ER301_MACOS_TARGET_TRIPLE:-}"

  if [[ -z "$CC" || -z "$CXX" ]]; then
    echo "error: macOS cross-build requires Rack's target CC and CXX" >&2
    exit 1
  fi
  if [[ -z "$TARGET_TRIPLE" ]]; then
    TARGET_TRIPLE="$($CC -dumpmachine 2>/dev/null || true)"
  fi
  if [[ "$TARGET_TRIPLE" != *darwin* ]]; then
    echo "error: macOS cross-build target is not Darwin: ${TARGET_TRIPLE:-unknown}" >&2
    exit 1
  fi

  XCRUN="${ER301_MACOS_XCRUN:-$(command -v xcrun || true)}"
  if [[ -z "$XCRUN" ]]; then
    echo "error: macOS cross-build requires OSXCross xcrun in PATH" >&2
    exit 1
  fi
  SDK="$($XCRUN --show-sdk-path)"
  AR="$($XCRUN -f ar)"
  RANLIB="$($XCRUN -f ranlib)"
fi

if [[ -z "$SDK" || ! -d "$SDK" ]]; then
  echo "error: macOS SDK path is unavailable: ${SDK:-<empty>}" >&2
  exit 1
fi

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

  if [[ "$CROSS_BUILD" == "1" ]]; then
    cmake "$@" -B "$build" \
      -G "Unix Makefiles" \
      -DCMAKE_SYSTEM_NAME=Darwin \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_OSX_ARCHITECTURES="$CMAKE_ARCH" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT" \
      -DCMAKE_OSX_SYSROOT="$SDK" \
      -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_AR="$AR" \
      -DCMAKE_RANLIB="$RANLIB"
  else
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
  fi

  cmake --build "$build" --parallel "$JOBS"
  cmake --install "$build"
}

echo "Rebuilding pinned macOS dependencies"
echo "  host:       $HOST_OS"
echo "  platform:   $PLATFORM"
echo "  arch:       $CMAKE_ARCH"
echo "  deployment: macOS $DEPLOYMENT"
echo "  cc:         $CC"
echo "  cxx:        $CXX"
echo "  sdk:        $SDK"
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

# FFTW: single precision + pthreads, static-only. Native Mac builds intentionally
# keep their historical configure invocation. Cross-builds provide Autoconf the
# Darwin host triplet so it never attempts to execute a target Mach-O program on
# the GNU/Linux build host.
FFTW_BUILD="$BUILD_ROOT/fftw"
mkdir -p "$FFTW_BUILD"
(
  cd "$FFTW_BUILD"
  if [[ "$CROSS_BUILD" == "1" ]]; then
    if ! env \
      CC="$CC" \
      CPP="$CC -E" \
      CFLAGS="-O3 -fPIC -arch $CMAKE_ARCH -isysroot $SDK -mmacosx-version-min=$DEPLOYMENT" \
      CPPFLAGS="-isysroot $SDK -mmacosx-version-min=$DEPLOYMENT" \
      LDFLAGS="-arch $CMAKE_ARCH -isysroot $SDK -mmacosx-version-min=$DEPLOYMENT" \
      MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT" \
      "$SRC_ROOT/fftw-3.3.11/configure" \
        --host="$TARGET_TRIPLE" \
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
  else
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
ER301_MACOS_XCRUN="${XCRUN:-}" \
"$SCRIPT_DIR/audit-third-party-macos.sh"

echo "Pinned macOS dependency rebuild: PASS"
