#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE_ROOT="$ROOT/er-301-native"
BUILD="$ROOT/build/package-test"
CXX="${CXX:-c++}"
PLUCK_PKG="${PLUCK_PKG:-${1:-}}"

if [[ -z "$PLUCK_PKG" || ! -f "$PLUCK_PKG" ]]; then
  echo "Set PLUCK_PKG=/path/to/Pluck-0.6.0.pkg (or pass it as argument)." >&2
  exit 2
fi

rm -rf "$BUILD"
mkdir -p "$BUILD/package"
unzip -q "$PLUCK_PKG" -d "$BUILD/package"
[[ -f "$BUILD/package/libPluck.so" ]]

"$CXX" -std=c++11 -Wall -Wextra -Werror \
  -I"$ENGINE_ROOT/emu" \
  "$ROOT/test/native_package_patch_test.cpp" \
  "$ENGINE_ROOT/emu/NativePackageMachO.cpp" \
  -o "$BUILD/native_package_patch_test"

ENGINE_IMAGE="/tmp/ER-301SoundComputerPackageTestEngine.dylib"
PATCHED="$BUILD/libPluck.er-301soundcomputer.so"
"$BUILD/native_package_patch_test" \
  "$BUILD/package/libPluck.so" "$PATCHED" "$ENGINE_IMAGE"

file "$PATCHED" | grep -q 'Mach-O 64-bit arm64'
strings "$PATCHED" | grep -q '/tmp/ER-301SoundComputerPackageTestEngine.dylib'
strings "$PATCHED" | grep -q '_soundcomputer_compat_inlet_ctor'
strings "$PATCHED" | grep -q '_soundcomputer_compat_outlet_ctor'

if command -v llvm-objdump >/dev/null 2>&1; then
  llvm-objdump --macho --dylibs-used "$PATCHED" | \
    grep -q '/tmp/ER-301SoundComputerPackageTestEngine.dylib'
  llvm-objdump --macho --lazy-bind "$PATCHED" | \
    grep -q 'ER-301SoundComputerPackageTestEngine.*_soundcomputer_compat_inlet_ctor'
elif command -v otool >/dev/null 2>&1; then
  otool -L "$PATCHED" | grep -q '/tmp/ER-301SoundComputerPackageTestEngine.dylib'
fi

# Source contracts: one canonical archive repository, one shared installed
# package tree, isolated native image preparation, and no legacy imports.
grep -q 'paths.frontPath = system::join(sharedRoot, "front")' "$ROOT/src/ER301.cpp"
grep -q 'system::join(paths.frontPath, "ER-301", "packages")' "$ROOT/src/ER301.cpp"
grep -q 'seedBundledPackageOnce' "$ROOT/src/ER301.cpp"
[[ -f "$ROOT/res/er301/packages/core-0.7.0-dev1.8.pkg" ]]
grep -q 'installBundledCoreOnce' \
  "$ROOT/res/er301/xroot/Package/Manager.lua"
grep -q 'prepareSharedInstalledPackages' "$ROOT/src/ER301.cpp"
if grep -q 'legacyRepositories\|copyPackageArchives' "$ROOT/src/ER301.cpp"; then
  echo "Legacy package import is still present." >&2
  exit 1
fi
grep -q 'soundcomputer_prepare_native_package' \
  "$ENGINE_ROOT/libs/lua54/loadlib.c"
grep -q 'patchNativePackageMachO' \
  "$ENGINE_ROOT/emu/NativePackageHost.cpp"
grep -q 'lua_setglobal(L, "dirAll")' \
  "$ENGINE_ROOT/od/glue/AppInterpreter.cpp"
grep -q 'local iterator = dirAll or dir' \
  "$ENGINE_ROOT/xroot/Path.lua"
grep -q 'removeInstallationFolder' \
  "$ENGINE_ROOT/xroot/Package/Manager.lua"
grep -q 'Removing stale unregistered installation folder' \
  "$ENGINE_ROOT/xroot/Package/Manager.lua"
if grep -Eq 'External sample libraries|externalLibraryManager' \
  "$ROOT/src/ER301.cpp"; then
  echo "Obsolete External Libraries UI/runtime initialization is still present." >&2
  exit 1
fi
if [[ -e "$ROOT/src/ExternalLibraries.cpp" || \
      -e "$ROOT/src/ExternalLibraries.hpp" ]]; then
  echo "Obsolete External Libraries implementation is still present." >&2
  exit 1
fi
if grep -q 'native ER-301 packages are not yet supported' \
  "$ENGINE_ROOT/libs/lua54/loadlib.c"; then
  echo "Secondary native-package rejection is still present." >&2
  exit 1
fi

echo "Package uninstall and front-layout verification: PASS"
