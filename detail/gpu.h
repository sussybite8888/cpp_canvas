#pragma once
// Native backend, part 3: drawing a canvas with the GPU (OpenGL 3.3 core).
//
// The approach is florrcustom's cp6 renderer: every shape becomes one
// instanced quad, and the fragment shader works out how much of each pixel
// the shape covers. What is kept from this library is the geometry: paths are
// flattened, and strokes outlined (joins, caps, dashes), by the same code the
// software rasterizer uses, and the shader computes the same exact area
// coverage from the resulting edges -- so the GPU's pixels match the
// software's to within rounding.
//
// A canvas with a GpuSurface keeps its pixels in a texture. Draws the shader
// does not do (gradients, patterns, shadows, filters, composite operations
// other than source-over, text) run in the software rasterizer on a copy of
// the region they touch, which is then put back. Whoever reads the pixels
// (getImageData, a draw from this canvas onto another) gets them read back.
//
// Included only by canvas.cpp, after soft.h.

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#endif

namespace canvas_detail {
namespace gl {
// The handful of GL types and constants used, so no platform GL header is
// needed: the functions come from the window's loader (SDL_GL_GetProcAddress).
using GLenum = unsigned int; using GLuint = unsigned int; using GLint = int; using GLsizei = int;
using GLboolean = unsigned char; using GLbitfield = unsigned int; using GLfloat = float; using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
constexpr GLenum TEXTURE_2D = 0x0DE1, RGBA = 0x1908, RGBA8 = 0x8058, UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406, SHORT = 0x1402, UNSIGNED_SHORT = 0x1403,
    RGBA32F = 0x8814, R8 = 0x8229, RED = 0x1903, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800,
    NEAREST = 0x2600, LINEAR = 0x2601, TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F,
    FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5, ARRAY_BUFFER = 0x8892,
    STREAM_DRAW = 0x88E0, TRIANGLE_STRIP = 0x0005, BLEND = 0x0BE2, ONE = 1, ZERO = 0,
    ONE_MINUS_SRC_ALPHA = 0x0303, SRC_COLOR = 0x0300, VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30, COMPILE_STATUS = 0x8B81,
    LINK_STATUS = 0x8B82, TEXTURE0 = 0x84C0, UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05,
    SCISSOR_TEST = 0x0C11, COLOR_BUFFER_BIT = 0x4000, MAX_TEXTURE_SIZE = 0x0D33, UNPACK_ROW_LENGTH = 0x0CF2,
    PACK_ROW_LENGTH = 0x0D02, RENDERBUFFER = 0x8D41, DEPTH_COMPONENT24 = 0x81A6, DEPTH_ATTACHMENT = 0x8D00,
    DEPTH_BUFFER_BIT = 0x0100, LESS = 0x0201, DEPTH_TEST = 0x0B71, CULL_FACE = 0x0B44, DYNAMIC_DRAW = 0x88E8;

#define CPP_CANVAS_GL_FUNCTIONS_ES(X) \
    X(void, GenTextures, (GLsizei, GLuint*)) X(void, DeleteTextures, (GLsizei, const GLuint*)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) X(void, PixelStorei, (GLenum, GLint)) \
    X(void, ActiveTexture, (GLenum)) X(void, GenFramebuffers, (GLsizei, GLuint*)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*)) X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, Clear, (GLbitfield)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, CompileShader, (GLuint)) X(void, GetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) X(GLuint, CreateProgram, ()) \
    X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*)) X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, UseProgram, (GLuint)) X(GLint, GetUniformLocation, (GLuint, const GLchar*)) \
    X(void, Uniform1i, (GLint, GLint)) X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, GenVertexArrays, (GLsizei, GLuint*)) X(void, BindVertexArray, (GLuint)) \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*)) X(void, GenBuffers, (GLsizei, GLuint*)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint*)) X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum)) X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, BufferSubData, (GLenum, GLsizeiptr, GLsizeiptr, const void*)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void, VertexAttribDivisor, (GLuint, GLuint)) X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) X(void, Finish, ()) \
    X(void, DeleteProgram, (GLuint)) X(void, DeleteShader, (GLuint)) X(void, GetIntegerv, (GLenum, GLint*)) \
    X(GLenum, GetError, ()) X(void, GenRenderbuffers, (GLsizei, GLuint*)) X(void, BindRenderbuffer, (GLenum, GLuint)) \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint*)) X(void, DepthFunc, (GLenum)) X(void, DepthMask, (GLboolean))
#define CPP_CANVAS_GL_FUNCTIONS(X) CPP_CANVAS_GL_FUNCTIONS_ES(X) X(void, ClearDepth, (double))

struct Api {
#define CPP_CANVAS_GL_MEMBER(ret, name, args) ret(*name) args = nullptr;
    CPP_CANVAS_GL_FUNCTIONS(CPP_CANVAS_GL_MEMBER)
#undef CPP_CANVAS_GL_MEMBER
#ifdef __EMSCRIPTEN__
    // The browser's: emscripten's own GL functions, one for one, except
    // glClearDepth, which GLES spells glClearDepthf.
    bool load(void* (*)(const char*)) {
#define CPP_CANVAS_GL_DIRECT(ret, name, args) name = reinterpret_cast<ret(*) args>(&gl##name);
        CPP_CANVAS_GL_FUNCTIONS_ES(CPP_CANVAS_GL_DIRECT)
#undef CPP_CANVAS_GL_DIRECT
        ClearDepth = [](double d) { glClearDepthf(static_cast<float>(d)); };
        return true;
    }
#else
    bool load(void* (*getProc)(const char*)) {
        bool ok = true;
#define CPP_CANVAS_GL_LOAD(ret, name, args) \
        name = reinterpret_cast<ret(*) args>(getProc("gl" #name)); ok = ok && name != nullptr;
        CPP_CANVAS_GL_FUNCTIONS(CPP_CANVAS_GL_LOAD)
#undef CPP_CANVAS_GL_LOAD
        return ok;
    }
#endif
};
}  // namespace gl

// sqrt(x*x + y*y): std::hypot's care for overflow costs a hundred times
// more in WebAssembly, and these are device-pixel distances.
inline float gpuLength(float x, float y) { return std::sqrt(x * x + y * y); }

// What a draw takes from the drawing state besides its paint.
// A clip as cp6 keeps it (the browser's GPU canvas): up to four device-space
// paths and exact circles, tested per pixel, nothing rasterized.
struct GpuClipPaths {
    struct Path { Poly poly; bool evenOdd = false; };
    std::vector<Path> paths;
    struct Circle { float cx, cy, r; bool outside; };
    std::vector<Circle> circles;
    int x0 = -(1 << 28), y0 = -(1 << 28), x1 = 1 << 28, y1 = 1 << 28;  // bounds of what is inside
    std::size_t count() const { return paths.size() + circles.size(); }
};
// A path being built straight into the GPU's layout (cp6's current path), in
// slots of two floats, two to a texel: per subpath a header slot (points,
// closed), then a slot per point (x, y), in device pixels. The open
// subpath's header is filled in when it is drawn.
struct GpuTexPath {
    std::vector<float> buf;
    std::size_t len = 0;       // floats in use
    std::size_t hdr = 0;       // the open subpath's header
    int n = 0;                 // its points
    bool open = false;
    bool anyOpen = false;      // a subpath ended without closePath
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    float fx = 0, fy = 0;      // the open subpath's first point
    void reset() { len = 0; open = false; anyOpen = false; x0 = y0 = 1e30f; x1 = y1 = -1e30f; }
    __attribute__((noinline)) void enlarge(std::size_t k) { buf.resize(std::max(buf.size() * 2, len + k + 1024)); }
    __attribute__((always_inline)) float* grow(std::size_t k) {
        if (len + k > buf.size()) enlarge(k);
        float* o = buf.data() + len;
        len += k;
        return o;
    }
    __attribute__((always_inline)) void box(float X, float Y) { x0 = std::min(x0, X); x1 = std::max(x1, X); y0 = std::min(y0, Y); y1 = std::max(y1, Y); }
    void start(float X, float Y) {
        hdr = len;
        float* o = grow(4);
        o[0] = o[1] = 0; o[2] = X; o[3] = Y;
        n = 1; open = true; fx = X; fy = Y;
        box(X, Y);
    }
    __attribute__((always_inline)) void add(float X, float Y) {
        float* o = grow(2);
        o[0] = X; o[1] = Y;
        ++n;
        box(X, Y);
    }
    void header(bool closed) { buf[hdr] = static_cast<float>(n); buf[hdr + 1] = closed ? 1.f : 0.f; }
    // A lone point is not a subpath anything draws: dropped.
    void finish(bool closed) {
        if (!open) return;
        open = false;
        if (n < 2) { len = hdr; return; }
        header(closed);
        if (!closed) anyOpen = true;
    }
    // The floats a draw takes: the open subpath with its header as it stands.
    std::size_t drawable() {
        if (!open) return len;
        if (n < 2) return hdr;
        header(false);
        return len;
    }
    bool allClosed() const { return !anyOpen && !open; }
    void toPoly(Poly& out) {
        out.reset();
        const std::size_t end = drawable();
        for (std::size_t i = 0; i < end;) {
            const int k = static_cast<int>(buf[i]);
            const bool closed = buf[i + 1] > 0.5f;
            i += 2;
            for (int j = 0; j < k; ++j, i += 2) { out.pts.push_back(buf[i]); out.pts.push_back(buf[i + 1]); }
            out.ends.push_back(out.points());
            out.closed.push_back(closed ? 1 : 0);
        }
    }
};
struct GpuStyle {
    float alpha = 1;
    std::shared_ptr<const ClipMask> clip;
    std::shared_ptr<const GpuClipPaths> clipPaths;
};

// One shape: a quad and what the fragment shader needs to cover it.
struct GpuInst {
    float m[4];        // local -> device, linear part (a b c d)
    float e, f;
    float quad[4];     // x0 y0 x1 y1: local space for images, device for the rest
    float p[4];        // per kind: rect, (edge start, edge count, even-odd), image source rect
    float maskX, maskY;  // device pixel + (maskX, maskY) = its texel in the mask atlas (or the clip list)
    float alpha;
    float z;           // draw order: later is nearer
    std::uint16_t clip[4];  // device box everything is clipped to (inside the canvas)
    std::int16_t img[4];    // image placement in its texture (x y w h), a texel offset, or the stroke colour (0-255)
    std::uint32_t colour;   // straight RGBA
    std::uint8_t kind, flags, hasMask, pad;
};
static_assert(sizeof(GpuInst) == 96, "instance layout");
enum GpuKind { kGpuDevRect = 0, kGpuPoly = 1, kGpuImage = 2, kGpuStroke = 3, kGpuMultiply = 4, kGpuGlyph = 5, kGpuCircle = 6, kGpuPath = 7 };
enum GpuBlend { kBlendOver = 0, kBlendClear = 1, kBlendMultiply = 2 };
enum GpuFlags { kGpuNearest = 1, kGpuAxisAligned = 2, kGpuFillAndStroke = 4, kGpuGradient = 8, kGpuCoverage = 16, kGpuPathKept = 32, kGpuRing = 32, kGpuPathFill = 64, kGpuPathStroke = 128 };

// A shelf-packed atlas, R8 or RGBA8.
struct GpuAtlas {
    gl::GLuint tex = 0;
    int size = 0, x = 0, y = 0, rowH = 0;
    void reset() { x = 0; y = 0; rowH = 0; }
    bool place(int w, int h, int& ox, int& oy) {
        if (w > size || h > size) return false;
        if (x + w > size) { x = 0; y += rowH + 1; rowH = 0; }
        if (y + h > size) return false;
        ox = x; oy = y;
        x += w + 1; rowH = std::max(rowH, h);
        return true;
    }
};

struct GpuSurface {
    gl::Api g;
    int w = 0, h = 0;
    // The shapes' program, and the same with gradients compiled in (kept
    // apart: the gradient code alone slowed every pixel by a third).
    gl::GLuint programGradients = 0;
    // Rects and circles alone: a small shader, for the big ones (glows,
    // backdrops), which the full one's register use would slow.
    gl::GLuint programLean = 0, programShapes = 0;
    gl::GLint uViewLean = -1, uViewShapes = -1;
    gl::GLint uViewGradients = -1;
    gl::GLuint fbo = 0, colour = 0, program = 0, present = 0, vao = 0, vbo = 0, edgeTex = 0, emptyVao = 0;
    gl::GLint uView = -1, presentAlpha = -1;
    int viewW = -1, viewH = -1;  // the size the programs' uView was set to
    // Draws instances from `base` on with the attributes set once (WebGL's
    // base-instance extension), when there is one: else each run sets them.
    void (*drawBaseInstance)(gl::GLenum, gl::GLint, gl::GLsizei, gl::GLsizei, gl::GLuint) = nullptr;
    int edgeRows = 0;
    static constexpr int kEdgeWidth = 2048;
    // Paths kept across frames in their own space (keepPath): x y z w per
    // texel, uploaded from `keptSent` on; all dropped (keptGen moves on) when
    // full.
    std::vector<float> kept;
    std::size_t keptSent = 0;
    gl::GLuint keptTex = 0;
    int keptRows = 0;
    std::uint32_t keptGen = 1;
    static constexpr std::size_t kKeptTexels = 1u << 20;  // 16 MB
    GpuAtlas masks, transient, glyphAtlas;
    // Gradients drawn this batch: a colour ramp per row of `ramps`, and
    // kind and geometry per row of `rampParams`.
    static constexpr int kMaxGradients = 512;
    gl::GLuint ramps = 0, rampParams = 0;
    std::unordered_map<const GradientImpl*, int> rampRow;
    std::vector<std::shared_ptr<const GradientImpl>> rampKeep;
    std::vector<std::uint8_t> rampPixels;
    std::vector<float> rampGeometry;
    // The paint the next instances take: solid (row < 0) or gradient `row`
    // in user space mapped by `paintM`.
    int paintRow = -1;
    Matrix paintM{1, 0, 0, 1, 0, 0};
    gl::GLuint gamma = 0;  // the text coverage ramps (textGammaLut), a row per luminance bucket
    struct GlyphKey {
        const Font* face; std::uint16_t glyph; std::uint32_t size; std::uint8_t phase;
        bool operator==(const GlyphKey& o) const { return face == o.face && glyph == o.glyph && size == o.size && phase == o.phase; }
    };
    struct GlyphKeyHash { std::size_t operator()(const GlyphKey& k) const { return std::hash<const void*>()(k.face) ^ (k.glyph * 0x9E3779B1u) ^ (k.size * 0x85EBCA77u) ^ k.phase; } };
    std::unordered_map<GlyphKey, std::pair<int, int>, GlyphKeyHash> glyphPlaced;
    // Instances blended in order, and those that cover their whole quad with
    // an opaque colour: drawn first with depth, so the GPU never shades what
    // a later opaque one hides (it is what blending everything cost most).
    std::vector<GpuInst> inst, opaque;
    std::uint32_t seq = 0;
    gl::GLuint depth = 0;
    std::vector<float> edges;  // x0 y0 x1 y1 per texel
    struct Batch { int start = 0, count = 0; gl::GLuint image = 0; int blend = kBlendOver; bool gradients = false; };
    std::vector<Batch> batches;
    std::vector<std::uint8_t> progOf;  // per instance, at draw: its program
    std::unordered_map<const ClipMask*, std::pair<int, int>> maskPlaced;
    std::vector<std::shared_ptr<const ClipMask>> maskKeep;
    // Images uploaded once and kept: a source canvas (by its id, at a
    // version) or a kept image copy.
    struct Cached { gl::GLuint tex = 0; int w = 0, h = 0; std::uint64_t version = 0; std::uint64_t used = 0; std::weak_ptr<const ImageCopy> copy; };
    std::unordered_map<std::uint64_t, Cached> canvases;
    std::unordered_map<const ImageCopy*, Cached> copies;
    std::vector<gl::GLuint> oneOff;  // textures for one draw, deleted after the batch runs
    std::uint64_t frame = 0;
    std::vector<std::uint8_t> scratch;
    bool ok = false;

    bool init(void* (*getProc)(const char*), int width, int height, const std::vector<Color>& px) {
#ifdef __EMSCRIPTEN__
        if (!g.load(getProc)) return false;
#else
        if (!getProc || !g.load(getProc)) return false;
#endif
        w = width; h = height;
        gl::GLint maxTex = 0;
        g.GetIntegerv(gl::MAX_TEXTURE_SIZE, &maxTex);
        if (w > maxTex || h > maxTex) return false;
        program = link(kVertex, kFragment);
        const std::string withGradients = std::string("#define GRADIENTS 1\n") + kFragment;
        programGradients = link(kVertex, withGradients.c_str());
        present = link(kPresentVertex, kPresentFragment);
        programLean = link(kVertex, kLeanFragment);
        const std::string shapes = std::string("#define SHAPES 1\n") + kFragment;
        programShapes = link(kVertex, shapes.c_str());
        if (!program || !present || !programGradients || !programLean || !programShapes) return false;
        uViewShapes = g.GetUniformLocation(programShapes, "uView");
        uViewLean = g.GetUniformLocation(programLean, "uView");
        g.UseProgram(programLean);
        g.Uniform1i(g.GetUniformLocation(programLean, "uMask"), 1);
        uView = g.GetUniformLocation(program, "uView");
        uViewGradients = g.GetUniformLocation(programGradients, "uView");
        presentAlpha = g.GetUniformLocation(present, "uKeepAlpha");
        for (const gl::GLuint prog : {program, programGradients, programShapes}) {
            g.UseProgram(prog);
            g.Uniform1i(g.GetUniformLocation(prog, "uEdges"), 0);
            g.Uniform1i(g.GetUniformLocation(prog, "uMask"), 1);
            g.Uniform1i(g.GetUniformLocation(prog, "uImage"), 2);
            g.Uniform1i(g.GetUniformLocation(prog, "uGamma"), 3);
            g.Uniform1i(g.GetUniformLocation(prog, "uRamps"), 4);
            g.Uniform1i(g.GetUniformLocation(prog, "uRampParams"), 5);
            g.Uniform1i(g.GetUniformLocation(prog, "uKept"), 6);
        }
        g.UseProgram(present);
        g.Uniform1i(g.GetUniformLocation(present, "uTex"), 0);
        colour = texture(gl::RGBA8, w, h, gl::NEAREST);
        g.GenFramebuffers(1, &fbo);
        g.BindFramebuffer(gl::FRAMEBUFFER, fbo);
        g.FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, colour, 0);
        g.GenRenderbuffers(1, &depth);
        g.BindRenderbuffer(gl::RENDERBUFFER, depth);
        g.RenderbufferStorage(gl::RENDERBUFFER, gl::DEPTH_COMPONENT24, w, h);
        g.FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, depth);
        if (g.CheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE) return false;
        masks.size = 2048; masks.tex = texture(gl::R8, masks.size, masks.size, gl::NEAREST);
        transient.size = 2048; transient.tex = texture(gl::RGBA8, transient.size, transient.size, gl::NEAREST);
        glyphAtlas.size = 2048; glyphAtlas.tex = texture(gl::R8, glyphAtlas.size, glyphAtlas.size, gl::NEAREST);
        ramps = texture(gl::RGBA8, kGradientLut, kMaxGradients, gl::NEAREST);
        g.GenTextures(1, &rampParams);
        g.BindTexture(gl::TEXTURE_2D, rampParams);
        g.TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(gl::RGBA32F), 2, kMaxGradients, 0, gl::RGBA, gl::FLOAT, nullptr);
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::NEAREST));
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::NEAREST));
        {
            std::vector<std::uint8_t> ramps(256 * 16);
            for (int b = 0; b < 16; ++b) {
                const auto grey = static_cast<std::uint8_t>(std::lround(b * 255.f / 15.f));
                std::memcpy(ramps.data() + b * 256, textGammaLut(Color{grey, grey, grey, 255}), 256);
            }
            gamma = texture(gl::R8, 256, 16, gl::NEAREST);
            g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
            g.TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, 256, 16, gl::RED, gl::UNSIGNED_BYTE, ramps.data());
        }
        g.GenVertexArrays(1, &emptyVao);
        g.GenVertexArrays(1, &vao);
        g.BindVertexArray(vao);
        g.GenBuffers(1, &vbo);
        g.BindBuffer(gl::ARRAY_BUFFER, vbo);
        for (gl::GLuint i = 0; i < 10; ++i) { g.EnableVertexAttribArray(i); g.VertexAttribDivisor(i, 1); }
        g.GenTextures(1, &edgeTex);
        g.GenTextures(1, &keptTex);
        upload(px, IBox{0, 0, w, h});
        ok = true;
        return true;
    }
    ~GpuSurface() {
        if (!g.DeleteTextures) return;
        for (auto& [k, c] : canvases) g.DeleteTextures(1, &c.tex);
        for (auto& [k, c] : copies) g.DeleteTextures(1, &c.tex);
        for (gl::GLuint t : oneOff) g.DeleteTextures(1, &t);
        gl::GLuint t[10] = {colour, masks.tex, transient.tex, edgeTex, glyphAtlas.tex, gamma, ramps, rampParams, keptTex, multiplyTex};
        g.DeleteTextures(multiplyTex ? 10 : 9, t);
        if (fbo) g.DeleteFramebuffers(1, &fbo);
        if (depth) g.DeleteRenderbuffers(1, &depth);
        if (vbo) g.DeleteBuffers(1, &vbo);
        if (vao) g.DeleteVertexArrays(1, &vao);
        if (emptyVao) g.DeleteVertexArrays(1, &emptyVao);
        if (program) g.DeleteProgram(program);
        if (programGradients) g.DeleteProgram(programGradients);
        if (programLean) g.DeleteProgram(programLean);
        if (programShapes) g.DeleteProgram(programShapes);
        if (present) g.DeleteProgram(present);
    }

    gl::GLuint texture(gl::GLenum format, int tw, int th, gl::GLenum filter) {
        gl::GLuint t = 0;
        g.GenTextures(1, &t);
        g.BindTexture(gl::TEXTURE_2D, t);
        const bool rgba = format != gl::R8;
        g.TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(format), tw, th, 0, rgba ? gl::RGBA : gl::RED, gl::UNSIGNED_BYTE, nullptr);
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<gl::GLint>(filter));
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<gl::GLint>(filter));
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<gl::GLint>(gl::CLAMP_TO_EDGE));
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<gl::GLint>(gl::CLAMP_TO_EDGE));
        return t;
    }
