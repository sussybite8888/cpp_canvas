#pragma once
// The browser backend's Canvas methods. Included at the end of web.h.

// ===========================================================================
// Flushing one canvas.
// ===========================================================================
void Canvas::flush() {
  OpBuffer& b = opBuffer();
  if (b.owner != this || b.count == 0) return;
  if (contextId_ < 0) {
    // Recorded after a move took its context: there is nowhere to send it.
    discardOps(b);
    return;
  }
  ++contentVersions()[contextId_];
  // A GPU canvas records only around what the GPU does not draw, and those
  // records are for its shadow surface (its 2D context): they go as any
  // other canvas's do. Anything recorded outside such a span is treated as one.
  const bool stray = ops_->gpu && !ops_->gpu->generic;
  if (stray) ops_->gpu->beginGeneric();
  if (cullingEnabled_) gFrameStats.culled += cullBuffer(b, width_, height_, ops_->pathGen);
  const int count = b.count;
  b.count = 0;
  const double started = emscripten_get_now();
  c2d_flush(contextId_, &b.slots[0].i, count, b.text.empty() ? nullptr : b.text.c_str(), b.paths.empty() ? nullptr : b.paths.data(),
            b.bytes.empty() ? nullptr : b.bytes.data());
  gFrameStats.flushMillis += emscripten_get_now() - started;
  gFrameStats.ops += count;
  ++gFrameStats.batches;
  b.text.clear(); b.paths.clear(); b.bytes.clear();
  if (stray) ops_->gpu->endGeneric(layerDepth_ > 0);
}
void Canvas::setCulling(bool enabled) { cullingEnabled_ = enabled; }

int canvasOpsEmitted() { return gFrameStats.ops + opBuffer().count; }
const int* canvasOpTypeCounts() { return gOpTypeCounts; }
CanvasFrameStats canvasTakeFrameStats() {
  const CanvasFrameStats taken = gFrameStats;
  gFrameStats = CanvasFrameStats{};
  for (int& count : gOpTypeCounts) count = 0;
  return taken;
}
const char* canvasOpName(int code) {
  switch (code) {
    case 0: return "save"; case 1: return "restore"; case 2: return "reset"; case 3: return "scale";
    case 4: return "rotate"; case 5: return "translate"; case 6: return "transform"; case 7: return "setTransform";
    case 8: return "resetTransform"; case 9: return "fillStyle"; case 10: return "strokeStyle"; case 11: return "globalAlpha";
    case 12: return "composite"; case 13: return "filter"; case 14: return "lineWidth"; case 15: return "lineCap";
    case 16: return "lineJoin"; case 17: return "miterLimit"; case 18: return "lineDashOffset"; case 19: return "shadow";
    case 20: return "font"; case 21: return "textAlign"; case 22: return "textBaseline"; case 23: return "direction";
    case 24: return "smoothing"; case 25: return "smoothingQuality"; case 26: return "setLineDash"; case 27: return "letterSpacing";
    case 28: return "wordSpacing"; case 29: return "fontKerning";
    case 30: return "clearRect"; case 31: return "fillRect"; case 32: return "strokeRect"; case 33: return "beginPath";
    case 34: return "closePath"; case 35: return "moveTo"; case 36: return "lineTo"; case 37: return "quadraticCurveTo";
    case 38: return "bezierCurveTo"; case 39: return "arc"; case 40: return "arcTo"; case 41: return "ellipse";
    case 42: return "rect"; case 43: return "roundRect"; case 44: return "fill"; case 45: return "stroke";
    case 46: return "clip"; case 47: return "fillText"; case 48: return "strokeText"; case 49: return "roundRect(radii)";
    case 50: return "fill(cached path)"; case 51: return "drawImage"; case 52: return "drawImage(sub)"; case 53: return "fill(new path)";
    case 54: return "drawImage(remote)"; case 55: return "drawImage(pixels)"; case 56: return "putImageData"; case 57: return "dropImage";
    case 58: return "clear"; case 59: return "beginLayer"; case 60: return "endLayer"; case 61: return "culled";
    default: return nullptr;
  }
}

CanvasContexts canvasCheckContexts() {
  static const int useProbe = c2d_blink() ? 0 : 1;
  switch (c2d_loss_check(useProbe)) {
    case 1: return CanvasContexts::Lost;
    case 2: return CanvasContexts::LostTooLong;
    case 3: return CanvasContexts::Recoverable;
    default: return CanvasContexts::Fine;
  }
}
void canvasRecoverContexts() {
  canvasFlushOps();
  const int replaced = c2d_recover();
  for (auto& [context, canvas] : liveCanvases()) canvas->forgetPageState();
  imageKeys().clear();
  const std::string note = "[canvas] the GPU lost this page's canvases: bakes dropped, " + std::to_string(replaced) + " kept surface(s) replaced";
  c2d_warn(note.c_str());
}

bool canvasRegisterFont(const std::string& family, const std::vector<std::uint8_t>& data, int weight, bool italic) {
  if (data.empty()) return false;
  return c2d_register_font(family.c_str(), data.data(), static_cast<int>(data.size()), weight, italic ? 1 : 0) != 0;
}

// ===========================================================================
// State mirror.
// ===========================================================================
const std::string* Canvas::WebString::intern(const std::string& s) {
  // Never freed: these are the few names a canvas state takes.
  static auto* table = new std::unordered_set<std::string>();
  return &*table->insert(s).first;
}
namespace {
const std::string& cssFill(const Canvas::WebState& w) { if (w.fill.empty() && w.fillRgbaOk) w.fill = css(w.fillRgba); return w.fill; }
const std::string& cssStroke(const Canvas::WebState& w) { if (w.stroke.empty() && w.strokeRgbaOk) w.stroke = css(w.strokeRgba); return w.stroke; }
}  // namespace
void Canvas::sync(unsigned need) const {
  const unsigned todo = dirty_ & need;
  if (todo == 0) return;
  dirty_ &= ~todo;
  const WebState& w = web_;
  WebState& a = applied_;
  if ((todo & kFillBit) && cssFill(w) != a.fill) { a.fill = w.fill; a.fillObject = w.fillObject; OP(9, {}, a.fill.c_str()); }
  if ((todo & kStrokeBit) && cssStroke(w) != a.stroke) { a.stroke = w.stroke; a.strokeObject = w.strokeObject; OP(10, {}, a.stroke.c_str()); }
  if ((todo & kAlphaBit) && w.alpha != a.alpha) { a.alpha = w.alpha; OP(11, {a.alpha}); }
  if ((todo & kCompositeBit) && w.composite != a.composite) { a.composite = w.composite; OP(12, {}, a.composite.c_str()); }
  if ((todo & kFilterBit) && w.filter != a.filter) { a.filter = w.filter; OP(13, {}, a.filter.c_str()); }
  if ((todo & kShadowBit) && (w.shadowColour != a.shadowColour || w.shadowBlur != a.shadowBlur || w.shadowOffsetX != a.shadowOffsetX || w.shadowOffsetY != a.shadowOffsetY)) {
    a.shadowColour = w.shadowColour; a.shadowBlur = w.shadowBlur; a.shadowOffsetX = w.shadowOffsetX; a.shadowOffsetY = w.shadowOffsetY;
    OP(19, {a.shadowBlur, a.shadowOffsetX, a.shadowOffsetY}, a.shadowColour.c_str());
  }
  if ((todo & kLineWidthBit) && w.lineWidth != a.lineWidth) { a.lineWidth = w.lineWidth; OP(14, {a.lineWidth}); }
  if ((todo & kLineCapBit) && w.lineCap != a.lineCap) { a.lineCap = w.lineCap; OP(15, {}, a.lineCap.c_str()); }
  if ((todo & kLineJoinBit) && w.lineJoin != a.lineJoin) { a.lineJoin = w.lineJoin; OP(16, {}, a.lineJoin.c_str()); }
  if ((todo & kMiterBit) && w.miterLimit != a.miterLimit) { a.miterLimit = w.miterLimit; OP(17, {a.miterLimit}); }
  if ((todo & kDashBit) && w.dash != a.dash) {
    // The array rides in the path arena: no flush needed for it any more.
    a.dash = w.dash;
    OpSlot* r = pushRef(*this, BUF, 26, 0, {static_cast<float>(a.dash.size())});
    r[11].i = static_cast<std::int32_t>(BUF.paths.size());
    BUF.paths.insert(BUF.paths.end(), a.dash.begin(), a.dash.end());
  }
  if ((todo & kDashOffsetBit) && w.dashOffset != a.dashOffset) { a.dashOffset = w.dashOffset; OP(18, {a.dashOffset}); }
  if ((todo & kFontBit) && w.font != a.font) { a.font = w.font; OP(20, {}, a.font.c_str()); }
  if ((todo & kTextAlignBit) && w.textAlign != a.textAlign) { a.textAlign = w.textAlign; OP(21, {}, a.textAlign.c_str()); }
  if ((todo & kTextBaselineBit) && w.textBaseline != a.textBaseline) { a.textBaseline = w.textBaseline; OP(22, {}, a.textBaseline.c_str()); }
  if ((todo & kDirectionBit) && w.direction != a.direction) { a.direction = w.direction; OP(23, {}, a.direction.c_str()); }
  if ((todo & kSmoothingBit) && w.smoothing != a.smoothing) { a.smoothing = w.smoothing; OP(24, {a.smoothing ? 1.f : 0.f}); }
  if ((todo & kSmoothingQualityBit) && w.smoothingQuality != a.smoothingQuality) { a.smoothingQuality = w.smoothingQuality; OP(25, {}, a.smoothingQuality.c_str()); }
  if ((todo & kLetterSpacingBit) && w.letterSpacing != a.letterSpacing) { a.letterSpacing = w.letterSpacing; OP(27, {}, a.letterSpacing.c_str()); }
  if ((todo & kWordSpacingBit) && w.wordSpacing != a.wordSpacing) { a.wordSpacing = w.wordSpacing; OP(28, {}, a.wordSpacing.c_str()); }
  if ((todo & kKerningBit) && w.fontKerning != a.fontKerning) { a.fontKerning = w.fontKerning; OP(29, {}, a.fontKerning.c_str()); }
}
void Canvas::realSave() {
  if (webStack_.empty() || webStack_.back().real) return;
  webStack_.back().real = true;
  appliedStack_.push_back(applied_);
  OP(0, {});
}
void Canvas::forgetPageState() {
  applied_ = WebState{};
  appliedStack_.clear();
  for (WebFrame& frame : webStack_) frame.real = false;
  dirty_ = kAllStateBits;
  frameKnown_ = false;
  if (ops_->gpu) { ops_->gpu->beginPath(); ops_->gpu->pathStale = false; }
  currentPath_ = Path2D{}; pathMode_ = PathMode::Empty;
  pagePathDirty_ = false; pagePathSegments_ = 0; pathHasSubpath_ = false;
  pageSeamMoved_ = false; pathUniform_ = true;
  layerDepth_ = 0;
}

