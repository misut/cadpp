# tools/android

Shell helpers the `mise run android:*` tasks dispatch to. Every script
sources `_env.sh` so `ANDROID_HOME`, `ANDROID_NDK_HOME`, `JAVA_HOME`,
`ADB`, `EMULATOR`, and `PHENOTYPE_ANDROID_*` resolve consistently.

## Tasks

| mise task | script | notes |
|---|---|---|
| `android:doctor` | `doctor.sh` | Prints a tagged status report; exits 1 on any critical miss |
| `android:shaders` | `../compile_android_shaders.sh` | Recompiles GLSL → SPIR-V `.inl` |
| `android:build` | (runs `exon` directly) | `exon build --target aarch64-linux-android` |
| `android:apk` | `apk.sh` | Writes `local.properties`, runs `./gradlew assembleDebug`; depends on `android:build` |
| `android:emu:list` | `emu_list.sh` | `emulator -list-avds` + `adb devices` |
| `android:emu:start` | `emu_start.sh` | Boots `$PHENOTYPE_ANDROID_AVD` in the background, blocks on `sys.boot_completed` |
| `android:emu:stop` | `emu_stop.sh` | `adb emu kill` with SIGTERM/SIGKILL fallback |
| `android:install` | `install.sh` | `adb install -r $PHENOTYPE_ANDROID_APK`; depends on `android:apk` |
| `android:launch` | `launch.sh` | `am start` the configured activity |
| `android:stop` | `stop.sh` | `am force-stop` for a clean-slate relaunch |
| `android:logs` | `logs.sh` | `adb logcat` filtered to phenotype + crash categories |
| `android:screencap` | `screencap.sh` | PNG pulled to `$PHENOTYPE_ANDROID_STATE_DIR`, auto-opens on macOS |
| `android:run` | `run.sh` | End-to-end: emu:start → build → apk → install → stop → launch → log snapshot |
| `android:clean` | `clean.sh` | Gradle clean + wipe `.exon/aarch64-linux-android` + stop emulator |
| `android` | — | Alias for `android:run` |

## Environment

See the table in [`examples/android/README.md`](../../examples/android/README.md#run)
for the full list. Script-internal state goes under
`$PHENOTYPE_ANDROID_STATE_DIR` (default `/tmp/phenotype-android`):

- `emu.pid` / `emu.log` — backgrounded emulator process
- `phenotype-<epoch>.png` — screenshots from `android:screencap`

Everything is scoped to `/tmp`, so nothing needs to be in `.gitignore`.

## Running scripts directly

The scripts are self-contained and can be invoked outside of mise — e.g.
from an IDE run-config — as long as `ANDROID_HOME`, `ANDROID_NDK_HOME`,
and `JAVA_HOME` either have sensible defaults or are exported:

```sh
./tools/android/doctor.sh
./tools/android/emu_start.sh
./tools/android/run.sh
```
