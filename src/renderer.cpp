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

} // namespace cadpp
