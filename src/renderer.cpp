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
#include "hershey.hpp"

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

// Solid (or fallback flat-colour) fill: walk every loop into a
// `phenotype::PathBuilder` and dispatch to `Painter::fill_path`. Used
// for `solid == true` hatches and for non-solid hatches whose
// `pattern_lines` couldn't be resolved (LibreDWG returned an empty
// pattern, or the hatch is a gradient — gradient ramp rendering ships
// in a future slab).
inline void render_hatch_solid(phenotype::Painter& p,
                               Hatch const& h,
                               ViewportTransform const& transform) {
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

// Patterned hatch: stroke each defline into a parallel-line family,
// clip every line against the union of boundary loops with the
// standard odd-parity rule, and emit each inside segment as a 1px
// `Painter::line`. Dash patterns on each defline are ignored at this
// pass — the next slab can split each (t0, t1) inside-segment along
// `pl.dashes` to reproduce ANSI31's dot-dash variants. Hatches with
// no resolved pattern fall back to `render_hatch_solid` so the
// boundary at least reads as a flat fill.
inline void render_hatch_pattern(phenotype::Painter& p,
                                 Hatch const& h,
                                 ViewportTransform const& transform) {
    if (h.pattern_lines.empty() || h.loops.empty()) return;

    // Boundary bbox in CAD/world coords. Used to choose which k
    // parallel lines actually cover the hatch.
    double xmin = std::numeric_limits<double>::infinity();
    double xmax = -std::numeric_limits<double>::infinity();
    double ymin = std::numeric_limits<double>::infinity();
    double ymax = -std::numeric_limits<double>::infinity();
    for (auto const& loop : h.loops) {
        for (auto const& v : loop) {
            if (v.x < xmin) xmin = v.x;
            if (v.x > xmax) xmax = v.x;
            if (v.y < ymin) ymin = v.y;
            if (v.y > ymax) ymax = v.y;
        }
    }
    if (!(xmax > xmin && ymax > ymin)) return;

    auto const paint = to_paint(h.color);

    for (auto const& pl : h.pattern_lines) {
        double const dx = std::cos(pl.angle);
        double const dy = std::sin(pl.angle);
        double const ox = pl.offset.x;
        double const oy = pl.offset.y;
        double const off_len2 = ox * ox + oy * oy;
        // A zero offset would mean "all parallel lines stack on top
        // of one another" — degenerate; skip rather than emit a
        // single line at `pt0`. Real ANSI / ISO patterns always
        // carry a non-zero perpendicular spacing.
        if (off_len2 < 1e-18) continue;

        // Project every bbox corner onto `offset` to find the k
        // range that covers the hatch. `k = ((c - origin) · offset)
        // / |offset|^2`. Pad by 1 on each side so a corner-touching
        // line still fires.
        double const inv_off_len2 = 1.0 / off_len2;
        double k_min = std::numeric_limits<double>::infinity();
        double k_max = -std::numeric_limits<double>::infinity();
        double const corners_x[4] = {xmin, xmax, xmax, xmin};
        double const corners_y[4] = {ymin, ymin, ymax, ymax};
        for (int c = 0; c < 4; ++c) {
            double const k = ((corners_x[c] - pl.origin.x) * ox
                              + (corners_y[c] - pl.origin.y) * oy)
                             * inv_off_len2;
            if (k < k_min) k_min = k;
            if (k > k_max) k_max = k;
        }

        // Pathological caps — small offset relative to bbox would
        // otherwise spawn millions of lines. 10k caps the worst case
        // at "ANSI31 across a 50ft border with default scale" worth
        // of work and bails out short of OOM.
        constexpr int kMaxLinesPerDefline = 10000;
        int const k_lo = static_cast<int>(std::floor(k_min)) - 1;
        int const k_hi = static_cast<int>(std::ceil(k_max)) + 1;
        if (k_hi - k_lo > kMaxLinesPerDefline) continue;

        // Reused across iterations to avoid per-k allocations.
        std::vector<double> ts;
        ts.reserve(16);

        for (int k = k_lo; k <= k_hi; ++k) {
            double const lx = pl.origin.x + static_cast<double>(k) * ox;
            double const ly = pl.origin.y + static_cast<double>(k) * oy;

            ts.clear();
            for (auto const& loop : h.loops) {
                std::size_t const n = loop.size();
                if (n < 2) continue;
                for (std::size_t i = 0; i < n; ++i) {
                    auto const& a = loop[i];
                    auto const& b = loop[(i + 1) % n];
                    double const ex = b.x - a.x;
                    double const ey = b.y - a.y;
                    double const det = dy * ex - dx * ey;
                    // Edge parallel to the line — no single-point
                    // intersection. Skip; coincident edges are rare
                    // enough for first-pass to ignore.
                    if (std::abs(det) < 1e-12) continue;
                    double const rx = a.x - lx;
                    double const ry = a.y - ly;
                    double const inv_det = 1.0 / det;
                    double const u = (dx * ry - dy * rx) * inv_det;
                    // Half-open on the upper edge to avoid double-
                    // counting when a line passes exactly through a
                    // shared vertex (each vertex is owned by the
                    // edge that starts at it, not the edge that
                    // ends there).
                    if (u < 0.0 || u >= 1.0) continue;
                    double const t = (ry * ex - rx * ey) * inv_det;
                    ts.push_back(t);
                }
            }

            if (ts.size() < 2) continue;
            std::sort(ts.begin(), ts.end());

            // Pair up via odd parity: ts[2i] is an enter, ts[2i+1]
            // is an exit. Multi-loop hatches (outer + holes) work
            // because the same parity rule cuts holes out: an
            // enter-exit sandwich around a hole splits into two
            // shorter segments.
            for (std::size_t i = 0; i + 1 < ts.size(); i += 2) {
                double const t0 = ts[i];
                double const t1 = ts[i + 1];
                if (t1 - t0 < 1e-9) continue;
                auto const a = transform.apply(lx + dx * t0,
                                               ly + dy * t0);
                auto const b = transform.apply(lx + dx * t1,
                                               ly + dy * t1);
                p.line(static_cast<float>(a.x), static_cast<float>(a.y),
                       static_cast<float>(b.x), static_cast<float>(b.y),
                       1.0f, paint);
            }
        }
    }
}

inline void render_hatch_item(phenotype::Painter& p,
                              Hatch const& h,
                              ViewportTransform const& transform) {
    if (h.solid || h.pattern_lines.empty()) {
        render_hatch_solid(p, h, transform);
    } else {
        render_hatch_pattern(p, h, transform);
    }
}

inline bool solid_quad_is_convex(SolidQuad const& q) {
    constexpr double kConvexEpsilon = 1e-12;
    Point const pts[4] = {q.p0, q.p1, q.p2, q.p3};
    bool has_pos = false;
    bool has_neg = false;
    for (int i = 0; i < 4; ++i) {
        auto const& a = pts[i];
        auto const& b = pts[(i + 1) % 4];
        auto const& c = pts[(i + 2) % 4];
        double const abx = b.x - a.x;
        double const aby = b.y - a.y;
        double const bcx = c.x - b.x;
        double const bcy = c.y - b.y;
        double const cross = abx * bcy - aby * bcx;
        if (cross > kConvexEpsilon) has_pos = true;
        if (cross < -kConvexEpsilon) has_neg = true;
        if (has_pos && has_neg) return false;
    }
    return has_pos || has_neg;
}

inline void render_solid_quad_path(phenotype::Painter& p,
                                   SolidQuad const& q,
                                   ViewportTransform const& transform) {
    auto const p0 = transform.apply(q.p0.x, q.p0.y);
    auto const p1 = transform.apply(q.p1.x, q.p1.y);
    auto const p2 = transform.apply(q.p2.x, q.p2.y);
    auto const p3 = transform.apply(q.p3.x, q.p3.y);
    phenotype::PathBuilder pb;
    pb.move_to(static_cast<float>(p0.x), static_cast<float>(p0.y));
    pb.line_to(static_cast<float>(p1.x), static_cast<float>(p1.y));
    pb.line_to(static_cast<float>(p2.x), static_cast<float>(p2.y));
    pb.line_to(static_cast<float>(p3.x), static_cast<float>(p3.y));
    p.fill_path(pb, to_paint(q.color));
}

class SolidFillBatcher {
public:
    SolidFillBatcher(phenotype::Painter& painter,
                     ViewportTransform const& viewport_transform)
        : p(painter), transform(viewport_transform) {
        rect_batch.reserve(kBatchSize);
        quad_batch.reserve(kBatchSize);
    }

    void add(SolidQuad const& q) {
        auto const color = to_paint(q.color);
        bool const axis_aligned =
            near(q.p0.y, q.p1.y) && near(q.p2.y, q.p3.y)
            && near(q.p0.x, q.p3.x) && near(q.p1.x, q.p2.x);
        if (axis_aligned) {
            double const min_wx = std::min(q.p0.x, q.p1.x);
            double const max_wx = std::max(q.p0.x, q.p1.x);
            double const min_wy = std::min(q.p0.y, q.p3.y);
            double const max_wy = std::max(q.p0.y, q.p3.y);
            auto const ca = transform.apply(min_wx, max_wy);
            auto const cb = transform.apply(max_wx, min_wy);
            double const min_x = std::min(ca.x, cb.x);
            double const max_x = std::max(ca.x, cb.x);
            double const min_y = std::min(ca.y, cb.y);
            double const max_y = std::max(ca.y, cb.y);
            select_kind(BatchKind::Rect);
            rect_batch.push_back(phenotype::PaintRect{
                static_cast<float>(min_x),
                static_cast<float>(min_y),
                static_cast<float>(max_x - min_x),
                static_cast<float>(max_y - min_y),
                color,
            });
            if (rect_batch.size() == kBatchSize) flush();
            return;
        }

        if (!solid_quad_is_convex(q)) {
            flush();
            render_solid_quad_path(p, q, transform);
            return;
        }

        auto const p0 = transform.apply(q.p0.x, q.p0.y);
        auto const p1 = transform.apply(q.p1.x, q.p1.y);
        auto const p2 = transform.apply(q.p2.x, q.p2.y);
        auto const p3 = transform.apply(q.p3.x, q.p3.y);
        select_kind(BatchKind::Quad);
        quad_batch.push_back(phenotype::PaintQuad{
            static_cast<float>(p0.x), static_cast<float>(p0.y),
            static_cast<float>(p1.x), static_cast<float>(p1.y),
            static_cast<float>(p2.x), static_cast<float>(p2.y),
            static_cast<float>(p3.x), static_cast<float>(p3.y),
            color,
        });
        if (quad_batch.size() == kBatchSize) flush();
    }

    void flush() {
        if (!rect_batch.empty()) {
            p.fill_rects(rect_batch.data(),
                         static_cast<unsigned int>(rect_batch.size()));
            rect_batch.clear();
        }
        if (!quad_batch.empty()) {
            p.fill_quads(quad_batch.data(),
                         static_cast<unsigned int>(quad_batch.size()));
            quad_batch.clear();
        }
        active_kind = BatchKind::None;
    }

private:
    enum class BatchKind { None, Rect, Quad };
    static constexpr std::size_t kBatchSize = 8192;
    static constexpr double kAxisEpsilon = 1e-9;

    static bool near(double a, double b) {
        return std::abs(a - b) <= kAxisEpsilon;
    }

    void select_kind(BatchKind kind) {
        if (active_kind != BatchKind::None && active_kind != kind) flush();
        active_kind = kind;
    }

    phenotype::Painter& p;
    ViewportTransform const& transform;
    std::vector<phenotype::PaintRect> rect_batch;
    std::vector<phenotype::PaintQuad> quad_batch;
    BatchKind active_kind = BatchKind::None;
};

} // namespace