// ===========================================================================
// Culling metadata for the record just written.
// ===========================================================================
namespace {
// Whether a style string or object paints only opaque pixels.
bool styleOpaque(const std::string& s, const std::shared_ptr<void>& object) {
  if (!s.empty() && s[0] == '\x01') {
    if (s.size() > 1 && s[1] == 'g' && object) {
      const auto& d = *std::static_pointer_cast<CanvasGradient::Data>(object);
      if (d.kind == 1 || d.stops.empty()) return false;
      for (const auto& st : d.stops) if (st.second.a != 255) return false;
      return true;
    }
    if (s.size() > 1 && s[1] == 'p' && object) {
      const auto& d = *std::static_pointer_cast<CanvasPattern::Data>(object);
      return d.opaque && d.repeatX && d.repeatY;
    }
    return false;
  }
  Color c;
  return canvas_detail::parseCssColor(s, c) && c.a == 255;
}
bool unboundedComposite(const std::string& op) {
  return op == "copy" || op == "source-in" || op == "source-out" || op == "destination-in" || op == "destination-atop";
}
}  // namespace

// kind: 0 fill, 1 stroke, 2 image, 3 text, 4 clearRect, 5 putImageData (ignores clip), 6 clear
void Canvas::notePaint(int kind, const float* box, bool occluder, const float* inner) const {
  OpMeta& m = lastMeta(BUF);
  m.kind = 1;
  const WebState& w = web_;
  float x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
  const bool ignoresStyle = kind >= 4;
  if (!ignoresStyle) {
    const bool shadow = !(w.shadowColour.size() == 9 && w.shadowColour.compare(7, 2, "00") == 0) &&
                        (w.shadowBlur > 0 || w.shadowOffsetX != 0 || w.shadowOffsetY != 0);
    if (shadow) {
      const float spread = w.shadowBlur * 1.5f + 2;
      x0 = std::min(x0, x0 + w.shadowOffsetX) - spread; x1 = std::max(x1, x1 + w.shadowOffsetX) + spread;
      y0 = std::min(y0, y0 + w.shadowOffsetY) - spread; y1 = std::max(y1, y1 + w.shadowOffsetY) + spread;
    }
    if (w.filter != "none" || unboundedComposite(w.composite)) { x0 = -1e30f; y0 = -1e30f; x1 = 1e30f; y1 = 1e30f; }
  }
  if (kind != 5) { x0 = std::max(x0, w.clipX0); y0 = std::max(y0, w.clipY0); x1 = std::min(x1, w.clipX1); y1 = std::min(y1, w.clipY1); }
  m.x0 = x0; m.y0 = y0; m.x1 = x1; m.y1 = y1;
  if (!occluder || !inner) return;
  if (!ignoresStyle) {
    if (w.alpha < 1.f || w.composite != "source-over" || w.filter != "none") return;
  }
  float ox0 = std::ceil(inner[0]), oy0 = std::ceil(inner[1]), ox1 = std::floor(inner[2]), oy1 = std::floor(inner[3]);
  if (kind != 5 && w.clipped) {
    if (!w.clipExact) return;
    ox0 = std::max(ox0, std::ceil(w.clipX0)); oy0 = std::max(oy0, std::ceil(w.clipY0));
    ox1 = std::min(ox1, std::floor(w.clipX1)); oy1 = std::min(oy1, std::floor(w.clipY1));
  }
  const auto c = [](float v) { return static_cast<std::int32_t>(std::clamp(v, -1e8f, 1e8f)); };
  if (ox0 < ox1 && oy0 < oy1) { m.ox0 = c(ox0); m.oy0 = c(oy0); m.ox1 = c(ox1); m.oy1 = c(oy1); }
}
void Canvas::noteUnbounded() const {}

namespace {
// The device box of a user-space rect under a matrix, and its inner box when
// the matrix keeps it axis-aligned.
void rectBoxes(const std::array<double, 6>& m, float x, float y, float w, float h, float box[4], float inner[4], bool& axis) {
  const canvas_detail::Matrix f = floatMatrix(m);
  canvas_detail::Box b;
  canvas_detail::addMappedRect(b, f, x, y, x + w, y + h);
  box[0] = b.x0 - 1; box[1] = b.y0 - 1; box[2] = b.x1 + 1; box[3] = b.y1 + 1;
  axis = f[1] == 0 && f[2] == 0;
  inner[0] = b.x0; inner[1] = b.y0; inner[2] = b.x1; inner[3] = b.y1;
  if (b.empty()) { box[0] = box[1] = 0; box[2] = box[3] = -1; }
}
}  // namespace

// ===========================================================================
// Transform mapping into the page's frame (see canvas.h).
// ===========================================================================
const Canvas::PageFrame& Canvas::frameFor(const std::array<double, 6>& m, unsigned need) const {
  static const PageFrame kSame{};
  if (applied_.matrix == m) return kSame;
  if (!frameKnown_ || framePage_ != applied_.matrix || frameCall_ != m) {
    frame_ = relativeFrame(applied_.matrix, m);
    framePage_ = applied_.matrix;
    frameCall_ = m;
    frameKnown_ = true;
  }
  if ((frame_.fits & need) == need && frame_.fits != 0) return frame_;
  movePageTo(m);
  return kSame;
}
void Canvas::movePageTo(const std::array<double, 6>& m) const {
  if (applied_.matrix == m || !finiteMatrix(m)) return;
  applied_.matrix = m;
  OP(7, {static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2]), static_cast<float>(m[3]), static_cast<float>(m[4]), static_cast<float>(m[5])});
}
namespace {
void noteBlinkFullTurn(const CanvasPageFrame& r) {
  static int blink = -1;
  static bool said = false;
  if (said) return;
  if (blink < 0) blink = c2d_blink();
  if (!blink) { said = true; return; }
  if (!(r.fits & kFitSimilar) || ((r.fits & kFitAxis) && (r.fits & kFitPositive))) return;
  said = true;
  c2d_warn("[canvas] Chrome draws some full-circle arcs as half discs when float rounding leaves a whole turn's sweep just short of 2pi; "
           "filled full circles are sent starting at angle 0, which it always draws whole.");
}
// A segment as the record that adds it to the page's current path.
void emitSegment(const Canvas& c, OpBuffer& b, const Path2D::Segment& g) {
  const float* v = g.v;
  const float ccw = g.counterClockwise ? 1.f : 0.f;
  switch (g.command) {
    case Path2D::Command::Move: push(c, b, 35, {v[0], v[1]}); break;
    case Path2D::Command::Line: push(c, b, 36, {v[0], v[1]}); break;
    case Path2D::Command::Quadratic: push(c, b, 37, {v[0], v[1], v[2], v[3]}); break;
    case Path2D::Command::Bezier: push(c, b, 38, {v[0], v[1], v[2], v[3], v[4], v[5]}); break;
    case Path2D::Command::Arc: push(c, b, 39, {v[0], v[1], v[2], v[3], v[4], ccw}); break;
    case Path2D::Command::ArcTo: push(c, b, 40, {v[0], v[1], v[2], v[3], v[4]}); break;
    case Path2D::Command::Ellipse: push(c, b, 41, {v[0], v[1], v[2], v[3], v[4], v[5], v[6], ccw}); break;
    case Path2D::Command::Rect: push(c, b, 42, {v[0], v[1], v[2], v[3]}); break;
    case Path2D::Command::RoundRect: push(c, b, 43, {v[0], v[1], v[2], v[3], v[4]}); break;
    case Path2D::Command::Close: push(c, b, 34, {}); break;
  }
  OpMeta& m = lastMeta(b);
  m.kind = 2;
  m.pathGen = b.state->pathGen;
}
}  // namespace
void Canvas::emitSegmentAt(const std::array<double, 6>& m, const Path2D::Segment& g, bool loose) const {
  const unsigned need = loose ? (kFitAffine | kFitSimilar) : segmentNeed(g);
  if (need == 0) { emitSegment(*this, BUF, g); return; }
  if (wholeTurn(g) && applied_.matrix != m) noteBlinkFullTurn(relativeFrame(applied_.matrix, m));
  const PageFrame& f = frameFor(m, need);
  if (f.same()) { emitSegment(*this, BUF, g); return; }
  if (loose && !((f.fits & kFitAxis) && (f.fits & kFitPositive))) pageSeamMoved_ = true;
  emitSegment(*this, BUF, mapSegment(g, f, loose));
}

