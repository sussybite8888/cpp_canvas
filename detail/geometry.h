#pragma once
// Path geometry shared by every backend: flattening a Path2D into polygons,
// expanding strokes and dashes into fillable outlines, bounds and hit tests.
//
// Private to the library: included by exactly one translation unit (the
// backend's), so everything here has internal linkage.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "../canvas.h"

namespace canvas_detail {
namespace {

constexpr float kTau = 6.28318530718f;
constexpr float kPi = 3.14159265359f;
using Matrix = std::array<float, 6>;
inline constexpr Matrix kIdentity{1, 0, 0, 1, 0, 0};

// Polylines: `pts` holds x,y pairs, `ends[i]` is one past the last point of
// contour i, `closed[i]` whether it was closed by closePath (or is a rect).
struct Poly {
    std::vector<float> pts;
    std::vector<int> ends;
    std::vector<char> closed;
    void reset() { pts.clear(); ends.clear(); closed.clear(); }
    int begin() const { return ends.empty() ? 0 : ends.back(); }
    int points() const { return static_cast<int>(pts.size() / 2); }
    // A lone moveTo is not a subpath anything draws: dropped. A moveTo and a
    // lineTo to the same point is two points, which a stroke caps as a dot.
    void finish(bool isClosed) {
        const int n = points();
        if (n - begin() < 2) { pts.resize(static_cast<std::size_t>(begin()) * 2); return; }
        ends.push_back(n);
        closed.push_back(isClosed ? 1 : 0);
    }
};

inline bool isIdentity(const Matrix& m) { return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[4] == 0 && m[5] == 0; }
// Axis-aligned: no rotation or skew (mirroring allowed).
inline bool isAxisAligned(const Matrix& m) { return m[1] == 0 && m[2] == 0; }
inline bool finiteMatrix(const Matrix& m) {
    for (float v : m) if (!std::isfinite(v)) return false;
    return true;
}
// sqrt(|det|): the geometric-mean scale.
inline float matrixScale(const Matrix& m) {
    const float d = std::abs(m[0] * m[3] - m[1] * m[2]);
    return d > 0 ? std::sqrt(d) : 1e-3f;
}
// The largest singular value: how much the matrix can stretch a unit length.
// Flattening tolerances use this, so an anisotropic scale is never
// under-subdivided along its long axis.
inline float matrixMaxScale(const Matrix& m) {
    const float a = m[0], b = m[1], c = m[2], d = m[3];
    const float s = a * a + b * b + c * c + d * d;
    const float t = std::sqrt(std::max(0.f, (a * a + b * b - c * c - d * d) * (a * a + b * b - c * c - d * d) +
                                                4 * (a * c + b * d) * (a * c + b * d)));
    const float r = std::sqrt(std::max(0.f, (s + t) / 2));
    return r > 1e-6f ? r : 1e-6f;
}
inline Matrix multiply(const Matrix& m, const Matrix& n) {  // m * n (apply n first)
    return {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
            m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}
inline bool invert(const Matrix& m, Matrix& out) {
    const double det = static_cast<double>(m[0]) * m[3] - static_cast<double>(m[1]) * m[2];
    if (!(std::abs(det) > 1e-12) || !std::isfinite(det)) return false;
    const double ia = m[3] / det, ib = -m[1] / det, ic = -m[2] / det, id = m[0] / det;
    out = {static_cast<float>(ia), static_cast<float>(ib), static_cast<float>(ic), static_cast<float>(id),
           static_cast<float>(-(ia * m[4] + ic * m[5])), static_cast<float>(-(ib * m[4] + id * m[5]))};
    return true;
}

// Device-space flattening tolerance: the furthest a chord may stray from
// the true curve. A quarter pixel is what Skia uses for fills; this is a bit
// tighter so curved edges stay smooth under the exact-area rasterizer.
constexpr float kFlattenTolerance = 0.12f;

// Segment counts (Wang's formula for Béziers, exact sagitta for arcs).
inline int quadSteps(float x0, float y0, float x1, float y1, float x2, float y2, float scale) {
    const float dx = x0 - 2 * x1 + x2, dy = y0 - 2 * y1 + y2;
    const float dd = std::sqrt(dx * dx + dy * dy) * scale;
    const float n = std::ceil(std::sqrt(dd / (4 * kFlattenTolerance)));
    return static_cast<int>(std::clamp(std::isfinite(n) ? n : 1.f, 1.f, 256.f));
}
inline int cubicSteps(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, float scale) {
    const float ax = x0 - 2 * x1 + x2, ay = y0 - 2 * y1 + y2, bx = x1 - 2 * x2 + x3, by = y1 - 2 * y2 + y3;
    const float dd = std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by)) * scale;
    const float n = std::ceil(std::sqrt(dd * 0.75f / kFlattenTolerance));
    return static_cast<int>(std::clamp(std::isfinite(n) ? n : 1.f, 1.f, 512.f));
}
inline int arcSteps(float sweep, float radius, float scale) {
    const float r = radius * scale;
    if (!(r > kFlattenTolerance)) return std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 2))));
    const float step = 2 * std::acos(std::clamp(1 - kFlattenTolerance / r, -1.f, 1.f));
    const float n = std::ceil(std::abs(sweep) / std::max(step, 1e-3f));
    return static_cast<int>(std::clamp(std::isfinite(n) ? n : 4.f, 1.f, 2048.f));
}

