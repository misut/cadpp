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
};

struct Noop {};
using Msg = std::variant<Noop>;

void update(State&, Msg);

void view(State const&);

} // namespace cadpp