#ifdef __EMSCRIPTEN__
    static constexpr const char* kHeader = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
#else
    static constexpr const char* kHeader = "#version 330 core\n";
#endif
    gl::GLuint compile(gl::GLenum type, const char* body) {
        const std::string text = std::string(kHeader) + body;
        const char* src = text.c_str();
        const gl::GLuint s = g.CreateShader(type);
        g.ShaderSource(s, 1, &src, nullptr);
        g.CompileShader(s);
        gl::GLint okay = 0;
        g.GetShaderiv(s, gl::COMPILE_STATUS, &okay);
        if (!okay) { char log[2048]; g.GetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "[canvas gpu] shader: %s\n", log); return 0; }
        return s;
    }
    gl::GLuint link(const char* vs, const char* fs) {
        const gl::GLuint v = compile(gl::VERTEX_SHADER, vs), f = compile(gl::FRAGMENT_SHADER, fs);
        if (!v || !f) return 0;
        const gl::GLuint p = g.CreateProgram();
        g.AttachShader(p, v); g.AttachShader(p, f);
        g.LinkProgram(p);
        g.DeleteShader(v); g.DeleteShader(f);
        gl::GLint okay = 0;
        g.GetProgramiv(p, gl::LINK_STATUS, &okay);
        if (!okay) { char log[2048]; g.GetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "[canvas gpu] link: %s\n", log); return 0; }
        return p;
    }

    // ----- pixels between the texture and the canvas's own copy -----
    // Texture row r is canvas row r; the texture holds premultiplied colour,
    // the canvas straight.
    void upload(const std::vector<Color>& px, IBox b) {
        b = b.intersect({0, 0, w, h});
        if (b.empty()) return;
        const int bw = b.x1 - b.x0;
        scratch.resize(static_cast<std::size_t>(bw) * (b.y1 - b.y0) * 4);
        std::uint8_t* o = scratch.data();
        for (int y = b.y0; y < b.y1; ++y) {
            const Color* row = px.data() + static_cast<std::size_t>(y) * w + b.x0;
            for (int x = 0; x < bw; ++x, o += 4) premultiplyTo(row[x], o);
        }
        g.BindTexture(gl::TEXTURE_2D, colour);
        g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        g.TexSubImage2D(gl::TEXTURE_2D, 0, b.x0, b.y0, bw, b.y1 - b.y0, gl::RGBA, gl::UNSIGNED_BYTE, scratch.data());
    }
    void readback(std::vector<Color>& px, IBox b) {
        b = b.intersect({0, 0, w, h});
        if (b.empty()) return;
        const int bw = b.x1 - b.x0;
        scratch.resize(static_cast<std::size_t>(bw) * (b.y1 - b.y0) * 4);
        g.BindFramebuffer(gl::FRAMEBUFFER, fbo);
        g.PixelStorei(gl::PACK_ALIGNMENT, 1);
        g.ReadPixels(b.x0, b.y0, bw, b.y1 - b.y0, gl::RGBA, gl::UNSIGNED_BYTE, scratch.data());
        const std::uint8_t* s = scratch.data();
        for (int y = b.y0; y < b.y1; ++y) {
            Color* row = px.data() + static_cast<std::size_t>(y) * w + b.x0;
            for (int x = 0; x < bw; ++x, s += 4) row[x] = unpremultiply(s);
        }
    }
    static void premultiplyTo(Color c, std::uint8_t* o) {
        if (c.a == 255) { o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = 255; return; }
        o[0] = static_cast<std::uint8_t>(div255(c.r * c.a)); o[1] = static_cast<std::uint8_t>(div255(c.g * c.a));
        o[2] = static_cast<std::uint8_t>(div255(c.b * c.a)); o[3] = c.a;
    }
    static Color unpremultiply(const std::uint8_t* s) {
        const std::uint32_t a = s[3];
        if (a == 255) return Color{s[0], s[1], s[2], 255};
        if (a == 0) return Color{0, 0, 0, 0};
        const auto ch = [a](std::uint32_t v) { return static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (v * 255 + a / 2) / a)); };
        return Color{ch(s[0]), ch(s[1]), ch(s[2]), static_cast<std::uint8_t>(a)};
    }

    // ----- images -----
    // Straight RGBA `rgba` (iw x ih) into a texture as it is: images stay
    // straight on the GPU, and the shader premultiplies what it samples.
    void fillTexture(gl::GLuint tex, int x, int y, const std::uint8_t* rgba, int iw, int ih) {
        g.BindTexture(gl::TEXTURE_2D, tex);
        g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        g.TexSubImage2D(gl::TEXTURE_2D, 0, x, y, iw, ih, gl::RGBA, gl::UNSIGNED_BYTE, rgba);
    }
    // A source canvas's pixels: its own texture once it has been drawn
    // unchanged in two frames, the transient atlas the first time (a canvas
    // made, drawn once and dropped never costs a texture).
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> sighted;  // id -> (version, frame)
    gl::GLuint canvasImage(std::uint64_t id, std::uint64_t version, const std::vector<Color>& px, int iw, int ih, int& ox, int& oy) {
        ox = oy = 0;
        if (canvases.find(id) == canvases.end()) {
            auto& seen = sighted[id];
            const bool again = seen.second != 0 && seen.first == version && seen.second != frame + 1;
            if (!again) {
                if (seen.first != version || seen.second == 0) seen = {version, frame + 1};
                if (iw <= 512 && ih <= 512) {
                    int ax, ay;
                    if (transient.place(iw, ih, ax, ay)) {
                        fillTexture(transient.tex, ax, ay, reinterpret_cast<const std::uint8_t*>(px.data()), iw, ih);
                        ox = ax; oy = ay;
                        return transient.tex;
                    }
                }
            }
            sighted.erase(id);
        }
        return cachedCanvas(id, version, px, iw, ih);
    }
    gl::GLuint cachedCanvas(std::uint64_t id, std::uint64_t version, const std::vector<Color>& px, int iw, int ih) {
        Cached& c = canvases[id];
        if (!c.tex || c.w != iw || c.h != ih) {
            if (c.tex) g.DeleteTextures(1, &c.tex);
            c.tex = texture(gl::RGBA8, iw, ih, gl::NEAREST);
            c.w = iw; c.h = ih; c.version = ~0ull;
        }
        if (c.version != version) {
            fillTexture(c.tex, 0, 0, reinterpret_cast<const std::uint8_t*>(px.data()), iw, ih);
            c.version = version;
        }
        c.used = frame;
        return c.tex;
    }
    gl::GLuint cachedCopy(const std::shared_ptr<const ImageCopy>& img) {
        Cached& c = copies[img.get()];
        if (c.tex && (c.copy.expired() || c.copy.lock() != img)) { g.DeleteTextures(1, &c.tex); c = Cached{}; }
        if (!c.tex) {
            c.tex = texture(gl::RGBA8, img->w, img->h, gl::NEAREST);
            c.w = img->w; c.h = img->h; c.copy = img;
            fillTexture(c.tex, 0, 0, img->rgba.data(), img->w, img->h);
        }
        c.used = frame;
        return c.tex;
    }
    // Pixels used once: into the transient atlas, or a texture of their own.
    gl::GLuint oneOffImage(const std::uint8_t* rgba, int iw, int ih, int& ox, int& oy) {
        if (!transient.place(iw, ih, ox, oy)) {
            ox = oy = 0;
            const gl::GLuint t = texture(gl::RGBA8, iw, ih, gl::NEAREST);
            fillTexture(t, 0, 0, rgba, iw, ih);
            oneOff.push_back(t);
            return t;
        }
        fillTexture(transient.tex, ox, oy, rgba, iw, ih);
        return transient.tex;
    }
    // Textures not drawn for a while go.
    void evict() {
        ++frame;
        for (auto it = canvases.begin(); it != canvases.end();) {
            if (frame - it->second.used > 120) { g.DeleteTextures(1, &it->second.tex); it = canvases.erase(it); } else ++it;
        }
        if (sighted.size() > 4096) sighted.clear();
        for (auto it = copies.begin(); it != copies.end();) {
            if (frame - it->second.used > 120 || it->second.copy.expired()) { g.DeleteTextures(1, &it->second.tex); it = copies.erase(it); } else ++it;
        }
    }

    // ----- clip masks -----
    // Where clip mask `c` sits in the mask atlas, uploading it the first time
    // this batch needs it. False if it does not fit.
    bool placeMask(const std::shared_ptr<const ClipMask>& c, int& ox, int& oy) {
        if (const auto it = maskPlaced.find(c.get()); it != maskPlaced.end()) { ox = it->second.first; oy = it->second.second; return true; }
        const int mw = c->x1 - c->x0, mh = c->y1 - c->y0;
        if (!masks.place(mw, mh, ox, oy)) {
            if (mw > masks.size || mh > masks.size) return false;
            draw();  // the atlas is full: what uses it now goes first
            if (!masks.place(mw, mh, ox, oy)) return false;
        }
        g.BindTexture(gl::TEXTURE_2D, masks.tex);
        g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        g.TexSubImage2D(gl::TEXTURE_2D, 0, ox, oy, mw, mh, gl::RED, gl::UNSIGNED_BYTE, c->alpha.data());
        maskPlaced.emplace(c.get(), std::make_pair(ox, oy));
        maskKeep.push_back(c);
        return true;
    }

    // ----- recording -----
    static constexpr std::uint32_t kMaxSeq = (1u << 23) - 2;
    float nextZ() {
        if (seq >= kMaxSeq) draw();
        return 1.f - static_cast<float>(++seq) / static_cast<float>(1u << 23);
    }
    GpuInst& push(gl::GLuint image = 0, int blend = kBlendOver) {
        const float z = nextZ();
        if (!image && blend == kBlendOver && paintRow < 0 && !batches.empty() && batches.back().blend == kBlendOver && !batches.back().gradients) {
            // The common case: a plain instance onto a plain batch.
            ++batches.back().count;
            inst.emplace_back();
            zPending = z;
            return inst.back();
        }
        const bool gradients = paintRow >= 0;
        // Image 0: an instance that samples no texture, at home in any batch;
        // a batch of those takes on the first texture that comes.
        if (batches.empty() || batches.back().blend != blend || batches.back().gradients != gradients ||
            (image && batches.back().image && batches.back().image != image))
            batches.push_back(Batch{static_cast<int>(inst.size()), 0, image, blend, gradients});
        else if (image && !batches.back().image) batches.back().image = image;
        ++batches.back().count;
        inst.emplace_back();
        zPending = z;
        return inst.back();
    }
    float zPending = 0;
    // Stamps the instance just filled in with its order (filling it in
    // overwrites the whole struct).
    void stamp(GpuInst& i) { i.z = zPending; }
    GpuInst& pushOpaque() {
        zPending = nextZ();
        opaque.emplace_back();
        return opaque.back();
    }
    bool opaqueStyle(const GpuInst& probe, const GpuStyle& s, Color c) const { return paintRow < 0 && c.a == 255 && s.alpha >= 1.f && probe.hasMask < 0.5f; }
    static bool whole(float v) { return v == std::floor(v); }
    // Clip and colour common to every kind.
    // Where a draw may land when it has no mask clip (which setClip may fail on).
    IBox limitOf(const GpuStyle& s) const {
        IBox limit{0, 0, w, h};
        if (s.clipPaths && s.clipPaths->count()) limit = limit.intersect({s.clipPaths->x0, s.clipPaths->y0, s.clipPaths->x1, s.clipPaths->y1});
        return limit;
    }
    // No clip at all (the common case) inline; the rest in setClipAny.
    __attribute__((always_inline)) bool setClip(GpuInst& i, const GpuStyle& s, IBox& limit) {
        if (!s.clip && !s.clipPaths) {
            limit = IBox{0, 0, w, h};
            i.hasMask = 0; i.maskX = i.maskY = 0;
            i.clip[0] = 0; i.clip[1] = 0;
            i.clip[2] = static_cast<std::uint16_t>(std::min(w, 65535)); i.clip[3] = static_cast<std::uint16_t>(std::min(h, 65535));
            return true;
        }
        return setClipAny(i, s, limit);
    }
    bool setClipAny(GpuInst& i, const GpuStyle& s, IBox& limit) {
        limit = IBox{0, 0, w, h};
        i.hasMask = 0; i.maskX = i.maskY = 0;
        if (s.clip) {
            limit = limit.intersect({s.clip->x0, s.clip->y0, s.clip->x1, s.clip->y1});
            if (!s.clip->rect && !limit.empty()) {
                int ox, oy;
                if (!placeMask(s.clip, ox, oy)) return false;
                i.hasMask = 1;
                i.maskX = static_cast<float>(ox - s.clip->x0);
                i.maskY = static_cast<float>(oy - s.clip->y0);
            }
        }
        if (s.clipPaths && s.clipPaths->count()) {
            const GpuClipPaths& c = *s.clipPaths;
            limit = limit.intersect({c.x0, c.y0, c.x1, c.y1});
            i.hasMask = 2;
            i.maskX = static_cast<float>(clipList(s.clipPaths));
            i.maskY = static_cast<float>(c.count());
        }
        const auto u16 = [](int v) { return static_cast<std::uint16_t>(std::clamp(v, 0, 65535)); };
        i.clip[0] = u16(limit.x0); i.clip[1] = u16(limit.y0); i.clip[2] = u16(limit.x1); i.clip[3] = u16(limit.y1);
        return true;
    }
    // ----- cp6's paths: subpath headers (count, closed) then points, a
    // texel each, in the edge texture -----
    std::uint64_t drawGen = 1;  // which draw the edge buffer is filling
    // A path in slots (see GpuTexPath), from a texel boundary: its first slot;
    // `slots` gets how many (the padding to a whole texel not counted).
    int pushPath(const Poly& pl, int& slots) {
        const std::size_t at = edges.size();
        const std::size_t floats = 2 * (pl.ends.size() + static_cast<std::size_t>(pl.ends.empty() ? 0 : pl.ends.back()));
        edges.resize(at + floats + 2);
        float* o = edges.data() + at;
        int first = 0;
        for (std::size_t c = 0; c < pl.ends.size(); ++c) {
            const int last = pl.ends[c];
            if (last - first >= 1) {
                o[0] = static_cast<float>(last - first); o[1] = pl.closed[c] ? 1.f : 0.f; o += 2;
                for (int k = first; k < last; ++k) { o[0] = pl.pts[2 * k]; o[1] = pl.pts[2 * k + 1]; o += 2; }
            }
            first = last;
        }
        const std::size_t used = static_cast<std::size_t>(o - edges.data()) - at;
        slots = static_cast<int>(used / 2);
        edges.resize(at + ((used + 3) & ~static_cast<std::size_t>(3)));
        return static_cast<int>(at / 2);
    }
    // Where clip `c` sits in this draw's edge buffer: a texel per clip
    // (offset, texels, kind: 0 nonzero, 1 even-odd, 2 circle, 3 outside one).
    std::unordered_map<const GpuClipPaths*, std::pair<std::uint64_t, int>> clipLists;
    std::vector<std::shared_ptr<const GpuClipPaths>> clipKeep;
    int clipList(const std::shared_ptr<const GpuClipPaths>& c) {
        auto it = clipLists.find(c.get());
        if (it != clipLists.end() && it->second.first == drawGen) return it->second.second;
        std::vector<float> list;
        for (const auto& p : c->paths) {
            int slots = 0;
            const int at = pushPath(p.poly, slots);
            list.insert(list.end(), {static_cast<float>(at), static_cast<float>(slots), p.evenOdd ? 1.f : 0.f, 0.f});
        }
        for (const auto& k : c->circles) {
            const int at = static_cast<int>(edges.size() / 4);
            edges.insert(edges.end(), {k.cx, k.cy, k.r, 0.f});
            list.insert(list.end(), {static_cast<float>(at), 1.f, k.outside ? 3.f : 2.f, 0.f});
        }
        const int at = static_cast<int>(edges.size() / 4);
        edges.insert(edges.end(), list.begin(), list.end());
        clipLists[c.get()] = {drawGen, at};
        clipKeep.push_back(c);
        return at;
    }
    // A path, filled and/or stroked (round joins; caps 0 butt 1 round 2
    // square), as cp6 draws it: one instance, the fill under the stroke.
    // False, nothing drawn, if it is too big for that (the caller has
    // poly() and stroke()).
    static constexpr long long kPathBudget = 6 << 20;  // texels x pixels
    static constexpr int kBandRows = 16;
    bool path(const Poly& pl, bool evenOdd, bool fill, Color fc, bool doStroke, Color sc, float hw, int cap, const GpuStyle& s) {
        if (pl.pts.empty()) return true;
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (std::size_t k = 0; k < pl.pts.size(); k += 2) {
            bx0 = std::min(bx0, pl.pts[k]); bx1 = std::max(bx1, pl.pts[k]);
            by0 = std::min(by0, pl.pts[k + 1]); by1 = std::max(by1, pl.pts[k + 1]);
        }
        if (!(bx0 <= bx1) || !std::isfinite(bx0 + bx1 + by0 + by1)) return true;
        const float reach = (doStroke ? hw * (cap == 2 ? 1.5f : 1.f) : 0.f) + 1.f;
        GpuInst probe{};
        IBox limit;
        if (!setClip(probe, s, limit)) return false;
        const auto clampI = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
        const IBox q = IBox{clampI(std::floor(bx0 - reach)), clampI(std::floor(by0 - reach)), clampI(std::ceil(bx1 + reach)), clampI(std::ceil(by1 + reach))}.intersect(limit);
        if (q.empty()) return true;
        const long long texels = static_cast<long long>(pl.pts.size() / 2 + pl.ends.size());
        bool allClosed = true;
        for (char c : pl.closed) allClosed = allClosed && c;
        if (texels > 20 && std::max(q.x1 - q.x0, q.y1 - q.y0) > 2 * kBandRows && texels * q.area() > 40000) {
            const bool flagged = doStroke && !allClosed;
            bandSegs.clear(); bandFlags.clear();
            int first = 0;
            for (std::size_t c = 0; c < pl.ends.size(); ++c) {
                bandSubpath(pl.pts.data() + 2 * first, pl.ends[c] - first, pl.closed[c], flagged, fill);
                first = pl.ends[c];
            }
            if (bandPath(evenOdd, fill, fc, doStroke, sc, hw, cap, flagged, q, reach, probe, s)) return true;
        }
        if (texels * q.area() > kPathBudget) return false;
        int slots = 0;
        const int at = pushPath(pl, slots);
        pathInstance(probe, q, at, slots, evenOdd, fill, fc, doStroke, sc, hw, cap, s);
        return true;
    }
    // The current path as built (GpuTexPath): copied in as it is.
    bool pathTexels(GpuTexPath& t, bool evenOdd, bool fill, Color fc, bool doStroke, Color sc, float hw, int cap, const GpuStyle& s) {
        const std::size_t floats = t.drawable();
        if (!floats) return true;
        if (!(t.x0 <= t.x1) || !std::isfinite(t.x0 + t.x1 + t.y0 + t.y1)) return true;
        const float reach = (doStroke ? hw * (cap == 2 ? 1.5f : 1.f) : 0.f) + 1.f;
        GpuInst probe;  // (filled in only for a mask clip)
        IBox limit;
        if (s.clip) { probe = GpuInst{}; if (!setClip(probe, s, limit)) return false; }
        else limit = limitOf(s);
        const auto clampI = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
        const IBox q = IBox{clampI(std::floor(t.x0 - reach)), clampI(std::floor(t.y0 - reach)), clampI(std::ceil(t.x1 + reach)), clampI(std::ceil(t.y1 + reach))}.intersect(limit);
        if (q.empty()) return true;
        const long long texels = static_cast<long long>(floats / 2);  // slots, as texels count for the budget
        if (texels > 20 && std::max(q.x1 - q.x0, q.y1 - q.y0) > 2 * kBandRows && texels * q.area() > 40000) {
            const bool flagged = doStroke && !t.allClosed();
            bandSlots(t.buf.data(), floats, flagged, fill);
            if (!s.clip) { probe = GpuInst{}; setClip(probe, s, limit); }
            if (bandPath(evenOdd, fill, fc, doStroke, sc, hw, cap, flagged, q, reach, probe, s)) return true;
        }
        if (texels * q.area() > kPathBudget) return false;
        const std::size_t at = edges.size();
        edges.insert(edges.end(), t.buf.data(), t.buf.data() + floats);
        if (floats & 3) edges.insert(edges.end(), 4 - (floats & 3), 0.f);
        pathInstance(probe, q, static_cast<int>(at / 2), static_cast<int>(floats / 2), evenOdd, fill, fc, doStroke, sc, hw, cap, s);
        return true;
    }
    // A path in its own space (`pl` under `M`), transformed straight into the
    // edge buffer as it is copied (cp6's uploadPath).
    bool pathLocal(const Poly& pl, const Matrix& M, bool evenOdd, bool fill, Color fc, bool doStroke, Color sc, float hw, int cap, const GpuStyle& s) {
        if (pl.pts.empty()) return true;
        const std::size_t at = edges.size();
        const std::size_t most = 2 * (pl.ends.size() + static_cast<std::size_t>(pl.points()));
        edges.resize(at + ((most + 3) & ~static_cast<std::size_t>(3)));
        float* o = edges.data() + at;
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        bool allClosed = true;
        int first = 0;
        for (std::size_t c = 0; c < pl.ends.size(); ++c) {
            const int last = pl.ends[c];
            if (last > first) {
                o[0] = static_cast<float>(last - first); o[1] = pl.closed[c] ? 1.f : 0.f; o += 2;
                allClosed = allClosed && pl.closed[c];
                for (int k = first; k < last; ++k, o += 2) {
                    const float x = pl.pts[2 * k], y = pl.pts[2 * k + 1];
                    const float X = M[0] * x + M[2] * y + M[4], Y = M[1] * x + M[3] * y + M[5];
                    o[0] = X; o[1] = Y;
                    bx0 = std::min(bx0, X); bx1 = std::max(bx1, X); by0 = std::min(by0, Y); by1 = std::max(by1, Y);
                }
            }
            first = last;
        }
        const std::size_t used = static_cast<std::size_t>(o - (edges.data() + at));
        const auto undo = [&] { edges.resize(at); };
        if (!(bx0 <= bx1) || !std::isfinite(bx0 + bx1 + by0 + by1)) { undo(); return true; }
        const float reach = (doStroke ? hw * (cap == 2 ? 1.5f : 1.f) : 0.f) + 1.f;
        GpuInst probe;  // (filled in only for a mask clip)
        IBox limit;
        if (s.clip) { probe = GpuInst{}; if (!setClip(probe, s, limit)) { undo(); return false; } }
        else limit = limitOf(s);
        const auto clampI = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
        const IBox q = IBox{clampI(std::floor(bx0 - reach)), clampI(std::floor(by0 - reach)), clampI(std::ceil(bx1 + reach)), clampI(std::ceil(by1 + reach))}.intersect(limit);
        if (q.empty()) { undo(); return true; }
        const long long texels = static_cast<long long>(used / 2);
        if (texels > 20 && std::max(q.x1 - q.x0, q.y1 - q.y0) > 2 * kBandRows && texels * q.area() > 40000) {
            // Its segments, from what was just written; then the bands instead.
            const bool flagged = doStroke && !allClosed;
            bandSlots(edges.data() + at, used, flagged, fill);
            undo();
            if (!s.clip) { probe = GpuInst{}; setClip(probe, s, limit); }
            return bandPath(evenOdd, fill, fc, doStroke, sc, hw, cap, flagged, q, reach, probe, s);
        }
        if (texels * q.area() > kPathBudget) { undo(); return false; }
        edges.resize(at + ((used + 3) & ~static_cast<std::size_t>(3)));
        pathInstance(probe, q, static_cast<int>(at / 2), static_cast<int>(used / 2), evenOdd, fill, fc, doStroke, sc, hw, cap, s);
        return true;
    }
    void pathInstance(const GpuInst& probe, const IBox& q, int at, int slots, bool evenOdd, bool fill, Color fc, bool doStroke, Color sc, float hw, int cap, const GpuStyle& s) {
        GpuInst& i = push();
        if (s.clip) i = probe;
        else { IBox limit; setClip(i, s, limit); }
        stamp(i); identity(i);
        i.kind = kGpuPath;
        i.flags = static_cast<float>((fill ? kGpuPathFill : 0) | (doStroke ? kGpuPathStroke : 0));
        i.e = hw;
        i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
        i.p[0] = static_cast<float>(at); i.p[1] = static_cast<float>(slots); i.p[2] = evenOdd ? 1.f : 0.f; i.p[3] = static_cast<float>(cap);
        i.img[0] = sc.r; i.img[1] = sc.g; i.img[2] = sc.b; i.img[3] = sc.a;
        i.alpha = s.alpha; i.colour = pack(fc);
        lastPath = fill && !doStroke ? inst.size() : 0;
    }
    // The stroke of the path just filled (cp6's fill then stroke), into its
    // instance. False if that instance is not one it can join.
    std::size_t lastPath = 0;
    // A path cut into bands of rows, each with only the segments that reach
    // it: a row's winding needs only the segments crossing it, and coverage
    // only those within reach.
    // A stroke with open subpaths has a second texel per segment
    // (bandFlags): (starts an open subpath, ends one, only filled, 0) -- the
    // caps, and the closing segment a fill adds that is not stroked.
    std::vector<float> bandSegs, bandFlags;
    std::vector<int> bandCount, bandRange;
    void bandSeg(const float* a, const float* b, bool flagged, float start, float end, float fillOnly) {
        bandSegs.insert(bandSegs.end(), {a[0], a[1], b[0], b[1]});
        if (flagged) bandFlags.insert(bandFlags.end(), {start, end, fillOnly, 0.f});
    }
    // One subpath of `k` points (x y pairs) into bandSegs.
    void bandSubpath(const float* p, int k, bool closed, bool flagged, bool fill) {
        if (k < 2) return;
        if (!flagged || closed) {
            for (int j = 0; j < k; ++j) bandSeg(p + 2 * j, p + 2 * (j + 1 < k ? j + 1 : 0), flagged, 0, 0, 0);
            return;
        }
        for (int j = 0; j + 1 < k; ++j) bandSeg(p + 2 * j, p + 2 * j + 2, true, j == 0 ? 1.f : 0.f, j + 2 == k ? 1.f : 0.f, 0);
        if (fill) bandSeg(p + 2 * (k - 1), p, true, 0, 0, 1);
    }
    // Every subpath of a path in slots (GpuTexPath's layout) into bandSegs.
    void bandSlots(const float* buf, std::size_t floats, bool flagged, bool fill) {
        bandSegs.clear(); bandFlags.clear();
        for (std::size_t i = 0; i < floats;) {
            const int k = static_cast<int>(buf[i]);
            bandSubpath(buf + i + 2, k, buf[i + 1] > 0.5f, flagged, fill);
            i += 2 * (static_cast<std::size_t>(k) + 1);
        }
    }
    static constexpr std::size_t kBandTexels = 1u << 20;  // the most one path's bands take
    bool bandPath(bool evenOdd, bool fill, Color fc, bool doStroke, Color sc, float hw, int cap, bool flagged, const IBox& q, float reach, const GpuInst& probe, const GpuStyle& s) {
        const std::size_t nseg = bandSegs.size() / 4;
        const std::size_t per = flagged ? 2 : 1;  // texels a segment takes
        // Each segment counted into the bands it reaches, then written there:
        // a header texel per band, then its segments, a texel each. Bands
        // are rows, or columns for a path wider than tall (a line of text).
        const bool columns = q.x1 - q.x0 > q.y1 - q.y0;
        const int a0 = columns ? q.x0 : q.y0, a1 = columns ? q.x1 : q.y1;
        const int c0 = columns ? 1 : 0;  // the coordinate bands cut along: x (0) of a point or y (1)
        const int bands = (a1 - a0 + kBandRows - 1) / kBandRows;
        // Each segment's bands, once: (b0, b1), b0 > b1 when it reaches none.
        const float top = static_cast<float>(a0), bottom = static_cast<float>(a1), inv = 1.f / kBandRows;
        const float last = static_cast<float>(bands - 1);
        bandRange.resize(2 * nseg);
        bandCount.assign(static_cast<std::size_t>(bands) + 1, 0);
        const float* g = bandSegs.data();
        int* r = bandRange.data();
        for (std::size_t k = 0; k < nseg; ++k, g += 4, r += 2) {
            const float u0 = g[1 - c0], u1 = g[3 - c0];
            const float lo = std::min(u0, u1) - reach, hi = std::max(u0, u1) + reach;
            if (hi < top || lo > bottom) { r[0] = 1; r[1] = 0; continue; }
            r[0] = static_cast<int>(std::min(std::max(lo - top, 0.f) * inv, last));
            r[1] = static_cast<int>(std::min(std::max(hi - top, 0.f) * inv, last));
            for (int b = r[0]; b <= r[1]; ++b) ++bandCount[static_cast<std::size_t>(b)];
        }
        // Where each band's texels start (headers only for bands with segments).
        const std::size_t base = edges.size() / 4;
        std::size_t total = 0;
        for (int b = 0; b < bands; ++b) {
            const int c = bandCount[static_cast<std::size_t>(b)];
            bandCount[static_cast<std::size_t>(b)] = c ? static_cast<int>(total) : -1;
            if (c) total += 1 + per * static_cast<std::size_t>(c);
        }
        if (total > kBandTexels) return false;
        edges.resize(edges.size() + total * 4);
        float* E = edges.data() + base * 4;
        static thread_local std::vector<int> fillAt;
        fillAt.assign(static_cast<std::size_t>(bands), 0);
        g = bandSegs.data();
        r = bandRange.data();
        for (std::size_t k = 0; k < nseg; ++k, g += 4, r += 2) {
            for (int b = r[0]; b <= r[1]; ++b) {
                const std::size_t at = static_cast<std::size_t>(bandCount[static_cast<std::size_t>(b)]) + 1 + per * static_cast<std::size_t>(fillAt[static_cast<std::size_t>(b)]++);
                std::memcpy(E + at * 4, g, 4 * sizeof(float));
                if (flagged) std::memcpy(E + (at + 1) * 4, bandFlags.data() + 4 * k, 4 * sizeof(float));
            }
        }
        for (int b = 0; b < bands; ++b) {
            const int start = bandCount[static_cast<std::size_t>(b)];
            if (start < 0) continue;
            const int n = fillAt[static_cast<std::size_t>(b)];
            float* h = E + static_cast<std::size_t>(start) * 4;
            // 2 rows, 3 rows with flags, 4 columns, 5 columns with flags.
            h[0] = static_cast<float>(n); h[1] = (flagged ? 3.f : 2.f) + (columns ? 2.f : 0.f); h[2] = 0; h[3] = 0;
            const int at = static_cast<int>(base) + start;
            const int b0 = a0 + b * kBandRows, b1 = std::min(a1, b0 + kBandRows);
            const int y0 = columns ? q.y0 : b0, y1 = columns ? q.y1 : b1, x0 = columns ? b0 : q.x0, x1 = columns ? b1 : q.x1;
            GpuInst& i = push();
            i = probe; stamp(i); identity(i);
            i.kind = kGpuPath;
            i.flags = static_cast<float>((fill ? kGpuPathFill : 0) | (doStroke ? kGpuPathStroke : 0));
            i.e = hw;
            i.quad[0] = static_cast<float>(x0); i.quad[1] = static_cast<float>(y0); i.quad[2] = static_cast<float>(x1); i.quad[3] = static_cast<float>(y1);
            i.p[0] = static_cast<float>(2 * at); i.p[1] = static_cast<float>(2 * static_cast<int>(per) * n + 2); i.p[2] = evenOdd ? 1.f : 0.f; i.p[3] = static_cast<float>(cap);
            i.img[0] = sc.r; i.img[1] = sc.g; i.img[2] = sc.b; i.img[3] = sc.a;
            i.alpha = s.alpha; i.colour = pack(fc);
        }
        lastPath = 0;
        return true;
    }
    // ----- a path kept on the GPU in its own space -----
    // Its segments (closed: a fill closes every subpath) in bands of rows
    // of its own space, each with the segments within `reach` of it, kept
    // across frames: a draw of it is one instance with its transform, and
    // the shader takes each pixel back into the path's space.
    // At `at`: a header texel (top, 1 / band height, bands, 0), a texel per
    // band (its first segment texel, count), then the segments (a.xy, b.xy).
    struct KeptPath {
        std::uint32_t gen = 0;
        int at = 0;
        float reach = 0, scale = 0;  // local reach the bands allow, and the scale they were cut for
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // local bounds
    };
    // Whether `k` holds bands of `pl` good for a draw at `scale` needing
    // `reach` device pixels round each pixel; if not, cuts them. False when
    // they would not fit.
    bool keepPath(KeptPath& k, const Poly& pl, float scale, float reach) {
        // Bands cut for another scale still work (only less well): they are
        // cut again only when they do not reach far enough. The reach only
        // grows, so a path drawn at several sizes settles on one cut.
        float need = reach / scale;
        if (k.gen == keptGen) {
            if (k.reach >= need) return true;
            need = std::max(need, k.reach * 2.f);
        }
        // Its segments.
        bandSegs.clear();
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        int first = 0;
        for (std::size_t c = 0; c < pl.ends.size(); ++c) {
            const int last = pl.ends[c];
            if (last - first >= 2)
                for (int i = first; i < last; ++i) {
                    const int j = i + 1 < last ? i + 1 : first;
                    const float ax = pl.pts[2 * i], ay = pl.pts[2 * i + 1];
                    bandSegs.insert(bandSegs.end(), {ax, ay, pl.pts[2 * j], pl.pts[2 * j + 1]});
                    bx0 = std::min(bx0, ax); bx1 = std::max(bx1, ax); by0 = std::min(by0, ay); by1 = std::max(by1, ay);
                }
            first = last;
        }
        const std::size_t nseg = bandSegs.size() / 4;
        if (!nseg || !(bx0 <= bx1) || !std::isfinite(bx0 + bx1 + by0 + by1)) return false;
        // Bands about 16 device rows tall at this scale; one for a small path.
        const float slack = need * 1.25f;
        const float height = by1 - by0;
        int bands = 1;
        if (nseg > 20 && height * scale > 2 * kBandRows) bands = std::min(512, static_cast<int>(std::ceil(height * scale / kBandRows)));
        const float bandH = bands > 1 ? height / bands : 1.f, inv = bands > 1 ? 1.f / bandH : 0.f;
        const float lastBand = static_cast<float>(bands - 1);
        bandRange.resize(2 * nseg);
        bandCount.assign(static_cast<std::size_t>(bands), 0);
        std::size_t total = 0;
        for (std::size_t i = 0; i < nseg; ++i) {
            const float* g = bandSegs.data() + 4 * i;
            const float lo = std::min(g[1], g[3]) - slack, hi = std::max(g[1], g[3]) + slack;
            int* r = bandRange.data() + 2 * i;
            r[0] = static_cast<int>(std::clamp((lo - by0) * inv, 0.f, lastBand));
            r[1] = static_cast<int>(std::clamp((hi - by0) * inv, 0.f, lastBand));
            for (int b = r[0]; b <= r[1]; ++b) ++bandCount[static_cast<std::size_t>(b)];
            total += static_cast<std::size_t>(r[1] - r[0] + 1);
        }
        const std::size_t texels = 1 + static_cast<std::size_t>(bands) + total;
        if (texels > kKeptTexels / 4) return false;
        if (kept.size() / 4 + texels > kKeptTexels) {
            // Full: everything kept goes (what is recorded is drawn first).
            draw();
            kept.clear(); keptSent = 0; ++keptGen;
        }
        const std::size_t at = kept.size() / 4;
        kept.resize(kept.size() + texels * 4);
        float* K = kept.data() + at * 4;
        K[0] = by0; K[1] = inv; K[2] = static_cast<float>(bands); K[3] = 0;
        std::size_t next = at + 1 + static_cast<std::size_t>(bands);
        for (int b = 0; b < bands; ++b) {
            float* h = K + 4 * (1 + b);
            const int c = bandCount[static_cast<std::size_t>(b)];
            h[0] = static_cast<float>(next); h[1] = static_cast<float>(c); h[2] = 0; h[3] = 0;
            bandCount[static_cast<std::size_t>(b)] = static_cast<int>(next);  // now: where its next segment goes
            next += static_cast<std::size_t>(c);
        }
        for (std::size_t i = 0; i < nseg; ++i) {
            const int* r = bandRange.data() + 2 * i;
            for (int b = r[0]; b <= r[1]; ++b)
                std::memcpy(kept.data() + 4 * static_cast<std::size_t>(bandCount[static_cast<std::size_t>(b)]++), bandSegs.data() + 4 * i, 4 * sizeof(float));
        }
        k.gen = keptGen; k.at = static_cast<int>(at); k.reach = slack; k.scale = scale;
        k.x0 = bx0; k.y0 = by0; k.x1 = bx1; k.y1 = by1;
        return true;
    }
    // A kept path under `M` (a similarity: no skew, the same scale both
    // ways), filled and/or stroked: one instance. False if it cannot be
    // drawn this way (the caller draws it another).
    bool pathKept(KeptPath& k, const Poly& pl, const Matrix& M, bool evenOdd, bool fill, Color fc, bool doStroke, Color sc, float hw, const GpuStyle& s) {
        const float scale = std::sqrt(std::abs(M[0] * M[3] - M[1] * M[2]));
        if (!(scale > 0) || !std::isfinite(scale)) return false;
        const float reach = (doStroke ? hw : 0.f) + 1.f;
        if (!keepPath(k, pl, scale, reach)) return false;
        GpuInst probe;  // (filled in only for a mask clip)
        IBox limit;
        if (s.clip) { probe = GpuInst{}; if (!setClip(probe, s, limit)) return false; }
        else limit = limitOf(s);
        float dx0 = 1e30f, dy0 = 1e30f, dx1 = -1e30f, dy1 = -1e30f;
        for (int c = 0; c < 4; ++c) {
            const float x = c & 1 ? k.x1 : k.x0, y = c & 2 ? k.y1 : k.y0;
            const float X = M[0] * x + M[2] * y + M[4], Y = M[1] * x + M[3] * y + M[5];
            dx0 = std::min(dx0, X); dx1 = std::max(dx1, X); dy0 = std::min(dy0, Y); dy1 = std::max(dy1, Y);
        }
        if (!std::isfinite(dx0 + dx1 + dy0 + dy1)) return true;
        const auto clampI = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
        const IBox q = IBox{clampI(std::floor(dx0 - reach)), clampI(std::floor(dy0 - reach)), clampI(std::ceil(dx1 + reach)), clampI(std::ceil(dy1 + reach))}.intersect(limit);
        if (q.empty()) return true;
        GpuInst& i = push();
        if (s.clip) i = probe;
        else { IBox l; setClip(i, s, l); }
        stamp(i);
        i.m[0] = M[0]; i.m[1] = M[1]; i.m[2] = M[2]; i.m[3] = M[3]; i.e = M[4]; i.f = M[5];
        i.kind = kGpuPath;
        i.flags = static_cast<float>(kGpuPathKept | (fill ? kGpuPathFill : 0) | (doStroke ? kGpuPathStroke : 0));
        i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
        i.p[0] = static_cast<float>(k.at); i.p[1] = 0; i.p[2] = evenOdd ? 1.f : 0.f; i.p[3] = hw;
        i.img[0] = sc.r; i.img[1] = sc.g; i.img[2] = sc.b; i.img[3] = sc.a;
        i.alpha = s.alpha; i.colour = pack(fc);
        lastPath = fill && !doStroke ? inst.size() : 0;
        lastKept = &k;
        return true;
    }
    const KeptPath* lastKept = nullptr;
    // Whether the instance last recorded is a fill of a kept path (a stroke
    // of an open path cannot join it: its bands close every subpath).
    bool lastIsKept() const { return lastPath && lastPath == inst.size() && (static_cast<int>(inst.back().flags) & kGpuPathKept); }
    bool strokeLastPath(Color sc, float hw, int cap, const GpuStyle& s) {
        if (!lastPath || lastPath != inst.size()) return false;
        GpuInst& i = inst.back();
        if (i.kind != kGpuPath || i.alpha != s.alpha || i.hasMask > 1.5f || s.clipPaths || s.clip) return false;
        const bool keptPath = (static_cast<int>(i.flags) & kGpuPathKept) != 0;
        if (keptPath) {
            // Its bands must reach as far as the stroke does (else it is drawn on its own,
            // and that cuts bands that do for next time).
            const float scale = std::sqrt(std::abs(i.m[0] * i.m[3] - i.m[1] * i.m[2]));
            if (!lastKept || lastKept->gen != keptGen || lastKept->at != static_cast<int>(i.p[0]) || lastKept->reach * scale < hw + 1.f) return false;
        }
        const float grow = hw * (cap == 2 ? 1.5f : 1.f) + 1.f;
        const IBox q = IBox{static_cast<int>(std::floor(i.quad[0] - grow)), static_cast<int>(std::floor(i.quad[1] - grow)),
                            static_cast<int>(std::ceil(i.quad[2] + grow)), static_cast<int>(std::ceil(i.quad[3] + grow))}.intersect(IBox{0, 0, w, h});
        i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
        i.flags = static_cast<float>(static_cast<int>(i.flags) | kGpuPathStroke);
        if (keptPath) i.p[3] = hw;
        else { i.e = hw; i.p[3] = static_cast<float>(cap); }
        i.img[0] = sc.r; i.img[1] = sc.g; i.img[2] = sc.b; i.img[3] = sc.a;
        lastPath = 0;
        return true;
    }
    static std::uint32_t pack(Color c) {
        return static_cast<std::uint32_t>(c.r) | static_cast<std::uint32_t>(c.g) << 8 | static_cast<std::uint32_t>(c.b) << 16 | static_cast<std::uint32_t>(c.a) << 24;
    }
    static void identity(GpuInst& i) { i.m[0] = 1; i.m[1] = 0; i.m[2] = 0; i.m[3] = 1; i.e = 0; i.f = 0; }
    // Paints what follows with gradient `gr` (user space under `m`); false
    // if the batch has no room for another.
    bool useGradient(const std::shared_ptr<const GradientImpl>& gr, const Matrix& m) {
        auto it = rampRow.find(gr.get());
        if (it == rampRow.end()) {
            if (static_cast<int>(rampKeep.size()) >= kMaxGradients) draw();
            const int row = static_cast<int>(rampKeep.size());
            rampKeep.push_back(gr);
            rampPixels.resize(static_cast<std::size_t>(row + 1) * kGradientLut * 4);
            std::memcpy(rampPixels.data() + static_cast<std::size_t>(row) * kGradientLut * 4, gr->lut.data(), kGradientLut * 4);
            rampGeometry.insert(rampGeometry.end(), {static_cast<float>(gr->kind), gr->g[0], gr->g[1], gr->g[2], gr->g[3], gr->g[4], gr->g[5], 0.f});
            it = rampRow.emplace(gr.get(), row).first;
        }
        paintRow = it->second;
        paintM = m;
        return true;
    }
    void useSolid() { paintRow = -1; }
    // The current paint on instance `i` (after its colour is set).
    void applyPaint(GpuInst& i) const {
        if (paintRow < 0) return;
        i.flags = static_cast<float>(static_cast<int>(i.flags) | kGpuGradient);
        i.m[0] = paintM[0]; i.m[1] = paintM[1]; i.m[2] = paintM[2]; i.m[3] = paintM[3]; i.e = paintM[4]; i.f = paintM[5];
        i.colour = static_cast<std::uint32_t>(paintRow & 255) | static_cast<std::uint32_t>(paintRow >> 8) << 8 | 0xFF000000u;
    }

    // A device-space rectangle, covered exactly (by area). An opaque one
    // is drawn as its whole-pixel interior, opaque, and the strips of
    // partly covered pixels round it, blended.
    bool rect(float x0, float y0, float x1, float y1, const GpuStyle& s, Color c, bool clear = false) {
        GpuInst probe;  // (filled in only for a mask clip)
        IBox limit;
        if (s.clip) { probe = GpuInst{}; if (!setClip(probe, s, limit)) return false; }
        else limit = limitOf(s);
        IBox q = IBox{static_cast<int>(std::floor(x0)), static_cast<int>(std::floor(y0)), static_cast<int>(std::ceil(x1)), static_cast<int>(std::ceil(y1))}.intersect(limit);
        if (q.empty()) return true;
        const auto emit = [&](IBox part, bool solid) {
            if (part.empty()) return;
            GpuInst& i = solid ? pushOpaque() : push(0, clear ? kBlendClear : kBlendOver);
            if (s.clip) i = probe;
            else { IBox l; setClip(i, s, l); }
            stamp(i);
            identity(i);
            i.kind = kGpuDevRect; i.flags = 0;
            i.quad[0] = static_cast<float>(part.x0); i.quad[1] = static_cast<float>(part.y0); i.quad[2] = static_cast<float>(part.x1); i.quad[3] = static_cast<float>(part.y1);
            i.p[0] = x0; i.p[1] = y0; i.p[2] = x1; i.p[3] = y1;
            i.alpha = clear ? 1.f : s.alpha;
            i.colour = pack(c);
            if (!clear) applyPaint(i);
        };
        // (No mask: probe.hasMask is 0 when there is no mask clip.)
        if (clear || paintRow >= 0 || c.a != 255 || s.alpha < 1.f || (s.clip && probe.hasMask >= 0.5f) || (s.clipPaths && s.clipPaths->count())) { emit(q, false); return true; }
        const IBox inner = IBox{static_cast<int>(std::ceil(x0)), static_cast<int>(std::ceil(y0)), static_cast<int>(std::floor(x1)), static_cast<int>(std::floor(y1))}.intersect(q);
        if (inner.empty()) { emit(q, false); return true; }
        emit(inner, true);
        if (inner.x0 == q.x0 && inner.y0 == q.y0 && inner.x1 == q.x1 && inner.y1 == q.y1) return true;  // whole pixels
        emit(IBox{q.x0, q.y0, q.x1, inner.y0}, false);
        emit(IBox{q.x0, inner.y1, q.x1, q.y1}, false);
        emit(IBox{q.x0, inner.y0, inner.x0, inner.y1}, false);
        emit(IBox{inner.x1, inner.y0, q.x1, inner.y1}, false);
        return true;
    }
    // The ring between device rects `o` and `i` (inside it), covered exactly
    // (by area): an axis-aligned stroke of a rect with miter joins.
    bool rectRing(const float* o, const float* in, const GpuStyle& s, Color c) {
        if (paintRow >= 0) return false;  // (its inner rect takes the gradient's place)
        GpuInst probe;
        IBox limit;
        if (s.clip) { probe = GpuInst{}; if (!setClip(probe, s, limit)) return false; }
        else limit = limitOf(s);
        const IBox q = IBox{static_cast<int>(std::floor(o[0])), static_cast<int>(std::floor(o[1])), static_cast<int>(std::ceil(o[2])), static_cast<int>(std::ceil(o[3]))}.intersect(limit);
        if (q.empty()) return true;
        GpuInst& i = push();
        if (s.clip) i = probe;
        else { IBox l; setClip(i, s, l); }
        stamp(i);
        i.m[0] = in[0]; i.m[1] = in[1]; i.m[2] = in[2]; i.m[3] = in[3]; i.e = 0; i.f = 0;
        i.kind = kGpuDevRect; i.flags = kGpuRing;
        i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
        i.p[0] = o[0]; i.p[1] = o[1]; i.p[2] = o[2]; i.p[3] = o[3];
        i.alpha = s.alpha; i.colour = pack(c);
        return true;
    }
    // True for one closed contour that is convex: its winding is 0 or 1
    // everywhere, so the summed edge areas are its coverage exactly.
    static bool convexContour(const Poly& pl) {
        if (pl.ends.size() != 1) return false;
        const int n = pl.ends[0];
        if (n < 3) return false;
        int sign = 0, turnsY = 0;
        float lastDy = 0;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n, k = (i + 2) % n;
            const float ax = pl.pts[2 * j] - pl.pts[2 * i], ay = pl.pts[2 * j + 1] - pl.pts[2 * i + 1];
            const float bx = pl.pts[2 * k] - pl.pts[2 * j], by = pl.pts[2 * k + 1] - pl.pts[2 * j + 1];
            const float cross = ax * by - ay * bx;
            // Relative to the edges: a circle flattened finely turns very
            // little at each point, and float noise must not read as a turn.
            const float tol = 1e-4f * std::sqrt((ax * ax + ay * ay) * (bx * bx + by * by));
            if (cross > tol) { if (sign < 0) return false; sign = 1; }
            else if (cross < -tol) { if (sign > 0) return false; sign = -1; }
            if (ay != 0) { if (lastDy != 0 && (ay > 0) != (lastDy > 0)) ++turnsY; lastDy = ay; }
        }
        // Going round once, y turns back exactly twice; a star turns more.
        return turnsY <= 2;
    }
    // True for one contour that goes round exactly once without crossing
    // itself, as far as its total turning says (a star turns twice, a figure
    // eight not at all): every point is inside it at most once.
    static bool simpleContour(const Poly& pl) {
        if (pl.ends.size() != 1) return disjointSimple(pl);
        // The points without repeats (a closed contour often ends on its
        // first point): a repeat would hide the turn it sits on.
        static thread_local std::vector<float> q;
        q.clear();
        const int n0 = pl.ends[0];
        for (int i = 0; i < n0; ++i) {
            const float x = pl.pts[2 * i], y = pl.pts[2 * i + 1];
            if (!q.empty() && std::abs(q[q.size() - 2] - x) < 1e-4f && std::abs(q.back() - y) < 1e-4f) continue;
            q.push_back(x); q.push_back(y);
        }
        while (q.size() >= 4 && std::abs(q[0] - q[q.size() - 2]) < 1e-4f && std::abs(q[1] - q.back()) < 1e-4f) q.resize(q.size() - 2);
        const int n = static_cast<int>(q.size() / 2);
        if (n < 3) return false;
        float turning = 0;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n, k = (i + 2) % n;
            const float ax = q[2 * j] - q[2 * i], ay = q[2 * j + 1] - q[2 * i + 1];
            const float bx = q[2 * k] - q[2 * j], by = q[2 * k + 1] - q[2 * j + 1];
            turning += std::atan2(ax * by - ay * bx, ax * bx + ay * by);
        }
        return std::abs(std::abs(turning) - 6.2831853f) < 0.5f;
    }
    // Several contours, each simple, none reaching into another's box: no
    // point is inside more than one.
    static bool disjointSimple(const Poly& pl) {
        const std::size_t n = pl.ends.size();
        if (n < 2 || n > 16) return false;
        float box[16][4];
        static thread_local Poly one;
        int first = 0;
        for (std::size_t c = 0; c < n; ++c) {
            const int last = pl.ends[c];
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (int k = first; k < last; ++k) {
                x0 = std::min(x0, pl.pts[2 * k]); x1 = std::max(x1, pl.pts[2 * k]);
                y0 = std::min(y0, pl.pts[2 * k + 1]); y1 = std::max(y1, pl.pts[2 * k + 1]);
            }
            box[c][0] = x0; box[c][1] = y0; box[c][2] = x1; box[c][3] = y1;
            for (std::size_t d = 0; d < c; ++d)
                if (!(x1 < box[d][0] || box[d][2] < x0 || y1 < box[d][1] || box[d][3] < y0)) return false;
            one.reset();
            one.pts.assign(pl.pts.begin() + 2 * first, pl.pts.begin() + 2 * last);
            one.ends.push_back(last - first);
            one.closed.push_back(1);
            if (!simpleContour(one)) return false;
            first = last;
        }
        return true;
    }
    // A device-space polygon. Cut into tiles of kTile x kTile pixels, each
    // listing only the edges that cross it: the edges wholly to its left
    // matter only as winding, which becomes a few vertical edges along its
    // left side (none at all where they cancel, as across a ring's hole). A
    // tile with no edges of its own is skipped, or drawn as a solid rect.
    static constexpr int kTile = 16;
    // Polygons and strokes up to this many edges over up to this many pixels
    // are drawn whole; bigger ones are cut into tiles.
    static constexpr int kWholeEdges = 96;
    static constexpr long long kWholeArea = 160 * 160;
    // Whether two edges of the polygon may bound an overlap: they cross, or
    // touch without being neighbours along one contour.
    template <class E> bool overlapRisk(const E& a, const E& b) const {
        const int* wa = &polyWhere()[static_cast<std::size_t>(a.src) * 3];
        const int* wb = &polyWhere()[static_cast<std::size_t>(b.src) * 3];
        const bool neighbours = wa[0] == wb[0] && (std::abs(wa[1] - wb[1]) == 1 || std::abs(wa[1] - wb[1]) == wa[2] - 1);
        if (a.src == b.src) return false;
        if (neighbours) return crosses(a, b);
        return touches(a, b);
    }
    static std::vector<int>& polyWhere() { static thread_local std::vector<int> w; return w; }
    template <class E> static bool touches(const E& a, const E& b) {
        const float e = 1e-3f;
        if (std::max(a.x0, a.x1) + e < std::min(b.x0, b.x1) || std::max(b.x0, b.x1) + e < std::min(a.x0, a.x1) ||
            std::max(a.y0, a.y1) + e < std::min(b.y0, b.y1) || std::max(b.y0, b.y1) + e < std::min(a.y0, a.y1)) return false;
        const auto orient = [](float ax, float ay, float bx, float by, float cx, float cy) {
            const float v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            const float scale = std::max(1.f, std::abs(bx - ax) + std::abs(by - ay));
            return v > 1e-4f * scale ? 1 : v < -1e-4f * scale ? -1 : 0;
        };
        const int o1 = orient(a.x0, a.y0, a.x1, a.y1, b.x0, b.y0), o2 = orient(a.x0, a.y0, a.x1, a.y1, b.x1, b.y1);
        const int o3 = orient(b.x0, b.y0, b.x1, b.y1, a.x0, a.y0), o4 = orient(b.x0, b.y0, b.x1, b.y1, a.x1, a.y1);
        return o1 * o2 <= 0 && o3 * o4 <= 0;
    }
    // Whether two edges cross each other properly (not just touch).
    template <class E> static bool crosses(const E& a, const E& b) {
        if (std::max(a.x0, a.x1) < std::min(b.x0, b.x1) || std::max(b.x0, b.x1) < std::min(a.x0, a.x1) ||
            std::max(a.y0, a.y1) < std::min(b.y0, b.y1) || std::max(b.y0, b.y1) < std::min(a.y0, a.y1)) return false;
        const auto orient = [](float ax, float ay, float bx, float by, float cx, float cy) {
            const float v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            return v > 1e-6f ? 1 : v < -1e-6f ? -1 : 0;
        };
        const int o1 = orient(a.x0, a.y0, a.x1, a.y1, b.x0, b.y0), o2 = orient(a.x0, a.y0, a.x1, a.y1, b.x1, b.y1);
        const int o3 = orient(b.x0, b.y0, b.x1, b.y1, a.x0, a.y0), o4 = orient(b.x0, b.y0, b.x1, b.y1, a.x1, a.y1);
        return o1 * o2 < 0 && o3 * o4 < 0;
    }
    // `mayOverlap`: pieces of it overlap (a stroke's outline), so only exact
    // coverage reads it right.
    // `shape`, when the caller knows it: 2 convex, 1 simple, 0 neither.
    enum { kShapeUnknown = -1, kShapeGeneral = 0, kShapeSimple = 1, kShapeConvex = 2 };
    bool poly(const Poly& pl, bool evenOdd, const GpuStyle& s, Color c, bool mayOverlap = false, int shape = kShapeUnknown) {
        GpuInst probe{};
        IBox limit;
        if (!setClip(probe, s, limit)) return false;
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (std::size_t k = 0; k < pl.pts.size(); k += 2) {
            const float x = pl.pts[k], y = pl.pts[k + 1];
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y);
        }
        if (!(bx0 <= bx1)) return true;
        const auto clampI = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
        const IBox box = IBox{clampI(std::floor(bx0)), clampI(std::floor(by0)), clampI(std::ceil(bx1)), clampI(std::ceil(by1))}.intersect(limit);
        if (box.empty()) return true;
        // Edges, closing every contour as the rasterizer does.
        static thread_local std::vector<float> all;
        // Per edge: its contour, its index in it, and the contour's length,
        // so neighbours (which meet at a shared point) are told apart from
        // edges that touch where pieces overlap.
        std::vector<int>& where = polyWhere();
        all.clear(); where.clear();
        int first = 0;
        for (std::size_t ci = 0; ci < pl.ends.size(); ++ci) {
            const int last = pl.ends[ci];
            for (int k = first; k < last; ++k) {
                const int j = (k + 1 == last) ? first : k + 1;
                const float ax = pl.pts[2 * k], ay = pl.pts[2 * k + 1], bx = pl.pts[2 * j], by = pl.pts[2 * j + 1];
                if (ay == by || !(std::isfinite(ax) && std::isfinite(ay) && std::isfinite(bx) && std::isfinite(by))) continue;
                all.insert(all.end(), {ax, ay, bx, by});
                where.insert(where.end(), {static_cast<int>(ci), k - first, last - first});
            }
            first = last;
        }
        if (all.empty()) return true;
        const float exact = shape >= 0 ? (shape == kShapeConvex ? 1.f : 0.f) : (convexContour(pl) ? 1.f : 0.f);
        const auto isSimple = [&] { return shape >= 0 ? shape >= kShapeSimple : simpleContour(pl); };
        // A small polygon is one quad with all its edges: the GPU walks a few
        // more edges per pixel, and the CPU sorts nothing into tiles.
        const bool small = static_cast<int>(all.size() / 4) <= kWholeEdges && box.area() <= kWholeArea;
        if (small && (exact > 0.5f || (!mayOverlap && isSimple()))) {
            const std::size_t start = edges.size() / 4;
            float mode = exact;
            if (exact > 0.5f) {
                // Convex: each edge as the line it lies on, outward, and a
                // pixel's coverage from the largest signed distance (as cp6).
                float cx = 0, cy = 0;
                const int pts = pl.points();
                for (int k = 0; k < pts; ++k) { cx += pl.pts[2 * k]; cy += pl.pts[2 * k + 1]; }
                cx /= pts; cy /= pts;
                for (std::size_t k = 0; k < all.size(); k += 4) {
                    float nx = all[k + 3] - all[k + 1], ny = -(all[k + 2] - all[k]);
                    const float len = gpuLength(nx, ny);
                    if (!(len > 0)) continue;
                    nx /= len; ny /= len;
                    if (nx * (cx - all[k]) + ny * (cy - all[k + 1]) > 0) { nx = -nx; ny = -ny; }
                    edges.insert(edges.end(), {nx, ny, -(nx * all[k] + ny * all[k + 1]), 0.f});
                }
                mode = 2.f;
            } else {
                edges.insert(edges.end(), all.begin(), all.end());
                // Simple: inside by the winding at the pixel's centre, the
                // edge by its distance (as cp6).
                mode = 3.f;
            }
            const std::size_t count = edges.size() / 4 - start;
            GpuInst& i = push();
            i = probe; stamp(i); identity(i);
            i.kind = kGpuPoly; i.flags = 0;
            i.quad[0] = static_cast<float>(box.x0); i.quad[1] = static_cast<float>(box.y0); i.quad[2] = static_cast<float>(box.x1); i.quad[3] = static_cast<float>(box.y1);
            i.p[0] = static_cast<float>(start); i.p[1] = static_cast<float>(count); i.p[2] = evenOdd ? 1.f : 0.f; i.p[3] = mode;
            i.alpha = s.alpha; i.colour = pack(c);
            applyPaint(i);
            return true;
        }
        const auto inside = [evenOdd](int w) { return evenOdd ? (w & 1) != 0 : w != 0; };
        struct BandEdge { float x0, y0, x1, y1; int t0, t1, src; };
        static thread_local std::vector<BandEdge> band;
        static thread_local std::vector<std::vector<int>> tileEdges;
        static thread_local std::vector<std::vector<int>> leftFrom;  // edges wholly left of tile k, by first such k
        struct Piece { float y; int d; };
        static thread_local std::vector<Piece> backdrop;
        struct Run { float y0, y1; int w; };
        static thread_local std::vector<Run> net;
        const int tiles = (box.x1 - box.x0 + kTile - 1) / kTile;
        if (static_cast<int>(tileEdges.size()) < tiles + 1) { tileEdges.resize(static_cast<std::size_t>(tiles) + 1); leftFrom.resize(static_cast<std::size_t>(tiles) + 1); }
        for (int b0 = box.y0; b0 < box.y1; b0 += kTile) {
            const int b1 = std::min(box.y1, b0 + kTile);
            const float fy0 = static_cast<float>(b0), fy1 = static_cast<float>(b1);
            band.clear();
            for (int k = 0; k <= tiles; ++k) { tileEdges[static_cast<std::size_t>(k)].clear(); leftFrom[static_cast<std::size_t>(k)].clear(); }
            for (std::size_t k = 0; k < all.size(); k += 4) {
                float ax = all[k], ay = all[k + 1], bx = all[k + 2], by = all[k + 3];
                if (std::max(ay, by) <= fy0 || std::min(ay, by) >= fy1) continue;
                // Clipped to the band; the direction is kept.
                const float dxdy = (bx - ax) / (by - ay);
                if (ay < fy0 || ay > fy1) { const float yy = ay < fy0 ? fy0 : fy1; ax += (yy - ay) * dxdy; ay = yy; }
                if (by < fy0 || by > fy1) { const float yy = by < fy0 ? fy0 : fy1; bx = ax + (yy - ay) * dxdy; by = yy; }
                if (ay == by) continue;
                const float lo = std::min(ax, bx), hi = std::max(ax, bx);
                // Tiles the edge reaches; it is wholly left of every tile after t1.
                const int t0 = std::clamp(static_cast<int>(std::floor((lo - box.x0) / kTile)), 0, tiles);
                const int t1 = std::clamp(static_cast<int>(std::floor((hi - box.x0) / kTile)), -1, tiles - 1);
                const int idx = static_cast<int>(band.size());
                band.push_back({ax, ay, bx, by, t0, t1, static_cast<int>(k / 4)});
                for (int t = t0; t <= t1; ++t) tileEdges[static_cast<std::size_t>(t)].push_back(idx);
                if (t1 + 1 < tiles) leftFrom[static_cast<std::size_t>(std::max(0, t1 + 1))].push_back(idx);
            }
            if (band.empty()) continue;
            backdrop.clear();
            net.clear();
            int solidFrom = -1;  // a run of solid tiles not yet emitted
            const auto endSolid = [&](int upTo) {
                if (solidFrom < 0) return;
                const float x0 = static_cast<float>(std::max(box.x0 + solidFrom * kTile, limit.x0));
                const float x1 = static_cast<float>(std::min(box.x0 + upTo * kTile, box.x1));
                GpuInst& i = opaqueStyle(probe, s, c) ? pushOpaque() : push();
                i = probe; stamp(i); identity(i);
                i.kind = kGpuDevRect; i.flags = 0;
                i.quad[0] = x0; i.quad[1] = fy0; i.quad[2] = x1; i.quad[3] = fy1;
                i.p[0] = x0; i.p[1] = fy0; i.p[2] = x1; i.p[3] = fy1;
                i.alpha = s.alpha; i.colour = pack(c);
                applyPaint(i);
                solidFrom = -1;
            };
            for (int t = 0; t < tiles; ++t) {
                // Edges now wholly to the left join the backdrop.
                if (!leftFrom[static_cast<std::size_t>(t)].empty()) {
                    for (int idx : leftFrom[static_cast<std::size_t>(t)]) {
                        const BandEdge& e = band[static_cast<std::size_t>(idx)];
                        const int d = e.y0 < e.y1 ? 1 : -1;
                        backdrop.push_back({std::min(e.y0, e.y1), d});
                        backdrop.push_back({std::max(e.y0, e.y1), -d});
                    }
                    std::sort(backdrop.begin(), backdrop.end(), [](const Piece& a, const Piece& b) { return a.y < b.y; });
                    net.clear();
                    int w = 0;
                    float y = fy0;
                    for (const Piece& p : backdrop) {
                        if (p.y > y) { if (w) net.push_back({y, p.y, w}); y = p.y; }
                        w += p.d;
                    }
                }
                const std::vector<int>& own = tileEdges[static_cast<std::size_t>(t)];
                const int tx0 = box.x0 + t * kTile, tx1 = std::min(box.x1, tx0 + kTile);
                if (own.empty()) {
                    // Inside or outside the whole tile, unless the backdrop
                    // changes partway down it.
                    const bool whole = net.size() == 1 && net[0].y0 <= fy0 && net[0].y1 >= fy1;
                    if (net.empty() || (whole && !inside(net[0].w))) { endSolid(t); continue; }
                    if (whole) { if (solidFrom < 0) solidFrom = t; continue; }
                }
                endSolid(t);
                const IBox q = IBox{tx0, b0, tx1, b1}.intersect(limit);
                if (q.empty()) continue;
                // Summed areas are exact unless two of the tile's own edges
                // cross: only then can pieces overlap inside one pixel.
                float tileExact = exact;
                if (tileExact < 0.5f) {
                    tileExact = 1.f;
                    if (own.size() > 24) tileExact = 0.f;
                    for (std::size_t u = 0; u < own.size() && tileExact > 0.5f; ++u)
                        for (std::size_t v = u + 1; v < own.size(); ++v)
                            if (overlapRisk(band[static_cast<std::size_t>(own[u])], band[static_cast<std::size_t>(own[v])])) { tileExact = 0.f; break; }
                }
                const std::size_t start = edges.size() / 4;
                for (int idx : own) { const BandEdge& e = band[static_cast<std::size_t>(idx)]; edges.insert(edges.end(), {e.x0, e.y0, e.x1, e.y1}); }
                const float lx = static_cast<float>(tx0);
                for (const Run& r : net)
                    for (int k = 0; k < std::min(std::abs(r.w), 4); ++k)
                        r.w > 0 ? edges.insert(edges.end(), {lx, r.y0, lx, r.y1}) : edges.insert(edges.end(), {lx, r.y1, lx, r.y0});
                const std::size_t n = edges.size() / 4 - start;
                if (!n) continue;
                GpuInst& i = push();
                i = probe; stamp(i); identity(i);
                i.kind = kGpuPoly; i.flags = 0;
                i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
                i.p[0] = static_cast<float>(start); i.p[1] = static_cast<float>(n); i.p[2] = evenOdd ? 1.f : 0.f; i.p[3] = tileExact;
                i.alpha = s.alpha; i.colour = pack(c);
                applyPaint(i);
            }
            endSolid(tiles);
        }
        return true;
    }
    // A stroke drawn from its centre line: a pixel is covered by how far it
    // is from the nearest segment, which is the exact shape for round joins
    // (and for joins that turn too little for miter, bevel and round to
    // differ). `line` is device space; `hw` half the device width; open
    // subpaths get butt (0) or round (1) caps.
    // Joins: round ones are what distance gives; a miter is that plus a
    // convex wedge out to its tip, and a square cap the round one plus a
    // square. False, nothing drawn, for what this cannot do exactly (a bevel
    // or a miter past its limit on a corner sharp enough to show it).
    bool stroke(const Poly& line, float hw, int cap, int join, float miterLimit, const GpuStyle& s, Color c) {
        GpuInst probe{};
        IBox limit;
        if (!setClip(probe, s, limit)) return false;
        struct Seg { float ax, ay, bx, by; };
        // Each wedge: its box, then its four edges as lines (outward normal,
        // offset), so a pixel tests it with four dot products.
        static thread_local std::vector<float> wedges;
        wedges.clear();
        const auto addWedge = [](float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy) {
            const float q[8] = {ax, ay, bx, by, cx, cy, dx, dy};
            const float orient = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) > 0 ? 1.f : -1.f;
            wedges.insert(wedges.end(), {std::min({ax, bx, cx, dx}), std::min({ay, by, cy, dy}), std::max({ax, bx, cx, dx}), std::max({ay, by, cy, dy})});
            for (int j = 0; j < 4; ++j) {
                const float ex = q[(2 * j + 2) % 8] - q[2 * j], ey = q[(2 * j + 3) % 8] - q[2 * j + 1];
                const float l = gpuLength(ex, ey);
                float nx = l > 0 ? orient * ey / l : 0, ny = l > 0 ? -orient * ex / l : 0;
                wedges.insert(wedges.end(), {nx, ny, -(nx * q[2 * j] + ny * q[2 * j + 1])});
            }
            wedges.insert(wedges.end(), {0.f, 0.f, 0.f, 0.f});  // to five texels
        };
        // The butt-cap planes of the open subpaths' ends: point and inward
        // direction. A pixel near an end beyond its plane is cut.
        static thread_local std::vector<float> planes;
        planes.clear();
        static thread_local std::vector<Seg> segsOut;
        segsOut.clear();
        const float eps = 0.01f;
        int first = 0;
        static thread_local std::vector<float> pts;
        for (std::size_t ci = 0; ci < line.ends.size(); ++ci) {
            const int last = line.ends[ci];
            bool closed = line.closed[ci] != 0;
            pts.clear();
            for (int k = first; k < last; ++k) {
                const float x = line.pts[2 * k], y = line.pts[2 * k + 1];
                if (!(std::isfinite(x) && std::isfinite(y))) continue;
                if (pts.size() >= 2 && std::abs(pts[pts.size() - 2] - x) < eps && std::abs(pts.back() - y) < eps) continue;
                pts.push_back(x); pts.push_back(y);
            }
            first = last;
            if (!closed && pts.size() >= 8 && std::abs(pts[0] - pts[pts.size() - 2]) < eps && std::abs(pts[1] - pts.back()) < eps) {
                // Open, but ending where it starts and carrying straight on (a
                // whole ellipse without closePath): the two butt ends meet
                // flush, which is what a closed outline draws.
                // (It turns there no more than at its neighbouring corners.)
                const std::size_t m = pts.size();
                const auto turnCos = [&](std::size_t a, std::size_t b, std::size_t c) {
                    const float ax = pts[b] - pts[a], ay = pts[b + 1] - pts[a + 1], bx = pts[c] - pts[b], by = pts[c + 1] - pts[b + 1];
                    const float la = gpuLength(ax, ay), lb = gpuLength(bx, by);
                    return la > 0 && lb > 0 ? (ax * bx + ay * by) / (la * lb) : -1.f;
                };
                const float seam = turnCos(m - 4, m - 2, 2);
                const float near = std::min(turnCos(0, 2, 4), turnCos(m - 6, m - 4, m - 2));
                if (seam > 0.7f && seam >= near - 0.02f) closed = true;
            }
            if (closed && pts.size() >= 4 && std::abs(pts[0] - pts[pts.size() - 2]) < eps && std::abs(pts[1] - pts.back()) < eps) pts.resize(pts.size() - 2);
            const int n = static_cast<int>(pts.size() / 2);
            if (n == 0) continue;
            if (n == 1) {
                if (cap == 1) segsOut.push_back({pts[0], pts[1], pts[0], pts[1]});
                else if (cap == 2) return false;
                continue;
            }
            const int segments = closed ? n : n - 1;
            if (join != 1) {
                // Each corner that turns enough for a miter or bevel to differ
                // from a round join by a twentieth of a pixel.
                const int corners = closed ? n : n - 2;
                for (int v = 0; v < corners; ++v) {
                    const int i0 = closed ? (v + n - 1) % n : v, i1 = closed ? v : v + 1, i2 = closed ? (v + 1) % n : v + 2;
                    float d1x = pts[2 * i1] - pts[2 * i0], d1y = pts[2 * i1 + 1] - pts[2 * i0 + 1];
                    float d2x = pts[2 * i2] - pts[2 * i1], d2y = pts[2 * i2 + 1] - pts[2 * i1 + 1];
                    const float l1 = gpuLength(d1x, d1y), l2 = gpuLength(d2x, d2y);
                    if (!(l1 > 0) || !(l2 > 0)) continue;
                    d1x /= l1; d1y /= l1; d2x /= l2; d2y /= l2;
                    const float cross = d1x * d2y - d1y * d2x, dot = d1x * d2x + d1y * d2y;
                    // The outer side's normals.
                    const float side = cross > 0 ? -1.f : 1.f;
                    const float n1x = -d1y * side, n1y = d1x * side, n2x = -d2y * side, n2y = d2x * side;
                    const float cosHalf = std::sqrt(std::max(0.f, (1 + (n1x * n2x + n1y * n2y)) * 0.5f));
                    if (dot > 0.999999f || !(cosHalf > 1e-4f)) continue;
                    const float miterLen = hw / cosHalf;
                    if (miterLen - hw < 0.05f) continue;
                    if (join == 2 || miterLen > miterLimit * hw) return false;
                    float bx = n1x + n2x, by = n1y + n2y;
                    const float bl = gpuLength(bx, by);
                    bx /= bl; by /= bl;
                    const float px = pts[2 * i1], py = pts[2 * i1 + 1];
                    addWedge(px, py, px + n1x * hw, py + n1y * hw, px + bx * miterLen, py + by * miterLen, px + n2x * hw, py + n2y * hw);
                }
            }
            if (!closed && cap == 2) {
                // A square cap: the round one (from distance) and a square.
                for (int end = 0; end < 2; ++end) {
                    const int a = end == 0 ? 1 : n - 2, e = end == 0 ? 0 : n - 1;
                    float tx = pts[2 * e] - pts[2 * a], ty = pts[2 * e + 1] - pts[2 * a + 1];
                    const float tl = gpuLength(tx, ty);
                    if (!(tl > 0)) continue;
                    tx /= tl; ty /= tl;
                    const float nx = -ty * hw, ny = tx * hw, ex = pts[2 * e], ey = pts[2 * e + 1];
                    addWedge(ex + nx, ey + ny, ex + nx + tx * hw, ey + ny + ty * hw, ex - nx + tx * hw, ey - ny + ty * hw, ex - nx, ey - ny);
                }
            }
            for (int k = 0; k < segments; ++k) {
                const int j = (k + 1) % n;
                segsOut.push_back({pts[2 * k], pts[2 * k + 1], pts[2 * j], pts[2 * j + 1]});
            }
            if (!closed && cap == 0) {
                const auto unit = [](float dx, float dy, float& ux, float& uy) { const float l = gpuLength(dx, dy); ux = l > 0 ? dx / l : 0; uy = l > 0 ? dy / l : 0; };
                float sdx, sdy, edx, edy;
                unit(pts[2] - pts[0], pts[3] - pts[1], sdx, sdy);
                unit(pts[2 * (n - 1)] - pts[2 * (n - 2)], pts[2 * (n - 1) + 1] - pts[2 * (n - 2) + 1], edx, edy);
                planes.insert(planes.end(), {pts[0], pts[1], sdx, sdy, pts[2 * (n - 1)], pts[2 * (n - 1) + 1], -edx, -edy});
            }
        }
        if (segsOut.empty()) return true;
        // Two butt ends meeting flush (dashes around a closed outline, where
        // the last meets the first) cut nothing: the stroke carries on.
        for (std::size_t a = 0; a < planes.size(); a += 4)
            for (std::size_t b = a + 4; b < planes.size(); b += 4)
                if (std::abs(planes[a] - planes[b]) < eps && std::abs(planes[a + 1] - planes[b + 1]) < eps &&
                    planes[a + 2] * planes[b + 2] + planes[a + 3] * planes[b + 3] < -0.9f) {
                    planes.erase(planes.begin() + static_cast<std::ptrdiff_t>(b), planes.begin() + static_cast<std::ptrdiff_t>(b + 4));
                    planes.erase(planes.begin() + static_cast<std::ptrdiff_t>(a), planes.begin() + static_cast<std::ptrdiff_t>(a + 4));
                    a -= 4;
                    break;
                }
        const float reach = hw + 1.f;
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (const Seg& g : segsOut) {
            bx0 = std::min(bx0, std::min(g.ax, g.bx)); bx1 = std::max(bx1, std::max(g.ax, g.bx));
            by0 = std::min(by0, std::min(g.ay, g.by)); by1 = std::max(by1, std::max(g.ay, g.by));
        }
        for (std::size_t k = 0; k < wedges.size(); k += 20) {
            bx0 = std::min(bx0, wedges[k]); bx1 = std::max(bx1, wedges[k + 2]);
            by0 = std::min(by0, wedges[k + 1]); by1 = std::max(by1, wedges[k + 3]);
        }
        const auto clampI = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
        const IBox box = IBox{clampI(std::floor(bx0 - reach)), clampI(std::floor(by0 - reach)), clampI(std::ceil(bx1 + reach)), clampI(std::ceil(by1 + reach))}.intersect(limit);
        if (box.empty()) return true;
        if (static_cast<int>(segsOut.size()) <= kWholeEdges && box.area() <= kWholeArea) {
            const std::size_t start = edges.size() / 4;
            for (const Seg& g : segsOut) edges.insert(edges.end(), {g.ax, g.ay, g.bx, g.by});
            edges.insert(edges.end(), planes.begin(), planes.end());
            edges.insert(edges.end(), wedges.begin(), wedges.end());
            GpuInst& i = push();
            i = probe; stamp(i); identity(i);
            i.kind = kGpuStroke; i.flags = static_cast<float>((wedges.size() / 20) << 5);
            i.quad[0] = static_cast<float>(box.x0); i.quad[1] = static_cast<float>(box.y0); i.quad[2] = static_cast<float>(box.x1); i.quad[3] = static_cast<float>(box.y1);
            i.p[0] = static_cast<float>(start); i.p[1] = static_cast<float>(segsOut.size()); i.p[2] = hw; i.p[3] = static_cast<float>(planes.size() / 4);
            i.alpha = s.alpha; i.colour = pack(c);
            applyPaint(i);
            return true;
        }
        const int tx = (box.x1 - box.x0 + kTile - 1) / kTile, ty = (box.y1 - box.y0 + kTile - 1) / kTile;
        static thread_local std::vector<std::vector<int>> bins;
        if (bins.size() < static_cast<std::size_t>(tx) * ty) bins.resize(static_cast<std::size_t>(tx) * ty);
        for (std::size_t k = 0; k < static_cast<std::size_t>(tx) * ty; ++k) bins[k].clear();
        for (std::size_t k = 0; k < segsOut.size(); ++k) {
            const Seg& g = segsOut[k];
            const int x0 = std::clamp(static_cast<int>(std::floor((std::min(g.ax, g.bx) - reach - box.x0) / kTile)), 0, tx - 1);
            const int x1 = std::clamp(static_cast<int>(std::floor((std::max(g.ax, g.bx) + reach - box.x0) / kTile)), 0, tx - 1);
            const int y0 = std::clamp(static_cast<int>(std::floor((std::min(g.ay, g.by) - reach - box.y0) / kTile)), 0, ty - 1);
            const int y1 = std::clamp(static_cast<int>(std::floor((std::max(g.ay, g.by) + reach - box.y0) / kTile)), 0, ty - 1);
            if (std::max(g.ax, g.bx) + reach < box.x0 || std::min(g.ax, g.bx) - reach > box.x1 ||
                std::max(g.ay, g.by) + reach < box.y0 || std::min(g.ay, g.by) - reach > box.y1) continue;
            for (int yy = y0; yy <= y1; ++yy)
                for (int xx = x0; xx <= x1; ++xx) bins[static_cast<std::size_t>(yy) * tx + xx].push_back(static_cast<int>(k));
        }
        for (int yy = 0; yy < ty; ++yy)
            for (int xx = 0; xx < tx; ++xx) {
                const std::vector<int>& list = bins[static_cast<std::size_t>(yy) * tx + xx];
                if (list.empty()) continue;
                const IBox q = IBox{box.x0 + xx * kTile, box.y0 + yy * kTile, box.x0 + (xx + 1) * kTile, box.y0 + (yy + 1) * kTile}.intersect(box);
                if (q.empty()) continue;
                const std::size_t start = edges.size() / 4;
                for (int k : list) {
                    const Seg& g = segsOut[static_cast<std::size_t>(k)];
                    edges.insert(edges.end(), {g.ax, g.ay, g.bx, g.by});
                }
                edges.insert(edges.end(), planes.begin(), planes.end());
                edges.insert(edges.end(), wedges.begin(), wedges.end());
                GpuInst& i = push();
                i = probe; stamp(i); identity(i);
                i.kind = kGpuStroke; i.flags = static_cast<float>((wedges.size() / 20) << 5);
                i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
                i.p[0] = static_cast<float>(start); i.p[1] = static_cast<float>(list.size()); i.p[2] = hw; i.p[3] = static_cast<float>(planes.size() / 4);
                i.alpha = s.alpha; i.colour = pack(c);
                applyPaint(i);
            }
        return true;
    }
    // multiplyDevice: a mask (R8, `stride` wide, its top-left at device
    // (ox, oy)) darkening box `b` toward `floor`.
    gl::GLuint multiplyTex = 0;
    int multiplyW = 0, multiplyH = 0;
    bool multiplyPending = false;
    bool multiply(const std::uint8_t* mask, int stride, int ox, int oy, IBox b, Color floor, const GpuStyle& s) {
        if (s.clipPaths && s.clipPaths->count()) return false;  // the multiply shader reads only masks
        GpuInst probe{};
        IBox limit;
        if (!setClip(probe, s, limit)) return false;
        const IBox q = b.intersect(limit);
        if (q.empty()) return true;
        const int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        if (multiplyPending) draw();  // one mask at a time
        if (!multiplyTex || multiplyW < bw || multiplyH < bh) {
            if (multiplyTex) g.DeleteTextures(1, &multiplyTex);
            multiplyW = std::max(bw, w); multiplyH = std::max(bh, h);
            multiplyTex = texture(gl::R8, multiplyW, multiplyH, gl::NEAREST);
        }
        g.BindTexture(gl::TEXTURE_2D, multiplyTex);
        g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        g.PixelStorei(gl::UNPACK_ROW_LENGTH, stride);
        g.TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, bw, bh, gl::RED, gl::UNSIGNED_BYTE, mask + static_cast<std::size_t>(b.y0 - oy) * stride + (b.x0 - ox));
        g.PixelStorei(gl::UNPACK_ROW_LENGTH, 0);
        multiplyPending = true;
        GpuInst& i = push(multiplyTex, kBlendMultiply);
        i = probe; stamp(i); identity(i);
        i.kind = kGpuMultiply; i.flags = 0;
        i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
        i.img[0] = static_cast<std::int16_t>(-b.x0); i.img[1] = static_cast<std::int16_t>(-b.y0); i.img[2] = 0; i.img[3] = 0;
        i.alpha = s.alpha;
        i.colour = pack(floor);
        return true;
    }
    // Text from glyph masks: each glyph a quad reading its mask from the
    // glyph atlas (uploaded the first time it is drawn), through the coverage
    // ramp for the text's colour.
    bool glyph(const GlyphKey& key, const GlyphMask& gm, int x, int y, int ramp, const GpuStyle& s, Color c) {
        GpuInst probe{};
        IBox limit;
        if (!setClip(probe, s, limit)) return false;
        const IBox q = IBox{x, y, x + gm.w, y + gm.h}.intersect(limit);
        if (q.empty()) return true;
        auto found = glyphPlaced.find(key);
        if (found == glyphPlaced.end()) {
            int ax, ay;
            if (!glyphAtlas.place(gm.w, gm.h, ax, ay)) {
                if (gm.w > glyphAtlas.size || gm.h > glyphAtlas.size) return false;
                draw();  // full: everything drawn from it goes first
                glyphAtlas.reset(); glyphPlaced.clear();
                if (!glyphAtlas.place(gm.w, gm.h, ax, ay)) return false;
            }
            g.BindTexture(gl::TEXTURE_2D, glyphAtlas.tex);
            g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
            g.TexSubImage2D(gl::TEXTURE_2D, 0, ax, ay, gm.w, gm.h, gl::RED, gl::UNSIGNED_BYTE, gm.cov.data());
            found = glyphPlaced.emplace(key, std::make_pair(ax, ay)).first;
        }
        GpuInst& i = push(glyphAtlas.tex);
        i = probe; stamp(i); identity(i);
        i.kind = kGpuGlyph; i.flags = 0;
        i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
        i.img[0] = static_cast<std::int16_t>(found->second.first - x); i.img[1] = static_cast<std::int16_t>(found->second.second - y);
        i.p[0] = static_cast<float>(ramp);
        i.alpha = s.alpha; i.colour = pack(c);
        applyPaint(i);
        return true;
    }
    // A circle (device centre and radius), filled (hw < 0) or stroked with
    // half width hw: exact by distance, nothing to flatten (cp6's circle).
    bool circle(float cx, float cy, float r, float hw, const GpuStyle& s, Color c) {
        // The stroke of the circle just filled, the same way: one instance
        // (cp6's circle), the stroke over the fill.
        if (hw >= 0 && paintRow < 0 && !inst.empty() && lastCircleFill == inst.size() && !batches.empty() && batches.back().blend == kBlendOver) {
            GpuInst& f = inst.back();
            if (f.kind == kGpuCircle && f.p[3] < 0 && f.p[0] == cx && f.p[1] == cy && f.p[2] == r && f.alpha == s.alpha && f.hasMask < 0.5f && !s.clip && !(static_cast<int>(f.flags) & kGpuGradient)) {
                const float reach = r + hw + 1.f;
                const IBox q = IBox{static_cast<int>(std::floor(cx - reach)), static_cast<int>(std::floor(cy - reach)),
                                    static_cast<int>(std::ceil(cx + reach)), static_cast<int>(std::ceil(cy + reach))}.intersect(IBox{0, 0, w, h});
                f.quad[0] = static_cast<float>(q.x0); f.quad[1] = static_cast<float>(q.y0); f.quad[2] = static_cast<float>(q.x1); f.quad[3] = static_cast<float>(q.y1);
                f.p[3] = hw;
                f.img[0] = c.r; f.img[1] = c.g; f.img[2] = c.b; f.img[3] = c.a;
                f.flags = static_cast<float>(static_cast<int>(f.flags) | kGpuFillAndStroke);
                lastCircleFill = 0;
                return true;
            }
        }
        GpuInst probe;  // (filled in only for a mask clip)
        IBox limit;
        if (s.clip) { probe = GpuInst{}; if (!setClip(probe, s, limit)) return false; }
        else limit = limitOf(s);
        const float reach = r + std::max(hw, 0.f) + 1.f;
        const IBox q = IBox{static_cast<int>(std::floor(cx - reach)), static_cast<int>(std::floor(cy - reach)),
                            static_cast<int>(std::ceil(cx + reach)), static_cast<int>(std::ceil(cy + reach))}.intersect(limit);
        if (q.empty()) return true;
        GpuInst& i = push();
        if (s.clip) i = probe; else setClip(i, s, limit);
        stamp(i); identity(i);
        i.kind = kGpuCircle; i.flags = 0;
        i.quad[0] = static_cast<float>(q.x0); i.quad[1] = static_cast<float>(q.y0); i.quad[2] = static_cast<float>(q.x1); i.quad[3] = static_cast<float>(q.y1);
        i.p[0] = cx; i.p[1] = cy; i.p[2] = r; i.p[3] = hw;
        i.alpha = s.alpha; i.colour = pack(c);
        applyPaint(i);
        lastCircleFill = hw < 0 ? inst.size() : 0;
        return true;
    }
    std::size_t lastCircleFill = 0;
    // An image: source rect (sx sy sw sh) of a texture region placed at
    // (ix iy, iw x ih), onto local rect (dx dy dw dh) under `m`.
    bool image(gl::GLuint tex, float ix, float iy, int iw, int ih, const float* f, const Matrix& m, bool nearest, float alpha, const GpuStyle& s, Color tint = Color{255, 255, 255, 255}) {
        GpuInst probe{};
        IBox limit;
        if (!setClip(probe, s, limit)) return false;
        if (limit.empty()) return true;
        GpuInst& i = push(tex);
        i = probe;
        stamp(i);
        i.m[0] = m[0]; i.m[1] = m[1]; i.m[2] = m[2]; i.m[3] = m[3]; i.e = m[4]; i.f = m[5];
        i.kind = kGpuImage;
        i.flags = static_cast<float>((nearest ? kGpuNearest : 0) | (isAxisAligned(m) ? kGpuAxisAligned : 0));
        i.quad[0] = std::min(f[4], f[4] + f[6]); i.quad[1] = std::min(f[5], f[5] + f[7]);
        i.quad[2] = std::max(f[4], f[4] + f[6]); i.quad[3] = std::max(f[5], f[5] + f[7]);
        // Source rect, flipped where the destination size is negative.
        float sx = f[0], sy = f[1], sw = f[2], sh = f[3];
        if (f[6] < 0) { sx += sw; sw = -sw; }
        if (f[7] < 0) { sy += sh; sh = -sh; }
        i.p[0] = sx; i.p[1] = sy; i.p[2] = sw; i.p[3] = sh;
        i.img[0] = static_cast<std::int16_t>(ix); i.img[1] = static_cast<std::int16_t>(iy); i.img[2] = static_cast<std::int16_t>(iw); i.img[3] = static_cast<std::int16_t>(ih);
        i.alpha = alpha;
        i.colour = pack(Color{tint.r, tint.g, tint.b, 255});
        return true;
    }

    // An image whose alpha is coverage for colour `c` (text drawn white).
    bool coverage(gl::GLuint tex, float ix, float iy, int iw, int ih, const float* f, const Matrix& m, const GpuStyle& s, Color c) {
        const bool exact = isAxisAligned(m) && std::abs(m[0] * f[6] - f[2]) < 1e-3f && std::abs(m[3] * f[7] - f[3]) < 1e-3f;
        const std::size_t before = inst.size();
        if (!image(tex, ix, iy, iw, ih, f, m, exact, s.alpha, s)) return false;
        if (inst.size() == before) return true;
        GpuInst& i = inst.back();
        i.flags = static_cast<float>(static_cast<int>(i.flags) | kGpuCoverage);
        i.colour = pack(c);
        return true;
    }
    // Every pixel replaced by `c` (clear(colour), reset()).
    void clearTo(Color c) {
        draw();
        g.BindFramebuffer(gl::FRAMEBUFFER, fbo);
        g.Viewport(0, 0, w, h);
        g.Disable(gl::SCISSOR_TEST);
        const float a = c.a / 255.f;
        g.ClearColor(c.r / 255.f * a, c.g / 255.f * a, c.b / 255.f * a, a);
        g.Clear(gl::COLOR_BUFFER_BIT);
    }

    // ----- drawing -----
    // Runs everything recorded, in order, into the canvas texture.
    void draw() {
        if (inst.empty() && opaque.empty()) { edges.clear(); seq = 0; return; }
        g.BindFramebuffer(gl::FRAMEBUFFER, fbo);
        g.Viewport(0, 0, w, h);
        g.Disable(gl::CULL_FACE); g.Disable(gl::SCISSOR_TEST);
        g.Enable(gl::DEPTH_TEST);
        g.DepthFunc(gl::LESS);
        g.DepthMask(1);
        g.ClearDepth(1.0);
        g.Clear(gl::DEPTH_BUFFER_BIT);
        if (viewW != w || viewH != h) {
            // Program state: set again only when the size changes.
            viewW = w; viewH = h;
            for (const auto& [prog, loc] : {std::pair{programGradients, uViewGradients}, std::pair{programLean, uViewLean}, std::pair{programShapes, uViewShapes}, std::pair{program, uView}}) {
                g.UseProgram(prog);
                g.Uniform2f(loc, static_cast<float>(w), static_cast<float>(h));
            }
        }
        g.UseProgram(program);
        // Edges.
        g.ActiveTexture(gl::TEXTURE0);
        g.BindTexture(gl::TEXTURE_2D, edgeTex);
        const int texels = static_cast<int>(edges.size() / 4);
        const int rows = std::max(1, (texels + kEdgeWidth - 1) / kEdgeWidth);
        if (rows > edgeRows) {
            edgeRows = std::max(rows, std::max(edgeRows * 2, 16));
            g.TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(gl::RGBA32F), kEdgeWidth, edgeRows, 0, gl::RGBA, gl::FLOAT, nullptr);
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::NEAREST));
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::NEAREST));
        }
        if (texels) {
            edges.resize(static_cast<std::size_t>(rows) * kEdgeWidth * 4, 0.f);
            g.PixelStorei(gl::UNPACK_ALIGNMENT, 4);
            g.TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, kEdgeWidth, rows, gl::RGBA, gl::FLOAT, edges.data());
        }
        g.ActiveTexture(gl::TEXTURE0 + 1);
        g.BindTexture(gl::TEXTURE_2D, masks.tex);
        g.ActiveTexture(gl::TEXTURE0 + 3);
        g.BindTexture(gl::TEXTURE_2D, gamma);
        g.ActiveTexture(gl::TEXTURE0 + 4);
        g.BindTexture(gl::TEXTURE_2D, ramps);
        if (!rampKeep.empty()) {
            g.PixelStorei(gl::UNPACK_ALIGNMENT, 4);
            g.TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, kGradientLut, static_cast<gl::GLsizei>(rampKeep.size()), gl::RGBA, gl::UNSIGNED_BYTE, rampPixels.data());
        }
        g.ActiveTexture(gl::TEXTURE0 + 5);
        g.BindTexture(gl::TEXTURE_2D, rampParams);
        if (!rampKeep.empty()) g.TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, 2, static_cast<gl::GLsizei>(rampKeep.size()), gl::RGBA, gl::FLOAT, rampGeometry.data());
        // Kept paths: what was added since the last draw.
        g.ActiveTexture(gl::TEXTURE0 + 6);
        g.BindTexture(gl::TEXTURE_2D, keptTex);
        {
            const int texels = static_cast<int>(kept.size() / 4);
            const int rows = std::max(1, (texels + kEdgeWidth - 1) / kEdgeWidth);
            if (rows > keptRows) {
                keptRows = std::max(rows, std::max(keptRows * 2, 16));
                g.TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(gl::RGBA32F), kEdgeWidth, keptRows, 0, gl::RGBA, gl::FLOAT, nullptr);
                g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::NEAREST));
                g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::NEAREST));
                keptSent = 0;
            }
            if (kept.size() > keptSent) {
                // Whole rows, from the one the new texels start in.
                const int from = static_cast<int>(keptSent / 4) / kEdgeWidth;
                const std::size_t end = static_cast<std::size_t>(rows) * kEdgeWidth * 4;
                const std::size_t have = kept.size();
                kept.resize(end, 0.f);
                g.PixelStorei(gl::UNPACK_ALIGNMENT, 4);
                g.TexSubImage2D(gl::TEXTURE_2D, 0, 0, from, kEdgeWidth, rows - from, gl::RGBA, gl::FLOAT, kept.data() + static_cast<std::size_t>(from) * kEdgeWidth * 4);
                kept.resize(have);
                keptSent = have;
            }
        }
        // Instances.
        g.BindVertexArray(vao);
        g.BindBuffer(gl::ARRAY_BUFFER, vbo);
        // Opaque first, nearest first, then everything else in order.
        std::reverse(opaque.begin(), opaque.end());
        const std::size_t opaqueBytes = opaque.size() * sizeof(GpuInst);
        g.BufferData(gl::ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(opaqueBytes + inst.size() * sizeof(GpuInst)), nullptr, gl::STREAM_DRAW);
        if (!opaque.empty()) g.BufferSubData(gl::ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(opaqueBytes), opaque.data());
        if (!inst.empty()) g.BufferSubData(gl::ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(opaqueBytes), static_cast<gl::GLsizeiptr>(inst.size() * sizeof(GpuInst)), inst.data());
        const auto attributes = [&](std::size_t base) {
            const auto at = [&](gl::GLuint a, int n, gl::GLenum type, bool norm, std::size_t off) {
                g.VertexAttribPointer(a, n, type, norm ? 1 : 0, sizeof(GpuInst), reinterpret_cast<const void*>(base + off));
            };
            at(0, 4, gl::FLOAT, false, offsetof(GpuInst, m));
            at(1, 2, gl::FLOAT, false, offsetof(GpuInst, e));
            at(2, 4, gl::FLOAT, false, offsetof(GpuInst, quad));
            at(3, 4, gl::FLOAT, false, offsetof(GpuInst, p));
            at(4, 3, gl::FLOAT, false, offsetof(GpuInst, maskX));
            at(5, 1, gl::FLOAT, false, offsetof(GpuInst, z));
            at(6, 4, gl::UNSIGNED_SHORT, false, offsetof(GpuInst, clip));
            at(7, 4, gl::SHORT, false, offsetof(GpuInst, img));
            at(8, 4, gl::UNSIGNED_BYTE, true, offsetof(GpuInst, colour));
            at(9, 4, gl::UNSIGNED_BYTE, false, offsetof(GpuInst, kind));
        };
        if (!opaque.empty()) {
            g.Disable(gl::BLEND);
            attributes(0);
            g.ActiveTexture(gl::TEXTURE0 + 2);
            g.BindTexture(gl::TEXTURE_2D, transient.tex);
            g.DrawArraysInstanced(gl::TRIANGLE_STRIP, 0, 4, static_cast<gl::GLsizei>(opaque.size()));
        }
        g.DepthMask(0);
        g.Enable(gl::BLEND);
        int using_ = -1;  // 0 full, 1 gradients, 2 lean, 3 shapes
        // Which program an instance in a plain batch takes.
        const auto programOf = [&](const GpuInst& i) {
            const int k = static_cast<int>(i.kind);
            if ((k != kGpuCircle && k != kGpuDevRect && k != kGpuPath) || (static_cast<int>(i.flags) & kGpuGradient)) return 0;
            if (k == kGpuPath || i.hasMask > 1.5f) return 3;
            const float c0 = i.clip[0], c1 = i.clip[1], c2 = i.clip[2], c3 = i.clip[3];
            return (std::min(i.quad[2], c2) - std::max(i.quad[0], c0)) * (std::min(i.quad[3], c3) - std::max(i.quad[1], c1)) > 65536.f ? 2 : 3;
        };
        bool attributesSet = false;
        progOf.resize(inst.size());
        for (std::size_t k = 0; k < inst.size(); ++k) progOf[k] = static_cast<std::uint8_t>(programOf(inst[k]));
        gl::GLuint boundImage = 0;
        int boundBlend = -1;
        g.ActiveTexture(gl::TEXTURE0 + 2);
        for (const Batch& b : batches) {
            if (!b.count) continue;
            const gl::GLuint image = b.image ? b.image : transient.tex;
            if (image != boundImage) { g.BindTexture(gl::TEXTURE_2D, image); boundImage = image; }
            if (b.blend != boundBlend) {
                if (b.blend == kBlendClear) g.BlendFunc(gl::ZERO, gl::ONE_MINUS_SRC_ALPHA);
                else if (b.blend == kBlendMultiply) g.BlendFuncSeparate(gl::ZERO, gl::SRC_COLOR, gl::ZERO, gl::ONE);
                else g.BlendFunc(gl::ONE, gl::ONE_MINUS_SRC_ALPHA);
                boundBlend = b.blend;
            }
            // Runs: big rects and circles through the lean program.
            const bool plainBatch = !b.gradients && b.blend == kBlendOver;
            if (drawBaseInstance && !attributesSet) { attributes(opaqueBytes); attributesSet = true; }
            for (int at = b.start, end = b.start + b.count; at < end;) {
                int want = b.gradients ? 1 : 0, stop = end;
                if (plainBatch) {
                    want = progOf[static_cast<std::size_t>(at)];
                    stop = at + 1;
                    while (stop < end && progOf[static_cast<std::size_t>(stop)] == want) ++stop;
                }
                if (want != using_) { g.UseProgram(want == 3 ? programShapes : want == 2 ? programLean : want == 1 ? programGradients : program); using_ = want; }
                if (drawBaseInstance) drawBaseInstance(gl::TRIANGLE_STRIP, 0, 4, stop - at, static_cast<gl::GLuint>(at));
                else {
                    attributes(opaqueBytes + static_cast<std::size_t>(at) * sizeof(GpuInst));
                    g.DrawArraysInstanced(gl::TRIANGLE_STRIP, 0, 4, stop - at);
                }
                at = stop;
            }
        }
        g.BindVertexArray(0);
        g.DepthMask(1);
        g.Disable(gl::DEPTH_TEST);
        inst.clear(); opaque.clear(); edges.clear(); batches.clear(); seq = 0; lastCircleFill = 0; lastPath = 0; lastKept = nullptr;
        ++drawGen; clipLists.clear(); clipKeep.clear();
        // A draw in the middle of an op keeps the paint it was using.
        const std::shared_ptr<const GradientImpl> current = paintRow >= 0 ? rampKeep[static_cast<std::size_t>(paintRow)] : nullptr;
        rampRow.clear(); rampKeep.clear(); rampPixels.clear(); rampGeometry.clear(); paintRow = -1;
        if (current) useGradient(current, paintM);
        maskPlaced.clear(); maskKeep.clear(); masks.reset();
        multiplyPending = false;
        transient.reset();
        if (!oneOff.empty()) { g.DeleteTextures(static_cast<gl::GLsizei>(oneOff.size()), oneOff.data()); oneOff.clear(); }
    }
    // Writes straight RGBA rows into the canvas texture (putImageData).
    void put(const std::uint8_t* rgba, int stride, int dx, int dy, IBox b) {
        b = b.intersect({0, 0, w, h});
        if (b.empty()) return;
        draw();
        const int bw = b.x1 - b.x0;
        scratch.resize(static_cast<std::size_t>(bw) * (b.y1 - b.y0) * 4);
        std::uint8_t* o = scratch.data();
        for (int y = b.y0; y < b.y1; ++y) {
            const std::uint8_t* row = rgba + (static_cast<std::size_t>(y - dy) * stride + (b.x0 - dx)) * 4;
            for (int x = 0; x < bw; ++x, o += 4, row += 4) premultiplyTo(Color{row[0], row[1], row[2], row[3]}, o);
        }
        g.BindTexture(gl::TEXTURE_2D, colour);
        g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        g.TexSubImage2D(gl::TEXTURE_2D, 0, b.x0, b.y0, bw, b.y1 - b.y0, gl::RGBA, gl::UNSIGNED_BYTE, scratch.data());
    }
    // The canvas onto the current framebuffer (the window), stretched to
    // `vw` x `vh`, row 0 at the top, straight colour as the software path shows it.
    void show(int vw, int vh, bool keepAlpha = false) {
        g.BindFramebuffer(gl::FRAMEBUFFER, 0);
        g.Viewport(0, 0, vw, vh);
        g.Disable(gl::BLEND);
        g.UseProgram(present);
        g.Uniform1i(presentAlpha, keepAlpha ? 1 : 0);
        g.ActiveTexture(gl::TEXTURE0);
        g.BindTexture(gl::TEXTURE_2D, colour);
        // (The texture is NEAREST otherwise; LINEAR only to stretch it.)
        const bool stretch = vw != w || vh != h;
        if (stretch) {
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::LINEAR));
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::LINEAR));
        }
        g.BindVertexArray(emptyVao);
        g.DrawArrays(gl::TRIANGLE_STRIP, 0, 4);
        g.BindVertexArray(0);
        if (stretch) {
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::NEAREST));
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::NEAREST));
        }
    }

    static constexpr const char* kVertex = R"(layout(location=0) in vec4 aM; layout(location=1) in vec2 aEF; layout(location=2) in vec4 aQuad;
