#pragma once

// A Canvas 2D-shaped drawing library: one API, three backends.
//
//   native   a software rasterizer (exact-area antialiasing, a thread pool for
//            big jobs) with the whole HTML canvas drawing model: gradients,
//            patterns, every globalCompositeOperation, shadows, filters,
//            image smoothing, layers, text with kerning and fallback.
//   web      (emscripten) every Canvas is a real CanvasRenderingContext2D,
//            fed through a batched op stream.
//   skia     (emscripten, CPP_CANVAS_SKIA) Skia's GPU renderer in WebGL.
//
// Draw calls are DEFERRED on the native and web backends: they are recorded,
// and when a frame is flushed (Window::present(), flush(), any read-back) the
// recording is CULLED -- a call whose every pixel a later opaque call or clear
// overwrites is dropped before it costs anything -- and then executed. The
// result is identical to drawing every call; see flush() and setCulling().

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    constexpr Color() = default;
    constexpr Color(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 255)
        : r(red), g(green), b(blue), a(alpha) {}
};
inline bool operator==(Color x, Color y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; }
inline bool operator!=(Color x, Color y) { return !(x == y); }

// Parses a CSS colour (CSS Color 4: names, hex, rgb(), hsl(), hwb(), lab(),
// lch(), oklab(), oklch(), color()). False, leaving `out` alone, if invalid.
bool parseCssColor(const std::string& text, Color& out);

/// What a frame's op stream cost, for the client's own counters.
///
/// `ops` is how many drawing calls were recorded, `culled` how many of those
/// the flush dropped because later drawing covered them completely, and
/// `flushMillis` what executing the rest cost (the browser consuming the op
/// stream on the web; the rasterizer natively).
struct CanvasFrameStats {
    double flushMillis = 0;
    int ops = 0;
    int batches = 0;
    int culled = 0;
};

/// Drawing calls emitted since the frame's stats were last taken.
int canvasOpsEmitted();
/// How many op codes the counters below are indexed by.
inline constexpr int kCanvasOpCodes = 96;
/// The frame's drawing calls counted by kind, indexed by op code.
const int* canvasOpTypeCounts();
/// What op code `code` is, for a readout. Null for a code nothing emits.
const char* canvasOpName(int code);
/// Reads the stats accumulated since the last call, and zeroes them.
CanvasFrameStats canvasTakeFrameStats();

/// Makes `family` usable in setFont() on every canvas: a TrueType or CFF
/// (OpenType) font file's bytes. Natively it joins the system faces; in a
/// browser it becomes a FontFace on the page. `weight` 1..1000, `italic` as
/// the face is. False if the data is not a usable font.
bool canvasRegisterFont(const std::string& family, const std::vector<std::uint8_t>& data, int weight = 400,
                        bool italic = false);

#if defined(CPP_CANVAS_SKIA) && !defined(__EMSCRIPTEN__)
#error "the Skia backend is built for the browser only"
#endif

#if !defined(__EMSCRIPTEN__) && !defined(CPP_CANVAS_SKIA)
class Canvas;
/// For window.cpp: from now on `canvas` is drawn with the GPU of the
/// current OpenGL 3.3 context, whose functions `getProc` looks up (by their
/// "gl..." names). False, and nothing changes, if it cannot be.
bool canvasAttachGpu(Canvas& canvas, void* (*getProc)(const char*));
/// Flushes a GPU canvas and draws it over the current framebuffer.
void canvasShowGpu(Canvas& canvas, int width, int height);
bool canvasHasGpu(const Canvas& canvas);
#endif

#ifdef __EMSCRIPTEN__
/// Hands every canvas's batched drawing calls to the page (culling them
/// first). Window::present() calls it; anything that reads a canvas back
/// flushes that canvas for itself. Drawing left unflushed when control
/// returns to the browser is flushed automatically before the next paint.
void canvasFlushOps();

