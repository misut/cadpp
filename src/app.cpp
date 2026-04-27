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
           + std::to_string(e.ellipse_count)  + " ellipse(s), "
           + std::to_string(e.spline_count)   + " spline(s), "
           + std::to_string(e.text_count)     + " text(s)\n";
    out += "Composite entities: "
           + std::to_string(e.insert_count)    + " INSERT(s), "
           + std::to_string(e.minsert_count)   + " MINSERT(s), "
           + std::to_string(e.dimension_count) + " DIMENSION(s), "
           + std::to_string(e.hatch_count)     + " HATCH(s)\n";
    out += "Tessellated segments: " + std::to_string(e.lines.size()) + "\n";
    out += "Other entities (skipped): " + std::to_string(e.unknown_entities);
    return out;
}

void State::load(std::string path) {
    source_path = std::move(path);
    entities = parse_file(source_path);
    layer_visible.clear();
    for (auto const& layer : entities.layers) {
        // Match the DWG file's stored visibility on first paint —
        // `frozen` and `off` layers start hidden; the user can flip
        // them on through the layer panel without touching the file.
        layer_visible[layer.name] = !(layer.frozen || layer.off);
    }
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
        } else if constexpr (std::is_same_v<T, Pan>) {
            state.transform.pan(static_cast<double>(m.dx),
                                static_cast<double>(m.dy));
        } else if constexpr (std::is_same_v<T, Zoom>) {
            state.transform.zoom_at(static_cast<double>(m.factor),
                                    static_cast<double>(m.focus_x),
                                    static_cast<double>(m.focus_y));
        } else if constexpr (std::is_same_v<T, ToggleLayer>) {
            auto it = state.layer_visible.find(m.name);
            if (it != state.layer_visible.end()) {
                it->second = !it->second;
            }
        }
    }, msg);
}

namespace {

// Translate a phenotype `GestureEvent` (canvas-local coords already)
// into cad++ Pan / Zoom messages, then post + repaint. Single hop —
// phenotype delivers the event on the render thread, so post<Msg> is
// safe here.
void on_canvas_gesture(phenotype::GestureEvent const& ev) {
    using K = phenotype::GestureKind;
    bool any = false;
    switch (ev.kind) {
    case K::Pan:
        if (ev.dx != 0.0f || ev.dy != 0.0f) {
            phenotype::detail::post<Msg>(Pan{ev.dx, ev.dy});
            any = true;
        }
        break;
    case K::Pinch:
    case K::ScrollZoom:
        // Android folds two-finger midpoint Pan + Pinch into a single
        // GestureEvent (kind = Pinch). Posting both when present + a
        // single trigger_rebuild() halves the view-rebuild rate on
        // multi-pointer scrolls — the bottleneck behind the Galaxy
        // S25 Ultra two-finger lag.
        if (ev.dx != 0.0f || ev.dy != 0.0f) {
            phenotype::detail::post<Msg>(Pan{ev.dx, ev.dy});
            any = true;
        }
        if (ev.pinch_scale != 1.0f) {
            phenotype::detail::post<Msg>(
                Zoom{ev.pinch_scale, ev.focus_x, ev.focus_y});
            any = true;
        }
        break;
    }
    if (any) phenotype::detail::trigger_rebuild();
}

} // namespace

namespace {

void render_layer_panel(State const& state) {
    using namespace phenotype;
    if (state.entities.layers.empty()) return;
    layout::card([&] {
        widget::text("Layers", TextSize::Body);
        layout::column([&] {
            for (auto const& layer : state.entities.layers) {
                auto it = state.layer_visible.find(layer.name);
                bool const visible =
                    (it == state.layer_visible.end()) ? true : it->second;
                widget::checkbox<Msg>(
                    layer.name, visible,
                    ToggleLayer{layer.name});
            }
        }, SpaceToken::Xs);
    });
}

} // namespace

void view(State const& state) {
    using namespace phenotype;
    layout::padded(SpaceToken::Lg, [&] {
        layout::column([&] {
            widget::text("cad++", TextSize::Heading);
            widget::text("Slab 4 — layer model + visibility panel",
                         TextSize::Small, TextColor::Muted);
            widget::button<Msg>("Open...", OpenRequested{},
                                ButtonVariant::Primary);
            widget::text("File: " + state.source_path,
                         TextSize::Small, TextColor::Muted);
            widget::code(format_summary(state.entities));
            // Sidebar (layer panel) on the left, canvas on the right.
            // CrossAxisAlignment::Start keeps the sidebar pinned to
            // the top edge instead of vertical-centring against the
            // taller canvas.
            layout::row([&] {
                layout::column([&] {
                    render_layer_panel(state);
                }, SpaceToken::Md);
                widget::canvas(kCanvasWidth, kCanvasHeight,
                               [&state](Painter& p) {
                    // Frame the drawing region so the user can tell where
                    // the gesture-active surface starts and ends — without
                    // it the canvas blends into the page background.
                    // Stroked from the inside (offset by half-thickness)
                    // so the lines are not clipped by the canvas edge on
                    // backends that snap to pixel rows.
                    constexpr float kBorder = 2.0f;
                    // Qualify with `phenotype::` because Slab 3 added a
                    // cad++-side `Color` struct (parser.hpp) that shadows
                    // phenotype's `Color` inside the `cadpp` namespace.
                    constexpr phenotype::Color kBorderColor{107, 114, 128, 255}; // theme.muted
                    float inset = kBorder * 0.5f;
                    float w = kCanvasWidth  - inset;
                    float h = kCanvasHeight - inset;
                    p.line(inset, inset, w,     inset, kBorder, kBorderColor);
                    p.line(w,     inset, w,     h,     kBorder, kBorderColor);
                    p.line(w,     h,     inset, h,     kBorder, kBorderColor);
                    p.line(inset, h,     inset, inset, kBorder, kBorderColor);

                    // Hatches render first so subsequent strokes /
                    // arcs / text overlay correctly. CAD convention.
                    render_hatches(p, state.entities, state.transform,
                                   state.layer_visible);
                    render_lines(p, state.entities, state.transform,
                                 state.layer_visible);
                    render_arcs(p, state.entities, state.transform,
                                state.layer_visible);
                    render_paths(p, state.entities, state.transform,
                                 state.layer_visible);
                    render_texts(p, state.entities, state.transform,
                                 state.layer_visible);
                },
                               &on_canvas_gesture);
            }, SpaceToken::Lg, CrossAxisAlignment::Start);
        });
    });
}

} // namespace cadpp