layout(location=3) in vec4 aP; layout(location=4) in vec3 aMask; layout(location=5) in float aZ;
layout(location=6) in vec4 aClip; layout(location=7) in vec4 aImg; layout(location=8) in vec4 aColour; layout(location=9) in vec4 aKind;
uniform vec2 uView;
flat out vec4 fP; flat out vec4 fClip; flat out vec4 fMask; flat out vec4 fImg; flat out vec4 fColour;
flat out vec4 fInv; flat out vec2 fInvT; flat out vec4 fQuad; flat out vec4 fDev; flat out vec2 fKind;
void main() {
  vec2 corner = vec2((gl_VertexID & 1) == 0 ? 0.0 : 1.0, (gl_VertexID & 2) == 0 ? 0.0 : 1.0);
  int kind = int(aKind.x + 0.5);
  vec2 dev;
  mat2 M = mat2(aM.x, aM.y, aM.z, aM.w);
  if (kind == 2) {
    // An image quad is in local space: its device box, a pixel wider for the antialiased edge.
    vec2 c0 = M * aQuad.xy + aEF.xy, c1 = M * aQuad.zy + aEF.xy, c2 = M * aQuad.xw + aEF.xy, c3 = M * aQuad.zw + aEF.xy;
    vec2 lo = min(min(c0, c1), min(c2, c3)) - 1.0, hi = max(max(c0, c1), max(c2, c3)) + 1.0;
    lo = max(lo, aClip.xy); hi = min(hi, aClip.zw);
    hi = max(hi, lo);
    dev = mix(lo, hi, corner);
    fDev = vec4(min(min(c0, c1), min(c2, c3)), max(max(c0, c1), max(c2, c3)));
  } else {
    dev = mix(aQuad.xy, aQuad.zw, corner);
    // (A rect ring's inner rect rides in the matrix: rects have no other use for it.)
    fDev = kind == 0 ? aM : vec4(aEF.x, 0.0, 0.0, 0.0);
  }
  float det = aM.x * aM.w - aM.y * aM.z;
  mat2 inv = abs(det) > 1e-20 ? inverse(M) : mat2(0.0);
  fInv = vec4(inv[0], inv[1]);
  fInvT = -(inv * aEF.xy);
  fKind = aKind.xy;
  // Circles and paths keep their stroke colour there, as 0-255.
  fP = aP; fClip = aClip; fMask = vec4(aMask.xy, aKind.z, aMask.z); fImg = kind == 6 || kind == 7 ? aImg / 255.0 : aImg; fColour = aColour; fQuad = aQuad;
  gl_Position = vec4(dev.x / uView.x * 2.0 - 1.0, dev.y / uView.y * 2.0 - 1.0, aZ * 2.0 - 1.0, 1.0);
}
)";
    static constexpr const char* kLeanFragment = R"(flat in vec4 fP; flat in vec4 fClip; flat in vec4 fMask; flat in vec4 fImg; flat in vec4 fColour;
