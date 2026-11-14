// detail/effects.h -- the parts of the HTML canvas drawing model the software backend draws itself:
//   * Gaussian blur (Filter Effects 1 section 9, feGaussianBlur's three-box approximation) for `filter: blur()`,
//     drop-shadow() and shadowBlur, on premultiplied RGBA8 layers and on 8-bit masks;
//   * the CSS filter functions that are colour transforms (Filter Effects 1 section 13 shorthands);
//   * every globalCompositeOperation (Compositing and Blending 1: Porter-Duff operators, separable and non-separable
//     blend modes, plus `lighter`).
//
// Pixel contracts. Layers handed in here are PREMULTIPLIED RGBA8; the canvas itself stores STRAIGHT-alpha `Color`s. The
// compositing functions take a premultiplied source span and a straight destination span, do the arithmetic in
// premultiplied space (float) and write straight alpha back, rounding to nearest; a zero result alpha is stored as
// {0,0,0,0}. Pixels a bounded operator leaves alone may keep whatever colour a zero-alpha pixel already had.
//
// Chrome parity (measured against Chrome's software canvas; see tests/effects_test.cpp --chrome):
//   * gaussianBlurPremul is Skia's raster image blur (what `filter: blur()` / drop-shadow() / shadows of drawImage use):
//     the three boxes run as one cascade of integer running sums with a single final rounding
//     ((sum * round(2^32 / divisor) + 2^31) >> 32), d = floor(sigma * 3 sqrt(2 pi) / 4 + 0.5); a one pixel box
//     (sigma < ~0.8) is the identity. Bit-exact with Chrome on the cases tested.
//   * gaussianBlurAlpha is Skia's mask blur (what Chrome uses for shadows of fills, strokes and text): identical to the
//     above from sigma 2 up, but below sigma 2 (both axes) a small discrete-Gaussian kernel (see makeSmallKernel).
//     Shadows of axis-aligned rects take a third, analytic path in Chrome: effects_impl::rectBlurApplies /
//     rectBlurMask reproduce it (optional; use them for fillRect-style shadows to be pixel-exact).
//   * Shorthand colour filters run on sRGB values (Chrome does not linearise them), with the Filter Effects matrices
//     (grayscale/sepia: 4-digit coefficients; saturate/hue-rotate: Blink's 3-digit ones). brightness/contrast/invert/
//     opacity are component transfers that Blink bakes into truncated 8-bit lookup tables; emulated exactly.
//   * Non-separable blend modes use the spec's Lum weights 0.3/0.59/0.11.
//   * Keywords: Chrome 157 ignores "plus-lighter" (keeps the previous operation) but accepts "clear" (dst * (1 - shape
//     coverage), ignoring fill style and globalAlpha), which has no CompositeOp: emulate it with DestinationOut and a
//     source whose alpha is the bare coverage.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "../canvas.h"

// Forces inlining of the per-pixel operator bodies into the chunk loops (they must be inlined to vectorise).
#if defined(__GNUC__) || defined(__clang__)
#define CANVAS_EFFECTS_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define CANVAS_EFFECTS_ALWAYS_INLINE inline
#endif

