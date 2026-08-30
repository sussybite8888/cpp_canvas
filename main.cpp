#include "canvas.h"
#include "svg.h"

#include <iostream>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

namespace {
struct WebAnimation {
    Canvas* canvas;
    std::function<void(Canvas&, float)> draw;
    double startMilliseconds;
};

void renderWebFrame(void* argument) {
    auto& animation = *static_cast<WebAnimation*>(argument);
    animation.draw(*animation.canvas,
                   static_cast<float>((emscripten_get_now() - animation.startMilliseconds) / 1000.0));
}
} // namespace
#endif

int main(int argc, char** argv) {
    const auto drawDemo = [](Canvas& canvas, float animationTime) {
    canvas.clear(Color{247, 249, 252});

    canvas.setFillStyle(Color{35, 99, 235});
    canvas.fillRect(70, 60, 220, 120);
    canvas.setStrokeStyle(Color{15, 23, 42});
    canvas.setLineWidth(6);
    canvas.strokeRect(70, 60, 220, 120);

    canvas.setFillStyle(Color{249, 115, 22, 210});
    canvas.fillCircle(445, 190, 85);
    canvas.setStrokeStyle(Color{127, 29, 29});
    canvas.setLineWidth(8);
    canvas.strokeCircle(445, 190, 85);

    canvas.beginPath();
    canvas.moveTo(90, 390);
    canvas.lineTo(230, 270);
    canvas.lineTo(370, 390);
    canvas.lineTo(510, 290);
    canvas.lineTo(710, 390);
    canvas.setStrokeStyle(Color{22, 163, 74});
    canvas.setLineWidth(12);
    canvas.stroke();

    // A retained Path2D can be reused on any context.
    Path2D star;
    star.moveTo(640, 70); star.lineTo(660, 125); star.lineTo(720, 125);
    star.lineTo(672, 160); star.lineTo(690, 218); star.lineTo(640, 184);
    star.lineTo(590, 218); star.lineTo(608, 160); star.lineTo(560, 125);
    star.lineTo(620, 125); star.closePath();
    canvas.setFillStyle(Color{234, 179, 8});
    canvas.fill(star);

    // Virtual canvases are offscreen in the browser and independent buffers natively.
    static Canvas badge = [] {
        auto context = Canvas::createVirtual(90, 90);
        context.clear(Color{0, 0, 0, 0});
        context.setFillStyle(Color{168, 85, 247});
        context.fillCircle(45, 45, 40);
        return context;
    }();
    canvas.drawCanvas(badge, 665, 285);

    // SVG is compiled to the same Canvas commands; its animation is evaluated
    // at this timestamp rather than delegated to the browser SVG engine.
    const auto svg = SvgDocument::fromString(R"(
      <svg width="180" height="100">
        <rect x="580" y="400" width="70" height="70" rx="12" fill="#14b8a6">
          <animate attributeName="x" from="580" to="680" dur="2s" repeatCount="indefinite"/>
        </rect>
      </svg>)");
    svg.render(canvas, animationTime);

    // SMIL animateTransform is evaluated by the SVG compiler, then rasterized
    // as normal Canvas paths. These two examples share the same timeline.
    const auto smil = SvgDocument::fromString(R"(
      <svg width="800" height="500">
        <polygon points="0,-32 9,-10 32,-10 14,4 21,28 0,14 -21,28 -14,4 -32,-10 -9,-10"
                 fill="#f43f5e" stroke="#881337" stroke-width="3">
          <animateTransform attributeName="transform" type="rotate" from="0 0 0" to="360 0 0" dur="3s" repeatCount="indefinite"/>
        </polygon>
        <path d="M -28 -20 L 8 -36 L 36 -20 L 0 -3 Z" fill="#60a5fa" stroke="#172554" stroke-width="2">
          <animateTransform attributeName="transform" type="rotate" from="0 0 0" to="360 0 0" dur="4s" repeatCount="indefinite"/>
        </path>
        <path d="M -28 -20 L 0 -3 L 0 36 L -28 18 Z" fill="#2563eb" stroke="#172554" stroke-width="2">
          <animateTransform attributeName="transform" type="rotate" from="0 0 0" to="360 0 0" dur="4s" repeatCount="indefinite"/>
        </path>
        <path d="M 0 -3 L 36 -20 L 36 18 L 0 36 Z" fill="#1d4ed8" stroke="#172554" stroke-width="2">
          <animateTransform attributeName="transform" type="rotate" from="0 0 0" to="360 0 0" dur="4s" repeatCount="indefinite"/>
        </path>
      </svg>)");
    canvas.save(); canvas.translate(365, 295); smil.render(canvas, animationTime); canvas.restore();
    };

    Canvas canvas(800, 500);
    drawDemo(canvas, 0.75f);

#ifdef __EMSCRIPTEN__
    // Browser equivalent of the SDL loop: Emscripten schedules this on the
    // browser's animation frame cadence, making SVG SMIL continuously animate.
    auto* animation = new WebAnimation{&canvas, drawDemo, emscripten_get_now()};
    emscripten_set_main_loop_arg(renderWebFrame, animation, 0, true);
#else
#ifndef __EMSCRIPTEN__
    if (!canvas.savePPM("canvas-demo.ppm")) {
        std::cerr << "Could not save canvas-demo.ppm\n";
        return 1;
    }
    std::cout << "Wrote canvas-demo.ppm\n";
    if (argc > 1 && std::string(argv[1]) == "--window")
        return canvas.showWindow("C++ Canvas — SVG SMIL demo", drawDemo) ? 0 : 1;
#endif
#endif
    return 0;
}