// Normalises an arc sweep the way the canvas does: clockwise sweeps land in
// [0, 2pi], anticlockwise in [-2pi, 0], and a request of a full turn or more
// is exactly one turn.
inline float arcSweep(float a0, float a1, bool ccw) {
    float sweep = a1 - a0;
    if (!ccw) {
        if (sweep >= kTau) return kTau;
        sweep = std::fmod(sweep, kTau);
        if (sweep < 0) sweep += kTau;
    } else {
        if (sweep <= -kTau) return -kTau;
        sweep = std::fmod(sweep, kTau);
        if (sweep > 0) sweep -= kTau;
    }
    return sweep;
}

// The tangent arc arcTo() describes, per the canvas spec: from the current
// point P0 toward P1, turning onto P1->P2 with radius r. Returns false when the
// spec says it degenerates into a straight line to P1.
struct ArcToResult { float t1x, t1y, t2x, t2y, cx, cy, a0, a1; bool ccw; };
inline bool arcToGeometry(float x0, float y0, float x1, float y1, float x2, float y2, float r, ArcToResult& out) {
    if (!(r > 0)) return false;
    const float ux = x0 - x1, uy = y0 - y1, wx = x2 - x1, wy = y2 - y1;
    const float lu = std::hypot(ux, uy), lw = std::hypot(wx, wy);
    if (lu < 1e-6f || lw < 1e-6f) return false;
    const float cross = ux * wy - uy * wx;
    if (std::abs(cross) < 1e-6f * lu * lw) return false;  // collinear
    const float cosA = std::clamp((ux * wx + uy * wy) / (lu * lw), -1.f, 1.f);
    const float half = std::acos(cosA) / 2;
    const float dist = r / std::tan(half);
    const float ax = ux / lu, ay = uy / lu, bx = wx / lw, by = wy / lw;
    out.t1x = x1 + ax * dist; out.t1y = y1 + ay * dist;
    out.t2x = x1 + bx * dist; out.t2y = y1 + by * dist;
    // The centre is off the corner along the bisector.
    float mx = ax + bx, my = ay + by;
    const float ml = std::hypot(mx, my);
    if (ml < 1e-9f) return false;
    mx /= ml; my /= ml;
    const float cd = r / std::sin(half);
    out.cx = x1 + mx * cd; out.cy = y1 + my * cd;
    out.a0 = std::atan2(out.t1y - out.cy, out.t1x - out.cx);
    out.a1 = std::atan2(out.t2y - out.cy, out.t2x - out.cx);
    // Turning left (in y-down screen space, a positive cross of P0->P1 and
    // P1->P2) runs the arc anticlockwise.
    const float turn = (x1 - x0) * (y2 - y1) - (y1 - y0) * (x2 - x1);
    out.ccw = turn < 0;
    return true;
}

