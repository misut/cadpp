// cad++ — self-test for the embedded Hershey stroke renderer.
// SPDX-License-Identifier: GPL-3.0-or-later

import std;

#include "../../src/hershey.hpp"

namespace {

using cadpp::hershey::Variant;
using cadpp::hershey::is_shx_font_file;
using cadpp::hershey::resolve_variant;
using cadpp::hershey::measure_run;

// Tiny test harness — each `check_*` returns true on pass, false on
// fail, and logs the offending line to stderr. `main` exits with the
// first failing test's index (≥1) or 0 when everything passes.

int g_failed = 0;

void fail(int line, std::string_view msg) {
    // `import std` exposes the iostream streams but not the legacy
    // <cstdio> macros (stderr/stdout are #define's that aren't part
    // of the module surface). Use the streams directly.
    std::cerr << "FAIL hershey_test:" << line << ": " << msg << "\n";
    ++g_failed;
}

#define CHECK(cond)  do { if (!(cond)) fail(__LINE__, #cond); } while (0)

// 1. `.shx` extension detection
void test_is_shx_font_file() {
    CHECK(is_shx_font_file("romans.shx"));
    CHECK(is_shx_font_file("ROMANS.SHX"));
    CHECK(is_shx_font_file("Romans.Shx"));
    CHECK(is_shx_font_file("a.shx"));
    CHECK(!is_shx_font_file("arial.ttf"));
    CHECK(!is_shx_font_file(""));
    CHECK(!is_shx_font_file("shx"));        // no leading dot
    CHECK(!is_shx_font_file("foo.sh"));     // too short
    CHECK(!is_shx_font_file("foo.shxy"));   // extension off
}

// 2. Variant mapping — covers the whole table.
void test_resolve_variant() {
    // Non-SHX inputs: always kNone.
    CHECK(resolve_variant("Arial",   "arial.ttf")  == Variant::kNone);
    CHECK(resolve_variant("romans",  "")            == Variant::kNone);
    CHECK(resolve_variant("",        "anything.ttf")== Variant::kNone);

    // SHX inputs with known families.
    CHECK(resolve_variant("romans",   "romans.shx")   == Variant::kSimplex);
    CHECK(resolve_variant("simplex",  "simplex.shx")  == Variant::kSimplex);
    CHECK(resolve_variant("isocp",    "isocp.shx")    == Variant::kSimplex);
    CHECK(resolve_variant("isocpeur", "isocpeur.shx") == Variant::kSimplex);
    CHECK(resolve_variant("romand",   "romand.shx")   == Variant::kSimplexBold);
    CHECK(resolve_variant("romant",   "romant.shx")   == Variant::kTriplex);
    CHECK(resolve_variant("romanc",   "romanc.shx")   == Variant::kTriplex);
    CHECK(resolve_variant("isoct",    "isoct.shx")    == Variant::kTriplex);
    CHECK(resolve_variant("isocteur", "isocteur.shx") == Variant::kTriplex);
    CHECK(resolve_variant("italic",   "italic.shx")   == Variant::kItalic);
    CHECK(resolve_variant("italicc",  "italicc.shx")  == Variant::kItalicBold);
    CHECK(resolve_variant("italict",  "italict.shx")  == Variant::kItalicBold);
    CHECK(resolve_variant("scripts",  "scripts.shx")  == Variant::kScript);
    CHECK(resolve_variant("scriptc",  "scriptc.shx")  == Variant::kScriptComplex);
    CHECK(resolve_variant("txt",      "txt.shx")      == Variant::kPlain);
    CHECK(resolve_variant("txtmt",    "txtmt.shx")    == Variant::kPlain);

    // Case insensitivity (canonicalise() lowercases).
    CHECK(resolve_variant("ROMANS",   "ROMANS.SHX")   == Variant::kSimplex);
    CHECK(resolve_variant("ROmAnS",   "romans.SHX")   == Variant::kSimplex);

    // Unknown SHX family → Simplex fallback (matches AutoCAD's
    // behaviour for missing SHX files).
    CHECK(resolve_variant("blizzard", "blizzard.shx") == Variant::kSimplex);
    CHECK(resolve_variant("",         "mystery.shx")  == Variant::kSimplex);
}

// 3. measure_run sanity — advance-sum semantics (cursor delta).
//    N copies of the same glyph add up to N × advance_px exactly,
//    so multi-segment lines can sum without losing inter-segment
//    spaces.
void test_measure_run() {
    using cadpp::hershey::run_bearings;

    // kNone / empty → 0
    CHECK(measure_run(Variant::kNone, "AAA", 100.0f, 1.0f) == 0.0f);
    CHECK(measure_run(Variant::kSimplex, "",  100.0f, 1.0f) == 0.0f);

    float const w1  = measure_run(Variant::kSimplex, "A",   100.0f, 1.0f);
    float const w3  = measure_run(Variant::kSimplex, "AAA", 100.0f, 1.0f);
    CHECK(w1 > 0.0f);
    CHECK(std::abs(w3 - 3.0f * w1) < 0.001f);  // exact multiple

    // width_factor and font_px scale proportionally.
    float const w1_half = measure_run(Variant::kSimplex, "A", 100.0f, 0.5f);
    CHECK(std::abs(w1_half * 2.0f - w1) < 0.001f);
    float const w1_big  = measure_run(Variant::kSimplex, "A", 200.0f, 1.0f);
    CHECK(std::abs(w1_big - 2.0f * w1) < 0.001f);

    // Space character has positive advance (so trailing-whitespace
    // segments don't collapse onto the next word).
    float const w_space = measure_run(Variant::kSimplex, " ", 100.0f, 1.0f);
    CHECK(w_space > 0.0f);

    // Bearings: 'A' (min_x=1, max_x=17, advance_raw=9, factor=1.93).
    // first_left_bearing_px = 1/21 × 100 ≈ 4.76.
    // last_right_overflow_px = max_x × x_scale − advance × adv_scale
    //                        = 17/21*100 − 9*1.93/21*100 ≈ −1.76.
    auto const b = run_bearings(Variant::kSimplex, "A", 100.0f, 1.0f);
    CHECK(std::abs(b.first_left_bearing_px - 1.0f / 21.0f * 100.0f) < 0.01f);
    CHECK(std::abs(b.last_right_overflow_px
                   - (17.0f / 21.0f * 100.0f - 9.0f * 1.93f / 21.0f * 100.0f))
          < 0.01f);

    // kNone bearings are zero (the caller treats TTF segments as
    // bearing-neutral).
    auto const b_none = run_bearings(Variant::kNone, "A", 100.0f, 1.0f);
    CHECK(b_none.first_left_bearing_px == 0.0f);
    CHECK(b_none.last_right_overflow_px == 0.0f);
}

// 4. Snapshot of 'A's advance — guards against accidental upstream
//    JSON drift or unnoticed `k_advance_factor` changes. Hand-
//    computed: simplex 'A' raw o = 9, factor = 1.93, cap_units = 21,
//    at font_px = 100 → advance_px = 9 × 1.93 / 21 × 100 ≈ 82.71.
void test_simplex_snapshot() {
    float const w = measure_run(Variant::kSimplex, "A", 100.0f, 1.0f);
    CHECK(std::abs(w - 9.0f * 1.93f / 21.0f * 100.0f) < 0.01f);
}

// 5. Hand-authored extended glyphs (°, ±, ⌀). Each codepoint should
//    have a positive advance, a smaller advance than a typical full-
//    cap-width glyph (so they don't push line layout), and bearings
//    that match the hand-authored data. UTF-8 byte sequences:
//      °  U+00B0 = 0xC2 0xB0
//      ±  U+00B1 = 0xC2 0xB1
//      ⌀  U+2300 = 0xE2 0x8C 0x80
void test_extended_glyphs() {
    using cadpp::hershey::run_bearings;
    auto const w_deg = measure_run(Variant::kSimplex, "\xC2\xB0", 100.0f, 1.0f);
    auto const w_pm  = measure_run(Variant::kSimplex, "\xC2\xB1", 100.0f, 1.0f);
    auto const w_dia = measure_run(Variant::kSimplex, "\xE2\x8C\x80", 100.0f, 1.0f);
    CHECK(w_deg > 0.0f);
    CHECK(w_pm  > 0.0f);
    CHECK(w_dia > 0.0f);

    // Snapshot the degree-sign advance against hand-authored data
    // (advance=9 raw, same scaling math as ASCII).
    CHECK(std::abs(w_deg - 9.0f * 1.93f / 21.0f * 100.0f) < 0.01f);
    // Diameter advance = 18 raw (close to capital 'O').
    CHECK(std::abs(w_dia - 18.0f * 1.93f / 21.0f * 100.0f) < 0.01f);

    // Bearings are populated for extended glyphs too (matches the
    // hand-authored min_x / max_x in `hershey_extended.hpp`).
    auto const b_deg = run_bearings(Variant::kSimplex, "\xC2\xB0", 100.0f, 1.0f);
    CHECK(std::abs(b_deg.first_left_bearing_px - 1.0f / 21.0f * 100.0f) < 0.01f);
}

} // namespace

int main() {
    test_is_shx_font_file();
    test_resolve_variant();
    test_measure_run();
    test_simplex_snapshot();
    test_extended_glyphs();
    if (g_failed == 0) {
        std::cout << "PASS hershey_test (all)\n";
        return 0;
    }
    std::cerr << "FAIL hershey_test (" << g_failed << " check(s) failed)\n";
    return 1;
}
