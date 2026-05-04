// cad++ — DWG font-name → host font alias (lookup table).
// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import phenotype;
import phenotype.native;

#include "fonts.hpp"
#include "parser.hpp"

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

namespace {

// Push standard macOS font search directories onto `out`. Order
// matters — the env-var override comes first so users can shadow
// system fonts with their own copy of an AutoCAD face.
void collect_search_dirs(std::vector<std::string>& out) {
    if (char const* env = std::getenv("CADPP_FONT_DIR"); env != nullptr) {
        std::string_view rest{env};
        while (!rest.empty()) {
            auto const sep = rest.find(':');
            auto piece = (sep == std::string_view::npos) ? rest : rest.substr(0, sep);
            if (!piece.empty()) out.emplace_back(piece);
            if (sep == std::string_view::npos) break;
            rest = rest.substr(sep + 1);
        }
    }
    if (char const* home = std::getenv("HOME"); home != nullptr) {
        out.emplace_back(std::string{home} + "/Library/Fonts");
        out.emplace_back(std::string{home}
            + "/Library/Application Support/Autodesk/Web Services/"
              "Login State/SOMServices/com.autodesk.shared/Fonts");
    }
    out.emplace_back("/Library/Fonts");
    out.emplace_back("/System/Library/Fonts");
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    auto const off = s.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char a = s[off + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

// Given a STYLE `font_file` (e.g. "swisski.ttf" or "txt"), return the
// candidate filenames to look for in the search dirs. Always includes
// the original; for extensionless names (typical of SHX-style entries
// that the parser kept as-is) appends `.ttf`, `.otf`, `.ttc` variants.
std::vector<std::string> candidate_basenames(std::string const& font_file) {
    std::vector<std::string> out;
    if (font_file.empty()) return out;
    auto const slash = font_file.find_last_of("/\\");
    std::string base = (slash == std::string::npos)
        ? font_file : font_file.substr(slash + 1);
    out.push_back(base);
    bool const has_ext = ends_with_ci(base, ".ttf")
                      || ends_with_ci(base, ".otf")
                      || ends_with_ci(base, ".ttc")
                      || ends_with_ci(base, ".shx");
    if (!has_ext) {
        out.push_back(base + ".ttf");
        out.push_back(base + ".otf");
        out.push_back(base + ".ttc");
    }
    return out;
}

// Find the first match for any of `basenames` in any of `dirs` and
// return its absolute path. Empty when nothing matched. Skips entries
// whose names are not `.ttf` / `.otf` / `.ttc` (we can't register
// `.shx` shape files with CoreText anyway).
std::string find_font_file(std::vector<std::string> const& dirs,
                           std::vector<std::string> const& basenames) {
    namespace fs = std::filesystem;
    for (auto const& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto const& base : basenames) {
            // Direct hit at the basename (case-sensitive on APFS,
            // honoured by the underlying FS — no need to enumerate).
            fs::path const p = fs::path{dir} / base;
            if (fs::is_regular_file(p, ec)) return p.string();
        }
        // Fall back to a one-pass case-insensitive scan of the dir for
        // each basename. Cheap (~hundreds of entries on Library/Fonts)
        // and avoids missing the file when the on-disk name differs in
        // case from the DWG's `font_file`.
        fs::directory_iterator it{dir, ec};
        for (; !ec && !(it == std::default_sentinel); it.increment(ec)) {
            auto const& entry = *it;
            if (!entry.is_regular_file()) continue;
            auto const name = entry.path().filename().string();
            for (auto const& base : basenames) {
                if (ieq(name, base)) return entry.path().string();
            }
        }
    }
    return {};
}

} // namespace

unsigned int register_style_fonts(std::span<Style const> styles) {
    std::vector<std::string> dirs;
    collect_search_dirs(dirs);
    unsigned int registered = 0;
    std::set<std::string> already; // dedupe by font_family
    for (auto const& s : styles) {
        if (s.font_family.empty() || s.font_file.empty()) continue;
        if (!already.insert(s.font_family).second) continue;
        auto const candidates = candidate_basenames(s.font_file);
        auto const path = find_font_file(dirs, candidates);
        if (path.empty()) continue;
        // Skip SHX — CTFontManagerRegisterFontsForURL doesn't accept
        // shape-only files; the alias-table substitute kicks in for
        // those at render time.
        if (ends_with_ci(path, ".shx")) continue;
        if (phenotype::native::text::register_font_file(s.font_family, path)) {
            ++registered;
        }
    }
    return registered;
}

} // namespace cadpp
