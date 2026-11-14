#pragma once
// The software backend's scan converter and its thread pool.
//
// Coverage is EXACT AREA: every pixel row is cut into slabs at each edge
// endpoint (and at each crossing of two edges) that falls inside it, so within
// a slab every active edge is a straight line spanning it top to bottom and no
// two edges swap order. The fill rule is resolved per slab by walking the
// edges in x order, and only the edges that toggle inside/outside contribute:
// each adds the exact trapezoid area to its right into a per-row difference
// array (the font-rs / stb_truetype v2 accumulation). Overlapping pieces --
// the quads and wedges a stroke is built from, self-intersecting fills --
// therefore union exactly instead of double counting at their edges, which is
// what plain signed-area accumulation gets wrong. A row whose geometry is too
// busy for slabs (dozens of vertices inside one pixel row) falls back to
// eight point-sampled lines with exact horizontal coverage.
//
// Output is a sequence of RUNS of constant coverage per row, left to right,
// so the interior of a shape costs one call per row however wide it is.
//
// Private to the library; included only by the native backend's TU.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "geometry.h"

namespace canvas_detail {
namespace {

// ---------------------------------------------------------------------------
// Thread pool. Rows are independent, so large jobs are split into scanline
// bands; every caller writes only storage indexed by its own rows, which makes
// the split exact (bit-identical to the sequential walk).
// ---------------------------------------------------------------------------
// cx: the edge's x on the current sample line of the sampled fallback (its
// y clamped into the edge), which keeps the active list sorted line to line.
struct Edge { float x0, y0, x1, y1, dxdy, dir, cx; };

struct SlabEdge { float key, xa, xb, dir; };
struct Cross { float x, dir; };

struct FillScratch {
    std::vector<Edge> active, merge;
    std::vector<float> acc;      // per-column coverage deltas, width + 2
    std::vector<int> touched;    // columns with a delta this row
    std::vector<unsigned char> mark;
    std::vector<float> breaks;
    std::vector<SlabEdge> slab;
    std::vector<Cross> cross;
};

class BandPool {
public:
    static BandPool& instance() { static BandPool pool; return pool; }
    int maxBands() const { return bands_; }
    // True while a job is out: a job that splits its own work again runs
    // that split on its own thread.
    bool busy() const { return running_.load(std::memory_order_relaxed); }
    // Runs job(0) on the calling thread and job(1..bands-1) on the workers,
    // each band claimed by whichever thread is free next, returning once
    // every band is done. Not reentrant: a nested call runs in place.
    void run(int bands, const std::function<void(int)>& job) {
        if (bands <= 1 || busy()) { for (int b = 0; b < std::max(1, bands); ++b) job(b); return; }
        running_.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            bandCount_ = bands;
            next_.store(1, std::memory_order_relaxed);
            remaining_.store(bands - 1, std::memory_order_relaxed);
            ++generation_;
        }
        wake_.notify_all();
        job(0);
        // The calling thread helps with whatever is left.
        for (;;) {
            const int band = next_.fetch_add(1, std::memory_order_relaxed);
            if (band >= bands) break;
            job(band);
            remaining_.fetch_sub(1, std::memory_order_acq_rel);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        // Both: a worker decrements remaining_ while still inside its claim
        // loop, and must leave it before the job it holds can be destroyed.
        idle_.wait(lock, [this] { return remaining_.load(std::memory_order_acquire) == 0 && inLoop_ == 0; });
        job_ = nullptr;
        running_.store(false, std::memory_order_relaxed);
    }

private:
    BandPool() {
        const unsigned hardware = std::thread::hardware_concurrency();
        bands_ = static_cast<int>(std::min(hardware ? hardware : 1u, 8u));
        if (bands_ < 1) bands_ = 1;
        for (int i = 1; i < bands_; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~BandPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            ++generation_;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_) if (worker.joinable()) worker.join();
    }
    void loop() {
        std::uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this, seen] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            const std::function<void(int)>* const job = job_;
            const int bands = bandCount_;
            if (!job) continue;
            ++inLoop_;
            lock.unlock();
            for (;;) {
                const int band = next_.fetch_add(1, std::memory_order_relaxed);
                if (band >= bands) break;
                (*job)(band);
                remaining_.fetch_sub(1, std::memory_order_acq_rel);
            }
            lock.lock();
            --inLoop_;
            idle_.notify_all();
        }
    }
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    const std::function<void(int)>* job_ = nullptr;
    std::atomic<int> next_{0}, remaining_{0};
    std::uint64_t generation_ = 0;
    int bands_ = 1, bandCount_ = 0, inLoop_ = 0;
    std::atomic<bool> running_{false};
    bool stop_ = false;
};

