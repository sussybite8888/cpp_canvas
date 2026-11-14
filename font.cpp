#include "font.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#endif

namespace {

// Big-endian readers. TrueType is big-endian throughout; bounds are checked on
// every read because a font file is untrusted input.
std::uint8_t u8(const std::vector<std::uint8_t>& d, std::uint32_t at) {
  return at < d.size() ? d[at] : 0;
}
std::uint16_t u16(const std::vector<std::uint8_t>& d, std::uint32_t at) {
  return static_cast<std::uint16_t>((u8(d,at) << 8) | u8(d,at+1));
}
std::int16_t i16(const std::vector<std::uint8_t>& d, std::uint32_t at) {
  return static_cast<std::int16_t>(u16(d,at));
}
std::uint32_t u32(const std::vector<std::uint8_t>& d, std::uint32_t at) {
  return (static_cast<std::uint32_t>(u16(d,at)) << 16) | u16(d,at+2);
}
bool tagAt(const std::vector<std::uint8_t>& d, std::uint64_t at, const char* tag) {
  return at + 4 <= d.size() && std::memcmp(&d[static_cast<std::size_t>(at)], tag, 4) == 0;
}

// A clamp from 64-bit offset arithmetic back to the 32-bit offsets the readers
// take. Anything past the file reads as zeros, so saturating is always safe.
std::uint32_t clampOffset(std::uint64_t v) {
  return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v);
}

// The same readers over a borrowed byte range, for the tables a directory scan
// reads on their own rather than as part of a loaded file.
struct Span {
  const std::uint8_t* p = nullptr;
  std::size_t n = 0;
  std::uint8_t u8(std::size_t at) const { return at < n ? p[at] : 0; }
  std::uint16_t u16(std::size_t at) const { return static_cast<std::uint16_t>((u8(at) << 8) | u8(at + 1)); }
  std::int16_t i16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
};

// Decodes one UTF-8 codepoint, advancing `i`. Invalid bytes yield U+FFFD so a
// mangled string still renders something rather than desynchronising.
std::uint32_t decodeUtf8(const std::string& s, std::size_t& i) {
  const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char c = byte(i);
  if (c < 0x80) { ++i; return c; }
  const auto cont = [&](std::size_t k) { return k < s.size() && (byte(k) & 0xC0) == 0x80; };
  if ((c & 0xE0) == 0xC0 && cont(i+1)) { std::uint32_t v = ((c & 0x1Fu) << 6) | (byte(i+1) & 0x3Fu); i += 2; return v; }
  if ((c & 0xF0) == 0xE0 && cont(i+1) && cont(i+2)) { std::uint32_t v = ((c & 0x0Fu) << 12) | ((byte(i+1) & 0x3Fu) << 6) | (byte(i+2) & 0x3Fu); i += 3; return v; }
  if ((c & 0xF8) == 0xF0 && cont(i+1) && cont(i+2) && cont(i+3)) { std::uint32_t v = ((c & 0x07u) << 18) | ((byte(i+1) & 0x3Fu) << 12) | ((byte(i+2) & 0x3Fu) << 6) | (byte(i+3) & 0x3Fu); i += 4; return v; }
  ++i; return 0xFFFD;
}

void appendUtf8(std::string& out, std::uint32_t c) {
  if (c < 0x80) out += static_cast<char>(c);
  else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
  else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
}

// Mac OS Roman, 0x80..0xFF: old 'name' records and (1,0) cmaps use it.
const std::uint16_t kMacRoman[128] = {
  0x00C4,0x00C5,0x00C7,0x00C9,0x00D1,0x00D6,0x00DC,0x00E1,0x00E0,0x00E2,0x00E4,0x00E3,0x00E5,0x00E7,0x00E9,0x00E8,
  0x00EA,0x00EB,0x00ED,0x00EC,0x00EE,0x00EF,0x00F1,0x00F3,0x00F2,0x00F4,0x00F6,0x00F5,0x00FA,0x00F9,0x00FB,0x00FC,
  0x2020,0x00B0,0x00A2,0x00A3,0x00A7,0x2022,0x00B6,0x00DF,0x00AE,0x00A9,0x2122,0x00B4,0x00A8,0x2260,0x00C6,0x00D8,
  0x221E,0x00B1,0x2264,0x2265,0x00A5,0x00B5,0x2202,0x2211,0x220F,0x03C0,0x222B,0x00AA,0x00BA,0x03A9,0x00E6,0x00F8,
  0x00BF,0x00A1,0x00AC,0x221A,0x0192,0x2248,0x2206,0x00AB,0x00BB,0x2026,0x00A0,0x00C0,0x00C3,0x00D5,0x0152,0x0153,
  0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0x00F7,0x25CA,0x00FF,0x0178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
  0x2021,0x00B7,0x201A,0x201E,0x2030,0x00C2,0x00CA,0x00C1,0x00CB,0x00C8,0x00CD,0x00CE,0x00CF,0x00CC,0x00D3,0x00D4,
  0xF8FF,0x00D2,0x00DA,0x00DB,0x00D9,0x0131,0x02C6,0x02DC,0x00AF,0x02D8,0x02D9,0x02DA,0x00B8,0x02DD,0x02DB,0x02C7,
};

std::string lowerAscii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool endsWithFontExtension(const std::string& name) {
  const std::string n = lowerAscii(name);
  return n.size() > 4 && (n.compare(n.size()-4,4,".ttf") == 0 || n.compare(n.size()-4,4,".ttc") == 0 || n.compare(n.size()-4,4,".otf") == 0);
}

// ---- 'name', OS/2 and head: who a face is ---------------------------------

// The best record for `nameId`: Windows Unicode in US English, then Windows
// Unicode in any language, then the Unicode platform, then Mac Roman English.
std::string readName(Span name, std::uint16_t nameId) {
  const std::uint16_t count = name.u16(2);
  const std::size_t strings = name.u16(4);
  int bestScore = 0;
  std::size_t bestAt = 0, bestLength = 0;
  int bestKind = 0;    // 1 UTF-16BE, 2 Mac Roman
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t rec = 6 + i * 12;
    if (rec + 12 > name.n) break;
    if (name.u16(rec + 6) != nameId) continue;
    const std::uint16_t platform = name.u16(rec), encoding = name.u16(rec + 2), language = name.u16(rec + 4);
    int score = 0, kind = 0;
    if (platform == 3 && (encoding == 1 || encoding == 10)) { score = language == 0x409 ? 4 : 3; kind = 1; }
    else if (platform == 0) { score = 2; kind = 1; }
    else if (platform == 1 && encoding == 0 && language == 0) { score = 1; kind = 2; }
    if (score > bestScore) {
      bestScore = score; bestKind = kind;
      bestAt = strings + name.u16(rec + 10); bestLength = name.u16(rec + 8);
    }
  }
  std::string out;
  if (!bestScore || bestAt >= name.n) return out;
  bestLength = std::min(bestLength, name.n - bestAt);
  if (bestKind == 2) {
    for (std::size_t k = 0; k < bestLength; ++k) {
      const std::uint8_t c = name.u8(bestAt + k);
      appendUtf8(out, c < 0x80 ? c : kMacRoman[c - 0x80]);
    }
    return out;
  }
  for (std::size_t k = 0; k + 1 < bestLength; k += 2) {
    std::uint32_t c = name.u16(bestAt + k);
    if (c >= 0xD800 && c < 0xDC00 && k + 3 < bestLength) {
      const std::uint32_t lo = name.u16(bestAt + k + 2);
      if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); k += 2; }
    }
    if (c == 0) continue;
    appendUtf8(out, c);
  }
  return out;
}

struct FaceInfo {
  std::string family;     // typographic (16), else 1
  std::string family1;    // ID 1 as it stands
  std::string style;      // 17, else 2
  int weight = 400;
  int width = 5;
  bool italic = false;
  bool ok = false;
};

int weightFromStyle(const std::string& style) {
  std::string s = lowerAscii(style);
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), s.end());
  const auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
  if (has("thin") || has("hairline")) return 100;
  if (has("extralight") || has("ultralight")) return 200;
  if (has("light")) return 300;
  if (has("extrabold") || has("ultrabold")) return 800;
  if (has("semibold") || has("demibold") || has("demi")) return 600;
  if (has("bold")) return 700;
  if (has("black") || has("heavy")) return 900;
  if (has("medium")) return 500;
  return 400;
}

void describeFace(Span name, Span os2, Span head, FaceInfo& info) {
  info.family1 = readName(name, 1);
  info.family = readName(name, 16);
  if (info.family.empty()) info.family = info.family1;
  info.style = readName(name, 17);
  if (info.style.empty()) info.style = readName(name, 2);

  int weight = 0;
  bool italic = false;
  if (os2.n >= 64) {
    weight = os2.u16(4);
    if (weight >= 1 && weight <= 9) weight *= 100;     // a few old fonts use 1..9
    const int width = os2.u16(6);
    info.width = width >= 1 && width <= 9 ? width : 5;
    const std::uint16_t fsSelection = os2.u16(62);
    italic = (fsSelection & 0x0001) || (fsSelection & 0x0200);
  }
  if (weight < 1 || weight > 1000) weight = weightFromStyle(info.style);
  if (!italic && head.n >= 46) italic = (head.u16(44) & 0x0002) != 0;
  if (!italic) {
    const std::string s = lowerAscii(info.style);
    italic = s.find("italic") != std::string::npos || s.find("oblique") != std::string::npos;
  }
  info.weight = weight;
  info.italic = italic;
}

// ---- Geometry helpers -------------------------------------------------------

int pairCount(Path2D::Command c) {
  return c == Path2D::Command::Move || c == Path2D::Command::Line ? 1
       : c == Path2D::Command::Quadratic ? 2
       : c == Path2D::Command::Bezier ? 3 : 0;
}

// Tight ink box of a glyph path: on-curve points plus the curve extrema, so
// the box is what a rasterizer would touch rather than the control polygon.
void includeQuadExtrema(float p0, float p1, float p2, float q0, float q1, float q2,
                        float& x0, float& y0, float& x1, float& y1) {
  // Axis 1 is (p), axis 2 is (q); each is solved independently.
  const auto axis = [](float a, float b, float c, float t) { const float u = 1 - t; return u*u*a + 2*u*t*b + t*t*c; };
  const float dp = p0 - 2*p1 + p2;
  if (dp != 0) {
    const float t = (p0 - p1) / dp;
    if (t > 0 && t < 1) { const float v = axis(p0,p1,p2,t); x0 = std::min(x0, v); x1 = std::max(x1, v); }
  }
  const float dq = q0 - 2*q1 + q2;
  if (dq != 0) {
    const float t = (q0 - q1) / dq;
    if (t > 0 && t < 1) { const float v = axis(q0,q1,q2,t); y0 = std::min(y0, v); y1 = std::max(y1, v); }
  }
}

void cubicAxisExtrema(float a, float b, float c, float d, float& lo, float& hi) {
  const auto at = [&](float t) { const float u = 1 - t; return u*u*u*a + 3*u*u*t*b + 3*u*t*t*c + t*t*t*d; };
  const float d0 = b - a, d1 = c - b, d2 = d - c;
  const float A = d0 - 2*d1 + d2, B = 2*(d1 - d0), C = d0;
  const auto take = [&](float t) { if (t > 0 && t < 1) { const float v = at(t); lo = std::min(lo, v); hi = std::max(hi, v); } };
  if (std::fabs(A) < 1e-9f) {
    if (B != 0) take(-C / B);
    return;
  }
  const float disc = B*B - 4*A*C;
  if (disc < 0) return;
  const float r = std::sqrt(disc);
  take((-B + r) / (2*A));
  take((-B - r) / (2*A));
}

// ---- OpenType Layout common tables ------------------------------------------

// Index of `glyph` in a Coverage table, or -1.
int coverageIndex(const std::vector<std::uint8_t>& d, std::uint32_t cov, std::uint16_t glyph) {
  const std::uint16_t format = u16(d, cov);
  if (format == 1) {
    int lo = 0, hi = static_cast<int>(u16(d, cov + 2)) - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::uint16_t g = u16(d, cov + 4 + static_cast<std::uint32_t>(mid) * 2);
      if (g == glyph) return mid;
      if (g < glyph) lo = mid + 1; else hi = mid - 1;
    }
  } else if (format == 2) {
    int lo = 0, hi = static_cast<int>(u16(d, cov + 2)) - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::uint32_t rec = cov + 4 + static_cast<std::uint32_t>(mid) * 6;
      const std::uint16_t start = u16(d, rec), end = u16(d, rec + 2);
      if (glyph < start) hi = mid - 1;
      else if (glyph > end) lo = mid + 1;
      else return u16(d, rec + 4) + (glyph - start);
    }
  }
  return -1;
}

// Class of `glyph` in a ClassDef table; 0 for one it does not list.
std::uint16_t classOf(const std::vector<std::uint8_t>& d, std::uint32_t cd, std::uint16_t glyph) {
  const std::uint16_t format = u16(d, cd);
  if (format == 1) {
    const std::uint16_t start = u16(d, cd + 2), count = u16(d, cd + 4);
    if (glyph < start || glyph - start >= count) return 0;
    return u16(d, cd + 6 + (glyph - start) * 2u);
  }
  if (format == 2) {
    int lo = 0, hi = static_cast<int>(u16(d, cd + 2)) - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::uint32_t rec = cd + 4 + static_cast<std::uint32_t>(mid) * 6;
      const std::uint16_t start = u16(d, rec), end = u16(d, rec + 2);
      if (glyph < start) hi = mid - 1;
      else if (glyph > end) lo = mid + 1;
      else return u16(d, rec + 4);
    }
  }
  return 0;
}

int popcount8(std::uint16_t v) {
  int n = 0;
  for (int b = 0; b < 8; ++b) n += (v >> b) & 1;
  return n;
}