flat in vec4 fInv; flat in vec2 fInvT; flat in vec4 fQuad; flat in vec4 fDev; flat in vec2 fKind;
uniform sampler2D uMask;
out vec4 o;
float span(float a0, float a1, float lo, float hi) { return clamp(min(a1, hi) - max(a0, lo), 0.0, 1.0); }
// A rect ring (strokeRect): outer rect fP less inner rect fDev, each by the
// distance to its nearest edge, as Chrome strokes a rect.
float ringCoverage(vec2 c) {
  vec4 o = vec4(c - fP.xy, fP.zw - c), i = vec4(c - fDev.xy, fDev.zw - c);
  float a = clamp(min(min(o.x, o.y), min(o.z, o.w)) + 0.5, 0.0, 1.0), b = clamp(min(min(i.x, i.y), min(i.z, i.w)) + 0.5, 0.0, 1.0);
  return max(a - b, 0.0);
}
void main() {
  vec2 pix = floor(gl_FragCoord.xy);
  if (pix.x < fClip.x || pix.y < fClip.y || pix.x >= fClip.z || pix.y >= fClip.w) discard;
  float cov;
  vec4 F = vec4(fColour.rgb * fColour.a, fColour.a);
  if (int(fKind.x + 0.5) == 0) {
    cov = ((int(fKind.y + 0.5) & 32) != 0 ? ringCoverage(pix + 0.5) : span(fP.x, fP.z, pix.x, pix.x + 1.0) * span(fP.y, fP.w, pix.y, pix.y + 1.0));
  } else {
    float dc = length(pix + 0.5 - fP.xy) - fP.z;
    if ((int(fKind.y + 0.5) & 4) != 0) {
      float fa = clamp(0.5 - dc, 0.0, 1.0), d = abs(dc);
      float sa = clamp(min(d + 0.5, fP.w) - max(d - 0.5, -fP.w), 0.0, 1.0) * fImg.a;
      vec4 P = vec4(fImg.rgb * sa, sa) + F * fa * (1.0 - sa);
      if (fMask.z > 0.5) P *= texelFetch(uMask, ivec2(pix + fMask.xy), 0).r;
      o = P * fMask.w;
      return;
    }
    if (fP.w < 0.0) cov = clamp(0.5 - dc, 0.0, 1.0);
    else { float d = abs(dc); cov = clamp(min(d + 0.5, fP.w) - max(d - 0.5, -fP.w), 0.0, 1.0); }
  }
  if (fMask.z > 0.5) cov *= texelFetch(uMask, ivec2(pix + fMask.xy), 0).r;
  o = F * (cov * fMask.w);
}
)";
    static constexpr const char* kFragment = R"(flat in vec4 fP; flat in vec4 fClip; flat in vec4 fMask; flat in vec4 fImg; flat in vec4 fColour;
