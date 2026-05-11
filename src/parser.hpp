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
    // LWPOLYLINE constant width (DXF 43, world units). 0 = no
    // explicit width — the renderer falls back to `thickness` (the
    // lineweight-derived screen-pixel pen). When > 0, the renderer
    // multiplies by the active viewport scale so the stroke stays
    // proportional to the world geometry — paper-space borders that
    // saved a `const_width` of e.g. 0.051" render visibly thicker
    // than ordinary lineweight-only edges in
    // `blocks_and_tables_-_imperial.dwg`'s D-size Plot. Per-vertex
    // tapered widths (`flag & 0x20`) are still flattened to a single
    // world-width here — a future pass can split them into per-
    // segment values.
    float world_width = 0.0f;
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
    // STYLE table horizontal width factor (DXF 41). Multiplies each
    // glyph's advance + visual width along the text's local X axis.
    // 1.0 = the font's native proportions; 6.5 (e.g. ADDA in
    // `blocks_and_tables_-_imperial.dwg`) stretches an attribute
    // tag wide across the title-block column. The renderer feeds
    // this into phenotype's FontSpec so the same factor applies to
    // both glyph drawing and `measure_text` (which `render_texts`
    // uses to anchor centred / right-aligned runs and to drive the
    // MTEXT soft-wrap decisions). Per-entity overrides on TEXT /
    // ATTRIB live in `Text::width_factor` and take precedence over
    // this table value when populated; unset / non-positive entity
    // values fall back here, which itself falls back to 1.0.
    double       width_factor = 1.0;
    // STYLE table oblique angle (DXF 50), in *radians*. Positive →
    // the top of each glyph leans right relative to the bottom
    // (AutoCAD's italic-by-slant convention). LibreDWG carries the
    // raw value in degrees; the parser converts on read so the
    // renderer can feed it straight into `std::tan`. Currently only
    // the embedded Hershey stroke renderer honours this — phenotype's
    // TTF backend handles slant through `FontStyle::Italic` separately.
    double       oblique_angle = 0.0;
};

// One inline-styled segment of an MTEXT body. Populated when
// `parse_mtext_format` sees `\f<face>;`, `\C<n>;`, or `\H<n[x]>;`
// switches and needs to express that the next chunk of text uses a
// face / colour / size different from the entity's outer STYLE. Plain
// TEXT and MTEXT bodies that only contain literal characters leave
// `Text::runs` empty so the renderer's existing single-style fast path
// stays unchanged.
//
// `family_override` is the post-parse family token (no extension /
// weight suffix) — feed it through the renderer's font-alias step the
// same way `Text::style.font_family` is. `height_scale` multiplies the
// outer entity height (`\H1.5x;` → 1.5; `\H0.3;` is rare and treated
// as absolute world units / outer height when present without `x`).
// `color_override.a == 0` → inherit `Text::color`; otherwise use as-is
// (alpha defaults to 255 when AutoCAD colour-index codes resolve).
struct TextRun {
    std::string text;                    // UTF-8 segment, post-decode
    std::string family_override;         // empty → inherit Text::style
    double      height_scale = 1.0;      // multiplier on outer height
    Color       color_override{0, 0, 0, 0};
    bool        bold_override   = false; // logical OR over Text::style.bold
    bool        italic_override = false;
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
    // Optional inline-styled runs from MTEXT format codes. Empty
    // means "no inline overrides" — the renderer draws `content` as
    // a single styled block. When non-empty, the runs are the
    // ground truth and `content` is the flattened concatenation kept
    // for diagnostics / search.
    std::vector<TextRun> runs;
    // MTEXT line spacing factor (DXF 44, range 0.25–4.0). Multiplied
    // against AutoCAD's "3-on-5" default leading (5/3 of `height`)
    // to produce the per-line vertical advance in world units. 0 →
    // treat as 1.0 (default). Plain TEXT entities leave it at 1.0.
    double line_spacing = 1.0;
    // MTEXT paragraph tab stops in world units, measured from the
    // MTEXT's left edge (ins_pt.x). Extracted from inline `\pt<x>;`
    // / `\pxt<x>;` / `\pl<l>,t<x>;` paragraph-property codes. Empty
    // for plain TEXT and for MTEXT bodies that use no tabs. The
    // renderer consumes a tab stop per literal `\t` in the content,
    // advancing the line's x cursor to the matching world position
    // so multi-row labels share a column even when the labels
    // themselves vary in width.
    std::vector<double> tab_stops;
    // MTEXT bounding-rectangle width (DXF 41 — the `Defined Width`
    // the user dragged out when authoring the entity), in world
    // units post-INSERT scale. The renderer treats this as a soft
    // wrap boundary: when greater than zero, lines are broken at
    // word boundaries so the visible run-width never exceeds it.
    // Plain TEXT and MTEXT entities authored without a defined width
    // leave this at 0, which short-circuits the wrap path back to
    // the pre-wrap "break only on `\n`/`\t`" behaviour.
    //
    // Calibrated to AutoCAD's behaviour: a wider-than-rect_width
    // word overflows the rect rather than being broken mid-word —
    // matches Autodesk Viewer on the title-block notes in
    // `blocks_and_tables_-_imperial.dwg` where long URLs / single
    // long tokens sit on their own oversized line. AutoCAD's
    // `\W<x>;` per-word-width override code is not yet honoured.
    double wrap_width = 0.0;
    // Per-entity horizontal width factor (DXF 41 on TEXT / ATTRIB).
    // Overrides `style.width_factor` when populated (> 0). MTEXT
    // entities don't carry their own width_factor field — they
    // inherit from `style.width_factor`, so this stays at 1.0 for
    // them and the renderer falls back to the style value. Inline
    // `\W<x>;` / `\A` MTEXT codes that mid-run scale glyphs are not
    // yet plumbed and would land on `TextRun` (alongside the
    // existing `height_scale`) when added.
    double width_factor = 1.0;
    // Rotation in radians, CCW about the world +Z axis. Plain TEXT
    // reads `t->rotation` directly; MTEXT derives it from
    // `atan2(x_axis_dir.y, x_axis_dir.x)`. Parent INSERT / MINSERT
    // rotation composed in via `xf.rotation()` so nested labels
    // inherit the block instance's orientation. The renderer
    // negates this when handing it to phenotype's `Painter::text`
    // so the world-frame CCW convention matches phenotype's
    // canvas-frame screen-CCW (canvas y points down — see the
    // analogous arc handling in `render_arcs`).
    double rotation = 0.0;
};