// Flattens `path` under `m` into polylines. `scale` is how many device pixels
// one unit of the OUTPUT space is worth (1 when m maps to device pixels; the
// matrix's scale when flattening in local space for a stroke).
//
// Current-point rules follow the canvas spec: a lineTo or curve into an empty
// path starts a subpath at its first point; after closePath, rect() or
// roundRect() the next segment starts from that subpath's start point.
inline void flatten(const Path2D::Segment* segs, std::size_t count, const Matrix& m, float scale, Poly& out) {
    out.reset();
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false, hasCurrent = false;
    const auto emit = [&](float x, float y) {
        out.pts.push_back(m[0] * x + m[2] * y + m[4]);
        out.pts.push_back(m[1] * x + m[3] * y + m[5]);
    };
    const auto start = [&](float x, float y) {
        if (open) out.finish(false);
        emit(x, y);
        open = true; hasCurrent = true;
        cx = sx = x; cy = sy = y;
    };
    // The point a segment that continues the path begins at: the current
    // point, reopening a subpath after a close; or `fx,fy` in an empty path.
    const auto ensure = [&](float fx, float fy) {
        if (open) return;
        if (hasCurrent) start(cx, cy); else start(fx, fy);
    };
    const auto lineTo = [&](float x, float y) { ensure(x, y); emit(x, y); cx = x; cy = y; };
    const auto arcPoints = [&](float ex, float ey, float rx, float ry, float rot, float a0, float sweep) {
        const float cs = std::cos(rot), sn = std::sin(rot);
        const auto at = [&](float t, float& ox, float& oy) {
            const float px = rx * std::cos(t), py = ry * std::sin(t);
            ox = ex + px * cs - py * sn; oy = ey + px * sn + py * cs;
        };
        float px, py;
        at(a0, px, py);
        if (open || hasCurrent) { ensure(px, py); emit(px, py); } else start(px, py);
        const int n = arcSteps(sweep, std::max(rx, ry), scale);
        for (int i = 1; i <= n; ++i) { at(a0 + sweep * static_cast<float>(i) / n, px, py); emit(px, py); }
        cx = px; cy = py;
    };
    for (std::size_t si = 0; si < count; ++si) {
        const Path2D::Segment& s = segs[si];
        const float* v = s.v;
        switch (s.command) {
        case Path2D::Command::Move: start(v[0], v[1]); break;
        case Path2D::Command::Line: lineTo(v[0], v[1]); break;
        case Path2D::Command::Quadratic: {
            ensure(v[0], v[1]);
            const float x0 = cx, y0 = cy;
            const int n = quadSteps(x0, y0, v[0], v[1], v[2], v[3], scale);
            for (int i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) / n, u = 1 - t;
                emit(u * u * x0 + 2 * u * t * v[0] + t * t * v[2], u * u * y0 + 2 * u * t * v[1] + t * t * v[3]);
            }
            cx = v[2]; cy = v[3];
            break;
        }
        case Path2D::Command::Bezier: {
            ensure(v[0], v[1]);
            const float x0 = cx, y0 = cy;
            const int n = cubicSteps(x0, y0, v[0], v[1], v[2], v[3], v[4], v[5], scale);
            for (int i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) / n, u = 1 - t, a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
                emit(a * x0 + b * v[0] + c * v[2] + d * v[4], a * y0 + b * v[1] + c * v[3] + d * v[5]);
            }
            cx = v[4]; cy = v[5];
            break;
        }
        case Path2D::Command::Arc:
        case Path2D::Command::Ellipse: {
            const bool circle = s.command == Path2D::Command::Arc;
            const float rx = std::abs(v[2]), ry = circle ? std::abs(v[2]) : std::abs(v[3]);
            const float rot = circle ? 0.f : v[4], a0 = circle ? v[3] : v[5], a1 = circle ? v[4] : v[6];
            arcPoints(v[0], v[1], rx, ry, rot, a0, arcSweep(a0, a1, s.counterClockwise));
            break;
        }
        case Path2D::Command::ArcTo: {
            // An arcTo into an empty path is a moveTo to its first point.
            if (!open && !hasCurrent) { start(v[0], v[1]); break; }
            ensure(v[0], v[1]);
            ArcToResult a;
            if (!arcToGeometry(cx, cy, v[0], v[1], v[2], v[3], std::abs(v[4]), a)) { lineTo(v[0], v[1]); break; }
            lineTo(a.t1x, a.t1y);
            float sweep = a.a1 - a.a0;
            if (a.ccw) { while (sweep > 0) sweep -= kTau; } else { while (sweep < 0) sweep += kTau; }
            arcPoints(a.cx, a.cy, std::abs(v[4]), std::abs(v[4]), 0, a.a0, sweep);
            break;
        }
        case Path2D::Command::Rect: {
            if (open) { out.finish(false); open = false; }
            emit(v[0], v[1]); emit(v[0] + v[2], v[1]); emit(v[0] + v[2], v[1] + v[3]); emit(v[0], v[1] + v[3]);
            out.finish(true);
            cx = sx = v[0]; cy = sy = v[1]; hasCurrent = true;
            break;
        }
        case Path2D::Command::RoundRect: {
            if (open) { out.finish(false); open = false; }
            const float x = std::min(v[0], v[0] + v[2]), y = std::min(v[1], v[1] + v[3]);
            const float w = std::abs(v[2]), h = std::abs(v[3]), r = std::clamp(v[4], 0.f, std::min(w, h) / 2);
            const int n = std::max(1, arcSteps(kPi / 2, r, scale));
            const float cxs[4] = {x + w - r, x + w - r, x + r, x + r}, cys[4] = {y + r, y + h - r, y + h - r, y + r};
            for (int corner = 0; corner < 4; ++corner) {
                const float a = -kPi / 2 + corner * (kPi / 2);
                for (int i = 0; i <= n; ++i) {
                    const float t = a + static_cast<float>(i) * (kPi / 2) / n;
                    emit(cxs[corner] + std::cos(t) * r, cys[corner] + std::sin(t) * r);
                }
            }
            out.finish(true);
            cx = sx = v[0]; cy = sy = v[1]; hasCurrent = true;
            break;
        }
        case Path2D::Command::Close:
            if (open) { out.finish(true); open = false; }
            cx = sx; cy = sy;
            break;
        }
    }
    if (open) out.finish(false);
}
inline void flatten(const Path2D& path, const Matrix& m, float scale, Poly& out) {
    flatten(path.segments().data(), path.segments().size(), m, scale, out);
}