// Effective stroke pixel-thickness: a positive `world_width` (LWPOLYLINE
// const_width / per-vertex width) takes precedence over the lineweight-
// derived screen pen because it represents an authored geometric width,
// not just a rendering hint. The world value is multiplied by the
// canvas's world→pixel scale so the stroke fattens proportionally as
// the user zooms in — paper-space borders saved at e.g. 0.051" stay
// the same fraction of the sheet at every zoom level. We still floor at
// the lineweight thickness so a misauthored zero world width wouldn't
// silently shrink the stroke below the user's expectation.
inline float effective_thickness(float lineweight_px,
                                 float world_width,
                                 ViewportTransform const& transform) {
    if (world_width > 0.0f) {
        float const px = world_width * static_cast<float>(transform.scale);
        return std::max(lineweight_px, px);
    }
    return lineweight_px;
}

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
        float const stroke_px =
            effective_thickness(l.thickness, l.world_width, transform);
        p.line(static_cast<float>(a.x), static_cast<float>(a.y),
               static_cast<float>(b.x), static_cast<float>(b.y),
               stroke_px, to_paint(l.color));
    }
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::lines_idx,
                         entities.lines.size(), transform);
}

void render_arrows(phenotype::Painter& p,
                   Entities const& entities,
                   ViewportTransform const& transform,
                   LayerVisibility const& visibility) {
    // Each ArrowHead carries its tip in world coords, a unit
    // direction `(dx, dy)` pointing FROM the tip back along the
    // leader (so `tip + dir * size` is the centre of the triangle's
    // base), and the triangle edge length in world units. The
    // 0.18 half-width factor matches AutoCAD's "Closed Filled"
    // arrow style — base width = 0.36 × length, ~21° half-angle.
    //
    // Computing the three vertices in WORLD space and then
    // transforming each one through `transform.apply` keeps the
    // viewport's Y-flip self-consistent with the rest of the
    // line geometry — a leader pointing "up" in CAD coordinates
    // ends up with its triangle correctly opening upward on screen.
    constexpr double kHalfWidth = 0.18;
    for (auto const& a : entities.arrows) {
        if (!is_visible(visibility, a.layer_name)) continue;
        if (a.size <= 0.0) continue;
        // Perpendicular in world frame (CCW 90°).
        double const px = -a.dy;
        double const py =  a.dx;
        Point const tip{a.tip.x, a.tip.y};
        Point const base_c{
            tip.x + a.dx * a.size,
            tip.y + a.dy * a.size,
        };
        double const half = kHalfWidth * a.size;
        Point const base1{base_c.x + px * half, base_c.y + py * half};
        Point const base2{base_c.x - px * half, base_c.y - py * half};
        auto const t = transform.apply(tip.x,   tip.y);
        auto const b1 = transform.apply(base1.x, base1.y);
        auto const b2 = transform.apply(base2.x, base2.y);
        phenotype::PathBuilder pb;
        pb.move_to(static_cast<float>(t.x),  static_cast<float>(t.y));
        pb.line_to(static_cast<float>(b1.x), static_cast<float>(b1.y));
        pb.line_to(static_cast<float>(b2.x), static_cast<float>(b2.y));
        p.fill_path(pb, to_paint(a.color));
    }
}