// One field of a GPOS ValueRecord, or 0 if the format omits it.
int valueField(const std::vector<std::uint8_t>& d, std::uint32_t rec, std::uint16_t format, std::uint16_t bit) {
  if (!(format & bit)) return 0;
  return i16(d, rec + 2u * static_cast<std::uint32_t>(popcount8(static_cast<std::uint16_t>(format & (bit - 1)))));
}

// An AAT lookup table (kerx class tables), formats 0, 2, 4, 6, 8 and 10.
// `found` is false for a glyph the table does not cover.
std::uint16_t aatLookup(const std::vector<std::uint8_t>& d, std::uint32_t t, std::uint32_t end,
                        std::uint16_t glyph, std::uint16_t numGlyphs, bool& found) {
  found = false;
  const std::uint16_t format = u16(d, t);
  switch (format) {
    case 0:
      if (glyph >= numGlyphs || t + 2 + glyph * 2u + 2 > end) return 0;
      found = true; return u16(d, t + 2 + glyph * 2u);
    case 2: case 4: case 6: {
      const std::uint16_t unitSize = u16(d, t + 2), units = u16(d, t + 4);
      const std::uint32_t first = t + 12;
      if (unitSize < (format == 6 ? 4 : 6)) return 0;
      int lo = 0, hi = static_cast<int>(units) - 1;
      while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const std::uint32_t rec = first + static_cast<std::uint32_t>(mid) * unitSize;
        if (rec + unitSize > end) { hi = mid - 1; continue; }
        if (format == 6) {
          const std::uint16_t g = u16(d, rec);
          if (g == glyph) { found = true; return u16(d, rec + 2); }
          if (g < glyph) lo = mid + 1; else hi = mid - 1;
        } else {
          const std::uint16_t last = u16(d, rec), firstGlyph = u16(d, rec + 2);
          if (glyph > last) lo = mid + 1;
          else if (glyph < firstGlyph) hi = mid - 1;
          else {
            if (last == 0xFFFF && firstGlyph == 0xFFFF) return 0;   // the terminator
            found = true;
            if (format == 2) return u16(d, rec + 4);
            return u16(d, t + u16(d, rec + 4) + (glyph - firstGlyph) * 2u);
          }
        }
      }
      return 0;
    }
    case 8: {
      const std::uint16_t first = u16(d, t + 2), count = u16(d, t + 4);
      if (glyph < first || glyph - first >= count) return 0;
      found = true; return u16(d, t + 6 + (glyph - first) * 2u);
    }
    case 10: {
      const std::uint16_t unitSize = u16(d, t + 2), first = u16(d, t + 4), count = u16(d, t + 6);
      if (glyph < first || glyph - first >= count || unitSize == 0 || unitSize > 4) return 0;
      const std::uint32_t at = t + 8 + (glyph - first) * static_cast<std::uint32_t>(unitSize);
      std::uint32_t v = 0;
      for (std::uint16_t k = 0; k < unitSize; ++k) v = (v << 8) | u8(d, at + k);
      found = true; return static_cast<std::uint16_t>(v);
    }
    default: return 0;
  }
}

constexpr std::uint32_t kBuilding = 0xFFFFFFFFu;
// A single glyph is capped at this many path segments, and the cache as a
// whole at kCacheBudget; both are far above anything a real face needs and
// exist so a hostile file cannot make a composite or a subroutine loop grow
// without bound.
constexpr std::size_t kMaxGlyphSegments = 1u << 16;
constexpr std::size_t kCacheBudget = 1u << 23;

} // namespace

// ============================================================================
// Loading

bool Font::loadFromFile(const std::string& path) { return loadFromFile(path, 0); }
bool Font::loadFromMemory(std::vector<std::uint8_t> bytes) { return loadFromMemory(std::move(bytes), 0); }

bool Font::loadFromFile(const std::string& path, int faceIndex) {
  // A file that cannot be read leaves the font as it was, as it always has.
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const std::streamsize size = in.tellg();
  if (size <= 0 || static_cast<std::uint64_t>(size) > 0xFFFFFFF0ull) return false;
  in.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return false;
  return loadFromMemory(std::move(bytes), faceIndex);
}

bool Font::loadFromMemory(std::vector<std::uint8_t> bytes, int faceIndex) {
  reset();
  data_ = std::move(bytes);
  // Offsets are 32-bit throughout; a larger buffer cannot be a font.
  if (data_.size() > 0xFFFFFFF0ull) { data_.clear(); return false; }
  valid_ = parse(faceIndex);
  if (valid_) faceIndex_ = faceIndex;
  return valid_;
}

void Font::reset() {
  const bool kerning = kerning_;
  *this = Font();
  kerning_ = kerning;   // a setting, not a property of the file
}

int Font::faceCount(const std::vector<std::uint8_t>& data) {
  if (data.size() < 12) return 0;
  const std::uint32_t tag = u32(data, 0);
  if (tag == 0x74746366) {   // 'ttcf'
    const std::uint32_t n = u32(data, 8);
    const std::uint64_t room = (data.size() - 12) / 4;
    return static_cast<int>(std::min<std::uint64_t>(std::min<std::uint64_t>(n, room), 0x7FFFFFFF));
  }
  if (tag == 0x00010000 || tag == 0x74727565 || tag == 0x4F54544F) return 1;
  return 0;
}

bool Font::parse(int faceIndex) {
  if (data_.size() < 12 || faceIndex < 0) return false;

  std::uint32_t base = 0;
  const std::uint32_t tag = u32(data_, 0);
  if (tag == 0x74746366) {          // 'ttcf': a collection
    if (faceIndex >= faceCount(data_)) return false;
    base = u32(data_, 12 + 4u * static_cast<std::uint32_t>(faceIndex));
    if (static_cast<std::uint64_t>(base) + 12 > data_.size()) return false;
  } else if (faceIndex != 0) {
    return false;
  }

  const std::uint32_t version = u32(data_, base);
  // 0x00010000 is TrueType outlines and 'true' the older Apple tag for them;
  // 'OTTO' is CFF outlines.
  if (version != 0x00010000 && version != 0x74727565 && version != 0x4F54544F) return false;

  const std::uint16_t numTables = u16(data_, base + 4);
  if (static_cast<std::uint64_t>(base) + 12 + numTables * 16ull > data_.size()) return false;

  for (std::uint16_t i = 0; i < numTables; ++i) {
    const std::uint32_t rec = base + 12 + i * 16u;
    char name[5] = {0};
    std::memcpy(name, &data_[rec], 4);
    const std::uint32_t offset = u32(data_, rec + 8);
    const std::uint32_t length = u32(data_, rec + 12);
    if (offset > data_.size()) continue;
    tables_[name] = {offset, std::min<std::uint32_t>(length, static_cast<std::uint32_t>(data_.size()) - offset)};
  }

  const std::uint32_t head = tableOffset("head");
  const std::uint32_t maxp = tableOffset("maxp");
  if (!head || !maxp) return false;

  unitsPerEm_ = u16(data_, head + 18);
  if (unitsPerEm_ == 0) unitsPerEm_ = 1000;
  indexToLocFormat_ = i16(data_, head + 50);
  numGlyphs_ = u16(data_, maxp + 4);

  parseMetricsTables();

  // Prefer a Unicode subtable: (3,10) and the Unicode platform's full
  // repertoire encodings, then (3,1) BMP, then the Unicode platform's BMP
  // ones; Windows Symbol and Mac Roman only as a last resort. Format 14 is
  // variation selectors, never a primary map.
  const std::uint32_t cmap = tableOffset("cmap");
  if (cmap) {
    const std::uint16_t count = u16(data_, cmap + 2);
    std::uint32_t best = 0;
    int bestScore = -1;
    std::uint8_t bestKind = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
      const std::uint32_t rec = cmap + 4 + i * 8u;
      const std::uint16_t platform = u16(data_, rec);
      const std::uint16_t encoding = u16(data_, rec + 2);
      const std::uint64_t offset64 = static_cast<std::uint64_t>(cmap) + u32(data_, rec + 4);
      if (offset64 + 4 > data_.size()) continue;
      const std::uint32_t offset = static_cast<std::uint32_t>(offset64);
      const std::uint16_t format = u16(data_, offset);
      if (format != 0 && format != 4 && format != 6 && format != 12 && format != 13) continue;
      int score = -1;
      std::uint8_t kind = 0;
      if (platform == 3 && encoding == 10) score = 6;
      else if (platform == 0 && (encoding == 4 || encoding == 6)) score = 5;
      else if (platform == 3 && encoding == 1) score = 4;
      else if (platform == 0) score = 3;
      else if (platform == 3 && encoding == 0) { score = 2; kind = 1; }
      else if (platform == 1 && encoding == 0) { score = 1; kind = 2; }
      if (score > bestScore) { bestScore = score; best = offset; bestKind = kind; }
    }
    if (bestScore >= 0) {
      cmapOffset_ = best;
      cmapFormat_ = u16(data_, best);
      cmapKind_ = bestKind;
    }
  }

  parseNames();

  if (numGlyphs_ == 0) return false;

  glyf_ = tableOffset("glyf");
  loca_ = tableOffset("loca");
  if (!(glyf_ && loca_)) {
    glyf_ = loca_ = 0;
    if (!parseCff()) return false;
  }

  parseKerning();
  slot_.assign(numGlyphs_, 0);
  return true;
}

void Font::parseMetricsTables() {
  const std::uint32_t hhea = tableOffset("hhea");
  if (hhea && tableLength("hhea") >= 36) {
    haveHhea_ = true;
    hheaAscender_ = ascent_ = i16(data_, hhea + 4);
    hheaDescender_ = descent_ = i16(data_, hhea + 6);
    hheaLineGap_ = lineGap_ = i16(data_, hhea + 8);
    numberOfHMetrics_ = u16(data_, hhea + 34);
  }
  hmtx_ = tableOffset("hmtx");
  hmtxEnd_ = hmtx_ + tableLength("hmtx");

  // Canvas2D's 'top', 'middle' and 'bottom' are edges of the EM BOX, and the
  // browser splits that box at the baseline in the ratio of the OS/2 sTypo
  // metrics -- not hhea's. For Ubuntu hhea says 932/-189 per 1000 em while
  // sTypo says 776/-185, and taking hhea drops every top-anchored string 1-3px.
  // Only the ratio is kept here; ascent() below does the normalising, and
  // lineHeight() goes back to hhea, which is a different box again.
  const std::uint32_t os2 = tableOffset("OS/2");
  const std::uint32_t os2Length = tableLength("OS/2");
  if (os2 && os2 + 72 <= data_.size()) {
    const std::int16_t typoAscender = i16(data_, os2 + 68);
    const std::int16_t typoDescender = i16(data_, os2 + 70);
    if (typoAscender > 0 && typoAscender > typoDescender) {
      ascent_ = typoAscender;
      descent_ = typoDescender;
    }
  }
  if (os2 && os2Length >= 78) {
    haveOs2_ = true;
    strikeoutSize_ = i16(data_, os2 + 26);
    strikeoutPosition_ = i16(data_, os2 + 28);
    useTypoMetrics_ = (u16(data_, os2 + 62) & 0x0080) != 0;
    typoAscender_ = i16(data_, os2 + 68);
    typoDescender_ = i16(data_, os2 + 70);
    typoLineGap_ = i16(data_, os2 + 72);
    winAscent_ = u16(data_, os2 + 74);
    winDescent_ = u16(data_, os2 + 76);
    if (u16(data_, os2) >= 2 && os2Length >= 90) {
      xHeight_ = i16(data_, os2 + 86);
      capHeight_ = i16(data_, os2 + 88);
    }
  }

  const std::uint32_t post = tableOffset("post");
  if (post && tableLength("post") >= 12) {
    havePost_ = true;
    underlinePosition_ = i16(data_, post + 8);
    underlineThickness_ = i16(data_, post + 10);
  }
}

void Font::parseNames() {
  const auto span = [&](const char* tag) {
    Span s;
    const auto it = tables_.find(tag);
    if (it != tables_.end()) { s.p = data_.data() + it->second.first; s.n = it->second.second; }
    return s;
  };
  FaceInfo info;
  describeFace(span("name"), span("OS/2"), span("head"), info);
  familyName_ = info.family;
  styleName_ = info.style;
  weight_ = info.weight;
  italic_ = info.italic;
}

std::uint32_t Font::tableOffset(const char* tag) const {
  auto it = tables_.find(tag);
  return it == tables_.end() ? 0 : it->second.first;
}

std::uint32_t Font::tableLength(const char* tag) const {
  auto it = tables_.find(tag);
  return it == tables_.end() ? 0 : it->second.second;
}

// ============================================================================
// cmap

std::uint16_t Font::glyphIndex(std::uint32_t codepoint) const {
  if (!cmapOffset_) return 0;
  if (cmapKind_ == 1) {
    // A symbol font's cmap sits in U+F000..F0FF; browsers reach it from
    // Latin-1 text, so try the code as given and then shifted there.
    const std::uint16_t direct = lookupCmap(codepoint);
    if (direct || codepoint > 0xFF) return direct;
    return lookupCmap(0xF000 + codepoint);
  }
  if (cmapKind_ == 2) {
    if (codepoint < 0x80) return lookupCmap(codepoint);
    for (std::uint32_t k = 0; k < 128; ++k)
      if (kMacRoman[k] == codepoint) return lookupCmap(0x80 + k);
    return 0;
  }
  return lookupCmap(codepoint);
}

