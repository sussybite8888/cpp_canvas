#pragma once
// Path2D: the same retained segment list on every backend; each backend turns
// the segments into its own geometry when the path is drawn.
//
// Out-of-line definitions, #included by exactly one TU per build (the
// backend's: canvas.cpp or canvas_skia.cpp).

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "../canvas.h"

namespace {
inline bool finiteAll(std::initializer_list<float> values) {
    for (float v : values) if (!std::isfinite(v)) return false;
    return true;
}
inline Path2D::Segment pathSegment(Path2D::Command command, std::initializer_list<float> values, bool ccw = false) {
    Path2D::Segment result{};
    result.command = command;
    result.counterClockwise = ccw;
    std::copy(values.begin(), values.end(), result.v);
    return result;
}
}  // namespace

// Lazily allocated so that the paths a frame builds and throws away never
// consume one. Never reused: a key names one geometry for the life of the
// process.
std::uint32_t Path2D::cacheKey() const {
    static std::uint32_t next = 1;
    if (key_ == 0) key_ = next++;
    return key_;
}

// The canvas spec ignores a path call with any non-finite argument.
void Path2D::moveTo(float x, float y) { if (!finiteAll({x, y})) return; touch(); segments_.push_back(pathSegment(Command::Move, {x, y})); }
void Path2D::lineTo(float x, float y) { if (!finiteAll({x, y})) return; touch(); segments_.push_back(pathSegment(Command::Line, {x, y})); }
void Path2D::closePath() { touch(); segments_.push_back(pathSegment(Command::Close, {})); }
void Path2D::quadraticCurveTo(float a, float b, float c, float d) {
    if (!finiteAll({a, b, c, d})) return;
    touch(); segments_.push_back(pathSegment(Command::Quadratic, {a, b, c, d}));
}
void Path2D::bezierCurveTo(float a, float b, float c, float d, float e, float f) {
    if (!finiteAll({a, b, c, d, e, f})) return;
    touch(); segments_.push_back(pathSegment(Command::Bezier, {a, b, c, d, e, f}));
}
void Path2D::arc(float a, float b, float c, float d, float e, bool f) {
    if (!finiteAll({a, b, c, d, e}) || c < 0) return;
    touch(); segments_.push_back(pathSegment(Command::Arc, {a, b, c, d, e}, f));
}
void Path2D::arcTo(float a, float b, float c, float d, float e) {
    if (!finiteAll({a, b, c, d, e}) || e < 0) return;
    touch(); segments_.push_back(pathSegment(Command::ArcTo, {a, b, c, d, e}));
}
void Path2D::ellipse(float a, float b, float c, float d, float e, float f, float g, bool h) {
    if (!finiteAll({a, b, c, d, e, f, g}) || c < 0 || d < 0) return;
    touch(); segments_.push_back(pathSegment(Command::Ellipse, {a, b, c, d, e, f, g}, h));
}
void Path2D::rect(float a, float b, float c, float d) {
    if (!finiteAll({a, b, c, d})) return;
    touch(); segments_.push_back(pathSegment(Command::Rect, {a, b, c, d}));
}
void Path2D::roundRect(float a, float b, float c, float d, float e) {
    if (!finiteAll({a, b, c, d, e}) || e < 0) return;
    touch(); segments_.push_back(pathSegment(Command::RoundRect, {a, b, c, d, e}));
}

