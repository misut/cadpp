// cad++ — entity → phenotype draw command emitter.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `cmath` declarations come in through `parser.hpp`'s `import std;`
// chain. Including `<cmath>` directly here would re-declare the
// libc++ promote-traits aliases that `import std;` already exports
// (the abi-tag check fires on `__promote_t` — the well-known libc++
// `import std` + `#include <cmath>` collision).

#include "renderer.hpp"
#include "fonts.hpp"

namespace cadpp {

namespace {

constexpr double kHalfPi = 1.57079632679489661923;
constexpr double kTwoPi  = 6.28318530717958647692;

inline phenotype::Color to_paint(Color const& c) {
    return phenotype::Color{c.r, c.g, c.b, c.a};
}

// Per-character advance estimate in `em` units, calibrated against
// Arial Regular at size 1.0. Used by `text_advance_em()` to feed the
// alignment offset in `render_texts` — close enough for centred /
// right-anchored CAD labels to land within ±1 px of the actual
// rasterised position. A future pass should query phenotype's
// `measure_text` host hook directly for pixel-perfect alignment.
inline float char_em_width(char c) {
    static constexpr float W[128] = {
        0.00f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        // 0x20-0x2F: space ! " # $ % & ' ( ) * + , - . /
        0.28f, 0.28f, 0.36f, 0.55f, 0.55f, 0.88f, 0.67f, 0.19f,
        0.33f, 0.33f, 0.39f, 0.58f, 0.28f, 0.33f, 0.28f, 0.28f,
        // 0x30-0x3F: 0-9 : ; < = > ?
        0.55f, 0.55f, 0.55f, 0.55f, 0.55f, 0.55f, 0.55f, 0.55f,
        0.55f, 0.55f, 0.28f, 0.28f, 0.58f, 0.58f, 0.58f, 0.55f,
        // 0x40-0x4F: @ A B C D E F G H I J K L M N O
        1.01f, 0.67f, 0.67f, 0.72f, 0.72f, 0.67f, 0.61f, 0.78f,
        0.72f, 0.28f, 0.50f, 0.67f, 0.55f, 0.83f, 0.72f, 0.78f,
        // 0x50-0x5F: P Q R S T U V W X Y Z [ \ ] ^ _
        0.67f, 0.78f, 0.72f, 0.67f, 0.61f, 0.72f, 0.67f, 0.94f,
        0.67f, 0.67f, 0.61f, 0.28f, 0.28f, 0.28f, 0.47f, 0.55f,
        // 0x60-0x6F: ` a b c d e f g h i j k l m n o
        0.33f, 0.55f, 0.55f, 0.50f, 0.55f, 0.55f, 0.28f, 0.55f,
        0.55f, 0.22f, 0.22f, 0.50f, 0.22f, 0.83f, 0.55f, 0.55f,
        // 0x70-0x7F: p q r s t u v w x y z { | } ~ DEL
        0.55f, 0.55f, 0.33f, 0.50f, 0.28f, 0.55f, 0.50f, 0.72f,
        0.50f, 0.50f, 0.50f, 0.33f, 0.26f, 0.33f, 0.58f, 0.0f,
    };
    auto idx = static_cast<unsigned char>(c);
    if (idx >= 128) return 0.55f;  // generic fallback for non-ASCII
    return W[idx];
}

inline float text_advance_em(char const* str, std::size_t len) {
    float total = 0.0f;
    for (std::size_t i = 0; i < len; ++i) total += char_em_width(str[i]);
    return total;
}

// Slab 4 — layer visibility filter. Entities without a resolved layer
// are always rendered; entities whose layer doesn't appear in the map
// are visible by default (the panel only inserts the layers it knows
// about). Entities whose layer is in the map and false get skipped.
inline bool is_visible(LayerVisibility const& v,
                       std::string const& layer_name) {
    if (layer_name.empty()) return true;
    auto it = v.find(layer_name);
    return it == v.end() ? true : it->second;
}

// Slab 9 — emit `Painter::push_clip` / `pop_clip` for every clip
// marker whose `*_idx` field equals `at`. `field` selects which
// per-vector index to compare against — each render_* loop hands in
// the field that tracks its own entity vector. The push side projects
// the marker's paper-space rect through the active `ViewportTransform`
// (Y-flip + scale) and emits the canvas-pixel bbox so phenotype's
// scissor lands on the right region. Cursor is local to the caller so
// the same `clip_markers` stream is walked once per render_* call,
// keeping the Painter's clip stack balanced (push and pop pairs are
// emitted by the same render function — viewports never straddle
// entity-type boundaries because each viewport contributes to all
// types in one expand_viewport call).
inline void process_clip_markers(
        phenotype::Painter& p,
        std::vector<ClipMarker> const& markers,
        std::size_t& cursor,
        std::size_t ClipMarker::* field,
        std::size_t at,
        ViewportTransform const& transform) {
    while (cursor < markers.size() && (markers[cursor].*field) == at) {
        auto const& m = markers[cursor];
        if (m.kind == ClipMarker::Kind::Push) {
            // The active ViewportTransform y-flips world Y → canvas Y,
            // so the rect's CAD top-left (smaller y) and bottom-right
            // (larger y) swap on the canvas. Take an axis-aligned
            // bbox of both projected corners so the canvas-space rect
            // stays oriented correctly.
            auto const tl = transform.apply(m.x, m.y);
            auto const br = transform.apply(m.x + m.w, m.y + m.h);
            float const cx = static_cast<float>(std::min(tl.x, br.x));
            float const cy = static_cast<float>(std::min(tl.y, br.y));
            float const cw = static_cast<float>(std::abs(br.x - tl.x));
            float const ch = static_cast<float>(std::abs(br.y - tl.y));
            p.push_clip(cx, cy, cw, ch);
        } else {
            p.pop_clip();
        }
        ++cursor;
    }
}

} // namespace

void render_lines(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform,
                  LayerVisibility const& visibility) {
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < entities.lines.size(); ++i) {
        process_clip_markers(p, entities.clip_markers, cursor,
                             &ClipMarker::lines_idx, i, transform);
        auto const& l = entities.lines[i];
        if (!is_visible(visibility, l.layer_name)) continue;
        auto const a = transform.apply(l.a.x, l.a.y);
        auto const b = transform.apply(l.b.x, l.b.y);
        p.line(static_cast<float>(a.x), static_cast<float>(a.y),
               static_cast<float>(b.x), static_cast<float>(b.y),
               l.thickness, to_paint(l.color));
    }
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::lines_idx,
                         entities.lines.size(), transform);
}