std::uint16_t Font::lookupCmap(std::uint32_t codepoint) const {
  if (cmapFormat_ == 4) {
    if (codepoint > 0xFFFF) return 0;
    const std::uint16_t segX2 = u16(data_, cmapOffset_ + 6);
    if (segX2 < 2) return 0;
    const std::uint32_t ends = cmapOffset_ + 14;
    const std::uint32_t starts = ends + segX2 + 2;
    const std::uint32_t deltas = starts + segX2;
    const std::uint32_t ranges = deltas + segX2;

    // The first segment whose end is at or past the codepoint (segments are
    // sorted by end).
    int lo = 0, hi = segX2 / 2 - 1, found = -1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      if (u16(data_, ends + static_cast<std::uint32_t>(mid) * 2) >= codepoint) { found = mid; hi = mid - 1; }
      else lo = mid + 1;
    }
    if (found < 0) return 0;
    const std::uint32_t s = static_cast<std::uint32_t>(found) * 2;
    const std::uint16_t start = u16(data_, starts + s);
    if (codepoint < start) return 0;
    const std::uint16_t rangeOffset = u16(data_, ranges + s);
    if (rangeOffset == 0) {
      // idDelta arithmetic is modulo 65536.
      return static_cast<std::uint16_t>(codepoint + u16(data_, deltas + s));
    }
    // The idRangeOffset indirection is relative to its own slot, which is
    // the one genuinely strange thing in this table.
    const std::uint32_t at = ranges + s + rangeOffset + (codepoint - start) * 2u;
    const std::uint16_t index = u16(data_, at);
    if (index == 0) return 0;
    return static_cast<std::uint16_t>(index + i16(data_, deltas + s));
  }

  if (cmapFormat_ == 12 || cmapFormat_ == 13) {
    std::uint32_t groups = u32(data_, cmapOffset_ + 12);
    groups = std::min<std::uint32_t>(groups, static_cast<std::uint32_t>((data_.size() - std::min<std::size_t>(data_.size(), cmapOffset_ + 16u)) / 12));
    std::int64_t lo = 0, hi = static_cast<std::int64_t>(groups) - 1;
    while (lo <= hi) {
      const std::int64_t mid = (lo + hi) / 2;
      const std::uint32_t rec = cmapOffset_ + 16 + static_cast<std::uint32_t>(mid) * 12u;
      const std::uint32_t start = u32(data_, rec);
      const std::uint32_t end = u32(data_, rec + 4);
      if (codepoint < start) hi = mid - 1;
      else if (codepoint > end) lo = mid + 1;
      else if (cmapFormat_ == 13) return static_cast<std::uint16_t>(u32(data_, rec + 8));
      else return static_cast<std::uint16_t>(u32(data_, rec + 8) + (codepoint - start));
    }
    return 0;
  }

  if (cmapFormat_ == 6) {
    const std::uint16_t first = u16(data_, cmapOffset_ + 6);
    const std::uint16_t count = u16(data_, cmapOffset_ + 8);
    if (codepoint < first || codepoint >= static_cast<std::uint32_t>(first) + count) return 0;
    return u16(data_, cmapOffset_ + 10 + (codepoint - first) * 2u);
  }

  if (cmapFormat_ == 0) {
    if (codepoint > 255) return 0;
    return u8(data_, cmapOffset_ + 6 + codepoint);
  }

  return 0;
}

// ============================================================================
// Glyph cache and TrueType outlines

std::uint32_t Font::glyphOffset(std::uint32_t index) const {
  // loca has numGlyphs + 1 entries: the last glyph ends where the extra one says.
  if (!loca_ || index > numGlyphs_) return 0;
  if (indexToLocFormat_ == 0) return u16(data_, loca_ + index * 2u) * 2u;
  return u32(data_, loca_ + index * 4u);
}

std::int16_t Font::leftSideBearing(std::uint16_t index) const {
  if (!hmtx_ || numberOfHMetrics_ == 0) return 0;
  // Past numberOfHMetrics the side bearings continue as a bare array.
  if (index < numberOfHMetrics_) return i16(data_, hmtx_ + index * 4u + 2);
  return i16(data_, hmtx_ + numberOfHMetrics_ * 4u + (index - numberOfHMetrics_) * 2u);
}

std::uint16_t Font::advanceWidth(std::uint16_t index) const {
  if (!hmtx_ || numberOfHMetrics_ == 0) return unitsPerEm_ / 2;
  // Past numberOfHMetrics the advance is the last one; only the side bearing
  // continues to vary. Monospace fonts rely on this to store one metric.
  const std::uint16_t clamped = std::min<std::uint16_t>(index, numberOfHMetrics_ - 1);
  return u16(data_, hmtx_ + clamped * 4u);
}

const Font::Glyph& Font::glyph(std::uint16_t index) const {
  static const Glyph kEmpty;
  if (index < slot_.size()) {
    const std::uint32_t s = slot_[index];
    if (s == kBuilding) return kEmpty;        // a composite that contains itself
    if (s) return cache_[s - 1];
  }
  // Composites nest through here; past a few levels it is a hostile file.
  if (buildDepth_ > 8) return kEmpty;
  if (index < slot_.size()) slot_[index] = kBuilding;
  ++buildDepth_;
  Glyph g;
  buildGlyph(index, g);
  --buildDepth_;
  const std::size_t segments = static_cast<const Path2D&>(g.path).segments().size();
  if (index >= slot_.size() || cachedSegments_ + segments > kCacheBudget) {
    // Out of range (a cmap pointing past maxp), or the cache is full: hand
    // back a scratch copy that lives until the next glyph decoded this way.
    if (index < slot_.size()) slot_[index] = 0;
    scratch_ = std::move(g);
    return scratch_;
  }
  cachedSegments_ += segments;
  cache_.push_back(std::move(g));
  slot_[index] = static_cast<std::uint32_t>(cache_.size());
  return cache_.back();
}

void Font::buildGlyph(std::uint16_t index, Glyph& out) const {
  out.advance = advanceWidth(index);
  out.loaded = true;

  if (cff_.charStrings) {
    buildCffGlyph(index, out);
  } else {
    const std::uint32_t start = glyphOffset(index);
    const std::uint32_t end = glyphOffset(index + 1u);
    // An empty range is a blank glyph (a space), not an error.
    if (glyf_ && end > start && static_cast<std::uint64_t>(glyf_) + end <= data_.size()) {
      const std::uint32_t at = glyf_ + start;
      const std::int16_t contours = i16(data_, at);
      if (contours >= 0) buildSimpleGlyph(at, end - start, out);
      else buildCompositeGlyph(at, end - start, out, 0);
      // TrueType puts the origin at xMin - lsb (the first phantom point), so
      // when hmtx's side bearing disagrees with a simple glyph's xMin the
      // outline moves to match hmtx -- as CoreText draws it (ArialHB's Hebrew
      // points sit 23 units left of their coordinates). Not for composites,
      // which CoreText leaves where their components put them (Noto Sans
      // Kannada), and a component is placed unshifted: composite building
      // undoes this.
      const float shift = contours >= 0 ? static_cast<float>(leftSideBearing(index) - i16(data_, at + 2)) : 0.f;
      if (shift != 0 && !out.path.empty()) {
        out.shift = shift;
        for (Path2D::Segment& seg : out.path.segments()) {
          const int pairs = pairCount(seg.command);
          for (int k = 0; k < pairs; ++k) seg.v[k * 2] += shift;
        }
      }
    }
  }

  // The ink box, y down.
  const auto& segs = static_cast<const Path2D&>(out.path).segments();
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool any = false;
  float cx = 0, cy = 0;
  const auto include = [&](float x, float y) {
    if (!any) { x0 = x1 = x; y0 = y1 = y; any = true; return; }
    x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
  };
  for (const Path2D::Segment& s : segs) {
    switch (s.command) {
      case Path2D::Command::Move: cx = s.v[0]; cy = s.v[1]; break;
      case Path2D::Command::Line:
        include(cx, cy); include(s.v[0], s.v[1]); cx = s.v[0]; cy = s.v[1]; break;
      case Path2D::Command::Quadratic:
        include(cx, cy); include(s.v[2], s.v[3]);
        includeQuadExtrema(cx, s.v[0], s.v[2], cy, s.v[1], s.v[3], x0, y0, x1, y1);
        cx = s.v[2]; cy = s.v[3]; break;
      case Path2D::Command::Bezier:
        include(cx, cy); include(s.v[4], s.v[5]);
        cubicAxisExtrema(cx, s.v[0], s.v[2], s.v[4], x0, x1);
        cubicAxisExtrema(cy, s.v[1], s.v[3], s.v[5], y0, y1);
        cx = s.v[4]; cy = s.v[5]; break;
      default: break;
    }
  }
  out.ink = any && (x1 > x0 || y1 > y0);
  if (out.ink) { out.xMin = x0; out.yMin = y0; out.xMax = x1; out.yMax = y1; }
}

void Font::buildSimpleGlyph(std::uint32_t at, std::uint32_t length, Glyph& out) const {
  const std::int16_t contourCount = i16(data_, at);
  if (contourCount <= 0) return;
  const std::uint64_t limit = static_cast<std::uint64_t>(at) + length;

  std::uint32_t p = at + 10;
  std::vector<std::uint16_t> contourEnds(static_cast<std::size_t>(contourCount));
  for (int i = 0; i < contourCount; ++i) { contourEnds[static_cast<std::size_t>(i)] = u16(data_, p); p += 2; }

  const std::uint32_t pointCount = contourEnds.back() + 1u;
  if (pointCount > 10000) return;   // implausible; refuse rather than allocate

  const std::uint16_t instructionLength = u16(data_, p);
  p += 2 + instructionLength;

  std::vector<std::uint8_t> flags;
  flags.reserve(pointCount);
  while (flags.size() < pointCount && p < limit) {
    const std::uint8_t flag = u8(data_, p++);
    flags.push_back(flag);
    if (flag & 0x08) {                       // repeat
      std::uint8_t repeat = u8(data_, p++);
      while (repeat-- && flags.size() < pointCount) flags.push_back(flag);
    }
  }
  if (flags.size() < pointCount) return;

  std::vector<int> xs(pointCount), ys(pointCount);
  int value = 0;
  for (std::uint32_t i = 0; i < pointCount; ++i) {
    const std::uint8_t flag = flags[i];
    if (flag & 0x02) { const std::uint8_t d = u8(data_, p++); value += (flag & 0x10) ? d : -d; }
    else if (!(flag & 0x10)) { value += i16(data_, p); p += 2; }
    xs[i] = value;
  }
  value = 0;
  for (std::uint32_t i = 0; i < pointCount; ++i) {
    const std::uint8_t flag = flags[i];
    if (flag & 0x04) { const std::uint8_t d = u8(data_, p++); value += (flag & 0x20) ? d : -d; }
    else if (!(flag & 0x20)) { value += i16(data_, p); p += 2; }
    ys[i] = value;
  }

  // Font space is y-up; canvas space is y-down. Flipping here means every
  // consumer downstream works in ordinary screen coordinates.
  const auto px = [&](std::uint32_t i) { return static_cast<float>(xs[i]); };
  const auto py = [&](std::uint32_t i) { return static_cast<float>(-ys[i]); };
  const auto onCurve = [&](std::uint32_t i) { return (flags[i] & 0x01) != 0; };

  std::uint32_t first = 0;
  for (int c = 0; c < contourCount; ++c) {
    const std::uint32_t last = contourEnds[static_cast<std::size_t>(c)];
    if (last < first || last >= pointCount) break;
    const std::uint32_t count = last - first + 1;
    if (count < 2) { first = last + 1; continue; }

    // A contour may begin on an off-curve point, in which case the true start
    // is the implied midpoint between it and the last point.
    float startX, startY;
    std::uint32_t begin;
    if (onCurve(first)) {
      startX = px(first); startY = py(first); begin = first + 1;
    } else if (onCurve(last)) {
      startX = px(last); startY = py(last); begin = first;
    } else {
      startX = (px(first) + px(last)) * 0.5f;
      startY = (py(first) + py(last)) * 0.5f;
      begin = first;
    }
    out.path.moveTo(startX, startY);

    float controlX = 0, controlY = 0;
    bool haveControl = false;
    for (std::uint32_t k = 0; k < count; ++k) {
      const std::uint32_t i = first + ((begin - first) + k) % count;
      if (onCurve(i)) {
        if (haveControl) { out.path.quadraticCurveTo(controlX, controlY, px(i), py(i)); haveControl = false; }
        else out.path.lineTo(px(i), py(i));
      } else {
        if (haveControl) {
          // Two consecutive controls imply an on-curve point midway between.
          const float midX = (controlX + px(i)) * 0.5f;
          const float midY = (controlY + py(i)) * 0.5f;
          out.path.quadraticCurveTo(controlX, controlY, midX, midY);
        }
        controlX = px(i); controlY = py(i); haveControl = true;
      }
    }
    if (haveControl) out.path.quadraticCurveTo(controlX, controlY, startX, startY);
    out.path.closePath();

    first = last + 1;
  }
}

