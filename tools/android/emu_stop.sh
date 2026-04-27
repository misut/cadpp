#!/usr/bin/env sh
# Shut down the running emulator. Tries `adb emu kill` first (clean);
# falls back to the PID file emu_start.sh wrote if adb is unresponsive.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

pid_file="$CADPP_ANDROID_STATE_DIR/emu.pid"

if [ -x "${ADB:-}" ] && cadpp_android_emu_running; then
    printf 'sending adb emu kill...\n'
    "$ADB" emu kill >/dev/null 2>&1 || true
fi

# Give the emulator a moment to exit cleanly.
for _ in 1 2 3 4 5; do
    cadpp_android_emu_running || break
    sleep 1
done

if [ -f "$pid_file" ]; then
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        printf 'killing emulator pid %s\n' "$pid"
        kill "$pid" 2>/dev/null || true
        sleep 1
        kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
fi

if cadpp_android_emu_running; then
    echo "warning: emulator still shows as online to adb — run \`$ADB devices\` to inspect" >&2
    exit 1
fi

printf 'emulator stopped.\n'
