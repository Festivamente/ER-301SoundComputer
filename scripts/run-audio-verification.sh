#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ER301_DIR="${ER301_DIR:-$PLUGIN_DIR/er-301-native}"
BUILD_DIR="${AUDIO_TEST_BUILD_DIR:-$PLUGIN_DIR/build/audio-verification}"
CXX_BIN="${CXX:-c++}"
CC_BIN="${CC:-cc}"
RACK_DIR="${RACK_DIR:-$PLUGIN_DIR/../Rack-SDK}"

mkdir -p "$BUILD_DIR"

COMMON=(
  -std=c++11 -O2 -Wall -Wextra -Werror
  -Wno-unused-parameter -Wno-ignored-qualifiers
  -I"$PLUGIN_DIR/src"
  -I"$ER301_DIR"
)

"$CXX_BIN" "${COMMON[@]}" \
  "$PLUGIN_DIR/test/audio_contract_test.cpp" \
  -o "$BUILD_DIR/audio_contract_test"

"$CXX_BIN" "${COMMON[@]}" \
  "$PLUGIN_DIR/test/task_io_test.cpp" \
  "$PLUGIN_DIR/test/audio_test_stubs.cpp" \
  "$ER301_DIR/od/tasks/InputTask.cpp" \
  "$ER301_DIR/od/tasks/OutputTask.cpp" \
  "$ER301_DIR/od/tasks/Task.cpp" \
  "$ER301_DIR/od/objects/Port.cpp" \
  "$ER301_DIR/od/objects/Inlet.cpp" \
  "$ER301_DIR/od/objects/Outlet.cpp" \
  "$ER301_DIR/od/extras/ReferenceCounted.cpp" \
  -o "$BUILD_DIR/task_io_test"

"$CXX_BIN" "${COMMON[@]}" \
  -I"$RACK_DIR/include" -I"$RACK_DIR/dep/include" \
  "$PLUGIN_DIR/test/sample_rate_adapter_test.cpp" \
  -o "$BUILD_DIR/sample_rate_adapter_test"

"$CC_BIN" -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$ER301_DIR" \
  "$PLUGIN_DIR/test/config_rate_test.c" \
  "$ER301_DIR/od/config.c" \
  -lm -o "$BUILD_DIR/config_rate_test"

"$BUILD_DIR/audio_contract_test"
"$BUILD_DIR/task_io_test"
"$BUILD_DIR/sample_rate_adapter_test"
"$BUILD_DIR/config_rate_test"

echo "Audio and sample-rate verification: PASS"
