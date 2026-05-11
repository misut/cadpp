// cad++ — Embedded Hershey stroke font renderer for SHX fonts.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Consumers (renderer.cpp, tests) already import phenotype, which
// uses `import std;`. Mirror that to keep libc++ ABI tags consistent.
import std;
import phenotype;

namespace cadpp::hershey {

// Variant = stroke face shipped in `src/hershey_data.hpp`. The renderer
// picks one per text run via `resolve_variant(family, font_file)`. The
// raw glyph data comes from npm hersheytext (NIST Hershey, public
// domain) — see `tools/hershey/extract.mjs`.
//
// `kNone` means "not a stroke font" — the caller should fall through
// to the regular `Painter::text` path. The non-None values map to
// stroke face categories; the exact `hersheytext` JSON key each one
// resolves to is documented in `tools/hershey/extract.mjs::VARIANTS`.
enum class Variant : std::uint8_t {
    kNone           = 0,
    kSimplex,        // futural — sans 1-stroke (romans, simplex, isocp, ...)
    kSimplexBold,    // futuram — sans medium  (romand)
    kTriplex,        // timesr  — serif medium (romant, romanc, isoct, ...)
    kTriplexBold,    // timesrb — serif bold
    kItalic,         // timesi  — serif italic (italic)
    kItalicBold,     // timesib — serif bold italic (italicc, italict)
    kScript,         // scripts — script 1-stroke (scripts)
    kScriptComplex,  // scriptc — script medium (scriptc)
    kPlain,          // alias of kSimplex — AutoCAD `txt.shx` / `txtmt.shx`
};

// Case-insensitive `.shx` extension check. Empty input → false.
// Treats anything ending in `.shx` (regardless of preceding chars)
// as an SHX file reference. Used as the gate for stroke rendering:
// DWG STYLEs whose `font_file` ends in `.shx` are routed through
// the Hershey path even when an aliased TTF could substitute.
[[nodiscard]] bool is_shx_font_file(std::string_view font_file) noexcept;

// Pick a stroke variant from a (family, font_file) pair. When
// `font_file` doesn't look like an SHX reference, returns `kNone`
// so the caller knows to stay on the regular TTF code path. When
// `font_file` IS an SHX reference but `family` doesn't match any
// known SHX family token, returns `kSimplex` as the universally
// safe fallback — that's what AutoCAD does when an SHX is missing
// at draw time.
[[nodiscard]] Variant resolve_variant(std::string_view font_family,
                                      std::string_view font_file) noexcept;

// Cursor advance (kerning width) of a single line in canvas pixels —
// the amount the renderer's per-line cursor needs to step to draw the
// run, including trailing whitespace. Multi-segment lines accumulate
// these correctly because each segment's advance covers its own
// internal spaces. `Painter::measure_text` has the same semantic for
// TTF runs, so the two paths can share the renderer's wrap / cursor
// bookkeeping.
//
// `font_px` = cap-height in canvas pixels (= AutoCAD `text_height
// × view scale`). `width_factor` mirrors AutoCAD's STYLE width
// factor (1.0 = native).
[[nodiscard]] float measure_run(Variant v, std::string_view utf8,
                                float font_px, float width_factor) noexcept;

// Side-bearing adjustments for h-align Center / Right. The Hershey
// kerning `o` field often differs from the visible glyph extent
// (e.g. `D` has advance 11 but `max_x` 18, so its visible right edge
// sits *inside* the kerning), which makes `advance_sum / 2` drift
// off-centre for some letter pairs. The renderer uses these
// bearings to shift `line_start_xs[li]` by the per-line correction
// so the visible bounding box of the run lands on the anchor.
//
// Returns `{first_left_bearing_px, last_right_overflow_px}`:
//   - `first_left_bearing_px` = visible-left of the first non-space
//     glyph, in canvas pixels, relative to the run's starting cursor.
//   - `last_right_overflow_px` = `last.max_x − last.advance` in canvas
//     pixels (can be negative when the visible right sits before
//     where the cursor moves to).
//
// Empty / all-whitespace inputs return `{0, 0}`. Variant `kNone`
// also returns `{0, 0}` — the caller treats TTF runs as bearing-
// neutral.
struct RunBearings {
    float first_left_bearing_px = 0.0f;
    float last_right_overflow_px = 0.0f;
};
[[nodiscard]] RunBearings run_bearings(Variant v, std::string_view utf8,
                                       float font_px, float width_factor) noexcept;

// Draw `utf8` as a sequence of stroke glyphs through `painter.line()`
// calls.
//
// `(x, y)` is the visible run-origin: `x` is the visible left edge
// of the first non-space glyph, `y` is the input the renderer uses
// to drive vertical alignment (in the renderer's frame, the "font-
// size box top" of the segment — see `cap_offset_y`).
//
// `cap_offset_y` is the gap from `y` to the glyph's cap-top, in
// canvas pixels. The renderer computes this from its own v_align /
// total_height math so the visible centre / baseline of the run
// lands on the entity anchor as v_align expects.
//
// `font_px`, `width_factor`, `oblique_rad` compose the local glyph
// transform; `color` and `thickness` go directly into the line
// draw; `canvas_rotation` rotates each glyph around `(x, y)`
// (canvas-frame radians, CCW about +Z in canvas space).
//
// Clipping + viewport state are owned by the caller — this function
// does not touch `push_clip` / `pop_clip`. Calling with
// `v == kNone` is a no-op (safe to dispatch unconditionally).
void draw_run(phenotype::Painter& painter,
              Variant v,
              float x, float y,
              float cap_offset_y,
              std::string_view utf8,
              float font_px,
              float width_factor,
              float oblique_rad,
              phenotype::Color color,
              float thickness,
              float canvas_rotation) noexcept;

} // namespace cadpp::hershey
