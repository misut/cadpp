// cad++ — DWG parser facade implementation.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "parser.hpp"
// `geom.hpp` carries the `Affine` type the Slab 5 INSERT / DIMENSION
// expansion uses to flatten block-local coordinates into world space.
#include "geom.hpp"

extern "C" {
#include <dwg.h>
#include <dwg_api.h>
// Internal LibreDWG bit_convert_TU helper isn't in the public
// dwg_api.h — declare it here. Converts UTF-16LE (BITCODE_TU) to a
// freshly malloc'd UTF-8 string.
char* bit_convert_TU(uint16_t const* wstr);
}

namespace cadpp {

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// LibreDWG color-method codes (Dwg_Color_Method in include/dwg.h).
// Reproduced as integer constants here so we don't have to expose the
// LibreDWG enum through the parser facade.
constexpr int kColorMethodAci       = 0xc2;
constexpr int kColorMethodTruecolor = 0xc3;

// True if the CMC carries an actual RGB value (truecolor or a usable
// ACI palette index). False for BYLAYER / BYBLOCK / ACI-7 / unknown,
// which all delegate to the layer's colour or, ultimately, to the
// cad++ default ink (Color{}). ACI 7 ("white-on-dark, black-on-light")
// stays a fall-through because rendering ACI-7 white-on-light would
// be invisible — matches every CAD viewer.
bool is_resolvable_cmc(Dwg_Color const& c) {
    int const method = static_cast<int>(c.method);
    if (method == kColorMethodTruecolor) return true;
    if (method == kColorMethodAci) {
        int const idx = static_cast<int>(c.index);
        return idx > 0 && idx != 7 && idx < 256;
    }
    return false;
}

// Translate a resolvable CMC to RGBA. Non-resolvable CMCs return the
// cad++ default ink — callers normally check `is_resolvable_cmc` first
// and fall back to the layer's colour before hitting that branch.
Color color_from_cmc(Dwg_Color const& c) {
    int const method = static_cast<int>(c.method);
    if (method == kColorMethodTruecolor) {
        std::uint32_t const rgb = static_cast<std::uint32_t>(c.rgb);
        return Color{
            static_cast<std::uint8_t>((rgb >> 16) & 0xff),
            static_cast<std::uint8_t>((rgb >>  8) & 0xff),
            static_cast<std::uint8_t>( rgb        & 0xff),
            255,
        };
    }
    if (method == kColorMethodAci) {
        int const idx = static_cast<int>(c.index);
        if (idx > 0 && idx != 7 && idx < 256) {
            BITCODE_BL const packed = dwg_rgb_palette_index(
                static_cast<BITCODE_BS>(idx));
            return Color{
                static_cast<std::uint8_t>((packed >> 16) & 0xff),
                static_cast<std::uint8_t>((packed >>  8) & 0xff),
                static_cast<std::uint8_t>( packed        & 0xff),
                255,
            };
        }
    }
    return Color{};
}

// Slab 4: bundle of (resolved colour, layer name) for one entity.
// Doing both lookups in one helper means we only call
// `dwg_get_entity_layer` once per entity — either to resolve a
// BYLAYER / BYBLOCK / ACI-7 colour, or just to record which layer the
// entity belongs to so the panel can toggle its visibility.
struct EntityMetadata {
    Color       color;
    std::string layer_name;
};

// LibreDWG quirk: r2007+ DWG files store text fields as UTF-16LE
// (`BITCODE_TU` = `uint16_t*`), but the struct member type is the
// narrow `BITCODE_T` (= `char*`). The wide pointer is cast to char*
// without conversion, so for ASCII text "AutoCAD" stored as UTF-16
// (`41 00 75 00 ...`) `strlen()` returns 1 — the NUL high byte
// terminates the C string after one character.
//
// Detect r2007+ on the parent dwg and run `bit_convert_TU` to get a
// proper UTF-8 string. Caller owns the returned `std::string`.
std::string read_text_field(Dwg_Data const* dwg, char const* raw) {
    if (raw == nullptr) return {};
    if (dwg != nullptr && dwg->header.version >= R_2007) {
        char* utf8 = bit_convert_TU(reinterpret_cast<uint16_t const*>(raw));
        if (utf8 == nullptr) return {};
        std::string out{utf8};
        std::free(utf8);
        return out;
    }
    return std::string{raw};
}

EntityMetadata resolve_entity_metadata(Dwg_Data const* dwg,
                                       Dwg_Object_Entity const* ent) {
    EntityMetadata out{};
    if (ent == nullptr) return out;

    Dwg_Object_LAYER* layer = dwg_get_entity_layer(ent);
    if (layer != nullptr && layer->name != nullptr) {
        out.layer_name = read_text_field(dwg, layer->name);
    }

    if (is_resolvable_cmc(ent->color)) {
        out.color = color_from_cmc(ent->color);
    } else if (layer != nullptr && is_resolvable_cmc(layer->color)) {
        // BYLAYER (or BYBLOCK / ACI-7 — same code path here) — fall
        // back to the layer's stored colour. With Slab 4 in place this
        // is what makes ACI-coded layer colours actually appear in the
        // viewer instead of rendering as flat near-black ink.
        out.color = color_from_cmc(layer->color);
    }
    return out;
}

// Slab 7 — resolve an entity's effective linetype dash pattern.
//
// Tries the entity's own `ltype` handle first; if that's absent or
// resolves outside the LTYPE table (BYLAYER / BYBLOCK / null) the
// layer's stored linetype is used as the fallback. CONTINUOUS-named
// linetypes and zero-dash patterns return empty so the caller can
// fast-path solid LINEs through the existing emit.
//
// Returned vector is the raw signed dash pattern in world units —
// positive = dash, negative = gap, zero = dot.
std::vector<double> resolve_entity_dashes(
        Dwg_Data* dwg, Dwg_Object_Entity const* ent) {
    if (ent == nullptr) return {};

    auto try_ltype_obj = [&](BITCODE_H ref) -> Dwg_Object_LTYPE const* {
        if (ref == nullptr) return nullptr;
        Dwg_Object* obj = dwg_ref_object(dwg, ref);
        if (obj == nullptr
            || obj->supertype != DWG_SUPERTYPE_OBJECT
            || static_cast<int>(obj->fixedtype) != DWG_TYPE_LTYPE) {
            return nullptr;
        }
        return obj->tio.object->tio.LTYPE;
    };

    Dwg_Object_LTYPE const* lt = try_ltype_obj(ent->ltype);
    if (lt == nullptr) {
        Dwg_Object_LAYER* layer = dwg_get_entity_layer(ent);
        if (layer != nullptr) lt = try_ltype_obj(layer->ltype);
    }
    if (lt == nullptr || lt->numdashes == 0 || lt->dashes == nullptr) {
        return {};
    }
    // CONTINUOUS / Continuous — explicit solid linetype with no dashes.
    // BYLAYER / BYBLOCK names are sentinel pseudo-linetypes — leave
    // the line solid here; entity-side `BYLAYER` already fell through
    // to the layer's actual ltype above.
    if (lt->name != nullptr) {
        std::string const n = read_text_field(dwg, lt->name);
        if (n == "Continuous" || n == "CONTINUOUS"
            || n == "ByLayer" || n == "BYLAYER"
            || n == "ByBlock" || n == "BYBLOCK") return {};
    }

    std::vector<double> dashes;
    dashes.reserve(lt->numdashes);
    for (BITCODE_RC i = 0; i < lt->numdashes; ++i) {
        dashes.push_back(lt->dashes[i].length);
    }
    // Slab 7c — apply the global header `LTSCALE` and per-entity
    // `ltype_scale`, matching CAD's "linetype scale" convention.
    // Both default to 1.0; non-default values stretch / shrink the
    // pattern in world units.
    double pattern_scale = 1.0;
    if (dwg != nullptr && dwg->header_vars.LTSCALE > 0.0) {
        pattern_scale *= dwg->header_vars.LTSCALE;
    }
    if (ent->ltype_scale > 0.0) {
        pattern_scale *= ent->ltype_scale;
    }
    if (pattern_scale != 1.0) {
        for (auto& d : dashes) d *= pattern_scale;
    }
    // Degenerate (all-zero) pattern would loop forever in the
    // decomposer; treat it as solid.
    double sum = 0.0;
    for (double v : dashes) sum += std::abs(v);
    if (sum < 1e-9) return {};
    return dashes;
}

// Slab 7c — walk an N-vertex polyline (open or closed), decomposing
// it into one `Line` record per dash interval. Continues the dash
// pattern across vertex boundaries so multi-segment polylines
// preserve a continuous dash flow (matches CAD's "PEDIT" behaviour).
//
// `points` are stored in the target coordinate system (already xf-
// transformed for INSERT-expanded geometry); `dashes` is in the same
// distance scale (caller has already applied `xf.scale_factor()` for
// INSERT scaling on top of LTSCALE / ent->ltype_scale).
void decompose_dashed_polyline(std::vector<Point> const& points,
                               bool closed,
                               std::vector<double> const& dashes,
                               Color const& color,
                               std::string const& layer_name,
                               float thickness,
                               Entities& out) {
    if (points.size() < 2) return;
    double pattern_total = 0.0;
    for (double v : dashes) pattern_total += std::abs(v);
    if (pattern_total < 1e-9) {
        // Degenerate pattern — emit the polyline solid.
        for (std::size_t i = 1; i < points.size(); ++i) {
            out.lines.push_back(Line{
                points[i - 1], points[i],
                color, layer_name, thickness});
        }
        if (closed) {
            out.lines.push_back(Line{
                points.back(), points.front(),
                color, layer_name, thickness});
        }
        return;
    }

    double dash_pos = 0.0;        // distance into current dash element
    std::size_t dash_idx = 0;     // index into the dash pattern
    int safety = static_cast<int>(points.size()) * 4096 + 16;

    auto walk = [&](Point const& p0, Point const& p1) {
        double const dx = p1.x - p0.x;
        double const dy = p1.y - p0.y;
        double const seg_len = std::sqrt(dx * dx + dy * dy);
        if (seg_len < 1e-12) return;
        double const inv_len = 1.0 / seg_len;
        double const ux = dx * inv_len;
        double const uy = dy * inv_len;

        double consumed = 0.0;
        Point cursor = p0;
        while (consumed < seg_len && safety-- > 0) {
            double const dash_v = dashes[dash_idx % dashes.size()];
            double const dash_len = std::abs(dash_v);
            if (dash_len < 1e-9) {
                // Dot — visible only with thickness; skip.
                ++dash_idx;
                dash_pos = 0.0;
                continue;
            }
            double const remaining_dash = dash_len - dash_pos;
            double const remaining_seg  = seg_len - consumed;
            double const step = std::min(remaining_dash, remaining_seg);
            Point const next_cursor{
                cursor.x + ux * step, cursor.y + uy * step};
            if (dash_v > 0.0 && step > 1e-12) {
                out.lines.push_back(Line{
                    cursor, next_cursor,
                    color, layer_name, thickness});
            }
            cursor = next_cursor;
            consumed += step;
            dash_pos += step;
            if (dash_pos >= dash_len - 1e-9) {
                dash_pos = 0.0;
                ++dash_idx;
            }
        }
    };

    for (std::size_t i = 1; i < points.size(); ++i) {
        walk(points[i - 1], points[i]);
    }
    if (closed) walk(points.back(), points.front());
}

// Slab 7c — sample a circular arc into a chord polyline. Used as the
// fallback when `Painter::arc` can't render the entity (e.g. when the
// arc carries a dashed linetype and needs to be decomposed into
// individual line segments). 64 samples per `2π` sweep is smooth at
// typical CAD zooms; refining via radius-aware step is a future
// optimisation.
std::vector<Point> sample_arc_polyline(double cx, double cy, double radius,
                                       double start_angle, double end_angle,
                                       Affine const& xf) {
    std::vector<Point> pts;
    double sweep = end_angle - start_angle;
    if (sweep <= 0.0) sweep += kTwoPi;
    if (sweep > kTwoPi) sweep = kTwoPi;
    int const samples =
        std::max(8, static_cast<int>(std::ceil(64.0 * sweep / kTwoPi)));
    pts.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        double const t = start_angle + sweep
            * static_cast<double>(i) / static_cast<double>(samples);
        pts.push_back(xf.apply_point(
            cx + radius * std::cos(t),
            cy + radius * std::sin(t)));
    }
    return pts;
}

