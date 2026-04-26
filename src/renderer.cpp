// cad++ — entity → phenotype draw command emitter.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "renderer.hpp"

namespace cadpp {

namespace {

constexpr phenotype::Color kInkColor{26, 26, 26, 255};   // near-black
constexpr float kLineThickness = 1.0f;

} // namespace

void render_lines(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform) {
    for (auto const& l : entities.lines) {
        auto const a = transform.apply(l.a.x, l.a.y);
        auto const b = transform.apply(l.b.x, l.b.y);
        p.line(static_cast<float>(a.x), static_cast<float>(a.y),
               static_cast<float>(b.x), static_cast<float>(b.y),
               kLineThickness, kInkColor);
    }
}

void render_texts(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform) {
    for (auto const& t : entities.texts) {
        if (t.content.empty()) continue;
        // CAD's insertion_pt sits on the text's baseline; canvas's
        // text() takes the top-left of the font-size box. Move up by
        // `height` in CAD (= down by `height * scale` in canvas) to
        // line them up.
        auto const top_left = transform.apply(
            t.position.x, t.position.y + t.height);
        float const font_px = static_cast<float>(t.height * transform.scale);
        if (font_px < 4.0f) continue;  // below visual threshold; skip
        p.text(static_cast<float>(top_left.x),
               static_cast<float>(top_left.y),
               t.content.data(),
               static_cast<unsigned int>(t.content.size()),
               font_px, kInkColor);
    }
}

} // namespace cadpp