/// GPU context loss handling (see README): once a frame ask, and at
/// Recoverable drop every picture baked onto a Canvas and call
/// canvasRecoverContexts().
enum class CanvasContexts { Fine, Lost, LostTooLong, Recoverable };
CanvasContexts canvasCheckContexts();
void canvasRecoverContexts();

/// Images the PAGE fetches by URL, drawn without entering the wasm heap.
/// Status: 0 loading, 1 drawable (natural size in width/height), -1 failed.
int canvasRequestRemoteImage(const std::string& url);
int canvasRemoteImageStatus(int handle, int& width, int& height);
void canvasReleaseRemoteImage(int handle);
#endif

#ifdef CPP_CANVAS_SKIA
/// The face Skia sets text in (see canvas_skia.cpp).
bool canvasRegisterFontFile(const std::string& path);
#endif

// Retained path object, equivalent to the browser's Path2D.
class Path2D {
public:
    enum class Command : unsigned char { Move, Line, Quadratic, Bezier, Arc, ArcTo, Ellipse, Rect, RoundRect, Close };
    struct Segment { Command command; float v[8]{}; bool counterClockwise = false; };
    void moveTo(float x, float y); void lineTo(float x, float y); void closePath();
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    // Negative radii are ignored, as the browser throws on them.
    void arc(float x, float y, float radius, float startAngle, float endAngle, bool counterClockwise = false);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void ellipse(float x, float y, float radiusX, float radiusY, float rotation, float startAngle, float endAngle, bool counterClockwise = false);
    void rect(float x, float y, float width, float height); void roundRect(float x, float y, float width, float height, float radius);
    // Per-corner radii, CSS order: 1 value (all), 2 (top-left & bottom-right,
    // top-right & bottom-left), 3 (top-left, top-right & bottom-left,
    // bottom-right) or 4 (top-left, top-right, bottom-right, bottom-left).
    // Each corner is {rx, ry}; a circular corner has rx == ry. Radii that do
    // not fit are scaled down together, as the canvas spec says.
    struct CornerRadius { float x = 0, y = 0; CornerRadius() = default; CornerRadius(float r) : x(r), y(r) {} CornerRadius(float rx, float ry) : x(rx), y(ry) {} };
    void roundRect(float x, float y, float width, float height, const std::vector<CornerRadius>& radii);
    void addPath(const Path2D& other);
    // With a transform [a b c d e f] applied to `other` first.
    void addPath(const Path2D& other, const std::array<float, 6>& transform);
    void clear() { segments_.clear(); glyphOutlines_ = false; touch(); }
    bool empty() const { return segments_.empty(); }
    const std::vector<Segment>& segments() const { return segments_; }
    // Mutable access moves the revision whether or not the caller writes.
    std::vector<Segment>& segments() { touch(); return segments_; }

    // Identity for the browser build's retained-path cache: unique for the
    // life of the process, never reused.
    std::uint32_t cacheKey() const;
    std::uint32_t revision() const { return revision_; }

    Path2D() = default;
    ~Path2D() = default;
    // An SVG path `d` string, as `new Path2D(d)` takes it (defined in svg.cpp).
    explicit Path2D(const std::string& svgPathData);
    Path2D(const Path2D& other) : segments_(other.segments_), glyphOutlines_(other.glyphOutlines_) {}
    Path2D(Path2D&& other) noexcept
        : segments_(std::move(other.segments_)), glyphOutlines_(other.glyphOutlines_) { other.touch(); }
    Path2D& operator=(const Path2D& other) {
        if (this != &other) { segments_ = other.segments_; glyphOutlines_ = other.glyphOutlines_; touch(); }
        return *this;
    }
    Path2D& operator=(Path2D&& other) noexcept {
        if (this != &other) {
            segments_ = std::move(other.segments_); glyphOutlines_ = other.glyphOutlines_;
            touch(); other.touch();
        }
        return *this;
    }
    // True once Font::appendText has put glyph outlines in here: a filled
    // glyph's coverage goes up the browser's text gamma ramp, a shape's not.
    bool glyphOutlines() const { return glyphOutlines_; }
    void markGlyphOutlines() { glyphOutlines_ = true; }
private:
    void touch() { ++revision_; }
    std::vector<Segment> segments_;
    mutable std::uint32_t key_ = 0;
    std::uint32_t revision_ = 1;
    bool glyphOutlines_ = false;
};

