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

} // namespace cadpp
