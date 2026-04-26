// cad++ — DWG parser facade implementation.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "parser.hpp"

extern "C" {
#include <dwg.h>
#include <dwg_api.h>
}

namespace cadpp {

namespace {

constexpr int    kCircleSegments = 64;
constexpr double kPi             = 3.14159265358979323846;
constexpr double kTwoPi          = 2.0 * kPi;

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

// Sample the arc from (cx, cy, r) over [start_angle, end_angle] (in
// radians, CCW per AutoCAD convention) into ~kCircleSegments segments
// per full revolution. Number of segments scales with the arc span
// so a small arc gets a proportionally small number of chords.
void tessellate_arc(Entities& out, double cx, double cy, double r,
                    double start_angle, double end_angle) {
    double span = end_angle - start_angle;
    if (span <= 0.0) span += kTwoPi;          // AutoCAD wraps modulo 2π
    if (span > kTwoPi) span = kTwoPi;
    int n = static_cast<int>(
        std::ceil(static_cast<double>(kCircleSegments) * span / kTwoPi));
    if (n < 1) n = 1;
    double prev_x = cx + r * std::cos(start_angle);
    double prev_y = cy + r * std::sin(start_angle);
    for (int i = 1; i <= n; ++i) {
        double t  = start_angle + span * static_cast<double>(i)
                                       / static_cast<double>(n);
        double nx = cx + r * std::cos(t);
        double ny = cy + r * std::sin(t);
        out.lines.push_back(Line{Point{prev_x, prev_y}, Point{nx, ny}});
        prev_x = nx;
        prev_y = ny;
    }
}

void extract(Dwg_Object const* obj, Entities& out) {
    if (!obj) return;
    if (obj->supertype != DWG_SUPERTYPE_ENTITY) return;

    auto const fixedtype = static_cast<int>(obj->fixedtype);
    switch (fixedtype) {
        case DWG_TYPE_LINE: {
            auto const* line = obj->tio.entity->tio.LINE;
            if (!line) { ++out.unknown_entities; break; }
            out.lines.push_back(Line{
                Point{line->start.x, line->start.y},
                Point{line->end.x,   line->end.y},
            });
            ++out.line_count;
            break;
        }
        case DWG_TYPE_CIRCLE: {
            auto const* c = obj->tio.entity->tio.CIRCLE;
            if (!c) { ++out.unknown_entities; break; }
            tessellate_arc(out, c->center.x, c->center.y, c->radius,
                           0.0, kTwoPi);
            ++out.circle_count;
            break;
        }
        case DWG_TYPE_ARC: {
            auto const* a = obj->tio.entity->tio.ARC;
            if (!a) { ++out.unknown_entities; break; }
            tessellate_arc(out, a->center.x, a->center.y, a->radius,
                           a->start_angle, a->end_angle);
            ++out.arc_count;
            break;
        }
        case DWG_TYPE_TEXT: {
            auto const* t = obj->tio.entity->tio.TEXT;
            if (!t || !t->text_value) { ++out.unknown_entities; break; }
            out.texts.push_back(Text{
                Point{t->ins_pt.x, t->ins_pt.y},
                t->height,
                std::string(t->text_value),
            });
            ++out.text_count;
            break;
        }
        case DWG_TYPE_MTEXT: {
            auto const* m = obj->tio.entity->tio.MTEXT;
            if (!m || !m->text) { ++out.unknown_entities; break; }
            out.texts.push_back(Text{
                Point{m->ins_pt.x, m->ins_pt.y},
                m->text_height,
                std::string(m->text),
            });
            ++out.text_count;
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            auto const* p = obj->tio.entity->tio.LWPOLYLINE;
            if (!p || !p->points || p->num_points < 2) {
                ++out.unknown_entities; break;
            }
            auto const npts = p->num_points;
            for (BITCODE_BL i = 1; i < npts; ++i) {
                auto const& a = p->points[i - 1];
                auto const& b = p->points[i];
                out.lines.push_back(Line{Point{a.x, a.y}, Point{b.x, b.y}});
            }
            // flag bit 0 (= LWPLINE_CLOSED) → close the loop with a
            // segment from the last vertex back to the first.
            if ((p->flag & 0x1) != 0) {
                auto const& a = p->points[npts - 1];
                auto const& b = p->points[0];
                out.lines.push_back(Line{Point{a.x, a.y}, Point{b.x, b.y}});
            }
            ++out.polyline_count;
            // Bulged segments (`p->bulges`, signed-tangent arcs between
            // vertices) are treated as straight chords here; M5+ can
            // sample them properly once the hit list of common DWG
            // curves is fleshed out.
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
