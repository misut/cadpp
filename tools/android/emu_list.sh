#!/usr/bin/env sh
# Enumerate configured AVDs and currently-online adb devices.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

if [ ! -x "${EMULATOR:-}" ]; then
    echo "error: emulator binary not found — run \`mise run android:doctor\`" >&2
    exit 1
fi

printf 'avds:\n'
avds="$("$EMULATOR" -list-avds 2>/dev/null || true)"
if [ -n "$avds" ]; then
    printf '%s\n' "$avds" | sed 's/^/  /'
else
    printf '  (none)\n'
fi

if [ -x "${ADB:-}" ]; then
    printf '\nadb devices:\n'
    "$ADB" devices | awk 'NR>1 && NF>=2 {print "  "$0}'
fi