// ===========================================================================
// Retained paths (shape cache and path cache), as in florr's backend: a
// current path whose exact geometry was painted before is painted from a
// retained Path2D the page already holds.
// ===========================================================================
namespace {
constexpr std::size_t kMaxCachedShapes = 1024;
constexpr std::size_t kMaxCachedShapeSegments = 64;
constexpr std::size_t kShapeSightings = 4096;
struct ShapeCache {
  std::array<std::uint64_t, kShapeSightings> sighted{};
  std::unordered_map<std::uint64_t, std::unique_ptr<Path2D>> paths;
};
ShapeCache& shapeCache() { static ShapeCache cache; return cache; }
std::uint64_t shapeHash(const Path2D& path) {
  std::uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](std::uint32_t value) { h ^= value; h *= 1099511628211ull; };
  for (const Path2D::Segment& g : path.segments()) {
    mix(static_cast<std::uint32_t>(g.command) | (g.counterClockwise ? 0x100u : 0u));
    for (const float f : g.v) { std::uint32_t bits; std::memcpy(&bits, &f, sizeof bits); mix(bits); }
  }
  return h | 1;
}
bool sameShape(const Path2D& a, const Path2D& b) {
  const auto& x = a.segments();
  const auto& y = b.segments();
  if (x.size() != y.size()) return false;
  for (std::size_t i = 0; i < x.size(); ++i)
    if (x[i].command != y[i].command || x[i].counterClockwise != y[i].counterClockwise || std::memcmp(x[i].v, y[i].v, sizeof x[i].v) != 0) return false;
  return true;
}
bool worthCaching(const Path2D& path) {
  const auto& segments = path.segments();
  if (segments.empty() || segments.size() > kMaxCachedShapeSegments) return false;
  int curves = 0, others = 0;
  for (const Path2D::Segment& g : segments) {
    if (g.command == Path2D::Command::Arc || g.command == Path2D::Command::Ellipse) ++curves;
    else if (g.command != Path2D::Command::Move && g.command != Path2D::Command::Close) ++others;
  }
  return !(curves == 1 && others == 0);
}
const Path2D* cachedShape(const Path2D& path) {
  ShapeCache& cache = shapeCache();
  const std::uint64_t h = shapeHash(path);
  if (const auto found = cache.paths.find(h); found != cache.paths.end()) return sameShape(*found->second, path) ? found->second.get() : nullptr;
  std::uint64_t& slot = cache.sighted[h & (kShapeSightings - 1)];
  if (slot != h) { slot = h; return nullptr; }
  if (cache.paths.size() >= kMaxCachedShapes) cache.paths.clear();
  return cache.paths.emplace(h, std::make_unique<Path2D>(path)).first->second.get();
}
constexpr std::size_t kMaxCachedPaths = 2048;
std::unordered_map<std::uint32_t, std::uint32_t> gCachedRevision;
std::deque<std::uint32_t> gCacheOrder;
bool pathAlreadySent(const Path2D& path) {
  const std::uint32_t key = path.cacheKey();
  const auto it = gCachedRevision.find(key);
  if (it != gCachedRevision.end()) {
    if (it->second == path.revision()) return true;
    it->second = path.revision();
    return false;
  }
  gCacheOrder.push_back(key);
  if (gCacheOrder.size() > kMaxCachedPaths) {
    const std::uint32_t evicted = gCacheOrder.front();
    gCacheOrder.pop_front();
    gCachedRevision.erase(evicted);
    // Buffered records may name it: every buffer goes first.
    flushAllBuffers();
    c2d_path_drop(evicted);
  }
  gCachedRevision.emplace(key, path.revision());
  return false;
}
constexpr std::size_t kMaxInlinePathSegments = 8;
// A path with its geometry in the buffer's arena (record 53).
OpSlot* pushPathRecord(const Canvas& c, OpBuffer& b, std::int32_t key, const Path2D& path, int action, bool evenOdd) {
  OpSlot* r = opRecord(c, b, 53);
  const std::size_t at = b.paths.size();
  b.paths.reserve(at + path.segments().size() * 10);
  for (const auto& q : path.segments()) {
    b.paths.push_back(static_cast<float>(q.command));
    b.paths.push_back(q.counterClockwise);
    for (const float v : q.v) b.paths.push_back(v);
  }
  r[1].f = static_cast<float>(action);
  r[2].f = evenOdd ? 1.f : 0.f;
  r[3].f = static_cast<float>(at);
  r[4].f = static_cast<float>(path.segments().size());
  r[11].i = key;
  if (key) lastMeta(b).keepsPath = true;
  return r;
}
void emitPath(const Canvas& c, OpBuffer& b, const Path2D& path, int action, bool evenOdd) {
  const std::uint32_t key = path.cacheKey();
  if (pathAlreadySent(path)) { pushRef(c, b, 50, static_cast<std::int32_t>(key), {static_cast<float>(action), evenOdd ? 1.f : 0.f}); return; }
  pushPathRecord(c, b, static_cast<std::int32_t>(key), path, action, evenOdd);
}
}  // namespace

void Canvas::pathSegmentAdded() {
  if (ops_->gpu && !ops_->gpu->generic) {
    // Nothing is sent: the path is drawn from currentPath_ in the user space
    // of pathMatrix_. A transform that changes partway takes the path so far
    // into the new space (arcs become curves), and it carries on there.
    if (pathMode_ == PathMode::Empty) { pathMatrix_ = web_.matrix; pathMode_ = PathMode::Deferred; pathUniform_ = true; return; }
    if (web_.matrix == pathMatrix_) return;
    auto& segs = currentPath_.segments();
    Path2D old, moved;
    old.segments().assign(segs.begin(), segs.end() - 1);
    const Path2D::Segment last = segs.back();
    canvas_detail::Matrix inv;
    if (!canvas_detail::invert(toMatrix(web_.matrix), inv)) return;
    const canvas_detail::Matrix t = canvas_detail::multiply(inv, toMatrix(pathMatrix_));
    moved.addPath(old, t);
    moved.segments().push_back(last);
    currentPath_ = std::move(moved);
    pathMatrix_ = web_.matrix;
    return;
  }
  const Path2D& path = currentPath_;
  switch (pathMode_) {
    case PathMode::Empty:
      pathMatrix_ = web_.matrix;
      pathMode_ = PathMode::Deferred;
      pathUniform_ = true;
      return;
    case PathMode::Deferred:
      if (web_.matrix == pathMatrix_) return;
      pathUniform_ = false;
      sendHeldPath(path.segments().size() - 1);
      break;
    case PathMode::Sent:
      if (web_.matrix != pathMatrix_) pathUniform_ = false;
      break;
  }
  emitSegmentAt(web_.matrix, path.segments().back());
  ++pagePathSegments_;
  pagePathDirty_ = true;
}
void Canvas::sendHeldPath(std::size_t count, bool fillOnly) const {
  ++ops_->pathGen;
  OP(33, {});
  lastMeta(BUF).kind = 2;
  lastMeta(BUF).pathGen = ops_->pathGen;
  pageSeamMoved_ = false;
  const std::vector<Path2D::Segment>& segs = currentPath_.segments();
  for (std::size_t i = 0; i < count; ++i) emitSegmentAt(pathMatrix_, segs[i], fillOnly && looseTurn(segs, i));
  pagePathSegments_ = static_cast<int>(count);
  pagePathDirty_ = count > 0;
  pathMode_ = PathMode::Sent;
}
void Canvas::preparePagePath(bool fillOnly) const {
  if (pathMode_ == PathMode::Deferred) {
    sendHeldPath(currentPath_.segments().size(), fillOnly);
  } else if (pathMode_ == PathMode::Empty && pagePathDirty_) {
    ++ops_->pathGen;
    OP(33, {});
    lastMeta(BUF).kind = 2;
    lastMeta(BUF).pathGen = ops_->pathGen;
    pageSeamMoved_ = false; pagePathDirty_ = false; pagePathSegments_ = 0;
  }
}
bool Canvas::paintCachedPath(int action, bool evenOdd) {
  // A GPU canvas reads its paths from the records themselves.
  if (ops_->gpu) return false;
  if (pathMode_ != PathMode::Deferred || web_.matrix != pathMatrix_) return false;
  const Path2D& path = currentPath_;
  if (!worthCaching(path)) return false;
  const Path2D* cached = cachedShape(path);
  if (!cached) return false;
  movePageTo(pathMatrix_);
  const std::int32_t key = static_cast<std::int32_t>(cached->cacheKey());
  if (pathAlreadySent(*cached)) pushRef(*this, BUF, 50, key, {static_cast<float>(action), evenOdd ? 1.f : 0.f});
  else pushPathRecord(*this, BUF, key, *cached, action, evenOdd);
  return true;
}
void Canvas::emitPathAt(const Path2D& p, int action, bool evenOdd) const {
  if (p.segments().size() <= kMaxInlinePathSegments && action != 1) {
    const std::vector<Path2D::Segment>& segs = p.segments();
    unsigned need = kFitAffine;
    for (std::size_t i = 0; i < segs.size(); ++i) need |= looseTurn(segs, i) ? (kFitAffine | kFitSimilar) : segmentNeed(segs[i]);
    const PageFrame& f = frameFor(web_.matrix, need);
    if (f.same()) { pushPathRecord(*this, BUF, 0, p, action, evenOdd); return; }
    Path2D mapped;
    std::vector<Path2D::Segment>& out = mapped.segments();
    out.reserve(segs.size());
    for (std::size_t i = 0; i < segs.size(); ++i) out.push_back(mapSegment(segs[i], f, looseTurn(segs, i)));
    pushPathRecord(*this, BUF, 0, mapped, action, evenOdd);
    return;
  }
  movePageTo(web_.matrix);
  if (p.segments().size() <= kMaxInlinePathSegments) pushPathRecord(*this, BUF, 0, p, action, evenOdd);
  else emitPath(*this, BUF, p, action, evenOdd);
}

#define STATE(bit, field, value) \
  do { if (web_.field != (value)) { web_.field = (value); dirty_ |= (bit); } } while (0)

// ===========================================================================
// A GPU canvas (detail/webgpu.h): each draw goes to the GPU, unless the GPU
// does not do it; then the code below records it for the shadow surface, as
// for any canvas, from a page state that starts over.
// ===========================================================================
void Canvas::gpuGenericBegin() {
  // Already recording for the shadow (inside a layer): the page state the
  // records so far assume stands.
  if (ops_->gpu->generic) { ops_->gpu->beginGeneric(); return; }
  ops_->gpu->beginGeneric();
  applied_ = WebState{};
  appliedStack_.clear();
  for (WebFrame& frame : webStack_) frame.real = false;
  dirty_ = kAllStateBits;
  frameKnown_ = false;
}
void Canvas::gpuGenericEnd() {
  flush();
  ops_->gpu->endGeneric(layerDepth_ > 0);
}
struct GpuGenericScope {
  Canvas& c;
  explicit GpuGenericScope(Canvas& canvas) : c(canvas) { c.gpuGenericBegin(); }
  ~GpuGenericScope() { c.gpuGenericEnd(); }
};
// `fast`: the GPU's way, true when done. Otherwise the rest of the method
// records the draw for the shadow.
#define GPU_DRAW(fast) \
  std::optional<GpuGenericScope> gpuScope_; \
  if (ops_->gpu) { \
    ops_->gpu->use(web_); \
    if (!ops_->gpu->generic && (fast)) { ops_->gpu->shadowStale = true; return; } \
    gpuScope_.emplace(*this); \
    gpuSyncPath(); \
  }

