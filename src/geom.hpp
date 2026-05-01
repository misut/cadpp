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
    // Conservative arc bbox: enclosing-circle (`center ± radius`).
    // The exact bbox of a partial arc depends on which axis-aligned
    // extrema fall inside the sweep, but the enclosing circle is
    // safe — fit-on-load won't clip the arc — and cheap.
    for (auto const& a : e.arcs) {
        b.add(a.center.x - a.radius, a.center.y - a.radius);
        b.add(a.center.x + a.radius, a.center.y + a.radius);
    }
    // Ellipse bbox via the parametric extrema of `P(t) = C + U·cos(t)
    // + V·sin(t)`, where U = `major_axis` and V = perp(U) ·
    // `minor_ratio`. Solving `dP/dt = 0` per axis gives the closed
    // form half-extents below — exact for full ellipses and a safe
    // upper bound for partial sweeps (good enough for fit-on-load).
    // Without this loop the wheel-shaped ellipses in colorwh.dwg
    // contributed nothing to the bbox, leaving fit-to-canvas to scale
    // the wheels off the edge of the canvas.
    for (auto const& el : e.ellipses) {
        double const ux = el.major_axis.x;
        double const uy = el.major_axis.y;
        double const k2 = el.minor_ratio * el.minor_ratio;
        double const half_w = std::sqrt(ux * ux + uy * uy * k2);
        double const half_h = std::sqrt(uy * uy + ux * ux * k2);
        b.add(el.center.x - half_w, el.center.y - half_h);
        b.add(el.center.x + half_w, el.center.y + half_h);
    }
    // Splines are pre-sampled into a polyline at parse time; iterate
    // every sample. Closed splines reuse the start vertex implicitly
    // via the `closed` flag, no extra add needed.
    for (auto const& sp : e.splines) {
        for (auto const& p : sp.points) b.add(p.x, p.y);
    }
    // Bulged polylines: vertex bbox only. A `bulge != 0` arc segment
    // can swing outside the chord-vertex rectangle, but accounting
    // for that needs `bulge_to_arc` per segment — left as follow-up
    // because the wheel + plate samples we ship all bulge inward
    // enough that vertex bbox already encloses the curve.
    for (auto const& bp : e.bulged_polylines) {
        for (auto const& v : bp.vertices) b.add(v.x, v.y);
    }
    // Hatches store closed boundary polygons in CAD coords (the
    // parser pre-discretises any curved boundary segment), so vertex
    // iteration is the exact bbox per loop.
    for (auto const& h : e.hatches) {
        for (auto const& loop : h.loops) {
            for (auto const& p : loop) b.add(p.x, p.y);
        }
    }
    return b;
}

// 2D affine transform (3×3 with implicit `[0 0 1]` last row). Used
// for INSERT block expansion (Slab 5): the parser flattens block-
// owned entities into the top-level entity vectors, applying the
// INSERT's `ins_pt + rotation + scale` to every coordinate so the
// renderer sees a single Model-Space entity stream — no recursion at
// render time.
//
// `apply_point` translates AND rotates/scales; `apply_vector` only
// rotates/scales (suitable for vectors-from-origin like an ellipse's
// major-axis offset). `scale_factor()` returns the average linear
// magnification — used for radius / font-height / line-thickness so
// they grow with the block instance's scale.
struct Affine {
    double m00 = 1.0, m01 = 0.0, m02 = 0.0;  // row 0: m00*x + m01*y + m02
    double m10 = 0.0, m11 = 1.0, m12 = 0.0;  // row 1

    static Affine identity() { return Affine{}; }

    static Affine translate(double tx, double ty) {
        Affine a; a.m02 = tx; a.m12 = ty; return a;
    }

    static Affine scale_xy(double sx, double sy) {
        Affine a; a.m00 = sx; a.m11 = sy; return a;
    }

    static Affine rotate(double angle) {
        double const c = std::cos(angle);
        double const s = std::sin(angle);
        Affine a;
        a.m00 =  c; a.m01 = -s;
        a.m10 =  s; a.m11 =  c;
        return a;
    }

    // `*this * rhs` — applies `rhs` first, then `*this`.
    Affine compose(Affine const& rhs) const {
        Affine r;
        r.m00 = m00 * rhs.m00 + m01 * rhs.m10;
        r.m01 = m00 * rhs.m01 + m01 * rhs.m11;
        r.m02 = m00 * rhs.m02 + m01 * rhs.m12 + m02;
        r.m10 = m10 * rhs.m00 + m11 * rhs.m10;
        r.m11 = m10 * rhs.m01 + m11 * rhs.m11;
        r.m12 = m10 * rhs.m02 + m11 * rhs.m12 + m12;
        return r;
    }

    Point apply_point(double x, double y) const {
        return Point{
            m00 * x + m01 * y + m02,
            m10 * x + m11 * y + m12,
        };
    }

    Point apply_vector(double x, double y) const {
        return Point{
            m00 * x + m01 * y,
            m10 * x + m11 * y,
        };
    }

    // Average linear scaling. For uniform / similarity transforms this
    // is the exact magnification; for non-uniform scales it's a stable
    // scalar approximation suitable for radius / thickness / font-size
    // adjustments.
    double scale_factor() const {
        double const sx = std::sqrt(m00 * m00 + m10 * m10);
        double const sy = std::sqrt(m01 * m01 + m11 * m11);
        return 0.5 * (sx + sy);
    }

    // Net rotation extracted from the upper-left 2×2. Stable for
    // similarity transforms; for non-uniform scales it picks the
    // x-axis direction.
    double rotation() const { return std::atan2(m10, m00); }
};

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

    // Slide the drawing under the cursor by `(dx_canvas, dy_canvas)`
    // canvas pixels. Pure translation — `scale` is untouched.
    void pan(double dx_canvas, double dy_canvas) {
        pad_x += dx_canvas;
        pad_y += dy_canvas;
    }

    // Multiply the scale by `factor`, anchored at the canvas-local
    // point `(fx, fy)` so the world coordinate currently under that
    // point stays under it after the zoom (standard "zoom toward
    // cursor" affine identity). Floors the scale at a tiny positive
    // value so a runaway pinch can't flip the transform inside-out.
    void zoom_at(double factor, double fx, double fy) {
        if (!(factor > 0.0)) return;
        double new_scale = scale * factor;
        if (!(new_scale > 1e-9)) new_scale = 1e-9;
        // Solve for the world point at (fx, fy) under the OLD scale,
        // then re-anchor pad_* so apply(wx, wy) == (fx, fy) under the
        // NEW scale.
        double inv = (scale > 0.0) ? 1.0 / scale : 0.0;
        double wx  = bbox.min_x + (fx - pad_x) * inv;
        double wy  = bbox.max_y - (fy - pad_y) * inv;
        scale = new_scale;
        pad_x = fx - (wx - bbox.min_x) * scale;
        pad_y = fy - (bbox.max_y - wy) * scale;
    }
};

} // namespace cadpp