// Resolve a per-run FontSpec, combining the entity's outer Style with
// the run's optional overrides. Family always passes through the alias
// table so SHX / Bitstream names resolve to host fonts.
inline phenotype::FontSpec resolve_run_spec(Style const& outer,
                                            TextRun const& r) {
    std::string_view const fam_raw = r.family_override.empty()
        ? std::string_view{outer.font_family}
        : std::string_view{r.family_override};
    std::string_view const aliased = alias_font_family(fam_raw);
    std::string_view const family = aliased.empty() ? fam_raw : aliased;
    bool const bold   = outer.bold   || r.bold_override;
    bool const italic = outer.italic || r.italic_override;
    return phenotype::FontSpec{
        family,
        bold   ? phenotype::FontWeight::Bold   : phenotype::FontWeight::Regular,
        italic ? phenotype::FontStyle::Italic  : phenotype::FontStyle::Upright,
        false,
    };
}

void render_texts(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform,
                  LayerVisibility const& visibility) {
    std::size_t cursor = 0;
    for (std::size_t ti = 0; ti < entities.texts.size(); ++ti) {
        process_clip_markers(p, entities.clip_markers, cursor,
                             &ClipMarker::texts_idx, ti, transform);
        auto const& t = entities.texts[ti];
        if (t.content.empty()) continue;
        if (!is_visible(visibility, t.layer_name)) continue;
        float const outer_font_px =
            static_cast<float>(t.height * transform.scale);
        // Drop only truly sub-pixel runs — modern text backends
        // (CoreText, DirectWrite, Skia) rasterise legibly down to
        // ~1.5 px with built-in antialiasing, so the previous 4-px
        // cut was hiding plenty of legible body copy on tightly-fit
        // drawings (truetype.dwg, dimension labels at zoom-out).
        if (outer_font_px < 1.5f) continue;

        // Two-pass walk over the entity's content. Pass 1 measures
        // per-line widths + max heights into `line_widths` /
        // `line_heights`; pass 2 emits draw calls using those numbers
        // for h/v anchoring. Built around plain `vector<float>` /
        // `vector<size_t>` only — vectors of locally-defined structs
        // collide with libc++'s aligned-new overload set under
        // `import std;` (the same trap the file-header comment warns
        // about for `<cmath>`).
        std::vector<float> line_widths;
        std::vector<float> line_heights;
        std::vector<float> seg_measured;  // one entry per visible segment

        auto walk = [&](auto on_segment) {
            std::size_t li = 0;
            auto bump_line = [&]() {
                ++li;
                if (li >= line_widths.size()) {
                    line_widths.resize(li + 1, 0.0f);
                    line_heights.resize(li + 1, 0.0f);
                }
            };
            auto emit_run_text = [&](std::string_view text, float font_px,
                                     phenotype::FontSpec const& spec,
                                     phenotype::Color paint) {
                std::string_view rest = text;
                while (!rest.empty()) {
                    // Find the next break (newline or tab) — splitting
                    // at both lets the renderer (a) bump line index on
                    // \n, and (b) emit a tab marker the second pass
                    // can use to advance the line cursor to the next
                    // tab stop without consuming any visible glyphs.
                    auto const nl  = rest.find('\n');
                    auto const tab = rest.find('\t');
                    auto const next = std::min(nl, tab);
                    std::string_view const piece =
                        (next == std::string_view::npos) ? rest : rest.substr(0, next);
                    if (!piece.empty()) {
                        on_segment(li, piece, font_px, spec, paint);
                    }
                    if (next == std::string_view::npos) break;
                    char const brk = rest[next];
                    if (brk == '\n') {
                        bump_line();
                    } else { // '\t'
                        // Emit a sentinel tab segment with empty text;
                        // pass 1 records 0 measured + bumps tab counter,
                        // pass 2 advances cursor to next tab stop.
                        on_segment(li, std::string_view{},
                                   font_px, spec, paint);
                    }
                    rest = rest.substr(next + 1);
                }
            };
            // Seed line 0 in the metrics arrays so on_segment can index
            // into them on the very first segment.
            if (line_widths.empty()) {
                line_widths.push_back(0.0f);
                line_heights.push_back(0.0f);
            }
            if (t.runs.empty()) {
                std::string_view const alias =
                    alias_font_family(t.style.font_family);
                std::string_view const family =
                    alias.empty() ? std::string_view{t.style.font_family} : alias;
                phenotype::FontSpec const spec{
                    family,
                    t.style.bold   ? phenotype::FontWeight::Bold   : phenotype::FontWeight::Regular,
                    t.style.italic ? phenotype::FontStyle::Italic  : phenotype::FontStyle::Upright,
                    false,
                };
                emit_run_text(t.content, outer_font_px, spec, to_paint(t.color));
            } else {
                for (auto const& r : t.runs) {
                    float const run_font_px = static_cast<float>(
                        t.height * r.height_scale * transform.scale);
                    if (run_font_px < 1.5f) {
                        // Still advance line index past any newlines so
                        // a sub-pixel run does not collapse subsequent
                        // visible runs onto the wrong line.
                        for (char c : r.text) if (c == '\n') bump_line();
                        continue;
                    }
                    phenotype::FontSpec const spec = resolve_run_spec(t.style, r);
                    Color const color = (r.color_override.a != 0)
                        ? r.color_override : t.color;
                    phenotype::Color const paint = to_paint(color);
                    emit_run_text(r.text, run_font_px, spec, paint);
                }
            }
        };

        // MTEXT tab advance — jump to the RIGHTMOST defined paragraph
        // tab stop (`\pl<l>,t<x>;` / `\pt<x>;`) that is still strictly
        // greater than the current line cursor. Matches Autodesk
        // Viewer's behaviour on truetype.dwg: where multiple stops are
        // defined per paragraph, the *last* one is the intended sample
        // column and the earlier ones are alternates that AutoCAD
        // skips through to land at the canonical alignment. Strict
        // "first stop > current" would split the sample column across
        // sections (Outline → 7.188, Monospace → 6.000, Regular →
        // 8.385) and break the visual alignment Autodesk Viewer keeps.
        //
        // When the cursor is past the last defined stop (e.g.
        // ISOCTEUR's `\t\t` after a single `t7.2;` directive), extend
        // the column grid with a default interval — average spacing
        // between defined stops if there are ≥2, otherwise a fixed
        // 1.2 world-unit fallback (matches the typical column gap in
        // truetype.dwg's right column). Tab then degrades to a no-op
        // only when the cursor already overshoots the extended grid
        // by a full interval, never moving glyphs leftward.
        auto tab_advance = [&](float current_offset) -> float {
            if (t.tab_stops.empty()) return 0.0f;
            float chosen = -1.0f;
            for (double s : t.tab_stops) {
                float const stop = static_cast<float>(s) * transform.scale;
                if (stop > current_offset) chosen = stop;
            }
            if (chosen >= 0.0f) return chosen - current_offset;
            // Past all defined stops — extend by default interval.
            float const last =
                static_cast<float>(t.tab_stops.back()) * transform.scale;
            float interval = 1.2f * transform.scale;
            if (t.tab_stops.size() >= 2) {
                interval = static_cast<float>(
                    t.tab_stops.back() - t.tab_stops.front())
                    * transform.scale
                    / static_cast<float>(t.tab_stops.size() - 1);
            }
            float const past_last = current_offset - last;
            float const steps = std::floor(past_last / interval) + 1.0f;
            return last + steps * interval - current_offset;
        };

        // Pass 1: measure + accumulate per-line metrics. Empty `piece`
        // = TAB sentinel emitted by the splitter — advance to the
        // next defined tab stop (or default-extended grid past the
        // last stop) instead of measuring glyphs.
        walk([&](std::size_t li, std::string_view piece, float font_px,
                 phenotype::FontSpec const& spec, phenotype::Color) {
            float measured;
            if (piece.empty()) {
                measured = tab_advance(line_widths[li]);
            } else {
                measured = p.measure_text(
                    piece.data(), static_cast<unsigned int>(piece.size()),
                    font_px, spec);
                if (!(measured > 0.0f && std::isfinite(measured))) {
                    measured = font_px * text_advance_em(piece.data(), piece.size());
                }
            }
            seg_measured.push_back(measured);
            line_widths[li]  += measured;
            if (font_px > line_heights[li]) line_heights[li] = font_px;
        });

        if (line_widths.empty() || seg_measured.empty()) continue;
        for (auto& h : line_heights) {
            if (h == 0.0f) h = outer_font_px;
        }
        // Per-line vertical advance. Calibrated against truetype.dwg:
        // entity[1] reports `extents_height = 21.736` with 57 `\n` in
        // its flat content (58 rows in our `\n`-counting), so per-row
        // advance = 21.736 / 58 ≈ 0.375 — exactly `text_height × 1.25
        // × linespace_factor` for `text_height = 0.3` and
        // `linespace_factor = 1.0`. The 1.25 multiplier lines up the
        // body MTEXT rows with the separate LINE separator entities
        // that the same DWG draws across each font row. Plain TEXT
        // (runs empty, single line) reduces to font_px since the loop
        // below max()es against the per-line height.
        float const default_advance =
            outer_font_px * 1.25f * static_cast<float>(t.line_spacing);
        std::vector<float> line_advances(line_heights.size(), 0.0f);
        for (std::size_t li = 0; li < line_heights.size(); ++li) {
            // Most rows use the entity-wide advance. If a single run on
            // the line is taller than the default leading would cover
            // (rare — `\H`-scaled-up runs), fall back to the line's
            // own max so glyphs don't overlap into the next row.
            line_advances[li] = std::max(default_advance, line_heights[li]);
        }
        float total_height = 0.0f;
        for (float a : line_advances) total_height += a;

        auto const anchor_canvas =
            transform.apply(t.position.x, t.position.y);
        float const anchor_x = static_cast<float>(anchor_canvas.x);
        float const anchor_y = static_cast<float>(anchor_canvas.y);

        // V-anchor: pick the top-y of the entire text block. CAD's
        // Baseline / Bottom anchor sits below the text (matching the
        // single-line path's `ay -= font_px`); Middle puts the
        // visual centre of the block at the anchor; Top puts the top
        // edge there.
        //
        // `cap_offset` accounts for the gap between AutoCAD's bounding-
        // rect top (≈ cap-top of row 0) and phenotype's font_size box
        // top — phenotype.text(x, y) puts the FONT BOX top at y, but
        // the visible glyph cap-top is below that by the font's
        // internal leading. Calibrated against truetype.dwg: row
        // baselines line up with separator-LINE y values when the
        // body is shifted down by half the outer text height.
        float const cap_offset = outer_font_px * 0.5f;
        float top_y = anchor_y;
        switch (t.v_align) {
        case TextVAlign::Baseline:
        case TextVAlign::Bottom:  top_y = anchor_y - total_height;        break;
        case TextVAlign::Middle:  top_y = anchor_y - total_height * 0.5f; break;
        case TextVAlign::Top:     top_y = anchor_y + cap_offset;          break;
        }

        // Pre-compute per-line top y and per-line starting x (h-anchor).
        std::vector<float> line_top_ys(line_widths.size(), 0.0f);
        std::vector<float> line_start_xs(line_widths.size(), 0.0f);
        {
            float cumulative = top_y;
            for (std::size_t li = 0; li < line_widths.size(); ++li) {
                line_top_ys[li] = cumulative;
                cumulative += line_advances[li];
                float lx = anchor_x;
                switch (t.h_align) {
                case TextHAlign::Left:                                  break;
                case TextHAlign::Center: lx -= line_widths[li] * 0.5f;  break;
                case TextHAlign::Middle: lx -= line_widths[li] * 0.5f;  break;
                case TextHAlign::Right:  lx -= line_widths[li];         break;
                }
                line_start_xs[li] = lx;
            }
        }

        // Pass 2: emit draws. seg_measured indexed in the same visit
        // order as pass 1 so each on_segment call consumes its own
        // pre-computed measured width. Empty `piece` = TAB sentinel —
        // skip the draw call but still advance the cursor.
        std::vector<float> line_x_cursor = line_start_xs;
        std::size_t mi = 0;
        walk([&](std::size_t li, std::string_view piece, float font_px,
                 phenotype::FontSpec const& spec, phenotype::Color color) {
            float const measured = seg_measured[mi++];
            if (!piece.empty()) {
                // Bottom-align segments within the line so a tall and
                // a short run on the same line share a baseline-ish y.
                float const seg_y = line_top_ys[li] + (line_heights[li] - font_px);
                p.text(line_x_cursor[li], seg_y,
                       piece.data(), static_cast<unsigned int>(piece.size()),
                       font_px, color, spec);
            }
            line_x_cursor[li] += measured;
        });
    }
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::texts_idx,
                         entities.texts.size(), transform);
}