// ===========================================================================
// The API.
// ===========================================================================
void Canvas::save() { webStack_.push(web_); if (ops_->gpu) ops_->gpu->save(); }
void Canvas::restore() {
  if (webStack_.empty()) return;
  WebFrame& frame = webStack_.back();
  if (frame.real) {
    OP(1, {});
    applied_ = std::move(appliedStack_.back());
    appliedStack_.pop_back();
  }
  // Swapped, not moved: the frame keeps buffers for the next save to copy into.
  web_.swapWith(frame.state);
  webStack_.pop_back();
  dirty_ = kAllStateBits;
  if (ops_->gpu) ops_->gpu->restore();
}
void Canvas::reset() {
  webStack_.clear(); appliedStack_.clear();
  web_ = WebState{}; applied_ = WebState{}; dirty_ = 0;
  if (ops_->gpu) { ops_->gpu->beginPath(); ops_->gpu->pathStale = false; }
  currentPath_ = Path2D{}; pathMode_ = PathMode::Empty;
  pagePathDirty_ = false; pagePathSegments_ = 0; pathHasSubpath_ = false;
  pageSeamMoved_ = false; pathUniform_ = true;
  layerDepth_ = 0;
  if (ops_->gpu && !ops_->gpu->generic) {
    ops_->gpu->clip = WebGpu::Clip{}; ops_->gpu->clipStack.clear();
    ops_->gpu->clearTo(Color{0, 0, 0, 0});
    ops_->gpu->shadowStale = true;
    fill_ = stroke_ = Color{0, 0, 0}; lineWidth_ = 1;
    return;
  }
  OP(2, {});
  // A reset clears every pixel: nothing before it can show.
  const float box[4] = {0, 0, static_cast<float>(width_), static_cast<float>(height_)};
  notePaint(5, box, true, box);
  fill_ = stroke_ = Color{0, 0, 0}; lineWidth_ = 1;
}
void Canvas::scale(float a, float b) { if (!finiteArgs({a, b})) return; auto& m = web_.matrix; m[0] *= a; m[1] *= a; m[2] *= b; m[3] *= b; }
void Canvas::rotate(float a) {
  if (!std::isfinite(a)) return;
  auto& m = web_.matrix;
  const double c = std::cos(static_cast<double>(a)), s = std::sin(static_cast<double>(a));
  const double m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3];
  m[0] = m0 * c + m2 * s; m[1] = m1 * c + m3 * s; m[2] = m2 * c - m0 * s; m[3] = m3 * c - m1 * s;
}
void Canvas::translate(float a, float b) { if (!finiteArgs({a, b})) return; auto& m = web_.matrix; m[4] += m[0] * a + m[2] * b; m[5] += m[1] * a + m[3] * b; }
void Canvas::transform(float a, float b, float c, float d, float e, float f) {
  if (!finiteArgs({a, b, c, d, e, f})) return;
  const auto m = web_.matrix;
  auto& o = web_.matrix;
  o[0] = m[0] * a + m[2] * b; o[1] = m[1] * a + m[3] * b; o[2] = m[0] * c + m[2] * d; o[3] = m[1] * c + m[3] * d;
  o[4] = m[0] * e + m[2] * f + m[4]; o[5] = m[1] * e + m[3] * f + m[5];
}
void Canvas::setTransform(float a, float b, float c, float d, float e, float f) { if (!finiteArgs({a, b, c, d, e, f})) return; web_.matrix = {a, b, c, d, e, f}; }
void Canvas::resetTransform() { web_.matrix = {1, 0, 0, 1, 0, 0}; }
bool Canvas::isPlainComposite() const {
  const bool shadowless = web_.shadowColour.size() == 9 && web_.shadowColour.compare(7, 2, "00") == 0;
  const bool shadowStill = web_.shadowBlur == 0 && web_.shadowOffsetX == 0 && web_.shadowOffsetY == 0;
  return web_.composite == "source-over" && web_.filter == "none" && (shadowless || shadowStill);
}
float Canvas::globalAlpha() const { return web_.alpha; }
std::array<float, 6> Canvas::currentTransform() const { return floatMatrix(web_.matrix); }
Color Canvas::tinted(Color c) const {
  if (tint_.a == 0) return c;
  const float keep = 1.f - tint_.a / 255.f;
  const auto mix = [keep](std::uint8_t base, std::uint8_t add) { return static_cast<std::uint8_t>(std::clamp((base / 255.f * keep + add / 255.f) * 255.f, 0.f, 255.f)); };
  return Color{mix(c.r, tint_.r), mix(c.g, tint_.g), mix(c.b, tint_.b), c.a};
}
void Canvas::setFillStyle(Color c) {
  c = tinted(c); fill_ = c;
  if (web_.fillRgbaOk && web_.fillRgba == c) return;
  web_.fillRgba = c; web_.fillRgbaOk = true; web_.fill.clear(); web_.fillObject.reset(); dirty_ |= kFillBit;
}
void Canvas::setStrokeStyle(Color c) {
  c = tinted(c); stroke_ = c;
  if (web_.strokeRgbaOk && web_.strokeRgba == c) return;
  web_.strokeRgba = c; web_.strokeRgbaOk = true; web_.stroke.clear(); web_.strokeObject.reset(); dirty_ |= kStrokeBit;
}
void Canvas::setFillStyle(const std::string& s) { if (s.empty() || s[0] == '\x01') return; STATE(kFillBit, fill, s); web_.fillRgbaOk = false; web_.fillObject.reset(); }
void Canvas::setStrokeStyle(const std::string& s) { if (s.empty() || s[0] == '\x01') return; STATE(kStrokeBit, stroke, s); web_.strokeRgbaOk = false; web_.strokeObject.reset(); }

namespace {
int nextObjectId() { static int next = 1; return next++; }
// The page object for a gradient as it stands: a new one whenever its stops
// changed since the last was made.
std::string gradientRef(CanvasGradient::Data& d) {
  if (d.webId == 0 || d.webStops != d.stops.size()) {
    if (d.webId) droppedObjects().push_back("g" + std::to_string(d.webId));
    d.webId = nextObjectId();
    d.webStops = d.stops.size();
    std::vector<std::pair<float, Color>> stops = d.stops;
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<float> offsets;
    std::string names;
    for (const auto& s : stops) { offsets.push_back(s.first); names += css(s.second); names.push_back('\n'); }
    c2d_gradient(d.webId, d.kind, d.g[0], d.g[1], d.g[2], d.g[3], d.g[4], d.g[5], offsets.data(), names.c_str(), static_cast<int>(stops.size()));
  }
  return std::string("\x01g") + std::to_string(d.webId);
}
std::string patternRef(CanvasPattern::Data& d) {
  if (d.webVersion != d.version) {
    // A new transform: a new page object from the same snapshot.
    if (d.webId) droppedObjects().push_back("p" + std::to_string(d.webId));
    d.webId = nextObjectId();
    d.webVersion = d.version;
    const auto& t = d.transform;
    c2d_pattern(d.webId, -1, d.webSnapshot, nullptr, d.width, d.height, d.repetition.c_str(), t[0], t[1], t[2], t[3], t[4], t[5]);
  }
  return std::string("\x01p") + std::to_string(d.webId);
}
}  // namespace
void Canvas::setFillStyle(const CanvasGradient& g) {
  if (!g.valid()) return;
  STATE(kFillBit, fill, gradientRef(*g.data_)); web_.fillRgbaOk = false;
  web_.fillObject = g.data_;
  gradientsByRef()[web_.fill] = g.data_;
}
void Canvas::setStrokeStyle(const CanvasGradient& g) {
  if (!g.valid()) return;
  STATE(kStrokeBit, stroke, gradientRef(*g.data_)); web_.strokeRgbaOk = false;
  web_.strokeObject = g.data_;
  gradientsByRef()[web_.stroke] = g.data_;
}
void Canvas::setFillStyle(const CanvasPattern& p) {
  if (!p.valid()) return;
  STATE(kFillBit, fill, patternRef(*p.data_)); web_.fillRgbaOk = false;
  web_.fillObject = p.data_;
}
void Canvas::setStrokeStyle(const CanvasPattern& p) {
  if (!p.valid()) return;
  STATE(kStrokeBit, stroke, patternRef(*p.data_)); web_.strokeRgbaOk = false;
  web_.strokeObject = p.data_;
}
namespace {
// A pattern's page object, made from a snapshot of its source (kept on the
// page under the pattern's own id so a later transform can remake it).
void makePatternObject(CanvasPattern::Data& d, int sourceId, const std::uint8_t* pixels) {
  d.webSnapshot = nextObjectId();
  d.webId = nextObjectId();
  d.webVersion = d.version;
  const auto& t = d.transform;
  c2d_pattern(d.webId, sourceId, d.webSnapshot, pixels, d.width, d.height, d.repetition.c_str(), t[0], t[1], t[2], t[3], t[4], t[5]);
}
}  // namespace
CanvasPattern Canvas::createPattern(const Canvas& source, const std::string& repetition) {
  CanvasPattern p;
  bool rx, ry;
  if (!parseRepetition(repetition, rx, ry) || source.contextId_ < 0) return p;
  const_cast<Canvas&>(source).flush();
  p.data_ = std::make_shared<CanvasPattern::Data>();
  p.data_->width = source.width_; p.data_->height = source.height_;
  p.data_->repeatX = rx; p.data_->repeatY = ry; p.data_->repetition = repetition.empty() ? "repeat" : repetition;
  makePatternObject(*p.data_, source.contextId_, nullptr);
  return p;
}
CanvasPattern Canvas::createPattern(const std::uint8_t* rgba, int w, int h, const std::string& repetition) {
  CanvasPattern p;
  bool rx, ry;
  if (!rgba || w <= 0 || h <= 0 || !parseRepetition(repetition, rx, ry)) return p;
  p.data_ = std::make_shared<CanvasPattern::Data>();
  p.data_->width = w; p.data_->height = h;
  p.data_->repeatX = rx; p.data_->repeatY = ry; p.data_->repetition = repetition.empty() ? "repeat" : repetition;
  bool opaque = true;
  for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h && opaque; ++i) opaque = rgba[i * 4 + 3] == 255;
  p.data_->opaque = opaque;
  makePatternObject(*p.data_, -1, rgba);
  return p;
}

