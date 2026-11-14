// cpp_canvas demo: the same C++ draws natively (software rasterizer, SDL
// window) and in a browser (the page's Canvas2D).
//
//   ./canvas_demo            writes canvas-demo.ppm
//   ./canvas_demo --window   opens an interactive window (move the mouse)
#include "canvas.h"
#include "svg.h"
#include "window.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace {
constexpr int kWidth = 800, kHeight = 500;

const SvgDocument& spinner() {
    static const SvgDocument doc = SvgDocument::fromString(R"(
      <svg width="80" height="80" viewBox="-40 -40 80 80">
        <polygon points="0,-32 9,-10 32,-10 14,4 21,28 0,14 -21,28 -14,4 -32,-10 -9,-10"
                 fill="#f43f5e" stroke="#881337" stroke-width="3">
          <animateTransform attributeName="transform" type="rotate" from="0" to="360" dur="3s" repeatCount="indefinite"/>
        </polygon>
      </svg>)");
    return doc;
}

const Canvas& tile() {
    static Canvas t = [] {
        Canvas c = Canvas::createVirtual(24, 24);
        c.setFillStyle(Color{226, 232, 240}); c.fillRect(0, 0, 24, 24);
        c.setFillStyle(Color{203, 213, 225}); c.fillRect(0, 0, 12, 12); c.fillRect(12, 12, 12, 12);
        return c;
    }();
    return t;
}

void drawScene(Canvas& c, float t, float mouseX, float mouseY, const std::string& stats) {
    // Background: a pattern under a translucent gradient.
    c.setFillStyle(c.createPattern(tile(), "repeat"));
    c.fillRect(0, 0, kWidth, kHeight);
    CanvasGradient sky = c.createLinearGradient(0, 0, 0, kHeight);
    sky.addColorStop(0, "rgb(30 64 175 / 0.85)");
    sky.addColorStop(1, "rgb(147 197 253 / 0.35)");
    c.setFillStyle(sky);
    c.fillRect(0, 0, kWidth, kHeight);

    // A card with a soft shadow and per-corner radii.
    c.save();
    c.setShadow("rgba(15, 23, 42, 0.45)", 24, 0, 10);
    c.setFillStyle("#f8fafc");
    c.beginPath(); c.roundRect(40, 40, 330, 200, {Path2D::CornerRadius(28), Path2D::CornerRadius(6)}); c.fill();
    c.restore();
    c.setFillStyle("#0f172a"); c.setFont("bold 26px sans-serif"); c.fillText("cpp_canvas", 66, 92);
    c.setFont("15px sans-serif"); c.setFillStyle("#334155");
    c.fillText("One Canvas 2D API: native and browser.", 66, 122);
    c.fillText("Gradients, patterns, shadows, filters,", 66, 146);
    c.fillText("blend modes, layers, kerned text.", 66, 168);
    c.setFont("12px monospace"); c.setFillStyle("#64748b"); c.fillText(stats, 66, 214);

    // Radial gradient orbs blended with `screen`, following the pointer.
    c.save();
    c.setGlobalCompositeOperation("screen");
    for (int i = 0; i < 3; ++i) {
        const float a = t * 0.9f + i * 2.094f;
        const float x = mouseX + std::cos(a) * 70, y = mouseY + std::sin(a) * 70;
        CanvasGradient orb = c.createRadialGradient(x, y, 4, x, y, 90);
        const Color colours[3] = {Color{244, 63, 94}, Color{34, 197, 94}, Color{59, 130, 246}};
        orb.addColorStop(0, colours[i]);
        orb.addColorStop(1, Color{colours[i].r, colours[i].g, colours[i].b, 0});
        c.setFillStyle(orb);
        c.fillCircle(x, y, 90);
    }
    c.restore();

    // A dashed, animated stroke and a conic gradient wheel.
    c.save();
    c.setLineWidth(6); c.setLineCap("round"); c.setLineDash({18, 12}); c.setLineDashOffset(-t * 40);
    c.setStrokeStyle("#fbbf24");
    c.beginPath();
    for (int i = 0; i <= 60; ++i) c.lineTo(420 + i * 6.f, 420 + 40 * std::sin(i * 0.25f + t * 2));
    c.stroke();
    c.restore();
    CanvasGradient wheel = c.createConicGradient(t, 640, 150);
    for (int i = 0; i <= 6; ++i) {
        const float h = i / 6.f;
        wheel.addColorStop(h, "hsl(" + std::to_string(static_cast<int>(h * 360)) + " 90% 55%)");
    }
    c.setFillStyle(wheel); c.fillCircle(640, 150, 80);

    // Filters on a layer: one blur and drop shadow for the group.
    c.save();
    c.setFilter("drop-shadow(0 6px 6px rgb(0 0 0 / 0.4))");
    c.beginLayer();
    c.setFillStyle("#a855f7"); c.fillCircle(150, 360, 60);
    c.setFillStyle("#ec4899"); c.fillCircle(210, 360, 60);
    c.endLayer();
    c.restore();

    // SMIL animation compiled to Canvas calls.
    spinner().renderFitted(c, 300, 300, 120, 120, t);
}

std::string statsLine(double ms) {
    const CanvasFrameStats st = canvasTakeFrameStats();
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d ops, %d culled, %.2f ms", st.ops, st.culled, ms);
    return buf;
}

struct App {
    Window window;
    double started = 0;
    std::string stats = "";
    bool frame() {
        if (!window.pump()) return false;
        Canvas& c = window.canvas();
        c.setTransform(window.uiScale(), 0, 0, window.uiScale(), 0, 0);
        const float x = window.mouseX() > 0 ? window.mouseX() : kWidth * 0.6f;
        const float y = window.mouseY() > 0 ? window.mouseY() : kHeight * 0.55f;
        const double t0 = window.timeSeconds();
        drawScene(c, static_cast<float>(t0), x, y, stats);
        window.present();
        stats = statsLine((window.timeSeconds() - t0) * 1000);
        return true;
    }
};
}  // namespace

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    (void)argc; (void)argv;
    static App app;
    std::string error;
    if (!app.window.open(kWidth, kHeight, "cpp_canvas", error)) { std::printf("%s\n", error.c_str()); return 1; }
    app.window.setDesignSize(kWidth, kHeight);
    emscripten_set_main_loop_arg([](void* p) { static_cast<App*>(p)->frame(); }, &app, 0, true);
    return 0;
#else
    if (argc > 1 && std::string(argv[1]) == "--window") {
        App app;
        std::string error;
        if (!app.window.open(kWidth, kHeight, "cpp_canvas demo", error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        app.window.setDesignSize(kWidth, kHeight);
        while (app.frame()) app.window.frameDelay(60);
        return 0;
    }
    Canvas canvas(kWidth, kHeight);
    drawScene(canvas, 0.75f, kWidth * 0.6f, kHeight * 0.55f, "");
    if (!canvas.savePPM("canvas-demo.ppm")) { std::fprintf(stderr, "Could not save canvas-demo.ppm\n"); return 1; }
    std::printf("Wrote canvas-demo.ppm (%s)\n", statsLine(0).c_str());
    return 0;
#endif
}