flat in vec4 fInv; flat in vec2 fInvT; flat in vec4 fQuad; flat in vec4 fDev; flat in vec2 fKind;
uniform highp sampler2D uEdges; uniform sampler2D uMask; uniform sampler2D uImage; uniform sampler2D uGamma;
uniform sampler2D uRamps; uniform highp sampler2D uRampParams; uniform highp sampler2D uKept;
#ifdef GRADIENTS
// A gradient's colour at device point `c` (premultiplied), as the
// rasterizer's shader computes it.
vec4 gradient(vec2 c, int row) {
  vec4 a = texelFetch(uRampParams, ivec2(0, row), 0), b = texelFetch(uRampParams, ivec2(1, row), 0);
  vec2 p = mat2(fInv.xy, fInv.zw) * c + fInvT;
  int kind = int(a.x + 0.5);
  float t;
  if (kind == 0) {
    vec2 d = vec2(a.w, b.x) - a.yz;
    t = dot(p - a.yz, d) / dot(d, d);
  } else if (kind == 2) {
    t = (atan(p.y - a.w, p.x - a.z) - a.y) / 6.28318530718;
    t -= floor(t);
  } else {
    vec2 c0 = a.yz; float r0 = a.w; vec2 c1 = b.xy; float r1 = b.z;
    vec2 cd = c1 - c0, pd = p - c0; float dr = r1 - r0;
    float A = dot(cd, cd) - dr * dr, B = dot(pd, cd) + r0 * dr, C = dot(pd, pd) - r0 * r0;
    float w = -1.0; bool ok = false;
    if (abs(A) < 1e-6) { if (abs(B) > 1e-9) { w = C / (2.0 * B); ok = r0 + w * dr >= 0.0; } }
    else {
      float disc = B * B - A * C;
      if (disc >= 0.0) {
        float sq = sqrt(disc), w1 = (B + sq) / A, w2 = (B - sq) / A;
        float hiW = max(w1, w2), loW = min(w1, w2);
        if (r0 + hiW * dr >= 0.0) { w = hiW; ok = true; } else if (r0 + loW * dr >= 0.0) { w = loW; ok = true; }
      }
    }
    if (!ok) return vec4(0.0);
    t = w;
  }
  int idx = int(clamp(t, 0.0, 1.0) * 1023.0 + 0.5);
  return texelFetch(uRamps, ivec2(idx, row), 0);
}
#endif
out vec4 o;
// Integral over the edge's height of how much of [x, x + 1] lies right of it.
float F(float u) { return u <= 0.0 ? 0.0 : u >= 1.0 ? u - 0.5 : 0.5 * u * u; }
// cp6's path distance: subpaths of (count, closed) then points, a texel each.
// A slot: half a texel.
vec2 PS(int s) { int t = s >> 1; vec4 v = texelFetch(uEdges, ivec2(t & 2047, t >> 11), 0); return (s & 1) != 0 ? v.zw : v.xy; }
vec4 PT4(int i) { return texelFetch(uEdges, ivec2(i & 2047, i >> 11), 0); }
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
int windingOf(vec2 p, vec2 a, vec2 b) {
  if (a.y <= p.y) { if (b.y > p.y && cross2(b - a, p - a) > 0.0) return 1; }
  else if (b.y <= p.y && cross2(b - a, p - a) < 0.0) return -1;
  return 0;
}
// Distance to a stroke segment; s/e: it starts/ends an open subpath (caps:
// 0 butt, 1 round, 2 square).
float segDistance(vec2 p, vec2 a, vec2 b, int cap, bool s, bool e, float hw) {
  vec2 ba = b - a; float l2 = max(dot(ba, ba), 1e-12);
  if (cap != 1 && (s || e)) {
    vec2 dir = ba * inversesqrt(l2);
    if (cap == 2) { if (s) a -= dir * hw; if (e) b += dir * hw; ba = b - a; l2 = max(dot(ba, ba), 1e-12); }
    float t = dot(p - a, ba) / l2;
    if ((s && t < 0.0) || (e && t > 1.0)) return 1e20;
  }
  float h = clamp(dot(p - a, ba) / l2, 0.0, 1.0);
  return length(p - a - ba * h);
}
// (signed fill distance, stroke distance). Squared distances in the loop,
// one square root at the end.
vec2 pathDistance(vec2 p, int off, int cnt, bool evenOdd, int cap, float hw) {
  float dF2 = 1e30, dS2 = 1e30, dS = 1e20; int w = 0; int i = off; int end = off + cnt;
  while (i < end) {
    vec2 hd = PS(i); int n = int(hd.x + 0.5); bool closed = hd.y > 0.5;
    if (hd.y > 1.5) {
      // A band's segments, a texel each (a.xy, b.xy), after a whole-texel
      // header. With open subpaths stroked (y 3), each has a second texel:
      // (starts one, ends one, only filled).
      // Columns (y 4, 5) hold only the segments near their own column: the
      // winding is counted along a ray down it instead of across.
      int type = int(hd.y + 0.5);
      bool flagged = type == 3 || type == 5, columns = type >= 4;
      int t0 = (i >> 1) + 1, per = flagged ? 2 : 1;
      for (int k = 0; k < n; k++) {
        vec4 sg = PT4(t0 + per * k);
        vec2 pa = p - sg.xy, ba = sg.zw - sg.xy;
        vec2 v = pa - ba * clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
        float d2 = dot(v, v);
        dF2 = min(dF2, d2);
        if (flagged) {
          vec4 fl = PT4(t0 + 2 * k + 1);
          if (fl.z < 0.5) {
            if (cap != 1 && (fl.x > 0.5 || fl.y > 0.5)) dS = min(dS, segDistance(p, sg.xy, sg.zw, cap, fl.x > 0.5, fl.y > 0.5, hw));
            else dS2 = min(dS2, d2);
          }
        } else dS2 = min(dS2, d2);
        w += columns ? windingOf(p.yx, sg.yx, sg.wz) : windingOf(p, sg.xy, sg.zw);
      }
      i += 2 + 2 * per * n;
      continue;
    }
    i++;
    bool capped = !closed && cap != 1;
    vec2 first = PS(i); vec2 prev = first;
    for (int k = 1; k < n; k++) {
      vec2 cur = PS(i + k);
      vec2 pa = p - prev, ba = cur - prev;
      vec2 v = pa - ba * clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
      float d2 = dot(v, v);
      dF2 = min(dF2, d2);
      if (capped && (k == 1 || k == n - 1)) dS = min(dS, segDistance(p, prev, cur, cap, k == 1, k == n - 1, hw));
      else dS2 = min(dS2, d2);
      w += windingOf(p, prev, cur);
      prev = cur;
    }
    if (n > 1) {
      vec2 pa = p - prev, ba = first - prev;
      vec2 v = pa - ba * clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
      float d2 = dot(v, v);
      dF2 = min(dF2, d2); if (closed) dS2 = min(dS2, d2);
      w += windingOf(p, prev, first);
    } else if (cap != 0) {
      dS = min(dS, length(p - first));
    }
    i += n;
  }
  float dF = sqrt(dF2);
  dS = min(dS, sqrt(dS2));
  bool inside = evenOdd ? (w & 1) != 0 : w != 0;
  return vec2(inside ? -dF : dF, dS);
}
// A path kept in its own space (GpuSurface::keepPath) at device point `pc`:
// (signed fill distance, stroke distance) in device pixels. The pixel goes
// back into the path's space, finds its band there, and the distance found
// there is scaled out (the transform is a similarity).
vec2 keptDistance(vec2 pc, int h, bool evenOdd) {
  vec2 p = mat2(fInv.xy, fInv.zw) * pc + fInvT;
  vec4 hd = texelFetch(uKept, ivec2(h & 2047, h >> 11), 0);
  int b = clamp(int(floor((p.y - hd.x) * hd.y)), 0, int(hd.z + 0.5) - 1);
  int bt = h + 1 + b;
  vec4 bd = texelFetch(uKept, ivec2(bt & 2047, bt >> 11), 0);
  int s0 = int(bd.x + 0.5), n = int(bd.y + 0.5);
  float d2 = 1e30; int w = 0;
  for (int k = 0; k < n; k++) {
    int t = s0 + k;
    vec4 sg = texelFetch(uKept, ivec2(t & 2047, t >> 11), 0);
    vec2 pa = p - sg.xy, ba = sg.zw - sg.xy;
    vec2 v = pa - ba * clamp(dot(pa, ba) / max(dot(ba, ba), 1e-30), 0.0, 1.0);
    d2 = min(d2, dot(v, v));
    w += windingOf(p, sg.xy, sg.zw);
  }
  float d = sqrt(d2) * inversesqrt(abs(fInv.x * fInv.w - fInv.y * fInv.z));
  bool inside = evenOdd ? (w & 1) != 0 : w != 0;
  return vec2(inside ? -d : d, d);
}
// A cp6 path at pixel `pix`: (signed fill distance, stroke distance, half width).
vec3 pathAt(vec2 pix, int flags) {
  if ((flags & 32) != 0) return vec3(keptDistance(pix + 0.5, int(fP.x + 0.5), fP.z > 0.5), fP.w);
  return vec3(pathDistance(pix + 0.5, int(fP.x + 0.5), int(fP.y + 0.5), fP.z > 0.5, int(fP.w + 0.5), fDev.x), fDev.x);
}
float span(float a0, float a1, float lo, float hi) { return clamp(min(a1, hi) - max(a0, lo), 0.0, 1.0); }
// A rect ring (strokeRect): outer rect fP less inner rect fDev, each by the
// distance to its nearest edge, as Chrome strokes a rect.
float ringCoverage(vec2 c) {
  vec4 o = vec4(c - fP.xy, fP.zw - c), i = vec4(c - fDev.xy, fDev.zw - c);
  float a = clamp(min(min(o.x, o.y), min(o.z, o.w)) + 0.5, 0.0, 1.0), b = clamp(min(min(i.x, i.y), min(i.z, i.w)) + 0.5, 0.0, 1.0);
  return max(a - b, 0.0);
}
vec4 texel(ivec2 t) {
  t = clamp(t, ivec2(0), ivec2(fImg.zw) - 1);
  vec4 c = texelFetch(uImage, t + ivec2(fImg.xy), 0);
  return vec4(c.rgb * c.a, c.a);
}
float clipCoverage(vec2 pix) {
  float cov = 1.0;
  if (fMask.z > 1.5) {
    // cp6's clips: each path or circle, by distance.
    vec2 pc = pix + 0.5;
    int list = int(fMask.x + 0.5), nc = int(fMask.y + 0.5);
    for (int k = 0; k < nc; ++k) {
      vec4 c = texelFetch(uEdges, ivec2((list + k) & 2047, (list + k) >> 11), 0);
      int ck = int(c.z + 0.5);
      float d;
      if (ck >= 2) {
        vec4 cd = texelFetch(uEdges, ivec2(int(c.x + 0.5) & 2047, int(c.x + 0.5) >> 11), 0);
        d = length(pc - cd.xy) - cd.z;
        if (ck == 3) d = -d;
      } else d = pathDistance(pc, int(c.x + 0.5), int(c.y + 0.5), ck == 1, 1, 0.0).x;
      cov *= clamp(0.5 - d, 0.0, 1.0);
    }
  } else if (fMask.z > 0.5) cov = texelFetch(uMask, ivec2(pix + fMask.xy), 0).r;
  return cov;
}
#ifdef SHAPES
// Rects, circles and cp6 paths only (most of a scene), without the rest's
// registers.
void main() {
  vec2 pix = floor(gl_FragCoord.xy);
  if (pix.x < fClip.x || pix.y < fClip.y || pix.x >= fClip.z || pix.y >= fClip.w) discard;
  int kind = int(fKind.x + 0.5);
  int flags = int(fKind.y + 0.5);
  vec4 F = vec4(fColour.rgb * fColour.a, fColour.a);
  vec4 paint;
  if (kind == 7) {
    vec3 r = pathAt(pix, flags);
    float hw = r.z;
    float fa = (flags & 64) != 0 ? clamp(0.5 - r.x, 0.0, 1.0) : 0.0;
    float sa = (flags & 128) != 0 ? clamp(min(r.y + 0.5, hw) - max(r.y - 0.5, -hw), 0.0, 1.0) * fImg.a : 0.0;
    paint = vec4(fImg.rgb * sa, sa) + F * fa * (1.0 - sa);
  } else if (kind == 0) {
    paint = F * (((int(fKind.y + 0.5) & 32) != 0 ? ringCoverage(pix + 0.5) : span(fP.x, fP.z, pix.x, pix.x + 1.0) * span(fP.y, fP.w, pix.y, pix.y + 1.0)));
  } else {
    float dc = length(pix + 0.5 - fP.xy) - fP.z;
    if ((flags & 4) != 0) {
      float fa = clamp(0.5 - dc, 0.0, 1.0), d = abs(dc);
      float sa = clamp(min(d + 0.5, fP.w) - max(d - 0.5, -fP.w), 0.0, 1.0) * fImg.a;
      paint = vec4(fImg.rgb * sa, sa) + F * fa * (1.0 - sa);
    } else if (fP.w < 0.0) paint = F * clamp(0.5 - dc, 0.0, 1.0);
    else { float d = abs(dc); paint = F * clamp(min(d + 0.5, fP.w) - max(d - 0.5, -fP.w), 0.0, 1.0); }
  }
  if (fMask.z > 0.5) paint *= clipCoverage(pix);
  o = paint * fMask.w;
}
#else
void main() {
  vec2 pix = floor(gl_FragCoord.xy);
  if (pix.x < fClip.x || pix.y < fClip.y || pix.x >= fClip.z || pix.y >= fClip.w) discard;
  int kind = int(fKind.x + 0.5);
  int flags = int(fKind.y + 0.5);
  if (kind == 4) {
    // Multiply: the factor each channel keeps (the blend multiplies by it).
    float m = texelFetch(uImage, ivec2(pix + fImg.xy), 0).r;
    if (fMask.z > 0.5 && fMask.z < 1.5) m *= texelFetch(uMask, ivec2(pix + fMask.xy), 0).r;
    o = vec4(1.0 - m * fMask.w * (1.0 - fColour.rgb), 1.0);
    return;
  }
  float cov = 0.0;
  vec4 paint = vec4(fColour.rgb * fColour.a, fColour.a);
#ifdef GRADIENTS
  if ((flags & 8) != 0) paint = gradient(pix + 0.5, int(fColour.r * 255.0 + 0.5) + 256 * int(fColour.g * 255.0 + 0.5));
#endif
  if (kind == 0) {
    cov = ((int(fKind.y + 0.5) & 32) != 0 ? ringCoverage(pix + 0.5) : span(fP.x, fP.z, pix.x, pix.x + 1.0) * span(fP.y, fP.w, pix.y, pix.y + 1.0));
  } else if (kind == 1 && fP.w > 2.5) {
    // A simple polygon: inside by winding at the centre, the edge by distance.
    vec2 pc = pix + 0.5;
    float nearest = 1e20;
    int w = 0;
    int start = int(fP.x + 0.5), n = int(fP.y + 0.5);
    for (int k = 0; k < n; ++k) {
      int idx = start + k;
      vec4 e = texelFetch(uEdges, ivec2(idx & 2047, idx >> 11), 0);
      vec2 a = e.xy, ab = e.zw - e.xy;
      float t = clamp(dot(pc - a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
      vec2 q = pc - a - ab * t;
      nearest = min(nearest, dot(q, q));
      if ((a.y <= pc.y) != (e.w <= pc.y)) {
        float x = a.x + (pc.y - a.y) * ab.x / ab.y;
        if (x < pc.x) w += ab.y > 0.0 ? 1 : -1;
      }
    }
    bool inside = fP.z > 0.5 ? (w & 1) != 0 : w != 0;
    float d = sqrt(nearest);
    cov = clamp(0.5 + (inside ? d : -d), 0.0, 1.0);
  } else if (kind == 1 && fP.w > 1.5) {
    // Convex, as half-planes: the largest signed distance to an edge line.
    vec2 pc = pix + 0.5;
    float sd = -1e20;
    int start = int(fP.x + 0.5), n = int(fP.y + 0.5);
    for (int k = 0; k < n; ++k) {
      int idx = start + k;
      vec4 e = texelFetch(uEdges, ivec2(idx & 2047, idx >> 11), 0);
      sd = max(sd, dot(e.xy, pc) + e.z);
    }
    cov = clamp(0.5 - sd, 0.0, 1.0);
  } else if (kind == 1) {
    float acc = 0.0;
    bool touched = false;  // an edge crosses this pixel
    int start = int(fP.x + 0.5), n = int(fP.y + 0.5);
    float R = pix.x + 1.0, top = pix.y, bottom = pix.y + 1.0;
    for (int k = 0; k < n; ++k) {
      int idx = start + k;
      vec4 e = texelFetch(uEdges, ivec2(idx & 2047, idx >> 11), 0);
      float dir = e.y < e.w ? 1.0 : -1.0;
      vec2 a = e.y < e.w ? e.xy : e.zw, b = e.y < e.w ? e.zw : e.xy;
      float ya = max(a.y, top), yb = min(b.y, bottom);
      if (ya >= yb) continue;
      float dxdy = (b.x - a.x) / (b.y - a.y);
      float xa = a.x + (ya - a.y) * dxdy, xb = a.x + (yb - a.y) * dxdy;
      if (max(xa, xb) > pix.x && min(xa, xb) < R) touched = true;
      float ua = R - xa, ub = R - xb;
      float avg = abs(ub - ua) < 1e-6 ? clamp(ua, 0.0, 1.0) : (F(ub) - F(ua)) / (ub - ua);
      acc += dir * (yb - ya) * avg;
    }
    // The summed areas are the coverage exactly when the winding inside the
    // pixel only takes 0 and one other value (+1 or -1). Where pieces overlap
    // (a stroke's segments and joins, a path crossing itself) they count the
    // overlap twice: then lines through the pixel, exact across, decide. A
    // pixel no edge crosses has one winding throughout, which the clamp (or
    // the even-odd fold) already reads right.
    bool evenOdd = fP.z > 0.5;
    int lo = 0, hi = 0;
    float sampled = 0.0;
    for (int r = 0; r < (fP.w > 0.5 || !touched ? 0 : 8); ++r) {
      float sy = top + (float(r) + 0.5) * 0.125;
      int w = 0, m = 0;
      float xs[8]; int ds[8];
      for (int k = 0; k < n; ++k) {
        int idx = start + k;
        vec4 e = texelFetch(uEdges, ivec2(idx & 2047, idx >> 11), 0);
        float y0 = min(e.y, e.w), y1 = max(e.y, e.w);
        if (sy < y0 || sy >= y1) continue;
        int d = e.y < e.w ? 1 : -1;
        float x = e.x + (sy - e.y) * (e.z - e.x) / (e.w - e.y);
        if (x <= pix.x) { w += d; continue; }
        if (x >= R) continue;
        if (m == 8) { if (x < pix.x + 0.5) w += d; continue; }
        int j = m++;
        while (j > 0 && xs[j - 1] > x) { xs[j] = xs[j - 1]; ds[j] = ds[j - 1]; --j; }
        xs[j] = x; ds[j] = d;
      }
      lo = min(lo, w); hi = max(hi, w);
      float cur = pix.x, row = 0.0;
      for (int j = 0; j < m; ++j) {
        bool inside = evenOdd ? (w & 1) != 0 : w != 0;
        if (inside) row += xs[j] - cur;
        cur = xs[j]; w += ds[j];
        lo = min(lo, w); hi = max(hi, w);
      }
      if (evenOdd ? (w & 1) != 0 : w != 0) row += R - cur;
      sampled += row * 0.125;
    }
    if ((lo >= 0 && hi <= 1) || (lo >= -1 && hi <= 0)) {
      if (evenOdd) { float c = abs(acc); c -= 2.0 * floor(c * 0.5); cov = c > 1.0 ? 2.0 - c : c; }
      else cov = min(abs(acc), 1.0);
    } else cov = sampled;
  } else if (kind == 7) {
    // cp6's path: the fill under the stroke, one instance.
    vec3 r = pathAt(pix, flags);
    float hw = r.z;
    float fa = (flags & 64) != 0 ? clamp(0.5 - r.x, 0.0, 1.0) * fColour.a : 0.0;
    float sa = (flags & 128) != 0 ? clamp(min(r.y + 0.5, hw) - max(r.y - 0.5, -hw), 0.0, 1.0) * fImg.a : 0.0;
    paint = vec4(fImg.rgb * sa, sa) + vec4(fColour.rgb * fa, fa) * (1.0 - sa);
    cov = 1.0;
  } else if (kind == 6) {
    float dc = length(pix + 0.5 - fP.xy) - fP.z;
    if ((flags & 4) != 0) {
      // Filled and stroked: the stroke (colour in fImg) over the fill.
      float fa = clamp(0.5 - dc, 0.0, 1.0), d = abs(dc);
      float sa = clamp(min(d + 0.5, fP.w) - max(d - 0.5, -fP.w), 0.0, 1.0) * fImg.a;
      vec4 F = vec4(fColour.rgb * fColour.a, fColour.a) * fa, S = vec4(fImg.rgb * sa, sa);
      o = (S + F * (1.0 - sa)) * fMask.w;
      return;
    }
    if (fP.w < 0.0) cov = clamp(0.5 - dc, 0.0, 1.0);
    else { float d = abs(dc); cov = clamp(min(d + 0.5, fP.w) - max(d - 0.5, -fP.w), 0.0, 1.0); }
  } else if (kind == 5) {
    float m = texelFetch(uImage, ivec2(pix + fImg.xy), 0).r;
    cov = texelFetch(uGamma, ivec2(int(m * 255.0 + 0.5), int(fP.x + 0.5)), 0).r;
  } else if (kind == 3) {
    // Stroke: the nearest centre-line segment decides (as cp6); then each
    // butt end within reach cuts what lies beyond its plane, over half a pixel.
    vec2 pc = pix + 0.5;
    float hw = fP.z, best = 0.0;
    int start = int(fP.x + 0.5), n = int(fP.y + 0.5), caps = int(fP.w + 0.5);
    float nearest = 1e20;
    vec2 onLine = pc;  // the nearest centre-line point
    for (int k = 0; k < n; ++k) {
      int idx = start + k;
      vec4 e = texelFetch(uEdges, ivec2(idx & 2047, idx >> 11), 0);
      vec2 a = e.xy, ab = e.zw - e.xy;
      float l2 = dot(ab, ab);
      float t = l2 > 0.0 ? clamp(dot(pc - a, ab) / l2, 0.0, 1.0) : 0.0;
      vec2 q = pc - a - ab * t;
      float q2 = dot(q, q);
      if (q2 < nearest) { nearest = q2; onLine = a + ab * t; }
    }
    float d = sqrt(nearest);
    best = clamp(min(d + 0.5, hw) - max(d - 0.5, -hw), 0.0, 1.0);
    // Miter and square-cap wedges: convex quads, by their largest signed
    // distance to an edge.
    int wedgeCount = flags >> 5;
    for (int k = 0; k < wedgeCount; ++k) {
      int idx = start + n + caps + 5 * k;
      vec4 bb = texelFetch(uEdges, ivec2(idx & 2047, idx >> 11), 0);
      if (pc.x < bb.x - 1.0 || pc.y < bb.y - 1.0 || pc.x > bb.z + 1.0 || pc.y > bb.w + 1.0) continue;
      vec4 l0 = texelFetch(uEdges, ivec2((idx + 1) & 2047, (idx + 1) >> 11), 0);
      vec4 l1 = texelFetch(uEdges, ivec2((idx + 2) & 2047, (idx + 2) >> 11), 0);
      vec4 l2 = texelFetch(uEdges, ivec2((idx + 3) & 2047, (idx + 3) >> 11), 0);
      vec4 l3 = texelFetch(uEdges, ivec2((idx + 4) & 2047, (idx + 4) >> 11), 0);
      // 16 floats: four lines (nx, ny, c) and a pad, packed in order.
      float L[16] = float[16](l0.x, l0.y, l0.z, l0.w, l1.x, l1.y, l1.z, l1.w, l2.x, l2.y, l2.z, l2.w, l3.x, l3.y, l3.z, l3.w);
      float sd = max(max(dot(pc, vec2(L[0], L[1])) + L[2], dot(pc, vec2(L[3], L[4])) + L[5]),
                     max(dot(pc, vec2(L[6], L[7])) + L[8], dot(pc, vec2(L[9], L[10])) + L[11]));
      best = max(best, clamp(0.5 - sd, 0.0, 1.0));
    }
    for (int k = 0; k < caps; ++k) {
      int idx = start + n + k;
      vec4 pl = texelFetch(uEdges, ivec2(idx & 2047, idx >> 11), 0);
      // An end cuts only the stroke it ends (not a dash or line beside it).
      if (length(onLine - pl.xy) < 1.0) best *= clamp(0.5 + dot(pc - pl.xy, pl.zw), 0.0, 1.0);
    }
    cov = best;
  } else {
    // Image: the pixel centre in local space, then in the source.
    vec2 centre = pix + 0.5;
    vec2 local = mat2(fInv.xy, fInv.zw) * centre + fInvT;
    vec2 size = fQuad.zw - fQuad.xy;
    vec2 uv = fP.xy + (local - fQuad.xy) / size * fP.zw;
    if ((flags & 2) != 0) {
      cov = span(fDev.x, fDev.z, pix.x, pix.x + 1.0) * span(fDev.y, fDev.w, pix.y, pix.y + 1.0);
    } else {
      vec2 q = abs(local - (fQuad.xy + fQuad.zw) * 0.5) - size * 0.5;
      float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
      float scale = sqrt(abs(fInv.x * fInv.w - fInv.y * fInv.z));
      cov = clamp(0.5 - d / max(scale, 1e-12), 0.0, 1.0);
    }
    if ((flags & 1) != 0) {
      paint = texel(ivec2(floor(uv)));
    } else {
      vec2 s = uv - 0.5;
      vec2 f = fract(s);
      ivec2 i0 = ivec2(floor(s));
      vec4 t00 = texel(i0), t10 = texel(i0 + ivec2(1, 0)), t01 = texel(i0 + ivec2(0, 1)), t11 = texel(i0 + ivec2(1, 1));
      paint = mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);
    }
    // A coverage image (text) paints its colour through the image's alpha.
    if ((flags & 16) != 0) paint = vec4(fColour.rgb * fColour.a, fColour.a) * paint.a;
    else paint.rgb *= fColour.rgb;
  }
  if (fMask.z > 0.5) cov *= clipCoverage(pix);
  o = paint * (cov * fMask.w);
}
#endif
)";
    static constexpr const char* kPresentVertex = R"(out vec2 vUv;
