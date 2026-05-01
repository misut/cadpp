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
    float thickness = 1.0f;  // Slab 7 — pixels at canvas resolution
};

// Horizontal text anchor (matches DWG TEXT::horiz_alignment, modulo
// the Aligned / Fit modes which we treat as Left for now — they need
// width measurement we don't have).
enum class TextHAlign : std::uint8_t {
    Left   = 0,  // anchor = baseline-left
    Center = 1,  // anchor = baseline-centre
    Right  = 2,  // anchor = baseline-right
    Middle = 4,  // anchor = visual-centre (Mid for both axes)
};

// Vertical text anchor (matches DWG TEXT::vert_alignment).
enum class TextVAlign : std::uint8_t {
    Baseline = 0,
    Bottom   = 1,
    Middle   = 2,
    Top      = 3,
};

// DWG STYLE table entry (subset). The parser extracts what the
// renderer needs to round-trip TEXT/MTEXT through phenotype's FontSpec:
// the human-readable family name (derived from `font_file`'s basename
// minus weight/italic suffix tokens), and Bold/Italic flags inferred
// from substring tokens in `font_file` (e.g. "arialbd.ttf" → Bold,
// "ariali.ttf" → Italic). The raw `font_file` is kept for diagnostics
// and for cases where consumers want to do their own resolution.
//
// LibreDWG's `Dwg_Object_STYLE` carries the underlying `flag` bit field
// too (vertical / shape / etc) but those don't affect text rendering
// at the level cad++ does today.
struct Style {
    std::string  name;          // STYLE table entry name (e.g. "Standard")
    std::string  font_family;   // extracted from font_file basename
    std::string  font_file;     // raw "arialbd.ttf" / "Arial.ttf" / etc.
    bool         bold   = false;
    bool         italic = false;
};

struct Text {
    Point position;        // CAD coords — anchor point per (h_align, v_align)
    double height = 0.0;   // CAD units (font height in world space)
    std::string content;   // UTF-8 (LibreDWG normalises wide strings on read)
    Color color{};
    std::string layer_name;
    TextHAlign h_align = TextHAlign::Left;
    TextVAlign v_align = TextVAlign::Baseline;
    // Resolved STYLE handle data — empty Style{} when the TEXT/MTEXT
    // entity has no STYLE handle or the handle could not be resolved.
    Style style;
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
    float thickness = 1.0f;  // Slab 7 — pixels at canvas resolution
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
    float               thickness = 1.0f;  // Slab 7
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
    float thickness = 1.0f;  // Slab 7
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
    float              thickness = 1.0f;  // Slab 7
};

// AutoCAD HATCH entity (Slab 5). Each HATCH carries one or more
// boundary loops; the parser flattens every loop into a vector of
// world-space `Point`s by approximating curve segments at parse
// time (LINE seg → 2 points, CIRCULAR ARC seg → 32-chord polyline,
// polyline path with bulge → arc-chord polyline). The renderer
// (`render_hatches`) walks each loop into a `phenotype::PathBuilder`
// polyline and dispatches via `Painter::fill_path`.
//
// Multi-loop HATCH (a HATCH with holes) emits each loop as its own
// `fill_path` call; the holes overprint with the same colour so the
// even-odd cut-out is not visible. Real holes need a multi-loop
// fill pipeline and stay out of scope.
//
// `solid = true` is the only fill style rendered for now —
// patterned and gradient HATCH entities surface as flat-colour
// fills until pattern / gradient pipelines land.
struct Hatch {
    std::vector<std::vector<Point>> loops;  // each loop ≥ 3 vertices, world coords
    Color                           color{};
    std::string                     layer_name;
    bool                            solid = true;
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

// Slab 7 — DWG LTYPE (linetype) table entry. `dashes` carries the
// raw signed pattern (positive = dash, negative = gap, zero = dot;
// units = world coords). The parser pre-decomposes each LINE entity
// along its resolved linetype into multiple `Line` records — one per
// dash — so the existing `Painter::line` emit path renders the
// pattern as a sequence of solid segments without phenotype API
// changes.
//
// Curves (ARC / CIRCLE / ELLIPSE / SPLINE / bulged-LWPOLYLINE) keep
// their solid stroke for now; dashing them would require walking the
// parameterised path. Straight LINE coverage already accounts for
// the dominant CAD usage (centerlines, hidden edges, axis lines).
struct Linetype {
    std::string         name;
    std::vector<double> dashes;
};

// DWG LAYOUT table entry. Each LAYOUT corresponds to one selectable
// "view" in the model picker — Autodesk Viewer shows the file's
// layouts as Sheets (paper-space) plus the implicit Model layout.
// `block_owner` carries the BLOCK_HEADER name (e.g. `*MODEL_SPACE`,
// `*PAPER_SPACE`, `*Layout1`) whose entities make up this layout's
// content; the parser uses it as the per-layout selection key.
struct Layout {
    std::string name;          // user-visible (e.g. "Model", "True Color")
    int         tab_order = 0; // DWG-defined ordering for the picker
    bool        is_model  = false;
    std::string block_owner;   // BLOCK_HEADER name owning this layout's entities
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
    std::vector<Hatch>   hatches;                  // AutoCAD HATCH entities (Slab 5)
    std::vector<Layer>   layers;                   // DWG LAYER table (Slab 4)
    std::vector<Linetype> linetypes;               // DWG LTYPE table (Slab 7)
    std::vector<Style>   styles;                   // DWG STYLE table (Slab 8 — fonts)
    std::vector<Layout>  layouts;                  // DWG LAYOUT table — user-selectable views

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
    unsigned int minsert_count = 0;    // Slab 5 — MINSERT (rectangular array INSERT) entities
    unsigned int dimension_count = 0;  // Slab 5 — DIMENSION pre-rendered blocks expanded
    unsigned int hatch_count = 0;      // Slab 5 — HATCH boundary loops captured
    unsigned int linetype_count = 0;   // Slab 7 — LTYPE table entries captured
    unsigned int style_count = 0;      // Slab 8 — STYLE table entries captured
    unsigned int layout_count = 0;     // DWG LAYOUT objects captured

    // unknown_entities: count of DWG_SUPERTYPE_ENTITY records that
    // the parser does not (yet) extract — surfaces what's being lost.
    unsigned int unknown_entities = 0;
};

// Parse a DWG file into native draw primitives. `layout_filter` selects
// which layout's entities are extracted: empty → first layout in tab
// order (typically Model). The `Entities::layouts` vector is always
// populated regardless of which layout was selected, so the UI can
// list every available view on first paint.
Entities parse_file(std::string_view path,
                    std::string_view layout_filter = {});

} // namespace cadpp
