// cad++ — application surface implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut

#include "app.hpp"

import std;
import phenotype;

namespace cadpp {

std::string g_dwg_path = "test/fixtures/sample_2000.dwg";

std::string format_summary(Entities const& e) {
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

State::State() {
    source_path = g_dwg_path;
    entities = parse_file(source_path);
    if (entities.ok) {
        transform = ViewportTransform::fit(
            compute_bbox(entities), kCanvasWidth, kCanvasHeight);
    }
}

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
                render_lines(p, state.entities, state.transform);
                render_texts(p, state.entities, state.transform);
            });
        });
    });
}

} // namespace cadpp