// Slab 7c — sample an AutoCAD ELLIPSE arc into a chord polyline.
// Mirrors the renderer's parametric evaluation but emits points
// directly so the linetype decomposer can dash them. Center +
// major_axis are already in the target (xf-transformed) coord
// system; xf is reapplied identity-style to stay symmetric with
// `sample_arc_polyline` (caller passes the same xf used elsewhere
// in the entity case).
std::vector<Point> sample_ellipse_polyline(
        Point center, Point major_axis, double minor_ratio,
        double start_param, double end_param, Affine const& xf) {
    std::vector<Point> pts;
    double t0 = start_param;
    double t1 = end_param;
    if (std::abs(t1 - t0) < 1e-9) t1 = t0 + kTwoPi;
    if (t1 < t0) t1 += kTwoPi;

    double const ux =  major_axis.x;
    double const uy =  major_axis.y;
    double const vx = -uy * minor_ratio;
    double const vy =  ux * minor_ratio;

    double const sweep = t1 - t0;
    int const samples =
        std::max(16, static_cast<int>(std::ceil(64.0 * sweep / kTwoPi)));
    pts.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        double const t = t0 + sweep
            * static_cast<double>(i) / static_cast<double>(samples);
        Point const w{
            center.x + ux * std::cos(t) + vx * std::sin(t),
            center.y + uy * std::cos(t) + vy * std::sin(t),
        };
        pts.push_back(xf.apply_point(w.x, w.y));
    }
    return pts;
}