// The pixel box a polygon can touch, intersected with [cx0,cx1) x [cy0,cy1).
inline bool polyBounds(const Poly& p, int cx0, int cy0, int cx1, int cy1, int& x0, int& y0, int& x1, int& y1) {
    if (p.pts.empty()) return false;
    float lo = 1e30f, hi = -1e30f, top = 1e30f, bottom = -1e30f;
    for (std::size_t i = 0; i < p.pts.size(); i += 2) {
        const float x = p.pts[i], y = p.pts[i + 1];
        if (!(std::isfinite(x) && std::isfinite(y))) continue;
        lo = std::min(lo, x); hi = std::max(hi, x); top = std::min(top, y); bottom = std::max(bottom, y);
    }
    if (hi < lo) return false;
    const auto clampi = [](float v) { return static_cast<int>(std::clamp(v, -1e9f, 1e9f)); };
    x0 = std::max(cx0, clampi(std::floor(lo))); x1 = std::min(cx1, clampi(std::ceil(hi)) + 1);
    y0 = std::max(cy0, clampi(std::floor(top))); y1 = std::min(cy1, clampi(std::ceil(bottom)) + 1);
    return x0 < x1 && y0 < y1;
}

inline void pushContour(Poly& out, const float* pts, std::size_t n) {
    if (n < 3) return;
    double area = 0;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++)
        area += static_cast<double>(pts[2 * j]) * pts[2 * i + 1] - static_cast<double>(pts[2 * i]) * pts[2 * j + 1];
    if (area > 0) for (std::size_t i = n; i-- > 0;) { out.pts.push_back(pts[2 * i]); out.pts.push_back(pts[2 * i + 1]); }
    else for (std::size_t i = 0; i < n; ++i) { out.pts.push_back(pts[2 * i]); out.pts.push_back(pts[2 * i + 1]); }
    out.finish(true);
}
inline void pushContour(Poly& out, const std::vector<float>& pts) { pushContour(out, pts.data(), pts.size() / 2); }

// A pie wedge of the circle (x,y,r) from angle a0 sweeping `sweep`, centre
// included -- the part of a round join or cap that the segment quads do not
// already cover.
inline void pushWedge(Poly& out, float x, float y, float r, float a0, float sweep, float scale, std::vector<float>& pts) {
    const int n = std::max(1, arcSteps(sweep, r, scale));
    pts.clear();
    pts.push_back(x); pts.push_back(y);
    for (int i = 0; i <= n; ++i) {
        const float t = a0 + sweep * static_cast<float>(i) / n;
        pts.push_back(x + std::cos(t) * r); pts.push_back(y + std::sin(t) * r);
    }
    pushContour(out, pts);
}
inline void pushDisc(Poly& out, float x, float y, float r, float scale, std::vector<float>& pts) {
    const int n = std::max(8, arcSteps(kTau, r, scale));
    pts.clear();
    for (int i = 0; i < n; ++i) { const float t = kTau * i / n; pts.push_back(x + std::cos(t) * r); pts.push_back(y + std::sin(t) * r); }
    pushContour(out, pts);
}

