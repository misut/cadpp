// cad++ — application surface implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut

#include "app.hpp"
#include "fonts.hpp"

import std;
import phenotype;
import phenotype.native;
import phenotype.state;

namespace cadpp {

std::string g_dwg_path = "test/fixtures/sample_2000.dwg";
std::string g_dwg_layout;

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
    t.toggle_box_size *= 2.75f;
    t.radius_sm *= 2.0f;
    t.radius_md *= 2.0f;
    t.radius_lg *= 2.0f;
    phenotype::set_theme(t);
}
#else
void apply_platform_theme() {}
#endif

bool perf_enabled() {
    static bool enabled = [] {
        auto const* v = std::getenv("CADPP_PERF");
        if (v == nullptr || v[0] == '\0') return false;
        return v[0] == '1' || v[0] == 'y' || v[0] == 'Y'
            || v[0] == 't' || v[0] == 'T';
    }();
    return enabled;
}

using PerfClock = std::chrono::steady_clock;

#ifdef __ANDROID__
constexpr float kAndroidSheetPeekHeight = 320.0f;
constexpr float kAndroidSheetTopMargin  = 96.0f;

struct AndroidSheetAnimation {
    bool initialized = false;
    bool target_open = false;
    float start = 0.0f;
    float current = 0.0f;
    std::int64_t start_ms = 0;
};

float bezier_axis(float t, float p1, float p2) noexcept {
    float const u = 1.0f - t;
    return 3.0f * u * u * t * p1
         + 3.0f * u * t * t * p2
         + t * t * t;
}

float bezier_axis_derivative(float t, float p1, float p2) noexcept {
    float const u = 1.0f - t;
    return 3.0f * u * u * p1
         + 6.0f * u * t * (p2 - p1)
         + 3.0f * t * t * (1.0f - p2);
}

float cubic_bezier(float x, float x1, float y1,
                   float x2, float y2) noexcept {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;

    float t = x;
    for (int i = 0; i < 6; ++i) {
        float const estimate = bezier_axis(t, x1, x2) - x;
        float const slope = bezier_axis_derivative(t, x1, x2);
        if (slope == 0.0f) break;
        t -= estimate / slope;
        if (t < 0.0f || t > 1.0f) {
            t = x;
            break;
        }
    }
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 8; ++i) {
        float const estimate = bezier_axis(t, x1, x2);
        if (estimate < x) lo = t;
        else hi = t;
        t = (lo + hi) * 0.5f;
    }
    return bezier_axis(t, y1, y2);
}

float material_enter_sheet_easing(float x) noexcept {
    // Material deceleration curve: LinearOutSlowInInterpolator.
    return cubic_bezier(x, 0.0f, 0.0f, 0.2f, 1.0f);
}

float material_exit_sheet_easing(float x) noexcept {
    // Material acceleration curve: FastOutLinearInInterpolator.
    return cubic_bezier(x, 0.4f, 0.0f, 1.0f, 1.0f);
}

float android_sheet_progress(bool target_open) {
    auto& anim = phenotype::framework_local<AndroidSheetAnimation>();
    auto const now = phenotype::detail::steady_ms();
    float const target = target_open ? 1.0f : 0.0f;

    if (!anim.initialized) {
        anim.initialized = true;
        anim.target_open = target_open;
        anim.start = target;
        anim.current = target;
        anim.start_ms = now;
        return target;
    }

    if (anim.target_open != target_open) {
        anim.target_open = target_open;
        anim.start = anim.current;
        anim.start_ms = now;
    }

    int const duration_ms = target_open ? 225 : 195;
    float linear = static_cast<float>(now - anim.start_ms)
                 / static_cast<float>(duration_ms);
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 1.0f) linear = 1.0f;

    float const eased = target_open
        ? material_enter_sheet_easing(linear)
        : material_exit_sheet_easing(linear);
    anim.current = anim.start + (target - anim.start) * eased;

    float const delta = anim.current > target
        ? anim.current - target
        : target - anim.current;
    if (linear < 1.0f && delta > 0.001f) {
        phenotype::detail::g_app.has_active_animations = true;
    } else {
        anim.current = target;
    }
    return anim.current;
}

float android_viewport_height() noexcept {
    float h = phenotype::detail::g_app.debug_viewport_height;
    return h > 0.0f ? h : 1200.0f;
}