// createLinearGradient / createRadialGradient / createConicGradient. Shared,
// like the JavaScript object: a stop added after setFillStyle() reaches every
// LATER draw that uses it. Copies are handles to the same gradient.
class CanvasGradient {
public:
    CanvasGradient() = default;
    // Offsets outside [0, 1] are ignored (the browser throws).
    void addColorStop(float offset, Color color);
    // False if the offset is out of range or the colour does not parse.
    bool addColorStop(float offset, const std::string& cssColor);
    bool valid() const { return data_ != nullptr; }
    struct Data;
    const std::shared_ptr<Data>& data() const { return data_; }
private:
    friend class Canvas;
    std::shared_ptr<Data> data_;
};

// createPattern: an image repeated (or not) under its own transform. The
// pixels are a copy taken when the pattern is made, as the browser's are.
class CanvasPattern {
public:
    CanvasPattern() = default;
    void setTransform(float a, float b, float c, float d, float e, float f);
    bool valid() const { return data_ != nullptr; }
    struct Data;
    const std::shared_ptr<Data>& data() const { return data_; }
private:
    friend class Canvas;
    std::shared_ptr<Data> data_;
};

// measureText's TextMetrics, in CSS pixels at the current font.
struct TextMetrics {
    float width = 0;
    float actualBoundingBoxLeft = 0, actualBoundingBoxRight = 0;
    float actualBoundingBoxAscent = 0, actualBoundingBoxDescent = 0;
    float fontBoundingBoxAscent = 0, fontBoundingBoxDescent = 0;
    float emHeightAscent = 0, emHeightDescent = 0;
    float hangingBaseline = 0, alphabeticBaseline = 0, ideographicBaseline = 0;
};

#if defined(__EMSCRIPTEN__) && !defined(CPP_CANVAS_SKIA)
// The browser build's way with the transform (see canvas.cpp): a call drawn
// at matrix M is issued under the page's matrix P with its coordinates mapped
// through R = P^-1 * M whenever the call survives R exactly.
struct CanvasPageFrame {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    double scale = 1, rotation = 0;
    int sigma = 1;
    unsigned fits = ~0u;
    bool same() const { return fits == ~0u; }
};
#endif