namespace canvas_detail {

enum class ColorFilterKind : std::uint8_t { Brightness, Contrast, Grayscale, HueRotate, Invert, Opacity, Saturate, Sepia };

enum class CompositeOp : std::uint8_t {
    SourceOver, SourceIn, SourceOut, SourceAtop, DestinationOver, DestinationIn, DestinationOut, DestinationAtop,
    Lighter, Copy, Xor,
    Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, HardLight, SoftLight, Difference, Exclusion,
    Hue, Saturation, Color, Luminosity
};

namespace effects_impl {

static_assert(sizeof(::Color) == 4, "Color must be 4 packed bytes");

// round(x / 255) for x in [0, 65535].
inline std::uint32_t div255(std::uint32_t x) {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// ======================================================================================================================
// Blur
// ======================================================================================================================

// Largest sigma honoured. Past it the result is indistinguishable from transparent at 8 bits anyway, and it bounds the
// 64-bit sums of the analytic path below.
constexpr float kMaxBlurSigma = 10000.f;

struct BoxPlan {
    int d = 0;               // box size; 0 means "no blur on this axis"
    int border = 0;          // kernel radius: output i reads inputs [i - border, i + border]
    int ring2 = 0;           // length of the third box's trailing-edge ring (d - 1, or d for an even d)
    std::uint64_t divisor = 0;  // d^3, or d^2 (d + 1) for an even d
    std::uint64_t weight = 0;   // round(2^32 / divisor), the fixed-point reciprocal Skia uses
    int box[3][2] = {};      // (left, right) extents of the three boxes
};

inline BoxPlan makeBoxPlan(float sigma) {
    BoxPlan p;
    if (!(sigma >= 0.05f)) return p;  // also rejects NaN
    if (sigma > kMaxBlurSigma) sigma = kMaxBlurSigma;
    // d = floor(s * 3 * sqrt(2 * pi) / 4 + 0.5)  (Filter Effects 1, feGaussianBlur)
    const int d = static_cast<int>(std::floor(static_cast<double>(sigma) * 1.87997120597325 + 0.5));
    if (d <= 1) return p;  // three one-pixel boxes are the identity (Chrome leaves blur(0.5px) untouched too)
    p.d = d;
    const std::uint64_t ud = static_cast<std::uint64_t>(d);
    if (d & 1) {
        p.border = 3 * (d / 2);
        p.ring2 = d - 1;
        p.divisor = ud * ud * ud;
        for (auto& b : p.box) { b[0] = d / 2; b[1] = d / 2; }
    } else {
        // Two boxes of size d centred on the left / right pixel edge, then one of size d + 1 centred on the pixel.
        p.border = 3 * (d / 2) - 1;
        p.ring2 = d;
        p.divisor = ud * ud * (ud + 1);
        p.box[0][0] = d / 2;     p.box[0][1] = d / 2 - 1;
        p.box[1][0] = d / 2 - 1; p.box[1][1] = d / 2;
        p.box[2][0] = d / 2;     p.box[2][1] = d / 2;
    }
    p.weight = static_cast<std::uint64_t>(std::llround(4294967296.0 / static_cast<double>(p.divisor)));
    return p;
}

// The running-sum cascade keeps its sums in 32 bits: 255 * divisor must fit, which holds up to d = 255 (as in Skia).
constexpr int kMaxRingBox = 255;

// Fixed-point division shared by both paths so that they agree bit for bit.
inline std::uint32_t boxRound(std::uint64_t sum, std::uint64_t weight) {
    const std::uint64_t v = (sum * weight + 0x80000000ull) >> 32;
    return v > 255 ? 255u : static_cast<std::uint32_t>(v);
}

// One line of L interleaved lanes blurred in place by the three-box cascade, in a single sweep (Skia's GaussPass): the
// three box sums are updated per input sample and the trailing edges come out of three small ring buffers. Output i
// is produced when input i + border is consumed, so writing in place never clobbers an unread input.
//   lane l of sample j lives at base[j * step + (l / G) * laneStride + l % G]: groups of G contiguous bytes
//   (G == L: all lanes contiguous; G == 1: one byte from each of L rows; G == 4: one pixel from each of L / 4 rows)
// `ring` must hold (2 * (d - 1) + ring2) * L words.
template <int L, int G>
inline void ringBlurLine(std::uint8_t* base, std::ptrdiff_t step, std::ptrdiff_t laneStride, int n, const BoxPlan& plan,
                         std::uint32_t* ring) {
    const int p = plan.d - 1;
    const int p2 = plan.ring2;
    const int border = plan.border;
    const std::uint64_t weight = plan.weight;
    std::uint32_t* const b0 = ring;
    std::uint32_t* const b1 = ring + static_cast<std::size_t>(p) * L;
    std::uint32_t* const b2 = b1 + static_cast<std::size_t>(p) * L;
    std::fill(ring, b2 + static_cast<std::size_t>(p2) * L, 0u);
    std::uint32_t s0[L] = {}, s1[L] = {}, s2[L] = {};
    int k01 = 0, k2 = 0;

    auto lane = [&](std::ptrdiff_t l) -> std::ptrdiff_t { return G == L ? l : (l / G) * laneStride + (l % G); };
    auto advance = [&](const std::uint32_t* x) {
        std::uint32_t* r0 = b0 + static_cast<std::size_t>(k01) * L;
        std::uint32_t* r1 = b1 + static_cast<std::size_t>(k01) * L;
        std::uint32_t* r2 = b2 + static_cast<std::size_t>(k2) * L;
        for (int l = 0; l < L; ++l) {
            s2[l] -= r2[l];
            r2[l] = s1[l];
            s1[l] -= r1[l];
            r1[l] = s0[l];
            s0[l] -= r0[l];
            r0[l] = x[l];
        }
        if (++k01 == p) k01 = 0;
        if (++k2 == p2) k2 = 0;
    };
    auto accumulate = [&](const std::uint32_t* x) {
        for (int l = 0; l < L; ++l) {
            s0[l] += x[l];
            s1[l] += s0[l];
            s2[l] += s1[l];
        }
    };
    auto emit = [&](int i) {
        std::uint8_t* o = base + static_cast<std::ptrdiff_t>(i) * step;
        for (int l = 0; l < L; ++l) o[lane(l)] = static_cast<std::uint8_t>(boxRound(s2[l], weight));
    };
    auto load = [&](int j, std::uint32_t* x) {
        const std::uint8_t* in = base + static_cast<std::ptrdiff_t>(j) * step;
        for (int l = 0; l < L; ++l) x[l] = in[lane(l)];
    };

    std::uint32_t x[L];
    const int warm = std::min(n, border);
    int j = 0;
    for (; j < warm; ++j) {  // inputs whose outputs would land left of the line
        load(j, x);
        accumulate(x);
        advance(x);
    }
    for (; j < n; ++j) {  // steady state: one input in, one output out
        load(j, x);
        accumulate(x);
        emit(j - border);
        advance(x);
    }
    for (int l = 0; l < L; ++l) x[l] = 0;
    for (; j < border; ++j) {  // only when the kernel is wider than the line
        accumulate(x);
        advance(x);
    }
    for (; j < n + border; ++j) {  // drain: transparent black past the end
        accumulate(x);
        emit(j - border);
        advance(x);
    }
}

inline void store64(std::uint32_t* p, std::int64_t v) { std::memcpy(p, &v, sizeof v); }
inline std::int64_t load64(const std::uint32_t* p) {
    std::int64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// The same convolution for kernels much wider than the line, in O(n) whatever d is. With P3 the third iterated prefix
// sum of the (zero-extended) line, each box is a difference of two shifted prefix sums, so the cascade is a signed sum
// of eight shifted P3 samples. P3 is tabulated over [0, n] and is a known quadratic beyond n (the line sums to a
// constant there) and zero below 0. `scratch` must hold 2 * (n + 1) words.
inline void analyticBlurLine(std::uint8_t* base, std::ptrdiff_t step, int n, const BoxPlan& plan, std::uint32_t* scratch) {
    std::int64_t p1 = 0, p2 = 0, p3 = 0;
    store64(scratch, 0);
    for (int m = 0; m < n; ++m) {
        p3 += p2;
        p2 += p1;
        p1 += base[static_cast<std::ptrdiff_t>(m) * step];
        store64(scratch + 2 * static_cast<std::size_t>(m + 1), p3);
    }
    const std::int64_t total = p1, p2n = p2, p3n = p3;
    auto P3 = [&](std::int64_t m) -> std::int64_t {
        if (m <= 0) return 0;
        if (m <= n) return load64(scratch + 2 * static_cast<std::size_t>(m));
        const std::int64_t q = m - n;
        return p3n + q * p2n + total * (q * (q - 1) / 2);
    };
    std::int64_t off[8];
    int sign[8];
    for (int mask = 0; mask < 8; ++mask) {
        std::int64_t o = 0;
        int s = 1;
        for (int t = 0; t < 3; ++t) {
            if (mask & (1 << t)) {
                o += plan.box[t][1] + 1;
            } else {
                o -= plan.box[t][0];
                s = -s;
            }
        }
        off[mask] = o;
        sign[mask] = s;
    }
    const bool fixedPoint = plan.divisor < (1ull << 24);
    const double inv = 1.0 / static_cast<double>(plan.divisor);
    for (int i = 0; i < n; ++i) {
        std::int64_t sum = 0;
        for (int k = 0; k < 8; ++k) sum += sign[k] * P3(i + off[k]);
        if (sum < 0) sum = 0;
        std::uint32_t v;
        if (fixedPoint) {
            v = boxRound(static_cast<std::uint64_t>(sum), plan.weight);
        } else {
            const double r = std::floor(static_cast<double>(sum) * inv + 0.5);
            v = r > 255.0 ? 255u : static_cast<std::uint32_t>(r);
        }
        base[static_cast<std::ptrdiff_t>(i) * step] = static_cast<std::uint8_t>(v);
    }
}

// The ring sweep costs O(n + border) per line; past a few line lengths (or when its 32-bit sums could overflow) the
// analytic path is cheaper and always exact.
inline bool useAnalytic(const BoxPlan& plan, int n) { return plan.d > kMaxRingBox || plan.border > 4 * n + 16; }

inline std::size_t ringWords(const BoxPlan& plan, int lanes) {
    return static_cast<std::size_t>(2 * (plan.d - 1) + plan.ring2) * static_cast<std::size_t>(lanes);
}

constexpr int kStripPixels = 8;    // RGBA columns blurred together by the vertical pass (32 lanes)
constexpr int kStripBytes = 32;    // alpha columns blurred together by the vertical pass
constexpr int kRowBundle = 8;      // alpha rows blurred together by the horizontal pass
constexpr int kRgbaRowBundle = 4;  // RGBA rows blurred together by the horizontal pass (16 lanes)

inline void ensureScratch(std::vector<std::uint32_t>& scratch, std::size_t words) {
    if (scratch.size() < words) scratch.resize(words);
}

// ---- small-sigma mask blur ----------------------------------------------------------------------------------------
// Chrome blurs the coverage masks of shadowed fills and strokes with Skia's mask blur, which below sigma 2 (on both
// axes) uses no boxes but Lindeberg's discrete Gaussian T(k; t) = e^-t I_k(t), t = sigma^2 (SkGaussFilter): taps are
// added while the last one exceeds 1 %, then normalised so they sum to one, quantised to 0.16 fixed point and applied
// one axis at a time in 8.8 fixed point (per-tap products truncated, a half added before the final >> 8; the
// horizontal result is stored as 8 bits before the vertical pass). At or below sigma 1/3 nothing is blurred.
constexpr float kSmallSigma = 2.f;
constexpr int kMaxSmallRadius = 4;

struct SmallKernel {
    int radius = 0;  // 0: identity
    std::uint32_t g[kMaxSmallRadius + 2] = {};
};

inline SmallKernel makeSmallKernel(float sigma) {
    SmallKernel k;
    if (!(sigma > 1.f / 3.f) || !(sigma < kSmallSigma)) return k;
    const double t = static_cast<double>(sigma) * static_cast<double>(sigma);
    const double q = t * t / 4.0;
    double i0 = 1.0, f0 = 1.0, i1 = t / 2.0, f1 = t / 2.0;
    for (int m = 1; f0 > 1e-6; ++m) {
        f0 *= q / (static_cast<double>(m) * m);
        i0 += f0;
    }
    for (int m = 1; f1 > 1e-6; ++m) {
        f1 *= q / (static_cast<double>(m) * (m + 1));
        i1 += f1;
    }
    const double e = std::exp(t);
    double b[kMaxSmallRadius + 3] = {i0, i1};
    double gauss[kMaxSmallRadius + 3] = {i0 / e, i1 / e};
    int n = 1;
    while (gauss[n] > 0.01 && n < kMaxSmallRadius + 1) {
        b[n + 1] = -(2.0 * n / t) * b[n] + b[n - 1];
        gauss[n + 1] = b[n + 1] / e;
        ++n;
    }
    double sum = 0;
    for (int i = n - 1; i >= 1; --i) sum += 2 * gauss[i];
    sum += gauss[0];
    for (int i = 0; i < n; ++i) gauss[i] /= sum;
    sum = 0;
    for (int i = n - 1; i >= 1; --i) sum += 2 * gauss[i];
    gauss[0] = 1 - sum;
    if (n <= 1) return k;
    k.radius = n - 1;
    for (int i = 0; i < n; ++i) k.g[i] = static_cast<std::uint32_t>(std::round(gauss[i] * 65536.0)) & 0xffffu;
    return k;
}

// One line of L lanes (as in ringBlurLine) convolved with a small kernel. `scratch` holds (n + 2 radius) * L words.
template <int L, int G>
inline void smallBlurLine(std::uint8_t* base, std::ptrdiff_t step, std::ptrdiff_t laneStride, int n, const SmallKernel& k,
                          std::uint32_t* scratch) {
    const int r = k.radius;
    auto lane = [&](std::ptrdiff_t l) -> std::ptrdiff_t { return G == L ? l : (l / G) * laneStride + (l % G); };
    std::fill(scratch, scratch + static_cast<std::size_t>(r) * L, 0u);
    std::fill(scratch + static_cast<std::size_t>(n + r) * L, scratch + static_cast<std::size_t>(n + 2 * r) * L, 0u);
    for (int j = 0; j < n; ++j) {
        const std::uint8_t* in = base + static_cast<std::ptrdiff_t>(j) * step;
        std::uint32_t* o = scratch + static_cast<std::size_t>(j + r) * L;
        for (int l = 0; l < L; ++l) o[l] = static_cast<std::uint32_t>(in[lane(l)]) << 8;
    }
    for (int j = 0; j < n; ++j) {
        std::uint32_t acc[L];
        for (int l = 0; l < L; ++l) acc[l] = 0x80u;
        for (int t = -r; t <= r; ++t) {
            const std::uint32_t g = k.g[t < 0 ? -t : t];
            const std::uint32_t* s = scratch + static_cast<std::size_t>(j + r + t) * L;
            for (int l = 0; l < L; ++l) acc[l] += (s[l] * g) >> 16;
        }
        std::uint8_t* o = base + static_cast<std::ptrdiff_t>(j) * step;
        for (int l = 0; l < L; ++l) o[lane(l)] = static_cast<std::uint8_t>(std::min(acc[l] >> 8, 255u));
    }
}

// ---- Chrome's analytic rectangle shadow (SkBlurMask::BlurRect) ---------------------------------------------------
// Optional, for the canvas's shadow of an axis-aligned rectangle (fillRect, or a rect path under a scale/translate
// transform). Skia does not blur a rasterised mask for those: when the rectangle is large enough for a nine-patch
// (rectBlurApplies) it evaluates a separable profile built from the integral of the cubic B-spline it uses for the
// Gaussian, h(x) * v(y) / 255 rounded, over the rect's bounds outset by ceil(6 sigma) / 2 (rounded to pixels; the
// rect's fractional position only moves those rounded bounds). Narrower rects take the generic mask blur, i.e.
// gaussianBlurAlpha() of the rect's coverage.

inline float splineIntegral(float x) {  // SkBlurMask's gaussianIntegral: integral of the B-spline on [-inf, x], x in 2 sigma units
    if (x > 1.5f) return 0.0f;
    if (x < -1.5f) return 1.0f;
    const float x2 = x * x, x3 = x2 * x;
    if (x > 0.5f) return 0.5625f - (x3 / 6.0f - 3.0f * x2 * 0.25f + 1.125f * x);
    if (x > -0.5f) return 0.5f - (0.75f * x - x3 / 3.0f);
    return 0.4375f + (-x3 / 6.0f - 3.0f * x2 * 0.25f - 1.125f * x);
}

inline int skRoundToInt(float v) { return static_cast<int>(std::floor(v + 0.5f)); }
inline int rectProfileSize(float sigma) { return static_cast<int>(std::ceil(6.f * sigma)); }

// Mirrors SkBlurMaskFilterImpl::filterRectsToNine's decision (blur style normal, single rect).
inline bool rectBlurApplies(float left, float top, float right, float bottom, float sigma) {
    if (!(sigma > 0.f) || !(right > left) || !(bottom > top)) return false;
    const float lim = 32767.f;
    if (!(std::fabs(left) <= lim && std::fabs(top) <= lim && std::fabs(right) <= lim && std::fabs(bottom) <= lim)) return false;
    sigma = std::min(sigma, 532.f);
    const int ps = rectProfileSize(sigma);
    if (ps <= 0) return false;
    const int pad = ps / 2;
    const int dstW = skRoundToInt(right + pad) - skRoundToInt(left - pad);
    const int dstH = skRoundToInt(bottom + pad) - skRoundToInt(top - pad);
    const int srcW = static_cast<int>(std::ceil(right)) - static_cast<int>(std::floor(left));
    const int srcH = static_cast<int>(std::ceil(bottom)) - static_cast<int>(std::floor(top));
    const int smallW = dstW - srcW + 2 + 1, smallH = dstH - srcH + 2 + 1;
    const int dx = srcW - smallW, dy = srcH - smallH;
    if (dx < 0 || dy < 0) return false;
    return (right - left) - static_cast<float>(dx) >= 2.f && (bottom - top) - static_cast<float>(dy) >= 2.f;
}

// One blurred scanline of `width` (the outset bounds' size) samples (SkBlurMask::ComputeBlurredScanline).
inline void rectScanline(std::uint8_t* out, int width, float sigma) {
    const int ps = rectProfileSize(sigma);
    const int center = ps >> 1;
    const float invr = 1.f / (2.f * sigma);
    auto profile = [&](int x) -> std::uint8_t {
        if (x <= 0) return 255;
        const float gi = splineIntegral((static_cast<float>(center - x) - 0.5f) * invr);
        return static_cast<std::uint8_t>(255 - static_cast<std::uint8_t>(255.f * gi));
    };
    const int sw = width - ps;
    const int mid = (ps & ~1) - 1;
    const int w = sw - mid;
    for (int x = 0; x < width; ++x) {
        if (ps <= sw) {
            const int d = std::abs(((x << 1) + 1) - width) - w;
            out[x] = profile(d < 0 ? 0 : d >> 1);
        } else {
            const float span = static_cast<float>(sw) / (2.f * sigma);
            const float gx = 1.5f - (static_cast<float>(x) + 0.5f) / (2.f * sigma);
            out[x] = static_cast<std::uint8_t>(255.f * (splineIntegral(gx) - splineIntegral(gx + span)));
        }
    }
}

// Writes the shadow mask Chrome draws for the rect (device pixels) into mask[y * strideBytes + x] for the device pixels
// (originX + x, originY + y), 0 <= x < width, 0 <= y < height; call only when rectBlurApplies(). Emulates the
// nine-patch exactly: BlurRect of a small rect (the original with its right/bottom pulled in by whole pixels, so the
// fractional phase is kept) is split at its centre column/row; corners are copied, the centre column/row is stretched
// and the inside is filled solid. For fractional rects the small rect's centre row/column is not always saturated
// (Skia's short-scanline branch), which Chrome shows as half-intensity edges; that is reproduced as well.
inline void rectBlurMask(float left, float top, float right, float bottom, float sigma, std::uint8_t* mask, int originX,
                         int originY, int width, int height, int strideBytes, std::vector<std::uint32_t>& scratch) {
    if (!mask || width <= 0 || height <= 0) return;
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = mask + static_cast<std::ptrdiff_t>(y) * strideBytes;
        std::fill(row, row + width, 0);
    }
    if (!rectBlurApplies(left, top, right, bottom, sigma)) return;
    sigma = std::min(sigma, 532.f);
    const int pad = rectProfileSize(sigma) / 2;
    // Outer (dstM) bounds and the source mask bounds (rect.roundOut()).
    const int L = skRoundToInt(left - pad), T = skRoundToInt(top - pad);
    const int R = skRoundToInt(right + pad), B = skRoundToInt(bottom + pad);
    const int srcW = static_cast<int>(std::ceil(right)) - static_cast<int>(std::floor(left));
    const int srcH = static_cast<int>(std::ceil(bottom)) - static_cast<int>(std::floor(top));
    const int cx = (R - L) - srcW + 2, cy = (B - T) - srcH + 2;  // patch centre column / row
    const int dx = srcW - (cx + 1), dy = srcH - (cy + 1);
    // The patch: BlurRect of the small rect.
    const float sr = right - static_cast<float>(dx), sb = bottom - static_cast<float>(dy);
    const int pW = skRoundToInt(sr + pad) - L, pH = skRoundToInt(sb + pad) - T;
    if (pW <= cx || pH <= cy) return;
    ensureScratch(scratch, static_cast<std::size_t>(pW + pH + 3) / 4 + 2);
    std::uint8_t* hs = reinterpret_cast<std::uint8_t*>(scratch.data());
    std::uint8_t* vs = hs + pW;
    rectScanline(hs, pW, sigma);
    rectScanline(vs, pH, sigma);
    // Inner (stretched) rect.
    const int iL = L + cx, iT = T + cy, iR = R + (cx + 1 - pW), iB = B + (cy + 1 - pH);
    auto column = [&](int X, bool& centre) -> std::uint32_t {  // patch scanline value for device column X
        centre = false;
        if (X < iL) return hs[X - L];
        if (X >= iR) return hs[cx + 1 + (X - iR)];
        centre = true;
        return hs[cx];
    };
    const int x0 = std::max(0, L - originX), x1 = std::min(width, R - originX);
    const int y0 = std::max(0, T - originY), y1 = std::min(height, B - originY);
    for (int y = y0; y < y1; ++y) {
        const int Y = originY + y;
        bool rowCentre = false;
        std::uint32_t v;
        if (Y < iT) v = vs[Y - T];
        else if (Y >= iB) v = vs[cy + 1 + (Y - iB)];
        else { v = vs[cy]; rowCentre = true; }
        std::uint8_t* row = mask + static_cast<std::ptrdiff_t>(y) * strideBytes;
        for (int x = x0; x < x1; ++x) {
            bool colCentre;
            const std::uint32_t h = column(originX + x, colCentre);
            row[x] = (rowCentre && colCentre) ? 255 : static_cast<std::uint8_t>(div255(h * v));
        }
    }
}

}  // namespace effects_impl

// ======================================================================================================================
// Blur API
// ======================================================================================================================

// Kernel radius in pixels of either blur flavour (box cascade, or the small-sigma mask kernel below sigma 2).
inline int blurExtent(float sigma) {
    const effects_impl::BoxPlan p = effects_impl::makeBoxPlan(sigma);
    const int box = p.d ? p.border : 0;
    return std::max(box, effects_impl::makeSmallKernel(sigma).radius);
}

inline void gaussianBlurPremul(std::uint8_t* rgba, int width, int height, int stridePixels, float sigmaX, float sigmaY,
                               std::vector<std::uint32_t>& scratch) {
    using namespace effects_impl;
    if (!rgba || width <= 0 || height <= 0 || stridePixels < width) return;
    const BoxPlan px = makeBoxPlan(sigmaX), py = makeBoxPlan(sigmaY);
    const std::ptrdiff_t rowBytes = static_cast<std::ptrdiff_t>(stridePixels) * 4;
    if (px.d) {
        if (useAnalytic(px, width)) {
            ensureScratch(scratch, 2 * (static_cast<std::size_t>(width) + 1));
            for (int y = 0; y < height; ++y)
                for (int c = 0; c < 4; ++c) analyticBlurLine(rgba + y * rowBytes + c, 4, width, px, scratch.data());
        } else {
            // Several rows per sweep: independent running sums give the core more to overlap.
            ensureScratch(scratch, ringWords(px, 4 * kRgbaRowBundle));
            int y = 0;
            for (; y + kRgbaRowBundle <= height; y += kRgbaRowBundle)
                ringBlurLine<4 * kRgbaRowBundle, 4>(rgba + y * rowBytes, 4, rowBytes, width, px, scratch.data());
            for (; y < height; ++y) ringBlurLine<4, 4>(rgba + y * rowBytes, 4, 1, width, px, scratch.data());
        }
    }
    if (py.d) {
        if (useAnalytic(py, height)) {
            ensureScratch(scratch, 2 * (static_cast<std::size_t>(height) + 1));
            for (int x = 0; x < width * 4; ++x) analyticBlurLine(rgba + x, rowBytes, height, py, scratch.data());
        } else {
            ensureScratch(scratch, ringWords(py, 4 * kStripPixels));
            int x = 0;
            for (; x + kStripPixels <= width; x += kStripPixels)
                ringBlurLine<4 * kStripPixels, 4 * kStripPixels>(rgba + x * 4, rowBytes, 1, height, py, scratch.data());
            for (; x < width; ++x) ringBlurLine<4, 4>(rgba + x * 4, rowBytes, 1, height, py, scratch.data());
        }
    }
}

inline void gaussianBlurAlpha(std::uint8_t* alpha, int width, int height, int strideBytes, float sigmaX, float sigmaY,
                              std::vector<std::uint32_t>& scratch) {
    using namespace effects_impl;
    if (!alpha || width <= 0 || height <= 0 || strideBytes < width) return;
    const std::ptrdiff_t rowBytes = strideBytes;
    if (!(sigmaX > 0.f)) sigmaX = 0.f;  // NaN / negative -> no blur on that axis
    if (!(sigmaY > 0.f)) sigmaY = 0.f;
    if (sigmaX < kSmallSigma && sigmaY < kSmallSigma) {  // Skia's small-sigma mask path (see makeSmallKernel)
        const SmallKernel kx = makeSmallKernel(sigmaX), ky = makeSmallKernel(sigmaY);
        if (kx.radius) {
            ensureScratch(scratch, (static_cast<std::size_t>(width) + 2 * kMaxSmallRadius) * kRowBundle);
            int y = 0;
            for (; y + kRowBundle <= height; y += kRowBundle)
                smallBlurLine<kRowBundle, 1>(alpha + y * rowBytes, 1, rowBytes, width, kx, scratch.data());
            for (; y < height; ++y) smallBlurLine<1, 1>(alpha + y * rowBytes, 1, 1, width, kx, scratch.data());
        }
        if (ky.radius) {
            ensureScratch(scratch, (static_cast<std::size_t>(height) + 2 * kMaxSmallRadius) * kStripBytes);
            int x = 0;
            for (; x + kStripBytes <= width; x += kStripBytes)
                smallBlurLine<kStripBytes, kStripBytes>(alpha + x, rowBytes, 1, height, ky, scratch.data());
            for (; x < width; ++x) smallBlurLine<1, 1>(alpha + x, rowBytes, 1, height, ky, scratch.data());
        }
        return;
    }
    const BoxPlan px = makeBoxPlan(sigmaX), py = makeBoxPlan(sigmaY);
    if (px.d) {
        if (useAnalytic(px, width)) {
            ensureScratch(scratch, 2 * (static_cast<std::size_t>(width) + 1));
            for (int y = 0; y < height; ++y) analyticBlurLine(alpha + y * rowBytes, 1, width, px, scratch.data());
        } else {
            ensureScratch(scratch, ringWords(px, kRowBundle));
            int y = 0;
            for (; y + kRowBundle <= height; y += kRowBundle)
                ringBlurLine<kRowBundle, 1>(alpha + y * rowBytes, 1, rowBytes, width, px, scratch.data());
            for (; y < height; ++y) ringBlurLine<1, 1>(alpha + y * rowBytes, 1, 1, width, px, scratch.data());
        }
    }
    if (py.d) {
        if (useAnalytic(py, height)) {
            ensureScratch(scratch, 2 * (static_cast<std::size_t>(height) + 1));
            for (int x = 0; x < width; ++x) analyticBlurLine(alpha + x, rowBytes, height, py, scratch.data());
        } else {
            ensureScratch(scratch, ringWords(py, kStripBytes));
            int x = 0;
            for (; x + kStripBytes <= width; x += kStripBytes)
                ringBlurLine<kStripBytes, kStripBytes>(alpha + x, rowBytes, 1, height, py, scratch.data());
            for (; x < width; ++x) ringBlurLine<1, 1>(alpha + x, rowBytes, 1, height, py, scratch.data());
        }
    }
}

// ======================================================================================================================
// Pixel helpers
// ======================================================================================================================

inline void premultiply(const Color& straight, std::uint8_t out[4]) {
    const std::uint32_t a = straight.a;
    out[0] = static_cast<std::uint8_t>(effects_impl::div255(straight.r * a));
    out[1] = static_cast<std::uint8_t>(effects_impl::div255(straight.g * a));
    out[2] = static_cast<std::uint8_t>(effects_impl::div255(straight.b * a));
    out[3] = static_cast<std::uint8_t>(a);
}

inline Color unpremultiply(const std::uint8_t premul[4]) {
    const std::uint32_t a = premul[3];
    if (a == 0) return Color(0, 0, 0, 0);
    if (a == 255) return Color(premul[0], premul[1], premul[2], 255);
    auto un = [a](std::uint32_t c) -> std::uint8_t {
        const std::uint32_t v = (c * 255 + a / 2) / a;
        return static_cast<std::uint8_t>(v > 255 ? 255 : v);
    };
    return Color(un(premul[0]), un(premul[1]), un(premul[2]), static_cast<std::uint8_t>(a));
}

// ======================================================================================================================
// Colour filters
// ======================================================================================================================

namespace effects_impl {

struct ColorMatrix3 {
    float m[9];
};

// Filter Effects 1 section 13 shorthand matrices, with the coefficients Blink uses.
inline ColorMatrix3 filterMatrix(ColorFilterKind kind, float amount) {
    switch (kind) {
        case ColorFilterKind::Grayscale: {
            const float s = 1.f - clamp01(amount);
            return {{0.2126f + 0.7874f * s, 0.7152f - 0.7152f * s, 0.0722f - 0.0722f * s,
                     0.2126f - 0.2126f * s, 0.7152f + 0.2848f * s, 0.0722f - 0.0722f * s,
                     0.2126f - 0.2126f * s, 0.7152f - 0.7152f * s, 0.0722f + 0.9278f * s}};
        }
        case ColorFilterKind::Sepia: {
            const float s = 1.f - clamp01(amount);
            return {{0.393f + 0.607f * s, 0.769f - 0.769f * s, 0.189f - 0.189f * s,
                     0.349f - 0.349f * s, 0.686f + 0.314f * s, 0.168f - 0.168f * s,
                     0.272f - 0.272f * s, 0.534f - 0.534f * s, 0.131f + 0.869f * s}};
        }
        case ColorFilterKind::Saturate: {
            const float s = std::max(amount, 0.f);
            return {{0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s,
                     0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s,
                     0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s}};
        }
        default: {  // HueRotate
            const float c = std::cos(amount), s = std::sin(amount);
            return {{0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
                     0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
                     0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f}};
        }
    }
}

// Chrome (Skia's raster pipeline) unpremultiplies in float: straight = (c / 255) * (1 / (a / 255)). The table holds the
// second factor (0 for a == 0); emulating it exactly matters where c * 255 / a is a tie (Skia lands just below).
struct UnpremulTable {
    float v[256];
    UnpremulTable() {
        v[0] = 0.f;
        for (int a = 1; a < 256; ++a) v[a] = 1.f / (static_cast<float>(a) * (1.f / 255.f));
    }
};
inline const UnpremulTable& unpremulTable() {
    static const UnpremulTable t;
    return t;
}

// Blink turns brightness/contrast (feFunc type="linear") and invert/opacity (type="table") into 256-entry byte tables
// (FEComponentTransfer: Linear() in float, Table() in double, both truncated), which Skia then indexes with the
// rounded unpremultiplied value.
inline void linearTable(float slope, float intercept, std::uint8_t lut[256]) {
    for (int i = 0; i < 256; ++i) {
        double v = static_cast<double>(slope * static_cast<float>(i) + 255.f * intercept);
        v = v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
        lut[i] = static_cast<std::uint8_t>(v);
    }
}
inline void twoEntryTable(float v0, float v1, std::uint8_t lut[256]) {
    for (int i = 0; i < 256; ++i) {
        const double c = i / 255.0;
        const unsigned k = static_cast<unsigned>(c);
        const double a = k ? v1 : v0;
        double v = 255.0 * (a + (c - k) * (static_cast<double>(v1) - a));
        v = v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
        lut[i] = static_cast<std::uint8_t>(v);
    }
}

}  // namespace effects_impl

inline void applyColorFilter(std::uint8_t* p, std::size_t count, ColorFilterKind kind, float amount) {
    using namespace effects_impl;
    if (!p || count == 0) return;
    if (!std::isfinite(amount)) amount = 0.f;
    const float* inv = unpremulTable().v;

    if (kind == ColorFilterKind::Brightness || kind == ColorFilterKind::Contrast || kind == ColorFilterKind::Invert ||
        kind == ColorFilterKind::Opacity) {
        std::uint8_t lut[256], alut[256];
        for (int i = 0; i < 256; ++i) lut[i] = alut[i] = static_cast<std::uint8_t>(i);
        const float pos = std::max(amount, 0.f);
        switch (kind) {
            case ColorFilterKind::Brightness: linearTable(pos, 0.f, lut); break;
            case ColorFilterKind::Contrast:
                linearTable(pos, static_cast<float>(0.5 - 0.5 * static_cast<double>(pos)), lut);
                break;
            case ColorFilterKind::Invert: {
                const float u = clamp01(amount);
                twoEntryTable(u, static_cast<float>(1.0 - static_cast<double>(u)), lut);
                break;
            }
            default: {  // Opacity: identity colour tables, alpha table {0, amount}
                const float u = clamp01(amount);
                if (u >= 1.f) return;
                twoEntryTable(0.f, u, alut);
                break;
            }
        }
        for (std::size_t i = 0; i < count; ++i, p += 4) {
            const std::uint32_t a = p[3];
            if (a == 0) continue;
            const float ia = inv[a];
            const std::uint32_t a2 = alut[a];
            for (int c = 0; c < 3; ++c) {
                float v = static_cast<float>(p[c]) * (1.f / 255.f) * ia;  // Skia's order of operations, see UnpremulTable
                v = v > 1.f ? 1.f : v;
                const std::uint32_t idx = static_cast<std::uint32_t>(v * 255.f + 0.5f);
                p[c] = static_cast<std::uint8_t>(div255(lut[idx] * a2));
            }
            p[3] = static_cast<std::uint8_t>(a2);
        }
        return;
    }

    // Colour matrices (grayscale, sepia, saturate, hue-rotate) on unpremultiplied sRGB, clamped, re-premultiplied.
    const ColorMatrix3 cm = filterMatrix(kind, amount);
    const float* m = cm.m;
    for (std::size_t i = 0; i < count; ++i, p += 4) {
        const std::uint32_t a = p[3];
        const float ia = inv[a];
        const float r = p[0] * (1.f / 255.f) * ia, g = p[1] * (1.f / 255.f) * ia, b = p[2] * (1.f / 255.f) * ia;
        const float fa = static_cast<float>(a);  // premultiply and scale back to bytes in one step
        const float o0 = clamp01(m[0] * r + m[1] * g + m[2] * b);
        const float o1 = clamp01(m[3] * r + m[4] * g + m[5] * b);
        const float o2 = clamp01(m[6] * r + m[7] * g + m[8] * b);
        p[0] = static_cast<std::uint8_t>(o0 * fa + 0.5f);
        p[1] = static_cast<std::uint8_t>(o1 * fa + 0.5f);
        p[2] = static_cast<std::uint8_t>(o2 * fa + 0.5f);
    }
}

// ======================================================================================================================
// Compositing
// ======================================================================================================================

inline bool parseCompositeOp(std::string_view name, CompositeOp& out) {
    struct Entry {
        const char* name;
        CompositeOp op;
    };
    static constexpr Entry kNames[] = {
        {"source-over", CompositeOp::SourceOver},       {"source-in", CompositeOp::SourceIn},
        {"source-out", CompositeOp::SourceOut},         {"source-atop", CompositeOp::SourceAtop},
        {"destination-over", CompositeOp::DestinationOver}, {"destination-in", CompositeOp::DestinationIn},
        {"destination-out", CompositeOp::DestinationOut},   {"destination-atop", CompositeOp::DestinationAtop},
        {"lighter", CompositeOp::Lighter},              {"plus-lighter", CompositeOp::Lighter},
        {"copy", CompositeOp::Copy},                    {"xor", CompositeOp::Xor},
        {"multiply", CompositeOp::Multiply},            {"screen", CompositeOp::Screen},
        {"overlay", CompositeOp::Overlay},              {"darken", CompositeOp::Darken},
        {"lighten", CompositeOp::Lighten},              {"color-dodge", CompositeOp::ColorDodge},
        {"color-burn", CompositeOp::ColorBurn},         {"hard-light", CompositeOp::HardLight},
        {"soft-light", CompositeOp::SoftLight},         {"difference", CompositeOp::Difference},
        {"exclusion", CompositeOp::Exclusion},          {"hue", CompositeOp::Hue},
        {"saturation", CompositeOp::Saturation},        {"color", CompositeOp::Color},
        {"luminosity", CompositeOp::Luminosity},
    };
    for (const Entry& e : kNames) {
        if (name == e.name) {
            out = e.op;
            return true;
        }
    }
    return false;
}

inline const char* compositeOpName(CompositeOp op) {
    switch (op) {
        case CompositeOp::SourceOver: return "source-over";
        case CompositeOp::SourceIn: return "source-in";
        case CompositeOp::SourceOut: return "source-out";
        case CompositeOp::SourceAtop: return "source-atop";
        case CompositeOp::DestinationOver: return "destination-over";
        case CompositeOp::DestinationIn: return "destination-in";
        case CompositeOp::DestinationOut: return "destination-out";
        case CompositeOp::DestinationAtop: return "destination-atop";
        case CompositeOp::Lighter: return "lighter";
        case CompositeOp::Copy: return "copy";
        case CompositeOp::Xor: return "xor";
        case CompositeOp::Multiply: return "multiply";
        case CompositeOp::Screen: return "screen";
        case CompositeOp::Overlay: return "overlay";
        case CompositeOp::Darken: return "darken";
        case CompositeOp::Lighten: return "lighten";
        case CompositeOp::ColorDodge: return "color-dodge";
        case CompositeOp::ColorBurn: return "color-burn";
        case CompositeOp::HardLight: return "hard-light";
        case CompositeOp::SoftLight: return "soft-light";
        case CompositeOp::Difference: return "difference";
        case CompositeOp::Exclusion: return "exclusion";
        case CompositeOp::Hue: return "hue";
        case CompositeOp::Saturation: return "saturation";
        case CompositeOp::Color: return "color";
        case CompositeOp::Luminosity: return "luminosity";
    }
    return "source-over";
}

inline bool compositeIsUnbounded(CompositeOp op) {
    switch (op) {
        case CompositeOp::SourceIn:
        case CompositeOp::SourceOut:
        case CompositeOp::DestinationIn:
        case CompositeOp::DestinationAtop:
        case CompositeOp::Copy:
            return true;
        default:
            return false;
    }
}

inline void sourceOverSpan(Color* dst, const std::uint8_t* src, const std::uint8_t* clip, int count) {
    using effects_impl::div255;
    if (!dst || !src || count <= 0) return;
    std::uint8_t* d = reinterpret_cast<std::uint8_t*>(dst);
    constexpr int kBlock = 64;
    int i = 0;
    while (i < count) {
        const int n = std::min(kBlock, count - i);
        std::uint8_t* db = d + 4 * static_cast<std::ptrdiff_t>(i);
        const std::uint8_t* sb = src + 4 * static_cast<std::ptrdiff_t>(i);
        const std::uint8_t* cb = clip ? clip + i : nullptr;
        unsigned opaque = 255;
        for (int k = 0; k < n; ++k) opaque &= db[4 * k + 3];
        if (opaque == 255) {
            // Opaque destination: out = s + d (1 - as) per channel, alpha included (it stays 255). Branch-free.
            if (!cb) {
                for (int k = 0; k < n; ++k) {
                    const std::uint32_t ia = 255u - sb[4 * k + 3];
                    for (int c = 0; c < 4; ++c)
                        db[4 * k + c] = static_cast<std::uint8_t>(std::min(255u, sb[4 * k + c] + div255(db[4 * k + c] * ia)));
                }
            } else {
                // With clip coverage c the source is scaled by c: out = (s c 255 + d (65025 - as c)) / 65025.
                for (int k = 0; k < n; ++k) {
                    const std::uint32_t cov = cb[k];
                    const std::uint32_t kd = 65025u - sb[4 * k + 3] * cov;
                    const std::uint32_t ks = cov * 255u;
                    for (int c = 0; c < 4; ++c)
                        db[4 * k + c] = static_cast<std::uint8_t>(
                            std::min(255u, (sb[4 * k + c] * ks + db[4 * k + c] * kd + 32512u) / 65025u));
                }
            }
        } else {
            for (int k = 0; k < n; ++k) {
                const std::uint32_t sa = sb[4 * k + 3];
                const std::uint32_t cov = cb ? cb[k] : 255u;
                if (sa == 0 || cov == 0) continue;
                std::uint8_t* o = db + 4 * k;
                const std::uint8_t* s = sb + 4 * k;
                if (cov == 255 && sa == 255) {
                    o[0] = s[0];
                    o[1] = s[1];
                    o[2] = s[2];
                    o[3] = 255;
                    continue;
                }
                const float sk = static_cast<float>(cov) * (1.f / 65025.f);  // premul byte -> [0,1] incl. coverage
                const float as = static_cast<float>(sa) * sk;
                const float kd = static_cast<float>(o[3]) * (1.f / 255.f) * (1.f - as);  // dst weight on straight bytes
                const float ao = as + kd;
                const int A = static_cast<int>(ao * 255.f + 0.5f);
                if (A <= 0) {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }
                const float inv = 1.f / ao;
                for (int c = 0; c < 3; ++c) {
                    const float v = (static_cast<float>(s[c]) * sk * 255.f + static_cast<float>(o[c]) * kd) * inv + 0.5f;
                    o[c] = static_cast<std::uint8_t>(v >= 255.f ? 255.f : v);
                }
                o[3] = static_cast<std::uint8_t>(A > 255 ? 255 : A);
            }
        }
        i += n;
    }
}

namespace effects_impl {

// ---- operators on premultiplied floats in [0, 1]: (cs, as) source, (cb, ab) backdrop -> (co, ao) ---------------------

template <int Fa, int Fb>  // factor codes: 0 -> 0, 1 -> 1, 2 -> ab / as, 3 -> 1 - ab / 1 - as
struct PorterDuff {
    static CANVAS_EFFECTS_ALWAYS_INLINE float fa(float ab) { return Fa == 0 ? 0.f : Fa == 1 ? 1.f : Fa == 2 ? ab : 1.f - ab; }
    static CANVAS_EFFECTS_ALWAYS_INLINE float fb(float as) { return Fb == 0 ? 0.f : Fb == 1 ? 1.f : Fb == 2 ? as : 1.f - as; }
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        const float a = fa(ab), b = fb(as);
        for (int c = 0; c < 3; ++c) co[c] = a * cs[c] + b * cb[c];
        ao = a * as + b * ab;
    }
};

struct LighterOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        for (int c = 0; c < 3; ++c) co[c] = std::min(1.f, cs[c] + cb[c]);
        ao = std::min(1.f, as + ab);
    }
};