float android_sheet_height(float viewport_h) noexcept {
    float max_h = viewport_h - kAndroidSheetTopMargin;
    if (max_h < kAndroidSheetPeekHeight + 160.0f) {
        max_h = viewport_h;
    }
    float h = viewport_h * 0.72f;
    if (h < 680.0f) h = 680.0f;
    if (h > max_h) h = max_h;
    return h;
}

void android_fixed_block(float height, phenotype::Color background) {
    if (height <= 0.0f) return;
    auto h = phenotype::detail::alloc_node();
    auto& node = phenotype::detail::node_at(h);
    node.style.fixed_height = height;
    node.background = background;
    node.focusable = false;
    phenotype::detail::attach_to_scope(h);
}

template <typename M>
void android_click_block(float height, phenotype::Color background,
                         M message, std::string label) {
    if (height <= 0.0f) return;
    auto h = phenotype::detail::alloc_node();
    auto& node = phenotype::detail::node_at(h);
    auto const id = static_cast<unsigned int>(
        phenotype::detail::g_app.callbacks.size());
    node.style.fixed_height = height;
    node.background = background;
    node.callback_id = id;
    node.cursor_type = 1;
    node.focusable = false;
    node.interaction_role = phenotype::InteractionRole::Button;
    node.debug_semantic_role = "button";
    node.debug_semantic_label = std::move(label);
    node.debug_semantic_callback_id = id;

    phenotype::detail::g_app.callbacks.push_back(
        [msg = Msg{std::move(message)}] {
            phenotype::detail::post<Msg>(msg);
            phenotype::detail::trigger_rebuild();
        });
    phenotype::detail::g_app.callback_roles.push_back(
        phenotype::InteractionRole::Button);
    phenotype::detail::attach_to_scope(h);
}

void android_handle_bar() {
    using namespace phenotype;
    layout::row([&] {
        auto h = phenotype::detail::alloc_node();
        auto& node = phenotype::detail::node_at(h);
        node.style.max_width = 96.0f;
        node.style.fixed_height = 8.0f;
        node.background = phenotype::Color{107, 114, 128, 160};
        node.border_radius = phenotype::detail::g_app.theme.radius_full;
        node.focusable = false;
        phenotype::detail::attach_to_scope(h);
    }, SpaceToken::Xs, CrossAxisAlignment::Center, MainAxisAlignment::Center);
}

template <typename F>
void android_sheet_surface(F&& builder) {
    auto h = phenotype::detail::alloc_node();
    auto& node = phenotype::detail::node_at(h);
    auto const& t = phenotype::detail::g_app.theme;
    node.style.flex_direction = phenotype::FlexDirection::Column;
    node.style.gap = t.space_md;
    node.style.padding[0] = t.space_lg;
    node.style.padding[1] = t.space_lg;
    node.style.padding[2] = t.space_lg;
    node.style.padding[3] = t.space_lg;
    node.background = t.surface;
    node.border_color = t.border;
    node.border_width = 1.0f;
    node.border_radius = t.radius_lg;
    phenotype::detail::open_container(h, std::forward<F>(builder));
}
#endif

double elapsed_ms(PerfClock::time_point a,
                  PerfClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

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
           + std::to_string(e.solid_quad_count) + " SOLID/TRACE(s), "
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
    auto const parse_start = PerfClock::now();
    entities = parse_file(source_path, selected_layout);
    auto const parse_end = PerfClock::now();
    // Try to register each STYLE's font file with phenotype before the
    // first paint — when the file is found, FontSpec lookups for that
    // family resolve to the real AutoCAD face instead of the alias-
    // table substitute. Silently no-ops on misses; the alias table
    // covers the common SHX / Bitstream families either way.
    if (entities.ok) {
        register_style_fonts(entities.styles);
    }
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
        auto const bbox_start = PerfClock::now();
        auto bbox = compute_bbox(entities);
        auto const bbox_end = PerfClock::now();
        transform = ViewportTransform::fit(
            bbox, kCanvasWidth, kCanvasHeight);
        if (perf_enabled()) {
            std::cerr
                << "[cadpp.perf] load layout=\"" << selected_layout
                << "\" solids=" << entities.solid_quads.size()
                << " hatches=" << entities.hatches.size()
                << " lines=" << entities.lines.size()
                << " texts=" << entities.texts.size()
                << " parse_ms=" << elapsed_ms(parse_start, parse_end)
                << " bbox_ms=" << elapsed_ms(bbox_start, bbox_end)
                << "\n";
        }
    } else {
        transform = ViewportTransform{};
        if (perf_enabled()) {
            std::cerr
                << "[cadpp.perf] load failed parse_ms="
                << elapsed_ms(parse_start, parse_end)
                << " error=\"" << entities.error << "\"\n";
        }
    }
}

