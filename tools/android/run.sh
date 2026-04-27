#!/usr/bin/env sh
# One-shot: boot emulator -> (re)build + APK -> install -> force-stop ->
# launch -> dump the last ~2s of logcat. Stops short of tailing live;
# use `mise run android:logs` for that.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

step() { printf '\n── %s\n' "$1"; }

step "emu:start"
"$SCRIPT_DIR/emu_start.sh"

# android:apk depends on android:build in mise, but when we invoke the
# scripts directly we need to sequence them ourselves.
step "build"
(cd "$CADPP_ROOT" && mise exec -- exon build --target aarch64-linux-android)

step "apk"
"$SCRIPT_DIR/apk.sh"

step "install"
"$SCRIPT_DIR/install.sh"

step "force-stop (clean slate)"
"$SCRIPT_DIR/stop.sh" || true

step "launch"
"$SCRIPT_DIR/launch.sh"

step "last 2s of logcat"
# `-t` needs a count, not a duration — sample ~120 recent lines which
# roughly corresponds to 1–3 seconds of activity.
"$ADB" logcat -d -t 120 -v brief \
    -s cadpp:V AndroidRuntime:E DEBUG:F ActivityManager:I || true

printf '\nandroid:run done — tail live logs with: mise run android:logs\n'
