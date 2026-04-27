// cad++ — DWG viewer (Android + macOS).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// M5 — extends M4 to also extract TEXT / MTEXT entities. Text is
// rendered through phenotype's new Painter::text (#196) at the
// CAD-derived position, with the font size scaled by the same
// world-to-canvas transform that positions the geometry.

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
           + std::to_string(e.polyline_count) + " polyline(s), "
           + std::to_string(e.text_count)     + " text(s)\n";
    out += "Tessellated segments: " + std::to_string(e.lines.size()) + "\n";
    out += "Other entities (skipped): " + std::to_string(e.unknown_entities);
    return out;
}

} // namespace

// `phenotype::native::run_app` default-constructs the user's State
// internally, so the file path can't ride on the State's constructor
// without help from main(). This global is set by main() before
// run_app starts and read by State() during the first construction.
namespace cadpp {
namespace {
std::string g_dwg_path = "test/fixtures/sample_2000.dwg";
}
}  // namespace cadpp

struct State {
    cadpp::Entities entities;
    cadpp::ViewportTransform transform;
    std::string source_path;

    State() {
        source_path = cadpp::g_dwg_path;
        entities = cadpp::parse_file(source_path);
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
            widget::text("M5 — text entities (TEXT + MTEXT)",
                         TextSize::Small, TextColor::Muted);
            widget::text("File: " + state.source_path,
                         TextSize::Small, TextColor::Muted);
            widget::code(format_summary(state.entities));
            widget::canvas(kCanvasWidth, kCanvasHeight,
                           [&state](Painter& p) {
                cadpp::render_lines(p, state.entities, state.transform);
                cadpp::render_texts(p, state.entities, state.transform);
            });
        });
    });
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        cadpp::g_dwg_path = argv[1];
    }
    return phenotype::native::run_app<State, Msg>(900, 800, "cad++", view, update);
}
