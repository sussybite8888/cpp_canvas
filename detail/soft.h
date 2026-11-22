#pragma once
// Native backend, part 2: the drawing state, shaders and painters, the HTML
// canvas drawing model (layer -> filter -> shadow -> composite), and the
// deferred display list whose flush culls fully-covered draws.
//
// Included only by canvas.cpp.

#include <atomic>
#include <chrono>

#include "soft_paint.h"

namespace canvas_detail {

// ---------------------------------------------------------------------------
// Drawing state. Everything but the matrix is the STYLE, which a recorded
// draw keeps a snapshot of; the matrix goes in the draw itself, because it
// changes far more often than anything else.
// ---------------------------------------------------------------------------
struct Style {
    PaintStyle fill, stroke;
    float lineWidth = 1, alpha = 1, miterLimit = 10, dashOffset = 0;
    std::vector<float> dash;
    std::uint8_t lineCap = 0, lineJoin = 0;
    CompositeOp composite = CompositeOp::SourceOver;
    Color shadowColor{0, 0, 0, 0};
    float shadowBlur = 0, shadowX = 0, shadowY = 0;
    std::shared_ptr<const std::vector<CssFilterFunction>> filter;
    bool smoothing = true;
    std::uint8_t smoothingQuality = 0;  // low, medium, high
    std::shared_ptr<const ResolvedFont> font;
    std::uint8_t textAlign = kAlignStart, textBaseline = kBaseAlphabetic;
    bool rtl = false;
    float letterSpacing = 0, wordSpacing = 0;
    bool kerning = true;
    std::shared_ptr<const ClipMask> clip;
    bool shadowVisible() const { return shadowColor.a != 0 && (shadowBlur > 0 || shadowX != 0 || shadowY != 0); }
    bool plain() const { return composite == CompositeOp::SourceOver && !filter && !shadowVisible(); }
};
struct State {
    Matrix matrix{1, 0, 0, 1, 0, 0};
    Style style;
    std::uint64_t version = 1;
};


// ---------------------------------------------------------------------------
// Shaders: the colour of a paint at device pixel centres, premultiplied.
// ---------------------------------------------------------------------------
inline bool allOpaque(const std::uint8_t* rgba, std::size_t pixels) {
    for (std::size_t i = 0; i < pixels; ++i) if (rgba[i * 4 + 3] != 255) return false;
    return true;
}

struct Shader {
    enum Kind : std::uint8_t { Solid, Gradient, Image } kind = Solid;
    Color color{0, 0, 0, 255};  // Solid, straight
    const GradientImpl* grad = nullptr;
    Matrix inv{1, 0, 0, 1, 0, 0};  // device -> paint space
    const std::uint8_t* img = nullptr;
    int iw = 0, ih = 0;
    bool repeatX = false, repeatY = false;
    bool clampOutside = false;  // drawImage: edge pixels sample the edge; patterns: transparent beyond
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool bilinear = true;
    int grid = 1;
    bool exact = false;  // device pixel (x, y) is texel (x + offX, y + offY)
    int offX = 0, offY = 0;
    bool tinted = false;
    Color tint{255, 255, 255, 255};

