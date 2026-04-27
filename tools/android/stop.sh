#!/usr/bin/env sh
# Force-stop the cad++ process so the next `am start` reloads
# native state from scratch.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

cadpp_android_require_tool ADB "adb"

printf 'am force-stop: %s\n' "$CADPP_ANDROID_PACKAGE"
"$ADB" shell am force-stop "$CADPP_ANDROID_PACKAGE"
