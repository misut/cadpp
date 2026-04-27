#!/usr/bin/env sh
# Summarize the Android toolchain the other tools/android/*.sh scripts
# rely on. Prints each check with a leading [ok] / [miss] tag and
# exits 1 if any critical item is missing.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_env.sh"

rc=0
ok()   { printf '  [ok]   %s\n' "$*"; }
miss() { printf '  [miss] %s\n' "$*" >&2; rc=1; }
warn() { printf '  [warn] %s\n' "$*" >&2; }

printf 'cad++ · android doctor\n'
printf '  host           : %s\n' "$(uname -sm)"

# SDK
if [ -d "${ANDROID_HOME:-}" ]; then
    ok "ANDROID_HOME     = $ANDROID_HOME"
else
    miss "ANDROID_HOME not set or missing ($(_cadpp_android_default_sdk) expected)"
fi

# NDK
if [ -d "${ANDROID_NDK_HOME:-}" ]; then
    ok "ANDROID_NDK_HOME = $ANDROID_NDK_HOME"
else
    miss "ANDROID_NDK_HOME missing (install ndk 30.0.14904198-beta1 via sdkmanager, or unpack the zip under /tmp/ndk-r30/android-ndk-r30-beta1)"
fi

# JDK
if [ -d "${JAVA_HOME:-}" ]; then
    jver="$("$JAVA_HOME/bin/java" -version 2>&1 | head -n1)"
    case "$jver" in
        *\"17*) ok "JAVA_HOME        = $JAVA_HOME ($jver)" ;;
        *)      warn "JAVA_HOME resolves to $JAVA_HOME but $jver — AGP 8.7 requires JDK 17" ;;
    esac
else
    miss "JAVA_HOME missing (install JDK 17; on macOS: \`brew install --cask temurin@17\`)"
fi

# adb / emulator
if [ -x "${ADB:-}" ]; then
    ok "adb              = $ADB"
else
    miss "adb not found under \$ANDROID_HOME/platform-tools or \$PATH"
fi

if [ -x "${EMULATOR:-}" ]; then
    ok "emulator         = $EMULATOR"
else
    miss "emulator not found under \$ANDROID_HOME/emulator (install via sdkmanager)"
fi

# AVDs
if [ -x "${EMULATOR:-}" ]; then
    avds="$("$EMULATOR" -list-avds 2>/dev/null || true)"
    if [ -n "$avds" ]; then
        ok "avds             = $(printf '%s' "$avds" | tr '\n' ' ')"
        if [ -n "${CADPP_ANDROID_AVD:-}" ]; then
            ok "selected avd     = $CADPP_ANDROID_AVD"
        fi
    else
        miss "no AVDs (create one via Android Studio or \`avdmanager create avd\`)"
    fi
fi

# glslc (shader compile path)
if [ -d "${ANDROID_NDK_HOME:-}" ]; then
    host="$(_cadpp_android_host)-x86_64"
    glslc="$ANDROID_NDK_HOME/shader-tools/$host/glslc"
    if [ -x "$glslc" ]; then
        ok "glslc            = $glslc"
    else
        warn "glslc missing under \$ANDROID_NDK_HOME/shader-tools/$host (only needed when editing .vert/.frag)"
    fi
fi

# APK (advisory — run/install tasks build on demand)
if [ -f "$CADPP_ANDROID_APK" ]; then
    ok "apk              = $CADPP_ANDROID_APK"
else
    warn "apk not built yet — \`mise run android:apk\` will produce $CADPP_ANDROID_APK"
fi

# Running device summary
if [ -x "${ADB:-}" ]; then
    devs="$("$ADB" devices 2>/dev/null | awk 'NR>1 && NF>=2 {print $1"("$2")"}' | tr '\n' ' ')"
    [ -n "$devs" ] && ok "adb devices      = $devs"
fi

exit "$rc"
