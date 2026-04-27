// cad++ — entity → phenotype draw command emitter.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `cmath` declarations come in through `parser.hpp`'s `import std;`
// chain. Including `<cmath>` directly here would re-declare the
// libc++ promote-traits aliases that `import std;` already exports
// (the abi-tag check fires on `__promote_t` — the well-known libc++
// `import std` + `#include <cmath>` collision).

#include "renderer.hpp"

namespace cadpp {

namespace {

constexpr float  kLineThickness = 1.0f;
constexpr double kHalfPi = 1.57079632679489661923;
constexpr double kTwoPi  = 6.28318530717958647692;

inline phenotype::Color to_paint(Color const& c) {
    return phenotype::Color{c.r, c.g, c.b, c.a};
}

} // namespace

void render_lines(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform) {
    for (auto const& l : entities.lines) {
        auto const a = transform.apply(l.a.x, l.a.y);
        auto const b = transform.apply(l.b.x, l.b.y);
        p.line(static_cast<float>(a.x), static_cast<float>(a.y),
               static_cast<float>(b.x), static_cast<float>(b.y),
               kLineThickness, to_paint(l.color));
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
               font_px, to_paint(t.color));
    }
}

void render_arcs(phenotype::Painter& p,
                 Entities const& entities,
                 ViewportTransform const& transform) {
    for (auto const& a : entities.arcs) {
        auto const center_canvas = transform.apply(a.center.x, a.center.y);
        float const r_px = static_cast<float>(a.radius * transform.scale);
        if (r_px < 0.5f) continue;  // sub-pixel — skip
        // CAD's angle convention is y-up CCW; phenotype's `Painter::arc`
        // angle convention follows the canvas's y-down coordinate
        // system (CCW around the canvas frame). Mirroring the angle
        // across y reverses CCW into CW; swapping `start` and `end`
        // restores the original sweep direction in the new frame.
        float const canvas_start = static_cast<float>(-a.end_angle);
        float const canvas_end   = static_cast<float>(-a.start_angle);
        p.arc(static_cast<float>(center_canvas.x),
              static_cast<float>(center_canvas.y),
              r_px,
              canvas_start, canvas_end,
              kLineThickness, to_paint(a.color));
    }
}

namespace {

// AutoCAD bulge → world-frame circular-arc parameters.
//
// `bulge = tan(θ / 4)`, where θ is the included CCW arc angle from
// the start vertex to the end vertex (in CAD's y-up frame). Positive
// bulge sweeps CCW; negative sweeps CW. With the chord vector
// `d = end - start` and chord length `L`, the world-frame arc has:
//   radius = L · (1 + b²) / (4 · |b|)
//   centre = midpoint(start, end) + perp(d) · k
//   k      = (1 - b²) / (4 · b)            (signed; perp is rotated CCW)
struct BulgeArc {
    double cx, cy;       // world-frame centre
    double radius;       // world-frame radius
    double start_angle;  // world-frame CCW (positive bulge) or CW (negative)
    double end_angle;
    bool   ccw;          // true if positive bulge → CCW sweep
};

inline BulgeArc bulge_to_arc(Point const& a, Point const& b, double bulge) {
    BulgeArc out{};
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double abs_b = std::abs(bulge);
    double mid_x = 0.5 * (a.x + b.x);
    double mid_y = 0.5 * (a.y + b.y);
    double k     = (1.0 - bulge * bulge) / (4.0 * bulge);
    out.cx = mid_x - dy * k;
    out.cy = mid_y + dx * k;
    double chord = std::sqrt(dx * dx + dy * dy);
    out.radius = chord * (1.0 + bulge * bulge) / (4.0 * abs_b);
    out.start_angle = std::atan2(a.y - out.cy, a.x - out.cx);
    out.end_angle   = std::atan2(b.y - out.cy, b.x - out.cx);
    out.ccw = bulge > 0.0;
    return out;
}

} // namespace

