// Native tests: behaviour the browser has and the software backend must
// match, and the guarantee culling makes -- the pixels never change.
#include "canvas.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

struct Scene { const char* name; std::function<void(Canvas&)> draw; };
std::vector<Scene> scenes();  // tests/scenes.cpp

namespace {
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Px { int r, g, b, a; };
Px at(Canvas& c, int x, int y) {
    const auto p = c.getImageData(x, y, 1, 1);
    return {p[0], p[1], p[2], p[3]};
}
bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }
Canvas blank(int w = 64, int h = 64) {
    Canvas c = Canvas::createVirtual(w, h);
    c.clear(Color{255, 255, 255});
    return c;
}

void testCullingIsExact() {
    for (const Scene& s : scenes()) {
        Canvas a = Canvas::createVirtual(200, 150), b = Canvas::createVirtual(200, 150);
        b.setCulling(false);
        a.clear(Color{255, 255, 255}); b.clear(Color{255, 255, 255});
        s.draw(a); s.draw(b);
        const bool same = a.getImageData(0, 0, 200, 150) == b.getImageData(0, 0, 200, 150);
        if (!same) std::printf("  scene %s differs with culling\n", s.name);
        CHECK(same);
    }
}
void testCullingDropsHiddenWork() {
    Canvas c = Canvas::createVirtual(256, 256);
    canvasTakeFrameStats();
    for (int i = 0; i < 100; ++i) { c.setFillStyle(Color{200, 10, 10}); c.fillCircle(static_cast<float>(i * 2), 100, 20); }
    c.setFillStyle(Color{0, 0, 0}); c.fillRect(0, 0, 256, 256);   // covers everything
    c.setFillStyle(Color{0, 255, 0}); c.fillRect(10, 10, 10, 10);
    c.flush();
    const CanvasFrameStats st = canvasTakeFrameStats();
    CHECK(st.culled >= 100);
    const Px p = at(c, 50, 100), q = at(c, 15, 15);
    CHECK(p.r == 0 && p.g == 0 && p.b == 0);
    CHECK(q.g == 255 && q.r == 0);
}
void testDependenciesSurviveDeferral() {
    // A canvas drawn FROM keeps what it held at the draw, whatever is drawn
    // on it before the target flushes.
    Canvas sprite = Canvas::createVirtual(8, 8);
    sprite.setFillStyle(Color{255, 0, 0}); sprite.fillRect(0, 0, 8, 8);
    Canvas main = blank(32, 32);
    main.drawCanvas(sprite, 0, 0);
    sprite.setFillStyle(Color{0, 0, 255}); sprite.fillRect(0, 0, 8, 8);
    main.drawCanvas(sprite, 16, 0);
    const Px a = at(main, 4, 4), b = at(main, 20, 4);
    CHECK(a.r == 255 && a.b == 0);
    CHECK(b.b == 255 && b.r == 0);
}
void testBandedFlushIsExact() {
    // A frame flushed at once is drawn in horizontal bands on every core
    // (and culled); flushed after each draw, every draw runs alone. The
    // pixels are the same, including the draws that cannot be banded --
    // shadows, composite operations, reading the canvas itself, rotated text.
    Canvas sprite = Canvas::createVirtual(64, 64);
    sprite.setFillStyle(Color{200, 60, 90}); sprite.fillCircle(32, 32, 30);
    sprite.setFillStyle(Color{255, 255, 255, 180}); sprite.fillCircle(22, 22, 9);
    const Canvas* sp = &sprite;
    std::vector<std::function<void(Canvas&)>> draws;
    std::mt19937 r(11);
    const auto f = [&](int n) { return static_cast<float>(r() % static_cast<unsigned>(n)); };
    const auto col = [&](int a) { return Color{static_cast<unsigned char>(r() & 255), static_cast<unsigned char>(r() & 255), static_cast<unsigned char>(r() & 255), static_cast<unsigned char>(a)}; };
    for (int i = 0; i < 40; ++i) {
        const float x = f(640), y = f(480), s = 10 + f(90);
        const Color c = col(i % 3 ? 255 : 150);
        switch (i % 8) {
        case 0: draws.push_back([=](Canvas& k) { k.setFillStyle(c); k.fillRect(x + 0.3f, y + 0.6f, s * 2, s); }); break;
        case 1: draws.push_back([=](Canvas& k) { k.setFillStyle(c); k.fillCircle(x, y, s); }); break;
        case 2: draws.push_back([=](Canvas& k) { k.setStrokeStyle(c); k.setLineWidth(3); k.strokeCircle(x, y, s); }); break;
        case 3: draws.push_back([=](Canvas& k) { k.drawCanvas(*sp, x, y, s, s); }); break;
        case 4: draws.push_back([=](Canvas& k) { k.drawCanvas(*sp, std::floor(x), std::floor(y)); }); break;
        case 5: draws.push_back([=](Canvas& k) { k.setFillStyle(c); k.setFont("15px sans-serif"); k.fillText("Banded text 0123", x, y); }); break;
        case 6: draws.push_back([=](Canvas& k) {
            k.setStrokeStyle(c); k.setLineWidth(4); k.setLineJoin("round"); k.beginPath(); k.moveTo(x, y);
            for (int j = 1; j < 120; ++j) k.lineTo(x + j * 2.f, y + ((j * 7919) % 23) - 11.f);
            k.stroke();
        }); break;
        case 7: draws.push_back([=](Canvas& k) {
            k.save(); k.beginPath(); k.arc(x, y, s, 0, 6.2831853f); k.clip();
            CanvasGradient g = k.createLinearGradient(x - s, y, x + s, y);
            g.addColorStop(0, c); g.addColorStop(1, Color{20, 20, 200, 200});
            k.setFillStyle(g); k.fillRect(x - s, y - s, s * 2, s * 2); k.restore();
        }); break;
        }
        if (i == 12) draws.push_back([](Canvas& k) { k.clearRect(100.4f, 50.6f, 200, 150); });
        if (i == 18) draws.push_back([](Canvas& k) { k.save(); k.setShadow(Color{0, 0, 0, 160}, 8, 4, 4); k.setFillStyle(Color{240, 200, 40}); k.fillRect(300, 200, 120, 80); k.restore(); });
        if (i == 24) draws.push_back([](Canvas& k) { k.save(); k.setGlobalCompositeOperation("multiply"); k.setFillStyle(Color{90, 160, 255}); k.fillCircle(320, 240, 120); k.restore(); });
        if (i == 28) draws.push_back([](Canvas& k) { k.drawCanvas(k, 10, 10, 200, 150); });
        if (i == 32) draws.push_back([](Canvas& k) { k.save(); k.translate(400, 100); k.rotate(0.4f); k.setFillStyle(Color{0, 0, 0}); k.setFont("18px sans-serif"); k.fillText("rotated", 0, 0); k.restore(); });
        if (i == 36) draws.push_back([](Canvas& k) { std::vector<std::uint8_t> px(40 * 30 * 4, 128); k.putImageData(px, 40, 30, 500, 400); });
    }
    const auto render = [&](bool each) {
        Canvas c = Canvas::createVirtual(640, 480);
        c.clear(Color{255, 255, 255});
        for (const auto& d : draws) { d(c); if (each) c.flush(); }
        return c.getImageData(0, 0, 640, 480);
    };
    CHECK(render(false) == render(true));
}
void testStaticSourcesBatch() {
    // Drawing a canvas that has nothing pending does not flush the target,
    // so a frame of sprites is one batch.
    Canvas sprite = Canvas::createVirtual(16, 16);
    sprite.setFillStyle(Color{255, 0, 0}); sprite.fillRect(0, 0, 16, 16);
    sprite.flush();
    Canvas main = blank(256, 256);
    canvasTakeFrameStats();
    for (int i = 0; i < 50; ++i) main.drawCanvas(sprite, static_cast<float>(i * 4), static_cast<float>(i * 3), 20, 20);
    main.flush();
    CHECK(canvasTakeFrameStats().batches == 1);
    // A source changed through a layer or an immediate write is still read
    // as it was at each draw.
    Canvas a = blank(32, 32);
    a.drawCanvas(sprite, 0, 0);
    sprite.beginLayer(); sprite.setFillStyle(Color{0, 0, 255}); sprite.fillRect(0, 0, 16, 16); sprite.endLayer();
    a.drawCanvas(sprite, 16, 0);
    const std::vector<std::uint8_t> green(16 * 16 * 4, 255);
    std::vector<std::uint8_t> g = green;
    for (std::size_t i = 0; i < g.size(); i += 4) { g[i] = 0; g[i + 2] = 0; }
    a.drawCanvas(sprite, 0, 16);
    sprite.blitDevice(g.data(), 16, 16, 0, 0);
    a.drawCanvas(sprite, 16, 16);
    const Px p0 = at(a, 4, 4), p1 = at(a, 20, 4), p2 = at(a, 4, 20), p3 = at(a, 20, 20);
    CHECK(p0.r == 255 && p0.b == 0);
    CHECK(p1.b == 255 && p1.r == 0);
    CHECK(p2.b == 255 && p2.g == 0);
    CHECK(p3.g == 255 && p3.b == 0);
}
void testExactCoverage() {
    Canvas c = blank();
    c.setFillStyle(Color{0, 0, 0}); c.fillRect(10.5f, 10.25f, 20, 20);
    CHECK(near(at(c, 10, 20).r, 128, 1));   // half the pixel covered
    CHECK(near(at(c, 20, 10).r, 64, 1));    // a quarter of a row left white
    CHECK(near(at(c, 10, 10).r, 159, 2));   // 0.5 * 0.75 covered
}
void testArcToRounds() {
    Canvas c = blank();
    c.setStrokeStyle(Color{0, 0, 0}); c.setLineWidth(2);
    c.beginPath(); c.moveTo(10, 50); c.arcTo(10, 10, 50, 10, 20); c.lineTo(50, 10); c.stroke();
    // On the arc (centre (30,30), radius 20, at 225 degrees) there is ink;
    // on the chord between the tangent points there is none.
    CHECK(at(c, 16, 16).r < 128);
    CHECK(at(c, 20, 20).r > 200);
}
void testClosePathThenLineTo() {
    Canvas c = blank();
    c.setStrokeStyle(Color{0, 0, 0}); c.setLineWidth(2);
    c.beginPath(); c.moveTo(10, 10); c.lineTo(30, 10); c.lineTo(30, 30); c.closePath(); c.lineTo(10, 50); c.stroke();
    CHECK(at(c, 10, 40).r < 100);   // the new subpath starts at (10, 10)
}
void testPutImageDataReplaces() {
    Canvas c = blank(8, 8);
    std::vector<std::uint8_t> px(4 * 4 * 4, 0);  // transparent black
    c.putImageData(px, 4, 4, 2, 2);
    const Px p = at(c, 3, 3);
    CHECK(p.a == 0);
    CHECK(at(c, 0, 0).a == 255);
}
void testResetClears() {
    Canvas c = blank(8, 8);
    c.setFillStyle(Color{1, 2, 3}); c.translate(5, 5); c.reset();
    CHECK(at(c, 4, 4).a == 0);
    CHECK(c.getTransform()[4] == 0);
}
void testHitTests() {
    Canvas c = blank();
    c.setLineWidth(6);
    c.beginPath(); c.moveTo(10, 32); c.lineTo(54, 32);
    CHECK(c.isPointInStroke(30, 34));
    CHECK(!c.isPointInStroke(30, 40));
    Path2D p("M 0 0 L 20 0 L 20 20 Z");
    CHECK(c.isPointInPath(p, 15, 5));
    CHECK(!c.isPointInPath(p, 5, 15));
}
void testCssColours() {
    Canvas c = blank(4, 4);
    c.setFillStyle("rebeccapurple"); c.fillRect(0, 0, 4, 4);
    const Px p = at(c, 1, 1);
    CHECK(p.r == 102 && p.g == 51 && p.b == 153);
    c.setFillStyle("not a colour"); c.fillRect(0, 0, 4, 4);   // ignored: still purple
    CHECK(at(c, 1, 1).r == 102);
    Color k;
    CHECK(parseCssColor("hsl(0 100% 50% / 50%)", k) && k.r == 255 && k.a == 128);
}
void testGradient() {
    Canvas c = blank(100, 10);
    CanvasGradient g = c.createLinearGradient(0, 0, 100, 0);
    g.addColorStop(0, Color{0, 0, 0}); g.addColorStop(1, Color{255, 255, 255});
    c.setFillStyle(g); c.fillRect(0, 0, 100, 10);
    CHECK(near(at(c, 50, 5).r, 129, 3));
    CHECK(at(c, 2, 5).r < 12);
    // Stops added later reach later draws only.
    c.fillRect(0, 0, 10, 10);
    g.addColorStop(0, Color{255, 0, 0});
    c.fillRect(0, 0, 100, 10);
    CHECK(at(c, 1, 5).r > 200 && at(c, 1, 5).g < 10);
}
void testCompositeAndShadowAndFilter() {
    Canvas c = blank();
    c.setGlobalCompositeOperation("destination-out");
    c.setFillStyle(Color{0, 0, 0}); c.fillRect(0, 0, 32, 64);
    CHECK(at(c, 10, 10).a == 0 && at(c, 40, 10).a == 255);
    c.setGlobalCompositeOperation("source-over");
    Canvas s = blank();
    s.setShadow(Color{0, 0, 255, 255}, 0, 10, 10);
    s.setFillStyle(Color{255, 0, 0}); s.fillRect(10, 10, 20, 20);
    CHECK(at(s, 35, 35).b == 255 && at(s, 35, 35).r == 0);   // the hard shadow
    CHECK(at(s, 15, 15).r == 255);
    Canvas f = blank(8, 8);
    f.setFilter("grayscale(1)"); f.setFillStyle(Color{255, 0, 0}); f.fillRect(0, 0, 8, 8);
    const Px g = at(f, 4, 4);
    CHECK(near(g.r, 54, 1) && near(g.g, 54, 1) && near(g.b, 54, 1));
}
void testRotatedDrawCanvas() {
    Canvas src = Canvas::createVirtual(16, 16);
    src.setFillStyle(Color{0, 0, 0}); src.fillRect(0, 0, 16, 16);
    Canvas c = blank();
    c.translate(32, 32); c.rotate(0.7853982f); c.drawCanvas(src, -8, -8);
    CHECK(at(c, 32, 32).r < 10);         // the centre is covered
    CHECK(at(c, 32, 22).r < 128);        // a corner of the diamond reaches up
    CHECK(at(c, 25, 25).r > 200);        // the box's corner is not
}
void testLayers() {
    Canvas c = blank();
    c.setGlobalAlpha(0.5f);
    c.beginLayer();
    c.setFillStyle(Color{0, 0, 0}); c.fillRect(10, 10, 30, 30); c.fillRect(20, 20, 30, 30);
    c.endLayer();
    CHECK(near(at(c, 25, 25).r, 128, 1));   // the overlap is one 50% layer, not two
    CHECK(near(at(c, 15, 15).r, 128, 1));
}
void testText() {
    Canvas c = blank(200, 40);
    c.setFont("20px sans-serif");
    const TextMetrics m = c.measureTextMetrics("Hello");
    CHECK(m.width > 30 && m.width < 80);
    CHECK(m.actualBoundingBoxAscent > 10 && m.fontBoundingBoxAscent >= m.actualBoundingBoxAscent - 1);
    c.setFillStyle(Color{0, 0, 0}); c.fillText("Hello", 10, 30);
    int ink = 0;
    const auto px = c.getImageData(0, 0, 200, 40);
    for (std::size_t i = 0; i < px.size(); i += 4) ink += px[i] < 128;
    CHECK(ink > 50);
    // strokeText draws outlines, not filled glyphs.
    Canvas o = blank(200, 60);
    o.setFont("bold 48px sans-serif"); o.setStrokeStyle(Color{0, 0, 0}); o.setLineWidth(1);
    o.strokeText("O", 10, 50);
    const float w = o.measureText("O");
    CHECK(at(o, 10 + static_cast<int>(w / 2), 32).r > 200);   // the middle of the O stays clear
}
void testLineDash() {
    Canvas c = blank();
    c.setLineDash({5, 10, 15});
    CHECK(c.getLineDash().size() == 6);
    c.setLineDash({1, -1});   // invalid: ignored
    CHECK(c.getLineDash().size() == 6);
}
}  // namespace

int main() {
    testCullingIsExact();
    testCullingDropsHiddenWork();
    testDependenciesSurviveDeferral();
    testBandedFlushIsExact();
    testStaticSourcesBatch();
    testExactCoverage();
    testArcToRounds();
    testClosePathThenLineTo();
    testPutImageDataReplaces();
    testResetClears();
    testHitTests();
    testCssColours();
    testGradient();
    testCompositeAndShadowAndFilter();
    testRotatedDrawCanvas();
    testLayers();
    testText();
    testLineDash();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
