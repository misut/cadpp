// cad++ — application surface (State / Msg / view / update).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// Defined here (rather than in `main.cpp`) so both the desktop binary
// (`native/src/main.cpp`) and the Android entry point
// (`src/android_entry.cpp`) consume the same app definition through the
// `cadpp` library archive instead of recompiling it per platform.

#pragma once

import std;
import phenotype;

#include "geom.hpp"
#include "parser.hpp"
#include "renderer.hpp"

namespace cadpp {

// Set by the platform entry point (desktop main / Android glue) before
// `State` is default-constructed by `phenotype::native::run_app`. The
// constructor reads it on first call and treats it as the source DWG.
extern std::string g_dwg_path;

// Canvas size doubles on Android because phenotype's surface is
// 1:1 with physical pixels — at the desktop's 800×500 a phone screen
// would only fill the top 30% with a hard-to-see drawing.
#ifdef __ANDROID__
constexpr float kCanvasWidth  = 1000.0f;
constexpr float kCanvasHeight = 900.0f;
#else
constexpr float kCanvasWidth  = 800.0f;
constexpr float kCanvasHeight = 500.0f;
#endif

std::string format_summary(Entities const& e);

struct State {
    Entities entities;
    ViewportTransform transform;
    std::string source_path;

    State();

    // Re-parse `path` and refit the viewport. Called from update()
    // when the platform file dialog returns with a new file.
    void load(std::string path);
};

struct Noop {};

// Dispatched from the toolbar "Open..." button. update() responds by
// invoking the platform file dialog; the dialog backend's callback
// (synchronous on macOS, asynchronous on Android) posts FileChosen.
struct OpenRequested {};

// Dispatched by the dialog's C callback once the user confirms a
// selection. `path` is the filesystem path that LibreDWG can open
// directly — backends that pick from non-filesystem sources stage the
// bytes to a cache file before posting this message.
struct FileChosen {
    std::string path;
};

// Translation of phenotype's `GestureKind::Pan` event — slides the
// drawing under the user's finger / trackpad. Values are canvas-local
// pixel deltas already, so update() forwards them straight to
// `ViewportTransform::pan`.
struct Pan {
    float dx = 0.0f;
    float dy = 0.0f;
};

// Translation of phenotype's `GestureKind::Pinch` / `ScrollZoom`
// events. `factor` is the multiplicative zoom (1.0 ≡ no change),
// applied around (`focus_x`, `focus_y`) so the world point under the
// cursor stays under the cursor.
struct Zoom {
    float factor  = 1.0f;
    float focus_x = 0.0f;
    float focus_y = 0.0f;
};

using Msg = std::variant<Noop, OpenRequested, FileChosen, Pan, Zoom>;

void update(State&, Msg);

void view(State const&);

} // namespace cadpp
