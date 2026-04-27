// cad++ — Android entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// Compiled into `libcadpp.a` only on the Android target. The NDK glue
// in `android/app/src/main/cpp/cadpp_android_main.cpp` calls
// `cadpp_android_set_dwg_path` once with a filesystem path that points
// at the DWG copied out of APK assets, then installs `cadpp_android_run`
// as the runner phenotype's GameActivity shell will call on first
// `APP_CMD_INIT_WINDOW`. From there phenotype drives view/update like
// it does on the desktop.

#ifdef __ANDROID__

import std;
import phenotype;
import phenotype.native.android;

#include "app.hpp"

extern "C" {

__attribute__((visibility("default")))
void cadpp_android_set_dwg_path(char const* path) {
    if (path != nullptr && path[0] != '\0') {
        cadpp::g_dwg_path = path;
    }
}

__attribute__((visibility("default")))
void cadpp_android_run(void) {
    phenotype::native::android::run_app<cadpp::State, cadpp::Msg>(
        cadpp::view, cadpp::update);
}

} // extern "C"

#endif // __ANDROID__