// The canvas spec's roundRect(x, y, w, h, radii): corners in CSS order,
// flipped with a negative width or height (which also reverses the winding),
// scaled down together when they do not fit, then outlined as lines and
// elliptical quarter arcs, closed, and followed by a new subpath at (x, y).
void Path2D::roundRect(float x, float y, float w, float h, const std::vector<CornerRadius>& radii) {
    if (!finiteAll({x, y, w, h}) || radii.empty() || radii.size() > 4) return;
    for (const CornerRadius& r : radii) if (!finiteAll({r.x, r.y}) || r.x < 0 || r.y < 0) return;
    // Equal circular corners are the single-radius form, which every backend
    // draws natively.
    bool uniform = true;
    for (const CornerRadius& r : radii) uniform = uniform && r.x == radii[0].x && r.y == radii[0].x;
    if (uniform && w >= 0 && h >= 0) { roundRect(x, y, w, h, radii[0].x); return; }
    CornerRadius ul, ur, lr, ll;
    switch (radii.size()) {
    case 1: ul = ur = lr = ll = radii[0]; break;
    case 2: ul = lr = radii[0]; ur = ll = radii[1]; break;
    case 3: ul = radii[0]; ur = ll = radii[1]; lr = radii[2]; break;
    default: ul = radii[0]; ur = radii[1]; lr = radii[2]; ll = radii[3]; break;
    }
    if (w == 0 || h == 0) { rect(x, y, w, h); return; }
    bool clockwise = true;
    if (w < 0) { clockwise = !clockwise; x += w; w = -w; std::swap(ul, ur); std::swap(ll, lr); }
    if (h < 0) { clockwise = !clockwise; y += h; h = -h; std::swap(ul, ll); std::swap(ur, lr); }
    const float top = ul.x + ur.x, right = ur.y + lr.y, bottom = lr.x + ll.x, left = ul.y + ll.y;
    float scale = 1;
    if (top > 0) scale = std::min(scale, w / top);
    if (right > 0) scale = std::min(scale, h / right);
    if (bottom > 0) scale = std::min(scale, w / bottom);
    if (left > 0) scale = std::min(scale, h / left);
    if (scale < 1) for (CornerRadius* r : {&ul, &ur, &lr, &ll}) { r->x *= scale; r->y *= scale; }
    constexpr float kHalfPi = 1.57079632679f, kPiF = 3.14159265359f;
    if (clockwise) {
        moveTo(x + ul.x, y);
        lineTo(x + w - ur.x, y);
        ellipse(x + w - ur.x, y + ur.y, ur.x, ur.y, 0, -kHalfPi, 0);
        lineTo(x + w, y + h - lr.y);
        ellipse(x + w - lr.x, y + h - lr.y, lr.x, lr.y, 0, 0, kHalfPi);
        lineTo(x + ll.x, y + h);
        ellipse(x + ll.x, y + h - ll.y, ll.x, ll.y, 0, kHalfPi, kPiF);
        lineTo(x, y + ul.y);
        ellipse(x + ul.x, y + ul.y, ul.x, ul.y, 0, kPiF, kPiF + kHalfPi);
    } else {
        moveTo(x + ul.x, y);
        ellipse(x + ul.x, y + ul.y, ul.x, ul.y, 0, kPiF + kHalfPi, kPiF, true);
        lineTo(x, y + h - ll.y);
        ellipse(x + ll.x, y + h - ll.y, ll.x, ll.y, 0, kPiF, kHalfPi, true);
        lineTo(x + w - lr.x, y + h);
        ellipse(x + w - lr.x, y + h - lr.y, lr.x, lr.y, 0, kHalfPi, 0, true);
        lineTo(x + w, y + ur.y);
        ellipse(x + w - ur.x, y + ur.y, ur.x, ur.y, 0, 0, -kHalfPi, true);
    }
    closePath();
    moveTo(x, y);
}

void Path2D::addPath(const Path2D& other) {
    touch();
    segments_.insert(segments_.end(), other.segments_.begin(), other.segments_.end());
    glyphOutlines_ = glyphOutlines_ || other.glyphOutlines_;
}