// Slab 7 — walk a straight LINE under the linetype's dash pattern,
// emitting one `Line` record per dash interval. Gaps and dots are
// skipped (dots could be rendered as tiny length-0 dashes; deferred
// because they have no meaningful pixel size at typical zoom). The
// affine `xf` (INSERT-composed transform) applies to each emitted
// segment so dashes inherit any block scale / rotation.
//
// Pattern length lives in world units — same convention CAD uses —
// so dashes zoom in / out with the drawing. At extreme zoom-out the
// pattern can fall below 1 px and visually merge into a solid line;
// that matches every CAD viewer.
void decompose_dashed_line(Point a_world, Point b_world,
                           std::vector<double> const& dashes,
                           Affine const& xf,
                           Color const& color,
                           std::string const& layer_name,
                           float thickness,
                           Entities& out) {
    double const dx = b_world.x - a_world.x;
    double const dy = b_world.y - a_world.y;
    double const total_len = std::sqrt(dx * dx + dy * dy);
    if (total_len < 1e-9) return;
    double const inv_len = 1.0 / total_len;
    double const nx = dx * inv_len;
    double const ny = dy * inv_len;

    double t = 0.0;
    std::size_t i = 0;
    int safety = static_cast<int>(dashes.size()) * 1024 + 16;
    while (t < total_len && safety-- > 0) {
        double const v = dashes[i % dashes.size()];
        double const seg_len = std::abs(v);
        if (seg_len < 1e-9) {
            // Dot — visible only at thickness scale; skip in this cut.
            ++i;
            continue;
        }
        double const t_end = std::min(t + seg_len, total_len);
        if (v > 0.0 && t_end > t + 1e-12) {
            Point const p0{a_world.x + nx * t,     a_world.y + ny * t};
            Point const p1{a_world.x + nx * t_end, a_world.y + ny * t_end};
            out.lines.push_back(Line{
                xf.apply_point(p0.x, p0.y),
                xf.apply_point(p1.x, p1.y),
                color, layer_name, thickness,
            });
        }
        t = t_end;
        ++i;
    }
}

// Slab 7 — resolve an entity's effective lineweight in canvas pixels.
//
// `linewt` is a 0..31 enum index (NOT the raw 0.01 mm value);
// LibreDWG exposes `dxf_cvt_lweight()` to translate the index into
// either an actual `0.01 mm` lineweight (0..211) or one of the
// BYLAYER (-1) / BYBLOCK (-2) / BYLWDEFAULT (-3) sentinels. Earlier
// builds of this helper treated the raw byte as if it were already
// in mm-x100 — every line ended up at the 1 px floor regardless of
// the file's metadata. The conversion below now goes through the
// LibreDWG helper so files like Autodesk's `lineweights.dwg` show
// the expected hierarchy.
//
// 0.05 px per 0.01 mm with a 1 px floor maps the standard CAD
// weights into a clear visual hierarchy:
//
//   0.13 mm (lw idx  3 → 13)  → 1.0 px
//   0.25 mm (lw idx  7 → 25)  → 1.25 px
//   0.50 mm (lw idx 11 → 50)  → 2.5 px
//   0.70 mm (lw idx 14 → 70)  → 3.5 px
//   1.00 mm (lw idx 17 → 100) → 5.0 px
//
// Lineweight stays pixel-frame, not world-frame — strokes keep a
// constant on-screen weight at any zoom, matching every CAD viewer's
// "lineweight display" mode.
//
// Resolution order: entity own `linewt` → layer `linewt` (BYLAYER /
// BYBLOCK / DEFAULT fallback) → 1 px default.
float resolve_entity_lineweight_px(Dwg_Object_Entity const* ent) {
    constexpr float kDefaultPx        = 1.0f;
    constexpr float kPxPerHundredthMm = 0.05f;

    auto cvt = [&](int raw_byte) -> std::optional<float> {
        // dxf_cvt_lweight masks with `% 32` internally so any byte is
        // safe. Negative return values flag the sentinels — caller
        // then falls through to the next resolution level.
        int const mm100 = dxf_cvt_lweight(
            static_cast<BITCODE_BSd>(raw_byte));
        if (mm100 < 0) return std::nullopt;
        if (mm100 == 0) return std::nullopt;  // 0 mm = use default
        return std::max(
            1.0f, static_cast<float>(mm100) * kPxPerHundredthMm);
    };

    if (ent == nullptr) return kDefaultPx;
    if (auto v = cvt(static_cast<int>(ent->linewt))) return *v;

    Dwg_Object_LAYER* layer = dwg_get_entity_layer(ent);
    if (layer != nullptr) {
        if (auto v = cvt(static_cast<int>(layer->linewt))) return *v;
    }
    return kDefaultPx;
}