// CanvasRenderingContext2D-style API.
class Canvas {
public:
    // `desynchronized` asks the browser for a low-latency context on the
    // element (first context only). Ignored natively.
    Canvas(int width, int height, std::string elementId = "canvas", bool desynchronized = false);
    // A string literal used to pick the wrong (private) overload.
    Canvas(int width, int height, const char* elementId, bool desynchronized = false)
        : Canvas(width, height, std::string(elementId ? elementId : "canvas"), desynchronized) {}
    static Canvas createVirtual(int width, int height);
    Canvas(const Canvas&) = delete; Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&& other) noexcept; Canvas& operator=(Canvas&& other) noexcept;
    ~Canvas();
    // Size in USER units (see setLogicalSize); pixelWidth/Height are the
    // backing store's real size.
    int width() const { return logicalWidth_; } int height() const { return logicalHeight_; }
    int pixelWidth() const { return width_; } int pixelHeight() const { return height_; }
    void setLogicalSize(int width, int height);
    bool isVirtual() const { return virtual_; }
    void present(const std::string& elementId);

    void save(); void restore(); void reset();
    void scale(float x, float y); void rotate(float radians); void translate(float x, float y);
    void transform(float a, float b, float c, float d, float e, float f);
    void setTransform(float a = 1, float b = 0, float c = 0, float d = 1, float e = 0, float f = 0); void resetTransform();
    void setTransform(const std::array<float, 6>& m) { setTransform(m[0], m[1], m[2], m[3], m[4], m[5]); }
    std::array<float, 6> getTransform() const { return currentTransform(); }
    void setFillStyle(Color color); void setStrokeStyle(Color color);
    // Any CSS colour; an invalid string leaves the style as it was.
    void setFillStyle(const std::string& cssColor); void setStrokeStyle(const std::string& cssColor);
    void setFillStyle(const CanvasGradient& gradient); void setStrokeStyle(const CanvasGradient& gradient);
    void setFillStyle(const CanvasPattern& pattern); void setStrokeStyle(const CanvasPattern& pattern);
    CanvasGradient createLinearGradient(float x0, float y0, float x1, float y1);
    CanvasGradient createRadialGradient(float x0, float y0, float r0, float x1, float y1, float r1);
    CanvasGradient createConicGradient(float startAngle, float x, float y);
    // repetition: "repeat" (or ""), "repeat-x", "repeat-y", "no-repeat".
    CanvasPattern createPattern(const Canvas& source, const std::string& repetition = "repeat");
    CanvasPattern createPattern(const std::uint8_t* rgba, int width, int height, const std::string& repetition = "repeat");
    // A colour every LATER setFillStyle/setStrokeStyle(Color) is washed with:
    // c * (1 - tint.a) + tint.c, tint premultiplied. Alpha zero turns it off.
    void setColorTint(Color premultiplied) { tint_ = premultiplied; }
    Color colorTint() const { return tint_; }
    void setGlobalAlpha(float alpha);
    // Every canvas operation: source-over ... xor, multiply ... luminosity.
    void setGlobalCompositeOperation(const std::string& operation);
    // CSS filter functions: blur(), brightness(), contrast(), drop-shadow(),
    // grayscale(), hue-rotate(), invert(), opacity(), saturate(), sepia(); "none".
    void setFilter(const std::string& filter);
    void setLineWidth(float width); void setLineCap(const std::string& cap); void setLineJoin(const std::string& join); void setMiterLimit(float limit);
    void setLineDash(const std::vector<float>& segments); void setLineDashOffset(float offset);
    std::vector<float> getLineDash() const;
    void setShadow(Color color, float blur, float offsetX = 0, float offsetY = 0);
    void setShadow(const std::string& cssColor, float blur, float offsetX = 0, float offsetY = 0);
    // The CSS font shorthand: "italic bold 16px Helvetica, sans-serif".
    void setFont(const std::string& font); void setTextAlign(const std::string& align); void setTextBaseline(const std::string& baseline); void setDirection(const std::string& direction);
    // CSS lengths ("2px", "0.1em"), and "auto"/"normal"/"none" for kerning.
    void setLetterSpacing(const std::string& spacing); void setWordSpacing(const std::string& spacing);
    void setFontKerning(const std::string& kerning);
    void setImageSmoothingEnabled(bool enabled); void setImageSmoothingQuality(const std::string& quality);
    // The current transform, [a b c d e f], user space to device pixels.
    std::array<float, 6> currentTransform() const;
    // True when what is drawn next is composited plainly: source-over, no
    // filter, no visible shadow.
    bool isPlainComposite() const;
    float globalAlpha() const;
#ifndef __EMSCRIPTEN__
    // Device-pixel fast paths for presenters and whole-screen effects: no
    // transform, the clip and globalAlpha honoured. Each is recorded like
    // any draw, with its own copy of the rows that land, so the caller's
    // buffer can be reused at once.
    void blitDevice(const std::uint8_t* rgba, int imageWidth, int imageHeight, int deviceX, int deviceY);
    void darkenDevice(const std::uint8_t* mask, int maskWidth, int maskHeight, int deviceX, int deviceY);
    void multiplyDevice(const std::uint8_t* mask, int maskWidth, int maskHeight, int deviceX, int deviceY, Color floor);
    // The pixels, straight-alpha RGBA, pixelWidth() * pixelHeight() * 4 bytes,
    // after executing everything recorded. Valid until the next draw call.
    const std::uint8_t* pixelData();