void Canvas::setGlobalAlpha(float a) { if (!(a >= 0 && a <= 1)) return; STATE(kAlphaBit, alpha, a); }
void Canvas::setGlobalCompositeOperation(const std::string& s) {
  canvas_detail::CompositeOp op;
  if (!canvas_detail::parseCompositeOp(s, op)) return;
  STATE(kCompositeBit, composite, s);
}
void Canvas::setFilter(const std::string& s) {
  std::vector<canvas_detail::CssFilterFunction> list;
  if (!canvas_detail::parseCssFilter(s, list)) return;
  STATE(kFilterBit, filter, list.empty() ? std::string("none") : s);
}
void Canvas::setLineWidth(float a) { if (!(a > 0) || !std::isfinite(a)) return; lineWidth_ = a; STATE(kLineWidthBit, lineWidth, a); }
void Canvas::setLineCap(const std::string& s) { if (s != "butt" && s != "round" && s != "square") return; STATE(kLineCapBit, lineCap, s); }
void Canvas::setLineJoin(const std::string& s) { if (s != "miter" && s != "round" && s != "bevel") return; STATE(kLineJoinBit, lineJoin, s); }
void Canvas::setMiterLimit(float a) { if (!(a > 0) || !std::isfinite(a)) return; STATE(kMiterBit, miterLimit, a); }
void Canvas::setLineDash(const std::vector<float>& v) {
  for (float f : v) if (!(f >= 0) || !std::isfinite(f)) return;
  std::vector<float> d = v;
  if (d.size() % 2) d.insert(d.end(), v.begin(), v.end());
  STATE(kDashBit, dash, d);
}
std::vector<float> Canvas::getLineDash() const { return web_.dash; }
void Canvas::setLineDashOffset(float a) { if (!std::isfinite(a)) return; STATE(kDashOffsetBit, dashOffset, a); }
void Canvas::setShadow(Color c, float a, float b, float d) {
  const std::string s = css(c);
  const float blur = (a >= 0 && std::isfinite(a)) ? a : web_.shadowBlur;
  const float ox = std::isfinite(b) ? b : web_.shadowOffsetX, oy = std::isfinite(d) ? d : web_.shadowOffsetY;
  if (web_.shadowColour == s && web_.shadowBlur == blur && web_.shadowOffsetX == ox && web_.shadowOffsetY == oy) return;
  web_.shadowColour = s; web_.shadowBlur = blur; web_.shadowOffsetX = ox; web_.shadowOffsetY = oy;
  dirty_ |= kShadowBit;
}
void Canvas::setShadow(const std::string& cssColor, float a, float b, float d) {
  Color c;
  if (!canvas_detail::parseCssColor(cssColor, c)) { canvas_detail::parseCssColor(web_.shadowColour, c); }
  setShadow(c, a, b, d);
}
void Canvas::setFont(const std::string& s) {
  canvas_detail::CssFont f;
  if (!canvas_detail::parseCssFont(s, f)) return;
  STATE(kFontBit, font, s);
}
void Canvas::setTextAlign(const std::string& s) {
  if (s != "start" && s != "end" && s != "left" && s != "right" && s != "center") return;
  STATE(kTextAlignBit, textAlign, s);
}
void Canvas::setTextBaseline(const std::string& s) {
  if (s != "alphabetic" && s != "top" && s != "hanging" && s != "middle" && s != "ideographic" && s != "bottom") return;
  STATE(kTextBaselineBit, textBaseline, s);
}
void Canvas::setDirection(const std::string& s) { if (s != "ltr" && s != "rtl" && s != "inherit") return; STATE(kDirectionBit, direction, s); }
void Canvas::setLetterSpacing(const std::string& s) { float px; if (!canvas_detail::parseCssLength(s, px)) return; STATE(kLetterSpacingBit, letterSpacing, s); }
void Canvas::setWordSpacing(const std::string& s) { float px; if (!canvas_detail::parseCssLength(s, px)) return; STATE(kWordSpacingBit, wordSpacing, s); }
void Canvas::setFontKerning(const std::string& s) { if (s != "auto" && s != "normal" && s != "none") return; STATE(kKerningBit, fontKerning, s); }
void Canvas::setImageSmoothingEnabled(bool a) { STATE(kSmoothingBit, smoothing, a); }
void Canvas::setImageSmoothingQuality(const std::string& s) { if (s != "low" && s != "medium" && s != "high") return; STATE(kSmoothingQualityBit, smoothingQuality, s); }

void Canvas::clear(Color c) {
  if (ops_->gpu && !ops_->gpu->generic) { ops_->gpu->clearTo(c); ops_->gpu->shadowStale = true; return; }
  // Everything already buffered for this canvas is overwritten; it goes
  // anyway (the cull drops it), so that the state it set still reaches the page.
  OP(58, {}, css(c).c_str());
  const float box[4] = {0, 0, static_cast<float>(width_), static_cast<float>(height_)};
  notePaint(6, box, !web_.clipped || web_.clipExact, box);
}
void Canvas::clearRect(float a, float b, float c, float d) {
  if (!finiteArgs({a, b, c, d})) return;
  GPU_DRAW(ops_->gpu->clearRect(a, b, c, d))
  float box[4], inner[4];
  bool axis;
  rectBoxes(web_.matrix, a, b, c, d, box, inner, axis);
  { const PageFrame& f = frameFor(web_.matrix, kFitAffine | kFitAxis); if (!f.same()) mapBox(f, a, b, c, d); }
  OP(30, {a, b, c, d});
  notePaint(4, box, axis, inner);
}
namespace {
}  // namespace
void Canvas::fillRect(float a, float b, float c, float d) {
  if (!finiteArgs({a, b, c, d})) return;
  GPU_DRAW(ops_->gpu->fillRect(a, b, c, d))
  sync(kFillBits);
  float box[4], inner[4];
  bool axis;
  rectBoxes(web_.matrix, a, b, c, d, box, inner, axis);
  { const PageFrame& f = frameFor(web_.matrix, kFitAffine | kFitAxis); if (!f.same()) mapBox(f, a, b, c, d); }
  OP(31, {a, b, c, d});
  notePaint(0, box, axis && styleOpaque(cssFill(web_), web_.fillObject), inner);
}
void Canvas::strokeRect(float a, float b, float c, float d) {
  if (!finiteArgs({a, b, c, d})) return;
  GPU_DRAW(ops_->gpu->strokeRect(a, b, c, d))
  sync(kStrokeBits);
  movePageTo(web_.matrix);
  OP(32, {a, b, c, d});
  float box[4], inner[4];
  bool axis;
  rectBoxes(web_.matrix, a, b, c, d, box, inner, axis);
  const float reach = canvas_detail::strokeReach(web_.lineWidth, 2, 0, web_.miterLimit) * canvas_detail::matrixMaxScale(floatMatrix(web_.matrix)) + 1;
  box[0] -= reach; box[1] -= reach; box[2] += reach; box[3] += reach;
  notePaint(1, box, false, nullptr);
}
#define SEGMENT() (pathHasSubpath_ = true, pathSegmentAdded())
// While the GPU draws, a path is built only on it (in device space, as cp6
// does); currentPath_ catches up when something needs it (gpuSyncPath).
#define GPU_PATH(call) do { if (ops_->gpu) { \
    WebGpu& g_ = *ops_->gpu; \
    if (g_.generic) gpuSyncPath(); \
    g_.m = floatMatrix(web_.matrix); g_.call; \
    if (!g_.generic) { g_.pathStale = true; return; } } } while (0)
void Canvas::gpuSyncPath() const {
  WebGpu* g = ops_->gpu.get();
  if (!g || !g->pathStale) return;
  g->pathStale = false;
  Canvas& self = const_cast<Canvas&>(*this);
  g->m = floatMatrix(web_.matrix);
  g->currentLocal(self.currentPath_);
  self.pathMatrix_ = web_.matrix;
  self.pathUniform_ = true;
  self.pathMode_ = currentPath_.segments().empty() ? PathMode::Empty : PathMode::Deferred;
  self.pathHasSubpath_ = g->hasCurrent;
}
void Canvas::beginPath() {
  if (ops_->gpu) { ops_->gpu->beginPath(); ops_->gpu->pathStale = false; }
  if (!currentPath_.segments().empty()) currentPath_ = Path2D{};
  if (pathMode_ == PathMode::Sent && pagePathSegments_ > 32) { OP(33, {}); pageSeamMoved_ = false; pagePathDirty_ = false; pagePathSegments_ = 0; }
  pathMode_ = PathMode::Empty;
  pathHasSubpath_ = false;
}
void Canvas::closePath() { GPU_PATH(closePath()); currentPath_.closePath(); pathSegmentAdded(); }
void Canvas::lineTo(float a, float b) {
  if (!std::isfinite(a) || !std::isfinite(b)) return;
  if (ops_->gpu && !ops_->gpu->generic) { ops_->gpu->lineTo(web_.matrix, a, b); ops_->gpu->pathStale = true; return; }
  GPU_PATH(lineTo(a, b));
  if (pathHasSubpath_) currentPath_.lineTo(a, b); else currentPath_.moveTo(a, b);
  SEGMENT();
}
void Canvas::moveTo(float a, float b) { if (!finiteArgs({a, b})) return; GPU_PATH(moveTo(a, b)); currentPath_.moveTo(a, b); SEGMENT(); }
#define ENSURE_SUBPATH(x, y) do { if (!pathHasSubpath_) { currentPath_.moveTo(x, y); SEGMENT(); } } while (0)
void Canvas::quadraticCurveTo(float a, float b, float c, float d) { if (!finiteArgs({a, b, c, d})) return; GPU_PATH(quadraticCurveTo(a, b, c, d)); ENSURE_SUBPATH(a, b); currentPath_.quadraticCurveTo(a, b, c, d); SEGMENT(); }
void Canvas::bezierCurveTo(float a, float b, float c, float d, float e, float f) { if (!finiteArgs({a, b, c, d, e, f})) return; GPU_PATH(bezierCurveTo(a, b, c, d, e, f)); ENSURE_SUBPATH(a, b); currentPath_.bezierCurveTo(a, b, c, d, e, f); SEGMENT(); }
void Canvas::arc(float a, float b, float c, float d, float e, bool f) { if (!finiteArgs({a, b, c, d, e}) || c < 0) return; GPU_PATH(arc(a, b, c, d, e, f)); currentPath_.arc(a, b, c, d, e, f); SEGMENT(); }
void Canvas::arcTo(float a, float b, float c, float d, float e) {
  if (!finiteArgs({a, b, c, d, e}) || e < 0) return;
  GPU_PATH(arcTo(a, b, c, d, e));
  if (!pathHasSubpath_) { currentPath_.moveTo(a, b); SEGMENT(); return; }
  currentPath_.arcTo(a, b, c, d, e); SEGMENT();
}
void Canvas::ellipse(float a, float b, float c, float d, float e, float f, float g, bool h) { if (!finiteArgs({a, b, c, d, e, f, g}) || c < 0 || d < 0) return; GPU_PATH(ellipse(a, b, c, d, e, f, g, h)); currentPath_.ellipse(a, b, c, d, e, f, g, h); SEGMENT(); }
void Canvas::rect(float a, float b, float c, float d) { if (!finiteArgs({a, b, c, d})) return; GPU_PATH(rect(a, b, c, d)); currentPath_.rect(a, b, c, d); SEGMENT(); }
void Canvas::roundRect(float a, float b, float c, float d, float e) { if (!finiteArgs({a, b, c, d, e}) || e < 0) return; GPU_PATH(segments([&](Path2D& p) { p.roundRect(a, b, c, d, e); })); currentPath_.roundRect(a, b, c, d, e); SEGMENT(); }
void Canvas::roundRect(float a, float b, float c, float d, const std::vector<Path2D::CornerRadius>& r) {
  // The segments a radii list becomes are ordinary ones: one at a time.
  Path2D p;
  p.roundRect(a, b, c, d, r);
  GPU_PATH(segments([&](Path2D& q) { q.roundRect(a, b, c, d, r); }));
  for (const Path2D::Segment& g : p.segments()) {
    currentPath_.segments().push_back(g);
    SEGMENT();
  }
}