// Below these a job stays on the calling thread: waking a worker costs more
// than a small band's work.
constexpr int kMinBandRows = 24;
constexpr long long kMinThreadedArea = 1 << 15;

// Splits rows [y0, y1) of a job of the given pixel area across the pool.
// `costPerPixel` scales the area: a pixel that is sampled and filtered is
// worth several plain ones, so smaller jobs of that kind are split too.
template <class RowsFn>
void parallelRows(int y0, int y1, long long area, RowsFn&& rows, int costPerPixel = 1) {
    BandPool& pool = BandPool::instance();
    const int height = y1 - y0;
    const int minRows = costPerPixel > 1 ? kMinBandRows / 2 : kMinBandRows;
    int bands = 1;
    if (area * costPerPixel >= kMinThreadedArea && height >= 2 * minRows && !pool.busy()) bands = std::max(1, std::min(pool.maxBands(), height / minRows));
    if (bands <= 1) { rows(y0, y1, 0); return; }
    pool.run(bands, [&](int band) {
        rows(y0 + static_cast<int>(static_cast<long long>(height) * band / bands),
             y0 + static_cast<int>(static_cast<long long>(height) * (band + 1) / bands), band);
    });
}

// ---------------------------------------------------------------------------
// Exact-area scan conversion.
// ---------------------------------------------------------------------------
// Slabs cost one sort per vertex in the row, sampling one per sample line, so
// past a handful of vertices in one pixel row the samples are cheaper. Glyph
// masks, rasterized once and cached, pass a higher limit for exactness.
constexpr int kMaxSlabBreaks = 7;
constexpr int kMaxSlabBreaksExact = 32;
constexpr int kFallbackSamples = 8;
constexpr float kBreakMerge = 1.0f / 512;

// Slab pieces in a total order, so equal keys never depend on the order the
// active list happens to hold its edges in (which the sampled rows change).
inline bool slabLess(const SlabEdge& a, const SlabEdge& b) {
    if (a.key != b.key) return a.key < b.key;
    if (a.xa != b.xa) return a.xa < b.xa;
    if (a.xb != b.xb) return a.xb < b.xb;
    return a.dir < b.dir;
}
inline void insertionSort(SlabEdge* s, std::size_t n) {
    for (std::size_t i = 1; i < n; ++i) {
        const SlabEdge v = s[i];
        std::size_t j = i;
        while (j > 0 && slabLess(v, s[j - 1])) { s[j] = s[j - 1]; --j; }
        s[j] = v;
    }
}
// Edges by cx. Linear on a list that is already nearly in order -- the
// active list from one sample line to the next.
inline void insertionSortByCx(Edge* s, std::size_t n) {
    for (std::size_t i = 1; i < n; ++i) {
        if (!(s[i].cx < s[i - 1].cx)) continue;
        const Edge v = s[i];
        std::size_t j = i;
        while (j > 0 && s[j - 1].cx > v.cx) { s[j] = s[j - 1]; --j; }
        s[j] = v;
    }
}
inline void sortByCx(Edge* s, std::size_t n) {
    if (n <= 24) insertionSortByCx(s, n);
    else std::sort(s, s + n, [](const Edge& a, const Edge& b) { return a.cx < b.cx; });
}

