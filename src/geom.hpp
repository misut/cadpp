// cad++ — geometry helpers (bbox, world↔canvas transform).
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "parser.hpp"

namespace cadpp {

namespace detail {
// Sentinels chosen so any real DWG coordinate (typically within ±1e9
// even for engineering-scale plans) updates them on the first add().
inline constexpr double kPosHuge =  1.0e308;
inline constexpr double kNegHuge = -1.0e308;
inline constexpr double dmin(double a, double b) { return a < b ? a : b; }
} // namespace detail

struct BBox {
    double min_x =  detail::kPosHuge;
    double min_y =  detail::kPosHuge;
    double max_x =  detail::kNegHuge;
    double max_y =  detail::kNegHuge;

    void add(double x, double y) {
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }

    bool empty() const { return min_x > max_x; }
    double width() const { return max_x - min_x; }
    double height() const { return max_y - min_y; }
};

inline BBox compute_bbox(Entities const& e) {
    BBox b;
    for (auto const& l : e.lines) {
        b.add(l.a.x, l.a.y);
        b.add(l.b.x, l.b.y);
    }
    for (auto const& t : e.texts) {
        b.add(t.position.x, t.position.y);
        b.add(t.position.x, t.position.y + t.height);
    }
    return b;
}

// World-to-canvas transform. CAD's Y axis points up; canvas's Y axis
// points down — apply() flips Y. Computed once at parse time and held
// in State, so view() just looks values up.
struct ViewportTransform {
    BBox bbox{};
    double scale = 1.0;
    double pad_x = 0.0;
    double pad_y = 0.0;

    static ViewportTransform fit(BBox const& b,
                                 float viewport_w, float viewport_h,
                                 float margin = 16.0f) {
        ViewportTransform vt{};
        vt.bbox = b;
        if (b.empty() || (b.width() == 0.0 && b.height() == 0.0)) {
            vt.pad_x = static_cast<double>(viewport_w) / 2.0;
            vt.pad_y = static_cast<double>(viewport_h) / 2.0;
            return vt;
        }
        double avail_w = static_cast<double>(viewport_w) - 2.0 * margin;
        double avail_h = static_cast<double>(viewport_h) - 2.0 * margin;
        double sx = b.width()  > 0 ? avail_w / b.width()  : avail_h / (b.height() > 0 ? b.height() : 1.0);
        double sy = b.height() > 0 ? avail_h / b.height() : sx;
        vt.scale  = detail::dmin(sx, sy);
        double drawn_w = b.width()  * vt.scale;
        double drawn_h = b.height() * vt.scale;
        vt.pad_x = (static_cast<double>(viewport_w) - drawn_w) / 2.0;
        vt.pad_y = (static_cast<double>(viewport_h) - drawn_h) / 2.0;
        return vt;
    }

    Point apply(double wx, double wy) const {
        return Point{
            pad_x + (wx - bbox.min_x) * scale,
            pad_y + (bbox.max_y - wy) * scale,  // CAD Y is up; canvas Y is down.
        };
    }
};

} // namespace cadpp
