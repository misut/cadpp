// cad++ — DWG parser facade.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Consumers of this header import phenotype, which uses `import std;`.
// Mixing `import std` with `#include <vector>` blows up libc++'s
// abi_tag check, so std types are pulled in via the same import.
import std;

namespace cadpp {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Line {
    Point a;
    Point b;
};

// M4 deliverable: parse a .dwg file into a flat segment list.
// CIRCLE / ARC / LWPOLYLINE are tessellated to chords at parse time
// so the renderer only ever sees `Line`. POLYLINE_2D / POLYLINE_3D
// (which carry vertices via dynamic handle lists) and per-entity
// styling (color, layer, weight, bulge) are deferred. Every line
// still renders black at 1 px in the viewer.
struct Entities {
    bool ok = false;
    std::string error;             // populated when ok == false
    std::string version;           // e.g. "AC1015 (R2000)"
    std::vector<Line> lines;       // post-tessellation segment list

    // Source-entity counts (before tessellation) so the summary card
    // can show what kinds of geometry came in.
    unsigned int line_count = 0;
    unsigned int circle_count = 0;
    unsigned int arc_count = 0;
    unsigned int polyline_count = 0;

    // unknown_entities: count of DWG_SUPERTYPE_ENTITY records that
    // the parser does not (yet) extract — surfaces what's being lost.
    unsigned int unknown_entities = 0;
};

Entities parse_file(std::string_view path);

} // namespace cadpp