// LEADER / MULTILEADER arrowhead. The leader's polyline itself is
// emitted as ordinary `Line` records under the same colour/layer;
// this struct only carries the filled-triangle tip so the renderer
// can `Painter::fill_path` it. `tip` is the world-space anchor point
// in CAD coordinates (post-`xf.apply_point`). `dx`/`dy` is a unit
// vector pointing from the tip *back along the leader* (so the
// triangle's base sits on the leader's first segment, not past it).
// `size` is the triangle's edge length in world units after the
// parent MULTILEADER's scale factor was applied at parse time.
// Matches AutoCAD's default "Closed Filled" arrowhead — per-block
// custom arrowheads are deferred to a future slab.
struct ArrowHead {
    Point  tip;
    double dx = 0.0;
    double dy = 0.0;
    double size = 0.0;
    Color  color{};
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
    // See `Line::world_width`. Bulged LWPOLYLINEs share the same
    // const_width semantics so the renderer can thicken the rounded
    // title-block border the same way it does straight polylines.
    float               world_width = 0.0f;
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
// Patterned HATCHes also flow `pattern_lines` — each entry is one
// LibreDWG `defline` resolved into world space (the AutoCAD STYLE-style
// (origin + offset + rotation) is post-multiplied by the entity's
// `angle` and `scale_spacing` and the inherited INSERT scale, so the
// renderer can lay parallel lines down with no further bookkeeping).
// The renderer steps through the pattern_lines, emits a parallel-line
// family that covers the boundary loops, and clips each line against
// the loops with the standard odd-parity scanline rule. Solid HATCHes
// keep `pattern_lines` empty and route through the legacy fill_path.
//
// Gradient HATCHes still surface as flat-colour fills — the gradient
// stops, ramp direction, and `is_one_color` toggle don't have a
// renderer pipeline yet.
struct HatchPatternLine {
    Point  origin;             // pt0 (post-rotation, post-scale, world coords)
    Point  offset;             // step from one parallel line to the next, world coords
    double angle = 0.0;        // line direction, radians (post-rotation)
    std::vector<double> dashes; // raw signed dash lengths (positive=ink, negative=gap, zero=dot)
};

struct Hatch {
    std::vector<std::vector<Point>> loops;  // each loop ≥ 3 vertices, world coords
    Color                           color{};
    std::string                     layer_name;
    bool                            solid = true;
    std::vector<HatchPatternLine>   pattern_lines;
};

// SOLID / TRACE store a single filled quadrilateral. Keep them out of
// HATCH so the renderer can batch thousands of true-colour cells into
// phenotype::Painter::fill_quads instead of sending one FillPath per
// rectangle.
struct SolidQuad {
    Point p0;
    Point p1;
    Point p2;
    Point p3;
    Color color{};
    std::string layer_name;
};

enum class FillKind : std::uint8_t {
    SolidQuad,
    Hatch,
};

struct FillItem {
    FillKind kind = FillKind::SolidQuad;
    std::size_t index = 0;
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
// `block_owner_handle` is the absolute handle (LibreDWG `BITCODE_RLL`,
// stored as uint64) of the BLOCK_HEADER backing this layout. Required
// because two paper-space sheets in the same file can share the BH
// *name* (e.g. both `*Paper_Space`) — name-based lookup then collapses
// them onto whichever BH is enumerated first, which is the colorwh.dwg
// regression where the True Color sheet rendered the Color Index
// sheet's viewports. Handle-based matching disambiguates them.
//
// `layout_handle` is the LAYOUT object's own absolute handle, used by
// pass 2 to re-resolve the LAYOUT and walk its `viewports[]` list (the
// DWG-defined per-sheet VIEWPORT ownership) instead of iterating every
// entity inside the shared `*Paper_Space` block-header.
struct Layout {
    std::string         name;                  // user-visible (e.g. "Model", "True Color")
    int                 tab_order = 0;         // DWG-defined ordering for the picker
    bool                is_model  = false;
    std::string         block_owner;           // BLOCK_HEADER name (legacy fallback / debug)
    std::uint64_t       block_owner_handle = 0; // BLOCK_HEADER absolute handle, 0 = unknown
    std::uint64_t       layout_handle      = 0; // LAYOUT object absolute handle, 0 = unknown
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
// VIEWPORT clip marker (Slab 9). Each `expand_viewport` walk emits
// one Push marker before pulling the model BLOCK_HEADER through the
// viewport's affine and one matching Pop marker after. The renderer
// walks the marker stream alongside each entity vector and issues
// `Painter::push_clip` / `pop_clip` so model content drawn under the
// transform stays inside the viewport's paper-space rectangle.
//
// The `*_idx` fields record the entity index at which the marker
// takes effect for each per-type vector. Each render function
// advances a private cursor through `clip_markers` and consults only
// its own `*_idx` field, so the same marker stream serves all
// entity-type render passes without per-type duplication.
//
// `x / y / w / h` are in paper-space CAD coordinates. The renderer
// applies the active `ViewportTransform` to project them into canvas
// pixels right before emitting the Painter clip call.
struct ClipMarker {
    enum class Kind : std::uint8_t { Push, Pop };
    Kind   kind = Kind::Push;
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    std::size_t lines_idx    = 0;
    std::size_t arcs_idx     = 0;
    std::size_t bulged_idx   = 0;
    std::size_t ellipses_idx = 0;
    std::size_t splines_idx  = 0;
    std::size_t fills_idx    = 0;
    std::size_t solid_quads_idx = 0;
    std::size_t hatches_idx  = 0;
    std::size_t texts_idx    = 0;
};

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
    std::vector<SolidQuad> solid_quads;            // SOLID / TRACE fast fill quads
    std::vector<Hatch>   hatches;                  // AutoCAD HATCH entities (Slab 5)
    std::vector<ArrowHead> arrows;                 // LEADER / MULTILEADER arrowhead tips
    std::vector<FillItem> fills;                   // HATCH + SOLID/TRACE parse order
    std::vector<Layer>   layers;                   // DWG LAYER table (Slab 4)
    std::vector<Linetype> linetypes;               // DWG LTYPE table (Slab 7)
    std::vector<Style>   styles;                   // DWG STYLE table (Slab 8 — fonts)
    std::vector<Layout>  layouts;                  // DWG LAYOUT table — user-selectable views
    // Slab 9 — VIEWPORT clip stream. Empty when no paper-space sheet
    // with decoded VIEWPORTs was selected.
    std::vector<ClipMarker> clip_markers;

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
    unsigned int solid_quad_count = 0; // SOLID / TRACE quadrilaterals captured
    unsigned int hatch_count = 0;      // Slab 5 — HATCH boundary loops captured
    unsigned int linetype_count = 0;   // Slab 7 — LTYPE table entries captured
    unsigned int style_count = 0;      // Slab 8 — STYLE table entries captured
    unsigned int layout_count = 0;     // DWG LAYOUT objects captured
    unsigned int viewport_count = 0;   // Slab 9 — VIEWPORT entities expanded
    unsigned int leader_count = 0;     // LEADER / MULTILEADER entities expanded
    // ACAD_TABLE entities the parser handled via the placeholder *T-
    // block-walk path (the entity's `dxfname` is "ACAD_TABLE" but
    // LibreDWG 0.13.4 returns UNKNOWN_ENT for the type-specific
    // fields, so the table content rides through the auto-generated
    // `*T<N>` block_header at a fixed paper-space offset). Each
    // placeholder picks up the next slot in a horizontal fan-out so
    // multiple tables don't overlap.
    unsigned int acad_table_placeholder_count = 0;

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