    void premulTexel(int i, int j, float& r, float& g, float& b, float& a) const {
        if (i < 0 || j < 0) { r = g = b = a = 0; return; }  // a decal edge: transparent beyond
        const std::uint8_t* p = img + (static_cast<std::size_t>(j) * iw + i) * 4;
        const float al = p[3] * (1.f / 255.f);
        r = p[0] * al; g = p[1] * al; b = p[2] * al; a = p[3];
    }
    // Wraps or clamps a texel index on one axis; false when it falls outside a
    // non-repeating, non-clamping axis.
    // A texel index on one axis: wrapped when repeating, clamped to the edge
    // when clamping (drawImage), and -1 -- transparent -- past a decal edge
    // (a pattern that does not repeat), which is what fades its border.
    static bool axis(int& i, bool repeat, bool clampOutside, int lo, int hi, int size) {
        if (repeat) { i %= size; if (i < 0) i += size; return true; }
        if (i < lo || i >= hi) { if (!clampOutside) { i = -1; return false; } i = std::clamp(i, lo, hi - 1); }
        return true;
    }
    void sampleImage(float u, float v, float& r, float& g, float& b, float& a) const {
        r = g = b = a = 0;
        const int lo = static_cast<int>(std::floor(u0)), hi = std::max(lo + 1, static_cast<int>(std::ceil(u1)));
        const int loV = static_cast<int>(std::floor(v0)), hiV = std::max(loV + 1, static_cast<int>(std::ceil(v1)));
        if (!bilinear) {
            if (!repeatX && !clampOutside && (u < u0 || u >= u1)) return;
            if (!repeatY && !clampOutside && (v < v0 || v >= v1)) return;
            int i = static_cast<int>(std::floor(u)), j = static_cast<int>(std::floor(v));
            if (!axis(i, repeatX, true, lo, hi, iw) || !axis(j, repeatY, true, loV, hiV, ih)) return;
            premulTexel(i, j, r, g, b, a);
            return;
        }
        const float fu = u - 0.5f, fv = v - 0.5f;
        int i0 = static_cast<int>(std::floor(fu)), j0 = static_cast<int>(std::floor(fv));
        const float tx = fu - i0, ty = fv - j0;
        if (!repeatX && !clampOutside && (u < u0 - 0.5f || u >= u1 + 0.5f)) return;
        if (!repeatY && !clampOutside && (v < v0 - 0.5f || v >= v1 + 0.5f)) return;
        int i1 = i0 + 1, j1 = j0 + 1;
        axis(i0, repeatX, clampOutside, lo, hi, iw); axis(i1, repeatX, clampOutside, lo, hi, iw);
        axis(j0, repeatY, clampOutside, loV, hiV, ih); axis(j1, repeatY, clampOutside, loV, hiV, ih);
        float r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
        premulTexel(i0, j0, r00, g00, b00, a00); premulTexel(i1, j0, r10, g10, b10, a10);
        premulTexel(i0, j1, r01, g01, b01, a01); premulTexel(i1, j1, r11, g11, b11, a11);
        const float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
        r = w00 * r00 + w10 * r10 + w01 * r01 + w11 * r11;
        g = w00 * g00 + w10 * g10 + w01 * g01 + w11 * g11;
        b = w00 * b00 + w10 * b10 + w01 * b01 + w11 * b11;
        a = w00 * a00 + w10 * a10 + w01 * a01 + w11 * a11;
    }
    // One texel, premultiplied, in 0..255 integers.
    void texel(int i, int j, std::uint32_t& r, std::uint32_t& g, std::uint32_t& b, std::uint32_t& a) const {
        if (i < 0 || j < 0) { r = g = b = a = 0; return; }
        const std::uint8_t* p = img + (static_cast<std::size_t>(j) * iw + i) * 4;
        a = p[3];
        if (a == 255) { r = p[0]; g = p[1]; b = p[2]; return; }
        r = div255(p[0] * a); g = div255(p[1] * a); b = div255(p[2] * a);
    }
    // The common image mapping, no rotation or skew, sampled once per pixel:
    // integer bilinear (or nearest) with the row's v fixed and u stepping.
    // False when the mapping is not upright.
    bool shadeUpright(int x0, int y, int n, std::uint8_t* out) const {
        if (inv[1] != 0 || inv[2] != 0) return false;
        const int lo = static_cast<int>(std::floor(u0)), hi = std::max(lo + 1, static_cast<int>(std::ceil(u1)));
        const int loV = static_cast<int>(std::floor(v0)), hiV = std::max(loV + 1, static_cast<int>(std::ceil(v1)));
        const float v = inv[3] * (y + 0.5f) + inv[5];
        const float du = inv[0];
        float u = inv[0] * (x0 + 0.5f) + inv[4];
        const float tr = tinted ? tint.r / 255.f : 1, tg = tinted ? tint.g / 255.f : 1, tb = tinted ? tint.b / 255.f : 1;
        const auto finish = [&](std::uint8_t* o, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
            if (tinted) { r = static_cast<std::uint32_t>(r * tr); g = static_cast<std::uint32_t>(g * tg); b = static_cast<std::uint32_t>(b * tb); }
            o[0] = static_cast<std::uint8_t>(std::min(r, a)); o[1] = static_cast<std::uint8_t>(std::min(g, a));
            o[2] = static_cast<std::uint8_t>(std::min(b, a)); o[3] = static_cast<std::uint8_t>(a);
        };
        if (!bilinear) {
            int j = static_cast<int>(std::floor(v));
            axis(j, repeatY, true, loV, hiV, ih);
            for (int k = 0; k < n; ++k, u += du) {
                std::uint8_t* o = out + k * 4;
                if (!repeatX && !clampOutside && (u < u0 || u >= u1)) { std::memset(o, 0, 4); continue; }
                int i = static_cast<int>(std::floor(u));
                axis(i, repeatX, true, lo, hi, iw);
                std::uint32_t r, g, b, a;
                texel(i, j, r, g, b, a);
                finish(o, r, g, b, a);
            }
            return true;
        }
        if (!repeatY && !clampOutside && (v < v0 - 0.5f || v >= v1 + 0.5f)) { std::memset(out, 0, static_cast<std::size_t>(n) * 4); return true; }
        if (clampOutside && !repeatX && !repeatY && !tinted) {
            // The drawImage case: clamp at the image's edge, no repeat. Fixed
            // point along the row, two source rows fixed, and texels that are
            // opaque (most of a sprite) skip the premultiply.
            const float fv = v - 0.5f;
            const int jf = static_cast<int>(std::floor(fv));
            const std::uint32_t wy = static_cast<std::uint32_t>((fv - static_cast<float>(jf)) * 256 + 0.5f);
            const int j0 = std::clamp(jf, loV, hiV - 1), j1 = std::clamp(jf + 1, loV, hiV - 1);
            const std::uint8_t* r0 = img + static_cast<std::size_t>(j0) * static_cast<std::size_t>(iw) * 4;
            const std::uint8_t* r1 = img + static_cast<std::size_t>(j1) * static_cast<std::size_t>(iw) * 4;
            std::int64_t U = static_cast<std::int64_t>((u - 0.5f) * 65536.f);
            const std::int64_t dU = static_cast<std::int64_t>(du * 65536.f);
            const auto premul = [](const std::uint8_t* p, std::uint32_t c[4]) {
                const std::uint32_t a = p[3];
                c[3] = a;
                if (a == 255) { c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; return; }
                if (a == 0) { c[0] = c[1] = c[2] = 0; return; }
                c[0] = div255(p[0] * a); c[1] = div255(p[1] * a); c[2] = div255(p[2] * a);
            };
            for (int k = 0; k < n; ++k, U += dU) {
                const int i = static_cast<int>(U >> 16);
                const std::uint32_t wx = static_cast<std::uint32_t>((U >> 8) & 255);
                const int i0 = std::clamp(i, lo, hi - 1), i1 = std::clamp(i + 1, lo, hi - 1);
                const std::uint8_t *p00 = r0 + i0 * 4, *p10 = r0 + i1 * 4, *p01 = r1 + i0 * 4, *p11 = r1 + i1 * 4;
                std::uint8_t* o = out + k * 4;
                const std::uint32_t w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
                if ((p00[3] & p10[3] & p01[3] & p11[3]) == 255) {
                    // Opaque: straight is premultiplied. Two channels per
                    // 32-bit multiply, rounded, along x then y.
                    std::uint32_t q00, q10, q01, q11;
                    std::memcpy(&q00, p00, 4); std::memcpy(&q10, p10, 4); std::memcpy(&q01, p01, 4); std::memcpy(&q11, p11, 4);
                    const auto lerp = [](std::uint32_t a, std::uint32_t b, std::uint32_t t) {
                        const std::uint32_t it = 256 - t;
                        const std::uint32_t rb = ((((a & 0x00FF00FFu) * it + (b & 0x00FF00FFu) * t) + 0x00800080u) >> 8) & 0x00FF00FFu;
                        const std::uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * it + ((b >> 8) & 0x00FF00FFu) * t) + 0x00800080u) & 0xFF00FF00u;
                        return rb | ag;
                    };
                    const std::uint32_t q = lerp(lerp(q00, q10, wx), lerp(q01, q11, wx), wy);
                    std::memcpy(o, &q, 4);
                    continue;
                }
                std::uint32_t a[4], b[4], c[4], d[4];
                premul(p00, a); premul(p10, b); premul(p01, c); premul(p11, d);
                const std::uint32_t al = (a[3] * w00 + b[3] * w10 + c[3] * w01 + d[3] * w11 + 32768) >> 16;
                o[3] = static_cast<std::uint8_t>(al);
                for (int ch = 0; ch < 3; ++ch)
                    o[ch] = static_cast<std::uint8_t>(std::min(al, (a[ch] * w00 + b[ch] * w10 + c[ch] * w01 + d[ch] * w11 + 32768) >> 16));
            }
            return true;
        }
        const float fv = v - 0.5f;
        int j0 = static_cast<int>(std::floor(fv)), j1 = j0 + 1;
        const std::uint32_t wy = static_cast<std::uint32_t>((fv - std::floor(fv)) * 256 + 0.5f);
        axis(j0, repeatY, clampOutside, loV, hiV, ih);
        axis(j1, repeatY, clampOutside, loV, hiV, ih);
        for (int k = 0; k < n; ++k, u += du) {
            std::uint8_t* o = out + k * 4;
            if (!repeatX && !clampOutside && (u < u0 - 0.5f || u >= u1 + 0.5f)) { std::memset(o, 0, 4); continue; }
            const float fu = u - 0.5f;
            int i0 = static_cast<int>(std::floor(fu)), i1 = i0 + 1;
            const std::uint32_t wx = static_cast<std::uint32_t>((fu - std::floor(fu)) * 256 + 0.5f);
            axis(i0, repeatX, clampOutside, lo, hi, iw);
            axis(i1, repeatX, clampOutside, lo, hi, iw);
            std::uint32_t r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
            texel(i0, j0, r00, g00, b00, a00); texel(i1, j0, r10, g10, b10, a10);
            texel(i0, j1, r01, g01, b01, a01); texel(i1, j1, r11, g11, b11, a11);
            const std::uint32_t w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
            const auto mix = [&](std::uint32_t c00, std::uint32_t c10, std::uint32_t c01, std::uint32_t c11) {
                return (c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11 + 32768) >> 16;
            };
            finish(o, mix(r00, r10, r01, r11), mix(g00, g10, g01, g11), mix(b00, b10, b01, b11), mix(a00, a10, a01, a11));
        }
        return true;
    }

    // drawImage's common case -- upright, one tap (bilinear or nearest),
    // clamped at the image's edge, untinted -- sampled and blended in one
    // pass, with no row buffer between. Pixel for pixel the same as shade()
    // then blending.
    bool fusesUpright() const {
        return kind == Image && !exact && grid == 1 && clampOutside && !repeatX && !repeatY && !tinted && inv[1] == 0 && inv[2] == 0;
    }
    void blendUpright(Color* row, int x0, int y, int n, std::uint32_t k8) const {
        if (bilinear) blendBilinear(row, x0, y, n, k8);
        else blendNearest(row, x0, y, n, k8);
    }
    // One texel per pixel, the one under the pixel's centre: a transparent
    // texel is skipped, an opaque one at full alpha is copied, and only the
    // rest blend.
    void blendNearest(Color* row, int x0, int y, int n, std::uint32_t k8) const {
        const int lo = static_cast<int>(std::floor(u0)), hi = std::max(lo + 1, static_cast<int>(std::ceil(u1)));
        const int loV = static_cast<int>(std::floor(v0)), hiV = std::max(loV + 1, static_cast<int>(std::ceil(v1)));
        const int j = std::clamp(static_cast<int>(std::floor(inv[3] * (y + 0.5f) + inv[5])), loV, hiV - 1);
        const std::uint8_t* src = img + static_cast<std::size_t>(j) * static_cast<std::size_t>(iw) * 4;
        const float du = inv[0];
        float u = inv[0] * (x0 + 0.5f) + inv[4];
        for (int k = 0; k < n; ++k, u += du) {
            const std::uint8_t* p = src + std::clamp(static_cast<int>(std::floor(u)), lo, hi - 1) * 4;
            const std::uint32_t a = p[3];
            if (!a) continue;
            Color& d = row[x0 + k];
            if ((a & k8) == 255) { std::memcpy(&d, p, 4); continue; }
            std::uint32_t r = p[0], g = p[1], b = p[2];
            if (a != 255) { r = div255(r * a); g = div255(g * a); b = div255(b * a); }
            if (k8 == 255) blendPremulOver(d, r, g, b, a);
            else blendPremulOver(d, div255(r * k8), div255(g * k8), div255(b * k8), div255(a * k8));
        }
    }
    // Bilinear: texels are straight alpha, so a footprint of four opaque
    // texels filters as it is, two channels per multiply; four transparent
    // ones are skipped outright; only the mixed footprints along a sprite's
    // rim premultiply.
    void blendBilinear(Color* row, int x0, int y, int n, std::uint32_t k8) const {
        const int lo = static_cast<int>(std::floor(u0)), hi = std::max(lo + 1, static_cast<int>(std::ceil(u1)));
        const int loV = static_cast<int>(std::floor(v0)), hiV = std::max(loV + 1, static_cast<int>(std::ceil(v1)));
        const float v = inv[3] * (y + 0.5f) + inv[5];
        const float u = inv[0] * (x0 + 0.5f) + inv[4];
        const float fv = v - 0.5f;
        const int jf = static_cast<int>(std::floor(fv));
        const std::uint32_t wy = static_cast<std::uint32_t>((fv - static_cast<float>(jf)) * 256 + 0.5f);
        const int j0 = std::clamp(jf, loV, hiV - 1), j1 = std::clamp(jf + 1, loV, hiV - 1);
        const std::uint8_t* r0 = img + static_cast<std::size_t>(j0) * static_cast<std::size_t>(iw) * 4;
        const std::uint8_t* r1 = img + static_cast<std::size_t>(j1) * static_cast<std::size_t>(iw) * 4;
        std::int64_t U = static_cast<std::int64_t>((u - 0.5f) * 65536.f);
        const std::int64_t dU = static_cast<std::int64_t>(inv[0] * 65536.f);
        const auto lerp = [](std::uint32_t a, std::uint32_t b, std::uint32_t t) {
            const std::uint32_t it = 256 - t;
            const std::uint32_t rb = ((((a & 0x00FF00FFu) * it + (b & 0x00FF00FFu) * t) + 0x00800080u) >> 8) & 0x00FF00FFu;
            const std::uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * it + ((b >> 8) & 0x00FF00FFu) * t) + 0x00800080u) & 0xFF00FF00u;
            return rb | ag;
        };
        const auto premul = [](const std::uint8_t* p, std::uint32_t c[4]) {
            const std::uint32_t a = p[3];
            c[3] = a;
            if (a == 255) { c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; return; }
            if (a == 0) { c[0] = c[1] = c[2] = 0; return; }
            c[0] = div255(p[0] * a); c[1] = div255(p[1] * a); c[2] = div255(p[2] * a);
        };
        for (int k = 0; k < n; ++k, U += dU) {
            const int i = static_cast<int>(U >> 16);
            const std::uint32_t wx = static_cast<std::uint32_t>((U >> 8) & 255);
            const int i0 = std::clamp(i, lo, hi - 1), i1 = std::clamp(i + 1, lo, hi - 1);
            const std::uint8_t *p00 = r0 + i0 * 4, *p10 = r0 + i1 * 4, *p01 = r1 + i0 * 4, *p11 = r1 + i1 * 4;
            const std::uint32_t a00 = p00[3], a10 = p10[3], a01 = p01[3], a11 = p11[3];
            if ((a00 | a10 | a01 | a11) == 0) continue;
            Color& d = row[x0 + k];
            if ((a00 & a10 & a01 & a11) == 255) {
                std::uint32_t q00, q10, q01, q11;
                std::memcpy(&q00, p00, 4); std::memcpy(&q10, p10, 4); std::memcpy(&q01, p01, 4); std::memcpy(&q11, p11, 4);
                const std::uint32_t q = lerp(lerp(q00, q10, wx), lerp(q01, q11, wx), wy);
                if (k8 == 255) { std::memcpy(&d, &q, 4); continue; }
                blendPremulOver(d, div255((q & 255) * k8), div255(((q >> 8) & 255) * k8), div255(((q >> 16) & 255) * k8), k8);
                continue;
            }
            const std::uint32_t w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
            std::uint32_t a[4], b[4], c[4], e[4];
            premul(p00, a); premul(p10, b); premul(p01, c); premul(p11, e);
            const std::uint32_t al = (a[3] * w00 + b[3] * w10 + c[3] * w01 + e[3] * w11 + 32768) >> 16;
            std::uint32_t o[3];
            for (int ch = 0; ch < 3; ++ch) o[ch] = std::min(al, (a[ch] * w00 + b[ch] * w10 + c[ch] * w01 + e[ch] * w11 + 32768) >> 16);
            if (k8 == 255) blendPremulOver(d, o[0], o[1], o[2], al);
            else blendPremulOver(d, div255(o[0] * k8), div255(o[1] * k8), div255(o[2] * k8), div255(al * k8));
        }
    }

    // `n` premultiplied pixels starting at device (x0, y).
    void shade(int x0, int y, int n, std::uint8_t* out) const {
        if (kind == Solid) {
            std::uint8_t p[4];
            premultiply(color, p);
            for (int i = 0; i < n; ++i) std::memcpy(out + i * 4, p, 4);
            return;
        }
        if (kind == Gradient) {
            const float* g = grad->g;
            const float cy = y + 0.5f;
            if (grad->kind == 0) {
                const float dx = g[2] - g[0], dy = g[3] - g[1], len2 = dx * dx + dy * dy;
                const float ax = (inv[0] * dx + inv[1] * dy) / len2;
                const float bx = ((inv[2] * cy + inv[4] - g[0]) * dx + (inv[3] * cy + inv[5] - g[1]) * dy) / len2;
                float t = ax * (x0 + 0.5f) + bx;
                for (int i = 0; i < n; ++i, t += ax) {
                    const int idx = static_cast<int>(std::clamp(t, 0.f, 1.f) * (kGradientLut - 1) + 0.5f);
                    std::memcpy(out + i * 4, grad->lut[static_cast<std::size_t>(idx)].data(), 4);
                }
                return;
            }
            for (int i = 0; i < n; ++i) {
                const float cx = x0 + i + 0.5f;
                const float px = inv[0] * cx + inv[2] * cy + inv[4], py = inv[1] * cx + inv[3] * cy + inv[5];
                float t;
                if (grad->kind == 2) {
                    float a = std::atan2(py - g[2], px - g[1]) - g[0];
                    t = a / kTau;
                    t -= std::floor(t);
                } else {
                    // Two-circle radial: the largest w with the point on the
                    // circle (c(w), r(w)) and r(w) >= 0.
                    const float cdx = g[3] - g[0], cdy = g[4] - g[1], dr = g[5] - g[2];
                    const float pdx = px - g[0], pdy = py - g[1];
                    const float A = cdx * cdx + cdy * cdy - dr * dr;
                    const float B = pdx * cdx + pdy * cdy + g[2] * dr;
                    const float C = pdx * pdx + pdy * pdy - g[2] * g[2];
                    float w = -1;
                    bool ok = false;
                    if (std::abs(A) < 1e-6f) {
                        if (std::abs(B) > 1e-9f) { w = C / (2 * B); ok = g[2] + w * dr >= 0; }
                    } else {
                        const float disc = B * B - A * C;
                        if (disc >= 0) {
                            const float sq = std::sqrt(disc);
                            const float w1 = (B + sq) / A, w2 = (B - sq) / A;
                            const float hiW = std::max(w1, w2), loW = std::min(w1, w2);
                            if (g[2] + hiW * dr >= 0) { w = hiW; ok = true; }
                            else if (g[2] + loW * dr >= 0) { w = loW; ok = true; }
                        }
                    }
                    if (!ok) { std::memset(out + i * 4, 0, 4); continue; }
                    t = w;
                }
                const int idx = static_cast<int>(std::clamp(t, 0.f, 1.f) * (kGradientLut - 1) + 0.5f);
                std::memcpy(out + i * 4, grad->lut[static_cast<std::size_t>(idx)].data(), 4);
            }
            return;
        }
        // Image.
        if (exact) {
            const int j = y + offY;
            for (int i = 0; i < n; ++i) {
                const int ti = x0 + i + offX;
                std::uint8_t* o = out + i * 4;
                if (ti < 0 || ti >= iw || j < 0 || j >= ih) { std::memset(o, 0, 4); continue; }
                const std::uint8_t* p = img + (static_cast<std::size_t>(j) * iw + ti) * 4;
                const std::uint32_t a = p[3];
                if (a == 255) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255; }
                else { o[0] = static_cast<std::uint8_t>(div255(p[0] * a)); o[1] = static_cast<std::uint8_t>(div255(p[1] * a)); o[2] = static_cast<std::uint8_t>(div255(p[2] * a)); o[3] = static_cast<std::uint8_t>(a); }
                if (tinted) { o[0] = static_cast<std::uint8_t>(o[0] * tint.r / 255); o[1] = static_cast<std::uint8_t>(o[1] * tint.g / 255); o[2] = static_cast<std::uint8_t>(o[2] * tint.b / 255); }
            }
            return;
        }
        if (grid == 1 && shadeUpright(x0, y, n, out)) return;
        const float slice = 1.f / grid;
        const float inv2 = 1.f / (grid * grid);
        for (int i = 0; i < n; ++i) {
            float sr = 0, sg = 0, sb = 0, sa = 0;
            for (int gy = 0; gy < grid; ++gy) for (int gx = 0; gx < grid; ++gx) {
                const float px = x0 + i + (gx + 0.5f) * slice, py = y + (gy + 0.5f) * slice;
                const float u = inv[0] * px + inv[2] * py + inv[4], v = inv[1] * px + inv[3] * py + inv[5];
                float r, g, b, a;
                sampleImage(u, v, r, g, b, a);
                sr += r; sg += g; sb += b; sa += a;
            }
            std::uint8_t* o = out + i * 4;
            float a = sa * inv2, r = sr * inv2, g = sg * inv2, b = sb * inv2;
            if (tinted) { r *= tint.r / 255.f; g *= tint.g / 255.f; b *= tint.b / 255.f; }
            o[3] = static_cast<std::uint8_t>(std::min(255.f, a + 0.5f));
            o[0] = static_cast<std::uint8_t>(std::min(static_cast<float>(o[3]), r + 0.5f));
            o[1] = static_cast<std::uint8_t>(std::min(static_cast<float>(o[3]), g + 0.5f));
            o[2] = static_cast<std::uint8_t>(std::min(static_cast<float>(o[3]), b + 0.5f));
        }
    }
};