// Expands polylines into the outline of their stroke. Segment quads, join
// wedges and caps are separately closed, equally wound contours, so a nonzero
// fill of the lot is exactly their union. lineCap: 0 butt, 1 round, 2 square;
// lineJoin: 0 miter, 1 round, 2 bevel.
inline void strokeOutline(const Poly& src, float hw, int cap, int join, float miterLimit, float scale, Poly& out) {
    out.reset();
    if (!(hw > 0)) return;
    static thread_local std::vector<float> pts, quad, wedge;
    // Points closer than a hundredth of a device pixel are one point: a
    // segment that short has no direction worth joining to, and joining to
    // its float noise is how a miter turns into a spike.
    const float eps = 0.01f / std::max(scale, 1e-6f);
    int first = 0;
    for (std::size_t c = 0; c < src.ends.size(); ++c) {
        const int last = src.ends[c];
        const bool closed = src.closed[c] != 0;
        pts.clear();
        for (int i = first; i < last; ++i) {
            const float x = src.pts[2 * i], y = src.pts[2 * i + 1];
            if (!(std::isfinite(x) && std::isfinite(y))) continue;
            if (pts.size() >= 2 && std::abs(pts[pts.size() - 2] - x) < eps && std::abs(pts.back() - y) < eps) continue;
            pts.push_back(x); pts.push_back(y);
        }
        first = last;
        if (closed && pts.size() >= 4 && std::abs(pts[0] - pts[pts.size() - 2]) < eps && std::abs(pts[1] - pts.back()) < eps)
            pts.resize(pts.size() - 2);
        const int n = static_cast<int>(pts.size() / 2);
        if (n == 0) continue;
        if (n == 1) {
            // A zero-length subpath: only round and square caps show it.
            if (cap == 1) pushDisc(out, pts[0], pts[1], hw, scale, wedge);
            else if (cap == 2) {
                quad = {pts[0] - hw, pts[1] - hw, pts[0] + hw, pts[1] - hw, pts[0] + hw, pts[1] + hw, pts[0] - hw, pts[1] + hw};
                pushContour(out, quad);
            }
            continue;
        }
        const int segments = closed ? n : n - 1;
        // Directions and unit normals of each segment.
        static thread_local std::vector<float> dir;
        dir.assign(static_cast<std::size_t>(segments) * 3, 0.f);
        for (int i = 0; i < segments; ++i) {
            const int j = (i + 1) % n;
            const float dx = pts[2 * j] - pts[2 * i], dy = pts[2 * j + 1] - pts[2 * i + 1];
            const float len = std::hypot(dx, dy);
            dir[3 * i] = len > 0 ? dx / len : 0; dir[3 * i + 1] = len > 0 ? dy / len : 0; dir[3 * i + 2] = len;
        }
        // A join is SMOOTH when it turns so little that the miter, bevel and
        // round joins differ by under 0.05 device px, and the inner offset
        // lines meet well inside both segments. Runs of segments joined
        // smoothly become one ribbon contour -- offsets out along the left,
        // back along the right, meeting at the miter points -- instead of a
        // quad per segment and a wedge per join: a stroked circle is two
        // rings, where it was hundreds of overlapping pieces.
        const float hwDev = hw * scale;
        const float maxTurn = std::min(0.6f, 2.f * std::sqrt(2.f * 0.05f / std::max(hwDev, 1e-3f)));
        static thread_local std::vector<char> smooth;
        smooth.assign(static_cast<std::size_t>(n), 0);
        const auto joinIndex = [&](int k) { return closed ? k : k; };
        (void)joinIndex;
        for (int k = closed ? 0 : 1; k < (closed ? n : n - 1); ++k) {
            const int a = (k - 1 + segments) % segments, b = k % segments;
            const float d0x = dir[3 * a], d0y = dir[3 * a + 1], d1x = dir[3 * b], d1y = dir[3 * b + 1];
            const float dot = d0x * d1x + d0y * d1y, cross = d0x * d1y - d0y * d1x;
            const float turn = std::atan2(std::abs(cross), dot);
            const float minLen = std::min(dir[3 * a + 2], dir[3 * b + 2]);
            smooth[static_cast<std::size_t>(k)] = turn <= maxTurn && hw * std::tan(turn / 2) <= 0.45f * minLen ? 1 : 0;
        }
        // The miter point on the left (+) or right (-) of vertex k.
        const auto miterAt = [&](int k, float side, float& ox, float& oy) {
            const int a = (k - 1 + segments) % segments, b = k % segments;
            const float n0x = -dir[3 * a + 1], n0y = dir[3 * a], n1x = -dir[3 * b + 1], n1y = dir[3 * b];
            const float k1 = hw / std::max(1e-6f, 1 + n0x * n1x + n0y * n1y);
            ox = pts[2 * k] + side * (n0x + n1x) * k1; oy = pts[2 * (k % n) + 1] + side * (n0y + n1y) * k1;
        };
        bool allSmooth = closed;
        for (int k = 0; closed && k < n; ++k) allSmooth = allSmooth && smooth[static_cast<std::size_t>(k)];
        if (allSmooth) {
            // A closed, smooth outline: two rings, the inner one reversed.
            quad.clear();
            for (int k = 0; k < n; ++k) { float x, y; miterAt(k, 1, x, y); quad.push_back(x); quad.push_back(y); }
            for (std::size_t i = 0; i < quad.size(); i += 2) out.pts.push_back(quad[i]), out.pts.push_back(quad[i + 1]);
            out.finish(true);
            quad.clear();
            for (int k = n; k-- > 0;) { float x, y; miterAt(k, -1, x, y); quad.push_back(x); quad.push_back(y); }
            for (std::size_t i = 0; i < quad.size(); i += 2) out.pts.push_back(quad[i]), out.pts.push_back(quad[i + 1]);
            out.finish(true);
            continue;
        }
        // Open runs: a run starts at a segment whose start vertex is not a
        // smooth join and takes segments while the next join is smooth.
        int s0 = 0;
        if (closed) { while (s0 < segments && smooth[static_cast<std::size_t>(s0)]) ++s0; }
        for (int done = 0; done < segments;) {
            const int first = (s0 + done) % segments;
            int count = 1;
            while (done + count < segments && smooth[static_cast<std::size_t>((first + count) % n)]) ++count;
            // Left side forward, right side back.
            quad.clear();
            const auto startPoint = [&](int seg, float side, float& x, float& y) {
                x = pts[2 * seg] + side * -dir[3 * seg + 1] * hw; y = pts[2 * seg + 1] + side * dir[3 * seg] * hw;
            };
            const auto endPoint = [&](int seg, float side, float& x, float& y) {
                const int j = (seg + 1) % n;
                x = pts[2 * j] + side * -dir[3 * seg + 1] * hw; y = pts[2 * j + 1] + side * dir[3 * seg] * hw;
            };
            float x, y;
            startPoint(first, 1, x, y); quad.push_back(x); quad.push_back(y);
            for (int i = 1; i < count; ++i) { miterAt((first + i) % n, 1, x, y); quad.push_back(x); quad.push_back(y); }
            endPoint((first + count - 1) % segments, 1, x, y); quad.push_back(x); quad.push_back(y);
            endPoint((first + count - 1) % segments, -1, x, y); quad.push_back(x); quad.push_back(y);
            for (int i = count - 1; i >= 1; --i) { miterAt((first + i) % n, -1, x, y); quad.push_back(x); quad.push_back(y); }
            startPoint(first, -1, x, y); quad.push_back(x); quad.push_back(y);
            if (dir[3 * first + 2] > 1e-9f || count > 1) {
                for (std::size_t i = 0; i < quad.size(); i += 2) out.pts.push_back(quad[i]), out.pts.push_back(quad[i + 1]);
                out.finish(true);
            }
            done += count;
        }
        const int joins = closed ? n : n - 1;
        for (int k = closed ? 0 : 1; k < joins; ++k) {
            if (smooth[static_cast<std::size_t>(k)]) continue;
            const int prev = (k - 1 + n) % n, cur = k, nxt = (k + 1) % n;
            const float px = pts[2 * cur], py = pts[2 * cur + 1];
            float d0x = px - pts[2 * prev], d0y = py - pts[2 * prev + 1], d1x = pts[2 * nxt] - px, d1y = pts[2 * nxt + 1] - py;
            const float l0 = std::hypot(d0x, d0y), l1 = std::hypot(d1x, d1y);
            if (l0 < 1e-9f || l1 < 1e-9f) continue;
            d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
            const float cross = d0x * d1y - d0y * d1x;
            const float dot = d0x * d1x + d0y * d1y;
            if (std::abs(cross) < 1e-6f && dot > 0) continue;  // straight on: nothing to join
            const float side = cross > 0 ? -1.f : 1.f;
            const float o0x = -d0y * hw * side, o0y = d0x * hw * side, o1x = -d1y * hw * side, o1y = d1x * hw * side;
            if (join == 1) {
                // Only the outer pie between the two segment ends: the rest of
                // the disc lies inside the quads. A flattened curve turns a
                // hair at every vertex, so this is usually one triangle where
                // a whole disc used to be dozens of edges.
                const float a0 = std::atan2(o0y, o0x);
                float sweep = std::atan2(o1y, o1x) - a0;
                while (sweep > kPi) sweep -= kTau;
                while (sweep < -kPi) sweep += kTau;
                pushWedge(out, px, py, hw, a0, sweep, scale, wedge);
                continue;
            }
            // Only the OUTER wedge is emitted, and only when no miter quad
            // takes its place: the inner one lies inside the overlap of the
            // two segment quads.
            bool mitered = false;
            if (join == 0) {
                const float denom = -cross;
                if (std::abs(denom) >= 1e-9f) {
                    const float rx = o1x - o0x, ry = o1y - o0y;
                    const float t = (rx * (-d1y) - ry * (-d1x)) / denom;
                    const float qx = px + o0x + t * d0x, qy = py + o0y + t * d0y;
                    if (std::hypot(qx - px, qy - py) <= miterLimit * hw) {
                        quad = {px, py, px + o0x, py + o0y, qx, qy, px + o1x, py + o1y};
                        pushContour(out, quad);
                        mitered = true;
                    }
                }
            }
            if (!mitered) { quad = {px, py, px + o0x, py + o0y, px + o1x, py + o1y}; pushContour(out, quad); }
        }
        if (closed || cap == 0) continue;
        for (int e = 0; e < 2; ++e) {
            const int at = e == 0 ? 0 : n - 1;
            const int other = e == 0 ? 1 : n - 2;
            float dx = pts[2 * at] - pts[2 * other], dy = pts[2 * at + 1] - pts[2 * other + 1];
            const float len = std::hypot(dx, dy);
            if (len < 1e-9f) continue;
            dx /= len; dy /= len;
            const float ex = pts[2 * at], ey = pts[2 * at + 1];
            if (cap == 1) {
                // The outward half-disc.
                pushWedge(out, ex, ey, hw, std::atan2(-dx, dy), kPi, scale, wedge);
                continue;
            }
            const float nx = -dy * hw, ny = dx * hw;
            quad = {ex + nx, ey + ny, ex + nx + dx * hw, ey + ny + dy * hw, ex - nx + dx * hw, ey - ny + dy * hw, ex - nx, ey - ny};
            pushContour(out, quad);
        }
    }
}

