// cpp_canvas: the Canvas API and its native and web backends.
//
// Layout: the shared pieces (Path2D, gradients and patterns, CSS parsing)
// first; then the backend, whose internals live in detail/ -- detail/soft.h
// natively, detail/web.h under emscripten -- and are included only here, so
// this file stays the library's one translation unit for the Canvas.

#include "canvas.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include "font.h"
#include "detail/css.h"
#include "detail/geometry.h"
#include "detail/path2d_impl.h"

#include "detail/paint_objects.h"


#ifdef __EMSCRIPTEN__
#include "detail/web.h"
#else
#include "detail/soft.h"

CanvasGradient::Data::~Data() = default;
CanvasPattern::Data::~Data() = default;

// ===========================================================================
// Native backend
// ===========================================================================
using canvas_detail::Op;
using canvas_detail::OpKind;
using canvas_detail::PaintStyle;

namespace {
canvas_detail::Box rectBox(const canvas_detail::Matrix& m, float x, float y, float w, float h) {
    canvas_detail::Box b;
    canvas_detail::addMappedRect(b, m, x, y, x + w, y + h);
    return b;
}
const std::shared_ptr<const canvas_detail::ResolvedFont>& defaultFont() {
    static std::shared_ptr<const canvas_detail::ResolvedFont> f = canvas_detail::resolveFont("10px sans-serif");
    return f;
}
// Keyed copies of drawImage pixels: the key names those exact pixels for the
// life of the process, so a copy taken once serves every later frame.
std::shared_ptr<const canvas_detail::ImageCopy> imageCopy(const std::uint8_t* rgba, int w, int h, std::uint32_t key) {
    static std::unordered_map<std::uint32_t, std::shared_ptr<const canvas_detail::ImageCopy>> cache;
    static std::size_t bytes = 0;
    if (key) {
        const auto found = cache.find(key);
        if (found != cache.end() && found->second->w == w && found->second->h == h) return found->second;
    }
    auto copy = std::make_shared<canvas_detail::ImageCopy>();
    copy->w = w; copy->h = h;
    copy->rgba.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
    copy->opaque = canvas_detail::allOpaque(rgba, static_cast<std::size_t>(w) * h);
    if (key) {
        if (bytes > (256u << 20) || cache.size() > 4096) { cache.clear(); bytes = 0; }
        bytes += copy->rgba.size();
        cache[key] = copy;
    }
    return copy;
}
}  // namespace

Canvas::Canvas(int w, int h, std::string id, bool desynchronized)
    : width_(std::max(1, w)), height_(std::max(1, h)), elementId_(std::move(id)), logicalWidth_(width_), logicalHeight_(height_) {
    (void)desynchronized;
    soft_ = std::make_unique<Soft>(width_, height_, Color{255, 255, 255});
}
Canvas::Canvas(int w, int h, bool virtualCanvas)
    : width_(std::max(1, w)), height_(std::max(1, h)), virtual_(virtualCanvas), logicalWidth_(width_), logicalHeight_(height_) {
    soft_ = std::make_unique<Soft>(width_, height_, Color{0, 0, 0, 0});
}
Canvas Canvas::createVirtual(int w, int h) { return Canvas(w, h, true); }
void Canvas::setLogicalSize(int w, int h) {
    logicalWidth_ = w > 0 ? w : width_;
    logicalHeight_ = h > 0 ? h : height_;
}
Canvas::Canvas(Canvas&& o) noexcept
    : width_(o.width_), height_(o.height_), contextId_(o.contextId_), virtual_(o.virtual_), elementId_(std::move(o.elementId_)),
      logicalWidth_(o.logicalWidth_), logicalHeight_(o.logicalHeight_), fill_(o.fill_), stroke_(o.stroke_), lineWidth_(o.lineWidth_),
      currentPath_(std::move(o.currentPath_)), tint_(o.tint_), cullingEnabled_(o.cullingEnabled_), soft_(std::move(o.soft_)) {}
Canvas& Canvas::operator=(Canvas&& o) noexcept {
    if (this == &o) return *this;
    width_ = o.width_; height_ = o.height_; contextId_ = o.contextId_; virtual_ = o.virtual_; elementId_ = std::move(o.elementId_);
    logicalWidth_ = o.logicalWidth_; logicalHeight_ = o.logicalHeight_; fill_ = o.fill_; stroke_ = o.stroke_; lineWidth_ = o.lineWidth_;
    currentPath_ = std::move(o.currentPath_); tint_ = o.tint_; cullingEnabled_ = o.cullingEnabled_;
    soft_ = std::move(o.soft_);
    return *this;
}
Canvas::~Canvas() = default;
void Canvas::present(const std::string& id) { (void)id; flush(); }

