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

// 8-bit-per-channel RGBA. Defaults to the pre-Slab-3 near-black ink so
// any entity whose color resolves to BYLAYER / BYBLOCK / unknown stays
// indistinguishable from the legacy "all entities flat-black" rendering.
struct Color {
    std::uint8_t r = 26;
    std::uint8_t g = 26;
    std::uint8_t b = 26;
    std::uint8_t a = 255;
};

struct Line {
    Point a;
    Point b;
    Color color{};
};

struct Text {
    Point position;        // CAD coords (insertion point — top-left for TEXT)
    double height = 0.0;   // CAD units (font height in world space)
    std::string content;   // UTF-8 (LibreDWG normalises wide strings on read)
    Color color{};
};

// M5 deliverable: parse a .dwg file into a flat segment list (lines)
// plus a separate text list. CIRCLE / ARC / LWPOLYLINE are tessellated
// to chords at parse time so the renderer only ever sees `Line`. TEXT
// / MTEXT carry their own position + height + content. Each entity
// also carries a resolved RGBA color (Slab 3); BYLAYER / BYBLOCK
// remain default-black until the layer model lands in Slab 4.
//
// POLYLINE_2D / POLYLINE_3D (which carry vertices via dynamic handle
// lists), text rotation, and other per-entity styling (layer, weight,
// bulge) are still deferred.
struct Entities {
    bool ok = false;
    std::string error;             // populated when ok == false
    std::string version;           // e.g. "AC1015 (R2000)"
    std::vector<Line> lines;       // post-tessellation segment list
    std::vector<Text> texts;       // TEXT + MTEXT, untransformed CAD coords

    // Source-entity counts (before tessellation) so the summary card
    // can show what kinds of geometry came in.
    unsigned int line_count = 0;
    unsigned int circle_count = 0;
    unsigned int arc_count = 0;
    unsigned int polyline_count = 0;
    unsigned int text_count = 0;

    // unknown_entities: count of DWG_SUPERTYPE_ENTITY records that
    // the parser does not (yet) extract — surfaces what's being lost.
    unsigned int unknown_entities = 0;
};

Entities parse_file(std::string_view path);

} // namespace cadpp