struct RowAccumulator {
    float* acc;
    unsigned char* mark;
    std::vector<int>* touched;
    int width;  // columns 0..width-1 are in the box; width and width+1 are guards
    int lo = 0, hi = -1;  // the columns spanDense wrote, which keeps no list
    void touch(int c) {
        if (!mark[c]) { mark[c] = 1; touched->push_back(c); }
    }
    // A straight edge piece across one slab: x from xa (top) to xb (bottom), in
    // column space [0, width], contributing `d` (= signed slab height) to every
    // column wholly to its right, and the exact trapezoid fraction to the
    // columns it passes through.
    void line(float xa, float xb, float d) {
        float x0 = std::min(xa, xb), x1 = std::max(xa, xb);
        const float w = static_cast<float>(width);
        x0 = std::clamp(x0, 0.f, w); x1 = std::clamp(x1, 0.f, w);
        const int c0 = static_cast<int>(x0);
        if (x1 - x0 < 1e-6f || static_cast<int>(x1) == c0 || (x1 == std::floor(x1) && static_cast<int>(x1) == c0 + 1)) {
            // Within one column: the area right of the line there is the
            // column's width minus the line's mean offset into it.
            const float f = std::clamp(0.5f * (x0 + x1) - c0, 0.f, 1.f);
            acc[c0] += d * (1 - f); touch(c0);
            acc[c0 + 1] += d * f; touch(c0 + 1);
            return;
        }
        // F(x): area to the right of the line integrated over (-inf, x].
        const float span = x1 - x0, k = d / (2 * span);
        const auto F = [&](float x) {
            if (x <= x0) return 0.f;
            if (x >= x1) return d * (0.5f * span + (x - x1));
            const float t = x - x0;
            return k * t * t;
        };
        const int c1 = std::min(static_cast<int>(x1), width);
        float prevS = 0, prevF = F(static_cast<float>(c0));
        for (int c = c0; c <= c1 + 1 && c <= width + 1; ++c) {
            const float nextF = F(static_cast<float>(c + 1));
            const float S = nextF - prevF;
            acc[c] += S - prevS; touch(c);
            prevS = S; prevF = nextF;
        }
    }
    // The same, with x clamped to [0, width] piecewise rather than at the
    // endpoints: a line leaving the box is split where it crosses the side.
    void clampedLine(float xa, float xb, float h, float sign) {
        const float w = static_cast<float>(width);
        if (xa >= 0 && xb >= 0 && xa <= w && xb <= w) { line(xa, xb, h * sign); return; }
        float ts[4] = {0, 1, 1, 1};
        int n = 1;
        if (xb != xa) {
            const float tl = (0 - xa) / (xb - xa), tr = (w - xa) / (xb - xa);
            if (tl > 0 && tl < 1) ts[n++] = tl;
            if (tr > 0 && tr < 1) ts[n++] = tr;
        }
        ts[n++] = 1;
        std::sort(ts, ts + n);
        for (int i = 0; i + 1 < n; ++i) {
            const float t0 = ts[i], t1 = ts[i + 1];
            if (t1 - t0 <= 0) continue;
            const float p0 = std::clamp(xa + (xb - xa) * t0, 0.f, w), p1 = std::clamp(xa + (xb - xa) * t1, 0.f, w);
            line(p0, p1, h * (t1 - t0) * sign);
        }
    }
    // A horizontal span [a, b) at weight `wgt` (the sampled fallback).
    void span(float a, float b, float wgt) {
        const float w = static_cast<float>(width);
        a = std::clamp(a, 0.f, w); b = std::clamp(b, 0.f, w);
        if (!(b > a)) return;
        const int ia = static_cast<int>(a), ib = static_cast<int>(b);
        const float fa = a - ia, fb = b - ib;
        acc[ia] += wgt * (1 - fa); touch(ia);
        acc[ia + 1] += wgt * fa; touch(ia + 1);
        acc[ib] -= wgt * (1 - fb); touch(ib);
        acc[ib + 1] -= wgt * fb; touch(ib + 1);
    }
    // span() for a busy row, whose columns are resolved by walking [lo, hi]
    // rather than through the touched list.
    void spanDense(float a, float b, float wgt) {
        const float w = static_cast<float>(width);
        a = std::clamp(a, 0.f, w); b = std::clamp(b, 0.f, w);
        if (!(b > a)) return;
        const int ia = static_cast<int>(a), ib = static_cast<int>(b);
        const float fa = a - ia, fb = b - ib;
        acc[ia] += wgt * (1 - fa);
        acc[ia + 1] += wgt * fa;
        acc[ib] -= wgt * (1 - fb);
        acc[ib + 1] -= wgt * fb;
        lo = std::min(lo, ia); hi = std::max(hi, ib + 1);
    }
};