void render_paths(phenotype::Painter& p,
                  Entities const& entities,
                  ViewportTransform const& transform) {
    // ---- Bulged LWPOLYLINE → MoveTo + (LineTo | ArcTo) chain ----
    for (auto const& bp : entities.bulged_polylines) {
        if (bp.vertices.size() < 2) continue;
        phenotype::PathBuilder pb;

        auto const start_canvas =
            transform.apply(bp.vertices[0].x, bp.vertices[0].y);
        pb.move_to(static_cast<float>(start_canvas.x),
                   static_cast<float>(start_canvas.y));

        std::size_t const n  = bp.vertices.size();
        std::size_t const sn = bp.closed ? n : (n - 1);
        for (std::size_t i = 0; i < sn; ++i) {
            auto const& va = bp.vertices[i];
            auto const& vb = bp.vertices[(i + 1) % n];
            double const bulge =
                (i < bp.bulges.size()) ? bp.bulges[i] : 0.0;
            auto const cb = transform.apply(vb.x, vb.y);

            if (bulge == 0.0) {
                pb.line_to(static_cast<float>(cb.x),
                           static_cast<float>(cb.y));
            } else {
                BulgeArc arc = bulge_to_arc(va, vb, bulge);
                auto const cc = transform.apply(arc.cx, arc.cy);
                double radius_canvas = arc.radius * transform.scale;
                // Y-flip: same convention as render_arcs. CCW in
                // CAD-world corresponds to CW after the y-flip;
                // swap start/end to restore the visual sweep.
                float canvas_start =
                    static_cast<float>(-arc.end_angle);
                float canvas_end   =
                    static_cast<float>(-arc.start_angle);
                pb.arc_to(static_cast<float>(cc.x),
                          static_cast<float>(cc.y),
                          static_cast<float>(radius_canvas),
                          canvas_start, canvas_end);
                // The backend's path dispatcher does not advance the
                // pen across an ArcTo (centre-form arcs do not
                // self-describe their endpoint), so set it explicitly
                // to the next segment's start.
                pb.move_to(static_cast<float>(cb.x),
                           static_cast<float>(cb.y));
            }
        }
        if (bp.closed) pb.close();
        p.stroke_path(pb, kLineThickness, to_paint(bp.color));
    }

    // ---- ELLIPSE → MoveTo + cubic_to per ≤90° quadrant ----
    //
    // Standard 4-control-point cubic Bézier approximation of an ellipse
    // arc. `P(t) = C + U·cos(t) + V·sin(t)` where `U = major_axis` and
    // `V = perp(U) · minor_ratio`. For each chunk `[t0, t1]` with
    // `θ = t1 - t0 ≤ π/2`:
    //   k  = 4/3 · tan(θ / 4)
    //   p0 = P(t0)
    //   p3 = P(t1)
    //   t0_tan = -U·sin(t0) + V·cos(t0)   (parametric tangent at t0)
    //   t3_tan = -U·sin(t1) + V·cos(t1)
    //   p1 = p0 + k · t0_tan
    //   p2 = p3 - k · t3_tan
    //
    // Cubic Béziers are affine-invariant, so we can compute control
    // points in CAD-world space and then transform every point through
    // `ViewportTransform::apply` — the y-flip is automatic.
    for (auto const& e : entities.ellipses) {
        // Major axis vector U; perpendicular V is U rotated 90° CCW
        // (in CAD's y-up frame) scaled by minor_ratio.
        double const ux =  e.major_axis.x;
        double const uy =  e.major_axis.y;
        double const vx = -uy * e.minor_ratio;
        double const vy =  ux * e.minor_ratio;

        // Normalise the parametric range. AutoCAD ELLIPSE end_param
        // wraps past 2π for closed loops; clamp to a sensible sweep.
        double t0 = e.start_param;
        double t1 = e.end_param;
        // Treat (start == end) as a full ellipse — matches the
        // common DWG convention for closed ellipses.
        if (std::abs(t1 - t0) < 1e-9) t1 = t0 + kTwoPi;
        if (t1 < t0) t1 += kTwoPi;

        auto eval = [&](double t) -> Point {
            return Point{
                e.center.x + ux * std::cos(t) + vx * std::sin(t),
                e.center.y + uy * std::cos(t) + vy * std::sin(t),
            };
        };
        auto eval_tangent = [&](double t) -> Point {
            return Point{
                -ux * std::sin(t) + vx * std::cos(t),
                -uy * std::sin(t) + vy * std::cos(t),
            };
        };

        phenotype::PathBuilder pb;
        Point const p0_world = eval(t0);
        Point const p0_canvas = transform.apply(p0_world.x, p0_world.y);
        pb.move_to(static_cast<float>(p0_canvas.x),
                   static_cast<float>(p0_canvas.y));

        // Subdivide into chunks of ≤ π/2 so the cubic approximation
        // error stays bounded (~10⁻³ relative for the worst case).
        double tA = t0;
        while (tA < t1) {
            double tB = tA + kHalfPi;
            if (tB > t1) tB = t1;
            double const theta = tB - tA;
            double const k = (4.0 / 3.0) * std::tan(theta * 0.25);
            Point const pA       = eval(tA);
            Point const pB       = eval(tB);
            Point const tan_tA   = eval_tangent(tA);
            Point const tan_tB   = eval_tangent(tB);
            Point const c1_world = Point{pA.x + k * tan_tA.x,
                                         pA.y + k * tan_tA.y};
            Point const c2_world = Point{pB.x - k * tan_tB.x,
                                         pB.y - k * tan_tB.y};
            auto const c1 = transform.apply(c1_world.x, c1_world.y);
            auto const c2 = transform.apply(c2_world.x, c2_world.y);
            auto const cb = transform.apply(pB.x, pB.y);
            pb.cubic_to(static_cast<float>(c1.x), static_cast<float>(c1.y),
                        static_cast<float>(c2.x), static_cast<float>(c2.y),
                        static_cast<float>(cb.x), static_cast<float>(cb.y));
            tA = tB;
        }
        p.stroke_path(pb, kLineThickness, to_paint(e.color));
    }
}

} // namespace cadpp