// Separable blend modes with closed premultiplied forms (no division).
struct MultiplyOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        for (int c = 0; c < 3; ++c) co[c] = cs[c] * (1.f - ab) + cb[c] * (1.f - as) + cs[c] * cb[c];
        ao = as + ab - as * ab;
    }
};
struct ScreenOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        for (int c = 0; c < 3; ++c) co[c] = cs[c] + cb[c] - cs[c] * cb[c];
        ao = as + ab - as * ab;
    }
};
struct DarkenOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        for (int c = 0; c < 3; ++c) co[c] = cs[c] + cb[c] - std::max(cs[c] * ab, cb[c] * as);
        ao = as + ab - as * ab;
    }
};
struct LightenOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        for (int c = 0; c < 3; ++c) co[c] = cs[c] + cb[c] - std::min(cs[c] * ab, cb[c] * as);
        ao = as + ab - as * ab;
    }
};
struct DifferenceOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        for (int c = 0; c < 3; ++c) co[c] = cs[c] + cb[c] - 2.f * std::min(cs[c] * ab, cb[c] * as);
        ao = as + ab - as * ab;
    }
};
struct ExclusionOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        for (int c = 0; c < 3; ++c) co[c] = cs[c] + cb[c] - 2.f * cs[c] * cb[c];
        ao = as + ab - as * ab;
    }
};