namespace {
// The current path's device box and, for a lone rect / rounded rect / disc
// under an axis-aligned matrix, the box it covers completely.
bool pathBoxes(const Path2D& p, const std::array<double, 6>& m, float box[4], float inner[4]) {
  const canvas_detail::Matrix f = floatMatrix(m);
  const canvas_detail::Box b = canvas_detail::pathBounds(p, f);
  if (b.empty()) { box[0] = box[1] = 0; box[2] = box[3] = -1; return false; }
  box[0] = b.x0 - 1; box[1] = b.y0 - 1; box[2] = b.x1 + 1; box[3] = b.y1 + 1;
  if (p.segments().size() != 1 || f[1] != 0 || f[2] != 0) return false;
  const Path2D::Segment& g = p.segments()[0];
  canvas_detail::Box ib;
  if (g.command == Path2D::Command::Rect) canvas_detail::addMappedRect(ib, f, g.v[0], g.v[1], g.v[0] + g.v[2], g.v[1] + g.v[3]);
  else if (g.command == Path2D::Command::RoundRect) {
    const float x = std::min(g.v[0], g.v[0] + g.v[2]), y = std::min(g.v[1], g.v[1] + g.v[3]);
    const float w = std::abs(g.v[2]), h = std::abs(g.v[3]), r = std::clamp(g.v[4], 0.f, std::min(w, h) / 2);
    // The larger of the two crossed rects.
    if (w >= h) canvas_detail::addMappedRect(ib, f, x, y + r, x + w, y + h - r);
    else canvas_detail::addMappedRect(ib, f, x + r, y, x + w - r, y + h);
  } else if (g.command == Path2D::Command::Arc && std::abs(g.v[4] - g.v[3]) >= 6.2831f) {
    const float q = g.v[2] * 0.70710678f;
    canvas_detail::addMappedRect(ib, f, g.v[0] - q, g.v[1] - q, g.v[0] + q, g.v[1] + q);
  } else return false;
  if (ib.empty()) return false;
  inner[0] = ib.x0; inner[1] = ib.y0; inner[2] = ib.x1; inner[3] = ib.y1;
  return true;
}
}  // namespace

void Canvas::fill() {
  if (ops_->gpu && ops_->gpu->fastCircle(web_, 0)) { ops_->gpu->shadowStale = true; return; }
  static const std::string kNonzero = "nonzero";
  fill(kNonzero);
}
void Canvas::fill(const std::string& s) {
  GPU_DRAW(ops_->gpu->drawCurrent(0, s == "evenodd"))
  sync(kFillBits);
  float box[4], inner[4];
  const bool solidShape = pathBoxes(currentPath_, pathUniform_ ? pathMatrix_ : web_.matrix, box, inner);
  if (!pathUniform_) { box[0] = box[1] = -1e30f; box[2] = box[3] = 1e30f; }
  const bool occ = solidShape && pathUniform_ && styleOpaque(cssFill(web_), web_.fillObject) && s != "evenodd";
  if (paintCachedPath(0, s == "evenodd")) { notePaint(0, box, occ, inner); return; }
  preparePagePath(true);
  OP(44, {}, s.c_str());
  notePaint(0, box, occ, inner);
  lastMeta(BUF).usesPagePath = true;
  lastMeta(BUF).pathGen = ops_->pathGen;
}
void Canvas::desaturate(const std::string& s) {
  save();
  setGlobalCompositeOperation("saturation");
  setFillStyle(Color{128, 128, 128, 255});
  fill(s);
  restore();
}
void Canvas::stroke() {
  if (ops_->gpu && ops_->gpu->fastCircle(web_, 1)) { ops_->gpu->shadowStale = true; return; }
  GPU_DRAW(ops_->gpu->drawCurrent(1, false))
  sync(kStrokeBits);
  float box[4], inner[4];
  pathBoxes(currentPath_, pathUniform_ ? pathMatrix_ : web_.matrix, box, inner);
  if (!pathUniform_) { box[0] = box[1] = -1e30f; box[2] = box[3] = 1e30f; }
  const float reach = canvas_detail::strokeReach(web_.lineWidth, 2, web_.lineJoin == "miter" ? 0 : 1, web_.miterLimit) *
                      canvas_detail::matrixMaxScale(floatMatrix(web_.matrix)) + 1;
  box[0] -= reach; box[1] -= reach; box[2] += reach; box[3] += reach;
  if (paintCachedPath(1, false)) { notePaint(1, box, false, nullptr); return; }
  if (pathMode_ == PathMode::Deferred) movePageTo(pathMatrix_);
  if (pathMode_ == PathMode::Sent && pageSeamMoved_ && pathUniform_) { movePageTo(pathMatrix_); sendHeldPath(currentPath_.segments().size()); }
  preparePagePath();
  movePageTo(web_.matrix);
  OP(45, {});
  notePaint(1, box, false, nullptr);
  lastMeta(BUF).usesPagePath = true;
  lastMeta(BUF).pathGen = ops_->pathGen;
}
namespace {
// What a clip does to the culling view of the clip: its device box narrows,
// and it stays "exact" only while every clip is a pixel-aligned rectangle.
void narrowClip(Canvas::WebState& w, const Path2D& p, const std::array<double, 6>& m) {
  const canvas_detail::Matrix f = floatMatrix(m);
  const canvas_detail::Box b = canvas_detail::pathBounds(p, f);
  w.clipped = true;
  if (b.empty()) { w.clipX0 = w.clipY0 = 0; w.clipX1 = w.clipY1 = 0; return; }
  bool exact = false;
  if (p.segments().size() == 1 && p.segments()[0].command == Path2D::Command::Rect && f[1] == 0 && f[2] == 0) {
    const auto integral = [](float v) { return std::abs(v - std::round(v)) < 1e-4f; };
    exact = integral(b.x0) && integral(b.y0) && integral(b.x1) && integral(b.y1);
  }
  w.clipExact = w.clipExact && exact;
  w.clipX0 = std::max(w.clipX0, b.x0); w.clipY0 = std::max(w.clipY0, b.y0);
  w.clipX1 = std::min(w.clipX1, b.x1); w.clipY1 = std::min(w.clipY1, b.y1);
}
}  // namespace
void Canvas::clip(const std::string& s) {
  if (ops_->gpu) {
    // The GPU's clip always; the shadow's too while it is drawing (a layer).
    ops_->gpu->use(web_);
    ops_->gpu->drawCurrent(2, s == "evenodd");
    float x0, y0, x1, y1;
    web_.clipped = true; web_.clipExact = false;
    if (!ops_->gpu->deviceBox(x0, y0, x1, y1)) { web_.clipX0 = web_.clipY0 = web_.clipX1 = web_.clipY1 = 0; }
    else { web_.clipX0 = std::max(web_.clipX0, x0); web_.clipY0 = std::max(web_.clipY0, y0); web_.clipX1 = std::min(web_.clipX1, x1); web_.clipY1 = std::min(web_.clipY1, y1); }
    if (!ops_->gpu->generic) return;
    gpuSyncPath();
  }
  realSave();
  narrowClip(web_, currentPath_, pathUniform_ ? pathMatrix_ : web_.matrix);
  if (!pathUniform_) web_.clipExact = false;
  if (paintCachedPath(2, s == "evenodd")) return;
  preparePagePath(true);
  OP(46, {}, s.c_str());
  lastMeta(BUF).usesPagePath = true;
  lastMeta(BUF).pathGen = ops_->pathGen;
}
void Canvas::fill(const Path2D& p, const std::string& s) {
  GPU_DRAW(ops_->gpu->path(p, toMatrix(web_.matrix), 0, s == "evenodd"))
  sync(kFillBits);
  float box[4], inner[4];
  const bool solidShape = pathBoxes(p, web_.matrix, box, inner);
  emitPathAt(p, 0, s == "evenodd");
  notePaint(0, box, solidShape && styleOpaque(cssFill(web_), web_.fillObject) && s != "evenodd", inner);
}
void Canvas::stroke(const Path2D& p) {
  GPU_DRAW(ops_->gpu->path(p, toMatrix(web_.matrix), 1, false))
  sync(kStrokeBits);
  float box[4], inner[4];
  pathBoxes(p, web_.matrix, box, inner);
  const float reach = canvas_detail::strokeReach(web_.lineWidth, 2, web_.lineJoin == "miter" ? 0 : 1, web_.miterLimit) *
                      canvas_detail::matrixMaxScale(floatMatrix(web_.matrix)) + 1;
  box[0] -= reach; box[1] -= reach; box[2] += reach; box[3] += reach;
  emitPathAt(p, 1, false);
  notePaint(1, box, false, nullptr);
}
void Canvas::clip(const Path2D& p, const std::string& s) {
  if (ops_->gpu) {
    ops_->gpu->use(web_);
    ops_->gpu->path(p, toMatrix(web_.matrix), 2, s == "evenodd");
    narrowClip(web_, p, web_.matrix);
    if (!ops_->gpu->generic) return;
  }
  realSave();
  narrowClip(web_, p, web_.matrix);
  emitPathAt(p, 2, s == "evenodd");
}