State::State() {
    apply_platform_theme();
    load(g_dwg_path, g_dwg_layout);
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
            // changes between renders. On Android the sheet
            // auto-closes after a selection so the new view stays
            // visible while the picker slides away.
            state.load(state.source_path, m.name);
#ifdef __ANDROID__
            state.drawer_open = false;
#endif
        } else if constexpr (std::is_same_v<T, SetDrawerOpen>) {
            state.drawer_open = m.open;
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

void render_layer_contents(State const& state, bool show_title) {
    using namespace phenotype;
    if (state.entities.layers.empty()) return;
    if (show_title) {
        widget::text("Layers", TextSize::Body);
    }
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
}

void render_layer_panel(State const& state) {
    using namespace phenotype;
    if (state.entities.layers.empty()) return;
    layout::card([&] {
        render_layer_contents(state, true);
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
void render_view_picker_contents(State const& state, bool show_title) {
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
    if (show_title) {
        widget::text("Views", TextSize::Body);
    }
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
}

void render_view_panel(State const& state) {
    using namespace phenotype;
    if (state.entities.layouts.empty()) return;
    layout::card([&] {
        render_view_picker_contents(state, true);
    });
}

} // namespace

// Canvas paint callback. Inlined inside view() to avoid any subtle
// lifetime issue with extracting it into a helper that returns a
// lambda capturing a reference to the view's State parameter.
namespace {

// Compose phenotype's optional widget::canvas paint_token from every
// State field the canvas painter actually reads. As long as the
// returned uint64 is byte-stable across frames, phenotype reuses the
// previous frame's emitted FillPath / DrawLine command stream and
// skips canvas_painter entirely — collapsing the 36k-cmd HATCH dump
// for colorwh.dwg (and similar large drawings) from a per-frame
// re-emit to a single memcpy. Any input change (entity reload,
// pan/zoom, layer toggle, view switch) advances the token, forcing
// one fresh emit before the cache settles again.
//
// `0` is reserved by phenotype as "no token / always-dirty"; we
// fold a 1 in if the natural hash collides with zero. Hash collision
// risk is 1 in 2^64 per frame, which we accept — the cost of a stale
// blit is at most one frame of stale entities until the next input
// event.
std::uint64_t hash_canvas_inputs(State const& state) noexcept {
    auto mix = [](std::uint64_t h, std::uint64_t v) noexcept {
        h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    };
    auto bits = [](double d) noexcept {
        std::uint64_t u;
        std::memcpy(&u, &d, sizeof(u));
        return u;
    };
    std::uint64_t h = 0xCBF29CE484222325ULL;  // FNV offset basis
    // Entity buffer identity. State::load() rebuilds entities in
    // place — when the underlying std::vector storages relocate, the
    // data() pointers flip; when sizes change, the counts flip. Both
    // feed the hash so any reload advances the token.
    auto vec_print = [&](auto const& v) noexcept {
        h = mix(h, static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(v.data())));
        h = mix(h, static_cast<std::uint64_t>(v.size()));
    };
    vec_print(state.entities.lines);
    vec_print(state.entities.arcs);
    vec_print(state.entities.bulged_polylines);
    vec_print(state.entities.ellipses);
    vec_print(state.entities.splines);
    vec_print(state.entities.fills);
    vec_print(state.entities.solid_quads);
    vec_print(state.entities.hatches);
    vec_print(state.entities.texts);
    h = mix(h, state.entities.ok ? 1ULL : 0ULL);
    // Viewport transform: pan/zoom drives every numeric field.
    auto const& t = state.transform;
    h = mix(h, bits(t.scale));
    h = mix(h, bits(t.pad_x));
    h = mix(h, bits(t.pad_y));
    h = mix(h, bits(t.bbox.min_x));
    h = mix(h, bits(t.bbox.min_y));
    h = mix(h, bits(t.bbox.max_x));
    h = mix(h, bits(t.bbox.max_y));
    // Selected layout name — drives entity filter at parse time.
    for (char c : state.selected_layout)
        h = mix(h, static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
    h = mix(h, 0xFEFEFEFEFEFEFEFEULL);  // sentinel between fields
    // Layer visibility map.
    for (auto const& [name, visible] : state.layer_visible) {
        for (char c : name)
            h = mix(h, static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
        h = mix(h, visible ? 1ULL : 2ULL);
    }
    return h == 0 ? 1ULL : h;
}

auto canvas_painter(State const& state) {
    return [&state](phenotype::Painter& p) {
        auto const paint_start = PerfClock::now();
        constexpr float kBorder = 2.0f;
        constexpr phenotype::Color kBorderColor{107, 114, 128, 255};
        float inset = kBorder * 0.5f;
        float w = kCanvasWidth  - inset;
        float h = kCanvasHeight - inset;
        p.line(inset, inset, w,     inset, kBorder, kBorderColor);
        p.line(w,     inset, w,     h,     kBorder, kBorderColor);
        p.line(w,     h,     inset, h,     kBorder, kBorderColor);
        p.line(inset, h,     inset, inset, kBorder, kBorderColor);

        // Fills render first so subsequent strokes / arcs / text
        // overlay correctly. CAD convention.
        auto const fills_start = PerfClock::now();
        render_fills(p, state.entities, state.transform,
                     state.layer_visible);
        auto const fills_end = PerfClock::now();
        render_lines(p, state.entities, state.transform,
                     state.layer_visible);
        render_arcs(p, state.entities, state.transform,
                    state.layer_visible);
        render_paths(p, state.entities, state.transform,
                     state.layer_visible);
        render_texts(p, state.entities, state.transform,
                     state.layer_visible);
        auto const paint_end = PerfClock::now();
        if (perf_enabled()) {
            std::cerr
                << "[cadpp.perf] paint layout=\"" << state.selected_layout
                << "\" solids=" << state.entities.solid_quads.size()
                << " hatches=" << state.entities.hatches.size()
                << " fill_ms=" << elapsed_ms(fills_start, fills_end)
                << " total_ms=" << elapsed_ms(paint_start, paint_end)
                << " scale=" << state.transform.scale
                << "\n";
        }
    };
}

#ifdef __ANDROID__
void render_android_bottom_sheet(State const& state) {
    using namespace phenotype;

    float const progress = android_sheet_progress(state.drawer_open);
    float const viewport_h = android_viewport_height();
    float const sheet_h = android_sheet_height(viewport_h);
    float const visible_h = kAndroidSheetPeekHeight
        + (sheet_h - kAndroidSheetPeekHeight) * progress;
    float top_h = viewport_h - visible_h;
    if (top_h < 0.0f) top_h = 0.0f;

    unsigned char const scrim_alpha = static_cast<unsigned char>(
        progress <= 0.0f ? 0.0f : progress * 51.0f + 0.5f);
    phenotype::Color const scrim{0, 0, 0, scrim_alpha};
    layout::overlay([&] {
        if (progress > 0.02f || state.drawer_open) {
            android_click_block(top_h, scrim, SetDrawerOpen{false}, "Close views");
        } else {
            android_fixed_block(top_h, phenotype::Color{0, 0, 0, 0});
        }

        android_sheet_surface([&] {
            android_handle_bar();
            if (!state.drawer_open && progress < 0.05f) {
                widget::button<Msg>("Views", SetDrawerOpen{true},
                                    ButtonVariant::Primary);
                return;
            }

            layout::row([&] {
                widget::text("Views", TextSize::Body);
                layout::weighted(1.0f, [] {});
                widget::button<Msg>("Close", SetDrawerOpen{false},
                                    ButtonVariant::Default);
            }, SpaceToken::Md, CrossAxisAlignment::Center,
               MainAxisAlignment::SpaceBetween);

            float body_h = sheet_h - 220.0f;
            if (body_h < 280.0f) body_h = 280.0f;
            layout::scroll_view(body_h, [&] {
                render_view_picker_contents(state, false);
                render_layer_contents(state, true);
            }, SpaceToken::Md);
        });
    });
}
#endif
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
            // Android keeps the drawing visible and presents view/layer
            // controls as a Material-style bottom sheet overlay. The
            // sheet itself is emitted after the main tree so it paints
            // above the canvas and can animate without canvas flash.
            layout::row([&] {
                widget::canvas(kCanvasWidth, kCanvasHeight,
                               canvas_painter(state),
                               &on_canvas_gesture,
                               hash_canvas_inputs(state));
            }, SpaceToken::Xs, CrossAxisAlignment::Center,
               MainAxisAlignment::Center);
            render_android_bottom_sheet(state);
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
                               &on_canvas_gesture,
                               hash_canvas_inputs(state));
            }, SpaceToken::Lg, CrossAxisAlignment::Start);
#endif
        });
    });
}

} // namespace cadpp