void main() {
  vec2 c = vec2((gl_VertexID & 1) == 0 ? 0.0 : 1.0, (gl_VertexID & 2) == 0 ? 0.0 : 1.0);
  vUv = vec2(c.x, 1.0 - c.y);
  gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static constexpr const char* kPresentFragment = R"(in vec2 vUv; uniform sampler2D uTex; uniform int uKeepAlpha; out vec4 o;
void main() {
  vec4 c = texture(uTex, vUv);
  // To be seen: opaque, straight colour, as the software path shows it. To
  // be copied (into a 2D canvas): premultiplied, alpha kept.
  o = uKeepAlpha != 0 ? c : vec4(c.a > 0.0 ? c.rgb / c.a : vec3(0.0), 1.0);
}
)";
};

}  // namespace canvas_detail

#ifndef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// Canvas::Soft on the GPU.
// ---------------------------------------------------------------------------
inline Canvas::Soft::~Soft() {
    flushReaders();
    for (Soft* s : sources) s->dropReader(this);
}

inline void Canvas::Soft::syncCpu() {
    if (!gpu || cpuValid) return;
    gpu->draw();
    gpu->readback(px, canvasBox());
    cpuValid = true;
}
inline void Canvas::Soft::cpuOverwrite() {
    if (gpu) { gpu->draw(); gpuValid = false; }
    cpuValid = true;
    ++contentVersion;
}