// Separable blend functions B(Cb, Cs) on straight colour (Compositing 1 section 10.1).
// Written branch-free (both sides computed, then selected) so the chunk loops vectorise and random data does not
// mispredict; divisions by zero on the discarded side are harmless (no FP traps).
CANVAS_EFFECTS_ALWAYS_INLINE float blendHardLight(float cb, float cs) {
    const float lo = cb * 2.f * cs;  // Multiply(cb, 2 cs)
    const float s = 2.f * cs - 1.f;
    const float hi = cb + s - cb * s;  // Screen(cb, 2 cs - 1)
    return cs <= 0.5f ? lo : hi;
}
struct OverlayB { static CANVAS_EFFECTS_ALWAYS_INLINE float f(float cb, float cs) { return blendHardLight(cs, cb); } };
struct HardLightB { static CANVAS_EFFECTS_ALWAYS_INLINE float f(float cb, float cs) { return blendHardLight(cb, cs); } };
struct SoftLightB {
    static CANVAS_EFFECTS_ALWAYS_INLINE float f(float cb, float cs) {
        const float lo = cb - (1.f - 2.f * cs) * cb * (1.f - cb);
        const float dd = cb <= 0.25f ? ((16.f * cb - 12.f) * cb + 4.f) * cb : std::sqrt(cb);
        const float hi = cb + (2.f * cs - 1.f) * (dd - cb);
        return cs <= 0.5f ? lo : hi;
    }
};
// Cb == 0 -> 0; Cs == 1 -> 1; else min(1, Cb / (1 - Cs)). The clamped divisor gives both special cases.
struct ColorDodgeB {
    static CANVAS_EFFECTS_ALWAYS_INLINE float f(float cb, float cs) { return std::min(1.f, cb / std::max(1.f - cs, 1e-30f)); }
};
// Cb == 1 -> 1; Cs == 0 -> 0; else 1 - min(1, (1 - Cb) / Cs).
struct ColorBurnB {
    static CANVAS_EFFECTS_ALWAYS_INLINE float f(float cb, float cs) { return 1.f - std::min(1.f, (1.f - cb) / std::max(cs, 1e-30f)); }
};