template <class Run>
void scanFill(const Poly& poly, bool evenOdd, int cx0, int cy0, int cx1, int cy1, Run&& run, int maxBreaks = kMaxSlabBreaks) {
    static thread_local std::vector<Edge> edges, byRow;
    static thread_local std::vector<int> rowStart, cursor;
    int bx0, by0, bx1, by1;
    if (!polyBounds(poly, cx0, cy0, cx1, cy1, bx0, by0, bx1, by1)) return;
    edges.clear();
    int first = 0;
    const float left = static_cast<float>(bx0);
    for (std::size_t c = 0; c < poly.ends.size(); ++c) {
        const int last = poly.ends[c];
        for (int i = first; i < last; ++i) {
            const int j = (i + 1 == last) ? first : i + 1;
            const float ax = poly.pts[2 * i], ay = poly.pts[2 * i + 1], bx = poly.pts[2 * j], by = poly.pts[2 * j + 1];
            if (ay == by || !(std::isfinite(ax) && std::isfinite(ay) && std::isfinite(bx) && std::isfinite(by))) continue;
            // Columns relative to the box, so the accumulator indexes from 0.
            if (ay < by) edges.push_back({ax - left, ay, bx - left, by, (bx - ax) / (by - ay), 1.f, 0.f});
            else edges.push_back({bx - left, by, ax - left, ay, (ax - bx) / (ay - by), -1.f, 0.f});
        }
        first = last;
    }
    if (edges.empty()) return;
    const int rows = by1 - by0;
    const float boxTop = static_cast<float>(by0), boxBottom = static_cast<float>(by1);
    const auto firstRow = [&](const Edge& e) -> int {
        if (e.y1 <= boxTop) return -1;
        if (!(e.y0 < boxBottom)) return -1;
        if (e.y0 <= boxTop) return 0;
        return static_cast<int>(std::floor(e.y0)) - by0;
    };
    rowStart.assign(static_cast<std::size_t>(rows) + 1, 0);
    for (const Edge& e : edges) { const int r = firstRow(e); if (r >= 0) ++rowStart[static_cast<std::size_t>(r)]; }
    int total = 0;
    for (int r = 0; r <= rows; ++r) { const int n = rowStart[static_cast<std::size_t>(r)]; rowStart[static_cast<std::size_t>(r)] = total; total += n; }
    if (total == 0) return;
    byRow.resize(static_cast<std::size_t>(total));
    cursor.assign(rowStart.begin(), rowStart.end());
    for (const Edge& e : edges) { const int r = firstRow(e); if (r >= 0) byRow[static_cast<std::size_t>(cursor[static_cast<std::size_t>(r)]++)] = e; }

    const int width = bx1 - bx0;
    BandPool& pool = BandPool::instance();
    const long long area = static_cast<long long>(rows) * width;
    int bandCount = 1;
    if (area >= kMinThreadedArea && rows >= 2 * kMinBandRows && !pool.busy()) bandCount = std::max(1, std::min(pool.maxBands(), rows / kMinBandRows));
    const std::vector<Edge>& rowEdges = byRow;
    const std::vector<int>& starts = rowStart;

    const auto runBand = [&](int band) {
        // Per thread: a band runs on one thread at a time, and the flush's
        // own bands run fills on every thread at once.
        static thread_local FillScratch scratch;
        FillScratch& sc = scratch;
        std::vector<Edge>& active = sc.active;
        const int ys = by0 + static_cast<int>(static_cast<long long>(rows) * band / bandCount);
        const int ye = by0 + static_cast<int>(static_cast<long long>(rows) * (band + 1) / bandCount);
        if (ys >= ye) return;
        if (sc.acc.size() < static_cast<std::size_t>(width) + 3) { sc.acc.assign(static_cast<std::size_t>(width) + 3, 0.f); sc.mark.assign(static_cast<std::size_t>(width) + 3, 0); }
        RowAccumulator ra{sc.acc.data(), sc.mark.data(), &sc.touched, width};
        sc.touched.clear();
        active.clear();
        for (int r = 0; r < ys - by0; ++r)
            for (int k = starts[static_cast<std::size_t>(r)]; k < starts[static_cast<std::size_t>(r) + 1]; ++k)
                if (rowEdges[static_cast<std::size_t>(k)].y1 > static_cast<float>(ys)) active.push_back(rowEdges[static_cast<std::size_t>(k)]);

        const auto inside = [evenOdd](int w) { return evenOdd ? (w & 1) != 0 : w != 0; };
        // One slab [ta, tb] in which every listed edge spans the whole height.
        // Endpoints were merged within kBreakMerge, so "spans" allows that much.
        constexpr float tol = kBreakMerge * 1.01f;
        const auto doSlab = [&](float ta, float tb, int depth, auto& self) -> void {
            const float h = tb - ta;
            if (!(h > 1e-7f)) return;
            std::vector<SlabEdge>& se = sc.slab;
            se.clear();
            for (const Edge& e : active) {
                if (e.y0 > ta + tol || e.y1 < tb - tol) continue;
                const float xa = e.x0 + (std::max(ta, e.y0) - e.y0) * e.dxdy;
                const float xb = e.x0 + (std::min(tb, e.y1) - e.y0) * e.dxdy;
                se.push_back({xa + xb, xa, xb, e.dir});
            }
            const std::size_t n = se.size();
            if (n < 2) return;
            if (n <= 32) insertionSort(se.data(), n);
            else std::sort(se.begin(), se.end(), slabLess);
            if (depth < 4) {
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    const SlabEdge& a = se[i];
                    const SlabEdge& b = se[i + 1];
                    if (a.xa > b.xa + 1e-3f || a.xb > b.xb + 1e-3f) {
                        // Two edges swap order inside the slab: split at the
                        // crossing so each half has a fixed order.
                        const float den = (a.xb - a.xa) - (b.xb - b.xa);
                        float t = std::abs(den) > 1e-9f ? (b.xa - a.xa) / den : 0.5f;
                        if (!(t > 0.02f && t < 0.98f)) t = 0.5f;
                        const float ym = ta + h * t;
                        self(ta, ym, depth + 1, self);
                        self(ym, tb, depth + 1, self);
                        return;
                    }
                }
            }
            int w = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const bool before = inside(w);
                w += static_cast<int>(se[i].dir);
                const bool after = inside(w);
                if (before != after) ra.clampedLine(se[i].xa, se[i].xb, h, after ? 1.f : -1.f);
            }
        };

        // True while the active list is in cx order from the row above, so the
        // next sampled row only has to place the edges it adds.
        bool sorted = false;
        for (int y = ys; y < ye; ++y) {
            const float top = static_cast<float>(y), bottom = top + 1;
            std::size_t keep = 0;
            for (std::size_t i = 0; i < active.size(); ++i) if (active[i].y1 > top) active[keep++] = active[i];
            active.resize(keep);
            for (int k = starts[static_cast<std::size_t>(y - by0)]; k < starts[static_cast<std::size_t>(y - by0) + 1]; ++k)
                active.push_back(rowEdges[static_cast<std::size_t>(k)]);
            if (active.size() < 2) continue;
            std::vector<float>& br = sc.breaks;
            br.clear();
            const std::size_t breakCap = static_cast<std::size_t>(maxBreaks) * 2;
            for (const Edge& e : active) {
                if (e.y0 > top && e.y0 < bottom) br.push_back(e.y0);
                if (e.y1 > top && e.y1 < bottom) br.push_back(e.y1);
                if (br.size() > breakCap) break;  // the samples, whatever the rest are
            }
            if (br.size() <= breakCap) {
                std::sort(br.begin(), br.end());
                std::size_t u = 0;
                float lastBreak = top;
                for (float b : br) if (b - lastBreak > kBreakMerge && bottom - b > kBreakMerge) { br[u++] = b; lastBreak = b; }
                br.resize(u);
            }
            if (br.size() <= static_cast<std::size_t>(maxBreaks)) {
                float ta = top;
                for (float b : br) { doSlab(ta, b, 0, doSlab); ta = b; }
                doSlab(ta, bottom, 0, doSlab);
                sorted = false;
            } else {
                // Too busy for slabs: point-sample a few lines, exact across.
                // The active list is kept sorted by each edge's x on the
                // current line (y clamped into the edge, so an edge that
                // misses the line still has a place in the order): from one
                // line to the next the order barely changes, and one
                // insertion pass restores it where a sort per line was most
                // of what a busy stroke cost.
                const float wgt = 1.f / kFallbackSamples;
                const std::size_t n = active.size();
                Edge* const ae = active.data();
                const auto place = [](Edge& e, float sy) { e.cx = e.x0 + (std::clamp(sy, e.y0, e.y1) - e.y0) * e.dxdy; };
                const float sy0 = top + 0.5f * wgt;
                for (std::size_t i = 0; i < n; ++i) place(ae[i], sy0);
                if (sorted) insertionSortByCx(ae, keep); else sortByCx(ae, keep);
                if (n > keep) {
                    // The edges this row added, sorted on their own and
                    // merged in.
                    sortByCx(ae + keep, n - keep);
                    if (keep > 0 && ae[keep].cx < ae[keep - 1].cx) {
                        std::vector<Edge>& m = sc.merge;
                        m.resize(n);
                        std::merge(ae, ae + keep, ae + keep, ae + n, m.data(), [](const Edge& a, const Edge& b) { return a.cx < b.cx; });
                        std::copy(m.begin(), m.end(), ae);
                    }
                }
                if (sc.cross.size() < n) sc.cross.resize(n);
                Cross* const xs = sc.cross.data();
                ra.lo = width + 2; ra.hi = -1;
                for (int s = 0; s < kFallbackSamples; ++s) {
                    const float sy = top + (s + 0.5f) * wgt;
                    if (s > 0) {
                        for (std::size_t i = 0; i < n; ++i) place(ae[i], sy);
                        insertionSortByCx(ae, n);
                    }
                    // The crossings this line actually has, in x order --
                    // collected without a branch, since which edges reach
                    // the line is not something a predictor learns.
                    std::size_t nx = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        xs[nx] = Cross{ae[i].cx, ae[i].dir};
                        nx += (sy >= ae[i].y0 && sy < ae[i].y1) ? 1u : 0u;
                    }
                    int w = 0;
                    for (std::size_t i = 0; i + 1 < nx; ++i) {
                        w += static_cast<int>(xs[i].dir);
                        if (inside(w)) ra.spanDense(xs[i].x, xs[i + 1].x, wgt);
                    }
                }
                sorted = true;
                // Resolve the row over the columns the spans reached: a
                // column with no change continues the run to its left.
                float sum = 0;
                int prev = -1;
                for (int c = ra.lo; c <= ra.hi; ++c) {
                    const float d = sc.acc[static_cast<std::size_t>(c)];
                    if (d == 0) continue;
                    if (prev >= 0 && prev < width) {
                        const int end = std::min(c, width);
                        float cov = sum;
                        if (cov > 0.9998f) cov = 1.f;
                        if (cov > 0.0015f && end > prev) run(bx0 + prev, bx0 + end, y, std::min(cov, 1.f));
                    }
                    sum += d;
                    sc.acc[static_cast<std::size_t>(c)] = 0;
                    prev = c;
                }
                if (prev >= 0 && prev < width) {
                    float cov = sum;
                    if (cov > 0.9998f) cov = 1.f;
                    if (cov > 0.0015f) run(bx0 + prev, bx0 + width, y, std::min(cov, 1.f));
                }
                continue;
            }
            // Resolve the row: between touched columns the coverage is constant.
            std::vector<int>& t = sc.touched;
            if (t.empty()) continue;
            float sum = 0;
            const auto emitRun = [&](int c, int next) {
                if (c >= width) return;
                const int end = std::min(next, width);
                float cov = sum;
                if (cov > 0.9998f) cov = 1.f;
                if (cov > 0.0015f && end > c) run(bx0 + c, bx0 + end, y, std::min(cov, 1.f));
            };
            if (t.size() > 32) {
                // Many touched columns: walk their range instead of sorting.
                int lo = t[0], hi = t[0];
                for (const int c : t) { lo = std::min(lo, c); hi = std::max(hi, c); }
                if (static_cast<std::size_t>(hi - lo) <= t.size() * 8) {
                    int prev = -1;
                    for (int c = lo; c <= hi; ++c) {
                        if (!sc.mark[static_cast<std::size_t>(c)]) continue;
                        if (prev >= 0) emitRun(prev, c);
                        sum += sc.acc[static_cast<std::size_t>(c)];
                        sc.acc[static_cast<std::size_t>(c)] = 0;
                        sc.mark[static_cast<std::size_t>(c)] = 0;
                        prev = c;
                    }
                    emitRun(prev, width);
                    t.clear();
                    continue;
                }
                std::sort(t.begin(), t.end());
            } else {
                for (std::size_t i = 1; i < t.size(); ++i) { const int v = t[i]; std::size_t j = i; while (j > 0 && t[j - 1] > v) { t[j] = t[j - 1]; --j; } t[j] = v; }
            }
            for (std::size_t i = 0; i < t.size(); ++i) {
                const int c = t[i];
                sum += sc.acc[static_cast<std::size_t>(c)];
                sc.acc[static_cast<std::size_t>(c)] = 0;
                sc.mark[static_cast<std::size_t>(c)] = 0;
                emitRun(c, i + 1 < t.size() ? t[i + 1] : width);
            }
            t.clear();
        }
    };
    if (bandCount > 1) pool.run(bandCount, runBand); else runBand(0);
}

