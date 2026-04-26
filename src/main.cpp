// cad++ — DWG viewer (Android + macOS).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// M3 — opens a hardcoded sample DWG via LibreDWG, fits its line
// entities into a fixed canvas, and renders them through the new
// widget::canvas / Painter primitive that landed in phenotype #194.

import std;
import phenotype;
import phenotype.native;

#include "geom.hpp"
#include "parser.hpp"
#include "renderer.hpp"

namespace {

constexpr float kCanvasWidth  = 800.0f;
constexpr float kCanvasHeight = 500.0f;

std::string format_summary(cadpp::Entities const& e) {
    if (!e.ok) {
        return "Parse failed — " + e.error;
    }
    std::string out;
    out  = "Format: " + e.version + "\n";
    out += "Lines: " + std::to_string(e.lines.size()) + "\n";
    out += "Other entities (skipped): " + std::to_string(e.unknown_entities);
    return out;
}

} // namespace

struct State {
    cadpp::Entities entities;
    cadpp::ViewportTransform transform;

    // phenotype::native::run_app default-constructs State internally,
    // so initialisation has to happen here rather than in main().
    State() {
        entities = cadpp::parse_file("test/fixtures/sample_2000.dwg");
        if (entities.ok) {
            transform = cadpp::ViewportTransform::fit(
                cadpp::compute_bbox(entities), kCanvasWidth, kCanvasHeight);
        }
    }
};

struct Noop {};
using Msg = std::variant<Noop>;

void update(State&, Msg) {}

void view(State const& state) {
    using namespace phenotype;
    layout::padded(SpaceToken::Lg, [&] {
        layout::column([&] {
            widget::text("cad++", TextSize::Heading);
            widget::text("M3 — line rendering via widget::canvas",
                         TextSize::Small, TextColor::Muted);
            widget::text("Sample: test/fixtures/sample_2000.dwg",
                         TextSize::Small, TextColor::Muted);
            widget::code(format_summary(state.entities));
            widget::canvas(kCanvasWidth, kCanvasHeight,
                           [&state](Painter& p) {
                cadpp::render_lines(p, state.entities, state.transform);
            });
        });
    });
}

int main() {
    return phenotype::native::run_app<State, Msg>(900, 800, "cad++", view, update);
}
