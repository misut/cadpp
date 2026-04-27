#!/usr/bin/env sh
# Boot (or reuse) the Android emulator named $CADPP_ANDROID_AVD.
# Runs the emulator backgrounded with stdout/stderr redirected to
# $CADPP_ANDROID_STATE_DIR/emu.log; the PID is persisted for
# emu_stop.sh. If an emulator is already booted we return 0 without
# starting another.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

cadpp_android_require_tool ADB      "adb"
cadpp_android_require_tool EMULATOR "emulator"

if cadpp_android_emu_running; then
    printf 'emulator already running:\n'
    "$ADB" devices | awk 'NR>1 && NF>=2 {print "  "$0}'
    exit 0
fi

if [ -z "${CADPP_ANDROID_AVD:-}" ]; then
    echo "error: CADPP_ANDROID_AVD is empty and no AVD found via \`emulator -list-avds\`" >&2
    echo "       create one via Android Studio or \`avdmanager create avd\`" >&2
    exit 1
fi

pid_file="$CADPP_ANDROID_STATE_DIR/emu.pid"
log_file="$CADPP_ANDROID_STATE_DIR/emu.log"

# Clean a stale pid file from a prior crash.
if [ -f "$pid_file" ]; then
    stale_pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [ -n "$stale_pid" ] && ! kill -0 "$stale_pid" 2>/dev/null; then
        rm -f "$pid_file"
    fi
fi

printf 'booting avd: %s\n' "$CADPP_ANDROID_AVD"
printf '  log : %s\n'      "$log_file"

# `-no-audio` keeps the emulator silent during local runs; remove it if
# you want to hear UI SFX for debugging.
nohup "$EMULATOR" -avd "$CADPP_ANDROID_AVD" -no-audio \
    >"$log_file" 2>&1 &
emu_pid=$!
echo "$emu_pid" > "$pid_file"
printf '  pid : %s\n' "$emu_pid"

# Wait for the adb device to register and for sys.boot_completed to flip.
printf 'waiting for device... '
"$ADB" wait-for-device
printf 'registered.\nwaiting for boot_completed... '

deadline=$(( $(date +%s) + 180 ))
while :; do
    boot="$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)"
    if [ "$boot" = "1" ]; then
        printf 'done.\n'
        break
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        printf 'timeout.\n' >&2
        echo "error: emulator did not finish booting within 180s — tail $log_file" >&2
        exit 1
    fi
    sleep 2
done

# Dismiss the default lock screen; harmless if already unlocked.
"$ADB" shell input keyevent 82 >/dev/null 2>&1 || true

printf 'emulator ready.\n'
