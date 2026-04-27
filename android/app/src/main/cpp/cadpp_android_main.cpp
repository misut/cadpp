// GameActivity driver for cad++. Mirrors phenotype/examples/android with
// two cad++-specific additions:
//   1. Copy the bundled DWG out of APK assets into the app's internal
//      files dir on first launch and tell cad++ to use that path —
//      libredwg 0.13.4's public API only takes a filesystem path, so
//      AAssetManager bytes have to land on disk before parse_file().
//   2. Install `cadpp_android_run` as the runner phenotype's
//      `phenotype_android_start_app` will hand off to. Without this,
//      start_app falls back to phenotype's bundled demo6 sample.

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <android/asset_manager.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
// phenotype shell ABI — see phenotype/src/phenotype.native.android.cppm.
void phenotype_android_bind_jvm(void* jvm);
void phenotype_android_bind_assets(void* asset_manager);
void phenotype_android_attach_surface(void* native_window);
void phenotype_android_detach_surface(void);
void phenotype_android_draw_frame(void);
void phenotype_android_start_app(void);
void phenotype_android_install_runner(void (*runner)(void));
void phenotype_android_dispatch_pointer(float x, float y, int action);
void phenotype_android_dispatch_key(int android_keycode, int action, int mods);
void phenotype_android_dispatch_char(unsigned int codepoint);
void phenotype_android_dispatch_scroll(double dy);
char const* phenotype_android_startup_message(void);

// cad++ entry — defined in libcadpp.a (src/android_entry.cpp).
void cadpp_android_set_dwg_path(char const* path);
void cadpp_android_run(void);
}

namespace {

constexpr char const* TAG = "cadpp";

// Bundled sample shipped under app/src/main/assets/. Matches the macOS
// default cad++::g_dwg_path so behaviour parity is obvious in the logs.
constexpr char const* kBundledAssetName = "sample_2000.dwg";

bool g_surface_ready = false;
bool g_app_started = false;

// Copy `asset_name` out of the APK and into `dest_path` if it isn't
// already present. Returns true on success or when dest_path already
// exists. We don't bother with version checks — the cache lives in the
// app's internal files dir which is wiped on uninstall, and
// `versionCode` bumps on a real release rebuild the APK anyway.
bool stage_asset_to_path(android_app* app, char const* asset_name,
                         char const* dest_path) {
    struct stat st;
    if (stat(dest_path, &st) == 0 && st.st_size > 0) {
        return true;
    }

    if (app == nullptr || app->activity == nullptr
        || app->activity->assetManager == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "asset manager unavailable; cannot stage %s", asset_name);
        return false;
    }

    AAsset* asset = AAssetManager_open(app->activity->assetManager,
                                       asset_name, AASSET_MODE_STREAMING);
    if (asset == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "asset not found in APK: %s", asset_name);
        return false;
    }

    int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "open(%s) failed: %s", dest_path, strerror(errno));
        AAsset_close(asset);
        return false;
    }

    constexpr size_t kBuf = 64 * 1024;
    char buf[kBuf];
    int read_n;
    bool ok = true;
    while ((read_n = AAsset_read(asset, buf, kBuf)) > 0) {
        ssize_t written = 0;
        while (written < read_n) {
            ssize_t w = write(fd, buf + written, read_n - written);
            if (w <= 0) {
                __android_log_print(ANDROID_LOG_ERROR, TAG,
                    "write(%s) failed: %s", dest_path, strerror(errno));
                ok = false;
                break;
            }
            written += w;
        }
        if (!ok) break;
    }
    if (read_n < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "AAsset_read(%s) failed", asset_name);
        ok = false;
    }

    close(fd);
    AAsset_close(asset);

    if (ok) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "staged asset: %s -> %s", asset_name, dest_path);
    } else {
        unlink(dest_path);
    }
    return ok;
}

void publish_dwg_path(android_app* app) {
    char const* internal = app && app->activity
        ? app->activity->internalDataPath : nullptr;
    if (internal == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "internalDataPath unavailable; cad++ will use its compiled-in default");
        return;
    }
    static char dest_path[PATH_MAX];
    int n = snprintf(dest_path, sizeof(dest_path), "%s/%s",
                     internal, kBundledAssetName);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(dest_path)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "internalDataPath too long for sample asset path");
        return;
    }
    if (stage_asset_to_path(app, kBundledAssetName, dest_path)) {
        cadpp_android_set_dwg_path(dest_path);
    }
}