inline void applyDash(const Poly& src, const std::vector<float>& dash, float offset, Poly& out) {
    out.reset();
    float total = 0;
    for (float d : dash) total += std::max(0.f, d);
    if (total <= 1e-6f) { out = src; return; }
    const std::size_t count = dash.size();
    int first = 0;
    for (std::size_t c = 0; c < src.ends.size(); ++c) {
        const int last = src.ends[c];
        const bool closed = src.closed[c] != 0;
        std::size_t index = 0;
        float remaining = 0;
        bool on = true;
        const float period = total * ((count & 1) ? 2 : 1);
        float carry = std::fmod(offset, period);
        if (carry < 0) carry += period;
        while (carry > 0) {
            const float d = std::max(0.f, dash[index % count]);
            if (carry < d) { remaining = d - carry; carry = 0; }
            else { carry -= d; ++index; on = !on; }
        }
        if (remaining <= 0) remaining = std::max(0.f, dash[index % count]);
        bool started = false;
        const int totalPoints = closed ? (last - first + 1) : (last - first);
        // A cap budget: dashes of a hundredth of a pixel along a long path are
        // millions of pieces that draw nothing visible.
        int pieces = 0;
        for (int i = 1; i < totalPoints && pieces < 1000000; ++i) {
            const int a = first + ((i - 1) % (last - first)), b = first + (i % (last - first));
            float ax = src.pts[2 * a], ay = src.pts[2 * a + 1];
            const float bx = src.pts[2 * b], by = src.pts[2 * b + 1];
            float segment = std::hypot(bx - ax, by - ay);
            while (segment > 1e-9f) {
                if (on && !started) { out.pts.push_back(ax); out.pts.push_back(ay); started = true; }
                if (remaining >= segment) {
                    remaining -= segment;
                    if (on) { out.pts.push_back(bx); out.pts.push_back(by); }
                    segment = 0;
                } else {
                    const float t = remaining / segment;
                    const float mx = ax + (bx - ax) * t, my = ay + (by - ay) * t;
                    if (on) { out.pts.push_back(mx); out.pts.push_back(my); out.finish(false); started = false; ++pieces; }
                    ax = mx; ay = my; segment -= remaining;
                    ++index; on = !on;
                    remaining = std::max(1e-6f, dash[index % count]);
                }
            }
        }
        if (started) out.finish(false);
        first = last;
    }
}

