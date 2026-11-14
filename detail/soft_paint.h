#pragma once
// Native backend, part 1: paint sources, clip masks, fonts and text layout.
// Included only by canvas.cpp (after CanvasGradient::Data and
// CanvasPattern::Data are defined there).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../canvas.h"
#include "../font.h"
#include "css.h"
#include "effects.h"
#include "geometry.h"
#include "raster.h"

namespace canvas_detail {

// ---------------------------------------------------------------------------
// Gradients
// ---------------------------------------------------------------------------
constexpr int kGradientLut = 1024;

struct GradientImpl {
    int kind = 0;  // 0 linear, 1 radial, 2 conic
    float g[6]{};
    std::array<std::array<std::uint8_t, 4>, kGradientLut> lut{};  // premultiplied
    bool opaque = false;
    bool paintsNothing = true;
};

// The colour ramp per the canvas spec: stops in offset order (insertion
// order among equal offsets, the last of which wins from there on), colours
// interpolated in UNpremultiplied RGBA, padded beyond the ends.
inline std::shared_ptr<const GradientImpl> resolveGradient(CanvasGradient::Data& d) {
    if (d.nativeCache && d.nativeCacheVersion == d.version)
        return std::static_pointer_cast<const GradientImpl>(d.nativeCache);
    auto impl = std::make_shared<GradientImpl>();
    impl->kind = d.kind;
    std::copy(d.g, d.g + 6, impl->g);
    std::vector<std::pair<float, Color>> stops = d.stops;
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const bool degenerate =
        (d.kind == 0 && d.g[0] == d.g[2] && d.g[1] == d.g[3]) ||
        (d.kind == 1 && d.g[0] == d.g[3] && d.g[1] == d.g[4] && d.g[2] == d.g[5]);
    impl->paintsNothing = stops.empty() || degenerate;
    if (!impl->paintsNothing) {
        bool opaque = true;
        for (const auto& s : stops) opaque = opaque && s.second.a == 255;
        impl->opaque = opaque;
        for (int i = 0; i < kGradientLut; ++i) {
            const float t = static_cast<float>(i) / (kGradientLut - 1);
            std::size_t hi = 0;
            while (hi < stops.size() && stops[hi].first <= t) ++hi;
            Color c;
            if (hi == 0) c = stops.front().second;
            else if (hi == stops.size()) c = stops.back().second;
            else {
                const auto& a = stops[hi - 1];
                const auto& b = stops[hi];
                const float span = b.first - a.first;
                const float f = span > 0 ? (t - a.first) / span : 1.f;
                const auto mix = [f](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(std::lround(x + (y - x) * f)); };
                c = Color{mix(a.second.r, b.second.r), mix(a.second.g, b.second.g), mix(a.second.b, b.second.b), mix(a.second.a, b.second.a)};
            }
            premultiply(c, impl->lut[static_cast<std::size_t>(i)].data());
        }
    }
    d.nativeCache = impl;
    d.nativeCacheVersion = d.version;
    return impl;
}

// ---------------------------------------------------------------------------
// Paint styles (fill/stroke)
// ---------------------------------------------------------------------------
struct PaintStyle {
    enum Kind : std::uint8_t { Solid, Gradient, Pattern } kind = Solid;
    Color color{0, 0, 0, 255};
    std::shared_ptr<CanvasGradient::Data> gradient;
    std::shared_ptr<CanvasPattern::Data> pattern;
    // What a recorded draw keeps: the gradient's stops and the pattern's
    // transform as they were when it was recorded.
    std::shared_ptr<const GradientImpl> frozenGradient;
    std::array<float, 6> patternMatrix{1, 0, 0, 1, 0, 0};
    bool frozen = false;
    void freeze() {
        if (kind == Gradient && gradient) frozenGradient = resolveGradient(*gradient);
        if (kind == Pattern && pattern) patternMatrix = pattern->transform;
        frozen = true;
    }
    std::uint32_t dataVersion() const {
        if (kind == Gradient && gradient) return gradient->version;
        if (kind == Pattern && pattern) return pattern->version;
        return 0;
    }
    std::shared_ptr<const GradientImpl> gradientImpl() const {
        if (frozen && frozenGradient) return frozenGradient;
        return gradient ? resolveGradient(*gradient) : nullptr;
    }
    std::array<float, 6> patternTransform() const { return frozen || !pattern ? patternMatrix : pattern->transform; }
    // Every pixel the paint reaches is opaque (with full coverage).
    bool opaque() const {
        switch (kind) {
        case Solid: return color.a == 255;
        case Gradient: { auto g = gradientImpl(); return g && !g->paintsNothing && g->opaque && g->kind != 1; }
        case Pattern: return pattern && pattern->opaque && pattern->repeatX && pattern->repeatY;
        }
        return false;
    }
    bool paintsNothing() const {
        switch (kind) {
        case Solid: return color.a == 0;
        case Gradient: { auto g = gradientImpl(); return !g || g->paintsNothing; }
        case Pattern: return !pattern || pattern->width <= 0 || pattern->height <= 0;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Clip masks
// ---------------------------------------------------------------------------
// A clip is a coverage mask over its bounds, or -- the common case, a
// pixel-aligned rectangle -- just the bounds. Each mask row also records the
// run where coverage is full, so painting can skip per-pixel lookups there.
// One row of an 8-bit mask, indexed by ABSOLUTE x (the origin is subtracted
// at each access, so no pointer outside the row is ever formed).
struct RowView {
    const std::uint8_t* base = nullptr;
    int origin = 0;
    std::uint8_t operator[](int x) const { return base[x - origin]; }
    const std::uint8_t* at(int x) const { return base + (x - origin); }
    explicit operator bool() const { return base != nullptr; }
};

struct ClipMask {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool rect = true;
    std::vector<std::uint8_t> alpha;
    std::vector<int> fullX0, fullX1;
    bool empty() const { return x0 >= x1 || y0 >= y1; }
    int stride() const { return x1 - x0; }
    std::uint8_t at(int x, int y) const {
        if (x < x0 || x >= x1 || y < y0 || y >= y1) return 0;
        if (rect) return 255;
        return alpha[static_cast<std::size_t>(y - y0) * stride() + (x - x0)];
    }
    // Indexed by absolute x, valid for x0 <= x < x1.
    RowView row(int y) const { return {alpha.data() + static_cast<std::size_t>(y - y0) * static_cast<std::size_t>(stride()), x0}; }
    void computeRuns() {
        fullX0.assign(static_cast<std::size_t>(std::max(0, y1 - y0)), x0);
        fullX1.assign(static_cast<std::size_t>(std::max(0, y1 - y0)), x0);
        if (rect) { std::fill(fullX1.begin(), fullX1.end(), x1); return; }
        for (int y = y0; y < y1; ++y) {
            const RowView r = row(y);
            int best0 = x0, best1 = x0, cur0 = -1;
            for (int x = x0; x <= x1; ++x) {
                const bool full = x < x1 && r[x] == 255;
                if (full && cur0 < 0) cur0 = x;
                if (!full && cur0 >= 0) { if (x - cur0 > best1 - best0) { best0 = cur0; best1 = x; } cur0 = -1; }
            }
            fullX0[static_cast<std::size_t>(y - y0)] = best0;
            fullX1[static_cast<std::size_t>(y - y0)] = best1;
        }
    }
};

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
inline std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string squashName(const std::string& s) {
    std::string out;
    for (char c : s) if (c != ' ' && c != '-' && c != '_') out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

class FontRegistry {
public:
    static FontRegistry& instance() { static FontRegistry r; return r; }

    bool registerFont(const std::string& family, const std::vector<std::uint8_t>& data, int weight, bool italic) {
        auto font = std::make_unique<Font>();
        if (!font->loadFromMemory(data)) return false;
        registered_[lowerAscii(family)].push_back({weight, italic, font.get()});
        owned_.push_back(std::move(font));
        cache_.clear();
        ++generation_;
        return true;
    }
    std::uint32_t generation() const { return generation_; }

    // The face for one family name at a weight and style, or null.
    const Font* face(const std::string& family, bool generic, int weight, bool italic) {
        const std::string key = lowerAscii(family) + (generic ? "#g" : "#n") + std::to_string(weight) + (italic ? "i" : "r");
        const auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const Font* f = lookup(lowerAscii(family), generic, weight, italic);
        cache_[key] = f;
        return f;
    }
    // A system face with a glyph for `cp`, for characters no listed family has.
    const Font* fallbackFor(std::uint32_t cp, int weight, bool italic) {
        const auto found = fallbackCache_.find(cp);
        if (found != fallbackCache_.end()) return found->second;
        static const char* kFallbacks[] = {"Arial Unicode", "Arial Unicode MS", "Apple Symbols", "DejaVuSans", "DejaVu Sans",
                                           "NotoSans-Regular", "Noto Sans", "Segoe UI Symbol", "Hiragino Sans GB", "AppleGothic",
                                           "Arial", "Helvetica", "Geneva", "LastResort"};
        const Font* result = nullptr;
        for (const char* name : kFallbacks) {
            const Font* f = face(name, false, weight, italic);
            if (f && f->glyphIndex(cp) != 0) { result = f; break; }
        }
        if (fallbackCache_.size() > 4096) fallbackCache_.clear();
        fallbackCache_[cp] = result;
        return result;
    }

private:
    struct Registered { int weight; bool italic; const Font* font; };
    const Font* lookup(const std::string& family, bool generic, int weight, bool italic) {
        const auto reg = registered_.find(family);
        if (reg != registered_.end() && !reg->second.empty()) {
            const Registered* best = nullptr;
            int bestScore = 1 << 30;
            for (const Registered& r : reg->second) {
                const int score = std::abs(r.weight - weight) + (r.italic != italic ? 1000 : 0);
                if (score < bestScore) { bestScore = score; best = &r; }
            }
            return best->font;
        }
        std::vector<std::string> candidates;
        if (generic) {
            if (family == "serif" || family == "ui-serif") candidates = {"Times", "Times New Roman", "Georgia", "DejaVuSerif", "LiberationSerif-Regular", "NotoSerif-Regular"};
            else if (family == "monospace" || family == "ui-monospace") candidates = {"Menlo", "Courier New", "Courier", "Monaco", "DejaVuSansMono", "LiberationMono-Regular", "NotoSansMono-Regular"};
            else if (family == "cursive") candidates = {"Apple Chancery", "Comic Sans MS", "Brush Script", "Chalkboard"};
            else if (family == "fantasy") candidates = {"Papyrus", "Impact", "Herculanum"};
            else candidates = {"Helvetica", "Arial", "Helvetica Neue", "DejaVuSans", "LiberationSans-Regular", "NotoSans-Regular", "Verdana", "Geneva"};
        } else {
            candidates = {family};
        }
        std::string path;
        int faceIndex = 0;
        for (const std::string& name : candidates) {
            if (!Font::findSystemFont({name}, weight, italic, path, faceIndex)) continue;
            // findSystemFont falls back to some face when the name is
            // unknown; for a NAMED family that is a miss, so the next family
            // in the font list gets its turn.
            const std::string file = squashName(path.substr(path.find_last_of("/\\") + 1));
            if (!generic && file.find(squashName(name)) == std::string::npos) continue;
            const std::string key = path + "#" + std::to_string(faceIndex);
            const auto known = byPath_.find(key);
            if (known != byPath_.end()) return known->second;
            auto font = std::make_unique<Font>();
            if (!font->loadFromFile(path, faceIndex)) { byPath_[key] = nullptr; continue; }
            const Font* f = font.get();
            owned_.push_back(std::move(font));
            byPath_[key] = f;
            return f;
        }
        return nullptr;
    }
    std::map<std::string, std::vector<Registered>> registered_;
    std::unordered_map<std::string, const Font*> cache_, byPath_;
    std::unordered_map<std::uint32_t, const Font*> fallbackCache_;
    std::vector<std::unique_ptr<Font>> owned_;
    std::uint32_t generation_ = 0;
};

struct ResolvedFont {
    CssFont css;
    std::vector<const Font*> faces;  // the families that resolved, in order
    float size = 10;
};

inline std::shared_ptr<const ResolvedFont> resolveFont(const std::string& text) {
    static std::unordered_map<std::string, std::shared_ptr<const ResolvedFont>> cache;
    static std::uint32_t generation = 0;
    FontRegistry& reg = FontRegistry::instance();
    if (generation != reg.generation()) { cache.clear(); generation = reg.generation(); }
    const auto found = cache.find(text);
    if (found != cache.end()) return found->second;
    CssFont css;
    if (!parseCssFont(text, css)) return nullptr;
    auto r = std::make_shared<ResolvedFont>();
    r->css = css;
    r->size = css.sizePx;
    for (const CssFontFamily& fam : css.families) {
        const Font* f = reg.face(fam.name, fam.generic, css.weight, css.italic);
        if (f && std::find(r->faces.begin(), r->faces.end(), f) == r->faces.end()) r->faces.push_back(f);
    }
    // Every list ends in the default family, as a browser's does.
    if (const Font* f = reg.face("sans-serif", true, css.weight, css.italic))
        if (std::find(r->faces.begin(), r->faces.end(), f) == r->faces.end()) r->faces.push_back(f);
    if (cache.size() > 256) cache.clear();
    cache[text] = r;
    return r;
}

// ---------------------------------------------------------------------------
// Text layout
// ---------------------------------------------------------------------------
enum : std::uint8_t { kAlignStart, kAlignEnd, kAlignLeft, kAlignRight, kAlignCenter };
enum : std::uint8_t { kBaseAlphabetic, kBaseTop, kBaseHanging, kBaseMiddle, kBaseIdeographic, kBaseBottom };

struct GlyphPos {
    const Font* face;
    std::uint16_t glyph;
    float x;  // pen position from the run's origin, user units
};

struct TextLayout {
    float width = 0;
    float ascent = 0, descent = 0;          // font box, positive
    float emAscent = 0, emDescent = 0;      // em box split, positive
    float inkX0 = 0, inkX1 = 0, inkY0 = 0, inkY1 = 0;  // relative to the origin, y down
    bool hasInk = false;
};

// Lays `text` out left to right (no bidi), with per-glyph fallback across the
// font's families and then the system's, kerning within a face, and the
// canvas's letter and word spacing. Whitespace characters become spaces, as
// the canvas spec says.
inline TextLayout layoutText(const std::string& text, const ResolvedFont& font, float letterSpacing, float wordSpacing,
                             bool kerning, std::vector<GlyphPos>& out) {
    TextLayout lay;
    const float size = font.size;
    if (font.faces.empty()) return lay;
    const Font* primary = font.faces.front();
    const Font::FaceMetrics fm = primary->faceMetrics(size);
    lay.ascent = fm.ascent;
    lay.descent = fm.descent;
    const float em = fm.ascent + fm.descent;
    lay.emAscent = em > 0 ? size * fm.ascent / em : size * 0.8f;
    lay.emDescent = size - lay.emAscent;
    float pen = 0;
    const Font* prevFace = nullptr;
    std::uint16_t prevGlyph = 0;
    std::uint32_t prevCp = ' ';
    std::size_t i = 0;
    float ix0 = 1e30f, ix1 = -1e30f, iy0 = 1e30f, iy1 = -1e30f;
    while (i < text.size()) {
        std::uint32_t cp = Font::nextCodepoint(text, i);
        if (cp == '\t' || cp == '\n' || cp == '\f' || cp == '\r') cp = ' ';
        const Font* face = nullptr;
        std::uint16_t glyph = 0;
        for (const Font* f : font.faces) {
            const std::uint16_t g = f->glyphIndex(cp);
            if (g != 0) { face = f; glyph = g; break; }
        }
        if (!face && cp > 0x7f) {
            if (const Font* f = FontRegistry::instance().fallbackFor(cp, font.css.weight, font.css.italic)) { face = f; glyph = f->glyphIndex(cp); }
        }
        if (!face) { face = primary; glyph = 0; }
        // Kerned within a face and within a word: the browser shapes canvas
        // text a word at a time, so no pair across a space is ever applied.
        if (kerning && face == prevFace && prevFace && !Font::isWordDelimiter(prevCp) && !Font::isWordDelimiter(cp))
            pen += face->kerning(prevGlyph, glyph, size);
        out.push_back({face, glyph, pen});
        float gx0, gy0, gx1, gy1;
        if (face->glyphBounds(glyph, size, gx0, gy0, gx1, gy1)) {
            ix0 = std::min(ix0, pen + gx0); ix1 = std::max(ix1, pen + gx1);
            iy0 = std::min(iy0, gy0); iy1 = std::max(iy1, gy1);
            lay.hasInk = true;
        }
        pen += face->glyphAdvance(glyph, size) + letterSpacing;
        if (cp == ' ') pen += wordSpacing;
        prevFace = face;
        prevGlyph = glyph;
        prevCp = cp;
    }
    lay.width = pen;
    if (lay.hasInk) { lay.inkX0 = ix0; lay.inkX1 = ix1; lay.inkY0 = iy0; lay.inkY1 = iy1; }
    return lay;
}

// How far below the anchor y the alphabetic baseline sits, per textBaseline
// (Blink's CanvasRenderingContext2D::GetFontBaseline).
inline float baselineShift(const TextLayout& lay, std::uint8_t baseline) {
    switch (baseline) {
    case kBaseTop: return lay.emAscent;
    case kBaseHanging: return lay.ascent * 0.8f;
    case kBaseMiddle: return (lay.emAscent - lay.emDescent) / 2;
    case kBaseIdeographic: return -lay.descent;
    case kBaseBottom: return -lay.emDescent;
    default: return 0;
    }
}
inline float alignShift(float width, std::uint8_t align, bool rtl) {
    std::uint8_t a = align;
    if (a == kAlignStart) a = rtl ? kAlignRight : kAlignLeft;
    if (a == kAlignEnd) a = rtl ? kAlignLeft : kAlignRight;
    if (a == kAlignCenter) return -width / 2;
    if (a == kAlignRight) return -width;
    return 0;
}

// The browser blends a FILLED glyph's coverage up a ramp (its mask
// rasterizer's contrast), a shape's linearly, and how steep the ramp is
// depends on the text's own colour: light text on dark is pushed up much
// harder than dark text on light (Skia's luminance-dependent preblend). The
// ends are measured against Chrome: coverage^(1/3.2) for white text (on a
// bundled face, in florr.io_clone), and the dark end against the system
// faces (tools/conformance). Stroked glyphs are shapes and stay linear.
constexpr float kTextGammaLight = 3.2f;
constexpr float kTextGammaDark = 1.45f;
// A 256-entry coverage ramp for text of colour `c`, from 16 cached steps of
// luminance.
inline const std::uint8_t* textGammaLut(Color c) {
    // Built once, whole, so threads drawing text at once only read it.
    static const std::array<std::array<std::uint8_t, 256>, 16> luts = [] {
        std::array<std::array<std::uint8_t, 256>, 16> l{};
        for (int bucket = 0; bucket < 16; ++bucket) {
            const float t = bucket / 15.f, s = t * t * (3 - 2 * t);
            const float g = kTextGammaDark + (kTextGammaLight - kTextGammaDark) * s;
            for (int i = 0; i < 256; ++i)
                l[static_cast<std::size_t>(bucket)][static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(std::pow(i / 255.f, 1.f / g) * 255.f));
        }
        return l;
    }();
    const float lum = (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.f;
    const int bucket = std::clamp(static_cast<int>(lum * 15.f + 0.5f), 0, 15);
    return luts[static_cast<std::size_t>(bucket)].data();
}

// Glyph coverage masks, rasterized once per face, glyph, device size and
// quarter-pixel horizontal phase. Text in an upright transform is drawn from
// these; anything rotated, skewed or squeezed is drawn as outlines.
struct GlyphMask {
    int left = 0, top = 0, w = 0, h = 0;  // relative to the integer pen position
    std::vector<std::uint8_t> cov;
};
class GlyphCache {
public:
    static GlyphCache& instance() { static GlyphCache c; return c; }
    // While pinned -- a flush drawing on several threads -- lookups lock, and
    // nothing is evicted, so a mask handed out stays valid (the map's nodes
    // do not move when it grows). pin() and unpin() are the flushing
    // thread's, outside the parallel part.
    void pin() { ++pinned_; }
    void unpin() { --pinned_; }
    const GlyphMask& get(const Font* face, std::uint16_t glyph, float sizePx, int phase) {
        const std::uint32_t sizeQ = static_cast<std::uint32_t>(std::lround(sizePx * 64));
        const Key key{face, glyph, sizeQ, static_cast<std::uint8_t>(phase)};
        std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
        if (pinned_) lock.lock();
        const auto found = map_.find(key);
        if (found != map_.end()) return found->second;
        if (bytes_ > (32u << 20) && !pinned_) { map_.clear(); bytes_ = 0; }
        GlyphMask& m = map_[key];
        build(face, glyph, sizeQ / 64.f, phase, m);
        bytes_ += m.cov.size() + 64;
        return m;
    }
private:
    struct Key {
        const Font* face; std::uint16_t glyph; std::uint32_t size; std::uint8_t phase;
        bool operator==(const Key& o) const { return face == o.face && glyph == o.glyph && size == o.size && phase == o.phase; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::size_t h = std::hash<const void*>()(k.face);
            h ^= (static_cast<std::size_t>(k.glyph) << 1) ^ (static_cast<std::size_t>(k.size) << 17) ^ (static_cast<std::size_t>(k.phase) << 13);
            return h * 0x9E3779B97F4A7C15ull;
        }
    };
    static void build(const Font* face, std::uint16_t glyph, float size, int phase, GlyphMask& m) {
        const Path2D* outline = face->glyphOutline(glyph);
        if (!outline || outline->empty()) return;
        const float s = size / face->unitsPerEm();
        const Matrix mat{s, 0, 0, s, phase * 0.25f, 0};
        static thread_local Poly poly;
        flatten(*outline, mat, 1.f, poly);
        int bx0, by0, bx1, by1;
        if (!polyBounds(poly, -100000, -100000, 100000, 100000, bx0, by0, bx1, by1)) return;
        m.left = bx0; m.top = by0; m.w = bx1 - bx0; m.h = by1 - by0;
        m.cov.assign(static_cast<std::size_t>(m.w) * m.h, 0);
        scanFill(poly, false, bx0, by0, bx1, by1, [&](int x0, int x1, int y, float c) {
            // Linear coverage: the ramp is applied when it is drawn, by colour.
            const std::uint8_t v = static_cast<std::uint8_t>(std::lround(c * 255));
            std::uint8_t* row = m.cov.data() + static_cast<std::size_t>(y - by0) * static_cast<std::size_t>(m.w);
            for (int x = x0; x < x1; ++x) row[x - bx0] = v;
        }, kMaxSlabBreaksExact);
    }
    std::unordered_map<Key, GlyphMask, KeyHash> map_;
    std::size_t bytes_ = 0;
    std::mutex mutex_;
    int pinned_ = 0;
};

// The 5x7 cell font the native backend falls back on when no system face can
// be found at all (column-major bits, ASCII 32..126).
inline const unsigned char (&bitmapFont())[95][5] {
    static const unsigned char kFont[95][5] = {
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5f,0x00,0x00}, {0x00,0x03,0x00,0x03,0x00}, {0x14,0x7f,0x14,0x7f,0x14}, {0x24,0x2a,0x7f,0x2a,0x12},
        {0x63,0x13,0x08,0x64,0x63}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x00,0x03,0x00,0x00}, {0x00,0x1c,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1c,0x00},
        {0x2a,0x1c,0x3e,0x1c,0x2a}, {0x08,0x08,0x3e,0x08,0x08}, {0x00,0x40,0x30,0x10,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
        {0x60,0x10,0x08,0x04,0x03}, {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00}, {0x62,0x51,0x49,0x49,0x46}, {0x41,0x49,0x49,0x49,0x36},
        {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
        {0x06,0x49,0x49,0x29,0x1e}, {0x00,0x36,0x36,0x00,0x00}, {0x40,0x36,0x16,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
        {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x59,0x09,0x06}, {0x3e,0x41,0x5d,0x55,0x1e}, {0x7c,0x12,0x11,0x12,0x7c}, {0x7f,0x49,0x49,0x49,0x36},
        {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01}, {0x3e,0x41,0x49,0x49,0x3a},
        {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00}, {0x30,0x40,0x40,0x41,0x3f}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
        {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x06,0x08,0x30,0x7f}, {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e},
        {0x7f,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
        {0x7f,0x20,0x18,0x20,0x7f}, {0x63,0x14,0x08,0x14,0x63}, {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x7f,0x41,0x41},
        {0x03,0x04,0x08,0x10,0x60}, {0x41,0x41,0x7f,0x00,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x00,0x00},
        {0x20,0x54,0x54,0x54,0x78}, {0x7f,0x44,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x44,0x7f}, {0x38,0x54,0x54,0x54,0x18},
        {0x08,0x7e,0x09,0x09,0x02}, {0x0c,0x52,0x52,0x52,0x3e}, {0x7f,0x04,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00}, {0x20,0x40,0x44,0x3d,0x00},
        {0x7f,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x38,0x04,0x78}, {0x7c,0x04,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
        {0x7e,0x12,0x12,0x12,0x0c}, {0x0c,0x12,0x12,0x12,0x7e}, {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x4c,0x54,0x74,0x24}, {0x04,0x3f,0x44,0x44,0x20},
        {0x3c,0x40,0x40,0x40,0x7c}, {0x1c,0x20,0x40,0x20,0x1c}, {0x3c,0x40,0x38,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44}, {0x0e,0x50,0x50,0x50,0x3e},
        {0x44,0x64,0x54,0x4c,0x44}, {0x00,0x08,0x36,0x41,0x41}, {0x00,0x00,0x7f,0x00,0x00}, {0x41,0x41,0x36,0x08,0x00}, {0x18,0x04,0x08,0x10,0x0c},
    };
    return kFont;
}
inline void bitmapText(const std::string& text, float x, float y, float size, std::uint8_t align, std::uint8_t baseline, bool rtl,
                       float maxWidth, Path2D& out) {
    const float unit = size / 8.f, advance = 6 * unit, height = 7 * unit;
    const float full = static_cast<float>(text.size()) * advance;
    const float squeeze = (maxWidth > 0 && full > maxWidth) ? maxWidth / full : 1.f;
    float penX = x + alignShift(full * squeeze, align, rtl);
    const float penY = y - (baseline == kBaseTop || baseline == kBaseHanging ? 0.f : baseline == kBaseMiddle ? height / 2 : height);
    for (unsigned char ch : text) {
        if (ch >= 32 && ch < 127) {
            const unsigned char* columns = bitmapFont()[ch - 32];
            for (int c = 0; c < 5; ++c) for (int r = 0; r < 7; ++r)
                if (columns[c] >> r & 1) out.rect(penX + c * unit * squeeze, penY + r * unit, unit * squeeze + 0.01f, unit + 0.01f);
        }
        penX += advance * squeeze;
    }
}

}  // namespace canvas_detail
