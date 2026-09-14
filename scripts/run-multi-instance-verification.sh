#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/multi-instance-test"
CXX="${CXX:-c++}"
rm -rf "$BUILD"
mkdir -p "$BUILD/clones"

case "$(uname -s)" in
  Darwin)
    PROBE="$BUILD/ER-301SoundComputerProbe.dylib"
    "$CXX" -std=c++11 -dynamiclib \
      -Wl,-install_name,ER-301SoundComputerProbe.dylib \
      "$ROOT/test/image_clone_probe.cpp" -o "$PROBE"
    "$CXX" -std=c++11 "$ROOT/test/image_clone_test.cpp" \
      -o "$BUILD/image_clone_test"
    ;;
  Linux)
    PROBE="$BUILD/ER-301SoundComputerProbe.so"
    "$CXX" -std=c++11 -fPIC -shared \
      -Wl,-soname,ER-301SoundComputerProbe.so -Wl,-Bsymbolic \
      "$ROOT/test/image_clone_probe.cpp" -o "$PROBE"
    "$CXX" -std=c++11 "$ROOT/test/image_clone_test.cpp" \
      -ldl -o "$BUILD/image_clone_test"
    ;;
  *)
    echo "Copied-engine isolation verification is supported on macOS and Linux." >&2
    exit 2
    ;;
esac

"$BUILD/image_clone_test" "$PROBE" "$BUILD/clones"

grep -q 'soundcomputer_engine_api_v1' "$ROOT/src/er301_bridge.cpp"
grep -q 'slot-' "$ROOT/src/er301_bridge.cpp"
grep -q 'claimLiveInstanceKey' "$ROOT/src/ER301.cpp"
grep -q 'ER301FrontCardHitZone' "$ROOT/src/ER301.cpp"
grep -q '"Open front"' "$ROOT/src/ER301.cpp"
grep -q 'ModuleWidget.info' "$ROOT/src/ER301.cpp"
grep -q 'mods/core/core_swig.o' "$ROOT/er-301-native/scripts/er301-host.mk"
grep -q '\$(out_dir)/mods/core/core_swig.cpp: SWIGFLAGS += -nomoduleglobal' \
  "$ROOT/er-301-native/scripts/er301-host.mk"
if grep -q '^SWIGFLAGS += -nomoduleglobal' \
  "$ROOT/er-301-native/scripts/er301-host.mk"; then
  echo "The app SWIG module must remain global." >&2
  exit 1
fi
grep -q 'luaL_requiref(L, "core.libcore"' \
  "$ROOT/er-301-native/od/glue/AppInterpreter.cpp"
grep -q 'package.preload' "$ROOT/er-301-native/od/glue/AppInterpreter.cpp"
grep -q 'soundcomputer_prepare_native_package' \
  "$ROOT/er-301-native/libs/lua54/loadlib.c"
grep -q 'patchNativePackageMachO' \
  "$ROOT/er-301-native/emu/NativePackageHost.cpp"
if grep -q 'native ER-301 packages are not yet supported' \
  "$ROOT/er-301-native/libs/lua54/loadlib.c"; then
  echo "Secondary native-package rejection is still present." >&2
  exit 1
fi
grep -q 'fftwf_make_planner_thread_safe' "$ROOT/src/plugin.cpp"
if [[ "$(uname -s)" == "Linux" ]]; then
  grep -q -- '-Wl,-Bsymbolic' "$ROOT/Makefile"
fi

echo "Copied-engine isolation and source-contract verification: PASS"
