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
// 1:1 with physical pixels — a 1080×2400 phone would only fill the
// top sliver with a desktop-default-sized canvas.
//
// Desktop default targets a 1400×1000 GLFW window (set in
// native/src/main.cpp); the canvas is sized so the layer panel +
// summary card on the left and the canvas on the right both fit
// comfortably without horizontal scrolling.
#ifdef __ANDROID__
constexpr float kCanvasWidth  = 1000.0f;
constexpr float kCanvasHeight = 900.0f;
#else
constexpr float kCanvasWidth  = 1200.0f;
constexpr float kCanvasHeight = 800.0f;
#endif

std::string format_summary(Entities const& e);

// `std::unordered_map` link-fails under libc++ + `import std;` (the
// `__hash_memory` undefined-symbol trap), so the layer-visibility
// map is an ordered map. Layer counts are tiny (~50 for real DWGs)
// and the ordering happens to give the panel a stable, predictable
// alphabetical layer list — no sort step needed in the view.
using LayerVisibility = std::map<std::string, bool>;

struct State {
    Entities entities;
    ViewportTransform transform;
    std::string source_path;
    // Selected layout name (empty = first layout in tab order).
    // Drives both `parse_file`'s entity filter and the view-selector
    // panel's "active" highlight.
    std::string selected_layout;
    // Android only: whether the bottom drawer (view + layer picker) is
    // currently expanded. Native ignores this — its sidebar is always
    // visible. Defaults to open so the user sees the available views
    // immediately on first paint.
    bool drawer_open = true;
    // Layer name -> rendered? Initialised from each layer's
    // `frozen` / `off` flag at parse time so the viewer matches the
    // DWG's stored visibility on first paint, but the user can flip
    // any of them through the layer panel without touching the file.
    LayerVisibility layer_visible;

    State();

    // Re-parse `path` (filtered to `layout`, empty = first layout) and
    // refit the viewport. Called from update() on file open or view
    // selection. `layout` defaults to keeping the currently-selected
    // view name when the caller wants to reload the same layout.
    void load(std::string path, std::string layout = {});
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

// Dispatched by the layer panel's per-row checkbox. update() flips
// the named layer's entry in `State::layer_visible`; subsequent
// view() rebuilds skip rendering entities that name a now-hidden
// layer. The DWG file itself is never modified.
struct ToggleLayer {
    std::string name;
};

// Dispatched by the view-selector panel's per-row radio button.
// update() reloads the file filtered to the chosen layout. On
// Android the same dispatch flips `drawer_open = false` so the
// drawer auto-closes after a selection.
struct SelectView {
    std::string name;
};

// Dispatched by the Android drawer's toggle button. update() flips
// `drawer_open`. Native ignores `drawer_open` so this message is a
// no-op there (kept in the variant for source compatibility).
struct ToggleDrawer {};

using Msg = std::variant<Noop, OpenRequested, FileChosen,
                         Pan, Zoom, ToggleLayer,
                         SelectView, ToggleDrawer>;

void update(State&, Msg);

void view(State const&);

} // namespace cadpp
