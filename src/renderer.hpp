// cad++ — entity → phenotype draw command emitter.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

import phenotype;

#include "geom.hpp"
#include "parser.hpp"

namespace cadpp {

void render_lines(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform);

void render_texts(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform);

void render_arcs(phenotype::Painter& p,
                 Entities const& entities,
                 ViewportTransform const& transform);

// Bulged LWPOLYLINE + ELLIPSE → `Painter::stroke_path` (Slab 2.c).
// Both entity classes are expressed as `phenotype::PathBuilder` verb
// streams (LineTo / ArcTo / CubicTo) and dispatched through the new
// path API. Straight LWPOLYLINEs and CIRCLE / ARC keep their existing
// emit paths in `render_lines` / `render_arcs`.
void render_paths(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform);

} // namespace cadpp