// Resolve a per-run FontSpec, combining the entity's outer Style with
// the run's optional overrides. Family always passes through the alias
// table so SHX / Bitstream names resolve to host fonts. The entity-
// level `width_factor` is propagated onto every run's FontSpec — the
// macOS backend applies it via the Core Text font matrix so a
// stretched ATTRIB ("ADDA" with t_wf=6.54 in
// `blocks_and_tables_-_imperial.dwg`) renders wide along the run's
// local X axis. MTEXT inline `\W<x>;` codes are deferred and would
// land as a TextRun per-run override alongside `height_scale`.
inline phenotype::FontSpec resolve_run_spec(Style const& outer,
                                            TextRun const& r,
                                            float entity_width_factor) {
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
        entity_width_factor,
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
        // MTEXT bounding-rectangle width in canvas px. Plain TEXT
        // entities and unbounded MTEXT keep this at 0 so the wrap
        // path inside `walk` short-circuits to the legacy
        // "break only on `\n` / `\t`" behaviour and existing
        // single-line / multi-line MTEXT regressions stay
        // untouched.
        float const wrap_width_px =
            static_cast<float>(t.wrap_width * transform.scale);

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
        // Per-line side bearings for h-align correction. Hershey's
        // kerning advance `o` often differs from each glyph's visible
        // extent (e.g. `D` advances 11 but visibly ends at max_x=18,
        // sitting inside the kerning), so `line_widths/2` alone
        // shifts visible centres off the anchor for some letter
        // pairs (HRWD's `D` was leftward of HALL's `L` in the room
        // labels). `line_first_left_bearing[li]` is the very first
        // visible glyph's `min_x`; `line_last_right_overflow[li]` is
        // the most-recent stroke segment's last visible glyph's
        // `max_x − advance` (negative when the visible right edge
        // sits inside the kerning). TTF / empty segments leave these
        // alone — TTF runs through `Painter::measure_text` already
        // returns a near-visible width.
        std::vector<float> line_first_left_bearing;
        std::vector<float> line_last_right_overflow;
        std::vector<bool>  line_first_seen;

        auto walk = [&](auto on_segment) {
            std::size_t li = 0;
            // Per-line running width in canvas px. Tracked locally
            // inside walk (one fresh copy per pass call) so the
            // soft-wrap decisions stay consistent across pass 1 and
            // pass 2: each on_segment hook returns its measured
            // width and walk accumulates that into running[li].
            // Pass 1 measures fresh; pass 2 replays seg_measured
            // recorded in pass 1 — both deterministic, so wrap
            // points and bump_line calls match.
            std::vector<float> running(1, 0.0f);
            auto bump_line = [&]() {
                ++li;
                if (li >= line_widths.size()) {
                    line_widths.resize(li + 1, 0.0f);
                    line_heights.resize(li + 1, 0.0f);
                    line_first_left_bearing.resize(li + 1, 0.0f);
                    line_last_right_overflow.resize(li + 1, 0.0f);
                    line_first_seen.resize(li + 1, false);
                }
                if (li >= running.size()) running.resize(li + 1, 0.0f);
            };
            // Wrap-aware emit: when `wrap_width_px > 0`, split a
            // newline-/tab-free chunk into words greedily and call
            // on_segment per word, inserting bump_line() at each
            // soft break. Words that exceed the rect on their own
            // overflow rather than breaking mid-glyph — matches
            // AutoCAD's behaviour on long URLs / single tokens
            // wider than the defined width.
            auto emit_chunk = [&](std::string_view chunk, float font_px,
                                  phenotype::FontSpec const& spec,
                                  phenotype::Color paint,
                                  hershey::Variant variant) {
                if (chunk.empty()) return;
                if (wrap_width_px <= 0.0f) {
                    float const m = on_segment(li, chunk, font_px, spec,
                                               paint, variant);
                    running[li] += m;
                    return;
                }
                std::size_t i = 0;
                while (i < chunk.size()) {
                    // Tokenise into one word + trailing whitespace.
                    // Trailing spaces stay attached to the preceding
                    // word so the natural word-space-word cadence
                    // prints intact; only inter-word boundaries can
                    // become wrap points. UTF-8 multibyte sequences
                    // look like opaque non-space bytes, which is
                    // fine for the Western MTEXT this samples
                    // covers — CJK kinsoku rules are out of scope.
                    std::size_t const ws = i;
                    while (i < chunk.size() && chunk[i] != ' ') ++i;
                    std::size_t const we = i;
                    while (i < chunk.size() && chunk[i] == ' ') ++i;
                    std::string_view const word = chunk.substr(ws, we - ws);
                    // Stroke runs measure their own width — wrap
                    // points stay aligned with what `draw_run` will
                    // emit instead of asking the system Helvetica.
                    float const word_w = word.empty() ? 0.0f
                        : (variant != hershey::Variant::kNone
                            ? hershey::measure_run(variant, word, font_px,
                                                   spec.width_factor)
                            : p.measure_text(
                                word.data(),
                                static_cast<unsigned int>(word.size()),
                                font_px, spec));
                    // Soft-break before a word that would overshoot
                    // the rect, unless the line is already empty
                    // (a single oversized word emits on its own
                    // line and still overflows — same as AutoCAD).
                    if (running[li] > 0.0f
                        && running[li] + word_w > wrap_width_px) {
                        bump_line();
                    }
                    std::string_view const seg = chunk.substr(ws, i - ws);
                    float const m = on_segment(li, seg, font_px, spec,
                                               paint, variant);
                    running[li] += m;
                }
            };
            auto emit_run_text = [&](std::string_view text, float font_px,
                                     phenotype::FontSpec const& spec,
                                     phenotype::Color paint,
                                     hershey::Variant variant) {
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
                        emit_chunk(piece, font_px, spec, paint, variant);
                    }
                    if (next == std::string_view::npos) break;
                    char const brk = rest[next];
                    if (brk == '\n') {
                        bump_line();
                    } else { // '\t'
                        // Emit a sentinel tab segment with empty text;
                        // pass 1 records the tab advance + bumps the
                        // tab cursor, pass 2 advances cursor to next
                        // tab stop. on_segment returns the advance
                        // so walk can keep `running[li]` in sync for
                        // any post-tab wrap decision.
                        float const m = on_segment(li, std::string_view{},
                                                   font_px, spec, paint,
                                                   variant);
                        running[li] += m;
                    }
                    rest = rest.substr(next + 1);
                }
            };
            // Seed line 0 in the metrics arrays so on_segment can index
            // into them on the very first segment.
            if (line_widths.empty()) {
                line_widths.push_back(0.0f);
                line_heights.push_back(0.0f);
                line_first_left_bearing.push_back(0.0f);
                line_last_right_overflow.push_back(0.0f);
                line_first_seen.push_back(false);
            }
            float const entity_wf = static_cast<float>(t.width_factor);
            // Variant decision lives next to the FontSpec build so
            // both passes see the same value. `t.style.font_file`
            // is the only `font_file` cadpp carries; MTEXT `\f`
            // overrides ride on `r.family_override` (family only,
            // no file), and the family token is what
            // `resolve_variant`'s SHX-family table inspects.
            if (t.runs.empty()) {
                std::string_view const alias =
                    alias_font_family(t.style.font_family);
                std::string_view const family =
                    alias.empty() ? std::string_view{t.style.font_family} : alias;
                hershey::Variant const variant = hershey::resolve_variant(
                    t.style.font_family, t.style.font_file);
                phenotype::FontSpec const spec{
                    family,
                    t.style.bold   ? phenotype::FontWeight::Bold   : phenotype::FontWeight::Regular,
                    t.style.italic ? phenotype::FontStyle::Italic  : phenotype::FontStyle::Upright,
                    false,
                    entity_wf,
                };
                emit_run_text(t.content, outer_font_px, spec,
                              to_paint(t.color), variant);
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
                    phenotype::FontSpec const spec =
                        resolve_run_spec(t.style, r, entity_wf);
                    // Inline `\f<face>;` overrides feed a family but
                    // no font_file — defer to the entity STYLE's
                    // font_file for the SHX gate. When the override
                    // is empty we use the entity family directly.
                    std::string_view const fam_for_variant =
                        r.family_override.empty()
                            ? std::string_view{t.style.font_family}
                            : std::string_view{r.family_override};
                    hershey::Variant const variant = hershey::resolve_variant(
                        fam_for_variant, t.style.font_file);
                    Color const color = (r.color_override.a != 0)
                        ? r.color_override : t.color;
                    phenotype::Color const paint = to_paint(color);
                    emit_run_text(r.text, run_font_px, spec, paint, variant);
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
        // last stop) instead of measuring glyphs. Returns the
        // measured width so walk can keep its per-pass `running`
        // cursor in lockstep with `line_widths` for soft-wrap
        // decisions inside emit_chunk.
        walk([&](std::size_t li, std::string_view piece, float font_px,
                 phenotype::FontSpec const& spec, phenotype::Color,
                 hershey::Variant variant) -> float {
            float measured;
            if (piece.empty()) {
                measured = tab_advance(line_widths[li]);
            } else if (variant != hershey::Variant::kNone) {
                // Stroke runs measure with the same advance table
                // `draw_run` uses below — keeps pass 1 and pass 2 in
                // exact agreement so soft-wrap / h-align stay tight.
                measured = hershey::measure_run(variant, piece, font_px,
                                                spec.width_factor);
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
            // Update per-line stroke bearings for h-align Center/
            // Middle (see `line_first_left_bearing` declaration).
            // Only stroke segments contribute; tab sentinels and
            // TTF segments leave the per-line state alone, which is
            // what we want — TTF measures are already close to the
            // visible width, and tab sentinels have no glyphs.
            if (!piece.empty() && variant != hershey::Variant::kNone) {
                auto const b = hershey::run_bearings(variant, piece,
                                                    font_px,
                                                    spec.width_factor);
                if (!line_first_seen[li]) {
                    line_first_left_bearing[li] = b.first_left_bearing_px;
                    line_first_seen[li] = true;
                }
                // Last-segment overflow is overwritten by each new
                // stroke segment so the final value reflects the
                // line's right-most visible glyph.
                line_last_right_overflow[li] = b.last_right_overflow_px;
            } else if (!piece.empty() && !line_first_seen[li]) {
                // First segment of the line is TTF (or anything non-
                // stroke) — leave bearings at 0 but mark "first seen"
                // so a later stroke segment doesn't overwrite the
                // left-bearing entry (which would shift the line's
                // visible anchor based on the wrong glyph).
                line_first_seen[li] = true;
            }
            return measured;
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
        //
        // Stroke-style entities (STYLE.font_file ends in .shx) need
        // a slightly bumped multiplier: the glyphs draw with a larger
        // visible cap height than phenotype's TTF rasterisation
        // (cap == font_px vs. ~0.7×font_px for Helvetica) and they
        // emit descenders (g/y/p/q/j) into the row below their
        // baseline. The hand-off-tuned value below keeps the stroke
        // MTEXT rows readable without pushing them apart visibly
        // compared to Autodesk Viewer's SHX rendering (which uses
        // tighter leading than phenotype's TTF calibration).
        //
        // Note: `total_height` (sum of line_advances) scales with
        // this multiplier and drives the v_align Middle math. The
        // stroke `cap_offset` we pass to `hershey::draw_run` below
        // is locked to `(multiplier − 1) / 2 × font_px` so the
        // visible centre of a Middle-aligned single-line run stays
        // exactly on the anchor for any multiplier we pick — that
        // way line-spacing retuning doesn't drag hexagon callout
        // numbers off-centre.
        bool  const entity_is_stroke =
            hershey::is_shx_font_file(t.style.font_file);
        float const line_advance_multiplier =
            entity_is_stroke ? 1.55f : 1.25f;
        float const default_advance =
            outer_font_px * line_advance_multiplier
            * static_cast<float>(t.line_spacing);
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
                // Stroke runs need a small h-align correction so the
                // *visible* bounding box lands on the anchor — the
                // kerning-advance sum (`line_widths`) drifts off-
                // centre for letters whose visible right sits inside
                // their kerning width (e.g. `D`'s `max_x=18` vs.
                // `advance≈21`, which shifts the visible centre of
                // `HRWD` slightly left of `HALL`). The renderer of
                // measure_run + run_bearings populated these per-line
                // bookkeeping fields in pass 1; here we collapse
                // them into the line's start cursor.
                float const visible_center_adj =
                    (line_first_left_bearing[li]
                     + line_last_right_overflow[li]) * 0.5f;
                float lx = anchor_x;
                switch (t.h_align) {
                case TextHAlign::Left:                                  break;
                case TextHAlign::Center:
                    lx -= line_widths[li] * 0.5f + visible_center_adj; break;
                case TextHAlign::Middle:
                    lx -= line_widths[li] * 0.5f + visible_center_adj; break;
                case TextHAlign::Right:
                    lx -= line_widths[li] + line_last_right_overflow[li]; break;
                }
                line_start_xs[li] = lx;
            }
        }

        // Canvas-frame rotation. World-frame CAD rotation is CCW
        // about world +Z; the viewport flips world Y onto canvas
        // Y-down, so canvas-frame rotation is the negation. The
        // entity's `rotation` field is in CAD's world frame —
        // negate once for the canvas, then pass that signed value
        // through to phenotype which rotates each glyph quad
        // around the run's pivot. Mirrors the precedent at
        // render_arcs (line 599) where `start`/`end` swap signs to
        // compensate for the same Y-flip.
        float const canvas_rotation = -static_cast<float>(t.rotation);
        bool const rotated = canvas_rotation != 0.0f;
        float const cosR = rotated ? std::cos(canvas_rotation) : 1.0f;
        float const sinR = rotated ? std::sin(canvas_rotation) : 0.0f;

        // Pass 2: emit draws. seg_measured indexed in the same visit
        // order as pass 1 so each on_segment call consumes its own
        // pre-computed measured width. Empty `piece` = TAB sentinel —
        // skip the draw call but still advance the cursor. Returns
        // measured so walk's `running[li]` stays consistent with
        // pass 1 — the soft-wrap decisions inside emit_chunk re-run
        // here and must reach the same bump_line points.
        std::vector<float> line_x_cursor = line_start_xs;
        std::size_t mi = 0;
        walk([&](std::size_t li, std::string_view piece, float font_px,
                 phenotype::FontSpec const& spec,
                 phenotype::Color color,
                 hershey::Variant variant) -> float {
            float const measured = seg_measured[mi++];
            if (!piece.empty()) {
                // Bottom-align segments within the line so a tall and
                // a short run on the same line share a baseline-ish y.
                float const seg_y = line_top_ys[li] + (line_heights[li] - font_px);
                float draw_x = line_x_cursor[li];
                float draw_y = seg_y;
                if (rotated) {
                    // Pre-rotate the segment's run-origin around the
                    // entity's canvas anchor. Each segment becomes
                    // its own rotated pivot — phenotype (or the
                    // stroke renderer) then rotates the glyph stream
                    // around that pivot by `canvas_rotation`, so the
                    // per-glyph offset within a run stays axis-
                    // aligned in the rotated frame and the multi-
                    // segment block reads as a coherent rigid body.
                    float const dx = draw_x - anchor_x;
                    float const dy = draw_y - anchor_y;
                    draw_x = anchor_x + dx * cosR - dy * sinR;
                    draw_y = anchor_y + dx * sinR + dy * cosR;
                }
                if (variant != hershey::Variant::kNone) {
                    // Stroke text reuses the surrounding clip /
                    // viewport state and emits `Painter::line` calls
                    // under the same canvas frame as the TTF path.
                    // Thickness is held at a 1 px hairline — AutoCAD
                    // SHX rendering is conventionally drawn that way
                    // regardless of the layer's lineweight.
                    //
                    // `cap_offset_y` is the gap from `draw_y` (the
                    // segment's "font-box top" in this renderer's
                    // vocabulary) to the glyph's cap-top. Locked to
                    // `(line_advance_multiplier − 1) / 2 × font_px`
                    // so the v_align Middle visible centre of a
                    // single-line run lands exactly on the entity
                    // anchor (block math: total_height − font_px →
                    // halved → that's the cap-top offset that puts
                    // visual_center back at `anchor_y`).
                    float const stroke_cap_offset =
                        font_px * (line_advance_multiplier - 1.0f) * 0.5f;
                    hershey::draw_run(p, variant,
                                      draw_x, draw_y, stroke_cap_offset,
                                      piece,
                                      font_px,
                                      spec.width_factor,
                                      /*oblique_rad=*/0.0f,
                                      color, /*thickness=*/1.0f,
                                      canvas_rotation);
                } else {
                    p.text(draw_x, draw_y,
                           piece.data(),
                           static_cast<unsigned int>(piece.size()),
                           font_px, color, spec, canvas_rotation);
                }
            }
            line_x_cursor[li] += measured;
            return measured;
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
        float const stroke_px =
            effective_thickness(bp.thickness, bp.world_width, transform);
        p.stroke_path(pb, stroke_px, to_paint(bp.color));
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
        render_hatch_item(p, h, transform);
    }
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::hatches_idx,
                         entities.hatches.size(), transform);
}

void render_solid_quads(phenotype::Painter& p,
                        Entities const& entities,
                        ViewportTransform const& transform,
                        LayerVisibility const& visibility) {
    constexpr std::size_t kBatchSize = 8192;
    constexpr double kAxisEpsilon = 1e-9;
    std::vector<phenotype::PaintRect> rect_batch;
    std::vector<phenotype::PaintQuad> quad_batch;
    rect_batch.reserve(kBatchSize);
    quad_batch.reserve(kBatchSize);
    enum class BatchKind { None, Rect, Quad };
    BatchKind active_kind = BatchKind::None;

    auto flush = [&]() {
        if (!rect_batch.empty()) {
            p.fill_rects(rect_batch.data(),
                         static_cast<unsigned int>(rect_batch.size()));
            rect_batch.clear();
        }
        if (!quad_batch.empty()) {
            p.fill_quads(quad_batch.data(),
                         static_cast<unsigned int>(quad_batch.size()));
            quad_batch.clear();
        }
        active_kind = BatchKind::None;
    };
    auto select_kind = [&](BatchKind kind) {
        if (active_kind != BatchKind::None && active_kind != kind) flush();
        active_kind = kind;
    };
    auto near = [&](double a, double b) {
        return std::abs(a - b) <= kAxisEpsilon;
    };

    std::size_t cursor = 0;
    for (std::size_t qi = 0; qi < entities.solid_quads.size(); ++qi) {
        if (cursor < entities.clip_markers.size()
            && entities.clip_markers[cursor].solid_quads_idx == qi) {
            flush();
            process_clip_markers(p, entities.clip_markers, cursor,
                                 &ClipMarker::solid_quads_idx, qi, transform);
        }
        auto const& q = entities.solid_quads[qi];
        if (!is_visible(visibility, q.layer_name)) continue;

        auto const color = to_paint(q.color);
        bool const axis_aligned =
            near(q.p0.y, q.p1.y) && near(q.p2.y, q.p3.y)
            && near(q.p0.x, q.p3.x) && near(q.p1.x, q.p2.x);
        if (axis_aligned) {
            double const min_wx = std::min(q.p0.x, q.p1.x);
            double const max_wx = std::max(q.p0.x, q.p1.x);
            double const min_wy = std::min(q.p0.y, q.p3.y);
            double const max_wy = std::max(q.p0.y, q.p3.y);
            auto const ca = transform.apply(min_wx, max_wy);
            auto const cb = transform.apply(max_wx, min_wy);
            double const min_x = std::min(ca.x, cb.x);
            double const max_x = std::max(ca.x, cb.x);
            double const min_y = std::min(ca.y, cb.y);
            double const max_y = std::max(ca.y, cb.y);
            select_kind(BatchKind::Rect);
            rect_batch.push_back(phenotype::PaintRect{
                static_cast<float>(min_x),
                static_cast<float>(min_y),
                static_cast<float>(max_x - min_x),
                static_cast<float>(max_y - min_y),
                color,
            });
            if (rect_batch.size() == kBatchSize) flush();
            continue;
        }

        if (!solid_quad_is_convex(q)) {
            flush();
            render_solid_quad_path(p, q, transform);
            continue;
        }

        auto const p0 = transform.apply(q.p0.x, q.p0.y);
        auto const p1 = transform.apply(q.p1.x, q.p1.y);
        auto const p2 = transform.apply(q.p2.x, q.p2.y);
        auto const p3 = transform.apply(q.p3.x, q.p3.y);
        select_kind(BatchKind::Quad);
        quad_batch.push_back(phenotype::PaintQuad{
            static_cast<float>(p0.x), static_cast<float>(p0.y),
            static_cast<float>(p1.x), static_cast<float>(p1.y),
            static_cast<float>(p2.x), static_cast<float>(p2.y),
            static_cast<float>(p3.x), static_cast<float>(p3.y),
            color,
        });
        if (quad_batch.size() == kBatchSize) flush();
    }
    flush();
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::solid_quads_idx,
                         entities.solid_quads.size(), transform);
}