// ----- state -----
void Canvas::save() { soft_->stack.push_back(soft_->st); }
void Canvas::restore() {
    Soft& s = *soft_;
    if (s.stack.empty()) return;
    s.st = std::move(s.stack.back());
    s.stack.pop_back();
    fill_ = s.st.style.fill.color; stroke_ = s.st.style.stroke.color; lineWidth_ = s.st.style.lineWidth;
}
void Canvas::reset() {
    Soft& s = *soft_;
    s.flushReaders();
    s.discardPending();
    std::fill(s.px.begin(), s.px.end(), Color{0, 0, 0, 0});
    s.st = Soft::State{};
    s.touch();
    s.stack.clear();
    s.layers.clear();
    currentPath_ = Path2D{};
    fill_ = stroke_ = Color{0, 0, 0};
    lineWidth_ = 1;
}
void Canvas::scale(float a, float b) {
    if (!finiteArgs({a, b})) return;
    auto& m = soft_->st.matrix;
    m[0] *= a; m[1] *= a; m[2] *= b; m[3] *= b;
}
void Canvas::rotate(float a) {
    if (!std::isfinite(a)) return;
    auto& m = soft_->st.matrix;
    const float c = std::cos(a), s = std::sin(a), m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3];
    m[0] = m0 * c + m2 * s; m[1] = m1 * c + m3 * s; m[2] = m2 * c - m0 * s; m[3] = m3 * c - m1 * s;
}
void Canvas::translate(float a, float b) {
    if (!finiteArgs({a, b})) return;
    auto& m = soft_->st.matrix;
    m[4] += m[0] * a + m[2] * b; m[5] += m[1] * a + m[3] * b;
}
void Canvas::transform(float a, float b, float c, float d, float e, float f) {
    if (!finiteArgs({a, b, c, d, e, f})) return;
    soft_->st.matrix = canvas_detail::multiply(soft_->st.matrix, {a, b, c, d, e, f});
}
void Canvas::setTransform(float a, float b, float c, float d, float e, float f) {
    if (!finiteArgs({a, b, c, d, e, f})) return;
    soft_->st.matrix = {a, b, c, d, e, f};
}
void Canvas::resetTransform() { soft_->st.matrix = {1, 0, 0, 1, 0, 0}; }
std::array<float, 6> Canvas::currentTransform() const { return soft_->st.matrix; }
bool Canvas::isPlainComposite() const { return soft_->st.style.plain(); }
float Canvas::globalAlpha() const { return soft_->st.style.alpha; }

Color Canvas::tinted(Color c) const {
    if (tint_.a == 0) return c;
    const float keep = 1.f - tint_.a / 255.f;
    const auto mix = [keep](std::uint8_t base, std::uint8_t add) {
        return static_cast<std::uint8_t>(std::clamp((base / 255.f * keep + add / 255.f) * 255.f, 0.f, 255.f));
    };
    return Color{mix(c.r, tint_.r), mix(c.g, tint_.g), mix(c.b, tint_.b), c.a};
}

