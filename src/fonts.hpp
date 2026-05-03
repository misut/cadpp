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

} // namespace cadpp
