// cad++ — DWG font-name → host font alias.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

import std;

namespace cadpp {

// Map a DWG STYLE family token (already stripped to its bare basename
// by `extract_family_from_font_file` in parser.cpp) to a font that
// actually exists on the host. Lookup is case-insensitive over ASCII;
// punctuation/spaces in the DWG token are ignored. Empty input returns
// empty; an unknown family returns empty so the caller can fall back to
// passing the raw DWG family through to phenotype unchanged (the
// platform backend then logs the missing-font event itself).
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

} // namespace cadpp
