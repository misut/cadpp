#!/usr/bin/env sh
# Pull a PNG screenshot via `adb exec-out screencap -p`. Writes to
# $CADPP_ANDROID_STATE_DIR/cadpp-<epoch>.png and opens it
# automatically on macOS.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

cadpp_android_require_tool ADB "adb"

ts="$(date +%s)"
out="$CADPP_ANDROID_STATE_DIR/cadpp-$ts.png"
"$ADB" exec-out screencap -p > "$out"

printf 'screenshot: %s\n' "$out"

if [ "$(_cadpp_android_host)" = "darwin" ] && command -v open >/dev/null 2>&1; then
    open "$out"
fi
