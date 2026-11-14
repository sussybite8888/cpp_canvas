// Renders every scene twice (culling on, then off) and reports the pixels.
// Native: writes <out>/<scene>.rgba and <scene>.nocull.rgba.
// Web: puts "name base64" lines into <pre id=out> for --dump-dom.
#include "canvas.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

struct Scene { const char* name; std::function<void(Canvas&)> draw; };
std::vector<Scene> scenes();
constexpr int W = 200, H = 150;

static std::vector<std::uint8_t> render(const Scene& s, bool cull) {
    Canvas c = Canvas::createVirtual(W, H);
    c.setCulling(cull);
    c.clear(Color{255, 255, 255});
    s.draw(c);
    return c.getImageData(0, 0, W, H);
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, emit_line, (const char* name, const std::uint8_t* data, int size), {
  let bin = '';
  const bytes = HEAPU8.subarray(data, data + size);
  for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  const out = document.getElementById('out');
  out.textContent += UTF8ToString(name) + ' ' + btoa(bin) + '\n';
});
EM_JS(void, emit_done, (), { document.getElementById('out').textContent += 'DONE\n'; });
int main() {
    for (const Scene& s : scenes()) {
        const auto a = render(s, true), b = render(s, false);
        emit_line(s.name, a.data(), static_cast<int>(a.size()));
        emit_line((std::string(s.name) + ".nocull").c_str(), b.data(), static_cast<int>(b.size()));
    }
    const CanvasFrameStats st = canvasTakeFrameStats();
    std::printf("ops %d culled %d\n", st.ops, st.culled);
    emit_done();
}
#else
int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    for (const Scene& s : scenes()) {
        for (int cull = 1; cull >= 0; --cull) {
            const auto px = render(s, cull != 0);
            std::ofstream f(out + "/" + s.name + (cull ? "" : ".nocull") + ".rgba", std::ios::binary);
            f.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
        }
    }
    const CanvasFrameStats st = canvasTakeFrameStats();
    std::printf("ops %d culled %d\n", st.ops, st.culled);
}
#endif