inline std::uint8_t* scratchRow(int n) {
    static thread_local std::vector<std::uint8_t> buf;
    if (buf.size() < static_cast<std::size_t>(n) * 4) buf.resize(static_cast<std::size_t>(n) * 4 + 64);
    return buf.data();
}

// The colour that picks a text ramp: the solid colour, or mid-grey for a
// gradient or pattern (whose colour varies across the run).
inline Color textColour(const PaintStyle& p) { return p.kind == PaintStyle::Solid ? p.color : Color{128, 128, 128}; }

// Source-over straight onto the canvas: the fast path for plain compositing.
struct Painter {
    Color* px = nullptr;
    int stride = 0;
    const ClipMask* clip = nullptr;
    float alpha = 1;
    const Shader* shader = nullptr;
    const std::uint8_t* gammaLut = nullptr;  // text coverage ramp, or linear

    void spanSolid(Color* row, int x0, int x1, int y, std::uint32_t sa) const {
        const Color c = shader->color;
        if (!clip || clip->rect) { blendRunOver(row + x0, x1 - x0, c, sa); return; }
        const RowView cr = clip->row(y);
        const int fx0 = clip->fullX0[static_cast<std::size_t>(y - clip->y0)], fx1 = clip->fullX1[static_cast<std::size_t>(y - clip->y0)];
        int a = x0;
        const int leftEnd = std::min(x1, std::max(x0, fx0));
        for (; a < leftEnd; ++a) if (cr[a]) blendOver(row[a], c, div255(sa * cr[a]));
        const int midEnd = std::min(x1, std::max(a, fx1));
        if (midEnd > a) { blendRunOver(row + a, midEnd - a, c, sa); a = midEnd; }
        for (; a < x1; ++a) if (cr[a]) blendOver(row[a], c, div255(sa * cr[a]));
    }
    void run(int x0, int x1, int y, float cov) const {
        if (gammaLut) cov = gammaLut[static_cast<int>(cov * 255 + 0.5f)] * (1.f / 255.f);
        if (clip) {
            if (y < clip->y0 || y >= clip->y1) return;
            x0 = std::max(x0, clip->x0); x1 = std::min(x1, clip->x1);
        }
        if (x0 >= x1) return;
        Color* row = px + static_cast<std::size_t>(y) * stride;
        const float k = cov * alpha;
        if (shader->kind == Shader::Solid) {
            if (alpha < 1.f && cov >= 1.f && (!clip || clip->rect)) {
                // A fractional global alpha over a whole run: kept at 16 bits.
                blendRunOver16(row + x0, x1 - x0, shader->color, static_cast<std::uint32_t>(k * shader->color.a * 257.f + 0.5f));
                return;
            }
            const std::uint32_t sa = static_cast<std::uint32_t>(k * shader->color.a + 0.5f);
            if (sa) spanSolid(row, x0, x1, y, sa);
            return;
        }
        const int n = x1 - x0;
        const std::uint32_t k8 = static_cast<std::uint32_t>(k * 255 + 0.5f);
        const RowView cr = (clip && !clip->rect) ? clip->row(y) : RowView{};
        if (shader->kind == Shader::Image && shader->exact && !shader->tinted) {
            // A texel to a pixel: the source is straight alpha like the
            // canvas, so it blends as a colour at its own alpha, no buffer.
            const int j = y + shader->offY;
            if (j < 0 || j >= shader->ih) return;
            const int a = std::max(x0, -shader->offX), b = std::min(x1, shader->iw - shader->offX);
            const std::uint8_t* src = shader->img + (static_cast<std::size_t>(j) * shader->iw + (a + shader->offX)) * 4;
            if (k8 == 255 && !cr) {
                // Opaque texels are the destination as they are: copied a
                // run at a time. Transparent ones are skipped.
                int x = a;
                while (x < b) {
                    if (src[3] == 255) {
                        int e = x + 1;
                        const std::uint8_t* q = src + 4;
                        while (e < b && q[3] == 255) { ++e; q += 4; }
                        std::memcpy(row + x, src, static_cast<std::size_t>(e - x) * 4);
                        src = q; x = e;
                        continue;
                    }
                    if (src[3]) blendOver(row[x], Color{src[0], src[1], src[2], 255}, src[3]);
                    ++x; src += 4;
                }
                return;
            }
            for (int x = a; x < b; ++x, src += 4) {
                std::uint32_t sa = src[3];
                if (!sa) continue;
                // Fractional alpha: 16 bits, so a stack of translucent
                // layers does not drift (see blendOver16).
                float a16 = sa * k * 257.f;
                if (cr) a16 = a16 * cr[x] * (1.f / 255.f);
                blendOver16(row[x], Color{src[0], src[1], src[2], 255}, static_cast<std::uint32_t>(a16 + 0.5f));
            }
            return;
        }
        if (!cr && shader->fusesUpright()) {
            if (k8) shader->blendUpright(row, x0, y, n, k8);
            return;
        }
        std::uint8_t* buf = scratchRow(n);
        shader->shade(x0, y, n, buf);
        for (int i = 0; i < n; ++i) {
            const std::uint32_t c = cr ? div255(k8 * cr[x0 + i]) : k8;
            if (!c) continue;
            const std::uint8_t* p = buf + i * 4;
            if (c == 255) blendPremulOver(row[x0 + i], p[0], p[1], p[2], p[3]);
            else blendPremulOver(row[x0 + i], div255(p[0] * c), div255(p[1] * c), div255(p[2] * c), div255(p[3] * c));
        }
    }
    // 8-bit coverage for a row of pixels (glyph masks).
    void mask(int y, int x0, const std::uint8_t* cov, int n) const {
        int x1 = x0 + n;
        int skip = 0;
        if (clip) {
            if (y < clip->y0 || y >= clip->y1) return;
            if (x0 < clip->x0) { skip = clip->x0 - x0; x0 = clip->x0; }
            x1 = std::min(x1, clip->x1);
        }
        if (x0 < 0) { skip += -x0; x0 = 0; }
        x1 = std::min(x1, stride);
        if (x0 >= x1) return;
        cov += skip;
        Color* row = px + static_cast<std::size_t>(y) * stride;
        const RowView cr = (clip && !clip->rect) ? clip->row(y) : RowView{};
        const std::uint32_t a8 = static_cast<std::uint32_t>(alpha * 255 + 0.5f);
        if (shader->kind == Shader::Solid) {
            const std::uint32_t full = div255(a8 * shader->color.a);
            for (int x = x0; x < x1; ++x) {
                std::uint32_t c = cov[x - x0];
                if (!c) continue;
                if (gammaLut) c = gammaLut[c];
                std::uint32_t sa = div255(full * c);
                if (cr) sa = div255(sa * cr[x]);
                blendOver(row[x], shader->color, sa);
            }
            return;
        }
        std::uint8_t* buf = scratchRow(x1 - x0);
        shader->shade(x0, y, x1 - x0, buf);
        for (int x = x0; x < x1; ++x) {
            std::uint32_t c = div255(a8 * (gammaLut ? gammaLut[cov[x - x0]] : cov[x - x0]));
            if (cr) c = div255(c * cr[x]);
            if (!c) continue;
            const std::uint8_t* p = buf + (x - x0) * 4;
            blendPremulOver(row[x], div255(p[0] * c), div255(p[1] * c), div255(p[2] * c), div255(p[3] * c));
        }
    }
};

// Into a premultiplied layer (source-over within it), for the general model.
struct LayerPainter {
    std::uint8_t* L = nullptr;
    int lx = 0, ly = 0, lw = 0, lh = 0;
    const Shader* shader = nullptr;
    const std::uint8_t* gammaLut = nullptr;
    void put(std::uint8_t* d, const std::uint8_t* s, std::uint32_t c) const {
        const std::uint32_t sa = div255(s[3] * c);
        if (!sa) return;
        const std::uint32_t keep = 255 - sa;
        d[0] = static_cast<std::uint8_t>(std::min(255u, div255(s[0] * c) + div255(d[0] * keep)));
        d[1] = static_cast<std::uint8_t>(std::min(255u, div255(s[1] * c) + div255(d[1] * keep)));
        d[2] = static_cast<std::uint8_t>(std::min(255u, div255(s[2] * c) + div255(d[2] * keep)));
        d[3] = static_cast<std::uint8_t>(std::min(255u, sa + div255(d[3] * keep)));
    }
    void run(int x0, int x1, int y, float cov) const {
        if (gammaLut) cov = gammaLut[static_cast<int>(cov * 255 + 0.5f)] * (1.f / 255.f);
        if (y < ly || y >= ly + lh) return;
        x0 = std::max(x0, lx); x1 = std::min(x1, lx + lw);
        if (x0 >= x1) return;
        const std::uint32_t c = static_cast<std::uint32_t>(cov * 255 + 0.5f);
        if (!c) return;
        std::uint8_t* row = L + (static_cast<std::size_t>(y - ly) * static_cast<std::size_t>(lw) + static_cast<std::size_t>(x0 - lx)) * 4;
        std::uint8_t* buf = scratchRow(x1 - x0);
        shader->shade(x0, y, x1 - x0, buf);
        for (int x = x0; x < x1; ++x) put(row + (x - x0) * 4, buf + (x - x0) * 4, c);
    }
    void mask(int y, int x0, const std::uint8_t* cov, int n) const {
        if (y < ly || y >= ly + lh) return;
        int a = std::max(x0, lx), b = std::min(x0 + n, lx + lw);
        if (a >= b) return;
        std::uint8_t* row = L + (static_cast<std::size_t>(y - ly) * static_cast<std::size_t>(lw) + static_cast<std::size_t>(a - lx)) * 4;
        std::uint8_t* buf = scratchRow(b - a);
        shader->shade(a, y, b - a, buf);
        for (int x = a; x < b; ++x) if (cov[x - x0]) put(row + (x - a) * 4, buf + (x - a) * 4, gammaLut ? gammaLut[cov[x - x0]] : cov[x - x0]);
    }
};