void render_arcs(phenotype::Painter& p,
                 Entities const& entities,
                 ViewportTransform const& transform,
                 LayerVisibility const& visibility) {
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < entities.arcs.size(); ++i) {
        process_clip_markers(p, entities.clip_markers, cursor,
                             &ClipMarker::arcs_idx, i, transform);
        auto const& a = entities.arcs[i];
        if (!is_visible(visibility, a.layer_name)) continue;
        auto const center_canvas = transform.apply(a.center.x, a.center.y);
        float const r_px = static_cast<float>(a.radius * transform.scale);
        if (r_px < 0.5f) continue;  // sub-pixel — skip
        // CAD's angle convention is y-up CCW; phenotype's `Painter::arc`
        // angle convention follows the canvas's y-down coordinate
        // system (CCW around the canvas frame). Mirroring the angle
        // across y reverses CCW into CW; swapping `start` and `end`
        // restores the original sweep direction in the new frame.
        float const canvas_start = static_cast<float>(-a.end_angle);
        float const canvas_end   = static_cast<float>(-a.start_angle);
        p.arc(static_cast<float>(center_canvas.x),
              static_cast<float>(center_canvas.y),
              r_px,
              canvas_start, canvas_end,
              a.thickness, to_paint(a.color));
    }
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::arcs_idx,
                         entities.arcs.size(), transform);
}

