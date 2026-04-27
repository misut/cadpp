#!/usr/bin/env sh
# Build the Android example APK via Gradle. Assumes the exon-built
# aarch64-linux-android static libraries already exist (run
# `mise run android:build` first, which this script's mise entry
# depends on).
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

cadpp_android_require ANDROID_HOME     "Android SDK"
cadpp_android_require ANDROID_NDK_HOME "Android NDK"
cadpp_android_require JAVA_HOME        "JDK 17"

# Gradle wants `sdk.dir` in android/local.properties. Regenerate from the
# current ANDROID_HOME so worktree copies stay honest.
lp="$ANDROID_PROJECT_DIR/local.properties"
printf 'sdk.dir=%s\n' "$ANDROID_HOME" > "$lp"

cd "$ANDROID_PROJECT_DIR"

# AGP picks JAVA_HOME from the env; no extra flags needed.
./gradlew assembleDebug "$@"

printf '\napk: %s\n' "$CADPP_ANDROID_APK"