void Font::buildCompositeGlyph(std::uint32_t at, std::uint32_t length, Glyph& out, int depth) const {
  if (depth > 5) return;   // composites can nest; bound it rather than recurse forever

  const std::uint64_t limit = static_cast<std::uint64_t>(at) + length;
  std::uint32_t p = at + 10;
  std::vector<Path2D::Segment>& dest = out.path.segments();
  for (int component = 0; component < 4096 && p + 4ull <= limit; ++component) {
    const std::uint16_t flags = u16(data_, p);
    const std::uint16_t glyphIdx = u16(data_, p + 2);
    p += 4;

    float dx = 0, dy = 0;
    if (flags & 0x0001) {                       // ARG_1_AND_2_ARE_WORDS
      dx = i16(data_, p); dy = i16(data_, p + 2); p += 4;
    } else {
      dx = static_cast<std::int8_t>(u8(data_, p));
      dy = static_cast<std::int8_t>(u8(data_, p + 1));
      p += 2;
    }
    // Args can be point indices rather than offsets; that form is vanishingly
    // rare in text fonts and is treated as no offset rather than misplacing it.
    if (!(flags & 0x0002)) { dx = 0; dy = 0; }

    float a = 1, b = 0, c = 0, d = 1;
    const auto f2dot14 = [&](std::uint32_t o) { return i16(data_, o) / 16384.0f; };
    if (flags & 0x0008) { a = d = f2dot14(p); p += 2; }
    else if (flags & 0x0040) { a = f2dot14(p); d = f2dot14(p + 2); p += 4; }
    else if (flags & 0x0080) { a = f2dot14(p); b = f2dot14(p + 2); c = f2dot14(p + 4); d = f2dot14(p + 6); p += 8; }

    const Glyph& part = glyph(glyphIdx);
    const auto& partSegments = static_cast<const Path2D&>(part.path).segments();
    if (dest.size() + partSegments.size() > kMaxGlyphSegments) break;
    // The component's outline is already y-flipped, so the offset must be too,
    // and the b/c shear terms change sign with it: with x = X, y = -Y,
    // X' = a*X - c*Y + dx and Y' = -b*X + d*Y - dy.
    for (const Path2D::Segment& s : partSegments) {
      Path2D::Segment moved = s;
      const int pairs = pairCount(s.command);
      for (int i = 0; i < pairs; ++i) {
        const float x = s.v[i * 2] - part.shift, y = s.v[i * 2 + 1];
        moved.v[i * 2]     = a * x - c * y + dx;
        moved.v[i * 2 + 1] = -b * x + d * y - dy;
      }
      dest.push_back(moved);
    }
    // MORE_COMPONENTS. What follows the last component is its instructions
    // and the glyph's padding, which read as components would draw stray
    // glyphs over this one -- accented letters grew shapes thousands of units
    // away.
    if (!(flags & 0x0020)) break;
  }
}

// ============================================================================
// CFF

namespace {

std::uint32_t readOffSize(const std::vector<std::uint8_t>& d, std::uint32_t at, std::uint8_t size) {
  std::uint32_t v = 0;
  for (std::uint8_t k = 0; k < size; ++k) v = (v << 8) | u8(d, at + k);
  return v;
}

// A DICT, operator by operator. Operands are numbers (integers or reals);
// `onOperator` sees each operator (12 x escapes as 1200 + x) with its operands.
template <class F>
void parseDict(const std::vector<std::uint8_t>& d, std::uint32_t p, std::uint32_t end, F&& onOperator) {
  double operands[48];
  int n = 0;
  end = std::min<std::uint32_t>(end, static_cast<std::uint32_t>(d.size()));
  while (p < end) {
    const std::uint8_t b0 = d[p];
    if (b0 <= 21) {
      int op = b0;
      ++p;
      if (b0 == 12) { op = 1200 + u8(d, p); ++p; }
      onOperator(op, operands, n);
      n = 0;
      continue;
    }
    double v = 0;
    if (b0 == 28) { v = i16(d, p + 1); p += 3; }
    else if (b0 == 29) { v = static_cast<std::int32_t>(u32(d, p + 1)); p += 5; }
    else if (b0 == 30) {
      // A real, in nibbles: digits, '.', 'E', 'E-', '-', end. Parsed by hand
      // because strtod would follow the C locale's decimal point.
      double mantissa = 0, scale = 1;
      int exponent = 0, expSign = 1;
      bool negative = false, fraction = false, inExponent = false, done = false;
      ++p;
      for (int guard = 0; p < end && !done && guard < 64; ++guard, ++p) {
        const std::uint8_t byte = d[p];
        for (int half = 0; half < 2 && !done; ++half) {
          const int nib = half == 0 ? byte >> 4 : byte & 0xF;
          if (nib <= 9) {
            if (inExponent) { if (exponent < 1000) exponent = exponent * 10 + nib; }
            else if (fraction) { scale /= 10; mantissa += nib * scale; }
            else mantissa = mantissa * 10 + nib;
          }
          else if (nib == 0xA) fraction = true;
          else if (nib == 0xB) { inExponent = true; expSign = 1; }
          else if (nib == 0xC) { inExponent = true; expSign = -1; }
          else if (nib == 0xE) negative = true;
          else if (nib == 0xF) done = true;
        }
      }
      v = mantissa * std::pow(10.0, expSign * exponent);
      if (negative) v = -v;
      if (!std::isfinite(v)) v = 0;
    }
    else if (b0 >= 32 && b0 <= 246) { v = b0 - 139; p += 1; }
    else if (b0 >= 247 && b0 <= 250) { v = (b0 - 247) * 256 + u8(d, p + 1) + 108; p += 2; }
    else if (b0 >= 251 && b0 <= 254) { v = -(b0 - 251) * 256 - u8(d, p + 1) - 108; p += 2; }
    else { ++p; continue; }            // reserved
    if (n < 48) operands[n++] = v;
  }
}

int subrBias(std::uint32_t count) {
  return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

// Operands that would make a DICT offset negative or absurd read as "absent".
std::uint32_t dictOffset(double v) {
  return v >= 0 && v < 4294967295.0 ? static_cast<std::uint32_t>(v) : 0;
}

} // namespace

bool Font::readCffIndex(std::uint32_t at, CffIndex& out, std::uint32_t& next) const {
  out = CffIndex();
  if (static_cast<std::uint64_t>(at) + 2 > cff_.end) return false;
  out.count = u16(data_, at);
  if (out.count == 0) { next = at + 2; return true; }
  out.offSize = u8(data_, at + 2);
  if (out.offSize < 1 || out.offSize > 4) return false;
  out.offsets = at + 3;
  const std::uint64_t dataStart = static_cast<std::uint64_t>(out.offsets) + (out.count + 1ull) * out.offSize;
  if (dataStart > cff_.end) return false;
  out.data = static_cast<std::uint32_t>(dataStart - 1);     // offsets count from 1
  const std::uint64_t end = static_cast<std::uint64_t>(out.data) + readOffSize(data_, out.offsets + out.count * out.offSize, out.offSize);
  next = clampOffset(std::min<std::uint64_t>(end, cff_.end));
  return true;
}

bool Font::cffItem(const CffIndex& index, std::uint32_t i, std::uint32_t& start, std::uint32_t& end) const {
  if (i >= index.count) return false;
  const std::uint32_t a = readOffSize(data_, index.offsets + i * index.offSize, index.offSize);
  const std::uint32_t b = readOffSize(data_, index.offsets + (i + 1) * index.offSize, index.offSize);
  if (a < 1 || b < a) return false;
  const std::uint64_t s = static_cast<std::uint64_t>(index.data) + a;
  const std::uint64_t e = static_cast<std::uint64_t>(index.data) + b;
  if (e > cff_.end) return false;
  start = static_cast<std::uint32_t>(s);
  end = static_cast<std::uint32_t>(e);
  return true;
}

bool Font::parseCff() {
  const std::uint32_t off = tableOffset("CFF ");
  const std::uint32_t len = tableLength("CFF ");
  if (!off || len < 4) return false;
  cff_ = Cff();
  cff_.base = off;
  cff_.end = off + len;
  if (u8(data_, off) != 1) return false;      // major version 1 (CFF2 is not read)

  std::uint32_t p = off + u8(data_, off + 2);
  CffIndex names, topDicts, strings;
  if (!readCffIndex(p, names, p) || !readCffIndex(p, topDicts, p) || !readCffIndex(p, strings, p)) return false;
  if (!readCffIndex(p, cff_.globalSubrs, p)) cff_.globalSubrs = CffIndex();

  std::uint32_t topStart = 0, topEnd = 0;
  if (!cffItem(topDicts, 0, topStart, topEnd)) return false;

  std::uint32_t charStrings = 0, privateSize = 0, privateOffset = 0, fdArray = 0, fdSelect = 0;
  int charstringType = 2;
  double fontMatrix = 0;
  bool cid = false;
  parseDict(data_, topStart, topEnd, [&](int op, const double* v, int n) {
    if (op == 17 && n >= 1) charStrings = dictOffset(v[0]);
    else if (op == 18 && n >= 2) { privateSize = dictOffset(v[0]); privateOffset = dictOffset(v[1]); }
    else if (op == 1206 && n >= 1) charstringType = static_cast<int>(v[0]);
    else if (op == 1207 && n >= 1) fontMatrix = v[0];
    else if (op == 1230) cid = true;
    else if (op == 1236 && n >= 1) fdArray = dictOffset(v[0]);
    else if (op == 1237 && n >= 1) fdSelect = dictOffset(v[0]);
  });
  if (charstringType != 2 || !charStrings) return false;
  std::uint32_t next = 0;
  if (!readCffIndex(clampOffset(static_cast<std::uint64_t>(off) + charStrings), cff_.charStringIndex, next)) return false;
  if (cff_.charStringIndex.count == 0) return false;

  // A Private DICT's local subroutines, which it locates relative to itself.
  const auto localSubrs = [&](std::uint32_t size, std::uint32_t offset) {
    CffIndex subrs;
    if (!offset || !size) return subrs;
    const std::uint64_t start = static_cast<std::uint64_t>(off) + offset;
    if (start + size > cff_.end) return subrs;
    std::uint32_t subrsOffset = 0;
    parseDict(data_, static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(start + size),
              [&](int op, const double* v, int n) { if (op == 19 && n >= 1) subrsOffset = dictOffset(v[0]); });
    std::uint32_t after = 0;
    if (subrsOffset && !readCffIndex(clampOffset(start + subrsOffset), subrs, after)) subrs = CffIndex();
    return subrs;
  };

  if (cid && fdArray) {
    CffIndex fonts;
    if (readCffIndex(clampOffset(static_cast<std::uint64_t>(off) + fdArray), fonts, next)) {
      const std::uint32_t count = std::min<std::uint32_t>(fonts.count, 256);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t s = 0, e = 0, size = 0, offset = 0;
        if (cffItem(fonts, i, s, e))
          parseDict(data_, s, e, [&](int op, const double* v, int n) {
            if (op == 18 && n >= 2) { size = dictOffset(v[0]); offset = dictOffset(v[1]); }
          });
        cff_.fdSubrs.push_back(localSubrs(size, offset));
      }
    }
    if (fdSelect) {
      const std::uint64_t at = static_cast<std::uint64_t>(off) + fdSelect;
      if (at < cff_.end) {
        const std::uint8_t format = u8(data_, static_cast<std::uint32_t>(at));
        if (format == 0 || format == 3) { cff_.fdSelect = static_cast<std::uint32_t>(at); cff_.fdSelectFormat = format; }
      }
    }
  } else {
    cff_.localSubrs = localSubrs(privateSize, privateOffset);
  }

  // Charstrings are in FontMatrix units; the usual 1/1000 matches a 1000
  // unit head.unitsPerEm and needs nothing, anything else is rescaled here.
  if (fontMatrix > 0 && fontMatrix < 1) {
    const double scale = fontMatrix * unitsPerEm_;
    if (std::fabs(scale - 1) > 1e-4) cff_.scale = static_cast<float>(scale);
  }
  cff_.charStrings = off + charStrings;
  return true;
}

struct Font::CharstringState {
  double stack[48];
  int sp = 0;
  double transient[32] = {};
  int stems = 0;
  bool widthDone = false;
  double x = 0, y = 0;
  bool open = false;
  long ops = 0;
  const CffIndex* local = nullptr;
  Path2D* path = nullptr;
  std::vector<Path2D::Segment>* segs = nullptr;
  float scale = 1;
  bool failed = false;

  bool point(double dx, double dy) {
    x += dx; y += dy;
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 1e6 || std::fabs(y) > 1e6) { failed = true; return false; }
    if (segs->size() >= kMaxGlyphSegments) { failed = true; return false; }
    return true;
  }
  float fx() const { return static_cast<float>(x) * scale; }
  float fy() const { return static_cast<float>(-y) * scale; }
  void close() { if (open) { path->closePath(); open = false; } }
  void moveTo(double dx, double dy) {
    close();
    if (!point(dx, dy)) return;
    path->moveTo(fx(), fy()); open = true;
  }
  void lineTo(double dx, double dy) {
    if (!open) { path->moveTo(fx(), fy()); open = true; }
    if (!point(dx, dy)) return;
    path->lineTo(fx(), fy());
  }
  void curveTo(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
    if (!open) { path->moveTo(fx(), fy()); open = true; }
    if (!point(dx1, dy1)) return;
    const float ax = fx(), ay = fy();
    if (!point(dx2, dy2)) return;
    const float bx = fx(), by = fy();
    if (!point(dx3, dy3)) return;
    path->bezierCurveTo(ax, ay, bx, by, fx(), fy());
  }
};

void Font::buildCffGlyph(std::uint16_t index, Glyph& out) const {
  std::uint32_t start = 0, end = 0;
  if (!cffItem(cff_.charStringIndex, index, start, end)) return;
  CharstringState st;
  st.path = &out.path;
  st.segs = &out.path.segments();
  st.scale = cff_.scale;
  static const CffIndex kNone;
  st.local = &cff_.localSubrs;
  if (cff_.fdSelect) {
    int fd = -1;
    if (cff_.fdSelectFormat == 0) {
      fd = u8(data_, cff_.fdSelect + 1 + index);
    } else {
      const std::uint16_t ranges = u16(data_, cff_.fdSelect + 1);
      int lo = 0, hi = static_cast<int>(ranges) - 1;
      while (lo <= hi) {       // the last range whose first glyph is <= index
        const int mid = (lo + hi) / 2;
        const std::uint32_t rec = cff_.fdSelect + 3 + static_cast<std::uint32_t>(mid) * 3;
        if (u16(data_, rec) <= index) { fd = u8(data_, rec + 2); lo = mid + 1; }
        else hi = mid - 1;
      }
    }
    st.local = fd >= 0 && static_cast<std::size_t>(fd) < cff_.fdSubrs.size() ? &cff_.fdSubrs[static_cast<std::size_t>(fd)] : &kNone;
  }
  const int result = runCharstring(st, start, end, 0);
  if (result < 0 || st.failed) {
    // A charstring that breaks a rule is dropped whole, as FreeType does,
    // rather than drawn as whatever part of it ran.
    out.path.clear();
    return;
  }
  st.close();
}