// ---------------------------------------------------------------------------
// Pixel arithmetic. Pixels are straight-alpha RGBA8.
// ---------------------------------------------------------------------------
// x / 255, rounded, for x in [0, 255*255].
inline std::uint32_t div255(std::uint32_t x) { x += 128; return (x + (x >> 8)) >> 8; }

// Source-over of a straight colour at source alpha `sa` (0..255, coverage and
// global alpha already folded in) onto one pixel.
inline void blendOver(Color& d, Color c, std::uint32_t sa) {
    if (sa == 0) return;
    if (sa >= 255) { d = Color{c.r, c.g, c.b, 255}; return; }
    if (d.a == 255) {
        const std::uint32_t keep = 255 - sa;
        d.r = static_cast<std::uint8_t>(div255(c.r * sa + d.r * keep));
        d.g = static_cast<std::uint8_t>(div255(c.g * sa + d.g * keep));
        d.b = static_cast<std::uint8_t>(div255(c.b * sa + d.b * keep));
        return;
    }
    if (d.a == 0) { d = Color{c.r, c.g, c.b, static_cast<std::uint8_t>(sa)}; return; }
    // Translucent onto translucent: straight alpha needs the divide.
    const std::uint32_t da = d.a, keep = div255(da * (255 - sa));
    const std::uint32_t oa = sa + keep;
    const auto ch = [&](std::uint32_t s, std::uint32_t dc) { return static_cast<std::uint8_t>((s * sa + dc * keep + oa / 2) / oa); };
    d = Color{ch(c.r, d.r), ch(c.g, d.g), ch(c.b, d.b), static_cast<std::uint8_t>(oa)};
}