// addPath(path, transform): segments that stay what they are under an affine
// map (lines and Béziers) are mapped point by point; arcs, ellipses, rects and
// rounded rects become lines and cubic Béziers first (arcs in pieces of at
// most a quarter turn, which keeps a 1000px radius within a hundredth of a
// pixel), because a skewed circle is no longer something `arc` can say.
void Path2D::addPath(const Path2D& other, const std::array<float, 6>& m) {
    for (float v : m) if (!std::isfinite(v)) return;
    if (m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[4] == 0 && m[5] == 0) { addPath(other); return; }
    touch();
    const auto mx = [&](float x, float y) { return m[0] * x + m[2] * y + m[4]; };
    const auto my = [&](float x, float y) { return m[1] * x + m[3] * y + m[5]; };
    std::vector<Segment> out;
    out.reserve(other.segments_.size() * 2);
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool has = false, open = false;
    const auto move = [&](float x, float y) { out.push_back(pathSegment(Command::Move, {mx(x, y), my(x, y)})); cx = sx = x; cy = sy = y; has = open = true; };
    const auto line = [&](float x, float y) {
        if (!open) { if (has) move(cx, cy); else { move(x, y); return; } }
        out.push_back(pathSegment(Command::Line, {mx(x, y), my(x, y)})); cx = x; cy = y;
    };
    // One elliptical arc as cubic pieces.
    const auto ellipseArc = [&](float ex, float ey, float rx, float ry, float rot, float a0, float sweep) {
        const float cs = std::cos(rot), sn = std::sin(rot);
        const auto pt = [&](float t, float& ox, float& oy) {
            const float px = rx * std::cos(t), py = ry * std::sin(t);
            ox = ex + px * cs - py * sn; oy = ey + px * sn + py * cs;
        };
        const auto tangent = [&](float t, float& ox, float& oy) {
            const float px = -rx * std::sin(t), py = ry * std::cos(t);
            ox = px * cs - py * sn; oy = px * sn + py * cs;
        };
        float x0, y0;
        pt(a0, x0, y0);
        if (open || has) line(x0, y0); else move(x0, y0);
        const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / 1.5707964f - 1e-4f)));
        const float step = sweep / pieces, k = 4.f / 3.f * std::tan(step / 4);
        for (int i = 0; i < pieces; ++i) {
            const float t0 = a0 + step * i, t1 = t0 + step;
            float p0x, p0y, p1x, p1y, d0x, d0y, d1x, d1y;
            pt(t0, p0x, p0y); pt(t1, p1x, p1y); tangent(t0, d0x, d0y); tangent(t1, d1x, d1y);
            const float c1x = p0x + k * d0x, c1y = p0y + k * d0y, c2x = p1x - k * d1x, c2y = p1y - k * d1y;
            out.push_back(pathSegment(Command::Bezier, {mx(c1x, c1y), my(c1x, c1y), mx(c2x, c2y), my(c2x, c2y), mx(p1x, p1y), my(p1x, p1y)}));
            cx = p1x; cy = p1y;
        }
    };
    const auto normSweep = [](float a0, float a1, bool ccw) {
        constexpr float tau = 6.28318530718f;
        float sweep = a1 - a0;
        if (!ccw) { if (sweep >= tau) return tau; sweep = std::fmod(sweep, tau); if (sweep < 0) sweep += tau; }
        else { if (sweep <= -tau) return -tau; sweep = std::fmod(sweep, tau); if (sweep > 0) sweep -= tau; }
        return sweep;
    };
    for (const Segment& s : other.segments_) {
        const float* v = s.v;
        switch (s.command) {
        case Command::Move: move(v[0], v[1]); break;
        case Command::Line: line(v[0], v[1]); break;
        case Command::Quadratic:
            if (!open) { if (has) move(cx, cy); else move(v[0], v[1]); }
            out.push_back(pathSegment(Command::Quadratic, {mx(v[0], v[1]), my(v[0], v[1]), mx(v[2], v[3]), my(v[2], v[3])}));
            cx = v[2]; cy = v[3]; break;
        case Command::Bezier:
            if (!open) { if (has) move(cx, cy); else move(v[0], v[1]); }
            out.push_back(pathSegment(Command::Bezier, {mx(v[0], v[1]), my(v[0], v[1]), mx(v[2], v[3]), my(v[2], v[3]), mx(v[4], v[5]), my(v[4], v[5])}));
            cx = v[4]; cy = v[5]; break;
        case Command::Arc: ellipseArc(v[0], v[1], v[2], v[2], 0, v[3], normSweep(v[3], v[4], s.counterClockwise)); break;
        case Command::Ellipse: ellipseArc(v[0], v[1], v[2], v[3], v[4], v[5], normSweep(v[5], v[6], s.counterClockwise)); break;
        case Command::ArcTo: {
            if (!open && !has) { move(v[0], v[1]); break; }
            // The tangent arc (see canvas_detail::arcToGeometry), from the
            // current point.
            const float x0 = cx, y0 = cy, x1 = v[0], y1 = v[1], x2 = v[2], y2 = v[3], r = v[4];
            const float ux = x0 - x1, uy = y0 - y1, wx = x2 - x1, wy = y2 - y1;
            const float lu = std::hypot(ux, uy), lw = std::hypot(wx, wy);
            const float cross = ux * wy - uy * wx;
            if (r <= 0 || lu < 1e-6f || lw < 1e-6f || std::abs(cross) < 1e-6f * lu * lw) { line(x1, y1); break; }
            const float half = std::acos(std::max(-1.f, std::min(1.f, (ux * wx + uy * wy) / (lu * lw)))) / 2;
            const float dist = r / std::tan(half);
            const float ax = ux / lu, ay = uy / lu, bx = wx / lw, by = wy / lw;
            float bxm = ax + bx, bym = ay + by;
            const float bl = std::hypot(bxm, bym);
            bxm /= bl; bym /= bl;
            const float ccx = x1 + bxm * r / std::sin(half), ccy = y1 + bym * r / std::sin(half);
            const float t1x = x1 + ax * dist, t1y = y1 + ay * dist, t2x = x1 + bx * dist, t2y = y1 + by * dist;
            line(t1x, t1y);
            const float a0 = std::atan2(t1y - ccy, t1x - ccx), a1 = std::atan2(t2y - ccy, t2x - ccx);
            const bool ccw = ((x1 - x0) * (y2 - y1) - (y1 - y0) * (x2 - x1)) < 0;
            float sweep = a1 - a0;
            if (ccw) { while (sweep > 0) sweep -= 6.28318530718f; } else { while (sweep < 0) sweep += 6.28318530718f; }
            ellipseArc(ccx, ccy, r, r, 0, a0, sweep);
            break;
        }
        case Command::Rect:
            move(v[0], v[1]); line(v[0] + v[2], v[1]); line(v[0] + v[2], v[1] + v[3]); line(v[0], v[1] + v[3]);
            out.push_back(pathSegment(Command::Close, {})); open = false; cx = sx; cy = sy;
            move(v[0], v[1]); open = false;
            break;
        case Command::RoundRect: {
            const float x = std::min(v[0], v[0] + v[2]), y = std::min(v[1], v[1] + v[3]);
            const float w = std::abs(v[2]), h = std::abs(v[3]), r = std::max(0.f, std::min(v[4], std::min(w, h) / 2));
            constexpr float hp = 1.57079632679f;
            move(x + r, y);
            line(x + w - r, y); ellipseArc(x + w - r, y + r, r, r, 0, -hp, hp);
            line(x + w, y + h - r); ellipseArc(x + w - r, y + h - r, r, r, 0, 0, hp);
            line(x + r, y + h); ellipseArc(x + r, y + h - r, r, r, 0, hp, hp);
            line(x, y + r); ellipseArc(x + r, y + r, r, r, 0, 2 * hp, hp);
            out.push_back(pathSegment(Command::Close, {})); open = false; cx = sx; cy = sy;
            move(v[0], v[1]); open = false;
            break;
        }
        case Command::Close:
            out.push_back(pathSegment(Command::Close, {})); open = false; cx = sx; cy = sy; break;
        }
    }
    segments_.insert(segments_.end(), out.begin(), out.end());
    glyphOutlines_ = glyphOutlines_ || other.glyphOutlines_;
}