// Whether the shader can draw `op` as the rasterizer would.
inline bool Canvas::Soft::gpuCan(const Op& op, const Style& s) const {
    using namespace canvas_detail;
    switch (op.kind) {
    case OpKind::PutImage: case OpKind::Blit: case OpKind::Multiply: return op.raw == nullptr;
    case OpKind::ClearRect: return isAxisAligned(op.m);
    default: break;
    }
    if (!s.plain()) return false;
    switch (op.kind) {
    case OpKind::FillPath: return !op.glyph && s.fill.kind != PaintStyle::Pattern;
    case OpKind::FillRect: return s.fill.kind != PaintStyle::Pattern;
    case OpKind::Text: return textUsesMasks(op) && s.fill.kind != PaintStyle::Pattern;
    case OpKind::StrokePath: case OpKind::StrokeRect: return s.stroke.kind != PaintStyle::Pattern;
    case OpKind::Image:
        if (op.raw) return false;
        if (op.src) return op.src != this && !op.src->gpu;
        return op.a < images.size() && images[op.a] != nullptr;
    default: return false;
    }
}

// Records `op` for the GPU. False if it turned out not to fit (a clip mask
// bigger than the atlas), nothing recorded: the caller rasterizes it instead.
inline bool Canvas::Soft::gpuDo(const Op& op, const Style& s) {
    using namespace canvas_detail;
    GpuSurface& G = *gpu;
    const GpuStyle gs{s.alpha, s.clip};
    // The paint: a gradient's ramp goes to the GPU with the op's transform.
    G.useSolid();
    {
        const bool strokes = op.kind == OpKind::StrokePath || op.kind == OpKind::StrokeRect;
        const PaintStyle& p = strokes ? s.stroke : s.fill;
        if ((op.kind == OpKind::FillPath || op.kind == OpKind::FillRect || op.kind == OpKind::Text || strokes) && p.kind == PaintStyle::Gradient) {
            const auto gr = p.gradientImpl();
            if (!gr || gr->paintsNothing) return true;
            Matrix inv;
            if (!invert(op.m, inv)) return true;
            G.useGradient(gr, op.m);
        }
    }
    static thread_local Poly flat, dashed, outline, device;
    switch (op.kind) {
    case OpKind::FillPath: {
        if (s.fill.paintsNothing()) return true;
        flatten(segs.data() + op.a, op.b, op.m, matrixMaxScale(op.m), device);
        float X0, Y0, X1, Y1;
        if (!op.evenOdd && polyIsRect(device, X0, Y0, X1, Y1)) return G.rect(X0, Y0, X1, Y1, gs, s.fill.color);
        return G.poly(device, op.evenOdd, gs, s.fill.color);
    }
    case OpKind::FillRect: {
        if (s.fill.paintsNothing()) return true;
        float X0, Y0, X1, Y1;
        if (deviceRect(op.m, op.f[0], op.f[1], op.f[2], op.f[3], X0, Y0, X1, Y1)) return G.rect(X0, Y0, X1, Y1, gs, s.fill.color);
        Path2D box;
        box.rect(op.f[0], op.f[1], op.f[2], op.f[3]);
        flatten(box, op.m, matrixMaxScale(op.m), device);
        return G.poly(device, false, gs, s.fill.color);
    }
    case OpKind::StrokePath: case OpKind::StrokeRect: {
        if (!(s.lineWidth > 0) || s.stroke.paintsNothing()) return true;
        // From the centre line when the transform keeps circles circles and
        // the caps are butt or round; the joins too if they are round, or
        // turn too little to tell apart.
        const Matrix& m = op.m;
        const float det = m[0] * m[3] - m[1] * m[2], scale = std::sqrt(std::abs(det));
        const float tol = 1e-4f * std::max(1.f, scale);
        const bool similar = (std::abs(m[0] - m[3]) < tol && std::abs(m[1] + m[2]) < tol) || (std::abs(m[0] + m[3]) < tol && std::abs(m[1] - m[2]) < tol);
        if (similar && scale > 0) {
            Path2D box;
            if (op.kind == OpKind::StrokeRect) box.rect(op.f[0], op.f[1], op.f[2], op.f[3]);
            const Path2D::Segment* g = op.kind == OpKind::StrokeRect ? box.segments().data() : segs.data() + op.a;
            const std::size_t gn = op.kind == OpKind::StrokeRect ? box.segments().size() : op.b;
            flatten(g, gn, kIdentity, scale, flat);
            const Poly* src = &flat;
            if (!s.dash.empty()) { applyDash(flat, s.dash, s.dashOffset, dashed); src = &dashed; }
            transformPoly(*src, m, device);
            if (G.stroke(device, s.lineWidth * 0.5f * scale, s.lineCap, s.lineJoin, s.miterLimit, gs, s.stroke.color)) return true;
        }
        StrokeParams sp;
        sp.width = s.lineWidth; sp.miterLimit = s.miterLimit; sp.cap = s.lineCap; sp.join = s.lineJoin; sp.dash = &s.dash; sp.dashOffset = s.dashOffset;
        if (op.kind == OpKind::StrokeRect) {
            Path2D box;
            box.rect(op.f[0], op.f[1], op.f[2], op.f[3]);
            strokeToDevice(box.segments().data(), box.segments().size(), op.m, sp, flat, dashed, outline, device);
        } else {
            strokeToDevice(segs.data() + op.a, op.b, op.m, sp, flat, dashed, outline, device);
        }
        return G.poly(device, false, gs, s.stroke.color, true);
    }
    case OpKind::ClearRect: {
        float X0, Y0, X1, Y1;
        if (!deviceRect(op.m, op.f[0], op.f[1], op.f[2], op.f[3], X0, Y0, X1, Y1)) return true;
        const auto snap = [](float v) { return std::floor(std::clamp(v, -1e8f, 1e8f) + 0.5f); };
        return G.rect(snap(X0), snap(Y0), snap(X1), snap(Y1), gs, Color{0, 0, 0, 255}, true);
    }
    case OpKind::Image: {
        const bool nearest = !(s.smoothing && !op.src && !op.fromCanvas);
        const float alpha = s.alpha * std::clamp(op.f[8], 0.f, 1.f);
        if (alpha <= 0) return true;
        if (op.src) {
            int ox, oy;
            const gl::GLuint t = G.canvasImage(op.src->id, op.src->contentVersion, op.src->px, op.src->w, op.src->h, ox, oy);
            return G.image(t, static_cast<float>(ox), static_cast<float>(oy), op.src->w, op.src->h, op.f, op.m, nearest, alpha, gs, op.tinted ? op.tint : Color{255, 255, 255, 255});
        }
        const std::shared_ptr<const ImageCopy>& img = images[op.a];
        if (op.fromCanvas) {
            int ox, oy;
            const gl::GLuint t = G.oneOffImage(img->rgba.data(), img->w, img->h, ox, oy);
            return G.image(t, static_cast<float>(ox), static_cast<float>(oy), img->w, img->h, op.f, op.m, nearest, alpha, gs, op.tinted ? op.tint : Color{255, 255, 255, 255});
        }
        const gl::GLuint t = G.cachedCopy(img);
        return G.image(t, 0, 0, img->w, img->h, op.f, op.m, nearest, alpha, gs, op.tinted ? op.tint : Color{255, 255, 255, 255});
    }
    case OpKind::Blit: {
        // Device pixels, source-over at globalAlpha through the clip.
        const int bw = static_cast<int>(op.b), bh = op.box.y1 - op.box.y0;
        if (bw <= 0 || bh <= 0 || s.alpha <= 0) return true;
        int ox, oy;
        const gl::GLuint t = G.oneOffImage(bytes.data() + op.a, bw, bh, ox, oy);
        const float f[9] = {0, 0, static_cast<float>(bw), static_cast<float>(bh), op.f[0], op.f[1], static_cast<float>(bw), static_cast<float>(bh), 1};
        return G.image(t, static_cast<float>(ox), static_cast<float>(oy), bw, bh, f, Matrix{1, 0, 0, 1, 0, 0}, true, s.alpha, gs);
    }
    case OpKind::Text: {
        // As execText draws masks.
        const PaintStyle& p = s.fill;
        if (p.paintsNothing()) return true;
        const GlyphPos* gp = glyphs.data() + op.a;
        const Matrix& m = op.m;
        const float devSize = op.f[3] * m[0];
        const float lum = (0.2126f * p.color.r + 0.7152f * p.color.g + 0.0722f * p.color.b) / 255.f;
        const int ramp = std::clamp(static_cast<int>(lum * 15.f + 0.5f), 0, 15);
        GlyphCache& cache = GlyphCache::instance();
        const int iy = static_cast<int>(std::lround(m[3] * op.f[1] + m[5]));
        const std::uint32_t sizeQ = static_cast<std::uint32_t>(std::lround(devSize * 64));
        for (std::size_t k = 0; k < op.b; ++k) {
            const float dx = m[0] * (op.f[0] + gp[k].x) + m[4];
            const int ix = static_cast<int>(std::floor(dx));
            const int phase = std::min(3, static_cast<int>((dx - ix) * 4));
            const GlyphMask& gm = cache.get(gp[k].face, gp[k].glyph, devSize, phase);
            if (gm.cov.empty()) continue;
            if (!G.glyph(GpuSurface::GlyphKey{gp[k].face, gp[k].glyph, sizeQ, static_cast<std::uint8_t>(phase)}, gm, ix + gm.left, iy + gm.top, ramp, gs, p.color)) return false;
        }
        return true;
    }
    case OpKind::Multiply:
        return G.multiply(bytes.data() + op.a, static_cast<int>(op.b), static_cast<int>(op.f[0]), static_cast<int>(op.f[1]), op.box, op.tint, gs);
    case OpKind::PutImage: {
        const int dx = static_cast<int>(op.f[0]), dy = static_cast<int>(op.f[1]), cw = static_cast<int>(op.f[2]), ch = static_cast<int>(op.f[3]);
        G.put(bytes.data() + op.a, static_cast<int>(op.b), dx, dy, IBox{dx, dy, dx + cw, dy + ch});
        return true;
    }
    default: return false;
    }
}