namespace {

// AutoCAD bulge → world-frame circular-arc parameters.
//
// `bulge = tan(θ / 4)`, where θ is the included CCW arc angle from
// the start vertex to the end vertex (in CAD's y-up frame). Positive
// bulge sweeps CCW; negative sweeps CW. With the chord vector
// `d = end - start` and chord length `L`, the world-frame arc has:
//   radius = L · (1 + b²) / (4 · |b|)
//   centre = midpoint(start, end) + perp(d) · k
//   k      = (1 - b²) / (4 · b)            (signed; perp is rotated CCW)
struct BulgeArc {
    double cx, cy;       // world-frame centre
    double radius;       // world-frame radius
    double start_angle;  // world-frame CCW (positive bulge) or CW (negative)
    double end_angle;
    bool   ccw;          // true if positive bulge → CCW sweep
};

inline BulgeArc bulge_to_arc(Point const& a, Point const& b, double bulge) {
    BulgeArc out{};
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double abs_b = std::abs(bulge);
    double mid_x = 0.5 * (a.x + b.x);
    double mid_y = 0.5 * (a.y + b.y);
    double k     = (1.0 - bulge * bulge) / (4.0 * bulge);
    out.cx = mid_x - dy * k;
    out.cy = mid_y + dx * k;
    double chord = std::sqrt(dx * dx + dy * dy);
    out.radius = chord * (1.0 + bulge * bulge) / (4.0 * abs_b);
    out.start_angle = std::atan2(a.y - out.cy, a.x - out.cx);
    out.end_angle   = std::atan2(b.y - out.cy, b.x - out.cx);
    out.ccw = bulge > 0.0;
    return out;
}

} // namespace

