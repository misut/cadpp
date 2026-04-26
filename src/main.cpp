// cad++ — DWG viewer (Android + macOS).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// M4 — extends M3 to also extract CIRCLE / ARC / LWPOLYLINE entities
// from the sample DWG. CIRCLE and ARC are tessellated into chord
// segments (~64 per full revolution); LWPOLYLINE just walks adjacent
// vertices. Everything still flows through the M3 widget::canvas /
// Painter::line pipeline — the renderer doesn't need to know about
// curves, only segments.

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
    out += "Source entities: "
           + std::to_string(e.line_count)     + " line(s), "
           + std::to_string(e.circle_count)   + " circle(s), "
           + std::to_string(e.arc_count)      + " arc(s), "
           + std::to_string(e.polyline_count) + " polyline(s)\n";
    out += "Tessellated segments: " + std::to_string(e.lines.size()) + "\n";
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
            widget::text("M4 — circles, arcs, polylines via segment tessellation",
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