void render_fills(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform,
                  LayerVisibility const& visibility) {
    if (entities.hatches.empty()) {
        render_solid_quads(p, entities, transform, visibility);
        return;
    }
    if (entities.solid_quads.empty()) {
        render_hatches(p, entities, transform, visibility);
        return;
    }

    SolidFillBatcher batcher(p, transform);
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < entities.fills.size(); ++i) {
        if (cursor < entities.clip_markers.size()
            && entities.clip_markers[cursor].fills_idx == i) {
            batcher.flush();
            process_clip_markers(p, entities.clip_markers, cursor,
                                 &ClipMarker::fills_idx, i, transform);
        }

        auto const& item = entities.fills[i];
        if (item.kind == FillKind::SolidQuad) {
            if (item.index >= entities.solid_quads.size()) continue;
            auto const& q = entities.solid_quads[item.index];
            if (!is_visible(visibility, q.layer_name)) continue;
            batcher.add(q);
        } else {
            if (item.index >= entities.hatches.size()) continue;
            auto const& h = entities.hatches[item.index];
            if (!is_visible(visibility, h.layer_name)) continue;
            batcher.flush();
            render_hatch_item(p, h, transform);
        }
    }
    batcher.flush();
    process_clip_markers(p, entities.clip_markers, cursor,
                         &ClipMarker::fills_idx,
                         entities.fills.size(), transform);
}

} // namespace cadpp
