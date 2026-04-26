// cad++ — DWG parser facade implementation.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "parser.hpp"

extern "C" {
#include <dwg.h>
#include <dwg_api.h>
}

namespace cadpp {

namespace {

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
            out.lines.push_back(Line{
                Point{line->start.x, line->start.y},
                Point{line->end.x,   line->end.y},
            });
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