void render_paths(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform,
                  LayerVisibility const& visibility) {
    // ---- SPLINE → MoveTo + LineTo polyline ----
    //
    // The parser pre-samples splines into a polyline (De Boor at
    // uniform parameter steps for NURBS, fit-point passthrough for
    // Bezier-scenario splines), so this path is just a polyline
    // emit. Closed splines get a final `Close` verb.
    std::size_t spline_cursor = 0;
    for (std::size_t spi = 0; spi < entities.splines.size(); ++spi) {
        process_clip_markers(p, entities.clip_markers, spline_cursor,
                             &ClipMarker::splines_idx, spi, transform);
        auto const& sp = entities.splines[spi];
        if (sp.points.size() < 2) continue;
        if (!is_visible(visibility, sp.layer_name)) continue;
        phenotype::PathBuilder pb;
        auto const start = transform.apply(
            sp.points[0].x, sp.points[0].y);
        pb.move_to(static_cast<float>(start.x),
                   static_cast<float>(start.y));
        for (std::size_t i = 1; i < sp.points.size(); ++i) {
            auto const c = transform.apply(
                sp.points[i].x, sp.points[i].y);
            pb.line_to(static_cast<float>(c.x),
                       static_cast<float>(c.y));
        }
        if (sp.closed) pb.close();
        p.stroke_path(pb, sp.thickness, to_paint(sp.color));
    }
    process_clip_markers(p, entities.clip_markers, spline_cursor,
                         &ClipMarker::splines_idx,
                         entities.splines.size(), transform);

    // ---- Bulged LWPOLYLINE → MoveTo + (LineTo | ArcTo) chain ----
    std::size_t bulged_cursor = 0;
    for (std::size_t bi = 0; bi < entities.bulged_polylines.size(); ++bi) {
        process_clip_markers(p, entities.clip_markers, bulged_cursor,
                             &ClipMarker::bulged_idx, bi, transform);
        auto const& bp = entities.bulged_polylines[bi];
        if (bp.vertices.size() < 2) continue;
        if (!is_visible(visibility, bp.layer_name)) continue;
        phenotype::PathBuilder pb;

        auto const start_canvas =
            transform.apply(bp.vertices[0].x, bp.vertices[0].y);
        pb.move_to(static_cast<float>(start_canvas.x),
                   static_cast<float>(start_canvas.y));

        std::size_t const n  = bp.vertices.size();
        std::size_t const sn = bp.closed ? n : (n - 1);
        for (std::size_t i = 0; i < sn; ++i) {
            auto const& va = bp.vertices[i];
            auto const& vb = bp.vertices[(i + 1) % n];
            double const bulge =
                (i < bp.bulges.size()) ? bp.bulges[i] : 0.0;
            auto const cb = transform.apply(vb.x, vb.y);

            if (bulge == 0.0) {
                pb.line_to(static_cast<float>(cb.x),
                           static_cast<float>(cb.y));
            } else {
                BulgeArc arc = bulge_to_arc(va, vb, bulge);
                auto const cc = transform.apply(arc.cx, arc.cy);
                double radius_canvas = arc.radius * transform.scale;
                // Y-flip: same convention as render_arcs. CCW in
                // CAD-world corresponds to CW after the y-flip;
                // swap start/end to restore the visual sweep.
                float canvas_start =
                    static_cast<float>(-arc.end_angle);
                float canvas_end   =
                    static_cast<float>(-arc.start_angle);
                pb.arc_to(static_cast<float>(cc.x),
                          static_cast<float>(cc.y),
                          static_cast<float>(radius_canvas),
                          canvas_start, canvas_end);
                // The backend's path dispatcher does not advance the
                // pen across an ArcTo (centre-form arcs do not
                // self-describe their endpoint), so set it explicitly
                // to the next segment's start.
                pb.move_to(static_cast<float>(cb.x),
                           static_cast<float>(cb.y));
            }
        }
        if (bp.closed) pb.close();
        p.stroke_path(pb, bp.thickness, to_paint(bp.color));
    }
    process_clip_markers(p, entities.clip_markers, bulged_cursor,
                         &ClipMarker::bulged_idx,
                         entities.bulged_polylines.size(), transform);

    // ---- ELLIPSE → MoveTo + cubic_to per ≤90° quadrant ----
    //
    // Standard 4-control-point cubic Bézier approximation of an ellipse
    // arc. `P(t) = C + U·cos(t) + V·sin(t)` where `U = major_axis` and
    // `V = perp(U) · minor_ratio`. For each chunk `[t0, t1]` with
    // `θ = t1 - t0 ≤ π/2`:
    //   k  = 4/3 · tan(θ / 4)
    //   p0 = P(t0)
    //   p3 = P(t1)
    //   t0_tan = -U·sin(t0) + V·cos(t0)   (parametric tangent at t0)
    //   t3_tan = -U·sin(t1) + V·cos(t1)
    //   p1 = p0 + k · t0_tan
    //   p2 = p3 - k · t3_tan
    //
    // Cubic Béziers are affine-invariant, so we can compute control
    // points in CAD-world space and then transform every point through
    // `ViewportTransform::apply` — the y-flip is automatic.
    std::size_t ellipse_cursor = 0;
    for (std::size_t ei = 0; ei < entities.ellipses.size(); ++ei) {
        process_clip_markers(p, entities.clip_markers, ellipse_cursor,
                             &ClipMarker::ellipses_idx, ei, transform);
        auto const& e = entities.ellipses[ei];
        if (!is_visible(visibility, e.layer_name)) continue;
        // Major axis vector U; perpendicular V is U rotated 90° CCW
        // (in CAD's y-up frame) scaled by minor_ratio.
        double const ux =  e.major_axis.x;
        double const uy =  e.major_axis.y;
        double const vx = -uy * e.minor_ratio;
        double const vy =  ux * e.minor_ratio;

        // Normalise the parametric range. AutoCAD ELLIPSE end_param
        // wraps past 2π for closed loops; clamp to a sensible sweep.
        double t0 = e.start_param;
        double t1 = e.end_param;
        // Treat (start == end) as a full ellipse — matches the
        // common DWG convention for closed ellipses.
        if (std::abs(t1 - t0) < 1e-9) t1 = t0 + kTwoPi;
        if (t1 < t0) t1 += kTwoPi;

        auto eval = [&](double t) -> Point {
            return Point{
                e.center.x + ux * std::cos(t) + vx * std::sin(t),
                e.center.y + uy * std::cos(t) + vy * std::sin(t),
            };
        };
        auto eval_tangent = [&](double t) -> Point {
            return Point{
                -ux * std::sin(t) + vx * std::cos(t),
                -uy * std::sin(t) + vy * std::cos(t),
            };
        };

        phenotype::PathBuilder pb;
        Point const p0_world = eval(t0);
        Point const p0_canvas = transform.apply(p0_world.x, p0_world.y);
        pb.move_to(static_cast<float>(p0_canvas.x),
                   static_cast<float>(p0_canvas.y));

        // Subdivide into chunks of ≤ π/2 so the cubic approximation
        // error stays bounded (~10⁻³ relative for the worst case).
        double tA = t0;
        while (tA < t1) {
            double tB = tA + kHalfPi;
            if (tB > t1) tB = t1;
            double const theta = tB - tA;
            double const k = (4.0 / 3.0) * std::tan(theta * 0.25);
            Point const pA       = eval(tA);
            Point const pB       = eval(tB);
            Point const tan_tA   = eval_tangent(tA);
            Point const tan_tB   = eval_tangent(tB);
            Point const c1_world = Point{pA.x + k * tan_tA.x,
                                         pA.y + k * tan_tA.y};
            Point const c2_world = Point{pB.x - k * tan_tB.x,
                                         pB.y - k * tan_tB.y};
            auto const c1 = transform.apply(c1_world.x, c1_world.y);
            auto const c2 = transform.apply(c2_world.x, c2_world.y);
            auto const cb = transform.apply(pB.x, pB.y);
            pb.cubic_to(static_cast<float>(c1.x), static_cast<float>(c1.y),
                        static_cast<float>(c2.x), static_cast<float>(c2.y),
                        static_cast<float>(cb.x), static_cast<float>(cb.y));
            tA = tB;
        }
        p.stroke_path(pb, e.thickness, to_paint(e.color));
    }
    process_clip_markers(p, entities.clip_markers, ellipse_cursor,
                         &ClipMarker::ellipses_idx,
                         entities.ellipses.size(), transform);
}

