#!/usr/bin/env bash
# Extract the FULL Skate 3 ISO and push the game data to the device at
# /sdcard/skate3/ (where the Android build looks for default.xex + assets).
#
# Usage: tools/install_game_data.sh /path/to/Skate3.iso
set -euo pipefail

ISO="${1:?usage: install_game_data.sh <skate3.iso>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/out/game-data"

echo ">> Extracting full ISO to $STAGE (this is ~7 GB)..."
mkdir -p "$STAGE"
python3 "$ROOT/tools/extract_xiso.py" "$ISO" "$STAGE"

echo ">> Pushing to device /sdcard/skate3/ (large; uses adb push)..."
adb shell mkdir -p /sdcard/skate3
adb push "$STAGE/." /sdcard/skate3/

echo ">> Verifying default.xex on device..."
adb shell ls -l /sdcard/skate3/default.xex
echo ">> Done."