#endif

    // Natively an exact replace of every pixel; in a browser the same (a
    // clear, then an opaque fill).
    void clear(Color color = Color{255, 255, 255}); void clearRect(float x, float y, float width, float height);
    void fillRect(float x, float y, float width, float height); void strokeRect(float x, float y, float width, float height);
    void beginPath(); void closePath(); void moveTo(float x, float y); void lineTo(float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y); void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void arc(float x, float y, float radius, float startAngle, float endAngle, bool counterClockwise = false);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void ellipse(float x, float y, float radiusX, float radiusY, float rotation, float startAngle, float endAngle, bool counterClockwise = false);
    void rect(float x, float y, float width, float height); void roundRect(float x, float y, float width, float height, float radius);
    void roundRect(float x, float y, float width, float height, const std::vector<Path2D::CornerRadius>& radii);
    void fill(); void fill(const std::string& rule); void stroke(); void clip(const std::string& rule = "nonzero");
    void fill(const Path2D& path, const std::string& rule = "nonzero"); void stroke(const Path2D& path); void clip(const Path2D& path, const std::string& rule = "nonzero");
    /// Turns what is painted under the current path to grey: the `saturation`
    /// composite with a grey source.
    void desaturate(const std::string& rule = "nonzero");
    bool isPointInPath(float x, float y, const std::string& rule = "nonzero") const; bool isPointInStroke(float x, float y) const;
    bool isPointInPath(const Path2D& path, float x, float y, const std::string& rule = "nonzero") const;
    bool isPointInStroke(const Path2D& path, float x, float y) const;

    void fillText(const std::string& text, float x, float y, float maxWidth = -1); void strokeText(const std::string& text, float x, float y, float maxWidth = -1);
    float measureText(const std::string& text) const;
    TextMetrics measureTextMetrics(const std::string& text) const;
    // Natively a canvas drawn scaled takes the nearest texel under each
    // pixel, whatever imageSmoothingEnabled says (florr.io_clone's sprite
    // path); the browser smooths it as the context says. drawImage of pixel
    // data filters on both.
    void drawCanvas(const Canvas& source, float dx, float dy); void drawCanvas(const Canvas& source, float dx, float dy, float dw, float dh);
    // The nine-argument drawImage form.
    void drawCanvas(const Canvas& source, float sx, float sy, float sw, float sh,
                    float dx, float dy, float dw, float dh);
#ifndef __EMSCRIPTEN__
    // A one-to-one blit with every source colour multiplied by `tint`.
    void drawCanvasTinted(const Canvas& source, float sx, float sy, float sw, float sh,
                          float dx, float dy, Color tint);
#endif
    // Tightly packed straight-alpha RGBA into the user-space box, through the
    // transform, clip, filter, compositing and smoothing state. `alpha`
    // multiplies the source. `cacheKey` names these exact pixels for the life
    // of the process (0 = no name): the backend may keep a copy under it.
    void drawImage(const std::uint8_t* rgba, int imageWidth, int imageHeight,
                   float dx, float dy, float dw, float dh, float alpha = 1.0f,
                   std::uint32_t cacheKey = 0);
    void drawImage(const std::uint8_t* rgba, int imageWidth, int imageHeight,
                   float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
                   float alpha = 1.0f, std::uint32_t cacheKey = 0);
    // One level of a prefiltered image pyramid: level 0 is the image, each
    // later one the previous halved. Natively the closest level is drawn.
    struct ImageLevel { const std::uint8_t* rgba; int width; int height; };
    void drawImage(const ImageLevel* levels, int levelCount, float dx, float dy, float dw,
                   float dh, float alpha = 1.0f, std::uint32_t cacheKey = 0);
#ifdef __EMSCRIPTEN__
    void drawRemoteImage(int handle, float dx, float dy, float dw, float dh);
