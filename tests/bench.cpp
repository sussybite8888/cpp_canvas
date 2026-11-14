// Native benchmark: canvas_bench [--nocull] [--only scene] [--reps n] [--dump dir]
// Usage: bench [--dump dir] [--only name]
#include "canvas.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

#if 0
#define FLUSH(c) ((void)0)
#else
#define FLUSH(c) (c).flush()
#endif

static const int W = 1280, H = 720;

struct Scene { const char* name; std::function<void(Canvas&)> draw; int reps; };

static void writePPM(const Canvas& c, const std::string& path) {
    auto px = c.getImageData(0, 0, W, H);
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < px.size(); i += 4) {
        const float a = px[i + 3] / 255.f;
        unsigned char rgb[3];
        for (int k = 0; k < 3; ++k) rgb[k] = (unsigned char)std::lround(px[i + k] * a + 255 * (1 - a));
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    std::string dump, only;
    bool cull = true;
    int repsOverride = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--nocull")) cull = false;
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) repsOverride = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    }
    static Canvas sprite = [] {
        Canvas s = Canvas::createVirtual(128, 128);
        s.setFillStyle(Color{200, 60, 90, 255}); s.fillCircle(64, 64, 60);
        s.setFillStyle(Color{255, 255, 255, 180}); s.fillCircle(44, 44, 18);
        s.setStrokeStyle(Color{40, 10, 20}); s.setLineWidth(6); s.strokeCircle(64, 64, 57);
        return s;
    }();
    std::vector<Scene> scenes = {
        {"fullscreen_opaque_rects", [](Canvas& c) {
            for (int i = 0; i < 20; ++i) { c.setFillStyle(Color{(unsigned char)(i * 12), 40, 90}); c.fillRect(0, 0, W, H); }
        }, 5},
        {"small_rects_1000", [](Canvas& c) {
            std::mt19937 r(1);
            for (int i = 0; i < 1000; ++i) {
                c.setFillStyle(Color{(unsigned char)(r() & 255), (unsigned char)(r() & 255), (unsigned char)(r() & 255), (unsigned char)(i % 3 ? 255 : 128)});
                c.fillRect(float(r() % W) + 0.3f, float(r() % H) + 0.6f, 5 + float(r() % 60), 5 + float(r() % 40));
            }
        }, 10},
        {"circles_500", [](Canvas& c) {
            std::mt19937 r(2);
            for (int i = 0; i < 500; ++i) {
                c.setFillStyle(Color{(unsigned char)(r() & 255), (unsigned char)(r() & 255), 120, (unsigned char)(i % 2 ? 255 : 160)});
                c.fillCircle(float(r() % W), float(r() % H), 6 + float(r() % 50));
            }
        }, 10},
        {"stroked_circles_300", [](Canvas& c) {
            std::mt19937 r(3);
            c.setLineWidth(3); c.setLineJoin("round"); c.setLineCap("round");
            for (int i = 0; i < 300; ++i) {
                c.setStrokeStyle(Color{(unsigned char)(r() & 255), 30, (unsigned char)(r() & 255)});
                c.strokeCircle(float(r() % W), float(r() % H), 6 + float(r() % 50));
            }
        }, 10},
        {"polyline_stroke", [](Canvas& c) {
            std::mt19937 r(4);
            c.setLineWidth(4); c.setLineJoin("round"); c.setLineCap("round");
            c.setStrokeStyle(Color{20, 120, 60});
            c.beginPath(); c.moveTo(10, 360);
            for (int i = 0; i < 2000; ++i) c.lineTo(10 + i * 0.63f, 360 + 200 * std::sin(i * 0.05f) + float(r() % 20));
            c.stroke();
        }, 10},
        {"curves_fill_stroke", [](Canvas& c) {
            std::mt19937 r(5);
            for (int i = 0; i < 200; ++i) {
                float x = float(r() % W), y = float(r() % H);
                c.beginPath(); c.moveTo(x, y);
                c.bezierCurveTo(x + 80, y - 60, x + 120, y + 90, x + 30, y + 120);
                c.quadraticCurveTo(x - 50, y + 60, x, y);
                c.setFillStyle(Color{90, (unsigned char)(r() & 255), 200, 200}); c.fill();
                c.setStrokeStyle(Color{10, 10, 10}); c.setLineWidth(2); c.stroke();
            }
        }, 10},
        {"text_40_lines", [](Canvas& c) {
            c.setFillStyle(Color{20, 20, 30}); c.setFont("16px sans-serif");
            for (int i = 0; i < 40; ++i) c.fillText("The quick brown fox jumps over the lazy dog 0123456789", 10, 20 + i * 17.5f);
        }, 5},
        {"sprites_200", [](Canvas& c) {
            std::mt19937 r(6);
            for (int i = 0; i < 200; ++i) c.drawCanvas(sprite, float(r() % W) - 64, float(r() % H) - 64);
        }, 10},
        {"scaled_sprites_200", [](Canvas& c) {
            std::mt19937 r(7);
            for (int i = 0; i < 200; ++i) { float s = 20 + float(r() % 100); c.drawCanvas(sprite, float(r() % W), float(r() % H), s, s); }
        }, 10},
        {"clipped_fills", [](Canvas& c) {
            for (int k = 0; k < 10; ++k) {
                c.save(); c.beginPath(); c.rect(100 + k * 3, 80, 900, 500); c.clip();
                c.setFillStyle(Color{30, 30, (unsigned char)(k * 20), 255}); c.fillRect(0, 0, W, H);
                c.setFillStyle(Color{200, 30, 30, 128}); c.fillCircle(600, 360, 300);
                c.restore();
            }
        }, 5},
        {"translucent_fullscreen", [](Canvas& c) {
            for (int i = 0; i < 10; ++i) { c.setFillStyle(Color{255, 255, 255, 40}); c.fillRect(0, 0, W, H); }
        }, 5},
        {"game_frame", [](Canvas& c) {
            std::mt19937 r(8);
            c.setFillStyle(Color{30, 140, 70}); c.fillRect(0, 0, W, H);
            for (int y = 0; y < H; y += 64) for (int x = 0; x < W; x += 64) {
                c.setFillStyle(((x + y) / 64) % 2 ? Color{34, 150, 76} : Color{28, 132, 66}); c.fillRect(float(x), float(y), 64, 64);
            }
            c.setLineWidth(3);
            for (int i = 0; i < 150; ++i) {
                float x = float(r() % W), y = float(r() % H), rad = 8 + float(r() % 24);
                c.save(); c.translate(x, y); c.rotate(float(r() % 628) / 100.f);
                c.setFillStyle(Color{220, 200, 60}); c.beginPath(); c.arc(0, 0, rad, 0, 6.2831853f); c.fill();
                c.setStrokeStyle(Color{170, 150, 40}); c.stroke();
                c.setFillStyle(Color{0, 0, 0}); c.beginPath(); c.ellipse(-rad * 0.3f, -rad * 0.2f, rad * 0.12f, rad * 0.25f, 0, 0, 6.2831853f); c.fill();
                c.restore();
            }
            c.setFillStyle(Color{0, 0, 0, 140}); c.beginPath(); c.roundRect(20, 20, 300, 180, 12); c.fill();
            c.setFillStyle(Color{255, 255, 255}); c.setFont("14px sans-serif");
            for (int i = 0; i < 8; ++i) c.fillText("Player " + std::to_string(i) + " joined the game", 34, 46 + i * 19.f);
            c.setFillStyle(Color{240, 240, 245}); c.fillRect(0, 0, W, H);   // opaque menu covering everything
            c.setFillStyle(Color{50, 50, 60}); c.setFont("24px sans-serif"); c.fillText("Paused", 600, 360);
        }, 10},
    };
    double total = 0;
    for (auto& s : scenes) {
        if (!only.empty() && only != s.name) continue;
        Canvas c(W, H);
#if 1
        c.setCulling(cull);
#endif
        (void)cull;
        // warm-up (font load, thread pool)
        c.clear(Color{255, 255, 255}); s.draw(c); FLUSH(c);
        std::vector<double> times;
        for (int rep = 0; rep < (repsOverride ? repsOverride : s.reps); ++rep) {
            c.clear(Color{255, 255, 255});
            c.setTransform(1, 0, 0, 1, 0, 0);
            auto t0 = std::chrono::steady_clock::now();
            s.draw(c); FLUSH(c);
            volatile auto px = c.getImageData(0, 0, 1, 1);
            auto t1 = std::chrono::steady_clock::now();
            times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(times.begin(), times.end());
        const double med = times[times.size() / 2];
        total += med;
        std::printf("%-26s %9.3f ms\n", s.name, med);
        if (!dump.empty()) {
            Canvas d(W, H); d.clear(Color{255, 255, 255}); s.draw(d); FLUSH(d);
            writePPM(d, dump + "/" + s.name + ".ppm");
        }
    }
    std::printf("%-26s %9.3f ms\n", "TOTAL", total);
}