// Hit tests are answered here, from the same geometry the native backend
// rasterizes: no round trip, and no flush of the op stream.
namespace {
bool webHitPath(const Path2D& p, const std::array<double, 6>& m, float x, float y, bool evenOdd) {
  canvas_detail::Poly poly;
  const canvas_detail::Matrix f = floatMatrix(m);
  canvas_detail::flatten(p, f, canvas_detail::matrixMaxScale(f), poly);
  return canvas_detail::pointInPoly(poly, x, y, evenOdd);
}
bool webHitStroke(const Path2D& p, const std::array<double, 6>& m, const Canvas::WebState& w, float x, float y) {
  canvas_detail::StrokeParams sp;
  sp.width = w.lineWidth; sp.miterLimit = w.miterLimit;
  sp.cap = w.lineCap == "round" ? 1 : w.lineCap == "square" ? 2 : 0;
  sp.join = w.lineJoin == "round" ? 1 : w.lineJoin == "bevel" ? 2 : 0;
  sp.dash = &w.dash; sp.dashOffset = w.dashOffset;
  canvas_detail::Poly flat, dashed, outline, device;
  canvas_detail::strokeToDevice(p.segments().data(), p.segments().size(), floatMatrix(m), sp, flat, dashed, outline, device);
  return canvas_detail::pointInPoly(device, x, y, false);
}
}  // namespace
bool Canvas::isPointInPath(float a, float b, const std::string& s) const { gpuSyncPath(); return webHitPath(currentPath_, pathUniform_ ? pathMatrix_ : web_.matrix, a, b, s == "evenodd"); }
bool Canvas::isPointInPath(const Path2D& p, float a, float b, const std::string& s) const { return webHitPath(p, web_.matrix, a, b, s == "evenodd"); }
bool Canvas::isPointInStroke(float a, float b) const { gpuSyncPath(); return webHitStroke(currentPath_, pathUniform_ ? pathMatrix_ : web_.matrix, web_, a, b); }
bool Canvas::isPointInStroke(const Path2D& p, float a, float b) const { return webHitStroke(p, web_.matrix, web_, a, b); }

// ----- text -----
namespace {
// A box no glyph run can escape: generous, because a wrong guess here would
// cull visible text, while too big a box only culls less.
void textBox(const Canvas::WebState& w, const std::string& text, float x, float y, float maxWidth, float box[4]) {
  canvas_detail::CssFont f;
  if (!canvas_detail::parseCssFont(w.font, f)) f.sizePx = 1000;
  std::size_t chars = 0;
  for (std::size_t i = 0; i < text.size(); ++i) if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++chars;
  float letter = 0, word = 0;
  canvas_detail::parseCssLength(w.letterSpacing, letter, f.sizePx);
  canvas_detail::parseCssLength(w.wordSpacing, word, f.sizePx);
  float width = static_cast<float>(chars) * (f.sizePx * 4 + std::abs(letter) + std::abs(word));
  if (maxWidth > 0) width = std::min(width, maxWidth);
  const float pad = f.sizePx * 3 + 4;
  canvas_detail::Box b;
  canvas_detail::addMappedRect(b, floatMatrix(w.matrix), x - width - pad, y - pad, x + width + pad, y + pad);
  if (b.empty()) { box[0] = box[1] = -1e30f; box[2] = box[3] = 1e30f; return; }
  box[0] = b.x0; box[1] = b.y0; box[2] = b.x1; box[3] = b.y1;
}
}  // namespace
void Canvas::fillText(const std::string& s, float a, float b, float c) {
  if (!finiteArgs({a, b}) || c == 0 || std::isnan(c)) return;
  GPU_DRAW(ops_->gpu->fillText(s, a, b, c))
  sync(kFillBits | kTextBits);
  float box[4];
  textBox(web_, s, a, b, c, box);
  { const PageFrame& f = frameFor(web_.matrix, kFitAffine | kFitTranslate); if (!f.same()) { a = static_cast<float>(a + f.e); b = static_cast<float>(b + f.f); } }
  OpSlot* r = opRecord(*this, BUF, 47);
  r[11].i = static_cast<std::int32_t>(BUF.text.size());
  BUF.text.append(s); BUF.text.push_back('\0');
  r[1].f = c; r[2].f = a; r[3].f = b;
  notePaint(3, box, false, nullptr);
}
void Canvas::strokeText(const std::string& s, float a, float b, float c) {
  if (!finiteArgs({a, b}) || c == 0 || std::isnan(c)) return;
  GPU_DRAW(false)
  sync(kStrokeBits | kTextBits);
  float box[4];
  textBox(web_, s, a, b, c, box);
  const float reach = web_.lineWidth * std::max(1.f, web_.miterLimit) * canvas_detail::matrixMaxScale(floatMatrix(web_.matrix));
  box[0] -= reach; box[1] -= reach; box[2] += reach; box[3] += reach;
  { const PageFrame& f = frameFor(web_.matrix, kFitAffine | kFitTranslate); if (!f.same()) { a = static_cast<float>(a + f.e); b = static_cast<float>(b + f.f); } }
  OpSlot* r = opRecord(*this, BUF, 48);
  r[11].i = static_cast<std::int32_t>(BUF.text.size());
  BUF.text.append(s); BUF.text.push_back('\0');
  r[1].f = c; r[2].f = a; r[3].f = b;
  notePaint(3, box, false, nullptr);
}
TextMetrics Canvas::measureTextMetrics(const std::string& s) const {
  float v[12] = {};
  c2d_measure(web_.font.c_str(), s.c_str(), web_.letterSpacing.c_str(), web_.wordSpacing.c_str(), web_.fontKerning.c_str(),
              web_.direction == "rtl" ? "rtl" : "ltr", v);
  TextMetrics m;
  m.width = v[0];
  // Measured left-aligned on the alphabetic baseline; the canvas's own
  // alignment and baseline shift the box the way the browser would.
  canvas_detail::CssFont f;
  const float size = canvas_detail::parseCssFont(web_.font, f) ? f.sizePx : 10.f;
  (void)size;
  const bool rtl = web_.direction == "rtl";
  std::string a = web_.textAlign;
  if (a == "start") a = rtl ? "right" : "left";
  if (a == "end") a = rtl ? "left" : "right";
  const float shift = a == "center" ? -m.width / 2 : a == "right" ? -m.width : 0.f;
  const float asc = v[5], desc = v[6], emA = v[7], emD = v[8];
  const std::string& bl = web_.textBaseline;
  // Where the alphabetic baseline sits below the line textBaseline names.
  const float base = bl == "top" ? emA : bl == "hanging" ? v[9] : bl == "middle" ? (emA - emD) / 2 : bl == "ideographic" ? v[11] : bl == "bottom" ? -emD : 0.f;
  m.actualBoundingBoxLeft = v[1] - shift;
  m.actualBoundingBoxRight = v[2] + shift;
  m.actualBoundingBoxAscent = v[3] - base;
  m.actualBoundingBoxDescent = v[4] + base;
  m.fontBoundingBoxAscent = asc - base;
  m.fontBoundingBoxDescent = desc + base;
  m.emHeightAscent = emA - base;
  m.emHeightDescent = emD + base;
  m.hangingBaseline = v[9] - base;
  m.alphabeticBaseline = -base;
  m.ideographicBaseline = v[11] - base;
  return m;
}
float Canvas::measureText(const std::string& s) const { return measureTextMetrics(s).width; }