#endif
    std::vector<std::uint8_t> getImageData(int x, int y, int width, int height) const;
    // Replaces the pixels (no blending, no clip, no transform), as the browser does.
    void putImageData(const std::vector<std::uint8_t>& rgba, int sourceWidth, int sourceHeight, int dx, int dy);
    void putImageData(const std::vector<std::uint8_t>& rgba, int sourceWidth, int sourceHeight, int dx, int dy,
                      int dirtyX, int dirtyY, int dirtyWidth, int dirtyHeight);
    void fillCircle(float centerX, float centerY, float radius); void strokeCircle(float centerX, float centerY, float radius);

    // Canvas 2D layers: everything drawn until the matching endLayer() is
    // drawn into a transparent layer, which endLayer() then composites with
    // the globalAlpha, composite operation, filter and shadow that were set
    // when beginLayer() was called. Acts as save()/restore() too.
    void beginLayer(); void endLayer();

    // Executes (natively) or hands to the page (web) everything recorded for
    // this canvas, culling first. Called for you by Window::present(),
    // getImageData(), savePPM(), and by a canvas drawn from this one.
    void flush();
    // Culling is on by default; off, draw calls still record and flush the
    // same way but nothing is dropped. For measuring and debugging.
    void setCulling(bool enabled);
    bool culling() const { return cullingEnabled_; }

    bool savePPM(const std::string& path) const;
    // Native-only interactive presentation. Blocks until the user closes the window.
    bool showWindow(const std::string& title = "C++ Canvas",
                    const std::function<void(Canvas&, float)>& drawFrame = {});

    // Implementation detail of the native backend, public only so its
    // internals can be defined out of line.
    struct Soft;
private:
    Canvas(int width, int height, bool isVirtual);
    int width_, height_, contextId_ = -1; bool virtual_ = false; std::string elementId_;
    int logicalWidth_ = 0, logicalHeight_ = 0;
    Color fill_{0, 0, 0}, stroke_{0, 0, 0}; float lineWidth_ = 1.0f; Path2D currentPath_;
    Color tint_{0, 0, 0, 0};
    bool cullingEnabled_ = true;
    Color tinted(Color c) const;
#ifdef CPP_CANVAS_SKIA
    struct Skia;
    std::unique_ptr<Skia> skia_;
