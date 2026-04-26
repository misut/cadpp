// cad++ — DWG parser facade implementation.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "parser.hpp"

extern "C" {
#include <dwg.h>
#include <dwg_api.h>
}

#include <cstring>
#include <string>

namespace cadpp {

namespace {

char const* version_string(unsigned int v) {
    // dwg.h defines the R_<n> constants for the per-AutoCAD-release
    // dialects libredwg supports. Map the most common ones to a human
    // label; fall through to a generic tag otherwise.
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

void tally(Dwg_Object const* obj, EntityCounts& out) {
    if (!obj) return;
    auto const fixedtype = static_cast<int>(obj->fixedtype);
    switch (fixedtype) {
        case DWG_TYPE_LINE:
            ++out.lines; break;
        case DWG_TYPE_CIRCLE:
            ++out.circles; break;
        case DWG_TYPE_ARC:
            ++out.arcs; break;
        case DWG_TYPE_LWPOLYLINE:
        case DWG_TYPE_POLYLINE_2D:
        case DWG_TYPE_POLYLINE_3D:
            ++out.polylines; break;
        case DWG_TYPE_TEXT:
        case DWG_TYPE_MTEXT:
            ++out.text; break;
        default:
            // Skip non-entity objects (BLOCK_HEADER, LAYER, dictionary
            // entries, etc.). We tally unrecognised entity types so the
            // M2 readout shows whether interesting geometry is being
            // missed.
            if (obj->supertype == DWG_SUPERTYPE_ENTITY)
                ++out.other;
            break;
    }
}

} // namespace

EntityCounts parse_file(std::string_view path) {
    EntityCounts counts;
    Dwg_Data dwg{};

    std::string path_owned(path);
    int err = dwg_read_file(path_owned.c_str(), &dwg);
    if (err >= DWG_ERR_CRITICAL) {
        counts.error = "dwg_read_file failed: code=" + std::to_string(err);
        dwg_free(&dwg);
        return counts;
    }

    counts.version = version_string(static_cast<unsigned int>(dwg.header.version));

    auto const num_objects = dwg.num_objects;
    for (BITCODE_BL i = 0; i < num_objects; ++i) {
        tally(&dwg.object[i], counts);
    }

    dwg_free(&dwg);
    counts.ok = true;
    return counts;
}

} // namespace cadpp
