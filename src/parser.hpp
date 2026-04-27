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

// CIRCLE and ARC entities, kept as native arcs so the renderer can
// hand the framework an arc primitive instead of a chord soup.
// `start_angle` / `end_angle` are radians, CCW per AutoCAD's
// y-up math convention; for a full circle, `start_angle = 0,
// end_angle = 2π`. The renderer (renderer.cpp::render_arcs) takes
// care of converting into phenotype's canvas-y-down convention.
struct Arc {
    Point  center;
    double radius      = 0.0;
    double start_angle = 0.0;
    double end_angle   = 0.0;
    Color  color{};
};

// Slab 2.a: parse a .dwg file into native draw primitives. CIRCLE
// and ARC ride a dedicated `Arc` list (no parse-time chord
// tessellation) so phenotype's `Painter::arc` rasterises them at
// the GPU's native resolution. LINE / TEXT / MTEXT keep their
// flat list shape from earlier slabs. LWPOLYLINE still flattens
// to chord segments — bulged polylines fold into native arcs in
// a future slab.
//
// POLYLINE_2D / POLYLINE_3D (vertices via handle lists), text
// rotation, and remaining per-entity styling (layer, weight) are
// still deferred.
struct Entities {
    bool ok = false;
    std::string error;             // populated when ok == false
    std::string version;           // e.g. "AC1015 (R2000)"
    std::vector<Line> lines;       // raw + LWPOLYLINE chord segments
    std::vector<Text> texts;       // TEXT + MTEXT, untransformed CAD coords
    std::vector<Arc>  arcs;        // CIRCLE + ARC, native arcs (no chord tessellation)

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
