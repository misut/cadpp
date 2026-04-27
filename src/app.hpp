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

constexpr float kCanvasWidth  = 800.0f;
constexpr float kCanvasHeight = 500.0f;

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