namespace {
PaintStyle solidPaint(Color c) { PaintStyle p; p.kind = PaintStyle::Solid; p.color = c; return p; }
}
void Canvas::setFillStyle(Color c) { c = tinted(c); fill_ = c; soft_->st.style.fill = solidPaint(c); soft_->touch(); }
void Canvas::setStrokeStyle(Color c) { c = tinted(c); stroke_ = c; soft_->st.style.stroke = solidPaint(c); soft_->touch(); }
void Canvas::setFillStyle(const std::string& s) {
    Color c;
    if (!canvas_detail::parseCssColor(s, c)) return;
    fill_ = c; soft_->st.style.fill = solidPaint(c); soft_->touch();
}
void Canvas::setStrokeStyle(const std::string& s) {
    Color c;
    if (!canvas_detail::parseCssColor(s, c)) return;
    stroke_ = c; soft_->st.style.stroke = solidPaint(c); soft_->touch();
}
void Canvas::setFillStyle(const CanvasGradient& g) {
    if (!g.valid()) return;
    PaintStyle p; p.kind = PaintStyle::Gradient; p.gradient = g.data_;
    soft_->st.style.fill = p; soft_->touch();
}
void Canvas::setStrokeStyle(const CanvasGradient& g) {
    if (!g.valid()) return;
    PaintStyle p; p.kind = PaintStyle::Gradient; p.gradient = g.data_;
    soft_->st.style.stroke = p; soft_->touch();
}
void Canvas::setFillStyle(const CanvasPattern& pt) {
    if (!pt.valid()) return;
    PaintStyle p; p.kind = PaintStyle::Pattern; p.pattern = pt.data_;
    soft_->st.style.fill = p; soft_->touch();
}
void Canvas::setStrokeStyle(const CanvasPattern& pt) {
    if (!pt.valid()) return;
    PaintStyle p; p.kind = PaintStyle::Pattern; p.pattern = pt.data_;
    soft_->st.style.stroke = p; soft_->touch();
}
CanvasPattern Canvas::createPattern(const Canvas& source, const std::string& repetition) {
    CanvasPattern p;
    bool rx, ry;
    if (!parseRepetition(repetition, rx, ry) || !source.soft_) return p;
    source.soft_->flush();
    p.data_ = std::make_shared<CanvasPattern::Data>();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(source.soft_->px.data());
    p.data_->rgba = std::make_shared<const std::vector<std::uint8_t>>(bytes, bytes + source.soft_->px.size() * 4);
    p.data_->width = source.width_; p.data_->height = source.height_;
    p.data_->repeatX = rx; p.data_->repeatY = ry; p.data_->repetition = repetition;
    p.data_->opaque = canvas_detail::allOpaque(bytes, source.soft_->px.size());
    return p;
}
CanvasPattern Canvas::createPattern(const std::uint8_t* rgba, int w, int h, const std::string& repetition) {
    CanvasPattern p;
    bool rx, ry;
    if (!rgba || w <= 0 || h <= 0 || !parseRepetition(repetition, rx, ry)) return p;
    p.data_ = std::make_shared<CanvasPattern::Data>();
    p.data_->rgba = std::make_shared<const std::vector<std::uint8_t>>(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
    p.data_->width = w; p.data_->height = h;
    p.data_->repeatX = rx; p.data_->repeatY = ry; p.data_->repetition = repetition;
    p.data_->opaque = canvas_detail::allOpaque(rgba, static_cast<std::size_t>(w) * h);
    return p;
}

void Canvas::setGlobalAlpha(float a) {
    if (!(a >= 0 && a <= 1)) return;
    if (soft_->st.style.alpha != a) { soft_->st.style.alpha = a; soft_->touch(); }
}
void Canvas::setGlobalCompositeOperation(const std::string& s) {
    canvas_detail::CompositeOp op;
    if (!canvas_detail::parseCompositeOp(s, op)) return;
    if (soft_->st.style.composite != op) { soft_->st.style.composite = op; soft_->touch(); }
}
void Canvas::setFilter(const std::string& s) {
    std::vector<canvas_detail::CssFilterFunction> list;
    if (!canvas_detail::parseCssFilter(s, list)) return;
    soft_->st.style.filter = list.empty() ? nullptr : std::make_shared<const std::vector<canvas_detail::CssFilterFunction>>(std::move(list));
    soft_->touch();
}
void Canvas::setLineWidth(float a) {
    if (!(a > 0) || !std::isfinite(a)) return;
    lineWidth_ = a;
    if (soft_->st.style.lineWidth != a) { soft_->st.style.lineWidth = a; soft_->touch(); }
}
void Canvas::setLineCap(const std::string& s) {
    const int v = s == "butt" ? 0 : s == "round" ? 1 : s == "square" ? 2 : -1;
    if (v < 0) return;
    soft_->st.style.lineCap = static_cast<std::uint8_t>(v); soft_->touch();
}
void Canvas::setLineJoin(const std::string& s) {
    const int v = s == "miter" ? 0 : s == "round" ? 1 : s == "bevel" ? 2 : -1;
    if (v < 0) return;
    soft_->st.style.lineJoin = static_cast<std::uint8_t>(v); soft_->touch();
}
void Canvas::setMiterLimit(float a) {
    if (!(a > 0) || !std::isfinite(a)) return;
    soft_->st.style.miterLimit = a; soft_->touch();
}
void Canvas::setLineDash(const std::vector<float>& v) {
    for (float f : v) if (!(f >= 0) || !std::isfinite(f)) return;
    std::vector<float> d = v;
    if (d.size() % 2) d.insert(d.end(), v.begin(), v.end());
    soft_->st.style.dash = std::move(d); soft_->touch();
}
std::vector<float> Canvas::getLineDash() const { return soft_->st.style.dash; }
void Canvas::setLineDashOffset(float a) {
    if (!std::isfinite(a)) return;
    soft_->st.style.dashOffset = a; soft_->touch();
}
void Canvas::setShadow(Color c, float blur, float ox, float oy) {
    auto& st = soft_->st.style;
    st.shadowColor = c;
    if (blur >= 0 && std::isfinite(blur)) st.shadowBlur = blur;
    if (std::isfinite(ox)) st.shadowX = ox;
    if (std::isfinite(oy)) st.shadowY = oy;
    soft_->touch();
}
void Canvas::setShadow(const std::string& css, float blur, float ox, float oy) {
    Color c;
    if (!canvas_detail::parseCssColor(css, c)) c = soft_->st.style.shadowColor;
    setShadow(c, blur, ox, oy);
}
void Canvas::setFont(const std::string& s) {
    auto f = canvas_detail::resolveFont(s);
    if (!f) return;
    soft_->st.style.font = std::move(f); soft_->touch();
}
void Canvas::setTextAlign(const std::string& s) {
    using namespace canvas_detail;
    const int v = s == "start" ? kAlignStart : s == "end" ? kAlignEnd : s == "left" ? kAlignLeft : s == "right" ? kAlignRight : s == "center" ? kAlignCenter : -1;
    if (v < 0) return;
    soft_->st.style.textAlign = static_cast<std::uint8_t>(v); soft_->touch();
}
void Canvas::setTextBaseline(const std::string& s) {
    using namespace canvas_detail;
    const int v = s == "alphabetic" ? kBaseAlphabetic : s == "top" ? kBaseTop : s == "hanging" ? kBaseHanging : s == "middle" ? kBaseMiddle
                : s == "ideographic" ? kBaseIdeographic : s == "bottom" ? kBaseBottom : -1;
    if (v < 0) return;
    soft_->st.style.textBaseline = static_cast<std::uint8_t>(v); soft_->touch();
}
void Canvas::setDirection(const std::string& s) {
    if (s != "ltr" && s != "rtl" && s != "inherit") return;
    soft_->st.style.rtl = s == "rtl"; soft_->touch();
}
void Canvas::setLetterSpacing(const std::string& s) {
    const auto& f = soft_->st.style.font ? soft_->st.style.font : defaultFont();
    float px;
    if (!canvas_detail::parseCssLength(s, px, f ? f->size : 10.f)) return;
    soft_->st.style.letterSpacing = px; soft_->touch();
}
void Canvas::setWordSpacing(const std::string& s) {
    const auto& f = soft_->st.style.font ? soft_->st.style.font : defaultFont();
    float px;
    if (!canvas_detail::parseCssLength(s, px, f ? f->size : 10.f)) return;
    soft_->st.style.wordSpacing = px; soft_->touch();
}
void Canvas::setFontKerning(const std::string& s) {
    if (s != "auto" && s != "normal" && s != "none") return;
    soft_->st.style.kerning = s != "none"; soft_->touch();
}
void Canvas::setImageSmoothingEnabled(bool a) { if (soft_->st.style.smoothing != a) { soft_->st.style.smoothing = a; soft_->touch(); } }
void Canvas::setImageSmoothingQuality(const std::string& s) {
    const int v = s == "low" ? 0 : s == "medium" ? 1 : s == "high" ? 2 : -1;
    if (v < 0) return;
    soft_->st.style.smoothingQuality = static_cast<std::uint8_t>(v); soft_->touch();
}

// ----- device fast paths (immediate) -----
void Canvas::blitDevice(const std::uint8_t* rgba, int iw, int ih, int dx, int dy) {
    Soft& s = *soft_;
    s.beforeImmediateWrite();
    const auto& st = s.st.style;
    if (!rgba || iw <= 0 || ih <= 0 || st.alpha <= 0) return;
    const int x0 = std::max(0, dx), x1 = std::min(width_, dx + iw), y0 = std::max(0, dy), y1 = std::min(height_, dy + ih);
    if (x0 >= x1 || y0 >= y1) return;
    const canvas_detail::ClipMask* c = st.clip.get();
    const std::uint32_t a8 = static_cast<std::uint32_t>(st.alpha * 255 + 0.5f);
    canvas_detail::parallelRows(y0, y1, static_cast<long long>(x1 - x0) * (y1 - y0), [&](int ya, int yb, int) {
        for (int y = ya; y < yb; ++y) {
            const std::uint8_t* src = rgba + (static_cast<std::size_t>(y - dy) * iw + (x0 - dx)) * 4;
            Color* dst = s.px.data() + static_cast<std::size_t>(y) * width_ + x0;
            for (int x = x0; x < x1; ++x, src += 4, ++dst) {
                if (src[3] == 0) continue;
                std::uint32_t sa = canvas_detail::div255(src[3] * a8);
                if (c) sa = canvas_detail::div255(sa * c->at(x, y));
                canvas_detail::blendOver(*dst, Color{src[0], src[1], src[2], 255}, sa);
            }
        }
    });
}
void Canvas::darkenDevice(const std::uint8_t* mask, int mw, int mh, int dx, int dy) {
    multiplyDevice(mask, mw, mh, dx, dy, Color{0, 0, 0, 255});
}
void Canvas::multiplyDevice(const std::uint8_t* mask, int mw, int mh, int dx, int dy, Color floor) {
    Soft& s = *soft_;
    s.beforeImmediateWrite();
    const auto& st = s.st.style;
    if (!mask || mw <= 0 || mh <= 0 || st.alpha <= 0) return;
    const int x0 = std::max(0, dx), x1 = std::min(width_, dx + mw), y0 = std::max(0, dy), y1 = std::min(height_, dy + mh);
    if (x0 >= x1 || y0 >= y1) return;
    const canvas_detail::ClipMask* c = st.clip.get();
    const float alpha = st.alpha;
    // What survives of each channel under each mask value.
    std::array<std::array<std::uint16_t, 256>, 3> keep{};
    const std::uint8_t floors[3] = {floor.r, floor.g, floor.b};
    for (int ch = 0; ch < 3; ++ch)
        for (int m = 0; m < 256; ++m) keep[ch][m] = static_cast<std::uint16_t>(255 - std::lround(m * alpha * (255 - floors[ch]) / 255.f));
    canvas_detail::parallelRows(y0, y1, static_cast<long long>(x1 - x0) * (y1 - y0), [&](int ya, int yb, int) {
        for (int y = ya; y < yb; ++y) {
            const std::uint8_t* src = mask + static_cast<std::size_t>(y - dy) * mw + (x0 - dx);
            Color* dst = s.px.data() + static_cast<std::size_t>(y) * width_ + x0;
            for (int x = x0; x < x1;) {
                if (x + 8 <= x1) {  // a vignette is empty over most of the screen
                    std::uint64_t word;
                    std::memcpy(&word, src, 8);
                    if (word == 0) { x += 8; src += 8; dst += 8; continue; }
                }
                std::uint32_t m = *src;
                if (m && c) m = canvas_detail::div255(m * c->at(x, y));
                if (m) {
                    dst->r = static_cast<std::uint8_t>(canvas_detail::div255(dst->r * keep[0][m]));
                    dst->g = static_cast<std::uint8_t>(canvas_detail::div255(dst->g * keep[1][m]));
                    dst->b = static_cast<std::uint8_t>(canvas_detail::div255(dst->b * keep[2][m]));
                }
                ++x; ++src; ++dst;
            }
        }
    });
}
const std::uint8_t* Canvas::pixelData() {
    soft_->flush();
    return reinterpret_cast<const std::uint8_t*>(soft_->px.data());
}

// ----- drawing -----
void Canvas::clear(Color c) {
    Soft& s = *soft_;
    s.flushReaders();
    s.discardPending();
    std::fill(s.px.begin(), s.px.end(), c);
}
void Canvas::clearRect(float x, float y, float w, float h) {
    if (!finiteArgs({x, y, w, h})) return;
    Soft& s = *soft_;
    Op op;
    op.kind = OpKind::ClearRect;
    op.m = s.st.matrix;
    op.f[0] = x; op.f[1] = y; op.f[2] = w; op.f[3] = h;
    // clearRect ignores alpha, compositing, shadow and filter: only the clip.
    op.box = canvas_detail::toIBox(rectBox(op.m, x, y, w, h)).intersect(s.clipBox(s.st.style));
    s.submit(op);
}
void Canvas::fillRect(float x, float y, float w, float h) {
    if (!finiteArgs({x, y, w, h}) || w == 0 || h == 0) {
        // A zero-size rect draws nothing -- except that an unbounded
        // operation still clears what it reaches.
        if (!finiteArgs({x, y, w, h}) || !canvas_detail::compositeIsUnbounded(soft_->st.style.composite)) return;
    }
    Soft& s = *soft_;
    Op op;
    op.kind = OpKind::FillRect;
    op.m = s.st.matrix;
    op.f[0] = x; op.f[1] = y; op.f[2] = w; op.f[3] = h;
    op.box = s.opBox(rectBox(op.m, x, y, w, h), s.st.style);
    s.submit(op);
}
void Canvas::strokeRect(float x, float y, float w, float h) {
    if (!finiteArgs({x, y, w, h})) return;
    Soft& s = *soft_;
    Op op;
    op.kind = OpKind::StrokeRect;
    op.m = s.st.matrix;
    op.f[0] = x; op.f[1] = y; op.f[2] = w; op.f[3] = h;
    canvas_detail::Box b = rectBox(op.m, x, y, w, h);
    const auto& st = s.st.style;
    b.outset(canvas_detail::strokeReach(st.lineWidth, st.lineCap, st.lineJoin, st.miterLimit) * canvas_detail::matrixMaxScale(op.m) + 1);
    op.box = s.opBox(b, st);
    s.submit(op);
}
void Canvas::beginPath() { currentPath_ = Path2D{}; }
void Canvas::closePath() { currentPath_.closePath(); }
void Canvas::moveTo(float a, float b) { currentPath_.moveTo(a, b); }
void Canvas::lineTo(float a, float b) { currentPath_.lineTo(a, b); }
void Canvas::quadraticCurveTo(float a, float b, float c, float d) { currentPath_.quadraticCurveTo(a, b, c, d); }
void Canvas::bezierCurveTo(float a, float b, float c, float d, float e, float f) { currentPath_.bezierCurveTo(a, b, c, d, e, f); }
void Canvas::arc(float a, float b, float c, float d, float e, bool f) { currentPath_.arc(a, b, c, d, e, f); }
void Canvas::arcTo(float a, float b, float c, float d, float e) { currentPath_.arcTo(a, b, c, d, e); }
void Canvas::ellipse(float a, float b, float c, float d, float e, float f, float g, bool h) { currentPath_.ellipse(a, b, c, d, e, f, g, h); }
void Canvas::rect(float a, float b, float c, float d) { currentPath_.rect(a, b, c, d); }
void Canvas::roundRect(float a, float b, float c, float d, float e) { currentPath_.roundRect(a, b, c, d, e); }
void Canvas::roundRect(float a, float b, float c, float d, const std::vector<Path2D::CornerRadius>& r) { currentPath_.roundRect(a, b, c, d, r); }

namespace {
// Records a path paint: the segments go into the canvas's arena.
void recordPath(Canvas::Soft& s, OpKind kind, const Path2D& path, bool evenOdd) {
    if (path.empty()) return;
    const auto& st = s.st.style;
    Op op;
    op.kind = kind;
    op.m = s.st.matrix;
    op.evenOdd = evenOdd;
    op.glyph = path.glyphOutlines();
    op.a = static_cast<std::uint32_t>(s.segs.size());
    op.b = static_cast<std::uint32_t>(path.segments().size());
    s.segs.insert(s.segs.end(), path.segments().begin(), path.segments().end());
    canvas_detail::Box b = canvas_detail::pathBounds(path, op.m);
    if (kind == OpKind::StrokePath)
        b.outset(canvas_detail::strokeReach(st.lineWidth, st.lineCap, st.lineJoin, st.miterLimit) * canvas_detail::matrixMaxScale(op.m) + 1);
    else b.outset(1);
    op.box = kind == OpKind::Desaturate ? canvas_detail::toIBox(b).intersect(s.clipBox(st)) : s.opBox(b, st);
    s.submit(op);
}
}  // namespace

void Canvas::fill(const std::string& rule) { recordPath(*soft_, OpKind::FillPath, currentPath_, rule == "evenodd"); }
void Canvas::stroke() { recordPath(*soft_, OpKind::StrokePath, currentPath_, false); }
void Canvas::clip(const std::string& rule) { clip(currentPath_, rule); }
void Canvas::fill(const Path2D& p, const std::string& rule) { recordPath(*soft_, OpKind::FillPath, p, rule == "evenodd"); }
void Canvas::stroke(const Path2D& p) { recordPath(*soft_, OpKind::StrokePath, p, false); }
void Canvas::clip(const Path2D& p, const std::string& rule) {
    soft_->clip(p.segments().data(), p.segments().size(), rule == "evenodd");
}
void Canvas::desaturate(const std::string& rule) { recordPath(*soft_, OpKind::Desaturate, currentPath_, rule == "evenodd"); }

namespace {
bool hitPath(const Path2D& p, const canvas_detail::Matrix& m, float x, float y, bool evenOdd) {
    static thread_local canvas_detail::Poly poly;
    canvas_detail::flatten(p, m, canvas_detail::matrixMaxScale(m), poly);
    return canvas_detail::pointInPoly(poly, x, y, evenOdd);
}
bool hitStroke(const Path2D& p, const canvas_detail::Matrix& m, const canvas_detail::StrokeParams& sp, float x, float y) {
    static thread_local canvas_detail::Poly flat, dashed, outline, device;
    canvas_detail::strokeToDevice(p.segments().data(), p.segments().size(), m, sp, flat, dashed, outline, device);
    return canvas_detail::pointInPoly(device, x, y, false);
}
canvas_detail::StrokeParams strokeParams(const canvas_detail::Style& st) {
    canvas_detail::StrokeParams sp;
    sp.width = st.lineWidth; sp.miterLimit = st.miterLimit; sp.cap = st.lineCap; sp.join = st.lineJoin; sp.dash = &st.dash; sp.dashOffset = st.dashOffset;
    return sp;
}
}  // namespace
bool Canvas::isPointInPath(float x, float y, const std::string& rule) const { return hitPath(currentPath_, soft_->st.matrix, x, y, rule == "evenodd"); }
bool Canvas::isPointInPath(const Path2D& p, float x, float y, const std::string& rule) const { return hitPath(p, soft_->st.matrix, x, y, rule == "evenodd"); }
bool Canvas::isPointInStroke(float x, float y) const { return hitStroke(currentPath_, soft_->st.matrix, strokeParams(soft_->st.style), x, y); }
bool Canvas::isPointInStroke(const Path2D& p, float x, float y) const { return hitStroke(p, soft_->st.matrix, strokeParams(soft_->st.style), x, y); }

// ----- text -----
namespace {
void recordText(Canvas::Soft& s, const std::string& text, float x, float y, float maxWidth, bool stroke) {
    using namespace canvas_detail;
    if (text.empty() || !finiteArgs({x, y}) || maxWidth == 0 || std::isnan(maxWidth)) return;
    const auto& st = s.st.style;
    const auto& font = st.font ? st.font : defaultFont();
    if (!font || font->faces.empty()) {
        // No usable system face at all: the built-in bitmap font.
        Path2D path;
        bitmapText(text, x, y, font ? font->size : 10.f, st.textAlign, st.textBaseline, st.rtl, maxWidth, path);
        recordPath(s, stroke ? OpKind::StrokePath : OpKind::FillPath, path, false);
        return;
    }
    const std::size_t start = s.glyphs.size();
    const TextLayout lay = layoutText(text, *font, st.letterSpacing, st.wordSpacing, st.kerning, s.glyphs);
    const float squeeze = (maxWidth > 0 && lay.width > maxWidth && lay.width > 0) ? maxWidth / lay.width : 1.f;
    Op op;
    op.kind = OpKind::Text;
    op.stroke = stroke;
    op.m = s.st.matrix;
    op.f[0] = x + alignShift(lay.width * squeeze, st.textAlign, st.rtl);
    op.f[1] = y + baselineShift(lay, st.textBaseline);
    op.f[2] = squeeze;
    op.f[3] = font->size;
    op.a = static_cast<std::uint32_t>(start);
    op.b = static_cast<std::uint32_t>(s.glyphs.size() - start);
    Box b;
    if (lay.hasInk) addMappedRect(b, op.m, op.f[0] + lay.inkX0 * squeeze, op.f[1] + lay.inkY0, op.f[0] + lay.inkX1 * squeeze, op.f[1] + lay.inkY1);
    b.outset(2 + (stroke ? strokeReach(st.lineWidth, st.lineCap, st.lineJoin, st.miterLimit) * matrixMaxScale(op.m) : 0));
    op.box = lay.hasInk ? s.opBox(b, st) : (compositeIsUnbounded(st.composite) ? s.clipBox(st) : IBox{});
    s.submit(op);
}
}  // namespace
void Canvas::fillText(const std::string& t, float x, float y, float maxWidth) { recordText(*soft_, t, x, y, maxWidth, false); }
void Canvas::strokeText(const std::string& t, float x, float y, float maxWidth) { recordText(*soft_, t, x, y, maxWidth, true); }
float Canvas::measureText(const std::string& t) const { return measureTextMetrics(t).width; }
TextMetrics Canvas::measureTextMetrics(const std::string& text) const {
    using namespace canvas_detail;
    TextMetrics tm;
    const auto& st = soft_->st.style;
    const auto& font = st.font ? st.font : defaultFont();
    if (!font || font->faces.empty()) {
        const float size = font ? font->size : 10.f;
        tm.width = static_cast<float>(text.size()) * 6.f * (size / 8.f);
        return tm;
    }
    std::vector<GlyphPos> tmp;
    const TextLayout lay = layoutText(text, *font, st.letterSpacing, st.wordSpacing, st.kerning, tmp);
    const float shift = alignShift(lay.width, st.textAlign, st.rtl);
    const float b = baselineShift(lay, st.textBaseline);
    tm.width = lay.width;
    if (lay.hasInk) {
        tm.actualBoundingBoxLeft = -(shift + lay.inkX0);
        tm.actualBoundingBoxRight = shift + lay.inkX1;
        tm.actualBoundingBoxAscent = -(b + lay.inkY0);
        tm.actualBoundingBoxDescent = b + lay.inkY1;
    }
    tm.fontBoundingBoxAscent = lay.ascent - b;
    tm.fontBoundingBoxDescent = lay.descent + b;
    tm.emHeightAscent = lay.emAscent - b;
    tm.emHeightDescent = lay.emDescent + b;
    tm.hangingBaseline = lay.ascent * 0.8f - b;
    tm.alphabeticBaseline = -b;
    tm.ideographicBaseline = -(b + lay.descent);
    return tm;
}

// ----- images -----
namespace {
// The spec clips a source rectangle to the image and moves the destination
// with it; negative sizes are normalised, not mirrored.
bool normaliseImageRects(float iw, float ih, float& sx, float& sy, float& sw, float& sh, float& dx, float& dy, float& dw, float& dh) {
    if (!finiteArgs({sx, sy, sw, sh, dx, dy, dw, dh})) return false;
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return false;
    const float kx = dw / sw, ky = dh / sh;
    if (sx < 0) { dx -= sx * kx; dw += sx * kx; sw += sx; sx = 0; }
    if (sy < 0) { dy -= sy * ky; dh += sy * ky; sh += sy; sy = 0; }
    if (sx + sw > iw) { const float over = sx + sw - iw; sw -= over; dw -= over * kx; }
    if (sy + sh > ih) { const float over = sy + sh - ih; sh -= over; dh -= over * ky; }
    return sw > 0 && sh > 0 && dw > 0 && dh > 0;
}
void recordImage(Canvas::Soft& s, Canvas::Soft* src, const std::uint8_t* rgba, int iw, int ih, std::uint32_t key, float sx, float sy,
                 float sw, float sh, float dx, float dy, float dw, float dh, float alpha, const Color* tint) {
    if (!normaliseImageRects(static_cast<float>(iw), static_cast<float>(ih), sx, sy, sw, sh, dx, dy, dw, dh)) return;
    if (!(alpha > 0)) return;
    Op op;
    op.kind = OpKind::Image;
    op.m = s.st.matrix;
    const float v[9] = {sx, sy, sw, sh, dx, dy, dw, dh, alpha};
    std::copy(v, v + 9, op.f);
    if (tint) { op.tinted = true; op.tint = *tint; }
    op.box = s.opBox(rectBox(op.m, dx, dy, dw, dh), s.st.style);
    if (op.box.empty()) { canvas_detail::nativeStats().ops++; canvas_detail::nativeStats().culled++; return; }
    if (src) {
        s.dependOn(src);
        op.src = src;
    } else if (s.deferring && s.layers.empty()) {
        op.a = static_cast<std::uint32_t>(s.images.size());
        s.images.push_back(imageCopy(rgba, iw, ih, key));
    } else {
        op.raw = rgba; op.rw = iw; op.rh = ih;
    }
    s.submit(op);
}
}  // namespace
void Canvas::drawCanvas(const Canvas& src, float dx, float dy) {
    drawCanvas(src, 0, 0, static_cast<float>(src.width_), static_cast<float>(src.height_), dx, dy, static_cast<float>(src.width_), static_cast<float>(src.height_));
}
void Canvas::drawCanvas(const Canvas& src, float dx, float dy, float dw, float dh) {
    drawCanvas(src, 0, 0, static_cast<float>(src.width_), static_cast<float>(src.height_), dx, dy, dw, dh);
}
void Canvas::drawCanvas(const Canvas& src, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh) {
    if (!src.soft_) return;
    recordImage(*soft_, src.soft_.get(), nullptr, src.width_, src.height_, 0, sx, sy, sw, sh, dx, dy, dw, dh, 1.f, nullptr);
}
void Canvas::drawCanvasTinted(const Canvas& src, float sx, float sy, float sw, float sh, float dx, float dy, Color tint) {
    if (!src.soft_) return;
    recordImage(*soft_, src.soft_.get(), nullptr, src.width_, src.height_, 0, sx, sy, sw, sh, dx, dy, sw, sh, 1.f, &tint);
}
void Canvas::drawImage(const std::uint8_t* rgba, int iw, int ih, float dx, float dy, float dw, float dh, float alpha, std::uint32_t key) {
    if (!rgba || iw <= 0 || ih <= 0) return;
    recordImage(*soft_, nullptr, rgba, iw, ih, key, 0, 0, static_cast<float>(iw), static_cast<float>(ih), dx, dy, dw, dh, alpha, nullptr);
}
void Canvas::drawImage(const std::uint8_t* rgba, int iw, int ih, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
                       float alpha, std::uint32_t key) {
    if (!rgba || iw <= 0 || ih <= 0) return;
    recordImage(*soft_, nullptr, rgba, iw, ih, key, sx, sy, sw, sh, dx, dy, dw, dh, alpha, nullptr);
}
void Canvas::drawImage(const ImageLevel* levels, int levelCount, float dx, float dy, float dw, float dh, float alpha, std::uint32_t key) {
    if (!levels || levelCount <= 0) return;
    // The level whose texels are closest to one per device pixel.
    const auto& t = soft_->st.matrix;
    const float ux = dw / std::max(1, levels[0].width), uy = dh / std::max(1, levels[0].height);
    const float stepX = std::hypot(t[0] * ux, t[1] * ux), stepY = std::hypot(t[2] * uy, t[3] * uy);
    float shrink = std::max(stepX > 1e-6f ? 1.f / stepX : 1.f, stepY > 1e-6f ? 1.f / stepY : 1.f);
    int level = 0;
    while (level + 1 < levelCount && shrink >= 2.f) { shrink *= 0.5f; ++level; }
    drawImage(levels[level].rgba, levels[level].width, levels[level].height, dx, dy, dw, dh, alpha, key ? key * 16u + static_cast<std::uint32_t>(level) : 0);
}

std::vector<std::uint8_t> Canvas::getImageData(int x, int y, int w, int h) const {
    std::vector<std::uint8_t> r(static_cast<std::size_t>(std::max(0, w)) * std::max(0, h) * 4);
    soft_->flush();
    const int sx0 = std::max(0, -x), sx1 = std::min(w, width_ - x);
    for (int row = 0; row < h; ++row) {
        const int sy = y + row;
        if (sy < 0 || sy >= height_ || sx1 <= sx0) continue;
        std::memcpy(&r[(static_cast<std::size_t>(row) * w + sx0) * 4], &soft_->px[static_cast<std::size_t>(sy) * width_ + x + sx0], static_cast<std::size_t>(sx1 - sx0) * 4);
    }
    return r;
}
void Canvas::putImageData(const std::vector<std::uint8_t>& rgba, int sw, int sh, int dx, int dy) {
    putImageData(rgba, sw, sh, dx, dy, 0, 0, sw, sh);
}
void Canvas::putImageData(const std::vector<std::uint8_t>& rgba, int sw, int sh, int dx, int dy, int dirtyX, int dirtyY, int dirtyW, int dirtyH) {
    if (sw <= 0 || sh <= 0 || rgba.size() < static_cast<std::size_t>(sw) * sh * 4) return;
    if (dirtyW < 0) { dirtyX += dirtyW; dirtyW = -dirtyW; }
    if (dirtyH < 0) { dirtyY += dirtyH; dirtyH = -dirtyH; }
    if (dirtyX < 0) { dirtyW += dirtyX; dirtyX = 0; }
    if (dirtyY < 0) { dirtyH += dirtyY; dirtyY = 0; }
    dirtyW = std::min(dirtyW, sw - dirtyX);
    dirtyH = std::min(dirtyH, sh - dirtyY);
    if (dirtyW <= 0 || dirtyH <= 0) return;
    Soft& s = *soft_;
    Op op;
    op.kind = OpKind::PutImage;
    op.f[0] = static_cast<float>(dx + dirtyX); op.f[1] = static_cast<float>(dy + dirtyY);
    op.f[2] = static_cast<float>(dirtyW); op.f[3] = static_cast<float>(dirtyH);
    op.box = canvas_detail::IBox{dx + dirtyX, dy + dirtyY, dx + dirtyX + dirtyW, dy + dirtyY + dirtyH}.intersect(s.canvasBox());
    if (s.deferring && s.layers.empty()) {
        // Only the dirty rectangle is kept.
        op.a = static_cast<std::uint32_t>(s.bytes.size());
        op.b = static_cast<std::uint32_t>(dirtyW);
        for (int row = 0; row < dirtyH; ++row) {
            const std::uint8_t* from = rgba.data() + (static_cast<std::size_t>(dirtyY + row) * sw + dirtyX) * 4;
            s.bytes.insert(s.bytes.end(), from, from + static_cast<std::size_t>(dirtyW) * 4);
        }
    } else {
        op.raw = rgba.data() + (static_cast<std::size_t>(dirtyY) * sw + dirtyX) * 4;
        op.b = static_cast<std::uint32_t>(sw);
    }
    s.submit(op);
}
void Canvas::fillCircle(float a, float b, float c) { beginPath(); arc(a, b, c, 0, 6.283185307f); fill(); }
void Canvas::strokeCircle(float a, float b, float c) { beginPath(); arc(a, b, c, 0, 6.283185307f); stroke(); }

// ----- layers -----
void Canvas::beginLayer() {
    Soft& s = *soft_;
    s.beforeImmediateWrite();
    save();
    Soft::LayerFrame frame;
    frame.captured = s.st.style;
    frame.parent.assign(s.px.size(), Color{0, 0, 0, 0});
    frame.parent.swap(s.px);
    s.layers.push_back(std::move(frame));
    // Inside the layer these start over; they apply when the layer is drawn.
    auto& st = s.st.style;
    st.alpha = 1;
    st.composite = canvas_detail::CompositeOp::SourceOver;
    st.filter = nullptr;
    st.shadowColor = Color{0, 0, 0, 0};
    s.touch();
}
void Canvas::endLayer() {
    using namespace canvas_detail;
    Soft& s = *soft_;
    if (s.layers.empty()) return;
    s.flushReaders();
    Soft::LayerFrame frame = std::move(s.layers.back());
    s.layers.pop_back();
    restore();
    // The layer's pixels, premultiplied, over the box they actually cover.
    std::vector<Color> layerPixels;
    layerPixels.swap(s.px);
    s.px = std::move(frame.parent);
    int x0 = s.w, y0 = s.h, x1 = 0, y1 = 0;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
            if (layerPixels[static_cast<std::size_t>(y) * s.w + x].a) { x0 = std::min(x0, x); x1 = std::max(x1, x + 1); y0 = std::min(y0, y); y1 = std::max(y1, y + 1); }
    if (x0 >= x1 && !compositeIsUnbounded(frame.captured.composite)) return;
    if (x0 >= x1) { x0 = y0 = 0; x1 = y1 = 1; }
    Style style = frame.captured;
    style.clip = s.st.style.clip;
    Shader shader;
    shader.kind = Shader::Image;
    shader.img = reinterpret_cast<const std::uint8_t*>(layerPixels.data());
    shader.iw = s.w; shader.ih = s.h;
    shader.exact = true;
    shader.u1 = static_cast<float>(s.w); shader.v1 = static_cast<float>(s.h);
    const IBox box{x0, y0, x1, y1};
    s.paint(style, shader, box, nullptr, [&](auto& sink, const IBox& limit) {
        produceRect(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1), limit, sink);
    });
}

