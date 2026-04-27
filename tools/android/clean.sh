#!/usr/bin/env sh
# Wipe Gradle + exon Android outputs and stop any running emulator.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

printf '── stopping emulator (if running)\n'
"$SCRIPT_DIR/emu_stop.sh" || true

printf '\n── gradle clean\n'
if [ -x "$ANDROID_PROJECT_DIR/gradlew" ]; then
    (cd "$ANDROID_PROJECT_DIR" && ./gradlew clean)
fi

printf '\n── exon clean (aarch64-linux-android)\n'
rm -rf "$CADPP_ROOT/.exon/aarch64-linux-android"

# The example's own cxx/build dirs tend to hold stale ninja state after
# an AGP version bump — scrub them too.
rm -rf "$ANDROID_PROJECT_DIR/app/.cxx" "$ANDROID_PROJECT_DIR/app/build"

printf '\nclean done.\n'
