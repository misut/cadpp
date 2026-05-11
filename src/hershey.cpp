// cad++ — Embedded Hershey stroke font renderer.
// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import phenotype;

#include "hershey.hpp"
#include "hershey_data.hpp"

namespace cadpp::hershey {

namespace {

// ---------- canonicalisation (mirrors fonts.cpp) ----------

// Lowercase ASCII letters/digits only — keeps the table independent
// from punctuation / casing / weight markers stripped earlier by
// `extract_family_from_font_file`. Same shape as fonts.cpp's helper:
// kept in this TU instead of widening fonts.hpp because the two
// callers don't share any other surface and the 30-line duplication
// is cheaper than a new public-API boundary.
std::array<char, 64> canonicalise(std::string_view in) noexcept {
    std::array<char, 64> out{};
    std::size_t j = 0;
    for (char c : in) {
        if (j + 1 >= out.size()) break;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        }
    }
    return out;
}

bool canon_contains(std::array<char, 64> const& a,
                    std::string_view needle) noexcept {
    if (needle.empty()) return false;
    std::size_t n = 0;
    while (n < a.size() && a[n] != '\0') ++n;
    if (needle.size() > n) return false;
    for (std::size_t i = 0; i + needle.size() <= n; ++i) {
        bool match = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (a[i + k] != needle[k]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// ---------- variant table ----------

// SHX family token (canonicalised, longest-first) → stroke variant.
// Substring match — so "isocteur" catches "ISOCTEUR.shx" / "isocteur"
// / "IsoCTEur" alike. `extract_family_from_font_file` has already
// stripped the .shx extension and weight markers before we get here,
// but we still substring-match for robustness against MTEXT `\f`
// switches that occasionally leak raw filenames.
constexpr std::pair<std::string_view, Variant> kVariantMap[] = {
    // ISO families. EUR variants are the European-encoding flavours
    // of the regular ISOCP / ISOCT — visually identical for Latin
    // glyphs, so they share the same stroke variant.
    {"isocteur",  Variant::kTriplex},
    {"isocpeur",  Variant::kSimplex},
    {"isoct",     Variant::kTriplex},
    {"isocp",     Variant::kSimplex},
    // Roman families. romand = duplex (two-stroke), romanc = complex
    // (multi-stroke serif); the Hershey set we ship doesn't have an
    // exact complex match, so romanc folds into Triplex which is the
    // closest available serif weight.
    {"romand",    Variant::kSimplexBold},
    {"romanc",    Variant::kTriplex},
    {"romant",    Variant::kTriplex},
    {"romans",    Variant::kSimplex},
    // Italic families. italicc / italict are the AutoCAD "italic
    // complex / triplex" weights — both heavier than plain italic.
    {"italicc",   Variant::kItalicBold},
    {"italict",   Variant::kItalicBold},
    {"italic",    Variant::kItalic},
    // Script families. scriptc carries the heavier strokes; scripts
    // is the single-stroke face used in handwritten-style labels.
    {"scriptc",   Variant::kScriptComplex},
    {"scripts",   Variant::kScript},
    // Generic / plain.
    {"simplex",   Variant::kSimplex},
    {"txtmt",     Variant::kPlain},
    {"txt",       Variant::kPlain},
    // Gothic — no good match in our 8 variants; pick Simplex so the
    // text at least lines up properly instead of falling back to a
    // system sans with totally different metrics.
    {"gothicg",   Variant::kSimplex},
    {"gothic",    Variant::kSimplex},
};

Variant variant_from_canonical(std::array<char, 64> const& canon) noexcept {
    for (auto const& [key, v] : kVariantMap) {
        if (canon_contains(canon, key)) return v;
    }
    return Variant::kNone;
}

// ---------- variant data dispatch ----------

struct VariantData {
    std::span<std::int8_t const>   strokes;
    std::span<std::uint16_t const> glyph_start;
    std::span<std::uint16_t const> glyph_count;
    std::span<std::int8_t const>   glyph_advance;
    std::span<std::int8_t const>   glyph_min_x;
    std::span<std::int8_t const>   glyph_max_x;
    std::int8_t y_top;
    std::int8_t y_baseline;
    std::int8_t y_descent;
    std::int8_t space_advance;
};

// One macro-equivalent per variant. Plain functions (vs a giant
// switch on `Variant`) keep the data spans `constexpr` and make each
// dispatch a single load — same code path the optimiser would pick
// for an `if`-chain but easier to read.
#define CADPP_HERSHEY_DATA(SUFFIX)                              \
    VariantData{                                                \
        std::span{ detail::k_##SUFFIX##_strokes },              \
        std::span{ detail::k_##SUFFIX##_glyph_start },          \
        std::span{ detail::k_##SUFFIX##_glyph_count },          \
        std::span{ detail::k_##SUFFIX##_glyph_advance },        \
        std::span{ detail::k_##SUFFIX##_glyph_min_x },          \
        std::span{ detail::k_##SUFFIX##_glyph_max_x },          \
        detail::k_##SUFFIX##_y_top,                             \
        detail::k_##SUFFIX##_y_baseline,                        \
        detail::k_##SUFFIX##_y_descent,                         \
        detail::k_##SUFFIX##_space_advance,                     \
    }

VariantData const& variant_data(Variant v) noexcept {
    static constexpr VariantData kSimplex      = CADPP_HERSHEY_DATA(simplex);
    static constexpr VariantData kSimplexBold  = CADPP_HERSHEY_DATA(simplexbold);
    static constexpr VariantData kTriplex      = CADPP_HERSHEY_DATA(triplex);
    static constexpr VariantData kTriplexBold  = CADPP_HERSHEY_DATA(triplexbold);
    static constexpr VariantData kItalic       = CADPP_HERSHEY_DATA(italic);
    static constexpr VariantData kItalicBold   = CADPP_HERSHEY_DATA(italicbold);
    static constexpr VariantData kScript       = CADPP_HERSHEY_DATA(script);
    static constexpr VariantData kScriptCmpx   = CADPP_HERSHEY_DATA(scriptcomplex);
    switch (v) {
    case Variant::kSimplex:       return kSimplex;
    case Variant::kSimplexBold:   return kSimplexBold;
    case Variant::kTriplex:       return kTriplex;
    case Variant::kTriplexBold:   return kTriplexBold;
    case Variant::kItalic:        return kItalic;
    case Variant::kItalicBold:    return kItalicBold;
    case Variant::kScript:        return kScript;
    case Variant::kScriptComplex: return kScriptCmpx;
    case Variant::kPlain:         return kSimplex;  // txt.shx ≈ Simplex
    case Variant::kNone:
    default:                      return kSimplex;  // unreachable in valid flow
    }
}

#undef CADPP_HERSHEY_DATA

// ---------- UTF-8 codepoint iteration ----------

// Decode the next codepoint from `s` starting at `i`. Advances `i`
// past the bytes consumed. Returns 0xFFFD on malformed input — the
// caller treats it like any other non-ASCII codepoint (tofu).
char32_t utf8_next(std::string_view s, std::size_t& i) noexcept {
    if (i >= s.size()) return 0;
    auto const c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) { ++i; return c0; }
    auto take = [&](std::size_t n, char32_t mask) -> char32_t {
        if (i + n > s.size()) { i = s.size(); return 0xFFFD; }
        char32_t cp = static_cast<char32_t>(c0 & mask);
        for (std::size_t k = 1; k < n; ++k) {
            auto const b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) { i += k; return 0xFFFD; }
            cp = (cp << 6) | (b & 0x3F);
        }
        i += n;
        return cp;
    };
    if      ((c0 & 0xE0) == 0xC0) return take(2, 0x1F);
    else if ((c0 & 0xF0) == 0xE0) return take(3, 0x0F);
    else if ((c0 & 0xF8) == 0xF0) return take(4, 0x07);
    ++i;
    return 0xFFFD;
}

// ---------- per-glyph advance + stroke walk ----------

// Index of `cp` inside one variant's 95-entry glyph table, or -1 if
// the codepoint has no Hershey glyph (space, control, or anything
// outside 0x21..0x7F). The caller should still advance the cursor
// for these — see `space_advance` / tofu policy.
int glyph_index_for(char32_t cp) noexcept {
    if (cp >= 0x21 && cp <= 0x7F) return static_cast<int>(cp - 0x21);
    return -1;
}

// Advance in canvas pixels for a single codepoint at `font_px` cap
// height. `width_factor` scales horizontally without changing the
// glyph's intrinsic shape (mirrors AutoCAD's STYLE width factor).
// `detail::k_advance_factor` is the inter-glyph spacing multiplier
// from the generated data — applied at runtime so the constant can
// be retuned without regenerating the embedded glyph stream.
float advance_px(VariantData const& vd, char32_t cp,
                 float font_px, float width_factor) noexcept {
    float const cap   = static_cast<float>(vd.y_baseline - vd.y_top);
    float const scale = detail::k_advance_factor / cap * font_px * width_factor;
    if (cp == 0x20)  // space
        return static_cast<float>(vd.space_advance) * scale;
    int const gi = glyph_index_for(cp);
    if (gi < 0)
        return static_cast<float>(vd.space_advance) * scale;
    return static_cast<float>(vd.glyph_advance[static_cast<std::size_t>(gi)])
        * scale;
}

} // namespace

// ---------- public API ----------

bool is_shx_font_file(std::string_view font_file) noexcept {
    if (font_file.size() < 4) return false;
    auto const tail = font_file.substr(font_file.size() - 4);
    // Manual case-fold rather than tolower'ing the whole string —
    // 4 bytes, no allocation.
    auto lower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    return tail[0] == '.'
        && lower(tail[1]) == 's'
        && lower(tail[2]) == 'h'
        && lower(tail[3]) == 'x';
}

Variant resolve_variant(std::string_view font_family,
                        std::string_view font_file) noexcept {
    if (!is_shx_font_file(font_file)) return Variant::kNone;
    auto const canon = canonicalise(font_family);
    Variant const v = variant_from_canonical(canon);
    // Unknown SHX family → Simplex. Matches AutoCAD's behaviour when
    // an SHX file is missing at draw time: it substitutes the default
    // simplex face rather than dropping the text entirely.
    return v == Variant::kNone ? Variant::kSimplex : v;
}

float measure_run(Variant v, std::string_view utf8,
                  float font_px, float width_factor) noexcept {
    if (v == Variant::kNone || utf8.empty()) return 0.0f;
    auto const& vd = variant_data(v);
    float total = 0.0f;
    std::size_t i = 0;
    while (i < utf8.size()) {
        char32_t const cp = utf8_next(utf8, i);
        if (cp == 0) break;
        total += advance_px(vd, cp, font_px, width_factor);
    }
    return total;
}

RunBearings run_bearings(Variant v, std::string_view utf8,
                         float font_px, float width_factor) noexcept {
    RunBearings out{};
    if (v == Variant::kNone || utf8.empty()) return out;
    auto const& vd = variant_data(v);
    float const cap        = static_cast<float>(vd.y_baseline - vd.y_top);
    float const adv_scale  = detail::k_advance_factor / cap * font_px * width_factor;
    float const x_scale    = font_px / cap * width_factor;

    // Walk codepoints, tracking the running cursor (spaces advance
    // it; non-space visible glyphs update first/last bookkeeping).
    // first_left_bearing = first non-space glyph's `min_x` in px,
    // measured from the run's starting cursor (= 0). last bearings
    // are recorded for the most recent visible glyph; on the final
    // glyph these become the run's last-glyph overflow.
    float cursor_px = 0.0f;
    bool  first_seen = false;
    bool  last_seen  = false;
    int   last_max_x_units = 0;
    int   last_advance_units = 0;
    float cursor_before_last = 0.0f;

    std::size_t i = 0;
    while (i < utf8.size()) {
        char32_t const cp = utf8_next(utf8, i);
        if (cp == 0) break;
        int const gi = (cp == 0x20) ? -1 : glyph_index_for(cp);
        if (gi < 0) {
            cursor_px += static_cast<float>(vd.space_advance) * adv_scale;
            continue;
        }
        auto const idx = static_cast<std::size_t>(gi);
        if (!first_seen) {
            out.first_left_bearing_px =
                static_cast<float>(vd.glyph_min_x[idx]) * x_scale;
            first_seen = true;
        }
        cursor_before_last = cursor_px;
        last_max_x_units    = vd.glyph_max_x[idx];
        last_advance_units  = vd.glyph_advance[idx];
        last_seen = true;
        cursor_px += static_cast<float>(last_advance_units) * adv_scale;
    }
    if (last_seen) {
        // The last glyph was drawn at cursor_before_last; cursor then
        // moved by last_advance_units × adv_scale. The visible right
        // edge is at cursor_before_last + last.max_x × x_scale. The
        // overflow vs. the cursor's end position is
        //   visible_right - cursor_end
        // = (cursor_before_last + max_x × x_scale)
        //   - (cursor_before_last + advance × adv_scale).
        // Folding the scales: max_x × x_scale - advance × adv_scale.
        out.last_right_overflow_px =
            static_cast<float>(last_max_x_units) * x_scale
            - static_cast<float>(last_advance_units) * adv_scale;
    }
    return out;
}

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
              float canvas_rotation) noexcept {
    if (v == Variant::kNone || utf8.empty() || font_px <= 0.0f) return;
    auto const& vd = variant_data(v);
    float const cap        = static_cast<float>(vd.y_baseline - vd.y_top);
    float const y_top_norm = static_cast<float>(vd.y_top);
    // Per-glyph advance scale: data is in raw Hershey units, and
    // `k_advance_factor` adds the inter-glyph gutter at runtime so
    // the constant can be retuned without regenerating the data.
    float const adv_scale  =
        detail::k_advance_factor / cap * font_px * width_factor;
    // The renderer drives glyph placement through `cap_offset_y` —
    // the gap from `y` (its segment's font-box top) to the glyph's
    // cap-top in canvas pixels. The renderer computes this from
    // the entity's v_align + total_height so the visible centre /
    // baseline of the run lands on the anchor as expected (e.g.
    // a Middle-aligned single-line label is centred on its anchor
    // for any line-advance multiplier the renderer picks).
    //
    // The renderer separately applies side-bearing offsets via
    // `run_bearings()` to `line_start_xs[li]`, so we don't shift
    // the cursor inside this function — that lets multi-segment
    // lines preserve trailing-whitespace advances at segment
    // boundaries.
    float const cap_offset = cap_offset_y;
    float const tan_obl    = oblique_rad == 0.0f ? 0.0f : std::tan(oblique_rad);
    bool  const rotated    = canvas_rotation != 0.0f;
    float const cosR       = rotated ? std::cos(canvas_rotation) : 1.0f;
    float const sinR       = rotated ? std::sin(canvas_rotation) : 0.0f;

    // Local-frame → canvas, with optional rotation around (x, y).
    auto place = [&](float lx, float ly) -> std::pair<float, float> {
        float px = x + lx;
        float py = y + cap_offset + ly;
        if (rotated) {
            float const dx = px - x;
            float const dy = py - y;
            px = x + dx * cosR - dy * sinR;
            py = y + dx * sinR + dy * cosR;
        }
        return { px, py };
    };

    // Convert a Hershey vertex (hx, hy) into local-frame canvas px.
    // `cursor_x` is the per-glyph horizontal offset accumulated as
    // we walk the run — applied to `lx` so each glyph slides right.
    auto vertex_local = [&](int hx, int hy,
                            float cursor_x) -> std::pair<float, float> {
        float const ny = (static_cast<float>(hy) - y_top_norm) / cap;
        float const nx = static_cast<float>(hx) / cap;
        float const lx = cursor_x + (nx * width_factor + ny * tan_obl) * font_px;
        float const ly = ny * font_px;
        return { lx, ly };
    };

    float cursor_x = 0.0f;
    std::size_t i = 0;
    while (i < utf8.size()) {
        char32_t const cp = utf8_next(utf8, i);
        if (cp == 0) break;

        if (cp == 0x20) {
            cursor_x += static_cast<float>(vd.space_advance) * adv_scale;
            continue;
        }
        int const gi = glyph_index_for(cp);
        if (gi < 0) {
            // Tofu placeholder: a tiny 5×5-Hershey-units rectangle
            // anchored at the same baseline as a normal glyph, so
            // missing chars are visible but don't overflow.
            constexpr int kTofuW = 5;
            constexpr int kTofuH = 12;  // cap-region height in hershey units
            auto const a = place(cursor_x + 0,        static_cast<float>(kTofuH - 0) * font_px / cap);
            auto const b = place(cursor_x + static_cast<float>(kTofuW) * font_px / cap,
                                 static_cast<float>(kTofuH - 0) * font_px / cap);
            auto const c = place(cursor_x + static_cast<float>(kTofuW) * font_px / cap, 0);
            auto const d = place(cursor_x + 0,        0);
            painter.line(a.first, a.second, b.first, b.second, thickness, color);
            painter.line(b.first, b.second, c.first, c.second, thickness, color);
            painter.line(c.first, c.second, d.first, d.second, thickness, color);
            painter.line(d.first, d.second, a.first, a.second, thickness, color);
            cursor_x += static_cast<float>(vd.space_advance) * adv_scale;
            continue;
        }

        std::uint16_t const start = vd.glyph_start[static_cast<std::size_t>(gi)];
        std::uint16_t const count = vd.glyph_count[static_cast<std::size_t>(gi)];
        // Walk the (x, y) byte pairs. (-128, -128) sentinel = pen-up:
        // close the current segment; the next pair after the sentinel
        // re-opens a fresh sub-path at that point. The first pair of
        // a glyph implicitly opens a sub-path.
        bool  seg_open = false;
        float prev_lx  = 0.0f;
        float prev_ly  = 0.0f;
        for (std::uint16_t k = 0; k + 1 < count; k += 2) {
            int const sx = vd.strokes[start + k];
            int const sy = vd.strokes[start + k + 1];
            if (sx == -128 && sy == -128) { seg_open = false; continue; }
            auto const [lx, ly] = vertex_local(sx, sy, cursor_x);
            if (!seg_open) {
                prev_lx = lx;
                prev_ly = ly;
                seg_open = true;
                continue;
            }
            auto const a = place(prev_lx, prev_ly);
            auto const b = place(lx, ly);
            painter.line(a.first, a.second, b.first, b.second, thickness, color);
            prev_lx = lx;
            prev_ly = ly;
        }

        cursor_x += static_cast<float>(
            vd.glyph_advance[static_cast<std::size_t>(gi)]) * adv_scale;
    }
}

} // namespace cadpp::hershey
