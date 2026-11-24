#pragma once
// The browser backend on the GPU: an element canvas drawn with WebGL2 through
// detail/gpu.h -- florrcustom's cp6 approach, as natively.
//
// The canvas records its calls into the op buffer like any other; its flush
// does not hand them to a 2D context but reads them (WebGpu::interpret), as
// the browser would, and turns each draw into GPU instances. Its 2D context
// is a hidden "shadow" surface the same size. A draw the GPU does not do
// (patterns, shadows, filters, composite operations, layers, stroked text,
// remote images) is replayed onto the shadow with its whole state, between
// a copy of the GPU's pixels onto it and a copy back.
//
// Included only by web.h, after the op buffer and before the Canvas methods.

#include <emscripten/html5.h>
#include <webgl/webgl2_ext.h>

#include "soft_paint.h"
#include "gpu.h"

EM_JS_DEPS(cpp_canvas_webgpu, "$GL");
namespace gl = canvas_detail::gl;

// Whether element canvases are drawn with WebGL2: yes unless the page set
// Module.cppCanvasGpu = false or has ?canvas-gpu=0 in its URL.
EM_JS(int, c2d_gpu_wanted, (), {
  if (Module.cppCanvasGpu === false) return 0;
  if (typeof location !== 'undefined' && /[?&]canvas-gpu=0/.test(location.search)) return 0;
  return typeof WebGL2RenderingContext !== 'undefined' ? 1 : 0;
});
EM_JS(int, c2d_gpu_is_webgl2, (), { return typeof WebGL2RenderingContext !== 'undefined' && GLctx instanceof WebGL2RenderingContext ? 1 : 0; });
// The element, sized: it is the WebGL canvas.
// The element replaced by a fresh copy: a WebGL context the GPU could not
// use would otherwise keep it from ever having a 2D one.
EM_JS(void, c2d_gpu_release_element, (const char* element), {
  const e = document.getElementById(UTF8ToString(element));
  if (e && e.parentNode) { const n = e.cloneNode(false); e.parentNode.replaceChild(n, e); }
});
EM_JS(void, c2d_gpu_element, (const char* element, int width, int height), {
  const id = UTF8ToString(element);
  const e = document.getElementById(id) || Object.assign(document.createElement('canvas'), {id});
  if (!e.parentNode) document.body.appendChild(e);
  if (e.width !== width) e.width = width;
  if (e.height !== height) e.height = height;
});
// The GPU's pixels, just drawn to the element, onto the shadow surface.
EM_JS(void, c2d_gpu_to_shadow, (int shadow, const char* element), {
  const entry = Module.cppCanvasContexts[shadow], e = document.getElementById(UTF8ToString(element));
  if (!entry || !entry.ctx || !e) return;
  const x = entry.ctx;
  x.save(); x.setTransform(1, 0, 0, 1, 0, 0); x.globalAlpha = 1; x.globalCompositeOperation = 'copy'; x.filter = 'none';
  x.shadowColor = 'transparent'; x.drawImage(e, 0, 0); x.restore();
});
// The shadow surface into GL texture `tex` (premultiplied, as the canvas
// texture is kept).
EM_JS(void, c2d_gpu_from_shadow, (int shadow, int tex), {
  const entry = Module.cppCanvasContexts[shadow], gl = GLctx;
  if (!entry || !gl) return;
  gl.bindTexture(gl.TEXTURE_2D, GL.textures[tex]);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
  gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, entry.surface);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
});
// A canvas's surface (kind 0, by context id) or an uploaded image (kind 1,
// by key) into GL texture `tex`, straight alpha. Its size into size[0..1];
// 0 if there is no such thing.
EM_JS(int, c2d_gpu_surface_texture, (int kind, int key, int tex, int* size), {
  const gl = GLctx;
  let source = null;
  if (kind === 0) { const e = Module.cppCanvasContexts[key]; source = e && e.surface; }
  else source = Module.cppCanvasImages && Module.cppCanvasImages.get(key);
  if (!source || !gl || !source.width || !source.height) return 0;
  gl.bindTexture(gl.TEXTURE_2D, GL.textures[tex]);
  // The same size as the texture holds: its storage kept, the pixels replaced.
  if (HEAP32[size >> 2] === source.width && HEAP32[(size >> 2) + 1] === source.height) gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, source);
  else gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
  HEAP32[size >> 2] = source.width; HEAP32[(size >> 2) + 1] = source.height;
  return 1;
});
// A run of text, drawn white at `scale` device pixels per unit into the
// text atlas (GL texture `tex`), once per font, text, layout and scale.
// out: atlas x, y, w, h, and the run's origin within that box. 1 placed,
// 0 nothing to draw, -1 the atlas is full (reset it and ask again).
EM_JS(int, c2d_gpu_text, (int tex, int atlasSize, const char* font, const char* text, const char* align, const char* baseline, const char* direction,
                          const char* letter, const char* word, const char* kerning, float maxWidth, float scale, float* out), {
  const A = Module.cppGpuText || (Module.cppGpuText = {map: new Map(), x: 0, y: 0, rowH: 0,
    scratch: (typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(64, 64) : document.createElement('canvas'))});
  const f = UTF8ToString(font), t = UTF8ToString(text), al = UTF8ToString(align), bl = UTF8ToString(baseline), dir = UTF8ToString(direction);
  const ls = UTF8ToString(letter), ws = UTF8ToString(word), kn = UTF8ToString(kerning);
  const key = f + '\u0001' + t + '\u0001' + al + '\u0001' + bl + '\u0001' + dir + '\u0001' + ls + '\u0001' + ws + '\u0001' + kn + '\u0001' + maxWidth + '\u0001' + scale;
  let e = A.map.get(key);
  if (!e) {
    const c = A.scratch, x = c.getContext('2d');
    const setup = () => {
      x.font = f; x.textAlign = al; x.textBaseline = bl; x.direction = dir;
      if ('letterSpacing' in x) x.letterSpacing = ls;
      if ('wordSpacing' in x) x.wordSpacing = ws;
      if ('fontKerning' in x) x.fontKerning = kn;
    };
    setup();
    const m = x.measureText(t);
    const shrink = maxWidth >= 0 && m.width > maxWidth && m.width > 0 ? maxWidth / m.width : 1;
    const l = Math.floor(-m.actualBoundingBoxLeft * shrink * scale) - 2, r = Math.ceil(m.actualBoundingBoxRight * shrink * scale) + 2;
    const tp = Math.floor(-m.actualBoundingBoxAscent * scale) - 2, bt = Math.ceil(m.actualBoundingBoxDescent * scale) + 2;
    const w = r - l, h = bt - tp;
    if (!(w > 0 && h > 0) || !t.length) { e = {empty: true}; A.map.set(key, e); return 0; }
    if (w > atlasSize || h > atlasSize) return 0;
    if (A.x + w > atlasSize) { A.x = 0; A.y += A.rowH + 1; A.rowH = 0; }
    if (A.y + h > atlasSize) return -1;
    if (c.width < w || c.height < h) { c.width = Math.max(c.width, w); c.height = Math.max(c.height, h); }
    x.setTransform(1, 0, 0, 1, 0, 0);
    x.clearRect(0, 0, c.width, c.height);
    setup();
    x.fillStyle = '#fff';
    x.setTransform(scale, 0, 0, scale, -l, -tp);
    if (maxWidth >= 0) x.fillText(t, 0, 0, maxWidth); else x.fillText(t, 0, 0);
    const gl = GLctx;
    gl.bindTexture(gl.TEXTURE_2D, GL.textures[tex]);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, A.x, A.y, w, h, gl.RGBA, gl.UNSIGNED_BYTE, x.getImageData(0, 0, w, h));
    e = {x: A.x, y: A.y, w, h, ox: -l, oy: -tp};
    A.x += w + 1; A.rowH = Math.max(A.rowH, h);
    A.map.set(key, e);
  }
  if (e.empty) return 0;
  const o = out >> 2;
  HEAPF32[o] = e.x; HEAPF32[o + 1] = e.y; HEAPF32[o + 2] = e.w; HEAPF32[o + 3] = e.h; HEAPF32[o + 4] = e.ox; HEAPF32[o + 5] = e.oy;
  return 1;
});
EM_JS(void, c2d_gpu_text_reset, (), { if (Module.cppGpuText) { Module.cppGpuText.map.clear(); Module.cppGpuText.x = Module.cppGpuText.y = Module.cppGpuText.rowH = 0; } });