#elif defined(__EMSCRIPTEN__)
public:
    // Implementation types of the browser backend (detail/web.h).
    // The drawing state, kept in wasm rather than in the browser context.
    // Paint styles that are objects (gradients, patterns) are named by a
    // reference string beginning with '\x01'.
    // A string kept once for the whole program (state names such as
    // "round" or "source-over"): copying one copies a pointer, and two are
    // equal when they point at the same one.
    class WebString {
    public:
        WebString() : p_(intern(std::string())) {}
        WebString(const char* s) : p_(intern(s)) {}
        WebString(const std::string& s) : p_(intern(s)) {}
        WebString& operator=(const std::string& s) { if (*p_ != s) p_ = intern(s); return *this; }
        WebString& operator=(const char* s) { if (*p_ != s) p_ = intern(s); return *this; }
        operator const std::string&() const { return *p_; }
        const std::string& str() const { return *p_; }
        const char* c_str() const { return p_->c_str(); }
        std::size_t size() const { return p_->size(); }
        bool empty() const { return p_->empty(); }
        char operator[](std::size_t i) const { return (*p_)[i]; }
        int compare(std::size_t pos, std::size_t n, const char* t) const { return p_->compare(pos, n, t); }
        operator std::string_view() const { return *p_; }
        friend bool operator==(const WebString& a, const WebString& b) { return a.p_ == b.p_; }
        friend bool operator!=(const WebString& a, const WebString& b) { return a.p_ != b.p_; }
        friend bool operator==(const WebString& a, const std::string& b) { return *a.p_ == b; }
        friend bool operator!=(const WebString& a, const std::string& b) { return *a.p_ != b; }
        friend bool operator==(const std::string& b, const WebString& a) { return *a.p_ == b; }
        friend bool operator!=(const std::string& b, const WebString& a) { return *a.p_ != b; }
        friend bool operator==(const WebString& a, const char* b) { return *a.p_ == b; }
        friend bool operator!=(const WebString& a, const char* b) { return *a.p_ != b; }
    private:
        static const std::string* intern(const std::string& s);
        const std::string* p_;
    };
    struct WebState {
        // fill / stroke as CSS; empty while set as a colour (fillRgba) and not
        // yet needed as text.
        mutable std::string fill = "#000000", stroke = "#000000";
        std::string font = "10px sans-serif";
        WebString lineCap = "butt", lineJoin = "miter";
        WebString textAlign = "start", textBaseline = "alphabetic";
        WebString composite = "source-over", filter = "none", direction = "inherit";
        WebString smoothingQuality = "low", shadowColour = "#00000000";
        WebString letterSpacing = "0px", wordSpacing = "0px", fontKerning = "auto";
        std::vector<float> dash;
        float lineWidth = 1, alpha = 1, miterLimit = 10, dashOffset = 0;
        float shadowBlur = 0, shadowOffsetX = 0, shadowOffsetY = 0;
        bool smoothing = true;
        std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};
        // The clip, for culling: its device bounding box, whether it is
        // exactly that box (only axis-aligned rect clips), and whether any
        // clip is set at all.
        float clipX0 = -1e30f, clipY0 = -1e30f, clipX1 = 1e30f, clipY1 = 1e30f;
        bool clipped = false, clipExact = true;
        // Objects the fill/stroke reference strings name, kept alive.
        std::shared_ptr<void> fillObject, strokeObject;
        // fill / stroke as a colour, when set as one (no parsing to draw it).
        // (Also filled in when a CSS colour is first parsed to draw with.)
        mutable Color fillRgba{0, 0, 0, 255}, strokeRgba{0, 0, 0, 255};
        mutable bool fillRgbaOk = false, strokeRgbaOk = false;
        // Member by member (restore()): cheaper than std::swap's three moves.
        // Every member is listed; a new one must be added here.
        void swapWith(WebState& o) {
            fill.swap(o.fill); stroke.swap(o.stroke); font.swap(o.font);
            std::swap(lineCap, o.lineCap); std::swap(lineJoin, o.lineJoin);
            std::swap(textAlign, o.textAlign); std::swap(textBaseline, o.textBaseline);
            std::swap(composite, o.composite); std::swap(filter, o.filter); std::swap(direction, o.direction);
            std::swap(smoothingQuality, o.smoothingQuality); std::swap(shadowColour, o.shadowColour);
            std::swap(letterSpacing, o.letterSpacing); std::swap(wordSpacing, o.wordSpacing); std::swap(fontKerning, o.fontKerning);
            dash.swap(o.dash);
            std::swap(lineWidth, o.lineWidth); std::swap(alpha, o.alpha); std::swap(miterLimit, o.miterLimit); std::swap(dashOffset, o.dashOffset);
            std::swap(shadowBlur, o.shadowBlur); std::swap(shadowOffsetX, o.shadowOffsetX); std::swap(shadowOffsetY, o.shadowOffsetY);
            std::swap(smoothing, o.smoothing);
            std::swap(matrix, o.matrix);
            std::swap(clipX0, o.clipX0); std::swap(clipY0, o.clipY0); std::swap(clipX1, o.clipX1); std::swap(clipY1, o.clipY1);
            std::swap(clipped, o.clipped); std::swap(clipExact, o.clipExact);
            fillObject.swap(o.fillObject); strokeObject.swap(o.strokeObject);
            std::swap(fillRgba, o.fillRgba); std::swap(strokeRgba, o.strokeRgba);
            std::swap(fillRgbaOk, o.fillRgbaOk); std::swap(strokeRgbaOk, o.strokeRgbaOk);
        }
    };
#if defined(__EMSCRIPTEN__) && !defined(__wasm64__)
    static_assert(sizeof(WebState) == 224, "WebState changed: update swapWith");
