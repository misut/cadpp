// cad++ — DWG viewer (Android + macOS).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// M1 — bootstrap: opens a placeholder window. File picker + DWG render
// arrive in M2 onwards.

#include <string>
#include <variant>
import phenotype;
import phenotype.native;

struct State {};

struct Noop {};
using Msg = std::variant<Noop>;

void update(State&, Msg) {}

void view(State const&) {
    using namespace phenotype;
    layout::padded(SpaceToken::Lg, [&] {
        layout::column([&] {
            widget::text("cad++", TextSize::Heading);
            widget::text(
                "DWG viewer for Android (and macOS for debugging). "
                "M1 placeholder — file picker and entity rendering arrive "
                "in subsequent milestones.",
                TextSize::Small,
                TextColor::Muted);
        });
    });
}

int main() {
    return phenotype::native::run_app<State, Msg>(900, 700, "cad++", view, update);
}