// ----- flushing -----
void Canvas::flush() { soft_->flush(); }
void Canvas::setCulling(bool enabled) { cullingEnabled_ = enabled; soft_->cullingEnabled = enabled; }

bool Canvas::savePPM(const std::string& path) const {
    soft_->flush();
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << width_ << " " << height_ << "\n255\n";
    for (const Color& p : soft_->px) {
        const float a = p.a / 255.f;
        f.put(static_cast<char>(std::lround(p.r * a + 255 * (1 - a))));
        f.put(static_cast<char>(std::lround(p.g * a + 255 * (1 - a))));
        f.put(static_cast<char>(std::lround(p.b * a + 255 * (1 - a))));
    }
    return !!f;
}

bool canvasRegisterFont(const std::string& family, const std::vector<std::uint8_t>& data, int weight, bool italic) {
    return canvas_detail::FontRegistry::instance().registerFont(family, data, weight, italic);
}
int canvasOpsEmitted() { return canvas_detail::nativeStats().ops; }
const int* canvasOpTypeCounts() { return canvas_detail::nativeOpCounts(); }
CanvasFrameStats canvasTakeFrameStats() {
    const CanvasFrameStats s = canvas_detail::nativeStats();
    canvas_detail::nativeStats() = CanvasFrameStats{};
    return s;
}
const char* canvasOpName(int code) {
    static const char* const kNames[] = {"fillPath", "strokePath", "fillRect", "strokeRect", "clearRect", "text", "image", "putImageData", "desaturate"};
    return code >= 0 && code < 9 ? kNames[code] : nullptr;
}
#endif  // native

// showWindow() is in window.cpp, with the rest of the SDL code: nothing here
// needs SDL's headers.