// De Boor evaluation of a non-rational B-spline at parameter `t`.
// `ctrl` is the control polygon, `knots` is the knot vector
// (length == ctrl.size() + degree + 1 for a clamped uniform spline).
// Out-of-range knot indices clamp to the vector edges so the
// evaluator stays robust at the boundary parameters
// `t = knots[degree]` and `t = knots[ctrl.size()]`.
//
// Rational (weighted) splines are evaluated as if w == 1. Real CAD
// splines are almost always non-rational; rational support can land
// in a follow-up once a fixture demonstrates the gap.
Point de_boor(double t,
              std::vector<Point> const& ctrl,
              std::vector<double> const& knots,
              int degree) {
    int const n = static_cast<int>(ctrl.size());
    if (n <= 0) return Point{};
    if (degree <= 0) {
        int idx = static_cast<int>(t);
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        return ctrl[idx];
    }

    // Find span k such that knots[k] <= t < knots[k+1]. Linear walk
    // is fine — for typical CAD splines (≤ 50 control points) the
    // cost is dominated by the De Boor recursion below.
    int const last_knot = static_cast<int>(knots.size()) - 1;
    int k = degree;
    while (k < last_knot && knots[k + 1] <= t) ++k;
    if (k >= n) k = n - 1;

    auto knot_at = [&](int idx) -> double {
        if (idx < 0) return knots.front();
        if (idx > last_knot) return knots[last_knot];
        return knots[idx];
    };
    auto ctrl_at = [&](int idx) -> Point {
        if (idx < 0) return ctrl.front();
        if (idx >= n) return ctrl[n - 1];
        return ctrl[idx];
    };

    std::vector<Point> d(degree + 1);
    for (int j = 0; j <= degree; ++j) {
        d[j] = ctrl_at(k - degree + j);
    }
    for (int r = 1; r <= degree; ++r) {
        for (int j = degree; j >= r; --j) {
            double const left  = knot_at(k - degree + j);
            double const right = knot_at(k + 1 + j - r);
            double const denom = right - left;
            double const alpha = denom > 1e-12 ? (t - left) / denom : 0.0;
            d[j].x = (1.0 - alpha) * d[j - 1].x + alpha * d[j].x;
            d[j].y = (1.0 - alpha) * d[j - 1].y + alpha * d[j].y;
        }
    }
    return d[degree];
}

char const* version_string(unsigned int v) {
    switch (static_cast<int>(v)) {
        case R_13:    return "AC1012 (R13)";
        case R_14:    return "AC1014 (R14)";
        case R_2000:  return "AC1015 (R2000)";
        case R_2004:  return "AC1018 (R2004)";
        case R_2007:  return "AC1021 (R2007)";
        case R_2010:  return "AC1024 (R2010)";
        case R_2013:  return "AC1027 (R2013)";
        case R_2018:  return "AC1032 (R2018)";
        default:      return "unknown";
    }
}

// Forward decl — the INSERT and DIMENSION cases recurse into block
// children, which themselves dispatch back through this same switch.
// Recursion is bounded because nested INSERTs eventually terminate at
// a block of leaf primitives, and pathological cycles (an INSERT
// reaching a block that ultimately contains the same INSERT again)
// would only manifest in malformed DWGs.
void extract_entity_xf(Dwg_Data* dwg, Dwg_Object const* obj,
                       Affine const& xf, Entities& out);

// Slab 5 — walk every entity owned by the BLOCK_HEADER referenced by
// `block_ref`, dispatching each child through `extract_entity_xf` so
// the inherited transform applies. Used by both `DWG_TYPE_INSERT`
// (where `xf` is composed with the INSERT's own affine before this
// call) and every `DWG_TYPE_DIMENSION_*` variant (whose `block`
// holds a precomputed `*D###` anonymous block of dimension lines /
// arcs / text and inherits `xf` verbatim — no extra transform).
void expand_block(Dwg_Data* dwg, BITCODE_H block_ref,
                  Affine const& xf, Entities& out) {
    if (block_ref == nullptr) return;
    Dwg_Object* block_obj = dwg_ref_object(dwg, block_ref);
    if (block_obj == nullptr
        || block_obj->supertype != DWG_SUPERTYPE_OBJECT) return;
    auto const* bh = block_obj->tio.object->tio.BLOCK_HEADER;
    if (bh == nullptr || bh->entities == nullptr) return;

    for (BITCODE_BL i = 0; i < bh->num_owned; ++i) {
        BITCODE_H href = bh->entities[i];
        if (href == nullptr) continue;
        Dwg_Object* child = dwg_ref_object(dwg, href);
        if (child == nullptr) continue;
        if (child->supertype != DWG_SUPERTYPE_ENTITY) continue;
        extract_entity_xf(dwg, child, xf, out);
    }
}

