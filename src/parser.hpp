// cad++ — DWG parser facade.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace cadpp {

// M2 deliverable: parse a .dwg file and report a one-line entity
// breakdown. Subsequent milestones replace this with a real entity
// list (lines, circles, arcs, polylines, text), positions, and
// per-layer metadata.
struct EntityCounts {
    bool ok = false;
    std::string error;             // populated when ok == false
    std::string version;           // e.g. "AC1015 (R2000)"
    unsigned int lines = 0;
    unsigned int circles = 0;
    unsigned int arcs = 0;
    unsigned int polylines = 0;    // POLYLINE_2D + LWPOLYLINE
    unsigned int text = 0;         // TEXT + MTEXT
    unsigned int other = 0;        // any entity opcode not above
};

EntityCounts parse_file(std::string_view path);

} // namespace cadpp
