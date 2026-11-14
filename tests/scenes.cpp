// Conformance scenes: the same C++ drawn by the native rasterizer and, built
// with emscripten, by the browser's own canvas. Compared by tools/compare.py.
#include "canvas.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Every scene is drawn on a 200x150 canvas.
struct Scene { const char* name; std::function<void(Canvas&)> draw; };

static void sprite(Canvas& s) {
    s.setFillStyle(Color{30, 90, 200}); s.fillRect(0, 0, 32, 32);
    s.setFillStyle(Color{250, 200, 40}); s.fillCircle(16, 16, 11);
    s.setFillStyle(Color{200, 30, 60}); s.fillRect(0, 0, 8, 8);
    s.setFillStyle(Color{20, 160, 60}); s.fillRect(24, 24, 8, 8);
}

std::vector<Scene> scenes() {
    std::vector<Scene> v;
    v.push_back({"rects_aa", [](Canvas& c) {
        for (int i = 0; i < 6; ++i) {
            c.setFillStyle(Color{static_cast<std::uint8_t>(40 * i), 80, 200, static_cast<std::uint8_t>(255 - 30 * i)});
            c.fillRect(10.3f + i * 30.17f, 10.6f + i * 3.3f, 20.4f, 40.25f + i);
        }
        c.setFillStyle("rgba(200, 30, 30, 0.5)"); c.fillRect(5.5f, 80.5f, 190, 20);
        c.setFillStyle("hsl(120deg 60% 40%)"); c.fillRect(20, 110.1f, 160.7f, 0.6f);
        c.setFillStyle("rebeccapurple"); c.fillRect(20, 120, 160, 10.5f);
    }});
    v.push_back({"paths_fill", [](Canvas& c) {
        c.beginPath();
        for (int i = 0; i < 5; ++i) { const float t = -1.5708f + i * 2 * 6.2832f / 5; i ? c.lineTo(50 + 40 * std::cos(t), 75 + 40 * std::sin(t)) : c.moveTo(50 + 40 * std::cos(t), 75 + 40 * std::sin(t)); }
        c.closePath(); c.setFillStyle(Color{220, 120, 20}); c.fill();
        c.beginPath();
        for (int i = 0; i < 5; ++i) { const float t = -1.5708f + i * 2 * 6.2832f / 5; i ? c.lineTo(140 + 40 * std::cos(t), 75 + 40 * std::sin(t)) : c.moveTo(140 + 40 * std::cos(t), 75 + 40 * std::sin(t)); }
        c.closePath(); c.setFillStyle(Color{20, 120, 220}); c.fill("evenodd");
        c.beginPath(); c.moveTo(10, 140); c.bezierCurveTo(40, 100, 80, 160, 110, 120); c.quadraticCurveTo(150, 90, 190, 140); c.closePath();
        c.setFillStyle(Color{40, 160, 90, 180}); c.fill();
    }});
    v.push_back({"strokes", [](Canvas& c) {
        const char* caps[] = {"butt", "round", "square"};
        const char* joins[] = {"miter", "round", "bevel"};
        for (int i = 0; i < 3; ++i) {
            c.setLineCap(caps[i]); c.setLineJoin(joins[i]); c.setLineWidth(9);
            c.setStrokeStyle(Color{30, static_cast<std::uint8_t>(60 * i), 140});
            c.beginPath(); c.moveTo(15 + i * 62, 60); c.lineTo(35 + i * 62, 15); c.lineTo(55 + i * 62, 60); c.stroke();
        }
        c.setLineWidth(1); c.setStrokeStyle(Color{0, 0, 0});
        c.beginPath(); c.moveTo(10.5f, 80.5f); c.lineTo(190.5f, 80.5f); c.stroke();
        c.setLineWidth(3.5f); c.setLineJoin("miter"); c.setMiterLimit(2);
        c.beginPath(); c.moveTo(20, 140); c.lineTo(60, 95); c.lineTo(70, 140); c.lineTo(120, 100); c.lineTo(130, 140); c.lineTo(185, 135); c.stroke();
        c.setLineWidth(0.4f); c.beginPath(); c.moveTo(10, 145); c.lineTo(190, 120); c.stroke();
    }});
    v.push_back({"dashes", [](Canvas& c) {
        c.setStrokeStyle(Color{200, 40, 40}); c.setLineWidth(4);
        c.setLineDash({12, 6}); c.beginPath(); c.moveTo(10, 20); c.lineTo(190, 20); c.stroke();
        c.setLineDash({2, 4, 8}); c.setLineDashOffset(5); c.beginPath(); c.moveTo(10, 40); c.lineTo(190, 40); c.stroke();
        c.setLineCap("round"); c.setLineDash({0, 10}); c.beginPath(); c.moveTo(10, 60); c.lineTo(190, 60); c.stroke();
        c.setLineCap("butt"); c.setLineDash({15, 5}); c.setLineDashOffset(0); c.setStrokeStyle(Color{40, 80, 200}); c.setLineWidth(3);
        c.strokeCircle(100, 110, 32);
    }});
    v.push_back({"arcs", [](Canvas& c) {
        c.setStrokeStyle(Color{20, 20, 20}); c.setLineWidth(3);
        c.beginPath(); c.arc(40, 40, 28, 0.3f, 2.8f); c.stroke();
        c.beginPath(); c.arc(110, 40, 28, 0.3f, 2.8f, true); c.stroke();
        c.beginPath(); c.ellipse(170, 40, 25, 12, 0.6f, 0, 6.2832f); c.setFillStyle(Color{90, 170, 220}); c.fill(); c.stroke();
        // arcTo: a rounded corner (the old native backend drew a chamfer).
        c.setStrokeStyle(Color{200, 30, 90}); c.setLineWidth(4);
        c.beginPath(); c.moveTo(20, 140); c.arcTo(20, 80, 90, 80, 30); c.arcTo(160, 80, 160, 140, 20); c.lineTo(160, 140); c.stroke();
        // lineTo after closePath starts at the closed subpath's start.
        c.setStrokeStyle(Color{20, 140, 40}); c.setLineWidth(2);
        c.beginPath(); c.moveTo(100, 145); c.lineTo(120, 120); c.lineTo(140, 145); c.closePath(); c.lineTo(190, 100); c.stroke();
    }});
    v.push_back({"roundrect", [](Canvas& c) {
        c.setFillStyle(Color{240, 150, 30});
        c.beginPath(); c.roundRect(10, 10, 80, 60, 14); c.fill();
        c.beginPath(); c.roundRect(110, 10, 80, 60, {Path2D::CornerRadius(4), Path2D::CornerRadius(20), Path2D::CornerRadius(30, 10), Path2D::CornerRadius(0)}); c.fill();
        c.setStrokeStyle(Color{30, 30, 120}); c.setLineWidth(3);
        c.beginPath(); c.roundRect(90, 80, -70, 60, {Path2D::CornerRadius(25), Path2D::CornerRadius(5)}); c.stroke();
        c.beginPath(); c.roundRect(110, 85, 80, 55, 200); c.stroke();
    }});
    v.push_back({"transforms", [](Canvas& c) {
        for (int i = 0; i < 6; ++i) {
            c.save(); c.translate(30 + i * 28.f, 75); c.rotate(i * 0.4f); c.scale(1 + i * 0.1f, 0.6f + i * 0.08f);
            c.setFillStyle(Color{static_cast<std::uint8_t>(40 * i), 100, 180}); c.fillRect(-12, -30, 24, 60);
            c.restore();
        }
        c.save(); c.transform(1, 0.3f, -0.4f, 1, 60, 110); c.setStrokeStyle(Color{200, 40, 40}); c.setLineWidth(4); c.strokeRect(0, 0, 80, 30); c.restore();
    }});
    v.push_back({"clips", [](Canvas& c) {
        c.save(); c.beginPath(); c.rect(20, 20, 70, 110); c.clip();
        c.setFillStyle(Color{40, 120, 220}); c.fillCircle(55, 75, 60);
        c.beginPath(); c.arc(55, 75, 30, 0, 6.2832f); c.clip();
        c.setFillStyle(Color{240, 200, 40}); c.fillRect(0, 0, 200, 150);
        c.restore();
        c.save(); c.beginPath(); c.moveTo(110, 20); c.lineTo(190, 50); c.lineTo(130, 140); c.closePath(); c.clip();
        c.setFillStyle(Color{200, 50, 80}); c.fillRect(100, 10, 100, 140);
        c.setStrokeStyle(Color{0, 0, 0}); c.setLineWidth(6); c.strokeCircle(150, 80, 30);
        c.restore();
    }});
    v.push_back({"alpha", [](Canvas& c) {
        c.setGlobalAlpha(0.5f);
        c.setFillStyle(Color{255, 0, 0}); c.fillCircle(70, 70, 45);
        c.setFillStyle(Color{0, 0, 255}); c.fillCircle(120, 70, 45);
        c.setGlobalAlpha(0.3f); c.setFillStyle(Color{0, 160, 0, 128}); c.fillRect(30, 90, 140, 40);
    }});
    v.push_back({"gradients", [](Canvas& c) {
        CanvasGradient lin = c.createLinearGradient(10, 0, 90, 0);
        lin.addColorStop(0, Color{255, 0, 0}); lin.addColorStop(0.5f, Color{255, 255, 0}); lin.addColorStop(1, Color{0, 0, 255});
        c.setFillStyle(lin); c.fillRect(10, 10, 80, 60);
        CanvasGradient rad = c.createRadialGradient(150, 40, 5, 140, 45, 40);
        rad.addColorStop(0, "white"); rad.addColorStop(0.6f, "rgba(0,128,0,0.8)"); rad.addColorStop(1, "transparent");
        c.setFillStyle(rad); c.fillRect(100, 0, 100, 80);
        CanvasGradient con = c.createConicGradient(0.5f, 50, 115);
        con.addColorStop(0, "#f00"); con.addColorStop(0.33f, "#0f0"); con.addColorStop(0.66f, "#00f"); con.addColorStop(1, "#f00");
        c.setFillStyle(con); c.fillCircle(50, 115, 30);
        CanvasGradient hard = c.createLinearGradient(110, 0, 190, 0);
        hard.addColorStop(0.5f, Color{0, 0, 0}); hard.addColorStop(0.5f, Color{255, 255, 255});
        c.setStrokeStyle(hard); c.setLineWidth(10); c.strokeRect(115, 95, 70, 40);
    }});
    v.push_back({"patterns", [](Canvas& c) {
        Canvas tile = Canvas::createVirtual(32, 32);
        sprite(tile);
        CanvasPattern rep = c.createPattern(tile, "repeat");
        c.setFillStyle(rep); c.fillRect(5, 5, 90, 70);
        CanvasPattern rx = c.createPattern(tile, "repeat-x");
        rx.setTransform(0.5f, 0, 0, 0.5f, 105, 10);
        c.setFillStyle(rx); c.fillRect(100, 5, 95, 70);
        CanvasPattern none = c.createPattern(tile, "no-repeat");
        none.setTransform(1.5f, 0.3f, -0.3f, 1.5f, 40, 85);
        c.setFillStyle(none); c.fillRect(0, 80, 200, 70);
    }});
    v.push_back({"composite", [](Canvas& c) {
        const char* ops[] = {"source-over", "source-in", "source-out", "source-atop", "destination-over", "destination-in", "destination-out",
                             "destination-atop", "lighter", "copy", "xor", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
                             "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity"};
        for (int i = 0; i < 26; ++i) {
            const float x = static_cast<float>((i % 7) * 28 + 2), y = static_cast<float>((i / 7) * 36 + 3);
            c.save(); c.beginPath(); c.rect(x, y, 26, 34); c.clip();
            c.setFillStyle(Color{30, 150, 220, 200}); c.fillRect(x + 2, y + 2, 16, 16);
            c.setGlobalCompositeOperation(ops[i]);
            c.setFillStyle(Color{230, 60, 50, 180}); c.fillCircle(x + 17, y + 17, 9);
            c.restore();
        }
    }});
    v.push_back({"shadows", [](Canvas& c) {
        c.setShadow(Color{0, 0, 0, 160}, 8, 5, 5);
        c.setFillStyle(Color{240, 180, 40}); c.fillRect(20, 20, 60, 40);
        c.setShadow("rgba(200, 0, 0, 0.8)", 3, -4, 6);
        c.setFillStyle(Color{40, 140, 220}); c.fillCircle(140, 45, 25);
        c.setShadow(Color{0, 0, 120, 255}, 0, 4, 4);
        c.setStrokeStyle(Color{30, 160, 60}); c.setLineWidth(5); c.strokeRect(30, 90, 140, 40);
    }});
    v.push_back({"filters", [](Canvas& c) {
        const char* filters[] = {"blur(2px)", "grayscale(1)", "sepia(0.8)", "hue-rotate(90deg)", "invert(0.9)", "contrast(1.8)", "drop-shadow(3px 3px 2px rgb(0 0 0 / 0.6))", "brightness(1.5) saturate(2)"};
        for (int i = 0; i < 8; ++i) {
            c.setFilter(filters[i]);
            const float x = static_cast<float>((i % 4) * 50 + 5), y = static_cast<float>((i / 4) * 75 + 5);
            c.setFillStyle(Color{220, 90, 40}); c.fillRect(x + 4, y + 4, 36, 30);
            c.setFillStyle(Color{40, 120, 210}); c.fillCircle(x + 22, y + 50, 14);
        }
    }});
    v.push_back({"text", [](Canvas& c) {
        c.setFillStyle(Color{20, 20, 30});
        c.setFont("16px sans-serif"); c.fillText("Hello, World! AVATAR", 8, 22);
        c.setFont("bold 20px serif"); c.fillText("Serif Bold", 8, 48);
        c.setFont("italic 13px sans-serif"); c.setTextAlign("center"); c.setTextBaseline("middle"); c.fillText("centered middle", 100, 70);
        c.setTextAlign("right"); c.setTextBaseline("top"); c.setFont("12px monospace"); c.fillText("mono right", 192, 82);
        c.setTextAlign("left"); c.setTextBaseline("alphabetic");
        c.setFont("28px sans-serif"); c.setStrokeStyle(Color{200, 30, 60}); c.setLineWidth(1.5f); c.strokeText("Stroke", 8, 135);
        c.setFont("14px sans-serif"); c.fillText("squeezed into 60px wide", 110, 135, 60);
    }});
    v.push_back({"text_light", [](Canvas& c) {
        // Light text on dark: the other end of the text contrast ramp.
        c.setFillStyle(Color{20, 24, 36}); c.fillRect(0, 0, 200, 150);
        c.setFillStyle(Color{255, 255, 255});
        c.setFont("16px sans-serif"); c.fillText("Hello, World! AVATAR", 8, 22);
        c.setFont("bold 20px serif"); c.fillText("Serif Bold", 8, 48);
        c.setFont("13px sans-serif"); c.fillText("small light text 0123", 8, 70);
        c.setFillStyle(Color{250, 210, 60}); c.setFont("bold 14px sans-serif"); c.fillText("Warm label", 8, 92);
        c.setFillStyle(Color{140, 150, 170}); c.setFont("12px sans-serif"); c.fillText("mid-grey caption text", 8, 112);
    }});
    v.push_back({"text_metrics", [](Canvas& c) {
        c.setFont("18px sans-serif");
        const char* words[] = {"Width", "of words", "measured", "here"};
        float y = 25;
        for (const char* w : words) {
            const TextMetrics m = c.measureTextMetrics(w);
            c.setFillStyle(Color{200, 220, 255}); c.fillRect(10, y - m.actualBoundingBoxAscent, m.width, m.actualBoundingBoxAscent + m.actualBoundingBoxDescent);
            c.setFillStyle(Color{0, 0, 0}); c.fillText(w, 10, y);
            y += 32;
        }
    }});
    v.push_back({"images", [](Canvas& c) {
        Canvas s = Canvas::createVirtual(32, 32);
        sprite(s);
        c.drawCanvas(s, 5, 5);
        c.drawCanvas(s, 45, 5, 64, 64);
        c.setImageSmoothingEnabled(false); c.drawCanvas(s, 115, 5, 64, 64); c.setImageSmoothingEnabled(true);
        c.drawCanvas(s, 8, 8, 16, 16, 5, 80, 50, 50);
        c.save(); c.translate(140, 110); c.rotate(0.6f); c.drawCanvas(s, -20, -20, 40, 40); c.restore();
        c.drawCanvas(s, 70, 85, 20, 20);
        std::vector<std::uint8_t> px(16 * 16 * 4);
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) { std::uint8_t* p = &px[(y * 16 + x) * 4]; p[0] = static_cast<std::uint8_t>(x * 16); p[1] = static_cast<std::uint8_t>(y * 16); p[2] = 128; p[3] = static_cast<std::uint8_t>((x + y) % 2 ? 255 : 120); }
        c.drawImage(px.data(), 16, 16, 95, 75, 32, 32, 0.8f, 77);
    }});
    v.push_back({"pixels", [](Canvas& c) {
        c.setFillStyle(Color{30, 130, 200}); c.fillRect(10, 10, 100, 80);
        std::vector<std::uint8_t> got = c.getImageData(20, 20, 40, 30);
        for (std::size_t i = 0; i < got.size(); i += 4) { got[i] = 255 - got[i]; got[i + 3] = 160; }
        c.putImageData(got, 40, 30, 120, 40);
        c.putImageData(got, 40, 30, 120, 90, 10, 5, 20, 20);
        c.clearRect(30.5f, 60.5f, 50, 20);
        c.save(); c.translate(150, 20); c.rotate(0.3f); c.clearRect(-5, -5, 30, 10); c.restore();
    }});
    v.push_back({"hittest", [](Canvas& c) {
        Path2D blob("M 20 20 C 80 0 120 60 60 90 Z");
        c.setFillStyle(Color{220, 230, 250}); c.fill(blob);
        c.setLineWidth(8);
        for (int y = 0; y < 150; y += 6) for (int x = 0; x < 200; x += 6) {
            const bool in = c.isPointInPath(blob, static_cast<float>(x), static_cast<float>(y));
            const bool onStroke = c.isPointInStroke(blob, static_cast<float>(x), static_cast<float>(y));
            if (in || onStroke) { c.setFillStyle(onStroke ? Color{200, 40, 40} : Color{40, 40, 200}); c.fillRect(static_cast<float>(x) - 1, static_cast<float>(y) - 1, 2, 2); }
        }
    }});
    v.push_back({"culling", [](Canvas& c) {
        // Everything here is hidden by what follows, then a panel is drawn
        // over part of it: the visible result must not depend on culling.
        for (int i = 0; i < 40; ++i) { c.setFillStyle(Color{static_cast<std::uint8_t>(i * 6), 30, 80}); c.fillCircle(static_cast<float>(i * 5), 75, 30); }
        c.clear(Color{250, 250, 250});
        for (int i = 0; i < 40; ++i) { c.setFillStyle(Color{30, static_cast<std::uint8_t>(i * 6), 80}); c.fillRect(static_cast<float>(i * 5), static_cast<float>(i * 3), 30, 30); }
        c.setFillStyle(Color{10, 10, 10}); c.fillRect(0, 0, 200, 150);
        c.setFillStyle(Color{200, 200, 30}); c.fillCircle(100, 75, 40);
        c.setFillStyle(Color{240, 240, 240}); c.fillRect(20, 20, 160, 50);
        c.setFillStyle(Color{30, 30, 200, 128}); c.fillRect(60, 40, 80, 60);
        c.beginPath(); c.roundRect(10, 100, 180, 45, 10); c.setFillStyle(Color{0, 90, 0}); c.fill();
        c.setFillStyle(Color{255, 255, 255}); c.setFont("14px sans-serif"); c.fillText("on top", 80, 128);
    }});
    v.push_back({"layers", [](Canvas& c) {
        c.setGlobalAlpha(0.5f);
        c.beginLayer();
        c.setFillStyle(Color{220, 40, 40}); c.fillCircle(80, 75, 45);
        c.setFillStyle(Color{40, 40, 220}); c.fillCircle(120, 75, 45);
        c.endLayer();
        c.setGlobalAlpha(1);
        c.setStrokeStyle(Color{0, 0, 0}); c.strokeRect(10, 10, 180, 130);
    }});
    v.push_back({"svgpath", [](Canvas& c) {
        Path2D p("M10 80 Q 52.5 10, 95 80 T 180 80 L 180 140 A 30 20 20 1 0 30 140 Z");
        c.setFillStyle(Color{120, 60, 200}); c.fill(p);
        Path2D q;
        q.addPath(p, {0.5f, 0, 0, 0.5f, 100, 0});
        c.setStrokeStyle(Color{0, 150, 0}); c.setLineWidth(2); c.stroke(q);
    }});
    return v;
}