void extract_entity_xf(Dwg_Data* dwg, Dwg_Object const* obj,
                       Affine const& xf, Entities& out) {
    auto const fixedtype = static_cast<int>(obj->fixedtype);
    switch (fixedtype) {
        case DWG_TYPE_LINE: {
            auto const* line = obj->tio.entity->tio.LINE;
            if (!line) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            float const thickness =
                resolve_entity_lineweight_px(obj->tio.entity);
            auto dashes = resolve_entity_dashes(dwg, obj->tio.entity);
            if (dashes.empty()) {
                out.lines.push_back(Line{
                    xf.apply_point(line->start.x, line->start.y),
                    xf.apply_point(line->end.x,   line->end.y),
                    meta.color,
                    std::move(meta.layer_name),
                    thickness,
                });
            } else {
                // Slab 7 — pre-decompose the dashed LINE into one
                // `Line` per dash. Renderer stays unchanged.
                decompose_dashed_line(
                    Point{line->start.x, line->start.y},
                    Point{line->end.x,   line->end.y},
                    dashes, xf,
                    meta.color, meta.layer_name, thickness, out);
            }
            ++out.line_count;
            break;
        }
        case DWG_TYPE_CIRCLE: {
            auto const* c = obj->tio.entity->tio.CIRCLE;
            if (!c) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            float const thickness =
                resolve_entity_lineweight_px(obj->tio.entity);
            auto dashes = resolve_entity_dashes(dwg, obj->tio.entity);
            if (dashes.empty()) {
                // Solid — keep the native arc primitive (SDF-rendered
                // by phenotype, zoom-stable smoothness).
                double const rot = xf.rotation();
                out.arcs.push_back(Arc{
                    xf.apply_point(c->center.x, c->center.y),
                    c->radius * xf.scale_factor(),
                    rot, rot + kTwoPi,
                    meta.color,
                    std::move(meta.layer_name),
                    thickness,
                });
            } else {
                // Slab 7c — dashed circle: sample to a polyline and
                // decompose into dashed line segments. Trade-off:
                // loses the SDF arc smoothness when dashed.
                if (xf.scale_factor() != 1.0) {
                    for (auto& d : dashes) d *= xf.scale_factor();
                }
                auto pts = sample_arc_polyline(
                    c->center.x, c->center.y, c->radius,
                    0.0, kTwoPi, xf);
                decompose_dashed_polyline(
                    pts, /*closed=*/true, dashes,
                    meta.color, meta.layer_name, thickness, out);
            }
            ++out.circle_count;
            break;
        }
        case DWG_TYPE_ARC: {
            auto const* a = obj->tio.entity->tio.ARC;
            if (!a) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            float const thickness =
                resolve_entity_lineweight_px(obj->tio.entity);
            auto dashes = resolve_entity_dashes(dwg, obj->tio.entity);
            if (dashes.empty()) {
                double const rot = xf.rotation();
                out.arcs.push_back(Arc{
                    xf.apply_point(a->center.x, a->center.y),
                    a->radius * xf.scale_factor(),
                    a->start_angle + rot, a->end_angle + rot,
                    meta.color,
                    std::move(meta.layer_name),
                    thickness,
                });
            } else {
                if (xf.scale_factor() != 1.0) {
                    for (auto& d : dashes) d *= xf.scale_factor();
                }
                auto pts = sample_arc_polyline(
                    a->center.x, a->center.y, a->radius,
                    a->start_angle, a->end_angle, xf);
                decompose_dashed_polyline(
                    pts, /*closed=*/false, dashes,
                    meta.color, meta.layer_name, thickness, out);
            }
            ++out.arc_count;
            break;
        }
        case DWG_TYPE_TEXT: {
            auto const* t = obj->tio.entity->tio.TEXT;
            if (!t || !t->text_value) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            // DWG TEXT carries two anchor points: `ins_pt` (the
            // baseline-left for the default Left/Baseline alignment)
            // and `alignment_pt` (used when horiz_alignment != 0 or
            // vert_alignment != 0 — the actual anchor depends on the
            // chosen mode). Per LibreDWG, `alignment_pt` is populated
            // when `dataflags & 2`.
            int const h_align = static_cast<int>(t->horiz_alignment);
            int const v_align = static_cast<int>(t->vert_alignment);
            // LibreDWG dataflags bit 1 is INVERTED: clear (= 0) means
            // `alignment_pt` is present in the wire format. See
            // `dwg.spec`: `if (!(dataflags & 0x02)) FIELD_2DD
            // (alignment_pt, ins_pt, 0);`. We use `alignment_pt`
            // whenever it's populated AND the entity declares a non-
            // default H/V alignment — otherwise the default Left /
            // Baseline anchor at `ins_pt` is the correct anchor.
            bool const has_align_pt = (t->dataflags & 0x2) == 0;
            Point const anchor = (has_align_pt
                                  && (h_align != 0 || v_align != 0))
                ? Point{t->alignment_pt.x, t->alignment_pt.y}
                : Point{t->ins_pt.x, t->ins_pt.y};
            // Aligned (3) / Fit (5) need the second alignment point
            // and would shrink the text to fit; until phenotype text
            // measurement is wired in, fall those back to Left so we
            // at least show the text in a reasonable position.
            TextHAlign const ha =
                (h_align == 1) ? TextHAlign::Center :
                (h_align == 2) ? TextHAlign::Right  :
                (h_align == 4) ? TextHAlign::Middle :
                                 TextHAlign::Left;
            TextVAlign const va =
                (v_align == 1) ? TextVAlign::Bottom :
                (v_align == 2) ? TextVAlign::Middle :
                (v_align == 3) ? TextVAlign::Top    :
                                 TextVAlign::Baseline;
            out.texts.push_back(Text{
                xf.apply_point(anchor.x, anchor.y),
                t->height * xf.scale_factor(),
                read_text_field(dwg, t->text_value),
                meta.color,
                std::move(meta.layer_name),
                ha, va,
            });
            ++out.text_count;
            break;
        }
        case DWG_TYPE_MTEXT: {
            auto const* m = obj->tio.entity->tio.MTEXT;
            if (!m || !m->text) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            // MTEXT `attachment` enum picks the corner the
            // ins_pt anchors at (1=top-left, 2=top-centre, 3=top-
            // right, 4=middle-left, 5=middle-centre, 6=middle-right,
            // 7=bottom-left, 8=bottom-centre, 9=bottom-right). Decode
            // into separate H/V anchors so the renderer applies the
            // same offset logic as for plain TEXT.
            int const att = static_cast<int>(m->attachment);
            TextHAlign const ha =
                (att == 2 || att == 5 || att == 8) ? TextHAlign::Center :
                (att == 3 || att == 6 || att == 9) ? TextHAlign::Right  :
                                                     TextHAlign::Left;
            TextVAlign const va =
                (att == 1 || att == 2 || att == 3) ? TextVAlign::Top    :
                (att == 4 || att == 5 || att == 6) ? TextVAlign::Middle :
                                                     TextVAlign::Bottom;
            out.texts.push_back(Text{
                xf.apply_point(m->ins_pt.x, m->ins_pt.y),
                m->text_height * xf.scale_factor(),
                read_text_field(dwg, m->text),
                meta.color,
                std::move(meta.layer_name),
                ha, va,
            });
            ++out.text_count;
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            auto const* p = obj->tio.entity->tio.LWPOLYLINE;
            if (!p || !p->points || p->num_points < 2) {
                ++out.unknown_entities; break;
            }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            float const thickness =
                resolve_entity_lineweight_px(obj->tio.entity);
            Color const color = meta.color;
            std::string const layer_name = meta.layer_name;
            auto const npts = p->num_points;
            bool const closed = (p->flag & 0x1) != 0;

            // Detect any non-zero bulge: those segments need to render
            // as actual circular arcs, not straight chords. The whole
            // polyline routes through `Painter::stroke_path` so the
            // straight + arc segments stay one continuous, correctly-
            // joined entity. Polylines with all-zero bulges keep the
            // legacy flat-line emit path.
            bool any_bulge = false;
            if (p->bulges && p->num_bulges > 0) {
                BITCODE_BL const nb =
                    (p->num_bulges < npts) ? p->num_bulges : npts;
                for (BITCODE_BL i = 0; i < nb; ++i) {
                    if (p->bulges[i] != 0.0) { any_bulge = true; break; }
                }
            }

            auto dashes = resolve_entity_dashes(dwg, obj->tio.entity);
            if (xf.scale_factor() != 1.0 && !dashes.empty()) {
                for (auto& d : dashes) d *= xf.scale_factor();
            }

            if (any_bulge && dashes.empty()) {
                // Solid bulged polyline — keep the native arc-aware
                // path. Dashed bulged polylines fall through to the
                // chord-flatten branch below; arc shape is lost when
                // dashed but the visual still reads as a dashed
                // polyline.
                BulgedPolyline bp{};
                bp.color      = color;
                bp.closed     = closed;
                bp.layer_name = layer_name;
                bp.thickness  = thickness;
                bp.vertices.reserve(npts);
                for (BITCODE_BL i = 0; i < npts; ++i) {
                    bp.vertices.push_back(
                        xf.apply_point(p->points[i].x, p->points[i].y));
                }
                BITCODE_BL const seg_count =
                    closed ? npts : (npts - 1);
                bp.bulges.assign(seg_count, 0.0);
                BITCODE_BL const nb =
                    (p->num_bulges < seg_count) ? p->num_bulges : seg_count;
                for (BITCODE_BL i = 0; i < nb; ++i) {
                    // Bulge value (= tan(θ/4)) is preserved under
                    // similarity (rotate + uniform scale). Non-uniform
                    // INSERT scale would distort the arc to an ellipse
                    // arc — accepted approximation for now.
                    bp.bulges[i] = p->bulges[i];
                }
                out.bulged_polylines.push_back(std::move(bp));
            } else {
                std::vector<Point> verts;
                verts.reserve(npts);
                for (BITCODE_BL i = 0; i < npts; ++i) {
                    verts.push_back(
                        xf.apply_point(p->points[i].x, p->points[i].y));
                }
                if (dashes.empty()) {
                    for (BITCODE_BL i = 1; i < npts; ++i) {
                        out.lines.push_back(Line{
                            verts[i - 1], verts[i],
                            color, layer_name,
                            thickness,
                        });
                    }
                    if (closed) {
                        out.lines.push_back(Line{
                            verts[npts - 1], verts[0],
                            color, layer_name,
                            thickness,
                        });
                    }
                } else {
                    decompose_dashed_polyline(
                        verts, closed, dashes,
                        color, layer_name, thickness, out);
                }
            }
            ++out.polyline_count;
            break;
        }
        case DWG_TYPE_ELLIPSE: {
            auto const* e = obj->tio.entity->tio.ELLIPSE;
            if (!e) { ++out.unknown_entities; break; }
            // LibreDWG `sm_axis` is the vector from `center` to the
            // major-axis endpoint (despite the historical "small axis"
            // name — the field name is a LibreDWG legacy and does NOT
            // refer to the minor axis). `axis_ratio` is the minor /
            // major length ratio (≤ 1). `start_angle` / `end_angle`
            // are parametric, not geometric.
            //
            // Centre transforms via `apply_point`; the major-axis
            // vector via `apply_vector` (it's a vector from centre,
            // not a world position). Minor ratio is preserved under
            // similarity transforms; non-uniform INSERT scale would
            // distort the ellipse — accepted approximation.
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            float const thickness =
                resolve_entity_lineweight_px(obj->tio.entity);
            auto dashes = resolve_entity_dashes(dwg, obj->tio.entity);
            Point const center_w = xf.apply_point(e->center.x, e->center.y);
            Point const major_w  = xf.apply_vector(e->sm_axis.x, e->sm_axis.y);
            if (dashes.empty()) {
                out.ellipses.push_back(Ellipse{
                    center_w, major_w,
                    e->axis_ratio,
                    e->start_angle,
                    e->end_angle,
                    meta.color,
                    std::move(meta.layer_name),
                    thickness,
                });
            } else {
                if (xf.scale_factor() != 1.0) {
                    for (auto& d : dashes) d *= xf.scale_factor();
                }
                // The ellipse evaluator already takes coords in the
                // target frame, so pass the identity transform — the
                // points returned are already in the same coord
                // system as the dash pattern.
                auto pts = sample_ellipse_polyline(
                    center_w, major_w, e->axis_ratio,
                    e->start_angle, e->end_angle, Affine::identity());
                bool const ellipse_closed =
                    std::abs(e->end_angle - e->start_angle) < 1e-9;
                decompose_dashed_polyline(
                    pts, ellipse_closed, dashes,
                    meta.color, meta.layer_name, thickness, out);
            }
            ++out.ellipse_count;
            break;
        }
        case DWG_TYPE_SPLINE: {
            auto const* s = obj->tio.entity->tio.SPLINE;
            if (!s) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            Spline sp{};
            sp.color      = meta.color;
            sp.layer_name = std::move(meta.layer_name);
            sp.thickness  = resolve_entity_lineweight_px(obj->tio.entity);
            // Bit 0 of legacy flag, plus bit 2 of the 2013+ splineflags.
            sp.closed = (s->closed_b != 0)
                        || ((s->splineflags & 0x4) != 0);

            int const degree = s->degree > 0
                ? static_cast<int>(s->degree) : 3;

            // Bezier scenario / files that ship interpolation points
            // (often alongside ctrl_pts) — connect the fit points
            // verbatim. Parser-side fidelity is bounded by what the
            // editor wrote, but it's exactly what the file says the
            // curve passes through.
            bool const have_fit_pts =
                (s->scenario == SPLINE_SCENARIO_BEZIER
                 || s->num_fit_pts > 1)
                && s->fit_pts != nullptr;

            if (have_fit_pts) {
                sp.points.reserve(s->num_fit_pts);
                for (BITCODE_BS i = 0; i < s->num_fit_pts; ++i) {
                    sp.points.push_back(xf.apply_point(
                        s->fit_pts[i].x, s->fit_pts[i].y));
                }
            } else if (s->num_ctrl_pts > 0 && s->ctrl_pts != nullptr
                       && s->num_knots >= static_cast<BITCODE_BL>(
                              s->num_ctrl_pts + degree + 1)
                       && s->knots != nullptr) {
                // De Boor sample at uniform parameter steps in world
                // coords first, then transform — keeps the De Boor
                // evaluator transform-agnostic.
                std::vector<Point> ctrl;
                ctrl.reserve(s->num_ctrl_pts);
                for (BITCODE_BL i = 0; i < s->num_ctrl_pts; ++i) {
                    ctrl.push_back(Point{
                        s->ctrl_pts[i].x, s->ctrl_pts[i].y,
                    });
                }
                std::vector<double> knots;
                knots.reserve(s->num_knots);
                for (BITCODE_BL i = 0; i < s->num_knots; ++i) {
                    knots.push_back(s->knots[i]);
                }

                double const t_min = knots[degree];
                double const t_max = knots[s->num_ctrl_pts];
                if (t_max > t_min) {
                    int const samples = std::max<int>(
                        64, static_cast<int>(s->num_ctrl_pts) * 8);
                    sp.points.reserve(samples + 1);
                    for (int i = 0; i <= samples; ++i) {
                        double t = t_min
                            + (t_max - t_min)
                                * static_cast<double>(i)
                                / static_cast<double>(samples);
                        if (i == samples) t = t_max - 1e-9;
                        Point const w = de_boor(t, ctrl, knots, degree);
                        sp.points.push_back(xf.apply_point(w.x, w.y));
                    }
                }
            }

            if (sp.points.size() >= 2) {
                auto dashes = resolve_entity_dashes(dwg, obj->tio.entity);
                if (dashes.empty()) {
                    out.splines.push_back(std::move(sp));
                } else {
                    if (xf.scale_factor() != 1.0) {
                        for (auto& d : dashes) d *= xf.scale_factor();
                    }
                    decompose_dashed_polyline(
                        sp.points, sp.closed, dashes,
                        sp.color, sp.layer_name, sp.thickness, out);
                }
                ++out.spline_count;
            } else {
                ++out.unknown_entities;
            }
            break;
        }
        case DWG_TYPE_INSERT: {
            auto const* ins = obj->tio.entity->tio.INSERT;
            if (!ins) { ++out.unknown_entities; break; }
            // Build the INSERT's local affine — translate(ins_pt) ∘
            // rotate(rotation) ∘ scale(scale.x, scale.y), in apply-
            // order. Compose with the inherited `xf` so nested
            // INSERTs accumulate correctly.
            Affine const local =
                Affine::translate(ins->ins_pt.x, ins->ins_pt.y)
                .compose(Affine::rotate(ins->rotation))
                .compose(Affine::scale_xy(ins->scale.x, ins->scale.y));
            Affine const child_xf = xf.compose(local);
            expand_block(dwg, ins->block_header, child_xf, out);
            ++out.insert_count;
            break;
        }
        case DWG_TYPE_MINSERT: {
            // MINSERT — rectangular array INSERT. Same block, replicated
            // num_cols × num_rows times with `(col_idx × col_spacing,
            // row_idx × row_spacing)` offsets in the block's *pre-
            // rotation, pre-scale* (block-local) frame, so the array
            // axes follow the MINSERT's own rotation. Each cell expands
            // the block under its own composed affine.
            auto const* mins = obj->tio.entity->tio.MINSERT;
            if (!mins) { ++out.unknown_entities; break; }
            int const cols = static_cast<int>(mins->num_cols);
            int const rows = static_cast<int>(mins->num_rows);
            if (cols <= 0 || rows <= 0) {
                ++out.unknown_entities; break;
            }
            Affine const local =
                Affine::translate(mins->ins_pt.x, mins->ins_pt.y)
                .compose(Affine::rotate(mins->rotation))
                .compose(Affine::scale_xy(mins->scale.x, mins->scale.y));
            Affine const base_xf = xf.compose(local);
            for (int row = 0; row < rows; ++row) {
                for (int col = 0; col < cols; ++col) {
                    Affine const cell_offset = Affine::translate(
                        static_cast<double>(col) * mins->col_spacing,
                        static_cast<double>(row) * mins->row_spacing);
                    Affine const cell_xf = base_xf.compose(cell_offset);
                    expand_block(dwg, mins->block_header, cell_xf, out);
                }
            }
            ++out.minsert_count;
            break;
        }
        case DWG_TYPE_DIMENSION_ORDINATE:
        case DWG_TYPE_DIMENSION_LINEAR:
        case DWG_TYPE_DIMENSION_ALIGNED:
        case DWG_TYPE_DIMENSION_ANG3PT:
        case DWG_TYPE_DIMENSION_ANG2LN:
        case DWG_TYPE_DIMENSION_RADIUS:
        case DWG_TYPE_DIMENSION_DIAMETER: {
            // Every DIMENSION_* variant carries a precomputed `*D###`
            // anonymous block whose entities (lines / arcs / text +
            // arrowheads) are already in world coordinates. Expand the
            // block under the inherited `xf` — that's identity at top
            // level, INSERT-composed when this DIMENSION lives inside
            // a block.
            //
            // Every variant shares the `block` field through the
            // DIMENSION_COMMON macro, so reading it through any one of
            // the typed views works.
            auto const* d = obj->tio.entity->tio.DIMENSION_LINEAR;
            if (d == nullptr) { ++out.unknown_entities; break; }
            expand_block(dwg, d->block, xf, out);
            ++out.dimension_count;
            break;
        }
        case DWG_TYPE_HATCH: {
            auto const* h = obj->tio.entity->tio.HATCH;
            if (!h || h->num_paths == 0 || h->paths == nullptr) {
                ++out.unknown_entities; break;
            }
            auto meta = resolve_entity_metadata(dwg, obj->tio.entity);
            Hatch hatch{};
            hatch.color      = meta.color;
            hatch.layer_name = std::move(meta.layer_name);
            hatch.solid      = h->is_solid_fill != 0;

            // Discretisation count for arc / bulge sweeps. 32 chords
            // is smooth at typical CAD HATCH scales (room interiors,
            // detail dashes); refining via radius-aware step is a
            // future optimisation.
            constexpr int kArcSegments = 32;

            for (BITCODE_BL pi = 0; pi < h->num_paths; ++pi) {
                auto const& path = h->paths[pi];
                std::vector<Point> loop;

                bool const is_polyline = (path.flag & 0x2) != 0;
                if (is_polyline && path.polyline_paths != nullptr) {
                    BITCODE_BL const n = path.num_segs_or_paths;
                    if (n < 2) continue;
                    for (BITCODE_BL i = 0; i < n; ++i) {
                        auto const& v = path.polyline_paths[i];
                        Point const a = xf.apply_point(v.point.x, v.point.y);
                        if (i == 0) {
                            loop.push_back(a);
                            continue;
                        }
                        // Bulge encodes a circular arc between this
                        // vertex and the previous one (`bulge =
                        // tan(θ / 4)`, sign = sweep direction). For
                        // first-cut HATCH we discretise non-zero
                        // bulges into chord polylines and treat
                        // zero-bulge segments as straight chords.
                        auto const& prev_v = path.polyline_paths[i - 1];
                        double const bulge = h->paths[pi].bulges_present
                            ? prev_v.bulge : 0.0;
                        if (bulge == 0.0) {
                            loop.push_back(a);
                        } else {
                            // Compute world-frame arc, then sample.
                            double const dx = v.point.x - prev_v.point.x;
                            double const dy = v.point.y - prev_v.point.y;
                            double const chord = std::sqrt(dx * dx + dy * dy);
                            if (chord < 1e-9) {
                                loop.push_back(a);
                                continue;
                            }
                            double const abs_b = std::abs(bulge);
                            double const radius =
                                chord * (1.0 + bulge * bulge) / (4.0 * abs_b);
                            double const k =
                                (1.0 - bulge * bulge) / (4.0 * bulge);
                            double const mx = 0.5 * (prev_v.point.x + v.point.x);
                            double const my = 0.5 * (prev_v.point.y + v.point.y);
                            double const cx = mx - dy * k;
                            double const cy = my + dx * k;
                            double sa = std::atan2(prev_v.point.y - cy,
                                                   prev_v.point.x - cx);
                            double ea = std::atan2(v.point.y - cy,
                                                   v.point.x - cx);
                            if (bulge < 0.0) std::swap(sa, ea);
                            double sweep = ea - sa;
                            if (sweep < 0.0) sweep += 2.0 * kPi;
                            for (int s = 1; s <= kArcSegments; ++s) {
                                double const t = sa + sweep
                                    * static_cast<double>(s)
                                    / static_cast<double>(kArcSegments);
                                Point const p = xf.apply_point(
                                    cx + radius * std::cos(t),
                                    cy + radius * std::sin(t));
                                loop.push_back(p);
                            }
                            // ArcTo end-point already included as the
                            // loop's last sample (s == kArcSegments
                            // hits exactly `(v.point.x, v.point.y)`
                            // up to floating-point error), so no
                            // explicit `loop.push_back(a)` needed.
                        }
                    }
                } else if (path.segs != nullptr) {
                    BITCODE_BL const n = path.num_segs_or_paths;
                    for (BITCODE_BL i = 0; i < n; ++i) {
                        auto const& seg = path.segs[i];
                        // curve_type: 1=LINE, 2=CIRCULAR ARC,
                        // 3=ELLIPTICAL ARC, 4=SPLINE.
                        if (seg.curve_type == 1) {
                            Point const a = xf.apply_point(
                                seg.first_endpoint.x, seg.first_endpoint.y);
                            if (loop.empty()) loop.push_back(a);
                            Point const b = xf.apply_point(
                                seg.second_endpoint.x,
                                seg.second_endpoint.y);
                            loop.push_back(b);
                        } else if (seg.curve_type == 2) {
                            double sa = seg.start_angle;
                            double ea = seg.end_angle;
                            if (!seg.is_ccw) std::swap(sa, ea);
                            double sweep = ea - sa;
                            if (sweep < 0.0) sweep += 2.0 * kPi;
                            // Seed the loop with the arc's start
                            // point so the prior seg's end (if any)
                            // joins cleanly via the LineTo emit when
                            // we walk the polyline at render time.
                            if (loop.empty()) {
                                Point const start = xf.apply_point(
                                    seg.center.x + seg.radius * std::cos(sa),
                                    seg.center.y + seg.radius * std::sin(sa));
                                loop.push_back(start);
                            }
                            for (int s = 1; s <= kArcSegments; ++s) {
                                double const t = sa + sweep
                                    * static_cast<double>(s)
                                    / static_cast<double>(kArcSegments);
                                Point const p = xf.apply_point(
                                    seg.center.x + seg.radius * std::cos(t),
                                    seg.center.y + seg.radius * std::sin(t));
                                loop.push_back(p);
                            }
                        }
                        // ELLIPTICAL ARC (3) and SPLINE (4) edge
                        // segments are out of scope; they are rare
                        // in HATCH boundaries and would degenerate
                        // to a chord at first cut. Skip them — the
                        // loop may end up as a coarse approximation
                        // but won't crash.
                    }
                }

                if (loop.size() >= 3) {
                    hatch.loops.push_back(std::move(loop));
                }
            }

            if (!hatch.loops.empty()) {
                out.hatches.push_back(std::move(hatch));
                ++out.hatch_count;
            } else {
                ++out.unknown_entities;
            }
            break;
        }
        default:
            ++out.unknown_entities;
            break;
    }
}