// Non-separable blend functions (Compositing 1 section 11), branch-free.
CANVAS_EFFECTS_ALWAYS_INLINE float min3(const float* c) { return std::min(std::min(c[0], c[1]), c[2]); }
CANVAS_EFFECTS_ALWAYS_INLINE float max3(const float* c) { return std::max(std::max(c[0], c[1]), c[2]); }
CANVAS_EFFECTS_ALWAYS_INLINE float lum3(const float* c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
// ClipColor: pulls an out-of-gamut colour towards its luminance. At most one of the two cases applies to the colours
// SetLum produces (they are shifted copies of in-gamut colours), and either rescales C - L by a single factor.
CANVAS_EFFECTS_ALWAYS_INLINE void clipColor3(float* c) {
    const float l = lum3(c), n = min3(c), x = max3(c);
    const float dn = l - n, dx = x - l;
    const float fn = n < 0.f ? (dn > 1e-6f ? l / dn : 0.f) : 1.f;
    const float fx = x > 1.f ? (dx > 1e-6f ? (1.f - l) / dx : 0.f) : 1.f;
    const float f = fn * fx;
    for (int i = 0; i < 3; ++i) c[i] = l + (c[i] - l) * f;
}
CANVAS_EFFECTS_ALWAYS_INLINE void setLum3(const float* c, float l, float* out) {
    const float d = l - lum3(c);
    for (int i = 0; i < 3; ++i) out[i] = c[i] + d;
    clipColor3(out);
}
CANVAS_EFFECTS_ALWAYS_INLINE float sat3(const float* c) { return max3(c) - min3(c); }
// SetSat: max -> s, min -> 0, mid -> (mid - min) s / (max - min); grey -> black. (c - min) * s / range does all three.
CANVAS_EFFECTS_ALWAYS_INLINE void setSat3(const float* c, float s, float* out) {
    const float mn = min3(c), range = max3(c) - mn;
    const float k = range > 0.f ? s / range : 0.f;
    for (int i = 0; i < 3; ++i) out[i] = (c[i] - mn) * k;
}
struct HueB {
    static CANVAS_EFFECTS_ALWAYS_INLINE void f(const float* cb, const float* cs, float* out) {
        float t[3];
        setSat3(cs, sat3(cb), t);
        setLum3(t, lum3(cb), out);
    }
};
struct SaturationB {
    static CANVAS_EFFECTS_ALWAYS_INLINE void f(const float* cb, const float* cs, float* out) {
        float t[3];
        setSat3(cb, sat3(cs), t);
        setLum3(t, lum3(cb), out);
    }
};
struct ColorB {
    static CANVAS_EFFECTS_ALWAYS_INLINE void f(const float* cb, const float* cs, float* out) { setLum3(cs, lum3(cb), out); }
};
struct LuminosityB {
    static CANVAS_EFFECTS_ALWAYS_INLINE void f(const float* cb, const float* cs, float* out) { setLum3(cb, lum3(cs), out); }
};

// co = cs (1 - ab) + cb (1 - as) + as ab B(Cb, Cs), ao = as + ab - as ab   (premultiplied in, straight Cb / Cs to B)
template <class B, bool Separable>
struct BlendOp {
    static CANVAS_EFFECTS_ALWAYS_INLINE void px(const float* cs, float as, const float* cb, float ab, float* co, float& ao) {
        const float ias = as > 0.f ? 1.f / as : 0.f;
        const float iab = ab > 0.f ? 1.f / ab : 0.f;
        float Cs[3], Cb[3], Bv[3];
        for (int c = 0; c < 3; ++c) {
            Cs[c] = clamp01(cs[c] * ias);
            Cb[c] = clamp01(cb[c] * iab);
        }
        if constexpr (Separable) {
            for (int c = 0; c < 3; ++c) Bv[c] = clamp01(B::f(Cb[c], Cs[c]));
        } else {
            B::f(Cb, Cs, Bv);
            for (int c = 0; c < 3; ++c) Bv[c] = clamp01(Bv[c]);
        }
        const float k = as * ab;
        for (int c = 0; c < 3; ++c) co[c] = cs[c] * (1.f - ab) + cb[c] * (1.f - as) + k * Bv[c];
        ao = as + ab - k;
    }
};

// Composites through planar float chunks so the per-op arithmetic vectorises: load (AoS bytes -> premultiplied
// planes), operate, lerp by clip coverage and convert back to straight bytes.
template <class Op>
inline void compositeChunked(Color* dst, const std::uint8_t* src, const std::uint8_t* clip, int count) {
    constexpr int kChunk = 64;
    constexpr float k255 = 1.f / 255.f;
    alignas(16) float cs[3][kChunk], as[kChunk], cb[3][kChunk], ab[kChunk], co[3][kChunk], ao[kChunk], cov[kChunk];
    std::uint8_t* d = reinterpret_cast<std::uint8_t*>(dst);
    for (int base = 0; base < count; base += kChunk) {
        const int n = std::min(kChunk, count - base);
        const std::uint8_t* s = src + 4 * static_cast<std::ptrdiff_t>(base);
        std::uint8_t* o = d + 4 * static_cast<std::ptrdiff_t>(base);
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) cs[c][i] = static_cast<float>(s[4 * i + c]) * k255;
            as[i] = static_cast<float>(s[4 * i + 3]) * k255;
            const float a = static_cast<float>(o[4 * i + 3]) * k255;
            for (int c = 0; c < 3; ++c) cb[c][i] = static_cast<float>(o[4 * i + c]) * (a * k255);
            ab[i] = a;
        }
        if (clip) {
            for (int i = 0; i < n; ++i) cov[i] = static_cast<float>(clip[base + i]) * k255;
        } else {
            for (int i = 0; i < n; ++i) cov[i] = 1.f;
        }
        for (int i = 0; i < n; ++i) {
            const float s3[3] = {cs[0][i], cs[1][i], cs[2][i]};
            const float b3[3] = {cb[0][i], cb[1][i], cb[2][i]};
            float o3[3], oa;
            Op::px(s3, as[i], b3, ab[i], o3, oa);
            for (int c = 0; c < 3; ++c) co[c][i] = o3[c];
            ao[i] = oa;
        }
        for (int i = 0; i < n; ++i) {
            const float t = cov[i];
            const float ra = ab[i] + (ao[i] - ab[i]) * t;
            const float a255 = ra * 255.f + 0.5f;
            const float A = a255 >= 256.f ? 255.f : (a255 < 1.f ? 0.f : a255);
            const float inv = A >= 1.f ? 255.f / ra : 0.f;
            for (int c = 0; c < 3; ++c) {
                const float rc = cb[c][i] + (co[c][i] - cb[c][i]) * t;
                float v = rc * inv + 0.5f;
                v = v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
                o[4 * i + c] = static_cast<std::uint8_t>(v);
            }
            o[4 * i + 3] = static_cast<std::uint8_t>(A);
        }
    }
}

}  // namespace effects_impl