void handle_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window) {
            phenotype_android_attach_surface(app->window);
            if (!g_app_started) {
                phenotype_android_start_app();
                g_app_started = true;
            }
            g_surface_ready = true;
            __android_log_print(ANDROID_LOG_INFO, TAG, "APP_CMD_INIT_WINDOW");
        }
        break;
    case APP_CMD_TERM_WINDOW:
        g_surface_ready = false;
        phenotype_android_detach_surface();
        __android_log_print(ANDROID_LOG_INFO, TAG, "APP_CMD_TERM_WINDOW");
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        // Vulkan backend recreates the swapchain on
        // VK_ERROR_OUT_OF_DATE_KHR; nothing to do here.
        break;
    default:
        break;
    }
}

} // namespace

extern "C" void android_main(android_app* app) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "cad++ starting");

    if (app->activity) {
        phenotype_android_bind_jvm(app->activity->vm);
        phenotype_android_bind_assets(app->activity->assetManager);
    }

    // Order matters: stage the asset and publish its path BEFORE
    // installing the runner, because phenotype invokes the runner from
    // start_app and the runner default-constructs cadpp::State, which
    // reads g_dwg_path immediately.
    publish_dwg_path(app);
    phenotype_android_install_runner(&cadpp_android_run);

    if (auto const* msg = phenotype_android_startup_message()) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "%s", msg);
    }
    app->onAppCmd = handle_cmd;

    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        int timeout = g_surface_ready ? 0 : -1;
        while (ALooper_pollOnce(timeout, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            timeout = 0;
        }

        if (!g_surface_ready) continue;

        if (android_input_buffer* ib = android_app_swap_input_buffers(app)) {
            for (uint64_t i = 0; i < ib->motionEventsCount; ++i) {
                GameActivityMotionEvent const& ev = ib->motionEvents[i];
                int32_t masked = ev.action & AMOTION_EVENT_ACTION_MASK;
                int ptr = (ev.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                        >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
                float x = GameActivityPointerAxes_getX(&ev.pointers[ptr]);
                float y = GameActivityPointerAxes_getY(&ev.pointers[ptr]);
                if (masked == AMOTION_EVENT_ACTION_DOWN
                    || masked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                    phenotype_android_dispatch_pointer(x, y, 0);
                } else if (masked == AMOTION_EVENT_ACTION_MOVE) {
                    phenotype_android_dispatch_pointer(x, y, 1);
                } else if (masked == AMOTION_EVENT_ACTION_UP
                        || masked == AMOTION_EVENT_ACTION_POINTER_UP
                        || masked == AMOTION_EVENT_ACTION_CANCEL) {
                    phenotype_android_dispatch_pointer(x, y, 2);
                } else if (masked == AMOTION_EVENT_ACTION_SCROLL) {
                    float dy = GameActivityPointerAxes_getAxisValue(
                        &ev.pointers[ptr], AMOTION_EVENT_AXIS_VSCROLL);
                    if (dy != 0.0f) {
                        phenotype_android_dispatch_scroll(
                            static_cast<double>(dy));
                    }
                }
            }
            android_app_clear_motion_events(ib);

            for (uint64_t i = 0; i < ib->keyEventsCount; ++i) {
                GameActivityKeyEvent const& ev = ib->keyEvents[i];
                int action;
                if (ev.action == AKEY_EVENT_ACTION_DOWN) {
                    action = (ev.repeatCount > 0) ? 2 : 1;
                } else if (ev.action == AKEY_EVENT_ACTION_UP) {
                    action = 0;
                } else {
                    continue;
                }
                phenotype_android_dispatch_key(ev.keyCode, action, ev.modifiers);
                if (ev.unicodeChar > 0 && action != 0) {
                    phenotype_android_dispatch_char(
                        static_cast<unsigned int>(ev.unicodeChar));
                }
            }
            android_app_clear_key_events(ib);
        }

        phenotype_android_draw_frame();
    }

    phenotype_android_detach_surface();
    __android_log_print(ANDROID_LOG_INFO, TAG, "cad++ exiting");
}
