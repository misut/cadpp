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
    out += "Linetypes: " + std::to_string(e.linetype_count) + "\n";
    out += "Tessellated segments: " + std::to_string(e.lines.size()) + "\n";
    out += "Other entities (skipped): " + std::to_string(e.unknown_entities);
    return out;
}

// Autodesk Viewer suppresses lineweight rendering in the Model layout
// (the "2D View" pane), independent of the file's LWDISPLAY header
// variable. Sheets render lineweight through their plot setup. cad++
// matches that convention by collapsing every entity's per-entity
// lineweight to the 1.0 px default when the active layout is Model.
void apply_lineweight_policy(Entities& e, bool is_model) {
    if (!is_model) return;
    for (auto& l : e.lines)            l.thickness = 1.0f;
    for (auto& a : e.arcs)             a.thickness = 1.0f;
    for (auto& bp : e.bulged_polylines) bp.thickness = 1.0f;
    for (auto& el : e.ellipses)        el.thickness = 1.0f;
    for (auto& s : e.splines)          s.thickness  = 1.0f;
}

void State::load(std::string path, std::string layout) {
    source_path = std::move(path);
    selected_layout = std::move(layout);
    entities = parse_file(source_path, selected_layout);
    // Snap `selected_layout` to whatever `parse_file` actually picked
    // so the picker's "active" highlight stays accurate when an empty
    // / stale filter falls through to the first layout in tab order.
    if (selected_layout.empty() && !entities.layouts.empty()) {
        selected_layout = entities.layouts.front().name;
    }
    // Match Autodesk Viewer: Model = lineweight off, Sheets = on.
    bool selected_is_model = false;
    for (auto const& lo : entities.layouts) {
        if (lo.name == selected_layout && lo.is_model) {
            selected_is_model = true;
            break;
        }
    }
    apply_lineweight_policy(entities, selected_is_model);
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
        } else if constexpr (std::is_same_v<T, SelectView>) {
            // Reload the file filtered to the chosen layout. The
            // existing source_path is reused — only the layout filter
            // changes between renders. On Android the drawer
            // auto-closes after a selection so the user immediately
            // sees the new view; on native the drawer flag is unused.
            state.load(state.source_path, m.name);
#ifdef __ANDROID__
            state.drawer_open = false;
#endif
        } else if constexpr (std::is_same_v<T, ToggleDrawer>) {
            state.drawer_open = !state.drawer_open;
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

// Mirrors Autodesk Viewer's left sidebar: paper-space layouts under a
// "Sheets" heading, the implicit Model layout under "Model". One row
// per layout — single-select via button styling (Primary = active,
// Default = inactive). `widget::radio` was tried first but its paint
// cache occasionally failed to repaint the row when transitioning a
// previously-untouched item from inactive → active in one hop;
// buttons sidestep that path while delivering the same UX.
//
// Display-name mapping: AutoCAD's Model layout is stored with
// `layout_name = "Model"` in the DWG, but Autodesk Viewer relabels
// it as "2D View" in the picker. Match that convention so the picker
// reads the same as the reference viewer. `SelectView` still posts
// the on-disk layout name so `parse_file` can match it.
void render_view_panel(State const& state) {
    using namespace phenotype;
    if (state.entities.layouts.empty()) return;
    auto const display_name = [](Layout const& l) -> std::string {
        return l.is_model ? "2D View" : l.name;
    };
    auto const button_for_layout = [&](Layout const& l) {
        bool const active = (l.name == state.selected_layout);
        widget::button<Msg>(
            display_name(l), SelectView{l.name},
            active ? ButtonVariant::Primary : ButtonVariant::Default);
    };
    layout::card([&] {
        widget::text("Views", TextSize::Body);
        bool any_sheet = false;
        for (auto const& l : state.entities.layouts) {
            if (!l.is_model) { any_sheet = true; break; }
        }
        if (any_sheet) {
            widget::text("Sheets", TextSize::Small, TextColor::Muted);
            layout::column([&] {
                for (auto const& l : state.entities.layouts) {
                    if (l.is_model) continue;
                    button_for_layout(l);
                }
            }, SpaceToken::Xs);
        }
        bool any_model = false;
        for (auto const& l : state.entities.layouts) {
            if (l.is_model) { any_model = true; break; }
        }
        if (any_model) {
            widget::text("Model", TextSize::Small, TextColor::Muted);
            layout::column([&] {
                for (auto const& l : state.entities.layouts) {
                    if (!l.is_model) continue;
                    button_for_layout(l);
                }
            }, SpaceToken::Xs);
        }
    });
}

} // namespace

// Canvas paint callback. Inlined inside view() to avoid any subtle
// lifetime issue with extracting it into a helper that returns a
// lambda capturing a reference to the view's State parameter.
namespace {
auto canvas_painter(State const& state) {
    return [&state](phenotype::Painter& p) {
        constexpr float kBorder = 2.0f;
        constexpr phenotype::Color kBorderColor{107, 114, 128, 255};
        float inset = kBorder * 0.5f;
        float w = kCanvasWidth  - inset;
        float h = kCanvasHeight - inset;
        p.line(inset, inset, w,     inset, kBorder, kBorderColor);
        p.line(w,     inset, w,     h,     kBorder, kBorderColor);
        p.line(w,     h,     inset, h,     kBorder, kBorderColor);
        p.line(inset, h,     inset, inset, kBorder, kBorderColor);

        // Hatches render first so subsequent strokes / arcs / text
        // overlay correctly. CAD convention.
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
    };
}
} // namespace

void view(State const& state) {
    using namespace phenotype;
    layout::padded(SpaceToken::Lg, [&] {
        layout::column([&] {
            widget::text("cad++", TextSize::Heading);
            widget::text("File: " + state.source_path,
                         TextSize::Small, TextColor::Muted);
            widget::button<Msg>("Open...", OpenRequested{},
                                ButtonVariant::Primary);
#ifdef __ANDROID__
            // Bottom drawer: when open, replace the canvas with the
            // view + layer pickers + a "Close" button at the top of
            // the sheet. When closed, show the canvas with a "Views"
            // toggle anchored beneath it (pull-up handle ergonomics).
            // Auto-close on selection happens in update() / SelectView.
            if (state.drawer_open) {
                widget::button<Msg>("Close", ToggleDrawer{},
                                    ButtonVariant::Default);
                render_view_panel(state);
                render_layer_panel(state);
            } else {
                widget::canvas(kCanvasWidth, kCanvasHeight,
                               canvas_painter(state),
                               &on_canvas_gesture);
                widget::button<Msg>("Views", ToggleDrawer{},
                                    ButtonVariant::Default);
            }
#else
            widget::text(
                "Slab 9 — view selector + per-layout entity filter",
                TextSize::Small, TextColor::Muted);
            widget::code(format_summary(state.entities));
            // Sidebar (view + layer pickers) on the left, canvas on
            // the right. CrossAxisAlignment::Start keeps the sidebar
            // pinned to the top edge instead of vertical-centring
            // against the taller canvas.
            layout::row([&] {
                layout::column([&] {
                    render_view_panel(state);
                    render_layer_panel(state);
                }, SpaceToken::Md);
                widget::canvas(kCanvasWidth, kCanvasHeight,
                               canvas_painter(state),
                               &on_canvas_gesture);
            }, SpaceToken::Lg, CrossAxisAlignment::Start);
#endif
        });
    });
}

} // namespace cadpp