// A run of one colour at one source alpha. The opaque-destination case is the
// whole frame once a background is down, and is written to vectorise.
inline void blendRunOver(Color* d, int n, Color c, std::uint32_t sa) {
    if (sa == 0 || n <= 0) return;
    if (sa >= 255) { const Color o{c.r, c.g, c.b, 255}; std::fill(d, d + n, o); return; }
    const std::uint32_t keep = 255 - sa, r = c.r * sa + 128, g = c.g * sa + 128, b = c.b * sa + 128;
    int i = 0;
    while (i < n) {
        if (d[i].a == 255) {
            int j = i;
            while (j < n && d[j].a == 255) ++j;
            for (int k = i; k < j; ++k) {
                const std::uint32_t xr = d[k].r * keep + r, xg = d[k].g * keep + g, xb = d[k].b * keep + b;
                d[k].r = static_cast<std::uint8_t>((xr + (xr >> 8)) >> 8);
                d[k].g = static_cast<std::uint8_t>((xg + (xg >> 8)) >> 8);
                d[k].b = static_cast<std::uint8_t>((xb + (xb >> 8)) >> 8);
            }
            i = j;
        } else {
            blendOver(d[i], c, sa);
            ++i;
        }
    }
}

// Source-over at a 16-bit source alpha (0..65535). Used whenever a
// fractional globalAlpha is involved: rounding it to 8 bits first (0.99 ->
// 252/255) is an error that compounds layer over layer -- a hundred nested
// opacity groups drift by a tenth of the range -- where the browser keeps
// its alpha at higher precision.
inline void blendOver16(Color& d, Color c, std::uint32_t sa) {
    if (sa == 0) return;
    if (sa >= 65535) { d = Color{c.r, c.g, c.b, 255}; return; }
    if (d.a == 255) {
        const std::uint32_t keep = 65535 - sa;
        d.r = static_cast<std::uint8_t>((c.r * sa + d.r * keep + 32767) / 65535);
        d.g = static_cast<std::uint8_t>((c.g * sa + d.g * keep + 32767) / 65535);
        d.b = static_cast<std::uint8_t>((c.b * sa + d.b * keep + 32767) / 65535);
        return;
    }
    const float s = sa * (1.f / 65535.f), da = d.a * (1.f / 255.f), keep = da * (1 - s), oa = s + keep;
    if (oa <= 0) { d = Color{0, 0, 0, 0}; return; }
    const auto ch = [&](std::uint8_t sc, std::uint8_t dc) { return static_cast<std::uint8_t>(std::min(255.f, (sc * s + dc * keep) / oa + 0.5f)); };
    d = Color{ch(c.r, d.r), ch(c.g, d.g), ch(c.b, d.b), static_cast<std::uint8_t>(std::min(255.f, oa * 255.f + 0.5f))};
}
inline void blendRunOver16(Color* d, int n, Color c, std::uint32_t sa) {
    if (sa >= 65535) { blendRunOver(d, n, c, 255); return; }
    for (int i = 0; i < n; ++i) blendOver16(d[i], c, sa);
}