// ----- images -----
namespace {
bool normaliseRects(float iw, float ih, float& sx, float& sy, float& sw, float& sh, float& dx, float& dy, float& dw, float& dh) {
  if (!finiteArgs({sx, sy, sw, sh, dx, dy, dw, dh})) return false;
  if (sw < 0) { sx += sw; sw = -sw; }
  if (sh < 0) { sy += sh; sh = -sh; }
  if (dw < 0) { dx += dw; dw = -dw; }
  if (dh < 0) { dy += dh; dh = -dh; }
  if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return false;
  const float kx = dw / sw, ky = dh / sh;
  if (sx < 0) { dx -= sx * kx; dw += sx * kx; sw += sx; sx = 0; }
  if (sy < 0) { dy -= sy * ky; dh += sy * ky; sh += sy; sy = 0; }
  if (sx + sw > iw) { const float over = sx + sw - iw; sw -= over; dw -= over * kx; }
  if (sy + sh > ih) { const float over = sy + sh - ih; sh -= over; dh -= over * ky; }
  return sw > 0 && sh > 0 && dw > 0 && dh > 0;
}
}  // namespace
void Canvas::drawCanvas(const Canvas& s, float a, float b) {
  drawCanvas(s, 0, 0, static_cast<float>(s.width_), static_cast<float>(s.height_), a, b, static_cast<float>(s.width_), static_cast<float>(s.height_));
}
void Canvas::drawCanvas(const Canvas& s, float a, float b, float c, float d) {
  drawCanvas(s, 0, 0, static_cast<float>(s.width_), static_cast<float>(s.height_), a, b, c, d);
}
void Canvas::drawCanvas(const Canvas& s, float sx, float sy, float sw, float sh, float a, float b, float c, float d) {
  if (s.contextId_ < 0 || !normaliseRects(static_cast<float>(s.width_), static_cast<float>(s.height_), sx, sy, sw, sh, a, b, c, d)) return;
  if (ops_->gpu && !s.ops_->gpu && opBuffer().owner == &s) const_cast<Canvas&>(s).flush();  // its pixels as they are now
  {
    const float f9[9] = {sx, sy, sw, sh, a, b, c, d, 1};
    GPU_DRAW(!s.ops_->gpu && ops_->gpu->drawCanvas(s.contextId_, f9))
    drawCanvasRecorded(s, sx, sy, sw, sh, a, b, c, d);
  }
}
void Canvas::drawCanvasRecorded(const Canvas& s, float sx, float sy, float sw, float sh, float a, float b, float c, float d) {
  // Drawing itself: what is buffered goes first, so that culling cannot drop
  // a paint the copy reads.
  if (&s == this) flush();
  // A GPU canvas's 2D surface has its pixels only when asked.
  else if (s.ops_->gpu) { const_cast<Canvas&>(s).flush(); s.ops_->gpu->syncShadow(); }
  sync(kImageBits);
  float box[4], inner[4];
  bool axis;
  rectBoxes(web_.matrix, a, b, c, d, box, inner, axis);
  { const PageFrame& f = frameFor(web_.matrix, kFitAffine | kFitAxis | kFitPositive); if (!f.same()) mapBox(f, a, b, c, d); }
  pushRef(*this, BUF, 52, s.contextId_, {sx, sy, sw, sh, a, b, c, d});
  notePaint(2, box, false, nullptr);
}
void Canvas::drawImage(const ImageLevel* levels, int levelCount, float dx, float dy, float dw, float dh, float alpha, std::uint32_t key) {
  if (!levels || levelCount <= 0) return;
  drawImage(levels[0].rgba, levels[0].width, levels[0].height, dx, dy, dw, dh, alpha, key);
}
void Canvas::drawImage(const std::uint8_t* rgba, int iw, int ih, float dx, float dy, float dw, float dh, float alpha, std::uint32_t key) {
  drawImage(rgba, iw, ih, 0, 0, static_cast<float>(iw), static_cast<float>(ih), dx, dy, dw, dh, alpha, key);
}
void Canvas::drawImage(const std::uint8_t* rgba, int iw, int ih, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
                       float alpha, std::uint32_t key) {
  if (!rgba || iw <= 0 || ih <= 0 || !(alpha > 0)) return;
  if (!normaliseRects(static_cast<float>(iw), static_cast<float>(ih), sx, sy, sw, sh, dx, dy, dw, dh)) return;
  const float f9[9] = {sx, sy, sw, sh, dx, dy, dw, dh, 1};
  GPU_DRAW(ops_->gpu->plain() && ops_->gpu->image(ops_->gpu->pixelTexture(rgba, iw, ih, key), iw, ih, f9, alpha, web_.smoothing))
  sync(kImageBits);
  // The pixels become a surface on the page once (per key), here and now:
  // uploading touches no context, so nothing buffered has to go first.
  static std::int32_t nextTemp = -1;
  std::int32_t ref;
  bool opaque = false;
  if (key) {
    ref = static_cast<std::int32_t>(key & 0x7fffffff);
    auto& keys = imageKeys();
    const auto found = keys.find(static_cast<std::uint32_t>(ref));
    if (found == keys.end() || found->second.w != iw || found->second.h != ih) {
      if (keys.size() >= 512) {
        // Evict the oldest -- after every buffer that might name it.
        flushAllBuffers();
        const ImageKey old = imageOrder().front();
        imageOrder().pop_front();
        keys.erase(old.key);
        c2d_image_drop(static_cast<int>(old.key));
      }
      bool op = true;
      for (std::size_t i = 0; i < static_cast<std::size_t>(iw) * ih && op; ++i) op = rgba[i * 4 + 3] == 255;
      c2d_image_upload(ref, rgba, iw, ih);
      const ImageKey k{static_cast<std::uint32_t>(ref), iw, ih, op};
      keys[k.key] = k;
      imageOrder().push_back(k);
      opaque = op;
    } else {
      opaque = found->second.opaque;
    }
  } else {
    ref = nextTemp--;
    if (nextTemp < -1000000000) nextTemp = -1;
    c2d_image_upload(ref, rgba, iw, ih);
  }
  float box[4], inner[4];
  bool axis;
  rectBoxes(web_.matrix, dx, dy, dw, dh, box, inner, axis);
  { const PageFrame& f = frameFor(web_.matrix, kFitAffine | kFitAxis | kFitPositive); if (!f.same()) mapBox(f, dx, dy, dw, dh); }
  OpSlot* r = pushRef(*this, BUF, 55, ref, {sx, sy, sw, sh, dx, dy, dw, dh, alpha});
  (void)r;
  if (ref < 0) lastMeta(BUF).tempImage = true;
  notePaint(2, box, axis && opaque && alpha >= 1.f, inner);
}
int canvasRequestRemoteImage(const std::string& url) { return c2d_remote_request(url.c_str()); }
int canvasRemoteImageStatus(int handle, int& width, int& height) {
  int size[2] = {0, 0};
  const int state = c2d_remote_status(handle, size);
  width = size[0]; height = size[1];
  return state;
}
void canvasReleaseRemoteImage(int handle) { canvasFlushOps(); c2d_remote_release(handle); }
void Canvas::drawRemoteImage(int handle, float dx, float dy, float dw, float dh) {
  if (handle <= 0 || dw <= 0 || dh <= 0) return;
  GPU_DRAW(false)
  sync(kImageBits);
  float box[4], inner[4];
  bool axis;
  rectBoxes(web_.matrix, dx, dy, dw, dh, box, inner, axis);
  { const PageFrame& f = frameFor(web_.matrix, kFitAffine | kFitAxis | kFitPositive); if (!f.same()) mapBox(f, dx, dy, dw, dh); }
  pushRef(*this, BUF, 54, handle, {dx, dy, dw, dh});
  notePaint(2, box, false, nullptr);
}
std::vector<std::uint8_t> Canvas::getImageData(int a, int b, int c, int d) const {
  std::vector<std::uint8_t> r(static_cast<std::size_t>(std::max(0, c)) * std::max(0, d) * 4);
  if (r.empty()) return r;
  const_cast<Canvas*>(this)->flush();
  if (ops_->gpu) return ops_->gpu->readPixels(a, b, c, d);
  c2d_get_pixels(contextId_, a, b, c, d, r.data());
  return r;
}
void Canvas::putImageData(const std::vector<std::uint8_t>& rgba, int sw, int sh, int dx, int dy) { putImageData(rgba, sw, sh, dx, dy, 0, 0, sw, sh); }
void Canvas::putImageData(const std::vector<std::uint8_t>& rgba, int sw, int sh, int dx, int dy, int dirtyX, int dirtyY, int dirtyW, int dirtyH) {
  if (sw <= 0 || sh <= 0 || rgba.size() < static_cast<std::size_t>(sw) * sh * 4) return;
  if (dirtyW < 0) { dirtyX += dirtyW; dirtyW = -dirtyW; }
  if (dirtyH < 0) { dirtyY += dirtyH; dirtyH = -dirtyH; }
  if (dirtyX < 0) { dirtyW += dirtyX; dirtyX = 0; }
  if (dirtyY < 0) { dirtyH += dirtyY; dirtyY = 0; }
  dirtyW = std::min(dirtyW, sw - dirtyX);
  dirtyH = std::min(dirtyH, sh - dirtyY);
  if (dirtyW <= 0 || dirtyH <= 0) return;
  if (ops_->gpu && !ops_->gpu->generic) {
    ops_->gpu->putImage(rgba.data() + (static_cast<std::size_t>(dirtyY) * sw + dirtyX) * 4, sw, dx + dirtyX, dy + dirtyY, dirtyW, dirtyH);
    ops_->gpu->shadowStale = true;
    return;
  }
  // Only the dirty rectangle rides in the buffer.
  OpSlot* r = pushRef(*this, BUF, 56, 0,
                      {static_cast<float>(dirtyW), static_cast<float>(dirtyH), static_cast<float>(dx + dirtyX), static_cast<float>(dy + dirtyY)});
  r[11].i = static_cast<std::int32_t>(BUF.bytes.size());
  for (int row = 0; row < dirtyH; ++row) {
    const std::uint8_t* from = rgba.data() + (static_cast<std::size_t>(dirtyY + row) * sw + dirtyX) * 4;
    BUF.bytes.insert(BUF.bytes.end(), from, from + static_cast<std::size_t>(dirtyW) * 4);
  }
  const float box[4] = {static_cast<float>(dx + dirtyX), static_cast<float>(dy + dirtyY), static_cast<float>(dx + dirtyX + dirtyW), static_cast<float>(dy + dirtyY + dirtyH)};
  notePaint(5, box, true, box);
}
void Canvas::fillCircle(float a, float b, float c) { beginPath(); arc(a, b, c, 0, 6.283185307f); fill(); }
void Canvas::strokeCircle(float a, float b, float c) { beginPath(); arc(a, b, c, 0, 6.283185307f); stroke(); }

// ----- layers -----
void Canvas::beginLayer() {
  // On a GPU canvas a layer is drawn on the shadow, begin to end.
  if (ops_->gpu) gpuGenericBegin();
  sync(kAllStateBits & ~kTransformBit);
  movePageTo(web_.matrix);
  save();
  // Like a real save on the page: the matching endLayer restores what the
  // page held, and inside the layer the four compositing fields start over.
  webStack_.back().real = true;
  appliedStack_.push_back(applied_);
  OP(59, {});
  lastMeta(BUF).kind = 0;
  ++layerDepth_;
  applied_.alpha = 1; applied_.composite = "source-over"; applied_.filter = "none";
  applied_.shadowColour = "#00000000"; applied_.shadowBlur = 0; applied_.shadowOffsetX = 0; applied_.shadowOffsetY = 0;
  web_.alpha = 1; web_.composite = "source-over"; web_.filter = "none";
  web_.shadowColour = "#00000000"; web_.shadowBlur = 0; web_.shadowOffsetX = 0; web_.shadowOffsetY = 0;
  dirty_ = kAllStateBits;
}
void Canvas::endLayer() {
  if (layerDepth_ <= 0 || webStack_.empty()) return;
  struct LayerEnd { Canvas& c; ~LayerEnd() { if (c.ops_->gpu) c.gpuGenericEnd(); } } layerEnd{*this};
  --layerDepth_;
  OP(60, {});
  // The composite of the layer: anywhere in the clip.
  const float box[4] = {-1e30f, -1e30f, 1e30f, 1e30f};
  WebFrame& frame = webStack_.back();
  frame.real = false;
  applied_ = std::move(appliedStack_.back());
  appliedStack_.pop_back();
  // Swapped, not moved: the frame keeps buffers for the next save to copy into.
  web_.swapWith(frame.state);
  webStack_.pop_back();
  dirty_ = kAllStateBits;
  notePaint(6, box, false, nullptr);
}

bool Canvas::savePPM(const std::string& path) const { (void)path; return true; }
bool Canvas::showWindow(const std::string& title, const std::function<void(Canvas&, float)>& drawFrame) { (void)title; (void)drawFrame; return true; }