void render_hatches(phenotype::Painter& p,
                    Entities const& entities,
                    ViewportTransform const& transform,
                    LayerVisibility const& visibility) {
    // One `fill_path` per boundary loop. The parser already
    // discretised every curve segment into a polyline at parse
    // time, so the renderer just walks each loop's vertex list as a
    // `MoveTo + LineTo*` chain. Phenotype's `fill_path` implicitly
    // closes the polygon, so no explicit `Close` verb is needed.
    std::size_t cursor = 0;
    for (std::size_t hi = 0; hi < entities.hatches.size(); ++hi) {
        process_clip_markers(p, entities.clip_markers, cursor,
                             &ClipMarker::hatches_idx, hi, transform);
        auto const& h = entities.hatches[hi];
        if (!is_visible(visibility, h.layer_name)) continue;
        for (auto const& loop : h.loops) {
            if (loop.size() < 3) continue;
            phenotype::PathBuilder pb;
            auto const start = transform.apply(loop[0].x, loop[0].y);
            pb.move_to(static_cast<float>(start.x),
                       static_cast<float>(start.y));
            for (std::size_t i = 1; i < loop.size(); ++i) {
                auto const c = transform.apply(loop[i].x, loop[i].y);
                pb.line_to(static_cast<float>(c.x),
                           static_cast<float>(c.y));
            }
            p.fill_path(pb, to_paint(h.color));
        }
    }
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::hatches_idx,
                         entities.hatches.size(), transform);
}

} // namespace cadpp