// Source-over of a premultiplied pixel (pr,pg,pb,pa all 0..255) onto a pixel.
inline void blendPremulOver(Color& d, std::uint32_t pr, std::uint32_t pg, std::uint32_t pb, std::uint32_t pa) {
    if (pa == 0) return;
    if (pa >= 255) { d = Color{static_cast<std::uint8_t>(std::min(pr, 255u)), static_cast<std::uint8_t>(std::min(pg, 255u)), static_cast<std::uint8_t>(std::min(pb, 255u)), 255}; return; }
    const std::uint32_t keep = 255 - pa;
    if (d.a == 255) {
        d.r = static_cast<std::uint8_t>(std::min(255u, pr + div255(d.r * keep)));
        d.g = static_cast<std::uint8_t>(std::min(255u, pg + div255(d.g * keep)));
        d.b = static_cast<std::uint8_t>(std::min(255u, pb + div255(d.b * keep)));
        return;
    }
    const std::uint32_t da = div255(d.a * keep);
    const std::uint32_t oa = pa + da;
    if (oa == 0) { d = Color{0, 0, 0, 0}; return; }
    const auto ch = [&](std::uint32_t s, std::uint32_t dc) {
        return static_cast<std::uint8_t>(std::min(255u, (s * 255 + dc * da + oa / 2) / oa));
    };
    d = Color{ch(pr, d.r), ch(pg, d.g), ch(pb, d.b), static_cast<std::uint8_t>(std::min(oa, 255u))};
}

}  // namespace
}  // namespace canvas_detail
