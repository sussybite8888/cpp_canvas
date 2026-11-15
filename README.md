# C++ Canvas

A drawing library shaped like the browser's Canvas 2D (`CanvasRenderingContext2D`
and `Path2D`), with one API and three backends:

| Backend | Build | What draws |
|---|---|---|
| **Native** | any C++17 compiler + SDL2 | A software rasterizer with exact-area antialiasing that draws a frame on every core |
| **Web** | emscripten | The page's own `CanvasRenderingContext2D`, fed through a batched op stream |
| **Skia** | emscripten + `-DCPP_CANVAS_SKIA=ON` | Skia's GPU renderer (Ganesh) in WebGL, from a prebuilt static Skia |

The native backend implements the whole HTML canvas drawing model, so the same
program looks the same natively and in a browser: CSS colours, gradients
(linear, radial, conic), patterns, all 26 `globalCompositeOperation` values,
shadows, CSS filters, image smoothing, layers, and kerned text with font
fallback.

## Building

```sh
cmake -S . -B build && cmake --build build
./build/canvas_demo            # writes canvas-demo.ppm
./build/canvas_demo --window   # an interactive SDL window
./build/canvas_tests           # native tests
./build/canvas_bench           # rasterizer benchmark (--nocull to disable culling)
```

SDL2 is the native window and input layer (`brew install sdl2`,
`apt install libsdl2-dev`, or vcpkg's `sdl2`).

Browser build:

```sh
emcmake cmake -S . -B build-web && cmake --build build-web
python3 -m http.server -d build-web 8080   # then open /canvas_demo.html
```

Skia build: `emcmake cmake -S . -B build-skia -DCPP_CANVAS_SKIA=ON
-DCPP_CANVAS_SKIA_DIR=<rust-skia wasm32-unknown-emscripten 0.153.3 binaries>`.
The configure step downloads the matching Skia headers once.

The library is the `cpp_canvas` CMake target. Its sources are `canvas.cpp`
(or `canvas_skia.cpp`), `font.cpp`, `svg.cpp`, `window.cpp` and
`touch_gesture.cpp`. Everything under `detail/` is private and is included by
those files, so a project that compiles the library from its sources
directly does not need to list it.

## Deferred drawing and culling

On the native and web backends, draw calls are **recorded**, not executed when
they are made. When a canvas is flushed, its recording is **culled** and then
executed. A canvas is flushed by `Window::present()`, `flush()`,
`getImageData()`, `savePPM()`, or by a canvas that draws from it. In the
browser, anything still buffered is also flushed automatically before the next
paint.

In the browser, every canvas records into one shared buffer, as florr.io_clone's
backend did. It holds one canvas's calls at a time: when another canvas starts
drawing, the one holding the buffer is flushed first. A canvas therefore costs
no buffer memory of its own, however many small canvases an app bakes.

Culling walks the recording backwards and keeps a one-bit-per-pixel map of
what later draws are certain to overwrite. A draw whose whole bounding box is
already covered is dropped before it costs anything. Its state changes still
apply, and so does the path it built if a later draw uses that path. These
draws count as overwriting their pixels:

- `clear()`, `reset()` and `putImageData()` replace pixels whatever was there.
- `clearRect()` clears its rectangle.
- An opaque fill of a rectangle, a rounded rectangle or a full circle counts
  for its interior. It must be drawn with `source-over`, global alpha 1, no
  filter and an axis-aligned transform. The paint can be a solid colour, an
  opaque gradient or an opaque repeating pattern.
- An opaque image counts for its destination rectangle.

Every draw's box includes its shadow, its filter's spread, and its stroke's
reach, and is cut to the clip. Composite operations that reach outside the
shape, such as `copy` or `source-in`, cover the whole clip. **Culling never
changes a pixel**: the tests render every conformance scene with culling on
and off and require identical output on both backends.

```cpp
canvas.setCulling(false);                        // per canvas, e.g. for profiling
CanvasFrameStats s = canvasTakeFrameStats();     // ops recorded, culled, flush time
```

On the native backend, the surviving draws are executed **in parallel**. The
canvas is cut into horizontal bands, and each core takes a band at a time and
runs every draw that reaches it, in order. A draw's rows do not depend on one
another, so this gives exactly the pixels that running the draws one after
another would. A frame of hundreds of small draws keeps every core busy
without handing work out once per draw. Some draws run on their own, between
the banded runs, with their rows split across the cores instead:

- draws with a shadow, a filter or a composite operation other than
  `source-over`, which are rendered into a layer first;
- draws that read the canvas they are drawn on;
- text drawn from outlines (rotated, skewed, stroked, or larger than 192 px);
- long paths that cross many bands, which are cheaper to flatten and stroke
  once than once per band.

The tests compare a frame flushed at once with the same frame flushed after
every draw.

A draw that reads another canvas, such as `drawCanvas()` or
`createPattern()`, sees that canvas as it was when the call was made. The
library tracks this dependency and flushes in the right order. A source canvas
with nothing pending does not flush the canvases drawing from it, so drawing
one sprite canvas hundreds of times stays a single batch. `drawImage()` with
raw pixels copies them, or keeps one copy per `cacheKey`, so the caller's
buffer can be reused straight away.

## API beyond the original

- **Styles**
  - `setFillStyle` and `setStrokeStyle` accept any CSS Color 4 string, a
    `CanvasGradient` from `createLinearGradient`, `createRadialGradient` or
    `createConicGradient`, or a `CanvasPattern` from `createPattern(canvas |
    rgba, repetition)`.
  - Shadows: `setShadow(color, blur, dx, dy)`.
  - Filters: `setFilter("blur(2px) drop-shadow(...) grayscale(1)")`.
  - Composite operations: `setGlobalCompositeOperation(...)` takes all 26
    values.
- **Paths**
  - `roundRect(x, y, w, h, {radii...})` takes per-corner, elliptical radii.
  - `Path2D("M0 0 L10 10 Z")` parses SVG path data.
  - `addPath(path, transform)` appends a transformed copy.
  - `arcTo` now draws the true tangent arc.
  - Hit testing: `isPointInPath(path, …)` and `isPointInStroke(…)`.
- **Text**
  - `setFont` takes the full CSS font shorthand: style, weight, stretch, size
    in any unit, and a family list. Faces are TrueType or CFF.
  - Text is kerned the way Chrome kerns it. Characters a face lacks fall back
    to the next family, then to system faces.
  - `measureTextMetrics()` returns a full `TextMetrics`.
  - `setLetterSpacing()`, `setWordSpacing()` and `setFontKerning()`.
  - `strokeText` draws outlines.
  - `canvasRegisterFont()` makes a font file usable on every backend.
- **Images**
  - `drawImage` works under any transform, with bilinear smoothing ("low"),
    area-averaged minification ("medium" or "high"), or nearest-neighbour
    sampling. With a source rectangle, filtering may read neighbouring pixels
    of the source image, as the spec says.
  - `drawCanvas` works under any transform too. Natively, a canvas drawn
    scaled takes the nearest texel under each pixel, as florr.io_clone's copy
    does, whatever `imageSmoothingEnabled` says. In the browser it is smoothed
    as the context says.
  - `drawImage` also has a nine-argument (source-rectangle) form.
  - Image pyramids are supported.
  - `putImageData` takes a dirty rectangle and replaces pixels.
- **Layers:** `beginLayer()` and `endLayer()` draw a group that is
  composited once with the alpha, composite operation, filter and shadow that
  were current when the layer began.
- **Other:** `getTransform()`, `setTransform(matrix)`, `getLineDash()`,
  `parseCssColor()`, and `pixelData()` (native: the pixels without a copy).
- **Window:** a window with HiDPI support, a design-size coordinate space,
  touch with list-scroll gestures, the on-screen keyboard (web), clipboard,
  cursor shapes, and an input guard. The same class works natively (SDL) and
  in the browser.
- **Web:** GPU-context loss recovery (`canvasCheckContexts()`) and remote
  images (`canvasRequestRemoteImage()`).

## Behaviour changes from the previous version

These changes bring the native backend in line with the HTML spec and Chrome:

- `putImageData` replaces pixels; it used to blend them.
- `reset()` clears the pixels and the path.
- `clearRect` is not antialiased (edges round half up), as in Chrome.
- Setters ignore invalid values instead of clamping them. This covers
  `globalAlpha` outside [0, 1], a line width ≤ 0, a negative or non-finite
  dash, and a bad colour, font or filter string.
- Text baselines snap to whole device pixels, and text is kerned (except
  across spaces, as Chrome does).
- `lineTo` after `closePath()` starts at the closed subpath's start. A lone
  `moveTo` is not stroked.
- The web backend's `clear(Color)` replaces pixels, as the native one always
  did.

## Accuracy and speed

`tools/conformance/run.sh` draws 23 scenes twice. One copy is drawn by the
native backend; the other is built for the web and drawn by headless
Chrome's own canvas. The script then compares each pair and checks that
culling on and off give identical pixels on both backends.

Measured with that script:

- **Shapes, strokes, images and effects:** within 1/255 mean difference of
  Chrome in every scene. Colour filters and composite operations agree to
  within 1–2 levels.
- **Text:** within 4/255. The glyph outlines and layout are the same; text
  widths match Chrome to 1/64 px. The remaining difference is the
  antialiasing of glyph edges. Glyph coverage follows a ramp that depends on
  the text's luminance, calibrated so that the ink of dark text on light and
  light text on dark is within a few percent of Chrome's.

Antialiasing is exact area coverage. Measured against 64×64 supersampled
ground truth, the error is within 0.006. Rows dense enough to be cheaper to
sample use 8 samples per pixel, and the error there stays under 0.05. The
previous scan converter's error was up to 0.27.

`canvas_bench` covers 12 scenes at 1280×720. The times below are native, in
ms, measured on an Apple M2 (4 performance and 4 efficiency cores). The four
configurations were run interleaved, best of three each:

| scene | previous version | florr.io_clone's copy | this, culling off | this, culling on |
|---|---|---|---|---|
| full-screen opaque rects ×20 | 93.3 | 4.96 | 0.29 | 0.07 |
| 1000 small rects | 6.42 | 3.54 | 0.38 | 0.42 |
| 500 circles | 14.0 | 7.84 | 1.53 | 1.52 |
| 300 stroked circles | 30.3 | 18.6 | 1.90 | 1.90 |
| 2000-point jagged polyline | 10.7 | 2.01 | 1.40 | 1.39 |
| 200 Bézier shapes, filled and stroked | 25.3 | 19.7 | 3.49 | 3.49 |
| 40 lines of text | 12.8 | 4.94 | 0.41 | 0.42 |
| 200 sprites at 1:1 | 15.5 | 4.55 | 0.53 | 0.52 |
| 200 scaled sprites | 5.94 | 1.69 | 1.10 | 1.10 |
| clipped full-screen fills | 65.7 | 8.40 | 0.77 | 0.33 |
| translucent full-screen fills | 46.7 | 7.90 | 1.81 | 1.81 |
| game frame under an opaque menu | 33.1 | 9.68 | 1.26 | 0.15 |
| **total** | **360** | **93.8** | **14.8** | **13.1** |

Every scene is faster than in florr.io_clone's copy, with culling on or off.
Scaled sprites are sampled the way florr's copy samples them, one texel per
pixel. Rows of the jagged polyline that are too busy for exact coverage take 8
samples per pixel here, where florr's copy takes 5.

Where the time went:

- A flush runs its draws in parallel bands (see
  [Deferred drawing and culling](#deferred-drawing-and-culling)). A single
  large draw has its rows split across the cores.
- Rows are emitted as constant-coverage runs, so a shape's interior costs one
  call per row.
- Rows too busy for exact coverage keep their edges in order from one sample
  line to the next instead of sorting every line.
- Culling checks a draw against the largest rectangle known to be covered
  before it walks the bitmap.
- A scaled sprite is sampled and blended in one pass: a transparent texel is
  skipped and an opaque one copied. A filtered image is one pass too: four
  opaque texels are filtered two channels per multiply, and four transparent
  ones are skipped.
- Blending uses integer arithmetic. When a fractional global alpha is
  involved it uses 16 bits, so stacked translucent layers do not drift.
- Pixel-aligned rectangle clips need no mask.
- Glyphs are rasterized once per size and subpixel phase, then blended from a
  cache.
- Opaque image texels are copied in runs.
- Smooth stretches of a stroke become one ribbon contour; a stroked circle is
  two rings rather than hundreds of overlapping pieces.
- `Window::present()` uploads the canvas without copying it.

## SVG

`SvgDocument::fromString()` and `fromFile()` parse a document once into
retained `Path2D` geometry. `render()` and `renderFitted()` evaluate its SMIL
animations at the given time and draw it with ordinary `Canvas` calls. Both
backends draw it the same way.

The compiler supports:

- **Shapes and paths:** basic shapes, every path command, `use`, `clipPath`,
  `transform` and `transform-origin`.
- **Styling:** presentation attributes, inline style, and `<style>` rules for
  element, class and id selectors.
- **Images:** embedded PNG images.
- **SMIL:** `animate`, `animateTransform` and `set`.

`svgPathData(d)` parses a single path string; `Path2D(d)` does the same.

## Files

- **`canvas.h`, `canvas.cpp`:** the API, and the native and web backends.
  - `detail/soft*.h` holds the native internals: the drawing model,
    shaders, the display list and culling.
  - `detail/raster.h` holds the scan converter and thread pool.
  - `detail/web*.h` holds the browser backend: the op stream, culling and
    page-side JavaScript.
  - `detail/geometry.h` holds flattening, stroking and hit testing.
  - `detail/css.h` holds the CSS colour, font and filter parsers.
  - `detail/effects.h` holds blur, colour filters and compositing.
- **`canvas_skia.cpp`:** the Skia backend.
- **`font.h`, `font.cpp`:** a TrueType/CFF reader. It provides kerning (GPOS,
  `kern`, `kerx`), metrics that match Chrome, TTC faces, and style matching.
- **`svg.h`, `svg.cpp`:** the SVG compiler.
- **`window.h`, `window.cpp`, `touch_gesture.*`, `input_guard.h`:** the
  window and input layer.
- **`tests/`:** native tests, the conformance scenes and the benchmark.
- **`tools/conformance/`:** the comparison against Chrome.

Painting is single-threaded per process. Draw from one thread, as in a
browser; the library spreads each flush across the cores itself.
