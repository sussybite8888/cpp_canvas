// Every scene in tests/scenes.cpp drawn in software and on the GPU: the
// largest and mean channel difference per scene, and side-by-side PPMs
// (software | GPU | difference x8). Run through run.sh.
#include "canvas.h"
#include "window.h"
#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
struct Scene { const char* name; std::function<void(Canvas&)> draw; };
std::vector<Scene> scenes();
constexpr int W = 200, H = 150;
static std::vector<std::uint8_t> render(const Scene& s, bool gpu) {
    Canvas c = Canvas::createVirtual(W, H);
    if (gpu && !canvasAttachGpu(c, &SDL_GL_GetProcAddress)) { std::fprintf(stderr, "attach failed\n"); std::exit(1); }
    c.clear(Color{255, 255, 255});
    s.draw(c);
    return c.getImageData(0, 0, W, H);
}
int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    Window win;
    std::string err;
    if (!win.open(64, 64, "gpu check", err, false) || !win.accelerated()) { std::fprintf(stderr, "no GPU window: %s\n", err.c_str()); return 1; }
    std::printf("%-14s %8s %8s %8s\n", "scene", "max", "mean", "%>16");
    for (const Scene& s : scenes()) {
        const auto a = render(s, false), b = render(s, true);
        int mx = 0; double sum = 0; int big = 0;
        std::vector<std::uint8_t> ppm;
        for (std::size_t i = 0; i < a.size(); i += 4) {
            int d = 0;
            for (int k = 0; k < 4; ++k) d = std::max(d, std::abs(int(a[i + k]) - int(b[i + k])));
            mx = std::max(mx, d); sum += d; big += d > 16;
        }
        std::printf("%-14s %8d %8.3f %7.2f%%\n", s.name, mx, sum / (a.size() / 4), 100.0 * big / (a.size() / 4));
        FILE* f = std::fopen((out + "/" + s.name + ".ppm").c_str(), "wb");
        std::fprintf(f, "P6\n%d %d\n255\n", W * 3, H);
        for (int y = 0; y < H; ++y)
            for (int panel = 0; panel < 3; ++panel)
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = (std::size_t(y) * W + x) * 4;
                    for (int k = 0; k < 3; ++k) {
                        int v;
                        if (panel == 0) v = a[i + k] * a[i + 3] / 255 + 255 - a[i + 3];
                        else if (panel == 1) v = b[i + k] * b[i + 3] / 255 + 255 - b[i + 3];
                        else { int d = 0; for (int q = 0; q < 4; ++q) d = std::max(d, std::abs(int(a[i + q]) - int(b[i + q]))); v = 255 - std::min(255, d * 8); }
                        std::fputc(v, f);
                    }
                }
        std::fclose(f);
    }
}