inline void compositeSpan(CompositeOp op, Color* dst, const std::uint8_t* src, const std::uint8_t* clip, int count) {
    using namespace effects_impl;
    if (!dst || !src || count <= 0) return;
    switch (op) {
        case CompositeOp::SourceOver: sourceOverSpan(dst, src, clip, count); return;
        // Porter-Duff factor codes: 0 -> 0, 1 -> 1, 2 -> (ab | as), 3 -> 1 - (ab | as)
        case CompositeOp::SourceIn: compositeChunked<PorterDuff<2, 0>>(dst, src, clip, count); return;
        case CompositeOp::SourceOut: compositeChunked<PorterDuff<3, 0>>(dst, src, clip, count); return;
        case CompositeOp::SourceAtop: compositeChunked<PorterDuff<2, 3>>(dst, src, clip, count); return;
        case CompositeOp::DestinationOver: compositeChunked<PorterDuff<3, 1>>(dst, src, clip, count); return;
        case CompositeOp::DestinationIn: compositeChunked<PorterDuff<0, 2>>(dst, src, clip, count); return;
        case CompositeOp::DestinationOut: compositeChunked<PorterDuff<0, 3>>(dst, src, clip, count); return;
        case CompositeOp::DestinationAtop: compositeChunked<PorterDuff<3, 2>>(dst, src, clip, count); return;
        case CompositeOp::Copy: compositeChunked<PorterDuff<1, 0>>(dst, src, clip, count); return;
        case CompositeOp::Xor: compositeChunked<PorterDuff<3, 3>>(dst, src, clip, count); return;
        case CompositeOp::Lighter: compositeChunked<LighterOp>(dst, src, clip, count); return;
        case CompositeOp::Multiply: compositeChunked<MultiplyOp>(dst, src, clip, count); return;
        case CompositeOp::Screen: compositeChunked<ScreenOp>(dst, src, clip, count); return;
        case CompositeOp::Overlay: compositeChunked<BlendOp<OverlayB, true>>(dst, src, clip, count); return;
        case CompositeOp::Darken: compositeChunked<DarkenOp>(dst, src, clip, count); return;
        case CompositeOp::Lighten: compositeChunked<LightenOp>(dst, src, clip, count); return;
        case CompositeOp::ColorDodge: compositeChunked<BlendOp<ColorDodgeB, true>>(dst, src, clip, count); return;
        case CompositeOp::ColorBurn: compositeChunked<BlendOp<ColorBurnB, true>>(dst, src, clip, count); return;
        case CompositeOp::HardLight: compositeChunked<BlendOp<HardLightB, true>>(dst, src, clip, count); return;
        case CompositeOp::SoftLight: compositeChunked<BlendOp<SoftLightB, true>>(dst, src, clip, count); return;
        case CompositeOp::Difference: compositeChunked<DifferenceOp>(dst, src, clip, count); return;
        case CompositeOp::Exclusion: compositeChunked<ExclusionOp>(dst, src, clip, count); return;
        case CompositeOp::Hue: compositeChunked<BlendOp<HueB, false>>(dst, src, clip, count); return;
        case CompositeOp::Saturation: compositeChunked<BlendOp<SaturationB, false>>(dst, src, clip, count); return;
        case CompositeOp::Color: compositeChunked<BlendOp<ColorB, false>>(dst, src, clip, count); return;
        case CompositeOp::Luminosity: compositeChunked<BlendOp<LuminosityB, false>>(dst, src, clip, count); return;
    }
}

inline Color compositePixel(CompositeOp op, Color dst, const std::uint8_t srcPremul[4]) {
    compositeSpan(op, &dst, srcPremul, nullptr, 1);
    return dst;
}

}  // namespace canvas_detail

#undef CANVAS_EFFECTS_ALWAYS_INLINE
