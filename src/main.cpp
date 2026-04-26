// cad++ — DWG viewer (Android + macOS).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// M2 — opens a hardcoded sample DWG via LibreDWG and shows the
// entity breakdown in the window. M3 onwards adds a file picker and
// actual entity rendering.

#include <string>
#include <variant>
import phenotype;
import phenotype.native;

#include "parser.hpp"

namespace {

std::string format_summary(cadpp::EntityCounts const& c) {
    if (!c.ok) {
        return "Parse failed — " + c.error;
    }
    std::string out;
    out  = "Format: " + c.version + "\n";
    out += "Lines: " + std::to_string(c.lines) + "\n";
    out += "Circles: " + std::to_string(c.circles) + "\n";
    out += "Arcs: " + std::to_string(c.arcs) + "\n";
    out += "Polylines: " + std::to_string(c.polylines) + "\n";
    out += "Text: " + std::to_string(c.text) + "\n";
    out += "Other entities: " + std::to_string(c.other);
    return out;
}

} // namespace

struct State {
    cadpp::EntityCounts counts;

    // phenotype::native::run_app default-constructs State internally,
    // so initialisation has to happen here rather than in main().
    State() {
        counts = cadpp::parse_file("test/fixtures/sample_2000.dwg");
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
            widget::text(
                "DWG viewer for Android (and macOS for debugging). M2 — parsing only.",
                TextSize::Small,
                TextColor::Muted);
            widget::text("Sample: test/fixtures/sample_2000.dwg", TextSize::Small,
                         TextColor::Muted);
            widget::code(format_summary(state.counts));
        });
    });
}

int main() {
    return phenotype::native::run_app<State, Msg>(900, 700, "cad++", view, update);
}
