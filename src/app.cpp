// cad++ — application surface implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut

#include "app.hpp"

import std;
import phenotype;
import phenotype.native;
import phenotype.state;

namespace cadpp {

std::string g_dwg_path = "test/fixtures/sample_2000.dwg";

namespace {

// phenotype renders into the surface's physical pixels with no DPI
// scaling, so a 1080×2400 phone shows the desktop default theme at
// almost-illegible sizes. Scale typography and spacing ~2.5× on
// Android — the same numbers stay 1× on desktop, where the GLFW
// surface lines up with logical points.
#ifdef __ANDROID__
void apply_platform_theme() {
    using phenotype::Theme;
    Theme t;  // start from desktop defaults
    t.body_font_size     *= 2.5f;
    t.heading_font_size  *= 2.5f;
    t.code_font_size     *= 2.5f;
    t.small_font_size    *= 2.5f;
    t.hero_title_size    *= 2.5f;
    t.hero_subtitle_size *= 2.5f;
    t.space_xs  *= 2.0f;
    t.space_sm  *= 2.0f;
    t.space_md  *= 2.0f;
    t.space_lg  *= 2.0f;
    t.space_xl  *= 2.0f;
    t.space_2xl *= 2.0f;
    t.space_3xl *= 2.0f;
    phenotype::set_theme(t);
}
#else
void apply_platform_theme() {}
#endif

} // namespace

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

void State::load(std::string path) {
    source_path = std::move(path);
    entities = parse_file(source_path);
    if (entities.ok) {
        transform = ViewportTransform::fit(
            compute_bbox(entities), kCanvasWidth, kCanvasHeight);
    } else {
        transform = ViewportTransform{};
    }
}

State::State() {
    apply_platform_theme();
    load(g_dwg_path);
}

namespace {

// Trampoline for `phenotype::native::dialog::open_file`. The dialog
// backend hands us a NUL-terminated UTF-8 path (or null on cancel)
// from the application's main event-loop thread, so it is safe to
// post a message back into the same view/update loop.
//
// Phenotype's message queue is type-erased — we just instantiate the
// post<Msg> template here with cad++'s own Msg variant so the runner
// drains it through update(state, msg).
extern "C" void on_picked(char const* path) {
    if (path == nullptr) {
        return;  // user cancelled
    }
    phenotype::detail::post<Msg>(FileChosen{ std::string(path) });
    phenotype::detail::trigger_rebuild();
}

} // namespace

void update(State& state, Msg msg) {
    std::visit([&](auto const& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, OpenRequested>) {
            phenotype::native::dialog::open_file("dwg", &on_picked);
        } else if constexpr (std::is_same_v<T, FileChosen>) {
            state.load(m.path);
        }
    }, msg);
}

void view(State const& state) {
    using namespace phenotype;
    layout::padded(SpaceToken::Lg, [&] {
        layout::column([&] {
            widget::text("cad++", TextSize::Heading);
            widget::text("M6d — file picker via phenotype platform_api",
                         TextSize::Small, TextColor::Muted);
            widget::button<Msg>("Open...", OpenRequested{},
                                ButtonVariant::Primary);
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