// Runs a Type 2 charstring (or subroutine) over [p, end). Returns 1 at
// endchar, 0 at return or the end of the bytes, -1 on anything malformed.
int Font::runCharstring(CharstringState& st, std::uint32_t p, std::uint32_t end, int depth) const {
  if (depth > 10) return -1;
  double* s = st.stack;
  int& sp = st.sp;
  const auto need = [&](int n) { return sp >= n; };
  while (p < end) {
    // The most any of ~1M CFF glyphs shipped with macOS needs is ~5600.
    if (++st.ops > 50000) return -1;
    const std::uint8_t b0 = data_[p];
    // Operands.
    if (b0 >= 32 || b0 == 28) {
      double v;
      if (b0 == 28) { if (p + 3 > end) return -1; v = i16(data_, p + 1); p += 3; }
      else if (b0 <= 246) { v = b0 - 139; p += 1; }
      else if (b0 <= 250) { if (p + 2 > end) return -1; v = (b0 - 247) * 256 + data_[p + 1] + 108; p += 2; }
      else if (b0 <= 254) { if (p + 2 > end) return -1; v = -(b0 - 251) * 256 - data_[p + 1] - 108; p += 2; }
      else { if (p + 5 > end) return -1; v = static_cast<std::int32_t>(u32(data_, p + 1)) / 65536.0; p += 5; }
      if (sp >= 48) return -1;
      s[sp++] = v;
      continue;
    }
    ++p;
    // The first stack-clearing operator may carry the advance width as an
    // extra leading operand; hmtx is the authority here, so it is skipped.
    const auto skipWidth = [&](bool present) {
      int first = 0;
      if (!st.widthDone && present) first = 1;
      st.widthDone = true;
      return first;
    };
    switch (b0) {
      case 1: case 3: case 18: case 23: {         // hstem vstem hstemhm vstemhm
        const int first = skipWidth(sp & 1);
        st.stems += (sp - first) / 2;
        sp = 0;
        break;
      }
      case 19: case 20: {                          // hintmask cntrmask
        const int first = skipWidth(sp & 1);
        st.stems += (sp - first) / 2;            // an implied vstemhm
        sp = 0;
        const std::uint64_t after = static_cast<std::uint64_t>(p) + static_cast<std::uint64_t>((st.stems + 7) / 8);
        if (after > end) return -1;
        p = static_cast<std::uint32_t>(after);
        break;
      }
      case 21: {                                   // rmoveto
        const int first = skipWidth(sp > 2);
        if (sp - first < 2) return -1;
        st.moveTo(s[first], s[first + 1]);
        sp = 0;
        break;
      }
      case 22: {                                   // hmoveto
        const int first = skipWidth(sp > 1);
        if (sp - first < 1) return -1;
        st.moveTo(s[first], 0);
        sp = 0;
        break;
      }
      case 4: {                                    // vmoveto
        const int first = skipWidth(sp > 1);
        if (sp - first < 1) return -1;
        st.moveTo(0, s[first]);
        sp = 0;
        break;
      }
      case 5:                                      // rlineto
        for (int i = 0; i + 1 < sp; i += 2) st.lineTo(s[i], s[i + 1]);
        sp = 0;
        break;
      case 6: case 7: {                            // hlineto vlineto
        bool horizontal = b0 == 6;
        for (int i = 0; i < sp; ++i) {
          if (horizontal) st.lineTo(s[i], 0); else st.lineTo(0, s[i]);
          horizontal = !horizontal;
        }
        sp = 0;
        break;
      }
      case 8:                                      // rrcurveto
        for (int i = 0; i + 5 < sp; i += 6) st.curveTo(s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
        sp = 0;
        break;
      case 24: {                                   // rcurveline
        if (sp < 8) return -1;
        int i = 0;
        for (; i + 6 <= sp - 2; i += 6) st.curveTo(s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
        st.lineTo(s[i], s[i + 1]);
        sp = 0;
        break;
      }
      case 25: {                                   // rlinecurve
        if (sp < 8) return -1;
        int i = 0;
        for (; i + 2 <= sp - 6; i += 2) st.lineTo(s[i], s[i + 1]);
        st.curveTo(s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
        sp = 0;
        break;
      }
      case 26: {                                   // vvcurveto
        int i = 0;
        double dx1 = 0;
        if (sp & 1) { dx1 = s[0]; i = 1; }
        for (; i + 3 < sp; i += 4) { st.curveTo(dx1, s[i], s[i+1], s[i+2], 0, s[i+3]); dx1 = 0; }
        sp = 0;
        break;
      }
      case 27: {                                   // hhcurveto
        int i = 0;
        double dy1 = 0;
        if (sp & 1) { dy1 = s[0]; i = 1; }
        for (; i + 3 < sp; i += 4) { st.curveTo(s[i], dy1, s[i+1], s[i+2], s[i+3], 0); dy1 = 0; }
        sp = 0;
        break;
      }
      case 30: case 31: {                          // vhcurveto hvcurveto
        bool horizontal = b0 == 31;
        for (int i = 0; i + 3 < sp; i += 4) {
          const double last = (sp - i == 5) ? s[i + 4] : 0;
          if (horizontal) st.curveTo(s[i], 0, s[i+1], s[i+2], last, s[i+3]);
          else st.curveTo(0, s[i], s[i+1], s[i+2], s[i+3], last);
          horizontal = !horizontal;
        }
        sp = 0;
        break;
      }
      case 10: case 29: {                          // callsubr callgsubr
        if (!need(1)) return -1;
        const CffIndex& subrs = b0 == 10 ? *st.local : cff_.globalSubrs;
        const double number = s[--sp] + subrBias(subrs.count);
        if (!(number >= 0) || number >= subrs.count) return -1;
        std::uint32_t ss = 0, se = 0;
        if (!cffItem(subrs, static_cast<std::uint32_t>(number), ss, se)) return -1;
        const int r = runCharstring(st, ss, se, depth + 1);
        if (r != 0) return r;
        break;
      }
      case 11:                                     // return
        return 0;
      case 14: {                                   // endchar
        // Four operands (after any width) are seac's accent composition,
        // which this decoder does not perform; the base outline so far stays.
        skipWidth(sp == 1 || sp == 5);
        st.close();
        sp = 0;
        return 1;
      }
      case 12: {
        if (p >= end) return -1;
        const std::uint8_t op = data_[p++];
        switch (op) {
          case 35:                                 // flex
            if (!need(13)) return -1;
            st.curveTo(s[0], s[1], s[2], s[3], s[4], s[5]);
            st.curveTo(s[6], s[7], s[8], s[9], s[10], s[11]);
            sp = 0; break;
          case 34:                                 // hflex
            if (!need(7)) return -1;
            st.curveTo(s[0], 0, s[1], s[2], s[3], 0);
            st.curveTo(s[4], 0, s[5], -s[2], s[6], 0);
            sp = 0; break;
          case 36:                                 // hflex1
            if (!need(9)) return -1;
            st.curveTo(s[0], s[1], s[2], s[3], s[4], 0);
            st.curveTo(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
            sp = 0; break;
          case 37: {                               // flex1
            if (!need(11)) return -1;
            const double dx = s[0] + s[2] + s[4] + s[6] + s[8];
            const double dy = s[1] + s[3] + s[5] + s[7] + s[9];
            st.curveTo(s[0], s[1], s[2], s[3], s[4], s[5]);
            if (std::fabs(dx) > std::fabs(dy)) st.curveTo(s[6], s[7], s[8], s[9], s[10], -dy);
            else st.curveTo(s[6], s[7], s[8], s[9], -dx, s[10]);
            sp = 0; break;
          }
          // The arithmetic and storage operators of the Type 2 spec.
          case 3: if (!need(2)) return -1; s[sp-2] = (s[sp-2] != 0 && s[sp-1] != 0) ? 1 : 0; --sp; break;   // and
          case 4: if (!need(2)) return -1; s[sp-2] = (s[sp-2] != 0 || s[sp-1] != 0) ? 1 : 0; --sp; break;   // or
          case 5: if (!need(1)) return -1; s[sp-1] = s[sp-1] == 0 ? 1 : 0; break;                            // not
          case 9: if (!need(1)) return -1; s[sp-1] = std::fabs(s[sp-1]); break;                              // abs
          case 10: if (!need(2)) return -1; s[sp-2] += s[sp-1]; --sp; break;                                 // add
          case 11: if (!need(2)) return -1; s[sp-2] -= s[sp-1]; --sp; break;                                 // sub
          case 12: if (!need(2)) return -1; s[sp-2] = s[sp-1] != 0 ? s[sp-2] / s[sp-1] : 0; --sp; break;     // div
          case 14: if (!need(1)) return -1; s[sp-1] = -s[sp-1]; break;                                       // neg
          case 15: if (!need(2)) return -1; s[sp-2] = s[sp-2] == s[sp-1] ? 1 : 0; --sp; break;               // eq
          case 18: if (!need(1)) return -1; --sp; break;                                                     // drop
          case 20: {                                                                                          // put
            if (!need(2)) return -1;
            const double i = s[sp-1];
            if (i >= 0 && i < 32) st.transient[static_cast<int>(i)] = s[sp-2];
            sp -= 2; break;
          }
          case 21: {                                                                                          // get
            if (!need(1)) return -1;
            const double i = s[sp-1];
            s[sp-1] = i >= 0 && i < 32 ? st.transient[static_cast<int>(i)] : 0;
            break;
          }
          case 22:                                                                                            // ifelse
            if (!need(4)) return -1;
            s[sp-4] = s[sp-2] <= s[sp-1] ? s[sp-4] : s[sp-3];
            sp -= 3; break;
          case 23: if (sp >= 48) return -1; s[sp++] = 0.5; break;                                            // random
          case 24: if (!need(2)) return -1; s[sp-2] *= s[sp-1]; --sp; break;                                 // mul
          case 26: if (!need(1)) return -1; s[sp-1] = s[sp-1] > 0 ? std::sqrt(s[sp-1]) : 0; break;           // sqrt
          case 27: if (!need(1) || sp >= 48) return -1; s[sp] = s[sp-1]; ++sp; break;                        // dup
          case 28: if (!need(2)) return -1; std::swap(s[sp-1], s[sp-2]); break;                              // exch
          case 29: {                                                                                          // index
            if (!need(1)) return -1;
            double i = s[sp-1];
            if (i < 0) i = 0;
            if (i >= sp - 1) return -1;
            s[sp-1] = s[sp - 2 - static_cast<int>(i)];
            break;
          }
          case 30: {                                                                                          // roll
            if (!need(2)) return -1;
            const double nd = s[sp-2], jd = s[sp-1];
            sp -= 2;
            if (!(nd >= 1) || nd > sp || !std::isfinite(jd)) return -1;
            const int n = static_cast<int>(nd);
            int j = static_cast<int>(std::fmod(jd, static_cast<double>(n)));
            if (j < 0) j += n;
            std::rotate(s + sp - n, s + sp - j, s + sp);
            break;
          }
          default: sp = 0; break;                  // reserved: clears, as the spec says
        }
        if (sp > 0 && !std::isfinite(s[sp-1])) return -1;
        break;
      }
      default:                                     // reserved operators
        sp = 0;
        break;
    }
    if (st.failed) return -1;
  }
  return 0;
}

// ============================================================================
// Kerning

void Font::parseKerning() {
  kernSource_ = KernSource::None;
  gposKernLookups_.clear();
  gposSingle_ = false;
  kernSubtables_.clear();

  // --- GPOS: the 'kern' feature's SinglePos and PairPos lookups.
  bool gposHasKernFeature = false;
  const std::uint32_t gpos = tableOffset("GPOS");
  const std::uint64_t gposEnd = static_cast<std::uint64_t>(gpos) + tableLength("GPOS");
  if (gpos && tableLength("GPOS") >= 10 && u16(data_, gpos) == 1) {
    const std::uint32_t scripts = gpos + u16(data_, gpos + 4);
    const std::uint32_t features = gpos + u16(data_, gpos + 6);
    const std::uint32_t lookups = gpos + u16(data_, gpos + 8);
    // The script HarfBuzz picks for a Latin run: 'latn', then 'DFLT', then
    // 'dflt'. Not "the first script" -- HarfBuzz then picks none, so a face
    // whose GPOS only knows, say, Tamil does not kern Latin text in Chrome
    // (InaiMathi), and neither does it here.
    const std::uint16_t scriptCount = u16(data_, scripts);
    std::uint32_t script = 0;
    for (const char* want : {"latn", "DFLT", "dflt"}) {
      for (std::uint16_t i = 0; i < scriptCount && !script; ++i) {
        const std::uint32_t rec = scripts + 2 + i * 6u;
        if (rec + 6 <= gposEnd && tagAt(data_, rec, want))
          script = scripts + u16(data_, rec + 4);
      }
      if (script) break;
    }
    std::uint32_t langSys = 0;
    if (script) {
      if (u16(data_, script)) langSys = script + u16(data_, script);
      else if (u16(data_, script + 2)) langSys = script + u16(data_, script + 4 + 4);
    }
    // Which lookups the script's 'kern' features name. Counts are 16-bit but
    // a hostile table can repeat them endlessly, so features are visited once
    // each and the reads are budgeted; real fonts use a handful.
    const std::uint16_t lookupCount = u16(data_, lookups);
    std::vector<bool> selected(lookupCount, false);
    if (langSys) {
      const std::uint16_t featureCount = u16(data_, features);
      std::vector<bool> visited(featureCount, false);
      std::vector<std::uint16_t> featureIndices;
      const std::uint16_t required = u16(data_, langSys + 2);
      if (required != 0xFFFF) featureIndices.push_back(required);
      const std::uint16_t count = u16(data_, langSys + 4);
      for (std::uint16_t i = 0; i < count; ++i) featureIndices.push_back(u16(data_, langSys + 6 + i * 2u));
      int budget = 65536;
      for (std::uint16_t fi : featureIndices) {
        if (fi >= featureCount || visited[fi]) continue;
        visited[fi] = true;
        const std::uint32_t rec = features + 2 + fi * 6u;
        if (rec + 6 > gposEnd || !tagAt(data_, rec, "kern")) continue;
        gposHasKernFeature = true;
        const std::uint32_t feature = features + u16(data_, rec + 4);
        const std::uint16_t n = u16(data_, feature + 2);
        for (std::uint16_t k = 0; k < n && budget > 0; ++k, --budget) {
          const std::uint16_t li = u16(data_, feature + 4 + k * 2u);
          if (li < lookupCount) selected[li] = true;
        }
      }
    }
    // Lookups apply in LookupList order, each once.
    std::size_t totalSubtables = 0;
    for (std::uint16_t li = 0; li < lookupCount && totalSubtables < 1024; ++li) {
      if (!selected[li]) continue;
      const std::uint32_t lookup = lookups + u16(data_, lookups + 2 + li * 2u);
      const std::uint16_t type = u16(data_, lookup);
      const std::uint16_t subCount = u16(data_, lookup + 4);
      // Extension lookups (type 9) name their real type in each subtable;
      // all of a lookup's subtables are of one type.
      GposLookup entry;
      for (std::uint16_t k = 0; k < subCount; ++k) {
        std::uint32_t sub = lookup + u16(data_, lookup + 6 + k * 2u);
        std::uint16_t subType = type;
        if (type == 9) {
          if (u16(data_, sub) != 1) continue;
          subType = u16(data_, sub + 2);
          sub = clampOffset(static_cast<std::uint64_t>(sub) + u32(data_, sub + 4));
        }
        if (subType != 1 && subType != 2) break;          // contextual and the rest: not read
        if (entry.type && entry.type != subType) continue;
        entry.type = subType;
        const std::uint16_t format = u16(data_, sub);
        if (sub < gposEnd && (format == 1 || format == 2)) entry.subtables.push_back(sub);
        // Real faces kern with a dozen subtables at most; this bounds the
        // per-pair search a hostile one could demand.
        if (++totalSubtables >= 1024) break;
      }
      if (!entry.subtables.empty()) {
        if (entry.type == 1) gposSingle_ = true;
        gposKernLookups_.push_back(std::move(entry));
      }
    }
  }

  // --- 'kerx' (AAT), formats 0 and 2.
  std::vector<KernSubtable> kerx;
  bool hasKerx = false, hasKern = false;
  const std::uint32_t kx = tableOffset("kerx");
  if (kx && tableLength("kerx") >= 8) {
    const std::uint64_t tableEnd = static_cast<std::uint64_t>(kx) + tableLength("kerx");
    const std::uint32_t count = std::min<std::uint32_t>(u32(data_, kx + 4), 4096);
    std::uint64_t at = kx + 8ull;
    for (std::uint32_t i = 0; i < count && at + 12 <= tableEnd; ++i) {
      const std::uint32_t length = u32(data_, static_cast<std::uint32_t>(at));
      const std::uint32_t coverage = u32(data_, static_cast<std::uint32_t>(at + 4));
      if (length < 12) break;
      hasKerx = true;
      const std::uint64_t subEnd = std::min<std::uint64_t>(at + length, tableEnd);
      const std::uint8_t format = coverage & 0xFF;
      // Not vertical, not cross-stream, not a variation.
      if (!(coverage & 0xE0000000u) && (format == 0 || format == 2)) {
        KernSubtable st;
        st.offset = static_cast<std::uint32_t>(at);
        st.end = static_cast<std::uint32_t>(subEnd);
        st.format = format;
        kerx.push_back(st);
      }
      // Version 3+ subtables may carry a subtable-coverage array after them;
      // it is counted in `length`, so stepping by length is still right.
      at += length;
    }
  }

  // --- 'kern', OpenType (version 0) or Apple (version 1.0) header.
  std::vector<KernSubtable> kern;
  const std::uint32_t kt = tableOffset("kern");
  if (kt && tableLength("kern") >= 4) {
    const std::uint64_t tableEnd = static_cast<std::uint64_t>(kt) + tableLength("kern");
    const bool apple = u16(data_, kt) == 1;
    const std::uint32_t count = std::min<std::uint32_t>(apple ? u32(data_, kt + 4) : u16(data_, kt + 2), 4096);
    std::uint64_t at = kt + (apple ? 8ull : 4ull);
    for (std::uint32_t i = 0; i < count && at + (apple ? 8 : 6) <= tableEnd; ++i) {
      const std::uint32_t a = static_cast<std::uint32_t>(at);
      std::uint64_t length;
      std::uint16_t coverage;
      std::uint8_t format;
      bool usable, override_ = false;
      if (apple) {
        length = u32(data_, a);
        coverage = u16(data_, a + 4);
        format = coverage & 0xFF;
        usable = !(coverage & 0xE000);                  // vertical, cross-stream, variation
      } else {
        length = u16(data_, a + 2);
        coverage = u16(data_, a + 4);
        format = static_cast<std::uint8_t>(coverage >> 8);
        // Horizontal, not minimum, not cross-stream.
        usable = (coverage & 0x01) && !(coverage & 0x02) && !(coverage & 0x04);
        override_ = (coverage & 0x08) != 0;
        // A 16-bit length overflows on a big format 0 table; the last
        // subtable runs to the end of the table whatever it says.
        if (i + 1 == count) length = tableEnd - at;
      }
      if (length < (apple ? 8u : 6u)) break;
      hasKern = true;
      const std::uint64_t subEnd = std::min<std::uint64_t>(at + length, tableEnd);
      if (usable && (format == 0 || format == 2 || format == 3)) {
        KernSubtable st;
        st.offset = a;
        st.end = static_cast<std::uint32_t>(subEnd);
        st.format = format;
        st.apple = apple;
        st.override_ = override_;
        kern.push_back(st);
      }
      at += length;
    }
  }

  // HarfBuzz's choice (hb-ot-shape.cc), which is Chrome's. A font with
  // 'morx' is shaped with it, and its GSUB then does not count; kerx wins
  // unless GSUB and GPOS are both in play; else GPOS; and when GPOS has no
  // 'kern' feature for the script, the legacy kern table. (Not kerx there:
  // checked against hb-shape 14.6 on Hiragino's interface face, whose GPOS
  // lacks 'kern' and whose kerx pairs it leaves unapplied.) The choice is
  // made on which tables HAVE data, not on which of their subtable formats
  // are read here -- an unread format kerns nothing, as an approximation,
  // rather than handing the job to a table HarfBuzz ignores.
  const bool hasMorx = tableLength("morx") >= 8 && u32(data_, tableOffset("morx") + 4) > 0;
  const bool hasGsub = !hasMorx && tableLength("GSUB") >= 10 && u16(data_, tableOffset("GSUB")) == 1;
  const bool hasGpos = tableLength("GPOS") >= 10 && u16(data_, gpos) == 1;
  bool applyKerx = false, applyGpos = false, applyKern = false;
  if (hasKerx && !(hasGsub && hasGpos)) applyKerx = true;
  else if (hasGpos) applyGpos = true;
  if (!applyKerx && (!gposHasKernFeature || !applyGpos) && hasKern) applyKern = true;
  if (applyKerx && !kerx.empty()) { kernSource_ = KernSource::Kerx; kernSubtables_ = std::move(kerx); }
  else if (applyGpos && gposHasKernFeature && !gposKernLookups_.empty()) kernSource_ = KernSource::Gpos;
  else if (applyKern && !kern.empty()) { kernSource_ = KernSource::Kern; kernSubtables_ = std::move(kern); }
  if (kernSource_ != KernSource::Gpos) { gposKernLookups_.clear(); gposSingle_ = false; }
}

int Font::gposPairValue(std::uint32_t st, std::uint16_t left, std::uint16_t right, bool& matched) const {
  matched = false;
  const std::uint16_t format = u16(data_, st);
  const int covered = coverageIndex(data_, st + u16(data_, st + 2), left);
  if (covered < 0) return 0;
  const std::uint16_t vf1 = u16(data_, st + 4), vf2 = u16(data_, st + 6);
  const std::uint32_t size1 = 2u * static_cast<std::uint32_t>(popcount8(vf1));
  const std::uint32_t size2 = 2u * static_cast<std::uint32_t>(popcount8(vf2));
  if (format == 1) {
    const std::uint16_t setCount = u16(data_, st + 8);
    if (static_cast<std::uint32_t>(covered) >= setCount) return 0;
    const std::uint32_t set = st + u16(data_, st + 10 + static_cast<std::uint32_t>(covered) * 2);
    const std::uint32_t recordSize = 2 + size1 + size2;
    int lo = 0, hi = static_cast<int>(u16(data_, set)) - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::uint32_t rec = set + 2 + static_cast<std::uint32_t>(mid) * recordSize;
      const std::uint16_t second = u16(data_, rec);
      if (second == right) {
        matched = true;
        return valueField(data_, rec + 2, vf1, 0x0004) + valueField(data_, rec + 2 + size1, vf2, 0x0001);
      }
      if (second < right) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
  }
  if (format == 2) {
    const std::uint16_t class1Count = u16(data_, st + 12), class2Count = u16(data_, st + 14);
    const std::uint16_t c1 = classOf(data_, st + u16(data_, st + 8), left);
    const std::uint16_t c2 = classOf(data_, st + u16(data_, st + 10), right);
    if (c1 >= class1Count || c2 >= class2Count) return 0;
    // A covered first glyph settles the pair in this subtable, zero or not,
    // as HarfBuzz has it.
    matched = true;
    const std::uint64_t rec = st + 16ull + (static_cast<std::uint64_t>(c1) * class2Count + c2) * (size1 + size2);
    const std::uint32_t r = clampOffset(rec);
    return valueField(data_, r, vf1, 0x0004) + valueField(data_, r + size1, vf2, 0x0001);
  }
  return 0;
}

int Font::legacyKernValue(const KernSubtable& t, std::uint16_t left, std::uint16_t right, bool& matched) const {
  matched = false;
  const std::uint32_t body = t.offset + (t.apple ? 8 : 6);
  if (t.format == 0) {
    const std::uint32_t pairs = body + 8;
    std::uint32_t n = u16(data_, body);
    if (pairs < t.end) n = std::min<std::uint32_t>(n, (t.end - pairs) / 6); else n = 0;
    const std::uint32_t key = (static_cast<std::uint32_t>(left) << 16) | right;
    std::int64_t lo = 0, hi = static_cast<std::int64_t>(n) - 1;
    while (lo <= hi) {
      const std::int64_t mid = (lo + hi) / 2;
      const std::uint32_t rec = pairs + static_cast<std::uint32_t>(mid) * 6;
      const std::uint32_t k = u32(data_, rec);
      if (k == key) { matched = true; return i16(data_, rec + 4); }
      if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
  }
  if (t.format == 2) {
    // Class values are byte offsets from the subtable's start, the left ones
    // already including the array's own offset; their sum locates the value.
    const auto classValue = [&](std::uint32_t table, std::uint16_t g, bool& ok) -> std::uint32_t {
      ok = false;
      const std::uint16_t first = u16(data_, table), count = u16(data_, table + 2);
      if (g < first || g - first >= count) return 0;
      ok = true;
      return u16(data_, table + 4 + (g - first) * 2u);
    };
    bool okLeft = false, okRight = false;
    const std::uint32_t l = classValue(t.offset + u16(data_, body + 2), left, okLeft);
    const std::uint32_t r = classValue(t.offset + u16(data_, body + 4), right, okRight);
    if (!okLeft || !okRight) return 0;
    const std::uint32_t array = u16(data_, body + 6);
    if (l + r < array) return 0;
    const std::uint64_t at = static_cast<std::uint64_t>(t.offset) + l + r;
    if (at + 2 > t.end) return 0;
    matched = true;
    return i16(data_, static_cast<std::uint32_t>(at));
  }
  if (t.format == 3) {
    // Byte classes into a byte index into a table of values.
    const std::uint16_t glyphs = u16(data_, body);
    const std::uint8_t valueCount = u8(data_, body + 2), leftCount = u8(data_, body + 3), rightCount = u8(data_, body + 4);
    if (left >= glyphs || right >= glyphs) return 0;
    const std::uint32_t values = body + 6;
    const std::uint32_t leftClasses = values + valueCount * 2u;
    const std::uint32_t rightClasses = leftClasses + glyphs;
    const std::uint32_t indices = rightClasses + glyphs;
    const std::uint8_t lc = u8(data_, leftClasses + left), rc = u8(data_, rightClasses + right);
    if (lc >= leftCount || rc >= rightCount) return 0;
    const std::uint32_t at = indices + static_cast<std::uint32_t>(lc) * rightCount + rc;
    if (at >= t.end) return 0;
    const std::uint8_t index = u8(data_, at);
    if (index >= valueCount) return 0;
    matched = true;
    return i16(data_, values + index * 2u);
  }
  return 0;
}

int Font::singleAdjustUnits(std::uint16_t g) const {
  if (!gposSingle_ || !kerning_) return 0;
  int total = 0;
  for (const GposLookup& lookup : gposKernLookups_) {
    if (lookup.type != 1) continue;
    for (std::uint32_t st : lookup.subtables) {
      const int covered = coverageIndex(data_, st + u16(data_, st + 2), g);
      if (covered < 0) continue;
      const std::uint16_t format = u16(data_, st), vf = u16(data_, st + 4);
      if (format == 1) total += valueField(data_, st + 6, vf, 0x0004);
      else if (static_cast<std::uint32_t>(covered) < u16(data_, st + 6))
        total += valueField(data_, st + 8 + static_cast<std::uint32_t>(covered) * 2u * static_cast<std::uint32_t>(popcount8(vf)), vf, 0x0004);
      break;                                  // the first subtable that covers it settles the lookup
    }
  }
  return total;
}

int Font::kerxValue(const KernSubtable& t, std::uint16_t left, std::uint16_t right, bool& matched) const {
  matched = false;
  const std::uint32_t body = t.offset + 12;
  if (t.format == 0) {
    const std::uint32_t pairs = body + 16;
    std::uint32_t n = u32(data_, body);
    if (pairs < t.end) n = std::min<std::uint32_t>(n, (t.end - pairs) / 6); else n = 0;
    const std::uint32_t key = (static_cast<std::uint32_t>(left) << 16) | right;
    std::int64_t lo = 0, hi = static_cast<std::int64_t>(n) - 1;
    while (lo <= hi) {
      const std::int64_t mid = (lo + hi) / 2;
      const std::uint32_t rec = pairs + static_cast<std::uint32_t>(mid) * 6;
      const std::uint32_t k = u32(data_, rec);
      if (k == key) { matched = true; return i16(data_, rec + 4); }
      if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
  }
  if (t.format == 2) {
    // Lookup-table classes; their sum indexes the FWORD array directly.
    bool okLeft = false, okRight = false;
    const std::uint32_t l = aatLookup(data_, clampOffset(static_cast<std::uint64_t>(t.offset) + u32(data_, body + 4)), t.end, left, numGlyphs_, okLeft);
    const std::uint32_t r = aatLookup(data_, clampOffset(static_cast<std::uint64_t>(t.offset) + u32(data_, body + 8)), t.end, right, numGlyphs_, okRight);
    const std::uint64_t at = static_cast<std::uint64_t>(t.offset) + u32(data_, body + 12) + 2ull * (l + r);
    if (at + 2 > t.end) return 0;
    matched = true;
    return i16(data_, static_cast<std::uint32_t>(at));
  }
  return 0;
}

int Font::kerningUnits(std::uint16_t left, std::uint16_t right) const {
  if (!valid_ || !kerning_) return 0;
  int total = 0;
  bool matched = false;
  switch (kernSource_) {
    case KernSource::Gpos:
      for (const GposLookup& lookup : gposKernLookups_) {
        if (lookup.type != 2) continue;
        for (std::uint32_t sub : lookup.subtables) {
          const int v = gposPairValue(sub, left, right, matched);
          if (matched) { total += v; break; }   // the first subtable that applies settles the lookup
        }
      }
      break;
    case KernSource::Kern:
      for (const KernSubtable& t : kernSubtables_) {
        const int v = legacyKernValue(t, left, right, matched);
        if (matched) total = t.override_ ? v : total + v;
      }
      break;
    case KernSource::Kerx:
      for (const KernSubtable& t : kernSubtables_) {
        const int v = kerxValue(t, left, right, matched);
        if (matched) total += v;
      }
      break;
    case KernSource::None:
      break;
  }
  return total;
}

bool Font::hasKerning() const { return valid_ && kernSource_ != KernSource::None; }

float Font::kerning(std::uint16_t left, std::uint16_t right, float pixelSize) const {
  if (!valid_ || !kerning_ || kernSource_ == KernSource::None) return 0;
  const int units = kerningUnits(left, right);
  return units ? static_cast<float>(units) * (pixelSize / static_cast<float>(unitsPerEm_)) : 0.f;
}

// ============================================================================
// Glyph-level API

float Font::glyphAdvance(std::uint16_t g, float pixelSize) const {
  if (!valid_) return 0;
  const int units = advanceWidth(g) + (gposSingle_ ? singleAdjustUnits(g) : 0);
  return static_cast<float>(units) * (pixelSize / static_cast<float>(unitsPerEm_));
}

void Font::appendGlyph(Path2D& path, std::uint16_t index, float x, float y, float pixelSize) const {
  if (!valid_ || pixelSize <= 0) return;
  path.markGlyphOutlines();
  if (index >= numGlyphs_) return;
  const float scale = pixelSize / static_cast<float>(unitsPerEm_);
  const Glyph& g = glyph(index);
  const auto& src = static_cast<const Path2D&>(g.path).segments();
  std::vector<Path2D::Segment>& dest = path.segments();
  dest.reserve(dest.size() + src.size());
  for (const Path2D::Segment& s : src) {
    Path2D::Segment placed = s;
    const int pairs = pairCount(s.command);
    for (int k = 0; k < pairs; ++k) {
      placed.v[k * 2]     = x + s.v[k * 2] * scale;
      placed.v[k * 2 + 1] = y + s.v[k * 2 + 1] * scale;
    }
    dest.push_back(placed);
  }
}

bool Font::glyphBounds(std::uint16_t index, float pixelSize, float& x0, float& y0, float& x1, float& y1) const {
  x0 = y0 = x1 = y1 = 0;
  if (!valid_ || index >= numGlyphs_) return false;
  const Glyph& g = glyph(index);
  if (!g.ink) return false;
  const float scale = pixelSize / static_cast<float>(unitsPerEm_);
  x0 = g.xMin * scale; y0 = g.yMin * scale; x1 = g.xMax * scale; y1 = g.yMax * scale;
  return true;
}

const Path2D* Font::glyphOutline(std::uint16_t index) const {
  if (!valid_ || index >= numGlyphs_) return nullptr;
  const Glyph& g = glyph(index);
  return static_cast<const Path2D&>(g.path).segments().empty() ? nullptr : &g.path;
}

// ============================================================================
// Strings

void Font::appendText(Path2D& path, const std::string& utf8, float x, float y, float pixelSize) const {
  if (!valid_ || utf8.empty() || pixelSize <= 0) return;
  // Downstream a glyph is an ordinary path, which is the point -- but the
  // rasterizer still has to know it is filling text, because the browser gives
  // glyph coverage a gamma it does not give a shape's.
  path.markGlyphOutlines();
  const float scale = pixelSize / static_cast<float>(unitsPerEm_);
  const bool kern = kerning_ && kernSource_ != KernSource::None;
  std::vector<Path2D::Segment>& dest = path.segments();

  float penX = 0;
  std::size_t i = 0;
  std::uint16_t previous = 0;
  bool kernable = false;      // whether the previous character can kern with this one
  while (i < utf8.size()) {
    const std::uint32_t codepoint = decodeUtf8(utf8, i);
    const std::uint16_t index = glyphIndex(codepoint);
    const bool delimiter = isWordDelimiter(codepoint);
    if (kern && kernable && !delimiter) penX += static_cast<float>(kerningUnits(previous, index));
    const Glyph& g = glyph(index);

    for (const Path2D::Segment& s : static_cast<const Path2D&>(g.path).segments()) {
      Path2D::Segment placed = s;
      const int pairs = pairCount(s.command);
      for (int k = 0; k < pairs; ++k) {
        placed.v[k * 2]     = x + (penX + s.v[k * 2]) * scale;
        placed.v[k * 2 + 1] = y + s.v[k * 2 + 1] * scale;
      }
      dest.push_back(placed);
    }
    penX += g.advance;
    if (gposSingle_ && kern) penX += static_cast<float>(singleAdjustUnits(index));
    previous = index;
    kernable = !delimiter;
  }
}

float Font::measure(const std::string& utf8, float pixelSize) const {
  if (!valid_ || pixelSize <= 0) return 0;
  const float scale = pixelSize / static_cast<float>(unitsPerEm_);
  const bool kern = kerning_ && kernSource_ != KernSource::None;
  float width = 0;
  std::size_t i = 0;
  std::uint16_t previous = 0;
  bool kernable = false;
  while (i < utf8.size()) {
    const std::uint32_t codepoint = decodeUtf8(utf8, i);
    const std::uint16_t index = glyphIndex(codepoint);
    const bool delimiter = isWordDelimiter(codepoint);
    if (kern && kernable && !delimiter) width += static_cast<float>(kerningUnits(previous, index));
    width += advanceWidth(index);
    if (gposSingle_ && kern) width += static_cast<float>(singleAdjustUnits(index));
    previous = index;
    kernable = !delimiter;
  }
  return width * scale;
}

bool Font::covers(const std::string& utf8) const {
  if (!valid_) return false;
  std::size_t i = 0;
  while (i < utf8.size()) {
    const std::uint32_t codepoint = decodeUtf8(utf8, i);
    // Controls never reach a glyph lookup that matters: nothing draws them.
    if (codepoint >= 0x20 && glyphIndex(codepoint) == 0) return false;
  }
  return true;
}

std::uint32_t Font::nextCodepoint(const std::string& utf8, std::size_t& i) {
  return decodeUtf8(utf8, i);
}

bool Font::isWordDelimiter(std::uint32_t codepoint) {
  return codepoint == 0x20 || codepoint == 0x09;
}

std::vector<std::uint32_t> Font::mappedCodepoints() const {
  std::vector<std::uint32_t> out;
  if (!valid_ || !cmapOffset_) return out;
  // The ranges the subtable can map at all, each codepoint in them then put
  // through glyphIndex() itself, so the list is what lookups resolve to and
  // not a second reading of the table.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
  if (cmapFormat_ == 4) {
    const std::uint16_t segX2 = u16(data_, cmapOffset_ + 6);
    const std::uint32_t ends = cmapOffset_ + 14;
    const std::uint32_t starts = ends + segX2 + 2;
    for (std::uint16_t s = 0; s + 1 < segX2; s += 2) {
      ranges.emplace_back(u16(data_, starts + s), u16(data_, ends + s));
    }
  } else if (cmapFormat_ == 12 || cmapFormat_ == 13) {
    const std::uint32_t groups = u32(data_, cmapOffset_ + 12);
    for (std::uint32_t g = 0; g < groups && cmapOffset_ + 16 + g * 12ull + 12 <= data_.size(); ++g) {
      const std::uint32_t rec = cmapOffset_ + 16 + g * 12u;
      const std::uint32_t end = std::min<std::uint32_t>(u32(data_, rec + 4), 0x10FFFF);
      ranges.emplace_back(u32(data_, rec), end);
    }
  } else if (cmapFormat_ == 6) {
    const std::uint32_t first = u16(data_, cmapOffset_ + 6);
    const std::uint32_t count = u16(data_, cmapOffset_ + 8);
    if (count) ranges.emplace_back(first, first + count - 1);
  } else if (cmapFormat_ == 0) {
    ranges.emplace_back(0, 255);
  }
  if (cmapKind_ == 2) {
    // Mac Roman codes are not codepoints; the map is ASCII plus the table.
    ranges.clear();
    ranges.emplace_back(0, 0x7F);
    for (std::uint16_t c : kMacRoman) ranges.emplace_back(c, c);
  } else if (cmapKind_ == 1) {
    ranges.emplace_back(0, 0xFF);
  }
  // Ranges merged first, so overlapping or repeated ones -- a malformed
  // table can list the whole of Unicode thousands of times -- cost one pass.
  for (auto& range : ranges) range.second = std::min<std::uint32_t>(range.second, 0x10FFFF);
  std::sort(ranges.begin(), ranges.end());
  std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
  for (const auto& range : ranges) {
    if (range.first > range.second) continue;
    if (!merged.empty() && range.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, range.second);
    else merged.push_back(range);
  }
  for (const auto& range : merged) {
    for (std::uint32_t c = range.first; c <= range.second; ++c) {
      if (glyphIndex(c) != 0) out.push_back(c);
    }
  }
  return out;
}

Font::GlyphMetrics Font::metrics(std::uint32_t codepoint) const {
  GlyphMetrics out;
  if (!valid_) return out;
  const std::uint16_t index = glyphIndex(codepoint);
  out.advance = advanceWidth(index);
  if (cff_.charStrings) {
    // No header box in CFF: the outline's own, rounded outwards, y up.
    const Glyph& g = glyph(index);
    if (!g.ink) return out;
    const auto clamp16 = [](float v) {
      return static_cast<std::int16_t>(std::max(-32768.f, std::min(32767.f, v)));
    };
    out.xMin = clamp16(std::floor(g.xMin));
    out.xMax = clamp16(std::ceil(g.xMax));
    out.yMin = clamp16(std::floor(-g.yMax));
    out.yMax = clamp16(std::ceil(-g.yMin));
    return out;
  }
  // The same test buildGlyph() makes for a blank glyph.
  const std::uint32_t start = glyphOffset(index);
  const std::uint32_t end = glyphOffset(index + 1u);
  if (!glyf_ || end <= start || static_cast<std::uint64_t>(glyf_) + end > data_.size() || end - start < 10) return out;
  const std::uint32_t at = glyf_ + start;
  out.xMin = i16(data_, at + 2);
  out.yMin = i16(data_, at + 4);
  out.xMax = i16(data_, at + 6);
  out.yMax = i16(data_, at + 8);
  return out;
}

// ============================================================================
// Metrics

namespace {

// One edge of the em box: the ascent and descent are rescaled so that together
// they span exactly one em, which is what makes the browser's 'middle' land
// half an em above its 'top' at every size. Values are snapped to 64ths of a
// pixel because the browser's metrics come out of 26.6 fixed point; that is
// what reproduces its baselines to the bit rather than to a hundredth.
float emBoxEdge(int metric, int span, float pixelSize, int unitsPerEm) {
  if (span <= 0) return metric * pixelSize / unitsPerEm;
  return std::round(metric * pixelSize * 64.0f / span) / 64.0f;
}

} // namespace

float Font::ascent(float pixelSize) const {
  return emBoxEdge(ascent_, ascent_ - descent_, pixelSize, unitsPerEm_);
}
float Font::descent(float pixelSize) const {
  return emBoxEdge(descent_, ascent_ - descent_, pixelSize, unitsPerEm_);
}
float Font::lineHeight(float pixelSize) const {
  // A `normal` line box is hhea's ascender, descender and gap, NOT the em box
  // above -- 14px Ubuntu is a 16px row in the browser, and only 932/-189/28
  // gives that. hhea is re-read here because ascent_/descent_ now hold the
  // OS/2 pair.
  const std::uint32_t hhea = tableOffset("hhea");
  if (hhea) {
    const int span = i16(data_, hhea + 4) - i16(data_, hhea + 6) + i16(data_, hhea + 8);
    return span * pixelSize / unitsPerEm_;
  }
  return (ascent_ - descent_ + lineGap_) * pixelSize / unitsPerEm_;
}

Font::FaceMetrics Font::faceMetrics(float pixelSize) const {
  FaceMetrics m;
  if (!valid_ || pixelSize <= 0) return m;
  const float scale = pixelSize / static_cast<float>(unitsPerEm_);

  // CoreText's ascent and descent are hhea's (it ignores USE_TYPO_METRICS --
  // checked against CTFontGetAscent over every face in /System/Library/Fonts).
  // A face without usable hhea numbers falls back to sTypo, then usWin.
  float asc, desc, gap;
  if (haveHhea_ && (hheaAscender_ != 0 || hheaDescender_ != 0)) {
    asc = hheaAscender_ * scale; desc = -hheaDescender_ * scale; gap = hheaLineGap_ * scale;
  } else if (haveOs2_ && (typoAscender_ != 0 || typoDescender_ != 0)) {
    asc = typoAscender_ * scale; desc = -typoDescender_ * scale; gap = typoLineGap_ * scale;
  } else if (haveOs2_) {
    asc = winAscent_ * scale; desc = winDescent_ * scale; gap = 0;
  } else {
    asc = ascent_ * scale; desc = -descent_ * scale; gap = lineGap_ * scale;
  }
  // Blink (FontMetrics::AscentDescentWithHacks): whole pixels, except that a
  // canvas font keeps the fractions when rounding would collapse a tiny one.
  float a = std::floor(asc + 0.5f), d = std::floor(desc + 0.5f);
  if (asc < 3 || asc + desc < 2) { a = asc; d = desc; }
  // Its Mac-only hack, to make these three match their Microsoft cousins.
  if (familyName_ == "Times" || familyName_ == "Helvetica" || familyName_ == "Courier")
    a += std::floor((a + d) * 0.15f + 0.5f);
  m.ascent = a;
  m.descent = d;
  m.lineGap = gap;

  const auto inkTop = [&](std::uint32_t codepoint) {
    const std::uint16_t g = glyphIndex(codepoint);
    if (!g) return 0.f;
    const Glyph& glyphData = glyph(g);
    return glyphData.ink ? -glyphData.yMin * scale : 0.f;
  };
  m.xHeight = xHeight_ > 0 ? xHeight_ * scale : inkTop('x');
  m.capHeight = capHeight_ > 0 ? capHeight_ * scale : inkTop('H');
  if (m.xHeight <= 0) m.xHeight = 0.5f * pixelSize;
  if (m.capHeight <= 0) m.capHeight = 0.7f * pixelSize;

  if (havePost_ && underlineThickness_ > 0) {
    m.underlinePosition = -underlinePosition_ * scale;
    m.underlineThickness = underlineThickness_ * scale;
  } else {
    m.underlineThickness = pixelSize / 14.f;
    m.underlinePosition = pixelSize / 10.f;
  }
  if (haveOs2_ && strikeoutSize_ > 0) {
    m.strikeoutPosition = strikeoutPosition_ * scale;
    m.strikeoutThickness = strikeoutSize_ * scale;
  } else {
    m.strikeoutThickness = m.underlineThickness;
    m.strikeoutPosition = m.xHeight / 2 + m.strikeoutThickness / 2;
  }
  return m;
}

// ============================================================================
// System fonts

bool Font::findSystemFont(const std::vector<std::string>& preferred, std::string& pathOut) {
  static const char* kDirectories[] = {
    "/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts",
    "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/truetype/ubuntu",
    "/usr/share/fonts/truetype/liberation", "/usr/share/fonts/TTF", "/usr/share/fonts",
    "C:\\Windows\\Fonts",
  };

  // Try each preferred family in order, across every directory, before falling
  // back — otherwise a low-priority directory's match beats a better one.
  for (const std::string& want : preferred) {
    for (const char* dir : kDirectories) {
      std::string candidate = std::string(dir) + "/" + want;
      { std::ifstream probe(candidate, std::ios::binary); if (probe) { pathOut = candidate; return true; } }
      for (const char* ext : {".ttf", ".ttc", ".otf"}) {
        candidate = std::string(dir) + "/" + want + ext;
        std::ifstream probe(candidate, std::ios::binary);
        if (probe) { pathOut = candidate; return true; }
      }
    }
  }

  // Last resort: a face that is present on essentially every install of each
  // platform, so text renders even when nothing preferred is available.
  static const char* kFallbacks[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Geneva.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
  };
  for (const char* path : kFallbacks) {
    std::ifstream probe(path, std::ios::binary);
    if (probe) { pathOut = path; return true; }
  }
  return false;
}

namespace {

#if defined(_WIN32)
const char kSeparator = '\\';
#else
const char kSeparator = '/';
#endif

// Font files under `dir`, descending `depth` more levels.
void listFontFiles(const std::string& dir, int depth, std::vector<std::string>& out) {
  if (out.size() > 20000) return;
#if defined(_WIN32)
  _finddata_t info;
  const intptr_t handle = _findfirst((dir + "\\*").c_str(), &info);
  if (handle == -1) return;
  do {
    const std::string name = info.name;
    if (name.empty() || name[0] == '.') continue;
    const std::string full = dir + kSeparator + name;
    if (info.attrib & _A_SUBDIR) { if (depth > 0) listFontFiles(full, depth - 1, out); }
    else if (endsWithFontExtension(name)) out.push_back(full);
  } while (_findnext(handle, &info) == 0);
  _findclose(handle);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> subdirs;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    const std::string full = dir + kSeparator + name;
    if (endsWithFontExtension(name)) out.push_back(full);
    else if (depth > 0) subdirs.push_back(full);   // opendir decides whether it is one
  }
  closedir(d);
  for (const std::string& sub : subdirs) listFontFiles(sub, depth - 1, out);
#endif
}

// Lowercase with spaces, '-' and '_' gone, and a font extension dropped:
// "Times New Roman", "TimesNewRoman" and "times-new-roman.ttf" all agree.
std::string normalizeFamily(const std::string& s) {
  std::string n = lowerAscii(s);
  if (endsWithFontExtension(n)) n.resize(n.size() - 4);
  n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), n.end());
  return n;
}

std::string fileStem(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  if (endsWithFontExtension(name)) name.resize(name.size() - 4);
  return name;
}

bool readRange(std::ifstream& in, std::uint64_t offset, std::uint32_t length, std::vector<std::uint8_t>& out) {
  out.assign(length, 0);
  in.clear();
  in.seekg(static_cast<std::streamoff>(offset));
  if (!in) return false;
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
  out.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
  return !out.empty();
}

// Who each face of a file is, reading only the directory and the 'name',
// 'OS/2' and 'head' tables -- not the outlines, which can be tens of MB.
std::vector<FaceInfo> probeFile(const std::string& path) {
  std::vector<FaceInfo> faces;
  std::ifstream in(path, std::ios::binary);
  if (!in) return faces;
  std::vector<std::uint8_t> header;
  if (!readRange(in, 0, 12, header) || header.size() < 12) return faces;
  std::vector<std::uint32_t> bases;
  if (u32(header, 0) == 0x74746366) {
    const std::uint32_t n = std::min<std::uint32_t>(u32(header, 8), 256);
    std::vector<std::uint8_t> offsets;
    if (!readRange(in, 12, n * 4, offsets)) return faces;
    for (std::uint32_t i = 0; i < n && i * 4 + 4 <= offsets.size(); ++i) bases.push_back(u32(offsets, i * 4));
  } else {
    bases.push_back(0);
  }
  for (std::uint32_t base : bases) {
    FaceInfo info;
    std::vector<std::uint8_t> dir;
    if (readRange(in, base, 12, dir) && dir.size() == 12) {
      const std::uint32_t version = u32(dir, 0);
      const std::uint16_t numTables = u16(dir, 4);
      std::vector<std::uint8_t> records;
      if ((version == 0x00010000 || version == 0x74727565 || version == 0x4F54544F) &&
          readRange(in, base + 12ull, numTables * 16u, records)) {
        std::vector<std::uint8_t> name, os2, head;
        for (std::uint32_t r = 0; r + 16 <= records.size(); r += 16) {
          const std::uint32_t offset = u32(records, r + 8);
          const std::uint32_t length = std::min<std::uint32_t>(u32(records, r + 12), 1u << 20);
          if (std::memcmp(&records[r], "name", 4) == 0) readRange(in, offset, length, name);
          else if (std::memcmp(&records[r], "OS/2", 4) == 0) readRange(in, offset, length, os2);
          else if (std::memcmp(&records[r], "head", 4) == 0) readRange(in, offset, length, head);
        }
        describeFace(Span{name.data(), name.size()}, Span{os2.data(), os2.size()},
                     Span{head.data(), head.size()}, info);
        info.ok = !info.family.empty() || !info.family1.empty();
      }
    }
    faces.push_back(info);
  }
  return faces;
}

// CSS's weight search, as a distance: the order in which CSS Fonts 4 would
// try the available weights for `want`.
int weightDistance(int want, int have) {
  if (want >= 400 && want <= 500) {
    if (have >= want && have <= 500) return have - want;
    if (have < want) return 1000 + (want - have);
    return 2000 + (have - want);
  }
  if (want < 400) return have <= want ? want - have : 1000 + (have - want);
  return have >= want ? have - want : 1000 + (want - have);
}

int faceScore(const FaceInfo& f, int weight, bool italic) {
  return std::abs(f.width - 5) * 1000000 + (f.italic != italic ? 100000 : 0) + weightDistance(weight, f.weight);
}

std::mutex& probeMutex() { static std::mutex m; return m; }
std::map<std::string, std::vector<FaceInfo>>& probeCache() {
  static std::map<std::string, std::vector<FaceInfo>> cache;
  return cache;
}
const std::vector<FaceInfo>& probeCached(const std::string& path) {
  auto& cache = probeCache();
  auto it = cache.find(path);
  if (it == cache.end()) it = cache.emplace(path, probeFile(path)).first;
  return it->second;
}

} // namespace

bool Font::findSystemFont(const std::vector<std::string>& preferred, int weight, bool italic,
                          std::string& pathOut, int& faceIndexOut) {
  std::vector<std::string> directories = {
    "/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts",
    "/usr/share/fonts", "/usr/local/share/fonts", "C:\\Windows\\Fonts",
  };
  if (const char* home = std::getenv("HOME")) {
    directories.push_back(std::string(home) + "/Library/Fonts");
    directories.push_back(std::string(home) + "/.local/share/fonts");
    directories.push_back(std::string(home) + "/.fonts");
  }
  std::vector<std::string> files;
  std::set<std::string> seen;
  for (const std::string& dir : directories) {
    std::vector<std::string> found;
    listFontFiles(dir, 3, found);
    std::sort(found.begin(), found.end());
    for (std::string& f : found)
      if (seen.insert(f).second) files.push_back(std::move(f));
  }

  std::lock_guard<std::mutex> lock(probeMutex());
  // The best face of `candidates` for which `accept` holds.
  const auto choose = [&](const std::vector<std::string>& candidates, const auto& accept) {
    int bestScore = -1;
    for (const std::string& path : candidates) {
      const std::vector<FaceInfo>& faces = probeCached(path);
      for (std::size_t i = 0; i < faces.size(); ++i) {
        if (!faces[i].ok || !accept(path, faces[i])) continue;
        const int score = faceScore(faces[i], weight, italic);
        if (bestScore < 0 || score < bestScore) {
          bestScore = score; pathOut = path; faceIndexOut = static_cast<int>(i);
        }
      }
    }
    return bestScore >= 0;
  };

  for (const std::string& want : preferred) {
    const std::string needle = normalizeFamily(want);
    if (needle.empty()) continue;
    const auto accept = [&](const std::string& path, const FaceInfo& f) {
      return normalizeFamily(f.family) == needle || normalizeFamily(f.family1) == needle ||
             normalizeFamily(fileStem(path)) == needle;
    };
    // Files named like the family first (cheap: only those are opened)...
    std::vector<std::string> named;
    for (const std::string& f : files)
      if (normalizeFamily(fileStem(f)).compare(0, needle.size(), needle) == 0) named.push_back(f);
    if (choose(named, accept)) return true;
  }
  // ...then every file, for families whose files are named otherwise
  // ("Hiragino Sans" lives in "ヒラギノ角ゴシック W3.ttc"). Probes are cached,
  // so this costs one pass over the directories per process.
  for (const std::string& want : preferred) {
    const std::string needle = normalizeFamily(want);
    if (needle.empty()) continue;
    const auto accept = [&](const std::string&, const FaceInfo& f) {
      return normalizeFamily(f.family) == needle || normalizeFamily(f.family1) == needle;
    };
    if (choose(files, accept)) return true;
  }

  static const char* kFallbacks[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Geneva.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
  };
  for (const char* path : kFallbacks) {
    std::ifstream probe(path, std::ios::binary);
    if (!probe) continue;
    // The fallback family's own faces, so a bold request still gets bold.
    const std::vector<FaceInfo>& faces = probeCached(path);
    if (faces.empty() || !faces[0].ok) continue;
    const std::string family = normalizeFamily(faces[0].family);
    const auto accept = [&](const std::string&, const FaceInfo& f) { return normalizeFamily(f.family) == family; };
    std::vector<std::string> siblings;
    for (const std::string& f : files)
      if (normalizeFamily(fileStem(f)).compare(0, family.size(), family) == 0) siblings.push_back(f);
    if (std::find(siblings.begin(), siblings.end(), std::string(path)) == siblings.end()) siblings.insert(siblings.begin(), path);
    if (choose(siblings, accept)) return true;
    pathOut = path; faceIndexOut = 0;
    return true;
  }
  return false;
}
