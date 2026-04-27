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

// LWPOLYLINE with at least one non-zero `bulge` value. Each segment
// from `vertices[i]` to `vertices[i + 1]` is a circular arc whose
// arc-angle θ satisfies `bulge = tan(θ / 4)`; positive bulge sweeps
// CCW (in CAD's y-up frame), negative sweeps CW. `bulges` has one
// entry per segment (so size == vertices.size() - 1 for an open
// polyline, or == vertices.size() for a closed one — the closing
// segment's bulge is `bulges[vertices.size() - 1]`). A bulge of 0
// degenerates to a straight chord.
struct BulgedPolyline {
    std::vector<Point>  vertices;
    std::vector<double> bulges;
    bool                closed = false;
    Color               color{};
};

// AutoCAD ELLIPSE entity. `major_axis` is the vector (in CAD world
// coords) from `center` to one endpoint of the major axis;
// `minor_ratio` = |minor| / |major| ≤ 1. `start_param` / `end_param`
// are parametric angles in radians (NOT geometric angles); the point
// at parameter `t` is `center + major_axis · cos(t) + minor_axis · sin(t)`
// where `minor_axis` is `major_axis` rotated 90° CCW and scaled by
// `minor_ratio`. For a full ellipse, `start_param = 0`,
// `end_param = 2π`.
struct Ellipse {
    Point  center;
    Point  major_axis;
    double minor_ratio = 1.0;
    double start_param = 0.0;
    double end_param   = 0.0;
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
    std::vector<BulgedPolyline> bulged_polylines;  // LWPOLYLINE with any non-zero bulge
    std::vector<Ellipse> ellipses;                 // AutoCAD ELLIPSE entities

    // Source-entity counts (before tessellation) so the summary card
    // can show what kinds of geometry came in.
    unsigned int line_count = 0;
    unsigned int circle_count = 0;
    unsigned int arc_count = 0;
    unsigned int polyline_count = 0;
    unsigned int ellipse_count = 0;
    unsigned int text_count = 0;

    // unknown_entities: count of DWG_SUPERTYPE_ENTITY records that
    // the parser does not (yet) extract — surfaces what's being lost.
    unsigned int unknown_entities = 0;
};

Entities parse_file(std::string_view path);

} // namespace cadpp