inline void transformPoly(const Poly& in, const Matrix& m, Poly& out) {
    out.reset();
    out.pts.resize(in.pts.size());
    for (std::size_t i = 0; i < in.pts.size(); i += 2) {
        const float x = in.pts[i], y = in.pts[i + 1];
        out.pts[i] = m[0] * x + m[2] * y + m[4];
        out.pts[i + 1] = m[1] * x + m[3] * y + m[5];
    }
    out.ends = in.ends;
    out.closed = in.closed;
}

// Winding/crossing test of a point against closed polygons.
inline bool pointInPoly(const Poly& p, float x, float y, bool evenOdd) {
    int winding = 0, crossings = 0, first = 0;
    for (std::size_t c = 0; c < p.ends.size(); ++c) {
        const int last = p.ends[c];
        for (int i = first; i < last; ++i) {
            const int j = (i + 1 == last) ? first : i + 1;
            const float ax = p.pts[2 * i], ay = p.pts[2 * i + 1], bx = p.pts[2 * j], by = p.pts[2 * j + 1];
            if ((ay > y) == (by > y)) continue;
            if (x < ax + (y - ay) * (bx - ax) / (by - ay)) { ++crossings; winding += by > ay ? 1 : -1; }
        }
        first = last;
    }
    return evenOdd ? (crossings & 1) != 0 : winding != 0;
}

// The stroke outline of `path` in device space (dash, caps, joins applied).
struct StrokeParams { float width = 1, miterLimit = 10, dashOffset = 0; int cap = 0, join = 0; const std::vector<float>* dash = nullptr; };
inline void strokeToDevice(const Path2D::Segment* segs, std::size_t count, const Matrix& m, const StrokeParams& sp, Poly& flat, Poly& dashed, Poly& outline, Poly& device) {
    const float scale = matrixMaxScale(m);
    flatten(segs, count, kIdentity, scale, flat);
    const Poly* source = &flat;
    if (sp.dash && !sp.dash->empty()) { applyDash(flat, *sp.dash, sp.dashOffset, dashed); source = &dashed; }
    strokeOutline(*source, sp.width / 2, sp.cap, sp.join, sp.miterLimit, scale, outline);
    transformPoly(outline, m, device);
}

