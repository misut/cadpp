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

// `layer_name` carries the DWG layer name the entity belongs to (Slab
// 4). Empty when the parser couldn't resolve a layer for the entity —
// e.g. the layer handle is missing or the layer isn't loaded yet. Used
// for (a) BYLAYER colour fall-through (already done at parse time, so
// the value baked into `color` is the resolved colour, not BYLAYER)
// and (b) the layer panel's visibility / freeze toggle.
struct Line {
    Point a;
    Point b;
    Color color{};
    std::string layer_name;
};

struct Text {
    Point position;        // CAD coords (insertion point — top-left for TEXT)
    double height = 0.0;   // CAD units (font height in world space)
    std::string content;   // UTF-8 (LibreDWG normalises wide strings on read)
    Color color{};
    std::string layer_name;
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
    std::string layer_name;
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
    std::string         layer_name;
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
    std::string layer_name;
};

// AutoCAD SPLINE entity (Slab 5). The DWG stores either control
// points + a knot vector (NURBS scenario) or fit points (Bezier
// scenario through interpolation points). The parser pre-samples
// the curve into a polyline at parse time so the renderer just walks
// `points` like any other line list. `closed` reflects the SPLINE's
// `closed_b` / 2013+ `splineflags` close bit and triggers a final
// `Close` verb in the path stream.
//
// Parse-time pre-sampling keeps the runtime renderer cheap (no De
// Boor evaluation per frame) at the cost of zoom-out fidelity going
// flat — splines lose their analytic smoothness at extreme zoom.
// Real CAD viewers handle this the same way; the per-segment pixel
// error caps out at ~0.5 px at the parse-time scale.
struct Spline {
    std::vector<Point> points;
    bool               closed = false;
    Color              color{};
    std::string        layer_name;
};

// Slab 4 — DWG LAYER table entry. Drawing entities reference layers
// either explicitly (the entity's `layer_name`) or implicitly (BYLAYER
// colour fall-through). Frozen / off layers are normally hidden by the
// renderer; cad++ exposes both flags through the layer panel so the
// user can toggle layer visibility independently of the DWG file's
// stored state. `color` is already resolved (CMC -> RGB) at parse time
// so the renderer doesn't have to look back into LibreDWG.
struct Layer {
    std::string name;
    Color       color{};
    bool        frozen = false;  // DWG flag bit 1 — temporarily invisible + non-printable
    bool        off    = false;  // DWG flag bit 2 — invisible but still regenerated
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
    std::vector<Spline>  splines;                  // AutoCAD SPLINE entities (Slab 5)
    std::vector<Layer>   layers;                   // DWG LAYER table (Slab 4)

    // Source-entity counts (before tessellation) so the summary card
    // can show what kinds of geometry came in.
    unsigned int line_count = 0;
    unsigned int circle_count = 0;
    unsigned int arc_count = 0;
    unsigned int polyline_count = 0;
    unsigned int ellipse_count = 0;
    unsigned int spline_count = 0;
    unsigned int text_count = 0;
    unsigned int insert_count = 0;     // Slab 5 — INSERT block instances expanded
    unsigned int dimension_count = 0;  // Slab 5 — DIMENSION pre-rendered blocks expanded

    // unknown_entities: count of DWG_SUPERTYPE_ENTITY records that
    // the parser does not (yet) extract — surfaces what's being lost.
    unsigned int unknown_entities = 0;
};

Entities parse_file(std::string_view path);

} // namespace cadpp