// Coverage of an axis-aligned device rect [X0,X1)x[Y0,Y1), exactly, as runs
// -- the fillRect/clearRect/drawImage fast path, which never builds a path.
template <class Sink>
void produceRect(float X0, float Y0, float X1, float Y1, const IBox& limit, const Sink& sink, int costPerPixel = 1) {
    if (!(X1 > X0 && Y1 > Y0)) return;
    const IBox b = IBox{static_cast<int>(std::floor(std::max(X0, -1e8f))), static_cast<int>(std::floor(std::max(Y0, -1e8f))),
                        static_cast<int>(std::ceil(std::min(X1, 1e8f))), static_cast<int>(std::ceil(std::min(Y1, 1e8f)))}.intersect(limit);
    if (b.empty()) return;
    const auto cover = [](float lo, float hi, int p) { return std::max(0.f, std::min(hi, p + 1.f) - std::max(lo, static_cast<float>(p))); };
    // Interior columns [ix0, ix1) are fully covered horizontally.
    const int ix0 = std::max(b.x0, static_cast<int>(std::ceil(X0))), ix1 = std::min(b.x1, static_cast<int>(std::floor(X1)));
    const auto row = [&](int y) {
        const float cy = cover(Y0, Y1, y);
        if (cy <= 0) return;
        if (ix0 >= ix1) {  // narrower than a pixel's interior
            for (int x = b.x0; x < b.x1; ++x) { const float c = cover(X0, X1, x) * cy; if (c > 0) sink.run(x, x + 1, y, c); }
            return;
        }
        for (int x = b.x0; x < ix0; ++x) { const float c = cover(X0, X1, x) * cy; if (c > 0) sink.run(x, x + 1, y, c); }
        sink.run(ix0, ix1, y, cy);
        for (int x = ix1; x < b.x1; ++x) { const float c = cover(X0, X1, x) * cy; if (c > 0) sink.run(x, x + 1, y, c); }
    };
    parallelRows(b.y0, b.y1, b.area(), [&](int ya, int yb, int) { for (int y = ya; y < yb; ++y) row(y); }, costPerPixel);
}

// Is `p` one axis-aligned rectangle (in device space)?

// Margins (left, top, right, bottom) by which a filter list spreads ink.
inline void filterMargins(const std::vector<CssFilterFunction>* f, int& l, int& t, int& r, int& b) {
    l = t = r = b = 0;
    if (!f) return;
    for (const CssFilterFunction& op : *f) {
        if (op.kind == CssFilterFunction::Kind::Blur) { const int e = blurExtent(op.amount); l += e; t += e; r += e; b += e; }
        else if (op.kind == CssFilterFunction::Kind::DropShadow) {
            const int e = blurExtent(op.amount / 2);
            l += e + static_cast<int>(std::ceil(std::max(0.f, -op.dx))); r += e + static_cast<int>(std::ceil(std::max(0.f, op.dx)));
            t += e + static_cast<int>(std::ceil(std::max(0.f, -op.dy))); b += e + static_cast<int>(std::ceil(std::max(0.f, op.dy)));
        }
    }
}

// A copy of a premultiplied image's alpha, blurred, tinted and shifted by a
// (possibly fractional) offset: a shadow. `out` covers the same box as the
// input (callers pad the box beforehand).
inline void makeShadow(const std::uint8_t* src, int w, int h, Color color, float sigma, float dx, float dy, std::vector<std::uint8_t>& out,
                       std::vector<std::uint32_t>& scratch) {
    static thread_local std::vector<std::uint8_t> alpha;
    alpha.assign(static_cast<std::size_t>(w) * h, 0);
    for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = src[i * 4 + 3];
    if (sigma > 0) gaussianBlurAlpha(alpha.data(), w, h, w, sigma, sigma, scratch);
    out.assign(static_cast<std::size_t>(w) * h * 4, 0);
    std::uint8_t pc[4];
    premultiply(color, pc);
    const int ix = static_cast<int>(std::floor(dx)), iy = static_cast<int>(std::floor(dy));
    const float fx = dx - ix, fy = dy - iy;
    const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    const auto A = [&](int x, int y) -> float { return (x < 0 || y < 0 || x >= w || y >= h) ? 0.f : alpha[static_cast<std::size_t>(y) * w + x]; };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int sx = x - ix, sy = y - iy;
            const float a = w00 * A(sx, sy) + w10 * A(sx - 1, sy) + w01 * A(sx, sy - 1) + w11 * A(sx - 1, sy - 1);
            if (a <= 0.5f) continue;
            const std::uint32_t k = static_cast<std::uint32_t>(a + 0.5f);
            std::uint8_t* o = out.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            o[0] = static_cast<std::uint8_t>(div255(pc[0] * k)); o[1] = static_cast<std::uint8_t>(div255(pc[1] * k));
            o[2] = static_cast<std::uint8_t>(div255(pc[2] * k)); o[3] = static_cast<std::uint8_t>(div255(pc[3] * k));
        }
    }
}

