#!/usr/bin/env sh
# `am start` the cad++ on the attached device.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

cadpp_android_require_tool ADB "adb"

target="$CADPP_ANDROID_PACKAGE/$CADPP_ANDROID_ACTIVITY"
printf 'am start: %s\n' "$target"
"$ADB" shell am start -n "$target"
