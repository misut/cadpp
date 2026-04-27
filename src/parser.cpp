// cad++ — DWG parser facade implementation.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "parser.hpp"

extern "C" {
#include <dwg.h>
#include <dwg_api.h>
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

EntityMetadata resolve_entity_metadata(Dwg_Object_Entity const* ent) {
    EntityMetadata out{};
    if (ent == nullptr) return out;

    Dwg_Object_LAYER* layer = dwg_get_entity_layer(ent);
    if (layer != nullptr && layer->name != nullptr) {
        out.layer_name = layer->name;
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

void extract(Dwg_Object const* obj, Entities& out) {
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
                std::string(l->name),
                color_from_cmc(l->color),
                l->frozen != 0,
                l->off    != 0,
            });
        }
        return;
    }

    if (obj->supertype != DWG_SUPERTYPE_ENTITY) return;

    auto const fixedtype = static_cast<int>(obj->fixedtype);
    switch (fixedtype) {
        case DWG_TYPE_LINE: {
            auto const* line = obj->tio.entity->tio.LINE;
            if (!line) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(obj->tio.entity);
            out.lines.push_back(Line{
                Point{line->start.x, line->start.y},
                Point{line->end.x,   line->end.y},
                meta.color,
                std::move(meta.layer_name),
            });
            ++out.line_count;
            break;
        }
        case DWG_TYPE_CIRCLE: {
            auto const* c = obj->tio.entity->tio.CIRCLE;
            if (!c) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(obj->tio.entity);
            out.arcs.push_back(Arc{
                Point{c->center.x, c->center.y},
                c->radius,
                0.0, kTwoPi,
                meta.color,
                std::move(meta.layer_name),
            });
            ++out.circle_count;
            break;
        }
        case DWG_TYPE_ARC: {
            auto const* a = obj->tio.entity->tio.ARC;
            if (!a) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(obj->tio.entity);
            out.arcs.push_back(Arc{
                Point{a->center.x, a->center.y},
                a->radius,
                a->start_angle, a->end_angle,
                meta.color,
                std::move(meta.layer_name),
            });
            ++out.arc_count;
            break;
        }
        case DWG_TYPE_TEXT: {
            auto const* t = obj->tio.entity->tio.TEXT;
            if (!t || !t->text_value) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(obj->tio.entity);
            out.texts.push_back(Text{
                Point{t->ins_pt.x, t->ins_pt.y},
                t->height,
                std::string(t->text_value),
                meta.color,
                std::move(meta.layer_name),
            });
            ++out.text_count;
            break;
        }
        case DWG_TYPE_MTEXT: {
            auto const* m = obj->tio.entity->tio.MTEXT;
            if (!m || !m->text) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(obj->tio.entity);
            out.texts.push_back(Text{
                Point{m->ins_pt.x, m->ins_pt.y},
                m->text_height,
                std::string(m->text),
                meta.color,
                std::move(meta.layer_name),
            });
            ++out.text_count;
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            auto const* p = obj->tio.entity->tio.LWPOLYLINE;
            if (!p || !p->points || p->num_points < 2) {
                ++out.unknown_entities; break;
            }
            auto meta = resolve_entity_metadata(obj->tio.entity);
            Color const color = meta.color;
            std::string const layer_name = meta.layer_name;
            auto const npts = p->num_points;
            bool const closed = (p->flag & 0x1) != 0;

            // Detect any non-zero bulge: those segments need to render
            // as actual circular arcs, not straight chords. The whole
            // polyline routes through `Painter::stroke_path` so the
            // straight + arc segments stay one continuous, correctly-
            // joined entity. Polylines with all-zero bulges keep the
            // legacy flat-line emit path — same wire output as before
            // Slab 2.c.
            bool any_bulge = false;
            if (p->bulges && p->num_bulges > 0) {
                BITCODE_BL const nb =
                    (p->num_bulges < npts) ? p->num_bulges : npts;
                for (BITCODE_BL i = 0; i < nb; ++i) {
                    if (p->bulges[i] != 0.0) { any_bulge = true; break; }
                }
            }

            if (any_bulge) {
                BulgedPolyline bp{};
                bp.color      = color;
                bp.closed     = closed;
                bp.layer_name = layer_name;
                bp.vertices.reserve(npts);
                for (BITCODE_BL i = 0; i < npts; ++i) {
                    bp.vertices.push_back(Point{p->points[i].x,
                                                p->points[i].y});
                }
                BITCODE_BL const seg_count =
                    closed ? npts : (npts - 1);
                bp.bulges.assign(seg_count, 0.0);
                BITCODE_BL const nb =
                    (p->num_bulges < seg_count) ? p->num_bulges : seg_count;
                for (BITCODE_BL i = 0; i < nb; ++i) {
                    bp.bulges[i] = p->bulges[i];
                }
                out.bulged_polylines.push_back(std::move(bp));
            } else {
                for (BITCODE_BL i = 1; i < npts; ++i) {
                    auto const& a = p->points[i - 1];
                    auto const& b = p->points[i];
                    out.lines.push_back(Line{
                        Point{a.x, a.y}, Point{b.x, b.y},
                        color, layer_name,
                    });
                }
                if (closed) {
                    auto const& a = p->points[npts - 1];
                    auto const& b = p->points[0];
                    out.lines.push_back(Line{
                        Point{a.x, a.y}, Point{b.x, b.y},
                        color, layer_name,
                    });
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
            auto meta = resolve_entity_metadata(obj->tio.entity);
            out.ellipses.push_back(Ellipse{
                Point{e->center.x, e->center.y},
                Point{e->sm_axis.x, e->sm_axis.y},
                e->axis_ratio,
                e->start_angle,
                e->end_angle,
                meta.color,
                std::move(meta.layer_name),
            });
            ++out.ellipse_count;
            break;
        }
        case DWG_TYPE_SPLINE: {
            auto const* s = obj->tio.entity->tio.SPLINE;
            if (!s) { ++out.unknown_entities; break; }
            auto meta = resolve_entity_metadata(obj->tio.entity);
            Spline sp{};
            sp.color      = meta.color;
            sp.layer_name = std::move(meta.layer_name);
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
                    sp.points.push_back(
                        Point{s->fit_pts[i].x, s->fit_pts[i].y});
                }
            } else if (s->num_ctrl_pts > 0 && s->ctrl_pts != nullptr
                       && s->num_knots >= static_cast<BITCODE_BL>(
                              s->num_ctrl_pts + degree + 1)
                       && s->knots != nullptr) {
                // De Boor sample at uniform parameter steps. The 8-per-
                // ctrl-pt heuristic (floored at 64) gives smooth output
                // for typical CAD splines without going overboard on
                // long control polygons.
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
                        // Nudge the right boundary inside the last
                        // knot span so the De Boor span search can't
                        // overshoot.
                        if (i == samples) t = t_max - 1e-9;
                        sp.points.push_back(
                            de_boor(t, ctrl, knots, degree));
                    }
                }
            }

            if (sp.points.size() >= 2) {
                out.splines.push_back(std::move(sp));
                ++out.spline_count;
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
        extract(&dwg.object[i], entities);
    }

    dwg_free(&dwg);
    entities.ok = true;
    return entities;
}

} // namespace cadpp
