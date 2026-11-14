#pragma once
// The pieces every backend shares: gradient and pattern objects, the
// gradient constructors, and the public CSS colour parser. Included by
// exactly one backend TU per build.

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../canvas.h"
#include "css.h"

struct CanvasGradient::Data {
    int kind = 0;  // 0 linear (x0,y0,x1,y1), 1 radial (x0,y0,r0,x1,y1,r1), 2 conic (angle,x,y)
    float g[6] = {};
    std::vector<std::pair<float, Color>> stops;
    std::uint32_t version = 1;
    // Native: the resolved colour ramp for `nativeCacheVersion`.
    std::shared_ptr<const void> nativeCache;
    std::uint32_t nativeCacheVersion = 0;
    // Web: the page's CanvasGradient, and the stops it has been given.
    int webId = 0;
    std::size_t webStops = 0;
    ~Data();
};
struct CanvasPattern::Data {
    std::shared_ptr<const std::vector<std::uint8_t>> rgba;  // straight RGBA
    int width = 0, height = 0;
    bool repeatX = true, repeatY = true, opaque = false;
    std::array<float, 6> transform{1, 0, 0, 1, 0, 0};
    std::uint32_t version = 1;
    // A backend's own form of the pixels (the Skia backend's SkImage).
    std::shared_ptr<const void> nativeCache;
    // Web: the page's CanvasPattern (for `webVersion`) and its source snapshot.
    int webId = 0, webSnapshot = 0;
    std::uint32_t webVersion = 0;
    std::string repetition = "repeat";
    ~Data();
};

void CanvasGradient::addColorStop(float offset, Color color) {
    if (!data_ || !(offset >= 0 && offset <= 1)) return;
    data_->stops.emplace_back(offset, color);
    ++data_->version;
}
bool CanvasGradient::addColorStop(float offset, const std::string& cssColor) {
    Color c;
    if (!data_ || !(offset >= 0 && offset <= 1) || !canvas_detail::parseCssColor(cssColor, c)) return false;
    addColorStop(offset, c);
    return true;
}
void CanvasPattern::setTransform(float a, float b, float c, float d, float e, float f) {
    if (!data_) return;
    for (float v : {a, b, c, d, e, f}) if (!std::isfinite(v)) return;
    data_->transform = {a, b, c, d, e, f};
    ++data_->version;
}

bool parseCssColor(const std::string& text, Color& out) { return canvas_detail::parseCssColor(text, out); }

namespace {
bool parseRepetition(const std::string& r, bool& x, bool& y) {
    if (r.empty() || r == "repeat") { x = y = true; return true; }
    if (r == "repeat-x") { x = true; y = false; return true; }
    if (r == "repeat-y") { x = false; y = true; return true; }
    if (r == "no-repeat") { x = y = false; return true; }
    return false;
}
bool finiteArgs(std::initializer_list<float> v) {
    for (float f : v) if (!std::isfinite(f)) return false;
    return true;
}
}  // namespace

CanvasGradient Canvas::createLinearGradient(float x0, float y0, float x1, float y1) {
    CanvasGradient g;
    if (!finiteArgs({x0, y0, x1, y1})) return g;
    g.data_ = std::make_shared<CanvasGradient::Data>();
    g.data_->kind = 0;
    const float v[6] = {x0, y0, x1, y1, 0, 0};
    std::copy(v, v + 6, g.data_->g);
    return g;
}
CanvasGradient Canvas::createRadialGradient(float x0, float y0, float r0, float x1, float y1, float r1) {
    CanvasGradient g;
    if (!finiteArgs({x0, y0, r0, x1, y1, r1}) || r0 < 0 || r1 < 0) return g;
    g.data_ = std::make_shared<CanvasGradient::Data>();
    g.data_->kind = 1;
    const float v[6] = {x0, y0, r0, x1, y1, r1};
    std::copy(v, v + 6, g.data_->g);
    return g;
}
CanvasGradient Canvas::createConicGradient(float angle, float x, float y) {
    CanvasGradient g;
    if (!finiteArgs({angle, x, y})) return g;
    g.data_ = std::make_shared<CanvasGradient::Data>();
    g.data_->kind = 2;
    const float v[6] = {angle, x, y, 0, 0, 0};
    std::copy(v, v + 6, g.data_->g);
    return g;
}