void extract(Dwg_Data* dwg, Dwg_Object const* obj, Entities& out) {
    if (!obj) return;

    // LAYER table entries arrive as DWG objects (not entities) — capture
    // them so the panel UI can list / toggle them and so the renderer
    // can already see the resolved colour on each entity that defers to
    // BYLAYER.
    if (obj->supertype == DWG_SUPERTYPE_OBJECT
        && static_cast<int>(obj->fixedtype) == DWG_TYPE_LAYER) {
        auto const* l = obj->tio.object->tio.LAYER;
        if (l != nullptr && l->name != nullptr) {
            out.layers.push_back(Layer{
                read_text_field(dwg, l->name),
                color_from_cmc(l->color),
                l->frozen != 0,
                l->off    != 0,
            });
        }
        return;
    }

    // Slab 7 — LTYPE table entries (DWG_SUPERTYPE_OBJECT). Captured so
    // the summary card can show how many linetypes the file ships;
    // the LINE-side dashing in `extract_entity_xf` resolves directly
    // through `dwg_ref_object` for accuracy, not through this list.
    if (obj->supertype == DWG_SUPERTYPE_OBJECT
        && static_cast<int>(obj->fixedtype) == DWG_TYPE_LTYPE) {
        auto const* lt = obj->tio.object->tio.LTYPE;
        if (lt != nullptr && lt->name != nullptr) {
            Linetype line_type{};
            line_type.name = read_text_field(dwg, lt->name);
            if (lt->dashes != nullptr) {
                line_type.dashes.reserve(lt->numdashes);
                for (BITCODE_RC i = 0; i < lt->numdashes; ++i) {
                    line_type.dashes.push_back(lt->dashes[i].length);
                }
            }
            out.linetypes.push_back(std::move(line_type));
            ++out.linetype_count;
        }
        return;
    }

    if (obj->supertype != DWG_SUPERTYPE_ENTITY) return;

    // Slab 5 — Model-Space filter. Block-owned entities (entmode == 3)
    // would otherwise render at their block-local coordinates,
    // independent of any INSERT. They reach the renderer only via the
    // INSERT case in `extract_entity_xf`, which composes the block's
    // transform. Paper-Space (entmode == 1) is skipped for now —
    // cad++ doesn't expose the plot view yet.
    if (obj->tio.entity->entmode != 2 /* MSPACE */) return;

    extract_entity_xf(dwg, obj, Affine::identity(), out);
}

} // namespace

Entities parse_file(std::string_view path) {
    Entities entities;
    Dwg_Data dwg{};

    std::string path_owned(path);
    int err = dwg_read_file(path_owned.c_str(), &dwg);
    if (err >= DWG_ERR_CRITICAL) {
        entities.error = "dwg_read_file failed: code=" + std::to_string(err);
        dwg_free(&dwg);
        return entities;
    }

    entities.version = version_string(static_cast<unsigned int>(dwg.header.version));

    auto const num_objects = dwg.num_objects;
    for (BITCODE_BL i = 0; i < num_objects; ++i) {
        extract(&dwg, &dwg.object[i], entities);
    }

    dwg_free(&dwg);
    entities.ok = true;
    return entities;
}

} // namespace cadpp
