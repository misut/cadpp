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

// Translate a LibreDWG entity color into cad++'s RGBA. Returns the
// default ink (Color{}) for BYLAYER / BYBLOCK / unknown methods —
// cad++ has no layer model yet (Slab 4), so any layer-relative color
// stays indistinguishable from the legacy flat-ink rendering.
//
// AutoCAD's ACI 7 ("white-on-dark, black-on-light") gets the same
// fall-back: rendering ACI-7 lines as white on cad++'s light canvas
// would make them invisible. Matches what every CAD viewer ships.
Color resolve_color(Dwg_Object_Entity const* ent) {
    if (ent == nullptr) return Color{};
    auto const& c = ent->color;
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
    int const idx = static_cast<int>(c.index);
    if (method != kColorMethodAci || idx <= 0 || idx == 7 || idx >= 256) {
        return Color{};
    }
    BITCODE_BL const packed = dwg_rgb_palette_index(static_cast<BITCODE_BS>(idx));
    return Color{
        static_cast<std::uint8_t>((packed >> 16) & 0xff),
        static_cast<std::uint8_t>((packed >>  8) & 0xff),
        static_cast<std::uint8_t>( packed        & 0xff),
        255,
    };
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
    if (obj->supertype != DWG_SUPERTYPE_ENTITY) return;

    auto const fixedtype = static_cast<int>(obj->fixedtype);
    switch (fixedtype) {
        case DWG_TYPE_LINE: {
            auto const* line = obj->tio.entity->tio.LINE;
            if (!line) { ++out.unknown_entities; break; }
            Color const color = resolve_color(obj->tio.entity);
            out.lines.push_back(Line{
                Point{line->start.x, line->start.y},
                Point{line->end.x,   line->end.y},
                color,
            });
            ++out.line_count;
            break;
        }
        case DWG_TYPE_CIRCLE: {
            auto const* c = obj->tio.entity->tio.CIRCLE;
            if (!c) { ++out.unknown_entities; break; }
            out.arcs.push_back(Arc{
                Point{c->center.x, c->center.y},
                c->radius,
                0.0, kTwoPi,
                resolve_color(obj->tio.entity),
            });
            ++out.circle_count;
            break;
        }
        case DWG_TYPE_ARC: {
            auto const* a = obj->tio.entity->tio.ARC;
            if (!a) { ++out.unknown_entities; break; }
            out.arcs.push_back(Arc{
                Point{a->center.x, a->center.y},
                a->radius,
                a->start_angle, a->end_angle,
                resolve_color(obj->tio.entity),
            });
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
                resolve_color(obj->tio.entity),
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
                resolve_color(obj->tio.entity),
            });
            ++out.text_count;
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            auto const* p = obj->tio.entity->tio.LWPOLYLINE;
            if (!p || !p->points || p->num_points < 2) {
                ++out.unknown_entities; break;
            }
            Color const color = resolve_color(obj->tio.entity);
            auto const npts = p->num_points;
            for (BITCODE_BL i = 1; i < npts; ++i) {
                auto const& a = p->points[i - 1];
                auto const& b = p->points[i];
                out.lines.push_back(Line{Point{a.x, a.y}, Point{b.x, b.y}, color});
            }
            // flag bit 0 (= LWPLINE_CLOSED) → close the loop with a
            // segment from the last vertex back to the first.
            if ((p->flag & 0x1) != 0) {
                auto const& a = p->points[npts - 1];
                auto const& b = p->points[0];
                out.lines.push_back(Line{Point{a.x, a.y}, Point{b.x, b.y}, color});
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