namespace {
using canvas_detail::Matrix;
using canvas_detail::Poly;
using canvas_detail::IBox;
using canvas_detail::ClipMask;
using canvas_detail::GpuSurface;
using canvas_detail::GpuStyle;

// Gradients by the name their page object has ("\x01g12"), for the GPU,
// which draws them itself.
std::unordered_map<std::string, std::weak_ptr<CanvasGradient::Data>>& gradientsByRef() {
  static auto* m = new std::unordered_map<std::string, std::weak_ptr<CanvasGradient::Data>>();
  return *m;
}
// Each canvas's content version: bumped whenever its calls reach the page,
// so a GPU canvas knows when its copy of it is out of date.
std::unordered_map<int, std::uint64_t>& contentVersions() { static auto* m = new std::unordered_map<int, std::uint64_t>(); return *m; }

// A pixel-aligned rectangle needs no mask; anything else is rasterized.
std::shared_ptr<const ClipMask> makeClipMask(const Poly& poly, bool evenOdd, const ClipMask* outer, IBox canvas) {
  using namespace canvas_detail;
  auto mask = std::make_shared<ClipMask>();
  IBox bounds = canvas;
  if (outer) bounds = bounds.intersect({outer->x0, outer->y0, outer->x1, outer->y1});
  int x0, y0, x1, y1;
  if (bounds.empty() || !polyBounds(poly, bounds.x0, bounds.y0, bounds.x1, bounds.y1, x0, y0, x1, y1)) {
    mask->rect = true;
    mask->computeRuns();
    return mask;
  }
  float X0, Y0, X1, Y1;
  const bool rect = !evenOdd && polyIsRect(poly, X0, Y0, X1, Y1);
  const auto integral = [](float v) { return std::abs(v - std::round(v)) < 1e-4f; };
  if (rect && integral(X0) && integral(Y0) && integral(X1) && integral(Y1) && (!outer || outer->rect)) {
    const IBox r = IBox{static_cast<int>(std::lround(X0)), static_cast<int>(std::lround(Y0)), static_cast<int>(std::lround(X1)), static_cast<int>(std::lround(Y1))}.intersect(bounds);
    mask->x0 = r.x0; mask->y0 = r.y0; mask->x1 = std::max(r.x0, r.x1); mask->y1 = std::max(r.y0, r.y1);
    mask->rect = true;
  } else {
    mask->x0 = x0; mask->y0 = y0; mask->x1 = x1; mask->y1 = y1;
    mask->rect = false;
    mask->alpha.assign(static_cast<std::size_t>(x1 - x0) * (y1 - y0), 0);
    std::uint8_t* cells = mask->alpha.data();
    const int stride = x1 - x0;
    scanFill(poly, evenOdd, x0, y0, x1, y1, [&](int a, int b, int y, float cov) {
      std::uint8_t* row = cells + static_cast<std::size_t>(y - y0) * static_cast<std::size_t>(stride);
      const std::uint32_t v = static_cast<std::uint32_t>(cov * 255 + 0.5f);
      if (!outer) { std::memset(row + (a - x0), static_cast<int>(v), static_cast<std::size_t>(b - a)); return; }
      for (int x = a; x < b; ++x) row[x - x0] = static_cast<std::uint8_t>(div255(v * outer->at(x, y)));
    });
  }
  mask->computeRuns();
  return mask;
}
}  // namespace

// A GPU canvas: its surface, and the state of the calls it reads.

inline canvas_detail::Matrix toMatrix(const std::array<double, 6>& d) {
  return canvas_detail::Matrix{static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]), static_cast<float>(d[3]), static_cast<float>(d[4]), static_cast<float>(d[5])};
}

// A GPU canvas: its surface, its clip, and the replays onto its shadow.
// The canvas's methods call in here directly (as cp6's context does): the
// drawing state is the canvas's own (Canvas::WebState), read as it stands.
struct WebGpu {
  GpuSurface G;
  int shadow = -1;       // the canvas's 2D context: the shadow surface
  std::string element;   // the WebGL canvas
  int w = 0, h = 0;
  bool shadowStale = false;  // the GPU has drawn since the shadow last had its pixels
  gl::GLuint textAtlas = 0;
  static constexpr int kTextAtlas = 2048;
  struct SourceTexture { gl::GLuint tex = 0; std::uint64_t version = ~0ull; int w = 0, h = 0; std::uint64_t used = 0; };
  std::unordered_map<int, SourceTexture> canvases;
  std::unordered_map<std::uint32_t, SourceTexture> images;
  std::uint64_t frame = 0;

