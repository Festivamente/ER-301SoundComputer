#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ER301_DIR="${1:-$(cd "$PLUGIN_DIR/er-301-native" && pwd)}"
SOURCE="$ER301_DIR/xroot"
DEST="$PLUGIN_DIR/res/er301/xroot"

if [[ ! -d "$SOURCE" ]]; then
  echo "ER-301 runtime not found: $SOURCE" >&2
  exit 1
fi

MANAGER_OVERRIDE="$DEST/Package/Manager.lua"
TMP_MANAGER="$(mktemp)"
trap 'rm -f "$TMP_MANAGER"' EXIT

if [[ ! -f "$MANAGER_OVERRIDE" ]]; then
  echo "VCV package-manager override not found: $MANAGER_OVERRIDE" >&2
  exit 1
fi
cp "$MANAGER_OVERRIDE" "$TMP_MANAGER"

rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
cp -R "$SOURCE" "$DEST"
cp "$TMP_MANAGER" "$DEST/Package/Manager.lua"

echo "Packaged ER-301 runtime synchronized from: $SOURCE"
echo "Preserved VCV Core-package bootstrap in Package/Manager.lua"
