#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Convert the bundled hersheytext JSON (public-domain NIST Hershey
// stroke data, repackaged by techninja/hersheytextjs under MIT) into
// the constexpr C++ tables that ship under src/hershey_data.hpp.
//
// Source:  https://unpkg.com/hersheytext@<VERSION>/hersheytext.json
// Output:  ../../src/hershey_data.hpp
//
// Regenerate with:
//   node tools/hershey/extract.mjs           # fetches over network
//   node tools/hershey/extract.mjs path.json # reads a local copy
//
// Stable in-place output — committed alongside the script so a clean
// clone needs no network to build.

import { writeFileSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const HERSHEYTEXT_VERSION = "2.0.0";
const HERSHEYTEXT_URL =
    `https://unpkg.com/hersheytext@${HERSHEYTEXT_VERSION}/hersheytext.json`;

// AutoCAD SHX → Hershey variant mapping. The hersheytext key is the
// JSON top-level entry; the C++ enum value is what `Variant::kFoo`
// resolves to in src/hershey.hpp.
const VARIANTS = [
    { key: "futural",  cxx: "kSimplex"       , name: "Simplex"        },
    { key: "futuram",  cxx: "kSimplexBold"   , name: "Simplex Bold"   },
    { key: "timesr",   cxx: "kTriplex"       , name: "Triplex"        },
    { key: "timesrb",  cxx: "kTriplexBold"   , name: "Triplex Bold"   },
    { key: "timesi",   cxx: "kItalic"        , name: "Italic"         },
    { key: "timesib",  cxx: "kItalicBold"    , name: "Italic Bold"    },
    { key: "scripts",  cxx: "kScript"        , name: "Script"         },
    { key: "scriptc",  cxx: "kScriptComplex" , name: "Script Complex" },
];

// Inter-glyph advance multiplier. hersheytext's reference SVG
// renderer uses 1.68 — visually correct for tight web display but
// noticeably tighter than what Autodesk Viewer's SHX rendering of
// `romans` / `simplex` produces side-by-side with a CAD reference
// drawing. Bumped to 1.93 to add ~15% extra side bearing per glyph
// (e.g. `A` at raw o=9 → advance 17 instead of 15 in Hershey units,
// matching the visible glyph width of 16 units plus a 1-unit
// gutter). This is stored as a constexpr in the generated header
// so the runtime can multiply on the fly — tweak this single line
// and re-run the script to retune without touching the consuming
// code.
const ADVANCE_FACTOR = 1.93;

// Public-domain Hershey y-conventions, verified empirically against
// every variant we ship: glyph 'A' (chars[32]) spans y=1..22 in all
// eight variants → cap height = 21 units. Descenders ('g', 'y', 'p',
// etc.) reach y≈29 for serif/sans variants and y≈34 for script.
// Set per variant when extraction picks up a different value.
const DEFAULT_METRICS = {
    y_top: 1,
    y_baseline: 22,
    y_descent: 29,
    // hersheytext's default `horiz-adv-x` for a space character is
    // 10 raw units; the renderer does NOT apply ADVANCE_FACTOR to
    // it (see lib/hersheytext.js:192). Keep parity so single spaces
    // line up between renderer flavours.
    space_advance_raw: 10,
};

// Pen-up sentinel: encoded as the (-128, -128) pair so the decoder
// reads a uniform 2-byte stride and only has to check one byte.
const PEN_UP = -128;

function parsePath(d) {
    // Tokens are whitespace-separated. A coord token is "x,y"; verb
    // tokens are "M" / "L" possibly fused with their first coord
    // (hersheytext writes e.g. "M5,1 L5,15" rather than "M 5,1 …").
    // Output: flat list of {x, y, penup} in encounter order — the
    // path always starts with M, and subsequent coords keep the last
    // verb's mode (M-then-coords-then-L-then-coords-...).
    const out = [];
    let mode = "M";
    for (const tok of d.trim().split(/\s+/)) {
        if (!tok) continue;
        let body = tok;
        if (body[0] === "M") { mode = "M"; body = body.slice(1); }
        else if (body[0] === "L") { mode = "L"; body = body.slice(1); }
        if (!body) continue;  // bare verb without an attached coord
        const comma = body.indexOf(",");
        if (comma < 0)
            throw new Error(`bad coord token: ${JSON.stringify(tok)}`);
        const x = Number(body.slice(0, comma));
        const y = Number(body.slice(comma + 1));
        if (!Number.isFinite(x) || !Number.isFinite(y))
            throw new Error(`non-finite coord: ${JSON.stringify(tok)}`);
        out.push({ x, y, penup: mode === "M" });
        // After an M's first coord, further bare coords on the same
        // line continue the polyline implicitly until another verb.
        if (mode === "M") mode = "L";
    }
    return out;
}

function encodeStrokes(verts) {
    // Layout: each vertex = (int8 x, int8 y). A (-128, -128) pair
    // sentinel before a vertex means "lift the pen first, then
    // start a fresh sub-path from this vertex". The first vertex
    // always implicitly opens a sub-path (no leading sentinel).
    const bytes = [];
    let firstInSeg = true;
    for (const v of verts) {
        for (const c of [v.x, v.y])
            if (c < -127 || c > 127)
                throw new Error(`coord out of int8 range: ${c}`);
        if (v.penup && !firstInSeg) bytes.push(PEN_UP, PEN_UP);
        bytes.push(v.x, v.y);
        firstInSeg = false;
    }
    return bytes;
}

function formatBytes(bytes, indent = "    ", perLine = 16) {
    const out = [];
    for (let i = 0; i < bytes.length; i += perLine) {
        const row = bytes.slice(i, i + perLine);
        out.push(indent + row.map(n => String(n).padStart(4, " ")).join(", ") + ",");
    }
    // Drop the trailing comma from the very last row (purely cosmetic
    // — C++ allows trailing commas in array initialisers, but the
    // generator already handles "last row needs comma" implicitly).
    return out.join("\n");
}

async function fetchJson(localPathOrNull) {
    if (localPathOrNull) {
        return JSON.parse(readFileSync(localPathOrNull, "utf8"));
    }
    const res = await fetch(HERSHEYTEXT_URL);
    if (!res.ok) throw new Error(`fetch ${HERSHEYTEXT_URL}: ${res.status}`);
    return await res.json();
}

function emitVariant(json, v) {
    const font = json[v.key];
    if (!font || !Array.isArray(font.chars))
        throw new Error(`hersheytext JSON missing variant ${v.key}`);
    if (font.chars.length !== 95)
        throw new Error(`${v.key}: expected 95 chars, got ${font.chars.length}`);

    const strokes = [];      // flat int8 stream for this variant
    const glyphs = [];       // {advance, stroke_start, stroke_count}
    let y_top = DEFAULT_METRICS.y_top;
    let y_baseline = DEFAULT_METRICS.y_baseline;
    let y_descent = DEFAULT_METRICS.y_descent;

    let var_min_y = Infinity, var_max_y = -Infinity;

    for (let i = 0; i < 95; ++i) {
        const ch = font.chars[i];
        const verts = parsePath(ch.d || "");
        const bytes = encodeStrokes(verts);
        const start = strokes.length;
        strokes.push(...bytes);
        // Store the raw hersheytext `o` (in native Hershey units).
        // The runtime multiplies by `k_advance_factor` to get the
        // final per-glyph advance, so retuning the factor doesn't
        // require regenerating this header.
        const advance_raw = Math.round(Number(ch.o));
        if (advance_raw < -127 || advance_raw > 127)
            throw new Error(`${v.key}[${i}] advance overflow: ${advance_raw}`);
        // Visible-bounding-box bearings — min and max x over the
        // glyph's actual ink. Hershey `o` advances the cursor by
        // a *kerning* width that often differs from the visible
        // extent (e.g. `D` has advance 11 but max_x 18 — the visible
        // right edge sits *inside* the kerning, leaving padding).
        // The runtime uses these to adjust h-align Center / Right
        // to the visible bounding box instead of the kerning sum,
        // matching AutoCAD Viewer's centring behaviour. Empty
        // glyphs (none in our 8 variants) collapse to (0, 0).
        let min_x = 127, max_x = -128;
        for (const vert of verts) {
            if (vert.x < min_x) min_x = vert.x;
            if (vert.x > max_x) max_x = vert.x;
        }
        if (verts.length === 0) { min_x = 0; max_x = 0; }
        if (min_x < -127 || min_x > 127 || max_x < -127 || max_x > 127)
            throw new Error(`${v.key}[${i}] bearing overflow: [${min_x}, ${max_x}]`);
        glyphs.push({
            advance: advance_raw,
            min_x,
            max_x,
            stroke_start: start,
            stroke_count: bytes.length,
        });
        for (const vert of verts) {
            if (vert.y < var_min_y) var_min_y = vert.y;
            if (vert.y > var_max_y) var_max_y = vert.y;
        }
    }
    // Trust empirical defaults for cap-top / baseline (verified for
    // every variant we ship); pick descent from the observed extent
    // so script descenders that reach y=34 are not clipped at the
    // sans default of 29.
    y_descent = Math.max(DEFAULT_METRICS.y_descent, var_max_y);

    const suffix = v.cxx.replace(/^k/, "").toLowerCase();
    // Space width is stored raw too — the runtime multiplies by
    // `k_advance_factor` like every other glyph so word-internal
    // spacing scales consistently when the factor is retuned.
    const space_advance_raw = DEFAULT_METRICS.space_advance_raw;

    return {
        cxx: v.cxx,
        suffix,
        strokes,
        glyphs,
        metrics: { y_top, y_baseline, y_descent, space_advance: space_advance_raw },
        upstream_name: font.name || v.name,
    };
}

function emitHeader(variants, sourceTag) {
    const lines = [];
    lines.push(`// cad++ — Hershey stroke font data, auto-generated.`);
    lines.push(`// SPDX-License-Identifier: GPL-3.0-or-later`);
    lines.push(`//`);
    lines.push(`// Source: npm hersheytext ${HERSHEYTEXT_VERSION}`);
    lines.push(`//   ${HERSHEYTEXT_URL}`);
    lines.push(`//   ${sourceTag}`);
    lines.push(`//`);
    lines.push(`// The bundled NIST Hershey font data is public domain; the`);
    lines.push(`// surrounding hersheytextjs JavaScript is MIT-licensed.`);
    lines.push(`//`);
    lines.push(`// Regenerate with: \`node tools/hershey/extract.mjs\`.`);
    lines.push(`// Do not edit by hand — re-run the script and commit the diff.`);
    lines.push(``);
    lines.push(`#pragma once`);
    lines.push(``);
    lines.push(`// Consumed only by hershey.cpp, which already pulls in std types`);
    lines.push(`// via \`import std;\`. Keeping the include-free layout avoids`);
    lines.push(`// dragging the libc++ \`<cstdint>\` ABI tags into a module unit.`);
    lines.push(``);
    lines.push(`namespace cadpp::hershey::detail {`);
    lines.push(``);
    lines.push(`inline constexpr float k_advance_factor = ${ADVANCE_FACTOR}f;`);
    lines.push(``);

    for (const v of variants) {
        const N = v.glyphs.length;
        lines.push(`// ${v.upstream_name} (${v.suffix})`);
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_strokes[] = {`);
        lines.push(formatBytes(v.strokes));
        lines.push(`};`);
        lines.push(`inline constexpr std::uint16_t k_${v.suffix}_glyph_start[${N}] = {`);
        const starts = v.glyphs.map(g => g.stroke_start);
        lines.push(formatBytes(starts, "    ", 12));
        lines.push(`};`);
        lines.push(`inline constexpr std::uint16_t k_${v.suffix}_glyph_count[${N}] = {`);
        const counts = v.glyphs.map(g => g.stroke_count);
        lines.push(formatBytes(counts, "    ", 12));
        lines.push(`};`);
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_glyph_advance[${N}] = {`);
        const advances = v.glyphs.map(g => g.advance);
        lines.push(formatBytes(advances, "    ", 16));
        lines.push(`};`);
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_glyph_min_x[${N}] = {`);
        const min_xs = v.glyphs.map(g => g.min_x);
        lines.push(formatBytes(min_xs, "    ", 16));
        lines.push(`};`);
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_glyph_max_x[${N}] = {`);
        const max_xs = v.glyphs.map(g => g.max_x);
        lines.push(formatBytes(max_xs, "    ", 16));
        lines.push(`};`);
        const m = v.metrics;
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_y_top          = ${m.y_top};`);
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_y_baseline     = ${m.y_baseline};`);
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_y_descent      = ${m.y_descent};`);
        lines.push(`inline constexpr std::int8_t k_${v.suffix}_space_advance  = ${m.space_advance};`);
        lines.push(``);
    }

    lines.push(`} // namespace cadpp::hershey::detail`);
    lines.push(``);
    return lines.join("\n");
}

async function main() {
    const argv = process.argv.slice(2);
    const json = await fetchJson(argv[0] || null);
    const variants = VARIANTS.map(v => emitVariant(json, v));

    const here = dirname(fileURLToPath(import.meta.url));
    const outPath = resolve(here, "..", "..", "src", "hershey_data.hpp");
    const text = emitHeader(variants, argv[0] ? `(local: ${argv[0]})` : "(network fetch)");
    writeFileSync(outPath, text);

    // Stats
    let total_bytes = 0;
    for (const v of variants) total_bytes += v.strokes.length;
    console.log(`hershey_data.hpp written to ${outPath}`);
    console.log(`  variants: ${variants.length}`);
    console.log(`  glyphs/variant: 95`);
    console.log(`  total stroke bytes: ${total_bytes}`);
    console.log(`  index overhead: ${variants.length * 95 * (2 + 2 + 1)} bytes`);
}

main().catch(err => { console.error(err); process.exit(1); });