  // The state a draw reads: the canvas's, and the transform as floats.
  const Canvas::WebState* ws = nullptr;
  Matrix m{1, 0, 0, 1, 0, 0};
  void use(const Canvas::WebState& state) {
    ws = &state;
    const auto& d = state.matrix;
    m = Matrix{static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]), static_cast<float>(d[3]), static_cast<float>(d[4]), static_cast<float>(d[5])};
  }

  // ----- the clip, saved and restored with the canvas -----
  // As cp6: up to four clips tested per pixel (paths, exact circles). Past
  // four, the rest is rasterized into a mask.
  struct ClipShape { Poly poly; bool evenOdd = false; };
  struct Clip {
    std::shared_ptr<const ClipMask> mask;
    std::shared_ptr<const canvas_detail::GpuClipPaths> paths;
    std::shared_ptr<const std::vector<ClipShape>> shapes;  // every clip, for replays
  };
  Clip clip;
  std::vector<Clip> clipStack;
  void save() { clipStack.push_back(clip); }
  void restore() { if (!clipStack.empty()) { clip = std::move(clipStack.back()); clipStack.pop_back(); clipChanged(); } }
  // The style draws take (style()): the clip kept in it as it changes, so a
  // draw copies no references.
  mutable GpuStyle st;
  void clipChanged() { st.clip = clip.mask; st.clipPaths = clip.paths; }
  void clipTo(const Poly& device, bool evenOdd, const float* circle = nullptr) {
    auto shapes = std::make_shared<std::vector<ClipShape>>(clip.shapes ? *clip.shapes : std::vector<ClipShape>{});
    shapes->push_back(ClipShape{device, evenOdd});
    clip.shapes = std::move(shapes);
    if (!clip.mask && (!clip.paths || clip.paths->count() < 4)) {
      auto paths = std::make_shared<canvas_detail::GpuClipPaths>(clip.paths ? *clip.paths : canvas_detail::GpuClipPaths{});
      int x0, y0, x1, y1;
      if (!canvas_detail::polyBounds(device, -(1 << 28), -(1 << 28), 1 << 28, 1 << 28, x0, y0, x1, y1)) { x0 = y0 = 0; x1 = y1 = 0; }
      if (circle) paths->circles.push_back({circle[0], circle[1], circle[2], false});
      else paths->paths.push_back({device, evenOdd});
      paths->x0 = std::max(paths->x0, x0 - 1); paths->y0 = std::max(paths->y0, y0 - 1);
      paths->x1 = std::min(paths->x1, x1 + 1); paths->y1 = std::min(paths->y1, y1 + 1);
      clip.paths = std::move(paths);
      clipChanged();
      return;
    }
    // A fifth: everything so far and it, rasterized.
    std::shared_ptr<const ClipMask> mask = clip.mask;
    if (!mask && clip.paths)
      for (const ClipShape& c : *clip.shapes) if (&c != &clip.shapes->back()) mask = makeClipMask(c.poly, c.evenOdd, mask.get(), IBox{0, 0, w, h});
    clip.mask = makeClipMask(device, evenOdd, mask.get(), IBox{0, 0, w, h});
    clip.paths.reset();
    clipChanged();
  }

  bool init(const std::string& id, int width, int height, int shadowId) {
    element = id; w = width; h = height; shadow = shadowId;
    c2d_gpu_element(id.c_str(), w, h);
    static std::unordered_map<std::string, EMSCRIPTEN_WEBGL_CONTEXT_HANDLE> contexts;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = contexts[id];
    if (!ctx) {
      EmscriptenWebGLContextAttributes a;
      emscripten_webgl_init_context_attributes(&a);
      a.majorVersion = 2; a.minorVersion = 0;
      a.alpha = true; a.depth = false; a.stencil = false; a.antialias = false;
      a.premultipliedAlpha = true; a.preserveDrawingBuffer = false;
      a.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
      ctx = emscripten_webgl_create_context(("#" + id).c_str(), &a);
      if (ctx <= 0) return false;
      contexts[id] = ctx;
    }
    if (emscripten_webgl_make_context_current(ctx) != EMSCRIPTEN_RESULT_SUCCESS) return false;
    if (!c2d_gpu_is_webgl2()) {
      // Linked without -sMAX_WEBGL_VERSION=2: a WebGL1 context, no use here.
      emscripten_webgl_destroy_context(ctx);
      contexts.erase(id);
      c2d_gpu_release_element(id.c_str());
      return false;
    }
    std::vector<Color> blank(static_cast<std::size_t>(w) * h, Color{0, 0, 0, 255});
    if (!G.init(nullptr, w, h, blank)) {
      emscripten_webgl_destroy_context(ctx);
      contexts.erase(id);
      c2d_gpu_release_element(id.c_str());
      return false;
    }
    textAtlas = G.texture(gl::RGBA8, kTextAtlas, kTextAtlas, gl::NEAREST);
    c2d_gpu_text_reset();
    return true;
  }
  ~WebGpu() {
    if (!G.ok) return;
    if (textAtlas) G.g.DeleteTextures(1, &textAtlas);
    for (auto& [k, t] : canvases) G.g.DeleteTextures(1, &t.tex);
    for (auto& [k, t] : images) G.g.DeleteTextures(1, &t.tex);
  }

  // ----- the screen and the shadow -----
  void present() {
    if (generic) return;  // mid-layer: the shadow has the pixels until it ends
    G.draw();
    G.show(w, h);
    ++frame;
    for (auto it = canvases.begin(); it != canvases.end();) { if (frame - it->second.used > 300) { G.g.DeleteTextures(1, &it->second.tex); it = canvases.erase(it); } else ++it; }
    for (auto it = images.begin(); it != images.end();) { if (frame - it->second.used > 300) { G.g.DeleteTextures(1, &it->second.tex); it = images.erase(it); } else ++it; }
    if (paths.size() > 8192) { paths.clear(); byKey.clear(); }
    if (currents.size() > 4096) currents.clear();
  }
  // The shadow gets the GPU's pixels (before a replay, or for whoever reads
  // the canvas's 2D surface).
  void syncShadow() {
    if (generic) return;
    G.draw();
    if (!shadowStale) return;
    G.show(w, h, true);
    c2d_gpu_to_shadow(shadow, element.c_str());
    shadowStale = false;
  }
  std::vector<std::uint8_t> readPixels(int x, int y, int rw, int rh) {
    G.draw();
    std::vector<Color> px(static_cast<std::size_t>(w) * h);
    G.readback(px, IBox{0, 0, w, h});
    std::vector<std::uint8_t> out(static_cast<std::size_t>(std::max(0, rw)) * std::max(0, rh) * 4, 0);
    for (int row = 0; row < rh; ++row) {
      const int sy = y + row;
      if (sy < 0 || sy >= h) continue;
      for (int col = 0; col < rw; ++col) {
        const int sx = x + col;
        if (sx < 0 || sx >= w) continue;
        const Color c = px[static_cast<std::size_t>(sy) * w + sx];
        std::uint8_t* o = out.data() + (static_cast<std::size_t>(row) * rw + col) * 4;
        o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
      }
    }
    return out;
  }

  // ----- textures -----
  bool pendingUses(gl::GLuint tex) const {
    for (const auto& b : G.batches) if (b.image == tex && b.count) return true;
    return false;
  }
  // A canvas's surface, current.
  SourceTexture* canvasTexture(int context) {
    SourceTexture& t = canvases[context];
    const std::uint64_t version = contentVersions()[context];
    t.used = frame;
    if (t.tex && t.version == version) return &t;
    if (t.tex && pendingUses(t.tex)) G.draw();  // what was drawn from it reads it as it was
    if (!t.tex) t.tex = G.texture(gl::RGBA8, 1, 1, gl::NEAREST);
    int size[2] = {t.w, t.h};  // what the texture holds now
    if (!c2d_gpu_surface_texture(0, context, static_cast<int>(t.tex), size)) { t.version = ~0ull; return nullptr; }
    t.w = size[0]; t.h = size[1]; t.version = version;
    return &t;
  }
  // Pixels the caller names by `key` (0: used once), straight RGBA.
  gl::GLuint pixelTexture(const std::uint8_t* rgba, int iw, int ih, std::uint32_t key) {
    if (!key) {
      const gl::GLuint t = G.texture(gl::RGBA8, iw, ih, gl::NEAREST);
      G.fillTexture(t, 0, 0, rgba, iw, ih);
      G.oneOff.push_back(t);
      return t;
    }
    SourceTexture& t = images[key];
    t.used = frame;
    if (t.tex && t.w == iw && t.h == ih) return t.tex;
    if (t.tex) { if (pendingUses(t.tex)) G.oneOff.push_back(t.tex); else G.g.DeleteTextures(1, &t.tex); }
    t.tex = G.texture(gl::RGBA8, iw, ih, gl::NEAREST);
    t.w = iw; t.h = ih;
    G.fillTexture(t.tex, 0, 0, rgba, iw, ih);
    return t.tex;
  }

  // ----- the paint -----
  enum class PaintKind { Solid, Gradient, Other };
  struct Paint { PaintKind kind = PaintKind::Other; Color colour; std::shared_ptr<const canvas_detail::GradientImpl> gradient; };
  Paint paint(const std::string& s, const std::shared_ptr<void>& object) {
    Paint p;
    if (!s.empty() && s[0] == '\x01') {
      if (s.size() > 1 && s[1] == 'g' && object) {
        p.kind = PaintKind::Gradient;
        p.gradient = canvas_detail::resolveGradient(*std::static_pointer_cast<CanvasGradient::Data>(object));
      }
      return p;
    }
    static std::unordered_map<std::string, Color> parsed;
    auto it = parsed.find(s);
    if (it == parsed.end()) {
      Color c;
      if (!parseCssColor(s, c)) return p;
      if (parsed.size() > 4096) parsed.clear();
      it = parsed.emplace(s, c).first;
    }
    p.kind = PaintKind::Solid; p.colour = it->second;
    return p;
  }
  // A CSS colour is parsed once: the state keeps the colour it names.
  Paint fillPaint() {
    if (ws->fillRgbaOk) { Paint p; p.kind = PaintKind::Solid; p.colour = ws->fillRgba; return p; }
    Paint p = paint(ws->fill, ws->fillObject);
    if (p.kind == PaintKind::Solid) { ws->fillRgba = p.colour; ws->fillRgbaOk = true; }
    return p;
  }
  Paint strokePaint() {
    if (ws->strokeRgbaOk) { Paint p; p.kind = PaintKind::Solid; p.colour = ws->strokeRgba; return p; }
    Paint p = paint(ws->stroke, ws->strokeObject);
    if (p.kind == PaintKind::Solid) { ws->strokeRgba = p.colour; ws->strokeRgbaOk = true; }
    return p;
  }
  bool shadowVisible() const {
    if (ws->shadowBlur <= 0 && ws->shadowOffsetX == 0 && ws->shadowOffsetY == 0) return false;
    Color c;
    return parseCssColor(ws->shadowColour, c) && c.a != 0;
  }
  // Whether the state lets the GPU draw at all.
  // (The setters only take valid names: "source-over" is the 11-letter one
  // with an 's' first and an 'o' eighth, and "none" the filter that starts with 'n'.)
  // (Interned: these compare as pointers.)
  inline static const Canvas::WebString kSourceOver{"source-over"}, kNoFilter{"none"}, kNoText{""};
  bool plain() const {
    return !generic && ws->composite == kSourceOver && (ws->filter == kNoFilter || ws->filter == kNoText) &&
           (ws->shadowBlur <= 0 && ws->shadowOffsetX == 0 && ws->shadowOffsetY == 0 ? true : !shadowVisible());
  }
  int capOf() const { const char c = ws->lineCap.empty() ? 'b' : ws->lineCap[0]; return c == 'r' ? 1 : c == 's' ? 2 : 0; }
  const GpuStyle& style() const { st.alpha = ws->alpha; return st; }
  GpuStyle style(float extraAlpha) const { return GpuStyle{ws->alpha * extraAlpha, clip.mask, clip.paths}; }
  bool usePaint(const Paint& p) {
    G.useSolid();
    if (p.kind == PaintKind::Gradient) {
      if (!p.gradient || p.gradient->paintsNothing) return true;
      G.useGradient(p.gradient, m);
      return true;
    }
    return p.kind == PaintKind::Solid;
  }
  static bool uniform(const Matrix& t) {
    const float det = t[0] * t[3] - t[1] * t[2], scale = std::sqrt(std::abs(det));
    const float tol = 1e-4f * std::max(1.f, scale);
    return scale > 0 && ((std::abs(t[0] - t[3]) < tol && std::abs(t[1] + t[2]) < tol) || (std::abs(t[0] + t[3]) < tol && std::abs(t[1] - t[2]) < tol));
  }
  static Matrix invertOr(const Matrix& t) { Matrix inv; if (!canvas_detail::invert(t, inv)) return Matrix{0, 0, 0, 0, 0, 0}; return inv; }

  // ----- the current path, in device space as it is built (cp6) -----
  canvas_detail::GpuTexPath tp;  // in the GPU's layout, so a fill copies it whole
  bool hasCurrent = false;
  float curX = 0, curY = 0, startX = 0, startY = 0;
  // A path that is one whole circle under a uniform transform, as a circle.
  bool isCircle = false;
  float circleX = 0, circleY = 0, circleR = 0, circleScale = 1;
  std::uint64_t pathVersion = 1;  // which path a fill was of, so its stroke can join it
  void beginPath() { tp.reset(); hasCurrent = false; isCircle = false; ++pathVersion; }
  void dropCircle() { if (isCircle) circleToPoints(); }
  void circleToPoints() {
    // It becomes points: its outline as an ordinary closed subpath.
    isCircle = false;
    const int n = std::max(8, std::min(256, static_cast<int>(std::ceil(6.2831853f * std::sqrt(std::max(circleR, 1.f)) * 1.1f))));
    tp.finish(false);
    for (int k = 0; k < n; ++k) {
      const float t = 6.2831853f * k / n, X = circleX + circleR * std::cos(t), Y = circleY + circleR * std::sin(t);
      k ? tp.add(X, Y) : tp.start(X, Y);
    }
    tp.finish(true);
    hasCurrent = true; curX = startX = circleX + circleR; curY = startY = circleY;
  }
  void apply(float x, float y, float& X, float& Y) const { X = m[0] * x + m[2] * y + m[4]; Y = m[1] * x + m[3] * y + m[5]; }
  void moveToDevice(float X, float Y) {
    tp.finish(false);
    tp.start(X, Y);
    hasCurrent = true; curX = startX = X; curY = startY = Y;
  }
  void moveTo(float x, float y) {
    dropCircle(); ++pathVersion;
    float X, Y; apply(x, y, X, Y);
    moveToDevice(X, Y);
  }
  __attribute__((noinline)) void reopen(float X, float Y) {
    if (!hasCurrent) { moveToDevice(X, Y); return; }
    tp.start(curX, curY); startX = curX; startY = curY;
    tp.add(X, Y);
    curX = X; curY = Y;
  }
  __attribute__((always_inline)) void lineToDevice(float X, float Y) {
    if (!tp.open) { reopen(X, Y); return; }
    tp.add(X, Y);
    curX = X; curY = Y;
  }
  void lineTo(float x, float y) { dropCircle(); ++pathVersion; float X, Y; apply(x, y, X, Y); lineToDevice(X, Y); }
  void closePath() {
    if (isCircle) return;
    ++pathVersion;
    tp.finish(true);
    if (hasCurrent) { curX = startX; curY = startY; }
  }
  void quadraticCurveTo(float cx, float cy, float x, float y) {
    dropCircle(); ++pathVersion;
    float ax, ay, bx, by; apply(cx, cy, ax, ay); apply(x, y, bx, by);
    if (!hasCurrent) { moveTo(cx, cy); }
    const float x0 = curX, y0 = curY;
    const float len = std::sqrt((ax - x0) * (ax - x0) + (ay - y0) * (ay - y0)) + std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    const int n = std::max(2, std::min(64, static_cast<int>(std::sqrt(len) * 1.2f)));
    for (int i = 1; i <= n; ++i) { const float t = static_cast<float>(i) / n, u = 1 - t; lineToDevice(u * u * x0 + 2 * u * t * ax + t * t * bx, u * u * y0 + 2 * u * t * ay + t * t * by); }
  }
  void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    dropCircle(); ++pathVersion;
    float ax, ay, bx, by, ex, ey; apply(c1x, c1y, ax, ay); apply(c2x, c2y, bx, by); apply(x, y, ex, ey);
    if (!hasCurrent) moveTo(c1x, c1y);
    const float x0 = curX, y0 = curY;
    const float len = std::sqrt((ax - x0) * (ax - x0) + (ay - y0) * (ay - y0)) + std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) + std::sqrt((ex - bx) * (ex - bx) + (ey - by) * (ey - by));
    const int n = std::max(2, std::min(96, static_cast<int>(std::sqrt(len) * 1.4f)));
    for (int i = 1; i <= n; ++i) {
      const float t = static_cast<float>(i) / n, u = 1 - t, A = u * u * u, B = 3 * u * u * t, C = 3 * u * t * t, D = t * t * t;
      lineToDevice(A * x0 + B * ax + C * bx + D * ex, A * y0 + B * ay + C * by + D * ey);
    }
  }
  // arc and ellipse as cp6 builds them: points in user space, the transform's
  // scale setting how many.
  void ellipse(float x, float y, float rx, float ry, float rot, float a0, float a1, bool ccw) {
    float sweep = a1 - a0;
    if (!ccw) { if (sweep >= 6.2831853f) sweep = 6.2831853f; else if (sweep < 0) sweep = std::fmod(sweep, 6.2831853f) + 6.2831853f; }
    else { if (sweep <= -6.2831853f) sweep = -6.2831853f; else if (sweep > 0) sweep = std::fmod(sweep, 6.2831853f) - 6.2831853f; }
    const bool full = std::abs(std::abs(sweep) - 6.2831853f) < 1e-4f;
    const auto startsHere = [&] {
      float sx, sy; apply(x + std::cos(a0) * rx, y + std::sin(a0) * rx, sx, sy);
      return std::abs(tp.fx - sx) < 1e-3f && std::abs(tp.fy - sy) < 1e-3f;
    };
    const bool lone = (tp.len == 0 && !hasCurrent) || (full && tp.open && tp.hdr == 0 && tp.n == 1 && startsHere());
    if (full && rx == ry && rot == 0 && lone && !isCircle && uniform(m)) {
      tp.reset(); hasCurrent = false;
      // A lone whole circle: kept as one.
      float X, Y; apply(x, y, X, Y);
      circleScale = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
      isCircle = true; circleX = X; circleY = Y; circleR = rx * circleScale;
      ++pathVersion;
      return;
    }
    dropCircle(); ++pathVersion;
    const float detail = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
    const float r = std::max(rx, ry) * detail;
    const int n = std::max(3, std::min(256, static_cast<int>(std::ceil(std::abs(sweep) * std::sqrt(std::max(r, 1.f)) * 1.1f))));
    const float cr = std::cos(rot), sr = std::sin(rot);
    const bool starts = !hasCurrent;
    // The angle steps by turning (cos, sin) a fixed step, in double (no drift
    // worth a float); the last point of an open arc exactly where it ends.
    const double step = static_cast<double>(sweep) / n, cs = std::cos(step), sn = std::sin(step);
    double c = std::cos(static_cast<double>(a0)), s = std::sin(static_cast<double>(a0));
    // Ellipse space -> device, all at once.
    const float ax = m[0] * cr + m[2] * sr, ay = m[1] * cr + m[3] * sr, bx = m[2] * cr - m[0] * sr, by = m[3] * cr - m[1] * sr;
    float ox, oy; apply(x, y, ox, oy);
    for (int i = 0; i <= n; ++i) {
      if (full && i == n) break;
      float ux, uy;
      if (i == n) { ux = std::cos(a0 + sweep) * rx; uy = std::sin(a0 + sweep) * ry; }
      else { ux = static_cast<float>(c) * rx; uy = static_cast<float>(s) * ry; }
      const float X = ox + ax * ux + bx * uy, Y = oy + ay * ux + by * uy;
      if (i == 0 && !hasCurrent) moveToDevice(X, Y);
      else lineToDevice(X, Y);
      const double c2 = c * cs - s * sn;
      s = s * cs + c * sn; c = c2;
    }
    if (full && starts) closePath();
  }
  void arc(float x, float y, float r, float a0, float a1, bool ccw) { ellipse(x, y, r, r, 0, a0, a1, ccw); }
  void arcTo(float x1, float y1, float x2, float y2, float r) {
    if (!hasCurrent) { moveTo(x1, y1); return; }
    segments([&](Path2D& p) { p.arcTo(x1, y1, x2, y2, r); });
  }
  // currentPath_ (the records' copy) is behind this one: only this one is
  // built while the GPU draws.
  bool pathStale = false;
  // The current path's device box; false when it has no points.
  bool deviceBox(float& x0, float& y0, float& x1, float& y1) const {
    if (isCircle) { x0 = circleX - circleR; y0 = circleY - circleR; x1 = circleX + circleR; y1 = circleY + circleR; return true; }
    if (!(tp.x0 <= tp.x1)) return false;
    x0 = tp.x0; y0 = tp.y0; x1 = tp.x1; y1 = tp.y1;
    return true;
  }
  void rect(float x, float y, float rw, float rh) {
    dropCircle(); ++pathVersion;
    moveTo(x, y); lineTo(x + rw, y); lineTo(x + rw, y + rh); lineTo(x, y + rh); closePath();
    moveTo(x, y);
  }
  // The rest (arcTo, roundRect): built as a Path2D in user space from the
  // current point, flattened through the transform.
  void segments(const std::function<void(Path2D&)>& build) {
    dropCircle(); ++pathVersion;
    Path2D tmp;
    if (hasCurrent) { const Matrix inv = invertOr(m); tmp.moveTo(inv[0] * curX + inv[2] * curY + inv[4], inv[1] * curX + inv[3] * curY + inv[5]); }
    build(tmp);
    static thread_local Poly flat;
    canvas_detail::flatten(tmp.segments().data(), tmp.segments().size(), m, canvas_detail::matrixMaxScale(m), flat);
    int first = 0;
    for (std::size_t c = 0; c < flat.ends.size(); ++c) {
      const int last = flat.ends[c];
      if (last > first) {
        const float px = flat.pts[2 * first], py = flat.pts[2 * first + 1];
        // The first subpath carries on from the current point; others start anew.
        const bool carries = c == 0 && hasCurrent && std::abs(px - curX) < 1e-3f && std::abs(py - curY) < 1e-3f;
        if (!carries) moveToDevice(px, py);
        else if (!tp.open) lineToDevice(px, py);
        for (int k = first + 1; k < last; ++k) lineToDevice(flat.pts[2 * k], flat.pts[2 * k + 1]);
        if (flat.closed[c]) closePath();
      }
      first = last;
    }
  }
  // The path as a Poly (clips, fallbacks): converted.
  const Poly& currentDevice() {
    static thread_local Poly done;
    tp.toPoly(done);
    return done;
  }
  bool drawCurrent(int action, bool evenOdd) { return drawCurrentIn(action, evenOdd); }
  // The common case straight through (cp6's circle): the current path is one
  // circle, the state plain, the paint a colour. False: take the long way.
  // (A stroke's width is under the transform at the stroke, as the long way.)
  bool fastCircle(const Canvas::WebState& w, int action) {
    if (!isCircle || generic) return false;
    ws = &w;
    if (action == 0 ? (!w.fillRgbaOk || w.fillObject) : (!w.strokeRgbaOk || w.strokeObject || !w.dash.empty())) return false;
    if (!plain()) return false;
    const Color c = action == 0 ? w.fillRgba : w.strokeRgba;
    G.useSolid();
    if (c.a == 0) return true;
    if (action == 0) { filledVersion = pathVersion; return G.circle(circleX, circleY, circleR, -1, style(), c); }
    const auto& d = w.matrix;
    const float pen = static_cast<float>(std::sqrt(std::abs(d[0] * d[3] - d[1] * d[2])));
    return G.circle(circleX, circleY, circleR, w.lineWidth * 0.5f * pen, style(), c);
  }
  // lineTo / moveTo straight from the canvas's matrix.
  __attribute__((always_inline)) void lineTo(const std::array<double, 6>& d, float x, float y) {
    if (isCircle) circleToPoints();
    ++pathVersion;
    lineToDevice(static_cast<float>(d[0] * x + d[2] * y + d[4]), static_cast<float>(d[1] * x + d[3] * y + d[5]));
  }
  // The current path filled (0), stroked (1) or clipped to (2), as cp6 draws
  // it. False when the GPU does not draw it (the caller replays it).
  std::uint64_t filledVersion = 0;
  // The draw a fill made (its instance count, in which batch of draws): a
  // stroke joins it only straight after.
  struct FillMark { std::uint64_t gen = 0; std::size_t inst = 0; };
  FillMark currentFill, path2dFill;
  FillMark markNow() const { return FillMark{G.drawGen, G.inst.size()}; }
  bool justAfter(const FillMark& f) const { return f.gen == G.drawGen && f.inst == G.inst.size() && f.inst != 0; }
  bool drawCurrentIn(int action, bool evenOdd) {
    if (action == 2) {
      if (isCircle) { const float c[3] = {circleX, circleY, circleR}; Poly ring; currentCircleRing(ring); clipTo(ring, false, c); return true; }
      clipTo(currentDevice(), evenOdd);
      return true;
    }
    if (!plain()) return false;
    const Paint p = action == 0 ? fillPaint() : strokePaint();
    if (p.kind == PaintKind::Other) return false;
    if (action == 1 && !ws->dash.empty()) {
      // Dashes: the exact stroke, from the path in user space.
      dropCircle();
      Matrix inv;
      if (!canvas_detail::invert(m, inv)) return true;
      static thread_local Poly local;
      transformPoly(currentDevice(), inv, local);
      if (!usePaint(p)) return false;
      return strokeLocal(local, p);
    }
    if (!usePaint(p)) return false;
    if (p.kind == PaintKind::Solid && p.colour.a == 0) return true;
    const bool gradient = p.kind == PaintKind::Gradient;
    if (isCircle && action == 0) { filledVersion = pathVersion; return G.circle(circleX, circleY, circleR, -1, style(), p.colour); }
    const float hw = action == 0 ? 0.f : ws->lineWidth * 0.5f * std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
    const int cap = capOf();
    if (isCircle) return G.circle(circleX, circleY, circleR, hw, style(), p.colour);
    if (action == 1 && !gradient && filledVersion == pathVersion && justAfter(currentFill) && (tp.allClosed() || !G.lastIsKept()) &&
        G.strokeLastPath(p.colour, hw, cap, style())) return true;
    // A big path drawn again as the same shape (in its own space): its kept bands.
    if (!gradient && (action == 0 || tp.allClosed()) && bigCurrent() && uniform(m)) {
      if (KeptCurrent* k = keptCurrent()) {
        const bool ok = action == 0 ? G.pathKept(k->bands, k->local, m, evenOdd, true, p.colour, false, Color{}, 0, style())
                                    : G.pathKept(k->bands, k->local, m, false, false, Color{}, true, p.colour, hw, style());
        if (ok) { if (action == 0) { filledVersion = pathVersion; currentFill = markNow(); } return true; }
      }
    }
    if (!gradient) {
      const bool ok = action == 0 ? G.pathTexels(tp, evenOdd, true, p.colour, false, Color{}, 0, cap, style())
                                  : G.pathTexels(tp, false, false, Color{}, true, p.colour, hw, cap, style());
      if (ok) { if (action == 0) { filledVersion = pathVersion; currentFill = markNow(); } return true; }
    }
    const Poly& device = currentDevice();
    // Gradients, and paths too big to draw whole: the exact way.
    if (action == 0) {
      float X0, Y0, X1, Y1;
      if (!evenOdd && canvas_detail::polyIsRect(device, X0, Y0, X1, Y1)) return G.rect(X0, Y0, X1, Y1, style(), p.colour);
      return G.poly(device, evenOdd, style(), p.colour);
    }
    Matrix inv;
    if (!canvas_detail::invert(m, inv)) return true;
    static thread_local Poly local;
    transformPoly(device, inv, local);
    return strokeLocal(local, p);
  }
  // ----- big current paths drawn again -----
  // A path built anew each frame under a transform (beginPath, arcs, fill)
  // is the same shape in its own space each time. Taken back there, its
  // points find the last frame's (to a fiftieth of a pixel), whose kept
  // bands draw it with no banding each frame.
  struct KeptCurrent { Poly local; canvas_detail::GpuSurface::KeptPath bands; std::uint64_t lastFrame = 0; bool again = false; };
  std::unordered_map<std::uint64_t, KeptCurrent> currents;
  // Whether the current path is one the GPU would cut into bands.
  bool bigCurrent() {
    const std::size_t texels = tp.drawable() / 2;
    const float hgt = tp.y1 - tp.y0, area = (tp.x1 - tp.x0 + 2) * (hgt + 2);
    return texels > 20 && hgt > 2 * canvas_detail::GpuSurface::kBandRows && static_cast<float>(texels) * area > 40000.f;
  }
  KeptCurrent* keptCurrent() {
    Matrix inv;
    if (!canvas_detail::invert(m, inv)) return nullptr;
    const float scale = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
    static thread_local Poly here;
    here.reset();
    const std::size_t end = tp.drawable();
    float lx0 = 1e30f, ly0 = 1e30f, lx1 = -1e30f, ly1 = -1e30f;
    std::uint64_t key = 0x9E3779B97F4A7C15ull;
    for (std::size_t i = 0; i < end;) {
      const int k = static_cast<int>(tp.buf[i]);
      const bool closed = tp.buf[i + 1] > 0.5f;
      i += 2;
      for (int j = 0; j < k; ++j, i += 2) {
        const float X = tp.buf[i], Y = tp.buf[i + 1];
        const float x = inv[0] * X + inv[2] * Y + inv[4], y = inv[1] * X + inv[3] * Y + inv[5];
        here.pts.push_back(x); here.pts.push_back(y);
        lx0 = std::min(lx0, x); lx1 = std::max(lx1, x); ly0 = std::min(ly0, y); ly1 = std::max(ly1, y);
      }
      here.ends.push_back(here.points());
      here.closed.push_back(closed ? 1 : 0);
      key = (key ^ (static_cast<std::uint64_t>(k) << 1 | (closed ? 1u : 0u))) * 0xff51afd7ed558ccdull;
    }
    if (!(lx0 <= lx1) || !std::isfinite(lx0 + lx1 + ly0 + ly1 + scale)) return nullptr;
    // Its bounds to a quarter pixel, and its scale to an eighth of a doubling.
    const float q = 4 * scale;
    for (float v : {lx0, ly0, lx1, ly1}) key = (key ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(v * q + 0.5f)))) * 0xc4ceb9fe1a85ec53ull;
    key ^= static_cast<std::uint64_t>(std::lround(std::log2(scale) * 8)) << 48;
    KeptCurrent& c = currents[key];
    const float tol = 0.02f / scale;
    bool same = c.local.pts.size() == here.pts.size() && c.local.ends == here.ends && c.local.closed == here.closed;
    for (std::size_t i = 0; same && i < here.pts.size(); ++i) same = std::abs(here.pts[i] - c.local.pts[i]) <= tol;
    if (!same) {
      // New (or another shape by the same key): seen once, not kept yet.
      std::swap(c.local, here);
      c.bands = canvas_detail::GpuSurface::KeptPath{};
      c.lastFrame = frame + 1; c.again = false;
      return nullptr;
    }
    if (c.lastFrame != frame + 1) { c.again = true; c.lastFrame = frame + 1; }
    return c.again ? &c : nullptr;
  }
  void currentCircleRing(Poly& ring) const {
    ring.reset();
    const int n = std::max(8, std::min(256, static_cast<int>(std::ceil(6.2831853f * std::sqrt(std::max(circleR, 1.f)) * 1.1f))));
    for (int k = 0; k < n; ++k) { const float t = 6.2831853f * k / n; ring.pts.push_back(circleX + circleR * std::cos(t)); ring.pts.push_back(circleY + circleR * std::sin(t)); }
    ring.finish(true);
  }
  // The current path in user space, for a replay.
  void currentLocal(Path2D& out) {
    const Matrix inv = invertOr(m);
    if (isCircle) {
      out = Path2D{};
      const float s = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
      out.arc(inv[0] * circleX + inv[2] * circleY + inv[4], inv[1] * circleX + inv[3] * circleY + inv[5], s > 0 ? circleR / s : 0, 0, 6.2831853f, false);
      return;
    }
    static thread_local Poly local;
    transformPoly(currentDevice(), inv, local);
    out = Path2D{};
    int first = 0;
    for (std::size_t c = 0; c < local.ends.size(); ++c) {
      const int last = local.ends[c];
      for (int k = first; k < last; ++k) k == first ? out.moveTo(local.pts[2 * k], local.pts[2 * k + 1]) : out.lineTo(local.pts[2 * k], local.pts[2 * k + 1]);
      if (local.closed[c]) out.closePath();
      first = last;
    }
  }

  // ----- paths, kept by their geometry -----
  // A path (the current one, or a Path2D) drawn every frame in its own
  // coordinates is flattened and sorted out once: by a hash of its segments,
  // checked whole on a hit.
  struct CachedPath {
    std::vector<Path2D::Segment> segs;
    Poly local;
    float scale = 0;
    int shape = -1;
    bool circle = false;
    float cx = 0, cy = 0, r = 0;
    bool allClosed = false;               // of `local`
    std::uint64_t lastFrame = 0;          // the frame it was last drawn in
    bool again = false;                   // drawn in more than one frame
    canvas_detail::GpuSurface::KeptPath bands;  // `local` kept on the GPU
  };
  std::unordered_map<std::uint64_t, CachedPath> paths;
  static bool sameSegments(const std::vector<Path2D::Segment>& a, const Path2D::Segment* b, std::size_t n) {
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i)
      if (a[i].command != b[i].command || a[i].counterClockwise != b[i].counterClockwise || std::memcmp(a[i].v, b[i].v, sizeof a[i].v) != 0) return false;
    return true;
  }
  CachedPath& kept(const Path2D::Segment* g, std::size_t n) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    for (std::size_t i = 0; i < n; ++i) {
      h = (h ^ (static_cast<std::uint64_t>(g[i].command) << 1 | g[i].counterClockwise)) * 0xff51afd7ed558ccdull;
      std::uint32_t words[8];
      std::memcpy(words, g[i].v, sizeof words);
      for (std::uint32_t wd : words) h = (h ^ wd) * 0xc4ceb9fe1a85ec53ull;
    }
    auto it = paths.find(h);
    if (it != paths.end() && sameSegments(it->second.segs, g, n)) return it->second;
    CachedPath& c = paths[h];
    c = CachedPath{};
    c.segs.assign(g, g + n);
    if (n == 1 && g[0].command == Path2D::Command::Arc && std::abs(g[0].v[4] - g[0].v[3]) >= 6.2831852f) {
      c.circle = true; c.cx = g[0].v[0]; c.cy = g[0].v[1]; c.r = g[0].v[2];
    } else if (n == 2 && g[0].command == Path2D::Command::Move && g[1].command == Path2D::Command::Arc &&
               std::abs(g[1].v[4] - g[1].v[3]) >= 6.2831852f &&
               std::abs(g[0].v[0] - (g[1].v[0] + g[1].v[2] * std::cos(g[1].v[3]))) < 1e-3f &&
               std::abs(g[0].v[1] - (g[1].v[1] + g[1].v[2] * std::sin(g[1].v[3]))) < 1e-3f) {
      // moveTo the arc's own start, then the whole arc: just the circle.
      c.circle = true; c.cx = g[1].v[0]; c.cy = g[1].v[1]; c.r = g[1].v[2];
    }
    return c;
  }
  // A Path2D object by its identity: its key and revision (a hit costs no
  // hashing); a miss goes to the geometry hash, so a path made anew each
  // frame still finds its outline.
  struct KeyEntry { std::uint32_t revision; CachedPath* path; };
  std::unordered_map<std::uint32_t, KeyEntry> byKey;
  CachedPath& keptPath(const Path2D& p) {
    const std::uint32_t key = p.cacheKey();
    auto it = byKey.find(key);
    if (it != byKey.end() && it->second.revision == p.revision()) return *it->second.path;
    CachedPath& c = kept(p.segments().data(), p.segments().size());
    byKey[key] = KeyEntry{p.revision(), &c};
    return c;
  }
  const Poly& outline(CachedPath& c, float scale) {
    // Flattened again only finer: a path drawn at two sizes would otherwise
    // be flattened anew at each, every frame (and its kept bands with it).
    if (c.scale == 0 || scale > c.scale * 1.5f) {
      canvas_detail::flatten(c.segs.data(), c.segs.size(), Matrix{1, 0, 0, 1, 0, 0}, scale, c.local);
      c.scale = scale;
      c.bands = canvas_detail::GpuSurface::KeptPath{};
      c.allClosed = true;
      for (char k : c.local.closed) c.allClosed = c.allClosed && k;
    }
    return c.local;
  }
  // What shape it is, for a fill: worked out once.
  int shapeOf(CachedPath& c) {
    if (c.shape < 0) c.shape = GpuSurface::convexContour(c.local) ? GpuSurface::kShapeConvex : GpuSurface::simpleContour(c.local) ? GpuSurface::kShapeSimple : GpuSurface::kShapeGeneral;
    return c.shape;
  }

  // ----- drawing on the GPU -----
  // A path (segments in the user space of `pm`) filled (0), stroked (1) or
  // clipped to (2). False if the GPU cannot draw it.
  std::uint32_t filledKey = 0, filledRevision = 0;
  Matrix filledMatrix{};
  bool path(const Path2D& p2, const Matrix& pm, int action, bool evenOdd) {
    if (p2.empty()) return true;
    CachedPath& c = keptPath(p2);
    if (action == 2) {
      static thread_local Poly device;
      transformPoly(outline(c, canvas_detail::matrixMaxScale(pm)), pm, device);
      clipTo(device, evenOdd);
      return true;
    }
    if (!plain()) return false;
    const Paint p = action == 0 ? fillPaint() : strokePaint();
    if (p.kind == PaintKind::Other) return false;
    if (!usePaint(p)) return false;
    if (p.kind == PaintKind::Solid && p.colour.a == 0) return true;
    const float k = std::sqrt(std::abs(pm[0] * pm[3] - pm[1] * pm[2]));
    if (c.circle && uniform(pm) && (action == 0 || (ws->dash.empty() && uniform(m)))) {
      const float X = pm[0] * c.cx + pm[2] * c.cy + pm[4], Y = pm[1] * c.cx + pm[3] * c.cy + pm[5];
      const float pen = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
      return G.circle(X, Y, c.r * k, action == 0 ? -1.f : ws->lineWidth * 0.5f * pen, style(), p.colour);
    }
    const Poly& local = outline(c, canvas_detail::matrixMaxScale(pm));
    if (p.kind == PaintKind::Solid && (action == 0 || ws->dash.empty())) {
      // As cp6: one instance, the fill or the stroke (round joins); the stroke
      // of the Path2D just filled joins the fill's.
      const int cap = capOf();
      const float pen = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
      const float hw = ws->lineWidth * 0.5f * pen;
      if (action == 1 && filledKey == p2.cacheKey() && filledRevision == p2.revision() && filledMatrix == pm && justAfter(path2dFill) &&
          (c.allClosed || !G.lastIsKept()) && G.strokeLastPath(p.colour, hw, cap, style())) return true;
      // Kept on the GPU in its own space, once it is drawn again in a later
      // frame (a path made anew each frame is not worth keeping): nothing
      // per point each frame.
      if (c.lastFrame != frame + 1) { c.again = c.again || c.lastFrame != 0; c.lastFrame = frame + 1; }
      if (c.again && uniform(pm) && (action == 0 || c.allClosed) &&
          (action == 0 ? G.pathKept(c.bands, local, pm, evenOdd, true, p.colour, false, Color{}, 0, style())
                       : G.pathKept(c.bands, local, pm, false, false, Color{}, true, p.colour, hw, style()))) {
        if (action == 0) { filledKey = p2.cacheKey(); filledRevision = p2.revision(); filledMatrix = pm; path2dFill = markNow(); }
        return true;
      }
      if (action == 0 ? G.pathLocal(local, pm, evenOdd, true, p.colour, false, Color{}, 0, cap, style())
                      : G.pathLocal(local, pm, false, false, Color{}, true, p.colour, hw, cap, style())) {
        if (action == 0) { filledKey = p2.cacheKey(); filledRevision = p2.revision(); filledMatrix = pm; path2dFill = markNow(); }
        return true;
      }
    }
    if (action == 0) {
      static thread_local Poly device;
      transformPoly(local, pm, device);
      float X0, Y0, X1, Y1;
      if (!evenOdd && canvas_detail::polyIsRect(device, X0, Y0, X1, Y1)) return G.rect(X0, Y0, X1, Y1, style(), p.colour);
      return G.poly(device, evenOdd, style(), p.colour, false, shapeOf(c));
    }
    if (pm == m) return strokeLocal(local, p);
    static thread_local Poly device, pen;
    transformPoly(local, pm, device);
    transformPoly(device, invertOr(m), pen);
    return strokeLocal(pen, p);
  }
  // A stroke of `local` (user space of the current transform).
  bool strokeLocal(const Poly& local, const Paint& p) {
    using namespace canvas_detail;
    if (!(ws->lineWidth > 0)) return true;
    static thread_local Poly dashed, outlinePoly, out;
    const int cap = capOf();
    const int join = ws->lineJoin == "round" ? 1 : ws->lineJoin == "bevel" ? 2 : 0;
    const Poly* src = &local;
    if (!ws->dash.empty()) { applyDash(local, ws->dash, ws->dashOffset, dashed); src = &dashed; }
    const float scale = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
    if (uniform(m)) {
      transformPoly(*src, m, out);
      if (G.stroke(out, ws->lineWidth * 0.5f * scale, cap, join, ws->miterLimit, style(), p.colour)) return true;
    }
    strokeOutline(*src, ws->lineWidth / 2, cap, join, ws->miterLimit, matrixMaxScale(m), outlinePoly);
    transformPoly(outlinePoly, m, out);
    return G.poly(out, false, style(), p.colour, true);
  }
  bool fillRect(float x, float y, float rw, float rh) {
    if (!plain()) return false;
    const Paint p = fillPaint();
    if (p.kind == PaintKind::Other) return false;
    if (!usePaint(p)) return false;
    if (p.kind == PaintKind::Solid && p.colour.a == 0) return true;
    float X0, Y0, X1, Y1;
    if (canvas_detail::isAxisAligned(m)) {
      float ax, ay, bx, by;
      ax = m[0] * x + m[4]; bx = m[0] * (x + rw) + m[4]; ay = m[3] * y + m[5]; by = m[3] * (y + rh) + m[5];
      X0 = std::min(ax, bx); X1 = std::max(ax, bx); Y0 = std::min(ay, by); Y1 = std::max(ay, by);
      return G.rect(X0, Y0, X1, Y1, style(), p.colour);
    }
    Path2D box;
    box.rect(x, y, rw, rh);
    static thread_local Poly device;
    canvas_detail::flatten(box.segments().data(), box.segments().size(), m, canvas_detail::matrixMaxScale(m), device);
    return G.poly(device, false, style(), p.colour, false, GpuSurface::kShapeConvex);
  }
  bool strokeRect(float x, float y, float rw, float rh) {
    if (!plain()) return false;
    const Paint p = strokePaint();
    if (p.kind == PaintKind::Other) return false;
    if (!usePaint(p)) return false;
    if (p.kind == PaintKind::Solid && p.colour.a == 0) return true;
    // Axis-aligned with square corners: the ring between two rects, exactly.
    // (Under a pixel wide, Chrome covers a stroke differently: the long way.)
    if (p.kind == PaintKind::Solid && rw != 0 && rh != 0 && canvas_detail::isAxisAligned(m) &&
        ws->lineWidth * std::abs(m[0]) >= 1 && ws->lineWidth * std::abs(m[3]) >= 1 &&
        ws->lineJoin == "miter" && ws->miterLimit >= 1.4143f) {
      const float ax = m[0] * x + m[4], bx = m[0] * (x + rw) + m[4], ay = m[3] * y + m[5], by = m[3] * (y + rh) + m[5];
      const float hx = ws->lineWidth * 0.5f * std::abs(m[0]), hy = ws->lineWidth * 0.5f * std::abs(m[3]);
      const float X0 = std::min(ax, bx), X1 = std::max(ax, bx), Y0 = std::min(ay, by), Y1 = std::max(ay, by);
      const float o[4] = {X0 - hx, Y0 - hy, X1 + hx, Y1 + hy};
      const float in[4] = {X0 + hx, Y0 + hy, std::max(X0 + hx, X1 - hx), std::max(Y0 + hy, Y1 - hy)};
      if (G.rectRing(o, in, style(), p.colour)) return true;
    }
    Path2D box;
    box.rect(x, y, rw, rh);
    static thread_local Poly local;
    canvas_detail::flatten(box.segments().data(), box.segments().size(), Matrix{1, 0, 0, 1, 0, 0}, canvas_detail::matrixMaxScale(m), local);
    // Round joins are the path stroke's own: one instance.
    if (p.kind == PaintKind::Solid && ws->dash.empty() && ws->lineJoin == "round" && uniform(m) &&
        G.pathLocal(local, m, false, false, Color{}, true, p.colour, ws->lineWidth * 0.5f * std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2])), capOf(), style()))
      return true;
    return strokeLocal(local, p);
  }
  bool clearRect(float x, float y, float rw, float rh) {
    if (generic || !canvas_detail::isAxisAligned(m)) return false;
    const float ax = m[0] * x + m[4], bx = m[0] * (x + rw) + m[4], ay = m[3] * y + m[5], by = m[3] * (y + rh) + m[5];
    const auto snap = [](float v) { return std::floor(std::clamp(v, -1e8f, 1e8f) + 0.5f); };
    G.useSolid();
    return G.rect(snap(std::min(ax, bx)), snap(std::min(ay, by)), snap(std::max(ax, bx)), snap(std::max(ay, by)), GpuStyle{1, clip.mask, clip.paths}, Color{0, 0, 0, 255}, true);
  }
  bool image(gl::GLuint tex, int iw, int ih, const float* f, float extraAlpha, bool smoothed) {
    if (!plain()) return false;
    const GpuStyle s = style(extraAlpha);
    if (s.alpha <= 0) return true;
    G.useSolid();
    return G.image(tex, 0, 0, iw, ih, f, m, !smoothed, s.alpha, s);
  }
  bool drawCanvas(int context, const float* f9) {
    if (!plain() || context == shadow) return false;
    SourceTexture* t = canvasTexture(context);
    if (!t) return true;
    return image(t->tex, t->w, t->h, f9, 1, ws->smoothing);
  }
  bool fillText(const std::string& t, float x, float y, float maxWidth) {
    if (!plain() || !canvas_detail::isAxisAligned(m) || !(m[0] > 0) || !(m[3] > 0)) return false;
    const Paint p = fillPaint();
    if (p.kind != PaintKind::Solid) return false;
    if (p.colour.a == 0) return true;
    const float scale = std::max(0.0625f, std::round(std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2])) * 16.f) / 16.f);
    float out[6];
    int placed = c2d_gpu_text(static_cast<int>(textAtlas), kTextAtlas, ws->font.c_str(), t.c_str(), ws->textAlign.c_str(), ws->textBaseline.c_str(), ws->direction.c_str(),
                              ws->letterSpacing.c_str(), ws->wordSpacing.c_str(), ws->fontKerning.c_str(), maxWidth, scale, out);
    if (placed < 0) {
      G.draw();
      c2d_gpu_text_reset();
      placed = c2d_gpu_text(static_cast<int>(textAtlas), kTextAtlas, ws->font.c_str(), t.c_str(), ws->textAlign.c_str(), ws->textBaseline.c_str(), ws->direction.c_str(),
                            ws->letterSpacing.c_str(), ws->wordSpacing.c_str(), ws->fontKerning.c_str(), maxWidth, scale, out);
    }
    if (placed <= 0) return placed == 0;
    const float f[9] = {0, 0, out[2], out[3], x - out[4] / scale, y - out[5] / scale, out[2] / scale, out[3] / scale, 1};
    G.useSolid();
    return G.coverage(textAtlas, out[0], out[1], static_cast<int>(out[2]), static_cast<int>(out[3]), f, m, style(), p.colour);
  }
  void putImage(const std::uint8_t* rgba, int stride, int dx, int dy, int pw, int ph) { G.put(rgba, stride, dx, dy, IBox{dx, dy, dx + pw, dy + ph}); }
  void clearTo(Color c) { G.clearTo(c); }

  // ----- what the GPU does not draw: replayed on the shadow -----
  // The canvas's own code records it (Canvas 2D calls with the state they
  // need), and those records run on the shadow between a copy of the GPU's
  // pixels onto it and a copy back. A layer stays on the shadow from its
  // begin to its end.
  bool generic = false;
  int genericDepth = 0;
  std::vector<OpSlot> replay;
  std::string replayText;
  std::vector<float> replayPaths;
  OpSlot* replayRecord(int code) {
    replay.resize(replay.size() + kOpSlots);
    OpSlot* r = replay.data() + replay.size() - kOpSlots;
    r[0].i = code;
    for (int i = 1; i < kOpSlots; ++i) r[i].i = 0;
    return r;
  }
  void replayPath(const Poly& poly, int action, bool evenOdd) {
    const std::size_t at = replayPaths.size();
    int segs = 0, first = 0;
    for (std::size_t c = 0; c < poly.ends.size(); ++c) {
      const int last = poly.ends[c];
      for (int k = first; k < last; ++k) {
        replayPaths.insert(replayPaths.end(), {static_cast<float>(k == first ? 0 : 1), 0.f, poly.pts[2 * k], poly.pts[2 * k + 1], 0, 0, 0, 0, 0, 0});
        ++segs;
      }
      if (poly.closed[c]) { replayPaths.insert(replayPaths.end(), {9.f, 0, 0, 0, 0, 0, 0, 0, 0, 0}); ++segs; }
      first = last;
    }
    OpSlot* r = replayRecord(53);
    r[1].f = static_cast<float>(action); r[2].f = evenOdd ? 1.f : 0.f;
    r[3].f = static_cast<float>(at); r[4].f = static_cast<float>(segs);
  }
  void sendReplay() {
    if (replay.empty()) return;
    replayText.push_back('\0');
    c2d_flush(shadow, &replay[0].i, static_cast<int>(replay.size() / kOpSlots), replayText.c_str(), replayPaths.empty() ? nullptr : replayPaths.data(), nullptr);
    replay.clear(); replayText.clear(); replayPaths.clear();
  }
  // Before the canvas's code records for the shadow: its pixels there, and
  // the clip (the shadow's own state is the default between replays).
  void beginGeneric() {
    if (generic) { ++genericDepth; return; }
    syncShadow();
    generic = true;
    genericDepth = 1;
    replayRecord(0);
    if (clip.shapes) {
      OpSlot* r = replayRecord(7); r[1].f = 1; r[4].f = 1;
      for (const ClipShape& c : *clip.shapes) replayPath(c.poly, 2, c.evenOdd);
    }
    sendReplay();
  }
  // After: unless a layer is open, the shadow's pixels go back to the GPU.
  void endGeneric(bool layerOpen) {
    if (!generic) return;
    if (--genericDepth > 0 || layerOpen) { genericDepth = std::max(genericDepth, 1); return; }
    replayRecord(1);
    sendReplay();
    c2d_gpu_from_shadow(shadow, static_cast<int>(G.colour));
    generic = false;
    genericDepth = 0;
    shadowStale = false;
  }
};

namespace {
// Every GPU canvas, for the end of the frame.
std::unordered_map<const Canvas*, WebGpu*>& gpuCanvases() { static auto* m = new std::unordered_map<const Canvas*, WebGpu*>(); return *m; }
void presentGpuCanvases() {
  for (auto& [canvas, gpu] : gpuCanvases()) gpu->present();
}
}  // namespace