// The flush, on the GPU: what it can draw is batched there; a run of draws
// it cannot is rasterized on a copy of the region they touch.
constexpr int kMaxGpuFallbackRuns = 4;
inline void Canvas::Soft::executeGpu() {
    using namespace canvas_detail;
    GpuSurface& G = *gpu;
    const std::size_t n = ops.size();
    // Every run of draws the rasterizer has to take costs a round trip of
    // pixels; a batch with many of them is cheaper drawn wholly in software.
    int runs = 0;
    bool inRun = false;
    long long cpuArea = 0;
    for (std::size_t k = 0; k < n; ++k) {
        if (ops[k].culled) continue;
        const bool cpu = !gpuCan(ops[k], styles[ops[k].style]);
        if (cpu && !inRun) ++runs;
        if (cpu) cpuArea += ops[k].box.intersect(canvasBox()).area();
        inRun = cpu;
    }
    if (runs > kMaxGpuFallbackRuns || cpuArea * 2 > canvasBox().area()) {
        syncCpu();
        executeOps();
        gpuValid = false;
        return;
    }
    if (!gpuValid) { G.upload(px, canvasBox()); gpuValid = true; }
    cpuValid = false;
    std::size_t i = 0;
    while (i < n) {
        if (ops[i].culled) { ++i; continue; }
        const Style& s0 = styles[ops[i].style];
        if (gpuCan(ops[i], s0) && gpuDo(ops[i], s0)) { ++i; continue; }
        // A run of draws for the rasterizer, and the region they may touch
        // (or read: a canvas drawn onto itself reads all of it).
        std::size_t j = i + 1;
        while (j < n && (ops[j].culled || !gpuCan(ops[j], styles[ops[j].style]))) ++j;
        IBox region{};
        bool whole = false;
        for (std::size_t k = i; k < j; ++k) {
            if (ops[k].culled) continue;
            const IBox& b = ops[k].box;
            if (ops[k].src == this) whole = true;
            region = region.empty() ? b : IBox{std::min(region.x0, b.x0), std::min(region.y0, b.y0), std::max(region.x1, b.x1), std::max(region.y1, b.y1)};
        }
        if (whole) region = canvasBox();
        region = region.intersect(canvasBox());
        G.draw();
        if (!region.empty()) {
            G.readback(px, region);
            for (std::size_t k = i; k < j; ++k) if (!ops[k].culled) execute(ops[k], styles[ops[k].style]);
            G.upload(px, region);
        }
        i = j;
    }
    G.draw();
}
#endif  // !__EMSCRIPTEN__