// Conservative device-space bounds of a path without flattening it: every
// point a segment can reach is inside the box of its control points (curves)
// or of its whole circle (arcs). Used to cull and to size layers.
struct Box {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool empty() const { return !(x0 <= x1 && y0 <= y1); }
    void add(float x, float y) {
        if (!(std::isfinite(x) && std::isfinite(y))) return;
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    void add(const Box& b) { if (!b.empty()) { add(b.x0, b.y0); add(b.x1, b.y1); } }
    void outset(float d) { if (!empty()) { x0 -= d; y0 -= d; x1 += d; y1 += d; } }
    void intersect(const Box& b) { x0 = std::max(x0, b.x0); y0 = std::max(y0, b.y0); x1 = std::min(x1, b.x1); y1 = std::min(y1, b.y1); }
};
inline void addMapped(Box& b, const Matrix& m, float x, float y) { b.add(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]); }
inline void addMappedRect(Box& b, const Matrix& m, float x0, float y0, float x1, float y1) {
    addMapped(b, m, x0, y0); addMapped(b, m, x1, y0); addMapped(b, m, x1, y1); addMapped(b, m, x0, y1);
}
inline Box pathBounds(const Path2D::Segment* segs, std::size_t count, const Matrix& m) {
    Box b;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool has = false;
    for (std::size_t si = 0; si < count; ++si) {
        const Path2D::Segment& s = segs[si];
        const float* v = s.v;
        switch (s.command) {
        case Path2D::Command::Move: addMapped(b, m, v[0], v[1]); cx = sx = v[0]; cy = sy = v[1]; has = true; break;
        case Path2D::Command::Line: addMapped(b, m, v[0], v[1]); cx = v[0]; cy = v[1]; has = true; break;
        case Path2D::Command::Quadratic:
            if (has) addMapped(b, m, cx, cy);
            addMapped(b, m, v[0], v[1]); addMapped(b, m, v[2], v[3]); cx = v[2]; cy = v[3]; has = true; break;
        case Path2D::Command::Bezier:
            if (has) addMapped(b, m, cx, cy);
            addMapped(b, m, v[0], v[1]); addMapped(b, m, v[2], v[3]); addMapped(b, m, v[4], v[5]); cx = v[4]; cy = v[5]; has = true; break;
        case Path2D::Command::Arc: case Path2D::Command::Ellipse: {
            const bool circle = s.command == Path2D::Command::Arc;
            const float r = circle ? std::abs(v[2]) : std::max(std::abs(v[2]), std::abs(v[3]));
            if (has) addMapped(b, m, cx, cy);
            addMappedRect(b, m, v[0] - r, v[1] - r, v[0] + r, v[1] + r);
            const float a1 = circle ? v[4] : v[6];
            const float rx = std::abs(v[2]), ry = circle ? rx : std::abs(v[3]), rot = circle ? 0.f : v[4];
            const float px = rx * std::cos(a1), py = ry * std::sin(a1);
            cx = v[0] + px * std::cos(rot) - py * std::sin(rot); cy = v[1] + px * std::sin(rot) + py * std::cos(rot);
            has = true;
            break;
        }
        case Path2D::Command::ArcTo: {
            if (has) addMapped(b, m, cx, cy);
            addMapped(b, m, v[0], v[1]); addMapped(b, m, v[2], v[3]);
            ArcToResult a;
            if (has && arcToGeometry(cx, cy, v[0], v[1], v[2], v[3], std::abs(v[4]), a)) {
                addMapped(b, m, a.t1x, a.t1y); addMapped(b, m, a.t2x, a.t2y);
                cx = a.t2x; cy = a.t2y;
            } else { cx = v[0]; cy = v[1]; }
            has = true;
            break;
        }
        case Path2D::Command::Rect: case Path2D::Command::RoundRect:
            addMappedRect(b, m, v[0], v[1], v[0] + v[2], v[1] + v[3]); cx = sx = v[0]; cy = sy = v[1]; has = true; break;
        case Path2D::Command::Close: cx = sx; cy = sy; break;
        }
    }
    return b;
}
inline Box pathBounds(const Path2D& path, const Matrix& m) { return pathBounds(path.segments().data(), path.segments().size(), m); }

// How far a stroke reaches beyond its centre line, in its own units.
inline float strokeReach(float width, int cap, int join, float miterLimit) {
    float k = 0.5f;
    if (cap == 2) k = std::max(k, 0.70711f);
    if (join == 0) k = std::max(k, 0.5f * std::max(1.f, miterLimit));
    return width * k;
}

}  // namespace
}  // namespace canvas_detail