#endif
    struct WebFrame { WebState state; bool real = false; };
    // The save() stack. Frames stay allocated when popped, so a save copies
    // into one already there (its strings keep their storage).
    struct FrameStack {
        std::vector<WebFrame> slots;
        std::size_t n = 0;
        FrameStack() = default;
        FrameStack(FrameStack&& o) noexcept : slots(std::move(o.slots)), n(o.n) { o.n = 0; }
        FrameStack& operator=(FrameStack&& o) noexcept { slots = std::move(o.slots); n = o.n; o.n = 0; return *this; }
        bool empty() const { return n == 0; }
        std::size_t size() const { return n; }
        WebFrame& back() { return slots[n - 1]; }
        void push(const WebState& s) {
            if (n < slots.size()) { slots[n].state = s; slots[n].real = false; }
            else slots.push_back(WebFrame{s, false});
            ++n;
        }
        void pop_back() { --n; slots[n].state.fillObject.reset(); slots[n].state.strokeObject.reset(); }
        void clear() { slots.clear(); n = 0; }
        WebFrame* begin() { return slots.data(); }
        WebFrame* end() { return slots.data() + n; }
    };
    struct WebOps;
private:
    WebState web_;
    FrameStack webStack_;
    mutable WebState applied_;
    std::vector<WebState> appliedStack_;
    mutable unsigned dirty_ = 0;
    void sync(unsigned fields) const;
    void realSave();
    void forgetPageState();
    // A GPU canvas (detail/webgpu.h): what its own code records for the
    // shadow surface, around a draw the GPU does not do.
    void gpuGenericBegin();
    void gpuSyncPath() const;
    void gpuGenericEnd();
    void drawCanvasRecorded(const Canvas& source, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh);
    friend struct GpuGenericScope;
    friend void canvasRecoverContexts();
    friend void canvasFlushOps();
    using PageFrame = CanvasPageFrame;
    const PageFrame& frameFor(const std::array<double, 6>& m, unsigned need) const;
    void movePageTo(const std::array<double, 6>& m) const;
    void emitSegmentAt(const std::array<double, 6>& m, const Path2D::Segment& g, bool loose = false) const;
    void emitPathAt(const Path2D& p, int action, bool evenOdd) const;
    mutable std::array<double, 6> framePage_{}, frameCall_{};
    mutable PageFrame frame_;
    mutable bool frameKnown_ = false;
    enum class PathMode : unsigned char { Empty, Deferred, Sent };
    mutable PathMode pathMode_ = PathMode::Empty;
    mutable bool pagePathDirty_ = false;
    mutable int pagePathSegments_ = 0;
    bool pathHasSubpath_ = false;
    std::array<double, 6> pathMatrix_{1, 0, 0, 1, 0, 0};
    void pathSegmentAdded();
    void sendHeldPath(std::size_t count, bool fillOnly = false) const;
    void preparePagePath(bool fillOnly = false) const;
    mutable bool pageSeamMoved_ = false;
    bool pathUniform_ = true;
    bool paintCachedPath(int action, bool evenOdd);
    // What this canvas keeps between batches of the shared op buffer.
    std::unique_ptr<WebOps> ops_;
    int layerDepth_ = 0;
    // Culling: what the next paint op covers and whether it is an occluder.
    void notePaint(int kind, const float* box, bool occluder, const float* inner) const;
    void noteUnbounded() const;
    // Canvases whose recorded ops read this one, and that this one's read.
    mutable std::vector<const Canvas*> readers_;
    mutable std::vector<const Canvas*> sources_;
    void beforeWrite() const;
    void dependOn(const Canvas& source) const;
    void dropDependencies() const;
    std::string styleRef(const std::shared_ptr<void>& object, int kind, int id) const;
#endif
#if !defined(__EMSCRIPTEN__)
    std::unique_ptr<Soft> soft_;
    friend bool canvasAttachGpu(Canvas&, void* (*)(const char*));
    friend void canvasShowGpu(Canvas&, int, int);
    friend bool canvasHasGpu(const Canvas&);
#endif
};
