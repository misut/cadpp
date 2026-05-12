// cad++ — DWG font-name → host font alias.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

import std;

namespace cadpp {

// Map a DWG STYLE family token (already stripped to its bare basename
// by `extract_family_from_font_file` in parser.cpp) to a fallback font
// that actually exists on the host. Lookup is case-insensitive over
// ASCII; punctuation/spaces in the DWG token are ignored. Empty input
// returns empty; an unknown family returns empty so the caller can
// fall back to passing the raw DWG family through to phenotype
// unchanged.
//
// As of issue #48 follow-up, the renderer (`render_texts` in
// renderer.cpp) treats this table as a **fallback path** — it first
// probes phenotype's `Painter::font_metrics` for the raw DWG family
// directly. If the host can resolve the original face (system-
// installed TTF, e.g. AutoCAD's `cityb___.ttf` from Microsoft Office
// or any face dropped into `~/Library/Fonts/`), the renderer uses
// that and skips the alias entirely — matching AutoCAD's font-
// discovery model. The alias substitute kicks in only when the probe
// reports a missing face. Set `CADPP_FONT_FORCE_ALIAS=1` to short-
// circuit the probe and force the legacy alias-only behaviour.
//
// Coverage focuses on the AutoCAD-shipped TTF basenames (txt, simplex,
// romans, isocp...) plus the Bitstream "Swis721 / Dutch801 / Monospac821
// / Stylus / BankGothic ..." family names that show up inside MTEXT
// `\f<face>;` switches and in STYLE table font_file fields. Calibrated
// against fonts that ship with macOS by default; Windows / Android
// substitutes are tracked in the same table when their macOS choice is
// also available cross-platform, otherwise left as a TODO.
std::string_view alias_font_family(std::string_view dwg_family) noexcept;

struct Style;  // parser.hpp — forward-declared so this header stays
               // narrow; the .cpp picks up the full definition.

// For each STYLE that names a non-empty font_file, search the host's
// font directories for a matching TTF / OTF / TTC and register it with
// phenotype under the style's `font_family` alias. After registration
// the renderer's `FontSpec{family=style.font_family}` resolves to the
// real face — no alias-table substitution needed for that family.
//
// Search order (macOS):
//   1. `$CADPP_FONT_DIR` (colon-separated list, if set)
//   2. `$HOME/Library/Fonts/`
//   3. `/Library/Fonts/`
//   4. `/System/Library/Fonts/`
//   5. `$HOME/Library/Application Support/Autodesk/Web Services/Login State/SOMServices/com.autodesk.shared/Fonts/`
//      (covers AutoCAD-installed fonts on macOS, when present)
//
// Files are matched case-insensitively against `font_file` (with and
// without `.ttf` / `.otf` / `.ttc` extensions when the STYLE entry
// already contains an extension). Returns the number of styles that
// were successfully registered (zero = nothing found, fall through to
// the alias-table substitutes — same end-user experience as before).
unsigned int register_style_fonts(std::span<Style const> styles);

// Register cad++'s shipped OFL fonts (Liberation Serif, Architects
// Daughter, Source Sans 3, JetBrains Mono) with phenotype under their
// canonical family names. After this call, the `kAliases` rows
// "times" → "Liberation Serif" / "cityblueprint" → "Architects
// Daughter" / etc. deliver guaranteed substitutes on a clean macOS
// install — the bundled face is reachable via
// `CTFontCreateWithName("Liberation Serif", ...)` even when no
// system-side copy exists.
//
// Probes `<exec>/../../../assets/fonts` (the workspace build layout)
// and `<exec>/assets/fonts` (a future install layout). Returns the
// number of files successfully registered (≤ 4); returns 0 with no
// side effects when the bundle dir cannot be located. Idempotent —
// phenotype swallows CoreText `kCTFontManagerErrorAlreadyRegistered`
// (105) so repeated calls from `State::load` are safe.
//
// macOS only for Phase B-1. Windows / Android currently return 0
// because `executable_dir_macos` is gated behind `__APPLE__`; the
// follow-up phases add per-platform executable-path resolution and an
// asset-manager bridge respectively.
unsigned int register_bundled_fonts();

} // namespace cadpp