inline void applyFilterChain(std::uint8_t* L, int w, int h, const std::vector<CssFilterFunction>& chain, std::vector<std::uint32_t>& scratch) {
    using K = CssFilterFunction::Kind;
    const std::size_t count = static_cast<std::size_t>(w) * h;
    for (const CssFilterFunction& f : chain) {
        switch (f.kind) {
        case K::Blur: gaussianBlurPremul(L, w, h, w, f.amount, f.amount, scratch); break;
        case K::Brightness: applyColorFilter(L, count, ColorFilterKind::Brightness, f.amount); break;
        case K::Contrast: applyColorFilter(L, count, ColorFilterKind::Contrast, f.amount); break;
        case K::Grayscale: applyColorFilter(L, count, ColorFilterKind::Grayscale, f.amount); break;
        case K::HueRotate: applyColorFilter(L, count, ColorFilterKind::HueRotate, f.amount); break;
        case K::Invert: applyColorFilter(L, count, ColorFilterKind::Invert, f.amount); break;
        case K::Opacity: applyColorFilter(L, count, ColorFilterKind::Opacity, f.amount); break;
        case K::Saturate: applyColorFilter(L, count, ColorFilterKind::Saturate, f.amount); break;
        case K::Sepia: applyColorFilter(L, count, ColorFilterKind::Sepia, f.amount); break;
        case K::DropShadow: {
            static thread_local std::vector<std::uint8_t> shadow;
            makeShadow(L, w, h, f.color, f.amount / 2, f.dx, f.dy, shadow, scratch);
            // The image over its own shadow.
            for (std::size_t i = 0; i < count; ++i) {
                std::uint8_t* d = L + i * 4;
                const std::uint8_t* s = shadow.data() + i * 4;
                const std::uint32_t keep = 255 - d[3];
                for (int c = 0; c < 4; ++c) d[c] = static_cast<std::uint8_t>(std::min(255u, d[c] + div255(s[c] * keep)));
            }
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// The display list.
// ---------------------------------------------------------------------------
enum class OpKind : std::uint8_t { FillPath, StrokePath, FillRect, StrokeRect, ClearRect, Text, Image, PutImage, Desaturate, Blit, Multiply };

struct Op {
    OpKind kind = OpKind::FillPath;
    bool evenOdd = false, glyph = false, stroke = false, culled = false, tinted = false;
    bool fromCanvas = false;  // an Image op whose canvas source was copied (detach)
    std::uint32_t style = 0;
    Matrix m{1, 0, 0, 1, 0, 0};
    float f[10]{};
    std::uint32_t a = 0, b = 0;
    Canvas::Soft* src = nullptr;
    const std::uint8_t* raw = nullptr;  // caller pixels, only for immediate execution
    int rw = 0, rh = 0;
    Color tint{255, 255, 255, 255};
    IBox box;
};

inline CanvasFrameStats& nativeStats() { static CanvasFrameStats s; return s; }
inline int* nativeOpCounts() { static int counts[kCanvasOpCodes] = {}; return counts; }

// The rows this thread may write while a flush draws in bands (everything,
// otherwise). Every draw's target comes from Soft::clipBox, which applies it.
struct BandWindow { int y0 = -(1 << 30), y1 = 1 << 30; };
inline BandWindow& bandWindow() { static thread_local BandWindow b; return b; }
struct BandScope {
    BandWindow saved;
    BandScope(int y0, int y1) : saved(bandWindow()) { bandWindow() = BandWindow{y0, y1}; }
    ~BandScope() { bandWindow() = saved; }
    BandScope(const BandScope&) = delete;
    BandScope& operator=(const BandScope&) = delete;
};
// Upright filled text small enough is drawn from cached glyph masks;
// anything else is drawn from outlines, which read the font's own cache.
inline bool textUsesMasks(const Op& op) {
    const Matrix& m = op.m;
    const bool upright = m[1] == 0 && m[2] == 0 && m[0] > 0 && std::abs(m[0] - m[3]) < 1e-6f * m[0];
    const float devSize = op.f[3] * m[0];
    return !op.stroke && upright && op.f[2] == 1.f && devSize <= 192.f && devSize >= 1.f;
}

struct GpuSurface;  // detail/gpu.h
inline std::uint64_t nextSoftId() { static std::atomic<std::uint64_t> next{1}; return next++; }

}  // namespace canvas_detail

// ---------------------------------------------------------------------------
// Canvas::Soft
// ---------------------------------------------------------------------------
struct Canvas::Soft {
    using Style = canvas_detail::Style;
    using State = canvas_detail::State;
    using Op = canvas_detail::Op;
    using OpKind = canvas_detail::OpKind;
    using IBox = canvas_detail::IBox;
    using Matrix = canvas_detail::Matrix;
    using Shader = canvas_detail::Shader;

    int w, h;
    std::vector<Color> px;
    State st;
    std::vector<State> stack;
    std::uint64_t nextVersion = 2;
    bool deferring = true;

    std::vector<Op> ops;
    std::vector<Style> styles;
    std::vector<Path2D::Segment> segs;
    std::vector<canvas_detail::GlyphPos> glyphs;
    std::vector<std::uint8_t> bytes;
    std::vector<std::shared_ptr<const canvas_detail::ImageCopy>> images;
    struct Snap { std::uint64_t version = 0; std::uint32_t fv = 0, sv = 0, index = 0; };
    std::array<Snap, 8> snaps{};
    int snapNext = 0;
    std::vector<Soft*> readers, sources;
    bool flushing = false;
    struct LayerFrame { std::vector<Color> parent; Style captured; };
    std::vector<LayerFrame> layers;
    std::vector<std::uint64_t> occlusion;

    // Drawn with the GPU (detail/gpu.h) when set. Then the pixels live in
    // its texture, and `px` is a copy kept only as far as it is needed:
    // cpuValid says px holds them, gpuValid that the texture does.
    std::unique_ptr<canvas_detail::GpuSurface> gpu;
    bool cpuValid = true, gpuValid = false;
    // Changes whenever the pixels may have; with `id`, what a GPU canvas
    // drawing this one knows its uploaded copy by.
    std::uint64_t contentVersion = 0;
    const std::uint64_t id = canvas_detail::nextSoftId();

    Soft(int width, int height, Color init) : w(width), h(height), px(static_cast<std::size_t>(width) * height, init) {}
    ~Soft();
    // px holds the pixels (read back from the GPU if they live there).
    void syncCpu();
    // px is about to be written outside the GPU: read back first, and the
    // texture is stale after.
    void cpuWrite() { syncCpu(); if (gpu) gpuValid = false; ++contentVersion; }
    // px is about to be overwritten whole: nothing to read back.
    void cpuOverwrite();
    void executeGpu();
    bool gpuCan(const Op& op, const Style& s) const;
    bool gpuDo(const Op& op, const Style& s);

    // ----- state -----
    void touch() { st.version = nextVersion++; }
    IBox canvasBox() const { return {0, 0, w, h}; }
    IBox clipBox(const Style& s) const {
        IBox b = canvasBox();
        if (s.clip) b = b.intersect({s.clip->x0, s.clip->y0, s.clip->x1, s.clip->y1});
        const canvas_detail::BandWindow& band = canvas_detail::bandWindow();
        b.y0 = std::max(b.y0, band.y0); b.y1 = std::min(b.y1, band.y1);
        return b;
    }

    // ----- dependencies between canvases -----
    void dropReader(Soft* r) { readers.erase(std::remove(readers.begin(), readers.end(), r), readers.end()); }
    void flushReaders() {
        // Whoever has recorded a draw FROM this canvas reads it as it is now:
        // from a copy of these pixels when that is cheap, else by flushing.
        std::vector<Soft*> rs;
        rs.swap(readers);
        for (Soft* r : rs) if (r != this && !r->detach(this)) r->flush();
    }
    // Repoints this canvas's pending draws from `src` at a copy of src's
    // pixels as they are now. A scratch canvas redrawn for every sprite
    // (florr's glitched mobs) otherwise flushes the whole frame each time.
    static constexpr std::size_t kMaxDetachPixels = 1u << 20;
    static constexpr std::size_t kMaxDetachedBytes = 64u << 20;
    std::size_t detachedBytes = 0;
    bool detach(Soft* src) {
        const std::size_t pixels = static_cast<std::size_t>(src->w) * src->h;
        if (flushing || pixels > kMaxDetachPixels || detachedBytes + pixels * 4 > kMaxDetachedBytes) return false;
        std::uint32_t index = 0;
        bool copied = false;
        for (Op& op : ops) {
            if (op.src != src) continue;
            if (!copied) {
                src->syncCpu();
                auto copy = std::make_shared<canvas_detail::ImageCopy>();
                copy->w = src->w; copy->h = src->h;
                const auto* p = reinterpret_cast<const std::uint8_t*>(src->px.data());
                copy->rgba.assign(p, p + pixels * 4);
                index = static_cast<std::uint32_t>(images.size());
                images.push_back(std::move(copy));
                detachedBytes += pixels * 4;
                copied = true;
            }
            op.src = nullptr; op.fromCanvas = true; op.a = index;
        }
        sources.erase(std::remove(sources.begin(), sources.end(), src), sources.end());
        return true;
    }
    void dependOn(Soft* src) {
        if (src == this) { flush(); return; }
        src->flush();
        src->syncCpu();
        if (std::find(src->readers.begin(), src->readers.end(), this) == src->readers.end()) src->readers.push_back(this);
        if (std::find(sources.begin(), sources.end(), src) == sources.end()) sources.push_back(src);
    }
    // Before pixels change outside a flush (immediate writes): draws recorded
    // from this canvas read it as it was.
    void beforeImmediateWrite() { flushReaders(); flush(); cpuWrite(); }

    // ----- recording -----
    std::uint32_t snapshot() {
        const std::uint32_t fv = st.style.fill.dataVersion(), sv = st.style.stroke.dataVersion();
        for (const Snap& s : snaps) if (s.version == st.version && s.fv == fv && s.sv == sv && s.index < styles.size()) return s.index;
        styles.push_back(st.style);
        styles.back().fill.freeze();
        styles.back().stroke.freeze();
        Snap& slot = snaps[static_cast<std::size_t>(snapNext++ & 7)];
        slot = {st.version, fv, sv, static_cast<std::uint32_t>(styles.size() - 1)};
        return slot.index;
    }
    void submit(Op& op) {
        canvas_detail::nativeStats().ops++;
        if (op.box.empty()) { canvas_detail::nativeStats().culled++; resetArenasIfIdle(); return; }
        if (deferring && layers.empty()) {
            op.style = snapshot();
            op.raw = nullptr;
            ops.push_back(op);
            // A bound on how far the recording may run ahead of the pixels.
            if (ops.size() >= 65536 || bytes.size() > (64u << 20) || segs.size() > (4u << 20)) flush();
            return;
        }
        flushReaders();
        cpuWrite();
        execute(op, st.style);
        resetArenasIfIdle();
    }
    void resetArenasIfIdle() {
        if (!ops.empty()) return;
        segs.clear(); glyphs.clear(); bytes.clear(); images.clear(); detachedBytes = 0;
    }
    // The device box an op may change: its geometry's, spread by shadow and
    // filter, the whole clip for the unbounded composite operations, always
    // within the clip and the canvas.
    IBox opBox(const canvas_detail::Box& geometry, const Style& s) const {
        using namespace canvas_detail;
        IBox b = toIBox(geometry);
        if (b.empty()) return {};
        int l, t, r, bt;
        filterMargins(s.filter.get(), l, t, r, bt);
        b = b.outset(l, t, r, bt);
        if (s.shadowVisible()) {
            const int e = blurExtent(s.shadowBlur / 2) + 1;
            IBox sh = b;
            sh.x0 += static_cast<int>(std::floor(s.shadowX)) - e; sh.x1 += static_cast<int>(std::ceil(s.shadowX)) + e;
            sh.y0 += static_cast<int>(std::floor(s.shadowY)) - e; sh.y1 += static_cast<int>(std::ceil(s.shadowY)) + e;
            b = {std::min(b.x0, sh.x0), std::min(b.y0, sh.y0), std::max(b.x1, sh.x1), std::max(b.y1, sh.y1)};
        }
        if (compositeIsUnbounded(s.composite)) return clipBox(s);
        return b.intersect(clipBox(s));
    }

    // ----- flushing and culling -----
    void flush() {
        if (flushing) return;
        // Nothing pending: the pixels stay as they are, so whatever reads
        // them has no reason to flush -- which is what lets a frame draw the
        // same sprite canvas hundreds of times as one batch.
        if (ops.empty()) { resetArenasIfIdle(); dropSources(); return; }
        flushReaders();
        flushing = true;
        const auto started = std::chrono::steady_clock::now();
        cull();
        if (gpu) executeGpu(); else executeOps();
        ++contentVersion;
        ops.clear(); styles.clear();
        for (Snap& s : snaps) s = Snap{};
        resetArenasIfIdle();
        dropSources();
        canvas_detail::nativeStats().flushMillis += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        canvas_detail::nativeStats().batches++;
        flushing = false;
    }
    void dropSources() {
        for (Soft* s : sources) s->dropReader(this);
        sources.clear();
    }
    void discardPending() {
        canvas_detail::nativeStats().culled += static_cast<int>(ops.size());
        ops.clear(); styles.clear();
        for (Snap& s : snaps) s = Snap{};
        resetArenasIfIdle();
        dropSources();
    }

    // Occlusion: one bit per pixel, set where a LATER op is known to replace
    // the pixel whatever it held. Walking the list backwards, an op whose
    // whole box is already set is invisible and dropped.
    bool covered(const IBox& b) const {
        // Inside the biggest rectangle known to be covered: no walk.
        if (b.x0 >= solid.x0 && b.y0 >= solid.y0 && b.x1 <= solid.x1 && b.y1 <= solid.y1) return true;
        const int words = (w + 63) / 64;
        for (int y = b.y0; y < b.y1; ++y) {
            const std::uint64_t* row = occlusion.data() + static_cast<std::size_t>(y) * words;
            int x = b.x0;
            while (x < b.x1) {
                const int wi = x >> 6, bit = x & 63;
                const int span = std::min(64 - bit, b.x1 - x);
                const std::uint64_t want = (span == 64 ? ~0ull : ((1ull << span) - 1)) << bit;
                if ((row[wi] & want) != want) return false;
                x += span;
            }
        }
        return true;
    }
    void occludeRow(int y, int x0, int x1) {
        x0 = std::max(x0, 0); x1 = std::min(x1, w);
        if (y < 0 || y >= h || x0 >= x1) return;
        const int words = (w + 63) / 64;
        std::uint64_t* row = occlusion.data() + static_cast<std::size_t>(y) * words;
        int x = x0;
        while (x < x1) {
            const int wi = x >> 6, bit = x & 63;
            const int span = std::min(64 - bit, x1 - x);
            row[wi] |= (span == 64 ? ~0ull : ((1ull << span) - 1)) << bit;
            x += span;
        }
    }
    // Marks the pixels of integer box `r` that the style's clip leaves fully
    // open.
    void occlude(IBox r, const Style& s) {
        r = r.intersect(canvasBox());
        if (r.empty()) return;
        const canvas_detail::ClipMask* c = s.clip.get();
        if (!c && r.area() > solid.area()) solid = r;
        for (int y = r.y0; y < r.y1; ++y) {
            if (!c) { occludeRow(y, r.x0, r.x1); continue; }
            if (y < c->y0 || y >= c->y1) continue;
            const std::size_t k = static_cast<std::size_t>(y - c->y0);
            occludeRow(y, std::max(r.x0, c->fullX0[k]), std::min(r.x1, c->fullX1[k]));
        }
    }
    static IBox innerBox(float X0, float Y0, float X1, float Y1) {
        if (!(X1 > X0 && Y1 > Y0)) return {};
        const auto c = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
        return {c(std::ceil(X0)), c(std::ceil(Y0)), c(std::floor(X1)), c(std::floor(Y1))};
    }
    static bool deviceRect(const Matrix& m, float x, float y, float rw, float rh, float& X0, float& Y0, float& X1, float& Y1) {
        if (!canvas_detail::isAxisAligned(m)) return false;
        const float ax = m[0] * x + m[4], bx = m[0] * (x + rw) + m[4], ay = m[3] * y + m[5], by = m[3] * (y + rh) + m[5];
        X0 = std::min(ax, bx); X1 = std::max(ax, bx); Y0 = std::min(ay, by); Y1 = std::max(ay, by);
        return std::isfinite(X0) && std::isfinite(X1) && std::isfinite(Y0) && std::isfinite(Y1);
    }
    // What part of the canvas `op` overwrites regardless of what was there.
    void addOccluder(const Op& op, const Style& s) {
        using namespace canvas_detail;
        const bool plainOpaque = s.alpha >= 1.f && s.composite == CompositeOp::SourceOver && !s.filter;
        float X0, Y0, X1, Y1;
        switch (op.kind) {
        case OpKind::ClearRect:
            if (deviceRect(op.m, op.f[0], op.f[1], op.f[2], op.f[3], X0, Y0, X1, Y1)) {
                const auto snap = [](float v) { return std::floor(std::clamp(v, -1e8f, 1e8f) + 0.5f); };
                occlude(innerBox(snap(X0), snap(Y0), snap(X1), snap(Y1)), s);
            }
            return;
        case OpKind::PutImage:
            occlude({static_cast<int>(op.f[0]), static_cast<int>(op.f[1]), static_cast<int>(op.f[0] + op.f[2]), static_cast<int>(op.f[1] + op.f[3])}, Style{});
            return;
        case OpKind::FillRect:
            if (plainOpaque && s.fill.opaque() && deviceRect(op.m, op.f[0], op.f[1], op.f[2], op.f[3], X0, Y0, X1, Y1))
                occlude(innerBox(X0, Y0, X1, Y1), s);
            return;
        case OpKind::FillPath: {
            if (!plainOpaque || !s.fill.opaque() || op.b != 1 || !isAxisAligned(op.m)) return;
            const Path2D::Segment& g = segs[op.a];
            if (g.command == Path2D::Command::Rect) {
                if (deviceRect(op.m, g.v[0], g.v[1], g.v[2], g.v[3], X0, Y0, X1, Y1)) occlude(innerBox(X0, Y0, X1, Y1), s);
            } else if (g.command == Path2D::Command::RoundRect) {
                // The rounded rect is the union of two crossed rects.
                const float x = std::min(g.v[0], g.v[0] + g.v[2]), y = std::min(g.v[1], g.v[1] + g.v[3]);
                const float rw = std::abs(g.v[2]), rh = std::abs(g.v[3]), r = std::clamp(g.v[4], 0.f, std::min(rw, rh) / 2);
                if (deviceRect(op.m, x + r, y, rw - 2 * r, rh, X0, Y0, X1, Y1)) occlude(innerBox(X0, Y0, X1, Y1), s);
                if (deviceRect(op.m, x, y + r, rw, rh - 2 * r, X0, Y0, X1, Y1)) occlude(innerBox(X0, Y0, X1, Y1), s);
            } else if (g.command == Path2D::Command::Arc && std::abs(g.v[4] - g.v[3]) >= kTau - 1e-4f) {
                // A whole disc holds its inscribed square.
                const float q = g.v[2] * 0.70710678f;
                if (deviceRect(op.m, g.v[0] - q, g.v[1] - q, 2 * q, 2 * q, X0, Y0, X1, Y1)) occlude(innerBox(X0, Y0, X1, Y1), s);
            }
            return;
        }
        case OpKind::Image: {
            if (!plainOpaque || op.f[8] < 1.f || !isAxisAligned(op.m)) return;
            bool opaque = false;
            int iw = 0, ih = 0;
            if (op.src || op.fromCanvas) { opaque = false; }
            else if (op.a < images.size() && images[op.a]) { opaque = images[op.a]->opaque; iw = images[op.a]->w; ih = images[op.a]->h; }
            if (!opaque) return;
            // Only when every destination pixel samples inside the image.
            if (op.f[0] < 0 || op.f[1] < 0 || op.f[0] + op.f[2] > iw || op.f[1] + op.f[3] > ih) return;
            if (deviceRect(op.m, op.f[4], op.f[5], op.f[6], op.f[7], X0, Y0, X1, Y1)) occlude(innerBox(X0, Y0, X1, Y1), s);
            return;
        }
        default: return;
        }
    }
    IBox solid;  // the largest rectangle the occlusion bitmap has wholly set
    void cull() {
        if (!cullingEnabled || ops.size() < 2) return;
        const int words = (w + 63) / 64;
        bool any = false;
        solid = IBox{};
        for (std::size_t i = ops.size(); i-- > 0;) {
            Op& op = ops[i];
            if (any && covered(op.box)) { op.culled = true; canvas_detail::nativeStats().culled++; continue; }
            const std::size_t before = any ? 1 : 0;
            (void)before;
            if (!any) {
                // The bitmap is only allocated once something can occlude.
                const OpKind k = op.kind;
                if (k == OpKind::FillRect || k == OpKind::FillPath || k == OpKind::ClearRect || k == OpKind::PutImage || k == OpKind::Image) {
                    occlusion.assign(static_cast<std::size_t>(words) * h, 0);
                    any = true;
                }
            }
            if (any) addOccluder(op, styles[op.style]);
        }
    }
    bool cullingEnabled = true;

    // ----- execution -----
    // A run of ops is drawn in horizontal bands, one thread per band at a
    // time, each band taking every op that reaches it in order. Rows are
    // independent for an op that blends straight onto the canvas, so this
    // is pixel for pixel the sequential result, and it is what keeps every
    // core busy on a frame of many small draws, where splitting each draw
    // on its own costs more in hand-offs than it saves. An op that cannot
    // be cut into rows -- one drawn through a layer (filter, shadow,
    // composite operation), one reading this canvas, text drawn from
    // outlines -- runs alone, between runs. So does a long path that spans
    // many bands: every band it reaches would flatten (and stroke) all of
    // it again, where alone it is built once and only its rows are split.
    bool bandSafe(const Op& op, const Style& s) const {
        if (!s.plain()) return false;
        switch (op.kind) {
        case OpKind::Image: return op.src != this;
        case OpKind::Text: return canvas_detail::textUsesMasks(op);
        case OpKind::FillPath: case OpKind::StrokePath: case OpKind::Desaturate: {
            const int bandRows = std::max(kMinFlushBandRows, h / (2 * canvas_detail::BandPool::instance().maxBands()));
            const long long extraBands = (op.box.y1 - op.box.y0) / bandRows;
            return static_cast<long long>(op.b) * extraBands <= kMaxRepeatedSegments;
        }
        default: return true;
        }
    }
    static constexpr long long kMaxRepeatedSegments = 1024;
    void executeOps() {
        using namespace canvas_detail;
        BandPool& pool = BandPool::instance();
        const std::size_t n = ops.size();
        std::size_t i = 0;
        while (i < n) {
            if (ops[i].culled) { ++i; continue; }
            if (pool.maxBands() < 2 || !bandSafe(ops[i], styles[ops[i].style])) { execute(ops[i], styles[ops[i].style]); ++i; continue; }
            std::size_t j = i;
            long long work = 0;
            int count = 0, y0 = h, y1 = 0;
            for (; j < n; ++j) {
                const Op& op = ops[j];
                if (op.culled) continue;
                if (!bandSafe(op, styles[op.style])) break;
                work += op.box.area();
                ++count;
                y0 = std::min(y0, op.box.y0); y1 = std::max(y1, op.box.y1);
            }
            if (count >= 2 && work >= kMinThreadedArea && y1 - y0 >= 2 * kMinFlushBandRows) executeBanded(i, j, y0, y1);
            else for (std::size_t k = i; k < j; ++k) if (!ops[k].culled) execute(ops[k], styles[ops[k].style]);
            i = j;
        }
    }
    static constexpr int kMinFlushBandRows = 16;
    void executeBanded(std::size_t first, std::size_t last, int y0, int y1) {
        using namespace canvas_detail;
        BandPool& pool = BandPool::instance();
        const int rows = y1 - y0;
        const int bands = std::max(1, std::min(pool.maxBands() * 2, rows / kMinFlushBandRows));
        GlyphCache& glyphs = GlyphCache::instance();
        glyphs.pin();
        pool.run(bands, [&](int band) {
            const int by0 = y0 + static_cast<int>(static_cast<long long>(rows) * band / bands);
            const int by1 = y0 + static_cast<int>(static_cast<long long>(rows) * (band + 1) / bands);
            const BandScope scope(by0, by1);
            for (std::size_t k = first; k < last; ++k) {
                const Op& op = ops[k];
                if (op.culled || op.box.y1 <= by0 || op.box.y0 >= by1) continue;
                execute(op, styles[op.style]);
            }
        });
        glyphs.unpin();
    }
    void execute(const Op& op, const Style& s);
    void makeShader(const canvas_detail::PaintStyle& p, const Matrix& m, const Style& s, Shader& out,
                    std::shared_ptr<const canvas_detail::GradientImpl>& keep) const;
    template <class Produce>
    void paint(const Style& s, const Shader& shader, const IBox& shapeBox, const std::uint8_t* gammaLut, Produce&& produce);
    void compositeLayer(const Style& s, const std::uint8_t* L, const IBox& lb, float alpha, bool unbounded);
    void fillPoly(const Style& s, const canvas_detail::PaintStyle& p, const Matrix& m, const canvas_detail::Poly& poly, bool evenOdd, bool glyph);
    void execText(const Op& op, const Style& s);
    void execImage(const Op& op, const Style& s);
    void execClearRect(const Op& op, const Style& s);
    void execDesaturate(const Op& op, const Style& s);

    // ----- clips -----
    void clip(const Path2D::Segment* g, std::size_t n, bool evenOdd);
};

inline void Canvas::Soft::makeShader(const canvas_detail::PaintStyle& p, const Matrix& m, const Style& s, Shader& out,
                                     std::shared_ptr<const canvas_detail::GradientImpl>& keep) const {
    using namespace canvas_detail;
    out = Shader{};
    switch (p.kind) {
    case PaintStyle::Solid:
        out.kind = Shader::Solid;
        out.color = p.color;
        return;
    case PaintStyle::Gradient: {
        keep = p.gradientImpl();
        out.kind = Shader::Gradient;
        out.grad = keep.get();
        if (!invert(m, out.inv)) out.grad = nullptr;
        return;
    }
    case PaintStyle::Pattern: {
        out.kind = Shader::Image;
        const CanvasPattern::Data& d = *p.pattern;
        out.img = d.rgba ? d.rgba->data() : nullptr;
        out.iw = d.width; out.ih = d.height;
        out.repeatX = d.repeatX; out.repeatY = d.repeatY;
        out.u0 = 0; out.v0 = 0; out.u1 = static_cast<float>(d.width); out.v1 = static_cast<float>(d.height);
        const std::array<float, 6> pt = p.patternTransform();
        const Matrix full = multiply(m, Matrix{pt[0], pt[1], pt[2], pt[3], pt[4], pt[5]});
        if (!invert(full, out.inv)) out.img = nullptr;
        out.bilinear = s.smoothing;
        // "low" (the default) is one bilinear tap, as Chrome's is; "medium"
        // and "high" average a grid of taps when the image is minified.
        const float shrink = std::max(std::hypot(out.inv[0], out.inv[1]), std::hypot(out.inv[2], out.inv[3]));
        out.grid = s.smoothing && s.smoothingQuality > 0 ? std::clamp(static_cast<int>(std::ceil(shrink - 0.01f)), 1, 4) : 1;
        return;
    }
    }
}

// The one place every draw lands. Plain compositing goes straight onto the
// canvas; anything else is rendered into a layer first and run through the
// drawing model: filter, shadow, then the composite operation.
template <class Produce>
void Canvas::Soft::paint(const Style& s, const Shader& shader, const IBox& shapeBox, const std::uint8_t* gammaLut, Produce&& produce) {
    using namespace canvas_detail;
    if (!(s.alpha > 0) && s.composite == CompositeOp::SourceOver) return;
    const IBox target = clipBox(s);
    if (target.empty()) return;
    if (s.plain()) {
        Painter p;
        p.px = px.data(); p.stride = w; p.clip = s.clip.get(); p.alpha = s.alpha; p.shader = &shader; p.gammaLut = gammaLut;
        produce(p, target);
        return;
    }
    // The layer: the shape's box, padded by what the filter spreads, kept to
    // what can still reach the target through filter and shadow.
    int fl, ft, fr, fb;
    filterMargins(s.filter.get(), fl, ft, fr, fb);
    int reach = 0;
    if (s.shadowVisible()) reach = blurExtent(s.shadowBlur / 2) + static_cast<int>(std::ceil(std::max(std::abs(s.shadowX), std::abs(s.shadowY)))) + 1;
    IBox lb = shapeBox.outset(fl, ft, fr, fb).intersect(target.outset(fl + fr + reach, ft + fb + reach, fl + fr + reach, ft + fb + reach));
    const bool unbounded = compositeIsUnbounded(s.composite);
    if (lb.empty()) {
        if (unbounded) compositeLayer(s, nullptr, IBox{}, s.alpha, true);
        return;
    }
    static thread_local std::vector<std::uint8_t> layer;
    static thread_local std::vector<std::uint32_t> scratch;
    const int lw = lb.x1 - lb.x0, lh = lb.y1 - lb.y0;
    layer.assign(static_cast<std::size_t>(lw) * lh * 4, 0);
    LayerPainter lp;
    lp.L = layer.data(); lp.lx = lb.x0; lp.ly = lb.y0; lp.lw = lw; lp.lh = lh; lp.shader = &shader; lp.gammaLut = gammaLut;
    produce(lp, lb);
    if (s.filter) applyFilterChain(layer.data(), lw, lh, *s.filter, scratch);
    if (s.shadowVisible()) {
        // The shadow gets its own padded box so its blur is not cut short.
        const int e = blurExtent(s.shadowBlur / 2) + 1;
        const int ox = static_cast<int>(std::ceil(std::abs(s.shadowX))), oy = static_cast<int>(std::ceil(std::abs(s.shadowY)));
        const IBox sb = lb.outset(e + ox, e + oy, e + ox, e + oy);
        const int sw = sb.x1 - sb.x0, sh = sb.y1 - sb.y0;
        static thread_local std::vector<std::uint8_t> padded, shadow;
        padded.assign(static_cast<std::size_t>(sw) * sh * 4, 0);
        for (int y = 0; y < lh; ++y)
            std::memcpy(padded.data() + (static_cast<std::size_t>(y + (lb.y0 - sb.y0)) * sw + (lb.x0 - sb.x0)) * 4,
                        layer.data() + static_cast<std::size_t>(y) * lw * 4, static_cast<std::size_t>(lw) * 4);
        makeShadow(padded.data(), sw, sh, s.shadowColor, s.shadowBlur / 2, s.shadowX, s.shadowY, shadow, scratch);
        compositeLayer(s, shadow.data(), sb, s.alpha, unbounded);
    }
    compositeLayer(s, layer.data(), lb, s.alpha, unbounded);
}

// Composites a premultiplied layer covering `lb` onto the canvas with the
// style's operation, within its clip, at `alpha`. Unbounded operations treat
// everything outside the layer (within the clip) as transparent source.
inline void Canvas::Soft::compositeLayer(const Style& s, const std::uint8_t* L, const IBox& lb, float alpha, bool unbounded) {
    using namespace canvas_detail;
    const IBox target = clipBox(s);
    const IBox region = unbounded ? target : lb.intersect(target);
    if (region.empty()) return;
    const std::uint32_t a8 = static_cast<std::uint32_t>(std::clamp(alpha, 0.f, 1.f) * 255 + 0.5f);
    const ClipMask* c = s.clip.get();
    const int lw = lb.x1 - lb.x0;
    parallelRows(region.y0, region.y1, region.area(), [&](int ya, int yb, int) {
        std::vector<std::uint8_t> src(static_cast<std::size_t>(region.x1 - region.x0) * 4);
        for (int y = ya; y < yb; ++y) {
            const int n = region.x1 - region.x0;
            std::fill(src.begin(), src.end(), 0);
            if (L && y >= lb.y0 && y < lb.y1) {
                const int a = std::max(region.x0, lb.x0), b = std::min(region.x1, lb.x1);
                const std::uint8_t* lr = L + (static_cast<std::size_t>(y - lb.y0) * lw + (a - lb.x0)) * 4;
                std::uint8_t* d = src.data() + static_cast<std::size_t>(a - region.x0) * 4;
                if (a8 == 255) std::memcpy(d, lr, static_cast<std::size_t>(std::max(0, b - a)) * 4);
                else for (int i = 0; i < (b - a) * 4; ++i) d[i] = static_cast<std::uint8_t>(div255(lr[i] * a8));
            }
            const std::uint8_t* clipRow = (c && !c->rect) ? c->row(y).at(region.x0) : nullptr;
            compositeSpan(s.composite, px.data() + static_cast<std::size_t>(y) * w + region.x0, src.data(), clipRow, n);
        }
    });
}

inline void Canvas::Soft::fillPoly(const Style& s, const canvas_detail::PaintStyle& p, const Matrix& m, const canvas_detail::Poly& poly,
                                   bool evenOdd, bool glyph) {
    using namespace canvas_detail;
    if (p.paintsNothing() && s.composite == CompositeOp::SourceOver) return;
    Shader shader;
    std::shared_ptr<const GradientImpl> keep;
    makeShader(p, m, s, shader, keep);
    if (shader.kind == Shader::Gradient && !shader.grad) return;
    if (shader.kind == Shader::Image && !shader.img) return;
    int bx0, by0, bx1, by1;
    if (!polyBounds(poly, -(1 << 28), -(1 << 28), 1 << 28, 1 << 28, bx0, by0, bx1, by1)) {
        if (compositeIsUnbounded(s.composite)) paint(s, shader, IBox{}, nullptr, [](auto&, const IBox&) {});
        return;
    }
    const IBox shapeBox{bx0, by0, bx1, by1};
    float X0, Y0, X1, Y1;
    const bool rect = !evenOdd && polyIsRect(poly, X0, Y0, X1, Y1);
    paint(s, shader, shapeBox, glyph ? textGammaLut(textColour(p)) : nullptr, [&](auto& sink, const IBox& limit) {
        if (rect) produceRect(X0, Y0, X1, Y1, limit, sink);
        else scanFill(poly, evenOdd, limit.x0, limit.y0, limit.x1, limit.y1,
                      [&](int x0, int x1, int y, float cov) { sink.run(x0, x1, y, cov); });
    });
}

// ----- clips -----
inline void Canvas::Soft::clip(const Path2D::Segment* g, std::size_t n, bool evenOdd) {
    using namespace canvas_detail;
    static thread_local Poly poly;
    flatten(g, n, st.matrix, matrixMaxScale(st.matrix), poly);
    const ClipMask* outer = st.style.clip.get();
    IBox bounds = clipBox(st.style);
    auto mask = std::make_shared<ClipMask>();
    int x0, y0, x1, y1;
    if (bounds.empty() || !polyBounds(poly, bounds.x0, bounds.y0, bounds.x1, bounds.y1, x0, y0, x1, y1)) {
        mask->x0 = mask->y0 = mask->x1 = mask->y1 = 0;
        mask->rect = true;
        mask->computeRuns();
        st.style.clip = mask;
        touch();
        return;
    }
    float X0, Y0, X1, Y1;
    const bool rect = !evenOdd && polyIsRect(poly, X0, Y0, X1, Y1);
    const auto integral = [](float v) { return std::abs(v - std::round(v)) < 1e-4f; };
    if (rect && integral(X0) && integral(Y0) && integral(X1) && integral(Y1) && (!outer || outer->rect)) {
        // The common clip: a pixel-aligned rectangle needs no mask at all.
        const IBox r = IBox{static_cast<int>(std::lround(X0)), static_cast<int>(std::lround(Y0)),
                            static_cast<int>(std::lround(X1)), static_cast<int>(std::lround(Y1))}.intersect(bounds);
        mask->x0 = r.x0; mask->y0 = r.y0; mask->x1 = std::max(r.x0, r.x1); mask->y1 = std::max(r.y0, r.y1);
        mask->rect = true;
    } else {
        mask->x0 = x0; mask->y0 = y0; mask->x1 = x1; mask->y1 = y1;
        mask->rect = false;
        mask->alpha.assign(static_cast<std::size_t>(x1 - x0) * (y1 - y0), 0);
        std::uint8_t* cells = mask->alpha.data();
        const int stride = x1 - x0;
        const auto write = [&](int a, int b, int y, float cov) {
            std::uint8_t* row = cells + static_cast<std::size_t>(y - y0) * static_cast<std::size_t>(stride);
            const std::uint32_t v = static_cast<std::uint32_t>(cov * 255 + 0.5f);
            if (!outer) { std::memset(row + (a - x0), static_cast<int>(v), static_cast<std::size_t>(b - a)); return; }
            for (int x = a; x < b; ++x) row[x - x0] = static_cast<std::uint8_t>(div255(v * outer->at(x, y)));
        };
        struct Sink { decltype(write)& f; void run(int a, int b, int y, float c) const { f(a, b, y, c); } } sink{write};
        if (rect) produceRect(X0, Y0, X1, Y1, IBox{x0, y0, x1, y1}, sink);
        else scanFill(poly, evenOdd, x0, y0, x1, y1, [&](int a, int b, int y, float c) { write(a, b, y, c); });
    }
    mask->computeRuns();
    st.style.clip = mask;
    touch();
}

inline void Canvas::Soft::execClearRect(const Op& op, const Style& s) {
    using namespace canvas_detail;
    // Pixels lose `coverage` of their alpha (destination-out), within the clip;
    // the transform applies, nothing else does.
    const ClipMask* c = s.clip.get();
    Color* const surface = px.data();
    const int stride = w;
    struct Sink {
        Color* surface; int stride; const ClipMask* c;
        void run(int x0, int x1, int y, float cov) const {
            if (c) { if (y < c->y0 || y >= c->y1) return; x0 = std::max(x0, c->x0); x1 = std::min(x1, c->x1); }
            if (x0 >= x1) return;
            Color* row = surface + static_cast<std::size_t>(y) * stride;
            const std::uint32_t k = static_cast<std::uint32_t>(cov * 255 + 0.5f);
            if (k >= 255 && (!c || c->rect)) { std::fill(row + x0, row + x1, Color{0, 0, 0, 0}); return; }
            for (int x = x0; x < x1; ++x) {
                const std::uint32_t kk = c ? div255(k * c->at(x, y)) : k;
                Color& p = row[x];
                p.a = static_cast<std::uint8_t>(div255(p.a * (255 - kk)));
                if (p.a == 0) p = Color{0, 0, 0, 0};
            }
        }
    } sink{surface, stride, c};
    // Not antialiased, as Chrome's is not: an edge rounds to the nearest
    // pixel boundary (half up), and a pixel is cleared or it is not. Only the
    // clip's own antialiasing shows.
    float X0, Y0, X1, Y1;
    const IBox limit = clipBox(s);
    if (deviceRect(op.m, op.f[0], op.f[1], op.f[2], op.f[3], X0, Y0, X1, Y1)) {
        const auto snap = [](float v) { return std::floor(std::clamp(v, -1e8f, 1e8f) + 0.5f); };
        produceRect(snap(X0), snap(Y0), snap(X1), snap(Y1), limit, sink);
        return;
    }
    Path2D box;
    box.rect(op.f[0], op.f[1], op.f[2], op.f[3]);
    static thread_local Poly poly;
    flatten(box, op.m, matrixMaxScale(op.m), poly);
    scanFill(poly, false, limit.x0, limit.y0, limit.x1, limit.y1, [&](int a, int b, int y, float cov) { if (cov >= 0.5f) sink.run(a, b, y, 1.f); });
}

inline void Canvas::Soft::execDesaturate(const Op& op, const Style& s) {
    using namespace canvas_detail;
    static thread_local Poly poly;
    flatten(segs.data() + op.a, op.b, op.m, matrixMaxScale(op.m), poly);
    const IBox limit = clipBox(s);
    const ClipMask* c = s.clip.get();
    const float alpha = s.alpha;
    scanFill(poly, op.evenOdd, limit.x0, limit.y0, limit.x1, limit.y1, [&](int x0, int x1, int y, float cov) {
        Color* row = px.data() + static_cast<std::size_t>(y) * w;
        for (int x = x0; x < x1; ++x) {
            float a = cov * alpha;
            if (c) a *= c->at(x, y) / 255.f;
            if (a <= 0.0005f) continue;
            Color& d = row[x];
            const auto lum = static_cast<std::uint8_t>(std::clamp(std::lround(0.3f * d.r + 0.59f * d.g + 0.11f * d.b), 0L, 255L));
            const float t = std::min(a, 1.f), keep = 1.f - t;
            d = Color{static_cast<std::uint8_t>(std::lround(lum * t + d.r * keep)), static_cast<std::uint8_t>(std::lround(lum * t + d.g * keep)),
                      static_cast<std::uint8_t>(std::lround(lum * t + d.b * keep)), d.a};
        }
    });
}

inline void Canvas::Soft::execText(const Op& op, const Style& s) {
    using namespace canvas_detail;
    // f: [0] origin x (aligned), [1] baseline y, [2] squeeze, [3] size; a,b: glyph range.
    const PaintStyle& p = op.stroke ? s.stroke : s.fill;
    const GlyphPos* gl = glyphs.data() + op.a;
    const std::size_t n = op.b;
    const float ox = op.f[0], oy = op.f[1], squeeze = op.f[2], size = op.f[3];
    const Matrix& m = op.m;
    const float devSize = size * m[0];
    if (textUsesMasks(op)) {
        Shader shader;
        std::shared_ptr<const GradientImpl> keep;
        makeShader(p, m, s, shader, keep);
        if ((shader.kind == Shader::Gradient && !shader.grad) || (shader.kind == Shader::Image && !shader.img)) return;
        GlyphCache& cache = GlyphCache::instance();
        const float baseY = m[3] * oy + m[5];
        const int iy = static_cast<int>(std::lround(baseY));
        // Bounds of the run's masks.
        IBox shape{1 << 30, 1 << 30, -(1 << 30), -(1 << 30)};
        struct Placed { const GlyphMask* mask; int x, y; };
        static thread_local std::vector<Placed> placed;
        placed.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const float dx = m[0] * (ox + gl[i].x) + m[4];
            const int ix = static_cast<int>(std::floor(dx));
            const int phase = std::min(3, static_cast<int>((dx - ix) * 4));
            const GlyphMask& gm = cache.get(gl[i].face, gl[i].glyph, devSize, phase);
            if (gm.cov.empty()) continue;
            placed.push_back({&gm, ix + gm.left, iy + gm.top});
            shape.x0 = std::min(shape.x0, ix + gm.left); shape.y0 = std::min(shape.y0, iy + gm.top);
            shape.x1 = std::max(shape.x1, ix + gm.left + gm.w); shape.y1 = std::max(shape.y1, iy + gm.top + gm.h);
        }
        if (placed.empty()) { if (compositeIsUnbounded(s.composite)) paint(s, shader, IBox{}, nullptr, [](auto&, const IBox&) {}); return; }
        paint(s, shader, shape, textGammaLut(textColour(p)), [&](auto& sink, const IBox& limit) {
            for (const Placed& pl : placed) {
                const GlyphMask& gm = *pl.mask;
                const int r0 = std::max(0, limit.y0 - pl.y), r1 = std::min(gm.h, limit.y1 - pl.y);
                for (int r = r0; r < r1; ++r) sink.mask(pl.y + r, pl.x, gm.cov.data() + static_cast<std::size_t>(r) * gm.w, gm.w);
            }
        });
        return;
    }
    // Outlines: rotated, skewed, squeezed, huge, or stroked text.
    Path2D path;
    for (std::size_t i = 0; i < n; ++i) gl[i].face->appendGlyph(path, gl[i].glyph, ox + gl[i].x, oy, size);
    if (squeeze != 1.f) {
        for (Path2D::Segment& g : path.segments()) {
            const int pairs = g.command == Path2D::Command::Move || g.command == Path2D::Command::Line ? 1
                            : g.command == Path2D::Command::Quadratic ? 2 : g.command == Path2D::Command::Bezier ? 3 : 0;
            for (int k = 0; k < pairs; ++k) g.v[k * 2] = ox + (g.v[k * 2] - ox) * squeeze;
        }
    }
    static thread_local Poly flat, dashed, outline, device;
    if (op.stroke) {
        StrokeParams sp;
        sp.width = s.lineWidth; sp.miterLimit = s.miterLimit; sp.cap = s.lineCap; sp.join = s.lineJoin; sp.dash = &s.dash; sp.dashOffset = s.dashOffset;
        strokeToDevice(path.segments().data(), path.segments().size(), m, sp, flat, dashed, outline, device);
        fillPoly(s, p, m, device, false, false);
        return;
    }
    flatten(path, m, matrixMaxScale(m), device);
    fillPoly(s, p, m, device, false, true);
}

inline void Canvas::Soft::execImage(const Op& op, const Style& s) {
    using namespace canvas_detail;
    // f: [0..3] source rect, [4..7] destination rect, [8] extra alpha.
    const std::uint8_t* img = nullptr;
    int iw = 0, ih = 0;
    std::vector<std::uint8_t> selfCopy;
    if (op.src) {
        const Soft* src = op.src;
        iw = src->w; ih = src->h;
        img = reinterpret_cast<const std::uint8_t*>(src->px.data());
        if (src == this) {  // drawing a canvas onto itself reads a snapshot
            selfCopy.assign(img, img + static_cast<std::size_t>(iw) * ih * 4);
            img = selfCopy.data();
        }
    } else if (op.raw) { img = op.raw; iw = op.rw; ih = op.rh; }
    else if (op.a < images.size() && images[op.a]) { img = images[op.a]->rgba.data(); iw = images[op.a]->w; ih = images[op.a]->h; }
    if (!img || iw <= 0 || ih <= 0) return;
    float sx = op.f[0], sy = op.f[1], sw = op.f[2], sh = op.f[3];
    const float dx = op.f[4], dy = op.f[5], dw = op.f[6], dh = op.f[7];
    if (!(sw > 0 && sh > 0 && dw != 0 && dh != 0)) return;
    // Image pixels -> user space -> device.
    const Matrix toUser{dw / sw, 0, 0, dh / sh, dx - sx * dw / sw, dy - sy * dh / sh};
    const Matrix full = multiply(op.m, toUser);
    Shader shader;
    shader.kind = Shader::Image;
    shader.img = img; shader.iw = iw; shader.ih = ih;
    shader.clampOutside = true;
    // Filtering may reach past the source rectangle into the rest of the
    // image (the spec requires it), and clamps at the image's own edge.
    shader.u0 = 0; shader.v0 = 0; shader.u1 = static_cast<float>(iw); shader.v1 = static_cast<float>(ih);
    shader.tinted = op.tinted; shader.tint = op.tint;
    if (!invert(full, shader.inv)) return;
    const float shrink = std::max(std::hypot(shader.inv[0], shader.inv[1]), std::hypot(shader.inv[2], shader.inv[3]));
    // A canvas (a sprite) drawn scaled takes the texel under each pixel's
    // centre, unfiltered, as florr.io_clone's copy does -- whatever
    // imageSmoothingEnabled says. Image data and patterns still filter.
    shader.bilinear = s.smoothing && !op.src && !op.fromCanvas;
    shader.grid = shader.bilinear && s.smoothingQuality > 0 ? std::clamp(static_cast<int>(std::ceil(shrink - 0.01f)), 1, 4) : 1;
    // One texel to one pixel on whole-pixel positions: a copy, no filtering.
    const auto integral = [](float v) { return std::abs(v - std::round(v)) < 1e-4f; };
    if (full[1] == 0 && full[2] == 0 && std::abs(full[0] - 1) < 1e-6f && std::abs(full[3] - 1) < 1e-6f && integral(full[4]) && integral(full[5])) {
        shader.exact = true;
        shader.offX = -static_cast<int>(std::lround(full[4]));
        shader.offY = -static_cast<int>(std::lround(full[5]));
    } else if (!s.smoothing && full[1] == 0 && full[2] == 0) {
        shader.grid = 1;
    }
    Style styled = s;
    styled.alpha = s.alpha * std::clamp(op.f[8], 0.f, 1.f);
    float X0, Y0, X1, Y1;
    if (deviceRect(op.m, dx, dy, dw, dh, X0, Y0, X1, Y1)) {
        const IBox shape = toIBox(Box{X0, Y0, X1, Y1});
        // A filtered image costs several times a fill per pixel.
        const int cost = shader.exact ? 1 : shader.bilinear ? 4 : 2;
        paint(styled, shader, shape, nullptr, [&](auto& sink, const IBox& limit) { produceRect(X0, Y0, X1, Y1, limit, sink, cost); });
        return;
    }
    static thread_local Poly poly;
    Path2D quad;
    quad.rect(dx, dy, dw, dh);
    flatten(quad, op.m, 1.f, poly);
    int bx0, by0, bx1, by1;
    if (!polyBounds(poly, -(1 << 28), -(1 << 28), 1 << 28, 1 << 28, bx0, by0, bx1, by1)) return;
    paint(styled, shader, IBox{bx0, by0, bx1, by1}, nullptr, [&](auto& sink, const IBox& limit) {
        scanFill(poly, false, limit.x0, limit.y0, limit.x1, limit.y1, [&](int a, int b, int y, float cov) { sink.run(a, b, y, cov); });
    });
}

inline void Canvas::Soft::execute(const Op& op, const Style& s) {
    using namespace canvas_detail;
    static thread_local Poly flat, dashed, outline, device;
    switch (op.kind) {
    case OpKind::FillPath:
        flatten(segs.data() + op.a, op.b, op.m, matrixMaxScale(op.m), device);
        fillPoly(s, s.fill, op.m, device, op.evenOdd, op.glyph);
        return;
    case OpKind::FillRect: {
        if (s.fill.paintsNothing() && s.composite == CompositeOp::SourceOver) return;
        float X0, Y0, X1, Y1;
        if (deviceRect(op.m, op.f[0], op.f[1], op.f[2], op.f[3], X0, Y0, X1, Y1)) {
            Shader shader;
            std::shared_ptr<const GradientImpl> keep;
            makeShader(s.fill, op.m, s, shader, keep);
            if ((shader.kind == Shader::Gradient && !shader.grad) || (shader.kind == Shader::Image && !shader.img)) return;
            paint(s, shader, toIBox(Box{X0, Y0, X1, Y1}), nullptr, [&](auto& sink, const IBox& limit) { produceRect(X0, Y0, X1, Y1, limit, sink); });
            return;
        }
        Path2D box;
        box.rect(op.f[0], op.f[1], op.f[2], op.f[3]);
        flatten(box, op.m, matrixMaxScale(op.m), device);
        fillPoly(s, s.fill, op.m, device, false, false);
        return;
    }
    case OpKind::StrokePath:
    case OpKind::StrokeRect: {
        if (!(s.lineWidth > 0)) return;
        StrokeParams sp;
        sp.width = s.lineWidth; sp.miterLimit = s.miterLimit; sp.cap = s.lineCap; sp.join = s.lineJoin; sp.dash = &s.dash; sp.dashOffset = s.dashOffset;
        if (op.kind == OpKind::StrokeRect) {
            Path2D box;
            box.rect(op.f[0], op.f[1], op.f[2], op.f[3]);
            strokeToDevice(box.segments().data(), box.segments().size(), op.m, sp, flat, dashed, outline, device);
        } else {
            strokeToDevice(segs.data() + op.a, op.b, op.m, sp, flat, dashed, outline, device);
        }
        fillPoly(s, s.stroke, op.m, device, false, false);
        return;
    }
    case OpKind::ClearRect: execClearRect(op, s); return;
    case OpKind::Text: execText(op, s); return;
    case OpKind::Image: execImage(op, s); return;
    case OpKind::Desaturate: execDesaturate(op, s); return;
    case OpKind::Blit: {
        // blitDevice: device pixels blended source-over at globalAlpha through
        // the clip, ignoring transform, composite, filter and shadow.
        // f: the device position of the source's top-left; bytes at a (or
        // raw), row stride b pixels; op.box is what it may touch.
        const IBox lim = op.box.intersect(clipBox(s));
        if (lim.empty()) return;
        const int ox = static_cast<int>(op.f[0]), oy = static_cast<int>(op.f[1]);
        const std::uint8_t* srcBytes = op.raw ? op.raw : bytes.data() + op.a;
        const ClipMask* c = s.clip.get();
        const std::uint32_t a8 = static_cast<std::uint32_t>(s.alpha * 255 + 0.5f);
        for (int y = lim.y0; y < lim.y1; ++y) {
            const std::uint8_t* src = srcBytes + (static_cast<std::size_t>(y - oy) * op.b + (lim.x0 - ox)) * 4;
            Color* dst = px.data() + static_cast<std::size_t>(y) * w + lim.x0;
            for (int x = lim.x0; x < lim.x1; ++x, src += 4, ++dst) {
                if (src[3] == 0) continue;
                std::uint32_t sa = div255(src[3] * a8);
                if (c) sa = div255(sa * c->at(x, y));
                blendOver(*dst, Color{src[0], src[1], src[2], 255}, sa);
            }
        }
        return;
    }
    case OpKind::Multiply: {
        // multiplyDevice: each colour channel times 1 - mask * alpha * (1 -
        // floor), through the clip; alpha untouched. f: the device position of
        // the mask's top-left; bytes at a (or raw), row stride b; tint: floor.
        const IBox lim = op.box.intersect(clipBox(s));
        if (lim.empty()) return;
        const int ox = static_cast<int>(op.f[0]), oy = static_cast<int>(op.f[1]);
        const std::uint8_t* mask = op.raw ? op.raw : bytes.data() + op.a;
        const ClipMask* c = s.clip.get();
        std::array<std::array<std::uint16_t, 256>, 3> keep{};
        const std::uint8_t floors[3] = {op.tint.r, op.tint.g, op.tint.b};
        for (int ch = 0; ch < 3; ++ch)
            for (int m = 0; m < 256; ++m) keep[ch][m] = static_cast<std::uint16_t>(255 - std::lround(m * s.alpha * (255 - floors[ch]) / 255.f));
        for (int y = lim.y0; y < lim.y1; ++y) {
            const std::uint8_t* src = mask + static_cast<std::size_t>(y - oy) * op.b + (lim.x0 - ox);
            Color* dst = px.data() + static_cast<std::size_t>(y) * w + lim.x0;
            for (int x = lim.x0; x < lim.x1;) {
                if (x + 8 <= lim.x1) {  // a vignette is empty over most of the screen
                    std::uint64_t word;
                    std::memcpy(&word, src, 8);
                    if (word == 0) { x += 8; src += 8; dst += 8; continue; }
                }
                std::uint32_t m = *src;
                if (m && c) m = div255(m * c->at(x, y));
                if (m) {
                    dst->r = static_cast<std::uint8_t>(div255(dst->r * keep[0][m]));
                    dst->g = static_cast<std::uint8_t>(div255(dst->g * keep[1][m]));
                    dst->b = static_cast<std::uint8_t>(div255(dst->b * keep[2][m]));
                }
                ++x; ++src; ++dst;
            }
        }
        return;
    }
    case OpKind::PutImage: {
        // f: dx, dy, width, height of the written box; bytes at a, source row stride b pixels.
        const int dx = static_cast<int>(op.f[0]), dy = static_cast<int>(op.f[1]), cw = static_cast<int>(op.f[2]), ch = static_cast<int>(op.f[3]);
        const std::uint8_t* srcBytes = op.raw ? op.raw : bytes.data() + op.a;
        const IBox rows = clipBox(Style{});
        for (int y = 0; y < ch; ++y) {
            const int ty = dy + y;
            if (ty < rows.y0 || ty >= rows.y1) continue;
            const int x0 = std::max(0, dx), x1 = std::min(w, dx + cw);
            if (x0 >= x1) continue;
            std::memcpy(px.data() + static_cast<std::size_t>(ty) * w + x0, srcBytes + (static_cast<std::size_t>(y) * op.b + (x0 - dx)) * 4,
                        static_cast<std::size_t>(x1 - x0) * 4);
            // A pixel written fully transparent is stored as transparent black.
            for (int x = x0; x < x1; ++x) { Color& p = px[static_cast<std::size_t>(ty) * w + x]; if (p.a == 0) p = Color{0, 0, 0, 0}; }
        }
        return;
    }
    }
}
