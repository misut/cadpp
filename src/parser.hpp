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

// M3 deliverable: parse a .dwg file into a flat per-type entity list.
// Subsequent milestones add Circle, Arc, Polyline, Text. Per-entity
// styling (color, layer, weight) is intentionally skipped at this
// stage — every line renders black at 1 px in the viewer.
struct Entities {
    bool ok = false;
    std::string error;             // populated when ok == false
    std::string version;           // e.g. "AC1015 (R2000)"
    std::vector<Line> lines;
    // unknown_entities: count of DWG_SUPERTYPE_ENTITY records that
    // the parser does not (yet) extract — surfaces what's being lost.
    unsigned int unknown_entities = 0;
};

Entities parse_file(std::string_view path);

} // namespace cadpp
