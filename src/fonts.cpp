// cad++ — DWG font-name → host font alias (lookup table).
// SPDX-License-Identifier: GPL-3.0-or-later

import std;

#include "fonts.hpp"

namespace cadpp {

namespace {

// Lowercase ASCII letters/digits only — strips spaces, punctuation,
// case so "Swis721 Lt BT", "swis721ltbt", "SWIS721_LT_BT" all collide.
// `\f<face>;` MTEXT codes embed the literal family name (often with
// spaces and a `BT` foundry suffix), STYLE table `font_file` fields
// arrive as bare basenames after parser.cpp's extension/weight/italic
// stripping — collapsing both into a single canonical form lets one
// table cover both call sites.
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

// True if `needle` appears as a substring anywhere in the
// canonicalised key. The key is a fixed-size array padded with NULs;
// stop the scan at the first NUL so trailing zeros don't match.
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

// (DWG family substring → macOS host font name). Tested left-to-right
// against the canonicalised key with `find` — the first hit wins, so
// more-specific entries must come before broader ones (e.g. the
// "isocteur" entry sits before "isoct" and "txtshx" sits before "txt"
// so the longer name takes precedence).
//
// Substring (not exact) match is what lets one row cover the full
// AutoCAD weight / width / italic family (Swis721, Swis721 BT,
// Swis721 Lt BT, Swis721 Blk BT, Swis721 LtCn BT, ...) — every
// canonicalised variant contains "swis721" so a single Swis721 →
// Helvetica entry catches them all. Bold / italic / weight bits are
// already preserved in `FontSpec` separately.
//
// Calibrated for fonts that ship with macOS by default. If a row's
// substitute is wrong-looking the fix is a one-line table edit.
constexpr std::pair<std::string_view, std::string_view> kAliases[] = {
    // Bitstream foundry names (used inside MTEXT `\f` switches and as
    // a few STYLE table basenames). Listed before the SHX equivalents
    // so they take priority when both could match (none currently do
    // since MTEXT `\f` always carries the foundry name verbatim).
    {"swis721",          "Helvetica"},        // Swis721, Swis721 Lt BT, Blk BT, LtCn BT, ...
    {"dutch801",         "Times New Roman"},  // Dutch801 Rm/Bd/It BT, ...
    {"monospac821",      "Menlo"},
    {"bankgothic",       "Impact"},           // BankGothic Lt/Md BT
    {"bgoth",            "Impact"},
    {"commercialscript", "Snell Roundhand"},
    {"commscript",       "Snell Roundhand"},
    {"comscr",           "Snell Roundhand"},
    {"vineta",           "Snell Roundhand"},
    {"commercialpi",     "Symbol"},
    {"commpi",           "Symbol"},
    {"universalmath",    "STIXGeneral"},
    {"univmath",         "STIXGeneral"},
    {"stylus",           "Marker Felt"},
    {"stylu",            "Marker Felt"},
    // AutoCAD-shipped SHX shape fonts. The parser may pass the raw
    // basename ("txt") OR a fully qualified name ("txt.shx") through
    // `\f` switches; canonicalise() normalises both to "txtshx" /
    // "txt" so the substring "txt" catches both. Place after the
    // longer SHX names so e.g. "txtmt.shx" doesn't pre-empt them.
    {"isocpeur",         "Helvetica"},
    {"isocteur",         "Times New Roman"},
    {"isoct",            "Times New Roman"},
    {"isocp",            "Helvetica"},
    {"romans",           "Helvetica"},
    {"romant",           "Times New Roman"},
    {"swiss",            "Helvetica"},        // covers "swiss", "swissk" (swisski.ttf)
    {"txt",              "Menlo"},            // catches "txt", "txt.shx", "txtmt.shx"
};

} // namespace

std::string_view alias_font_family(std::string_view dwg_family) noexcept {
    if (dwg_family.empty()) return {};
    auto const key = canonicalise(dwg_family);
    if (key[0] == '\0') return {};
    for (auto const& [from, to] : kAliases) {
        if (canon_contains(key, from)) return to;
    }
    return {};
}

} // namespace cadpp
