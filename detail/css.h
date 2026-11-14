#pragma once
// CSS value parsers for the canvas setters that take CSS text: fillStyle / strokeStyle / shadowColor (<color>), font
// (the 'font' shorthand), filter (<filter-value-list>) and letterSpacing / wordSpacing (<length>).
//
// Header-only and allocation-free for colours and lengths: a tiny CSS tokenizer over std::string_view (no std::string,
// no locale-dependent strtod), fast paths for hex, keywords and plain legacy rgb()/rgba(), named colours through a
// compile-time hash index over a sorted constexpr table. Fonts and filters allocate only for their result vectors.
// The grammar accepted is Chrome's for the canvas setters (probed with headless Chrome 157); where this file differs
// from Chrome the comment at the function says so.
//
// Not supported (the parse fails, so the canvas keeps its previous value): calc() and the other math functions,
// color-mix(), light-dark(), relative colours (`rgb(from ...)`), viewport / container / font-relative units other than
// em, rem, ex and ch, and url() filters.

#include "../canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canvas_detail {

struct CssFontFamily {
    std::string name;
    bool generic = false;
};

struct CssFont {
    float sizePx = 10;
    int weight = 400;         // bolder/lighter resolved against 400 (CSS Fonts 4 §2.2): bolder = 700, lighter = 100
    bool italic = false;      // italic or oblique (a non-zero angle)
    bool smallCaps = false;
    float stretch = 1.0f;     // 1 = normal (100%)
    std::vector<CssFontFamily> families;
};

struct CssFilterFunction {
    enum class Kind : std::uint8_t { Blur, Brightness, Contrast, DropShadow, Grayscale, HueRotate, Invert, Opacity, Saturate, Sepia };
    Kind kind;
    float amount = 0;           // number (1 = 100%); radians for HueRotate; px (sigma = this value) for Blur; px blur radius for DropShadow
    float dx = 0, dy = 0;       // DropShadow offset in px
    Color color{0, 0, 0, 255};  // DropShadow colour
};

namespace css_impl {

// ---- Character classes -------------------------------------------------------------------------------------------

// One table lookup per class test (the tokenizer's hot path).
enum : std::uint8_t { kClsWs = 1, kClsNewline = 2, kClsDigit = 4, kClsAlpha = 8, kClsNameStart = 16, kClsNameChar = 32 };
struct CharClassTable {
    std::uint8_t bits[256];
};
constexpr CharClassTable buildCharClassTable() {
    CharClassTable t{};
    for (int c = 0; c < 256; ++c) {
        std::uint8_t b = 0;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') b |= kClsWs;
        if (c == '\n' || c == '\r' || c == '\f') b |= kClsNewline;
        if (c >= '0' && c <= '9') b |= kClsDigit | kClsNameChar;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) b |= kClsAlpha | kClsNameStart | kClsNameChar;
        if (c == '_' || c >= 0x80) b |= kClsNameStart | kClsNameChar;
        if (c == '-') b |= kClsNameChar;
        t.bits[c] = b;
    }
    return t;
}
inline constexpr CharClassTable kCharClass = buildCharClassTable();
constexpr bool charIs(char c, std::uint8_t cls) { return (kCharClass.bits[static_cast<unsigned char>(c)] & cls) != 0; }
constexpr bool isWs(char c) { return charIs(c, kClsWs); }
constexpr bool isNewline(char c) { return charIs(c, kClsNewline); }
constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }
constexpr bool isAlpha(char c) { return charIs(c, kClsAlpha); }
constexpr bool isNameStart(char c) { return charIs(c, kClsNameStart); }
constexpr bool isNameChar(char c) { return charIs(c, kClsNameChar); }
constexpr char toLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
constexpr int hexVal(char c) {
    return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

// ASCII case-insensitive equality against an all-lowercase literal.
constexpr bool eqLower(std::string_view s, std::string_view lowerLiteral) {
    if (s.size() != lowerLiteral.size()) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (toLower(s[i]) != lowerLiteral[i]) return false;
    return true;
}

constexpr double kPi = 3.14159265358979323846;

inline double pow10Scale(double v, int e) {
    static constexpr double kPow[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (e >= 0) return e <= 22 ? v * kPow[e] : (e > 400 ? v * 1e300 * 1e300 : v * std::pow(10.0, e));
    e = -e;
    return e <= 22 ? v / kPow[e] : (e > 400 ? 0.0 : v / std::pow(10.0, e));
}

inline void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Appends the text of an ident or string body with CSS escapes resolved (CSS Syntax 3 §4.3.7): `\` + 1-6 hex digits
// (+ one optional whitespace) is that code point, `\` + newline is a line continuation, `\` + anything else is it.
inline void appendUnescaped(std::string& out, std::string_view raw) {
    for (std::size_t i = 0; i < raw.size();) {
        char c = raw[i++];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (i >= raw.size()) break;
        char n = raw[i];
        if (n == '\n' || n == '\f') {
            ++i;
        } else if (n == '\r') {
            ++i;
            if (i < raw.size() && raw[i] == '\n') ++i;
        } else if (hexVal(n) >= 0) {
            std::uint32_t cp = 0;
            for (int k = 0; k < 6 && i < raw.size() && hexVal(raw[i]) >= 0; ++k, ++i) cp = cp * 16 + static_cast<std::uint32_t>(hexVal(raw[i]));
            if (i < raw.size() && isWs(raw[i])) i += (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') ? 2 : 1;
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
            appendUtf8(out, cp);
        } else {
            out += n;
            ++i;
        }
    }
}

// A CSS <number> at p (the caller has checked one starts there): [+-]? digits* [. digits+]? [e[+-]?digits+]?.
// Advances p past it. Exact (correctly rounded) for up to 19 significant digits and |exponent| <= 22.
inline double scanNumber(const char*& p, const char* e) {
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    std::uint64_t mant = 0;
    int exp10 = 0, sig = 0;
    for (; p < e && isDigit(*p); ++p) {
        if (sig < 19) {
            mant = mant * 10 + static_cast<std::uint64_t>(*p - '0');
            if (mant) ++sig;
        } else {
            ++exp10;
        }
    }
    if (p + 1 < e && *p == '.' && isDigit(p[1])) {
        for (++p; p < e && isDigit(*p); ++p) {
            if (sig < 19) {
                mant = mant * 10 + static_cast<std::uint64_t>(*p - '0');
                if (mant) ++sig;
                --exp10;
            }
        }
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool eneg = false;
        if (q < e && (*q == '+' || *q == '-')) eneg = *q++ == '-';
        if (q < e && isDigit(*q)) {
            int ev = 0;
            for (; q < e && isDigit(*q); ++q)
                if (ev < 100000) ev = ev * 10 + (*q - '0');
            exp10 += eneg ? -ev : ev;
            p = q;
        }
    }
    double v = static_cast<double>(mant);
    if (mant && exp10) v = pow10Scale(v, exp10);
    return neg ? -v : v;
}

// ---- Tokenizer (CSS Syntax 3 §4, the subset these grammars need) -------------------------------------------------

enum class Tok : std::uint8_t { End, Ident, Function, Number, Percentage, Dimension, Hash, String, Comma, Slash, LParen, RParen, Delim, Bad };

struct Token {
    Tok type = Tok::End;
    bool escaped = false;   // text holds backslash escapes (resolve with appendUnescaped)
    double num = 0;         // Number / Percentage (the number before '%') / Dimension
    std::string_view text;  // Ident / Function (name without '(') / Dimension (unit) / Hash (after '#') / String (body) / Delim
};

// Whitespace and comments between tokens are skipped; nothing here needs them as tokens. End is returned for ever
// once the input is exhausted, which also lets an unclosed function close at end of input as CSS does.
class Lexer {
public:
    explicit Lexer(std::string_view s) : p_(s.data()), e_(s.data() + s.size()) {}

    const char* mark() const { return p_; }
    void reset(const char* m) { p_ = m; }
    Token peek() {
        const char* m = p_;
        Token t = next();
        p_ = m;
        return t;
    }

    Token next() {
        skipWsAndComments();
        Token t;
        if (p_ >= e_) return t;
        char c = *p_;
        if (isDigit(c)) return number();
        if (isNameStart(c)) return identLike();
        switch (c) {
            case ',': ++p_; t.type = Tok::Comma; return t;
            case ')': ++p_; t.type = Tok::RParen; return t;
            case '(': ++p_; t.type = Tok::LParen; return t;
            case '/': ++p_; t.type = Tok::Slash; return t;  // comments were skipped above
            case '"':
            case '\'': return consumeString(c);
            case '#':
                if (p_ + 1 < e_ && (isNameChar(p_[1]) || validEscape(p_ + 1))) {
                    ++p_;
                    t.type = Tok::Hash;
                    t.text = consumeName(t.escaped);
                    return t;
                }
                break;
            case '+':
            case '-':
            case '.':
                if (startsNumber(p_)) return number();
                if (c == '-' && startsIdent(p_)) return identLike();
                break;
            case '\\':
                if (validEscape(p_)) return identLike();
                break;
            default: break;
        }
        t.type = Tok::Delim;
        t.text = std::string_view(p_, 1);
        ++p_;
        return t;
    }

private:
    Token number() {
        Token t;
        t.num = consumeNumber();
        if (p_ < e_ && *p_ == '%') {
            ++p_;
            t.type = Tok::Percentage;
        } else if (p_ < e_ && (isNameStart(*p_) || *p_ == '-' || *p_ == '\\') && startsIdent(p_)) {
            t.type = Tok::Dimension;
            t.text = consumeName(t.escaped);
        } else {
            t.type = Tok::Number;
        }
        return t;
    }
    Token identLike() {
        Token t;
        t.text = consumeName(t.escaped);
        if (p_ < e_ && *p_ == '(') {
            ++p_;
            t.type = Tok::Function;
        } else {
            t.type = Tok::Ident;
        }
        return t;
    }
    void skipWsAndComments() {
        for (;;) {
            while (p_ < e_ && isWs(*p_)) ++p_;
            if (e_ - p_ >= 2 && p_[0] == '/' && p_[1] == '*') {
                const char* q = p_ + 2;
                while (q + 1 < e_ && !(q[0] == '*' && q[1] == '/')) ++q;
                p_ = q + 1 < e_ ? q + 2 : e_;
                continue;
            }
            return;
        }
    }
    bool validEscape(const char* q) const { return q + 1 < e_ && q[0] == '\\' && !isNewline(q[1]); }
    bool startsIdent(const char* q) const {
        if (q >= e_) return false;
        if (*q == '-') {
            ++q;
            return q < e_ && (isNameStart(*q) || *q == '-' || validEscape(q));
        }
        return isNameStart(*q) || validEscape(q);
    }
    bool startsNumber(const char* q) const {
        if (*q == '+' || *q == '-') ++q;
        if (q >= e_) return false;
        if (isDigit(*q)) return true;
        return *q == '.' && q + 1 < e_ && isDigit(q[1]);
    }
    std::string_view consumeName(bool& escaped) {
        const char* s = p_;
        while (p_ < e_) {
            if (isNameChar(*p_)) {
                ++p_;
            } else if (validEscape(p_)) {
                escaped = true;
                ++p_;
                if (hexVal(*p_) >= 0) {
                    for (int k = 0; k < 6 && p_ < e_ && hexVal(*p_) >= 0; ++k) ++p_;
                    if (p_ < e_ && isWs(*p_)) p_ += (*p_ == '\r' && p_ + 1 < e_ && p_[1] == '\n') ? 2 : 1;
                } else {
                    ++p_;
                }
            } else {
                break;
            }
        }
        return std::string_view(s, static_cast<std::size_t>(p_ - s));
    }
    double consumeNumber() { return scanNumber(p_, e_); }
    Token consumeString(char quote) {
        Token t;
        const char* s = ++p_;
        while (p_ < e_) {
            char c = *p_;
            if (c == quote) {
                t.type = Tok::String;
                t.text = std::string_view(s, static_cast<std::size_t>(p_ - s));
                ++p_;
                return t;
            }
            if (isNewline(c)) {
                t.type = Tok::Bad;
                return t;
            }
            ++p_;
            if (c == '\\') {
                t.escaped = true;
                if (p_ < e_) p_ += (*p_ == '\r' && p_ + 1 < e_ && p_[1] == '\n') ? 2 : 1;
            }
        }
        t.type = Tok::String;  // end of input closes a string
        t.text = std::string_view(s, static_cast<std::size_t>(p_ - s));
        return t;
    }

    const char* p_;
    const char* e_;
};

// The function's closing parenthesis, or the end of input (which closes every open block).
inline bool consumeClose(Lexer& lx) {
    Token t = lx.next();
    return t.type == Tok::RParen || t.type == Tok::End;
}

inline bool angleToDegrees(const Token& t, double& deg) {
    if (t.type != Tok::Dimension || t.escaped) return false;
    if (eqLower(t.text, "deg")) deg = t.num;
    else if (eqLower(t.text, "rad")) deg = t.num * (180.0 / kPi);
    else if (eqLower(t.text, "grad")) deg = t.num * 0.9;
    else if (eqLower(t.text, "turn")) deg = t.num * 360.0;
    else return false;
    return true;
}

// px per unit for an absolute or font-relative length unit (CSS Values 4 §6). ex and ch are 0.5em, as when no font
// metrics are available.
inline bool lengthUnitToPx(std::string_view unit, double emPx, double& factor) {
    std::size_t n = unit.size();
    if (n < 1 || n > 3) return false;
    char u[3] = {toLower(unit[0]), n > 1 ? toLower(unit[1]) : '\0', n > 2 ? toLower(unit[2]) : '\0'};
    if (n == 1) {
        if (u[0] != 'q') return false;
        factor = 96.0 / 101.6;
        return true;
    }
    if (n == 3) {
        if (u[0] != 'r' || u[1] != 'e' || u[2] != 'm') return false;
        factor = 16.0;
        return true;
    }
    switch (u[0]) {
        case 'p': if (u[1] == 'x') factor = 1.0; else if (u[1] == 't') factor = 4.0 / 3.0; else if (u[1] == 'c') factor = 16.0; else return false; return true;
        case 'i': if (u[1] != 'n') return false; factor = 96.0; return true;
        case 'c': if (u[1] == 'm') factor = 96.0 / 2.54; else if (u[1] == 'h') factor = emPx * 0.5; else return false; return true;
        case 'm': if (u[1] != 'm') return false; factor = 96.0 / 25.4; return true;
        case 'e': if (u[1] == 'm') factor = emPx; else if (u[1] == 'x') factor = emPx * 0.5; else return false; return true;
        default: return false;
    }
}

// A <length> token in px: a dimension with a length unit, or (allowUnitlessZero) the number 0.
inline bool tokenToPx(const Token& t, double emPx, bool allowUnitlessZero, double& px) {
    if (t.type == Tok::Number && allowUnitlessZero && t.num == 0) {
        px = 0;
        return true;
    }
    double factor = 0;
    if (t.type != Tok::Dimension || t.escaped || !lengthUnitToPx(t.text, emPx, factor)) return false;
    px = t.num * factor;
    return std::isfinite(px);
}

// ---- Colour keywords -------------------------------------------------------------------------------------------

struct ColorEntry {
    std::string_view name;
    std::uint32_t value;
};

// CSS Color 4 §6.1: the 148 named colours, 0xRRGGBB, sorted by name.
inline constexpr ColorEntry kNamedColors[] = {
    {"aliceblue", 0xF0F8FFU}, {"antiquewhite", 0xFAEBD7U}, {"aqua", 0x00FFFFU}, {"aquamarine", 0x7FFFD4U},
    {"azure", 0xF0FFFFU}, {"beige", 0xF5F5DCU}, {"bisque", 0xFFE4C4U}, {"black", 0x000000U},
    {"blanchedalmond", 0xFFEBCDU}, {"blue", 0x0000FFU}, {"blueviolet", 0x8A2BE2U}, {"brown", 0xA52A2AU},
    {"burlywood", 0xDEB887U}, {"cadetblue", 0x5F9EA0U}, {"chartreuse", 0x7FFF00U}, {"chocolate", 0xD2691EU},
    {"coral", 0xFF7F50U}, {"cornflowerblue", 0x6495EDU}, {"cornsilk", 0xFFF8DCU}, {"crimson", 0xDC143CU},
    {"cyan", 0x00FFFFU}, {"darkblue", 0x00008BU}, {"darkcyan", 0x008B8BU}, {"darkgoldenrod", 0xB8860BU},
    {"darkgray", 0xA9A9A9U}, {"darkgreen", 0x006400U}, {"darkgrey", 0xA9A9A9U}, {"darkkhaki", 0xBDB76BU},
    {"darkmagenta", 0x8B008BU}, {"darkolivegreen", 0x556B2FU}, {"darkorange", 0xFF8C00U}, {"darkorchid", 0x9932CCU},
    {"darkred", 0x8B0000U}, {"darksalmon", 0xE9967AU}, {"darkseagreen", 0x8FBC8FU}, {"darkslateblue", 0x483D8BU},
    {"darkslategray", 0x2F4F4FU}, {"darkslategrey", 0x2F4F4FU}, {"darkturquoise", 0x00CED1U}, {"darkviolet", 0x9400D3U},
    {"deeppink", 0xFF1493U}, {"deepskyblue", 0x00BFFFU}, {"dimgray", 0x696969U}, {"dimgrey", 0x696969U},
    {"dodgerblue", 0x1E90FFU}, {"firebrick", 0xB22222U}, {"floralwhite", 0xFFFAF0U}, {"forestgreen", 0x228B22U},
    {"fuchsia", 0xFF00FFU}, {"gainsboro", 0xDCDCDCU}, {"ghostwhite", 0xF8F8FFU}, {"gold", 0xFFD700U},
    {"goldenrod", 0xDAA520U}, {"gray", 0x808080U}, {"green", 0x008000U}, {"greenyellow", 0xADFF2FU},
    {"grey", 0x808080U}, {"honeydew", 0xF0FFF0U}, {"hotpink", 0xFF69B4U}, {"indianred", 0xCD5C5CU},
    {"indigo", 0x4B0082U}, {"ivory", 0xFFFFF0U}, {"khaki", 0xF0E68CU}, {"lavender", 0xE6E6FAU},
    {"lavenderblush", 0xFFF0F5U}, {"lawngreen", 0x7CFC00U}, {"lemonchiffon", 0xFFFACDU}, {"lightblue", 0xADD8E6U},
    {"lightcoral", 0xF08080U}, {"lightcyan", 0xE0FFFFU}, {"lightgoldenrodyellow", 0xFAFAD2U}, {"lightgray", 0xD3D3D3U},
    {"lightgreen", 0x90EE90U}, {"lightgrey", 0xD3D3D3U}, {"lightpink", 0xFFB6C1U}, {"lightsalmon", 0xFFA07AU},
    {"lightseagreen", 0x20B2AAU}, {"lightskyblue", 0x87CEFAU}, {"lightslategray", 0x778899U}, {"lightslategrey", 0x778899U},
    {"lightsteelblue", 0xB0C4DEU}, {"lightyellow", 0xFFFFE0U}, {"lime", 0x00FF00U}, {"limegreen", 0x32CD32U},
    {"linen", 0xFAF0E6U}, {"magenta", 0xFF00FFU}, {"maroon", 0x800000U}, {"mediumaquamarine", 0x66CDAAU},
    {"mediumblue", 0x0000CDU}, {"mediumorchid", 0xBA55D3U}, {"mediumpurple", 0x9370DBU}, {"mediumseagreen", 0x3CB371U},
    {"mediumslateblue", 0x7B68EEU}, {"mediumspringgreen", 0x00FA9AU}, {"mediumturquoise", 0x48D1CCU}, {"mediumvioletred", 0xC71585U},
    {"midnightblue", 0x191970U}, {"mintcream", 0xF5FFFAU}, {"mistyrose", 0xFFE4E1U}, {"moccasin", 0xFFE4B5U},
    {"navajowhite", 0xFFDEADU}, {"navy", 0x000080U}, {"oldlace", 0xFDF5E6U}, {"olive", 0x808000U},
    {"olivedrab", 0x6B8E23U}, {"orange", 0xFFA500U}, {"orangered", 0xFF4500U}, {"orchid", 0xDA70D6U},
    {"palegoldenrod", 0xEEE8AAU}, {"palegreen", 0x98FB98U}, {"paleturquoise", 0xAFEEEEU}, {"palevioletred", 0xDB7093U},
    {"papayawhip", 0xFFEFD5U}, {"peachpuff", 0xFFDAB9U}, {"peru", 0xCD853FU}, {"pink", 0xFFC0CBU},
    {"plum", 0xDDA0DDU}, {"powderblue", 0xB0E0E6U}, {"purple", 0x800080U}, {"rebeccapurple", 0x663399U},
    {"red", 0xFF0000U}, {"rosybrown", 0xBC8F8FU}, {"royalblue", 0x4169E1U}, {"saddlebrown", 0x8B4513U},
    {"salmon", 0xFA8072U}, {"sandybrown", 0xF4A460U}, {"seagreen", 0x2E8B57U}, {"seashell", 0xFFF5EEU},
    {"sienna", 0xA0522DU}, {"silver", 0xC0C0C0U}, {"skyblue", 0x87CEEBU}, {"slateblue", 0x6A5ACDU},
    {"slategray", 0x708090U}, {"slategrey", 0x708090U}, {"snow", 0xFFFAFAU}, {"springgreen", 0x00FF7FU},
    {"steelblue", 0x4682B4U}, {"tan", 0xD2B48CU}, {"teal", 0x008080U}, {"thistle", 0xD8BFD8U},
    {"tomato", 0xFF6347U}, {"turquoise", 0x40E0D0U}, {"violet", 0xEE82EEU}, {"wheat", 0xF5DEB3U},
    {"white", 0xFFFFFFU}, {"whitesmoke", 0xF5F5F5U}, {"yellow", 0xFFFF00U}, {"yellowgreen", 0x9ACD32U},
};

// CSS Color 4 §6.2 system colours and the deprecated §6.3 names, 0xRRGGBBAA, as Chrome 157's canvas resolves them
// (light colour scheme). accentcolor / accentcolortext and the -webkit- aliases are not colours to Chrome's canvas.
inline constexpr ColorEntry kSystemColors[] = {
    {"activeborder", 0x000000FFU},      {"activecaption", 0xFFFFFFFFU},   {"activetext", 0xFF0000FFU},
    {"appworkspace", 0xFFFFFFFFU},      {"background", 0xFFFFFFFFU},      {"buttonborder", 0x000000FFU},
    {"buttonface", 0xEFEFEFFFU},        {"buttonhighlight", 0xEFEFEFFFU}, {"buttonshadow", 0xEFEFEFFFU},
    {"buttontext", 0x000000FFU},        {"canvas", 0xFFFFFFFFU},          {"canvastext", 0x000000FFU},
    {"captiontext", 0x000000FFU},       {"field", 0xFFFFFFFFU},           {"fieldtext", 0x000000FFU},
    {"graytext", 0x808080FFU},          {"highlight", 0x80BCFE99U},       {"highlighttext", 0x000000FFU},
    {"inactiveborder", 0x000000FFU},    {"inactivecaption", 0xFFFFFFFFU}, {"inactivecaptiontext", 0x808080FFU},
    {"infobackground", 0xFFFFFFFFU},    {"infotext", 0x000000FFU},        {"linktext", 0x0000EEFFU},
    {"mark", 0xFFFF00FFU},              {"marktext", 0x000000FFU},        {"menu", 0xFFFFFFFFU},
    {"menutext", 0x000000FFU},          {"scrollbar", 0xFFFFFFFFU},       {"selecteditem", 0xB3D7FFFFU},
    {"selecteditemtext", 0x000000FFU},  {"threeddarkshadow", 0x000000FFU}, {"threedface", 0xEFEFEFFFU},
    {"threedhighlight", 0x000000FFU},   {"threedlightshadow", 0x000000FFU}, {"threedshadow", 0x000000FFU},
    {"visitedtext", 0x551A8BFFU},       {"window", 0xFFFFFFFFU},          {"windowframe", 0x000000FFU},
    {"windowtext", 0x000000FFU},
};

template <std::size_t N>
constexpr bool isSortedByName(const ColorEntry (&table)[N]) {
    for (std::size_t i = 1; i < N; ++i)
        if (!(table[i - 1].name < table[i].name)) return false;
    return true;
}
static_assert(sizeof(kNamedColors) / sizeof(kNamedColors[0]) == 148, "CSS defines 148 named colours");
static_assert(isSortedByName(kNamedColors), "kNamedColors must be sorted for binary search");
static_assert(isSortedByName(kSystemColors), "kSystemColors must be sorted for binary search");

// Binary search of a sorted [first, last) for a lowercase key.
inline const ColorEntry* findColorEntry(const ColorEntry* first, const ColorEntry* last, std::string_view key) {
    const ColorEntry* it = std::lower_bound(first, last, key, [](const ColorEntry& e, std::string_view k) { return e.name < k; });
    return (it != last && it->name == key) ? it : nullptr;
}

// A compile-time open-addressing hash index over kNamedColors (FNV-1a of the lowercase name, linear probing, 512
// slots for 148 names): a lookup is one hash pass and usually one comparison.
constexpr std::uint32_t kFnvBasis = 2166136261u, kFnvPrime = 16777619u;
constexpr std::uint32_t kNamedHashMask = 511;
struct NamedHashIndex {
    std::uint8_t slot[kNamedHashMask + 1];  // 1 + index into kNamedColors; 0 = empty
};
constexpr NamedHashIndex buildNamedHashIndex() {
    NamedHashIndex ix{};
    constexpr std::size_t n = sizeof(kNamedColors) / sizeof(kNamedColors[0]);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t h = kFnvBasis;
        for (char c : kNamedColors[i].name) h = (h ^ static_cast<unsigned char>(c)) * kFnvPrime;
        h &= kNamedHashMask;
        while (ix.slot[h]) h = (h + 1) & kNamedHashMask;
        ix.slot[h] = static_cast<std::uint8_t>(i + 1);
    }
    return ix;
}
inline constexpr NamedHashIndex kNamedHashIndex = buildNamedHashIndex();

// A colour keyword (named, transparent, currentcolor, system), ASCII case-insensitive.
inline bool lookupColorKeyword(std::string_view name, Color& out) {
    constexpr std::size_t kMax = 20;  // "lightgoldenrodyellow", "inactivecaptiontext" (19)
    std::size_t n = name.size();
    if (n < 3 || n > kMax) return false;
    char buf[kMax];
    std::uint32_t h = kFnvBasis;
    for (std::size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!isAlpha(c)) return false;
        buf[i] = static_cast<char>(c | 0x20);
        h = (h ^ static_cast<unsigned char>(buf[i])) * kFnvPrime;
    }
    std::string_view key(buf, n);
    for (h &= kNamedHashMask; kNamedHashIndex.slot[h]; h = (h + 1) & kNamedHashMask) {
        const ColorEntry& e = kNamedColors[kNamedHashIndex.slot[h] - 1];
        if (e.name == key) {
            out = Color(static_cast<std::uint8_t>(e.value >> 16), static_cast<std::uint8_t>(e.value >> 8), static_cast<std::uint8_t>(e.value), 255);
            return true;
        }
    }
    if (key == "transparent") {
        out = Color(0, 0, 0, 0);
        return true;
    }
    if (key == "currentcolor") {  // an offscreen canvas has no element colour: opaque black
        out = Color(0, 0, 0, 255);
        return true;
    }
    if (const ColorEntry* e = findColorEntry(std::begin(kSystemColors), std::end(kSystemColors), key)) {
        out = Color(static_cast<std::uint8_t>(e->value >> 24), static_cast<std::uint8_t>(e->value >> 16), static_cast<std::uint8_t>(e->value >> 8),
                    static_cast<std::uint8_t>(e->value));
        return true;
    }
    return false;
}

// CSS Color 4 §5.2: 3, 4, 6 or 8 hex digits (after the '#').
inline bool parseHexDigits(std::string_view s, Color& out) {
    std::size_t n = s.size();
    if (n != 3 && n != 4 && n != 6 && n != 8) return false;
    int d[8];
    for (std::size_t i = 0; i < n; ++i)
        if ((d[i] = hexVal(s[i])) < 0) return false;
    auto pair = [&](int i) { return static_cast<std::uint8_t>(d[i] * 16 + d[i + 1]); };
    auto dup = [&](int i) { return static_cast<std::uint8_t>(d[i] * 17); };
    if (n <= 4) out = Color(dup(0), dup(1), dup(2), n == 4 ? dup(3) : 255);
    else out = Color(pair(0), pair(2), pair(4), n == 8 ? pair(6) : 255);
    return true;
}

// ---- Colour spaces (CSS Color 4 §8-§10; matrices are the exact rational forms of the spec's conversions.js) -------

struct Vec3 {
    double x, y, z;
};
using Mat3 = double[3][3];

inline Vec3 mul(const Mat3& m, const Vec3& v) {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

inline constexpr Mat3 kXyzD65ToLinSrgb = {{12831.0 / 3959, -329.0 / 214, -1974.0 / 3959},
                                          {-851781.0 / 878810, 1648619.0 / 878810, 36519.0 / 878810},
                                          {705.0 / 12673, -2585.0 / 12673, 705.0 / 667}};
inline constexpr Mat3 kLinP3ToXyzD65 = {{608311.0 / 1250200, 189793.0 / 714400, 198249.0 / 1000160},
                                        {35783.0 / 156275, 247089.0 / 357200, 198249.0 / 2500400},
                                        {0.0, 32229.0 / 714400, 5220557.0 / 5000800}};
inline constexpr Mat3 kLinA98ToXyzD65 = {{573536.0 / 994567, 263643.0 / 1420810, 187206.0 / 994567},
                                         {591459.0 / 1989134, 6239551.0 / 9945670, 374412.0 / 4972835},
                                         {53769.0 / 1989134, 351524.0 / 4972835, 4929758.0 / 4972835}};
inline constexpr Mat3 kLinProphotoToXyzD50 = {{0.79776664490064230, 0.13518129740053308, 0.03134773412839220},
                                              {0.28807482881940130, 0.71183523424187300, 0.00008993693872564},
                                              {0.0, 0.0, 0.82510460251046020}};
inline constexpr Mat3 kLin2020ToXyzD65 = {{63426534.0 / 99577255, 20160776.0 / 139408157, 47086771.0 / 278816314},
                                          {26158966.0 / 99577255, 472592308.0 / 697040785, 8267143.0 / 139408157},
                                          {0.0, 19567812.0 / 697040785, 295819943.0 / 278816314}};
inline constexpr Mat3 kXyzD50ToD65 = {{0.955473421488075, -0.02309845494876471, 0.06325924320057072},
                                      {-0.0283697093338637, 1.0099953980813041, 0.021041441191917323},
                                      {0.012314014864481998, -0.020507649298898964, 1.330365926242124}};
inline constexpr Mat3 kOklabToLms = {{1.0, 0.3963377773761749, 0.2158037573099136},
                                     {1.0, -0.1055613458156586, -0.0638541728258133},
                                     {1.0, -0.0894841775298119, -1.2914855480194092}};
inline constexpr Mat3 kLmsToXyzD65 = {{1.2268798758459243, -0.5578149944602171, 0.2813910456659647},
                                      {-0.0405757452148008, 1.1122868032803170, -0.0717110580655164},
                                      {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816}};

inline double signOf(double v) { return v < 0 ? -1.0 : 1.0; }
// Transfer functions, mirrored about zero (CSS Color 4 §10) so out-of-range input stays monotonic before clipping.
inline double srgbDecode(double v) {
    double a = std::fabs(v);
    return signOf(v) * (a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4));
}
inline double srgbEncode(double v) {
    double a = std::fabs(v);
    return signOf(v) * (a > 0.0031308 ? 1.055 * std::pow(a, 1 / 2.4) - 0.055 : 12.92 * a);
}
inline double a98Decode(double v) { return signOf(v) * std::pow(std::fabs(v), 563.0 / 256); }
inline double prophotoDecode(double v) {
    double a = std::fabs(v);
    return a <= 16.0 / 512 ? v / 16 : signOf(v) * std::pow(a, 1.8);
}
inline double rec2020Decode(double v) {
    constexpr double kAlpha = 1.09929682680944, kBeta = 0.018053968510807;
    double a = std::fabs(v);
    return a < kBeta * 4.5 ? v / 4.5 : signOf(v) * std::pow((a + kAlpha - 1) / kAlpha, 1 / 0.45);
}

inline Vec3 xyzD65ToSrgb(const Vec3& xyz) {
    Vec3 l = mul(kXyzD65ToLinSrgb, xyz);
    return {srgbEncode(l.x), srgbEncode(l.y), srgbEncode(l.z)};
}

// CSS Color 4 §9.3 Lab (D50 white) -> sRGB.
inline Vec3 labToSrgb(double L, double a, double b) {
    constexpr double kKappa = 24389.0 / 27, kEpsilon = 216.0 / 24389;
    constexpr double kWx = 0.3457 / 0.3585, kWz = (1.0 - 0.3457 - 0.3585) / 0.3585;
    double f1 = (L + 16) / 116, f0 = a / 500 + f1, f2 = f1 - b / 200;
    auto inv = [&](double f) {
        double c = f * f * f;
        return c > kEpsilon ? c : (116 * f - 16) / kKappa;
    };
    double y = L > kKappa * kEpsilon ? f1 * f1 * f1 : L / kKappa;
    return xyzD65ToSrgb(mul(kXyzD50ToD65, Vec3{inv(f0) * kWx, y, inv(f2) * kWz}));
}

// CSS Color 4 §9.4 Oklab -> sRGB.
inline Vec3 oklabToSrgb(double L, double a, double b) {
    Vec3 lms = mul(kOklabToLms, Vec3{L, a, b});
    lms = {lms.x * lms.x * lms.x, lms.y * lms.y * lms.y, lms.z * lms.z * lms.z};
    return xyzD65ToSrgb(mul(kLmsToXyzD65, lms));
}

// CSS Color 4 §7.1: HSL (hue in degrees, s and l in 0..1) -> sRGB.
inline Vec3 hslToSrgb(double h, double s, double l) {
    double a = s * std::min(l, 1 - l);
    auto f = [&](double n) {
        double k = std::fmod(n + h / 30, 12.0);
        return l - a * std::max(-1.0, std::min(std::min(k - 3, 9 - k), 1.0));
    };
    return {f(0), f(8), f(4)};
}

inline double normalizeHue(double deg) {
    if (!std::isfinite(deg)) return 0;
    deg = std::fmod(deg, 360.0);
    return deg < 0 ? deg + 360 : deg;
}

inline double clamp01(double v) { return v > 0 ? (v < 1 ? v : 1) : 0; }  // NaN -> 0

// 0..1 -> 0..255, rounding half up as Chrome does (rgb(10.5 0 0) is 11; alpha 0.5 is 128).
inline std::uint8_t unitTo8(double v) { return static_cast<std::uint8_t>(std::floor(clamp01(v) * 255.0 + 0.5)); }
inline std::uint8_t byteTo8(double v) { return v > 0 ? (v < 255 ? static_cast<std::uint8_t>(std::floor(v + 0.5)) : 255) : 0; }

// ---- Colour functions ------------------------------------------------------------------------------------------

enum class CompKind : std::uint8_t { None, Number, Percent, Angle };
struct Comp {
    CompKind kind = CompKind::None;
    double v = 0;  // the number, the percentage (50% -> 50) or the angle in degrees
};

inline bool readComp(Lexer& lx, Comp& c) {
    Token t = lx.next();
    switch (t.type) {
        case Tok::Number: c = {CompKind::Number, t.num}; return true;
        case Tok::Percentage: c = {CompKind::Percent, t.num}; return true;
        case Tok::Dimension: c.kind = CompKind::Angle; return angleToDegrees(t, c.v);
        case Tok::Ident:
            if (t.escaped || !eqLower(t.text, "none")) return false;
            c = {CompKind::None, 0};
            return true;
        default: return false;
    }
}

// A component scaled so that 100% is `full` and a bare number is multiplied by `perNumber`; none is 0.
inline double compValue(const Comp& c, double full, double perNumber = 1) {
    switch (c.kind) {
        case CompKind::Number: return c.v * perNumber;
        case CompKind::Percent: return c.v * full / 100;
        default: return 0;
    }
}
inline double hueValue(const Comp& c) { return c.kind == CompKind::None ? 0 : normalizeHue(c.v); }

enum class ColorFn : std::uint8_t { Rgb, Hsl, Hwb, Lab, Lch, Oklab, Oklch, Color };
enum class ColorSpace : std::uint8_t { Srgb, SrgbLinear, DisplayP3, A98Rgb, ProphotoRgb, Rec2020, XyzD50, XyzD65 };

inline bool colorFunctionKind(std::string_view name, ColorFn& fn) {
    switch (name.size()) {
        case 3:
            if (eqLower(name, "rgb")) fn = ColorFn::Rgb;
            else if (eqLower(name, "hsl")) fn = ColorFn::Hsl;
            else if (eqLower(name, "hwb")) fn = ColorFn::Hwb;
            else if (eqLower(name, "lab")) fn = ColorFn::Lab;
            else if (eqLower(name, "lch")) fn = ColorFn::Lch;
            else return false;
            return true;
        case 4:
            if (eqLower(name, "rgba")) fn = ColorFn::Rgb;
            else if (eqLower(name, "hsla")) fn = ColorFn::Hsl;
            else return false;
            return true;
        case 5:
            if (eqLower(name, "oklab")) fn = ColorFn::Oklab;
            else if (eqLower(name, "oklch")) fn = ColorFn::Oklch;
            else if (eqLower(name, "color")) fn = ColorFn::Color;
            else return false;
            return true;
        default: return false;
    }
}

inline bool predefinedColorSpace(std::string_view name, ColorSpace& s) {
    if (eqLower(name, "srgb")) s = ColorSpace::Srgb;
    else if (eqLower(name, "srgb-linear")) s = ColorSpace::SrgbLinear;
    else if (eqLower(name, "display-p3")) s = ColorSpace::DisplayP3;
    else if (eqLower(name, "a98-rgb")) s = ColorSpace::A98Rgb;
    else if (eqLower(name, "prophoto-rgb")) s = ColorSpace::ProphotoRgb;
    else if (eqLower(name, "rec2020")) s = ColorSpace::Rec2020;
    else if (eqLower(name, "xyz") || eqLower(name, "xyz-d65")) s = ColorSpace::XyzD65;
    else if (eqLower(name, "xyz-d50")) s = ColorSpace::XyzD50;
    else return false;
    return true;
}

inline Vec3 predefinedToSrgb(ColorSpace s, const Vec3& v) {
    auto map = [](const Vec3& c, double (*f)(double)) { return Vec3{f(c.x), f(c.y), f(c.z)}; };
    switch (s) {
        case ColorSpace::Srgb: return v;
        case ColorSpace::SrgbLinear: return map(v, srgbEncode);
        case ColorSpace::DisplayP3: return xyzD65ToSrgb(mul(kLinP3ToXyzD65, map(v, srgbDecode)));
        case ColorSpace::A98Rgb: return xyzD65ToSrgb(mul(kLinA98ToXyzD65, map(v, a98Decode)));
        case ColorSpace::ProphotoRgb: return xyzD65ToSrgb(mul(kXyzD50ToD65, mul(kLinProphotoToXyzD50, map(v, prophotoDecode))));
        case ColorSpace::Rec2020: return xyzD65ToSrgb(mul(kLin2020ToXyzD65, map(v, rec2020Decode)));
        case ColorSpace::XyzD50: return xyzD65ToSrgb(mul(kXyzD50ToD65, v));
        case ColorSpace::XyzD65: return xyzD65ToSrgb(v);
    }
    return v;
}

// The body of a colour function, after its Function token, through the closing parenthesis.
//   Legacy (rgb/rgba/hsl/hsla only): commas, no `none`; rgb all numbers or all percentages; hsl percentages for s, l.
//   Modern: space-separated, each component number | percentage | none (hue: number | angle | none), "/ alpha".
// Components are clamped as Chrome's canvas does: rgb to 0..255; hsl s and l to 0..100% when both are percentages
// (Chrome's fast path), otherwise only to >= 0 (s or l > 100% is left alone); hwb w and b to >= 0; lab/lch L to
// 0..100 and oklab/oklch L to 0..1; chroma to >= 0; color() components not at all. The sRGB result is then clipped
// to the gamut (no CSS gamut mapping -- Chrome's canvas clips too, except for wildly out-of-range color() input).
inline bool parseColorFunctionBody(ColorFn fn, Lexer& lx, Color& out) {
    ColorSpace space = ColorSpace::Srgb;
    if (fn == ColorFn::Color) {
        Token s = lx.next();
        if (s.type != Tok::Ident || s.escaped || !predefinedColorSpace(s.text, space)) return false;
    }
    Comp c[3];
    Comp alpha{CompKind::Number, 1};
    if (!readComp(lx, c[0])) return false;
    bool legacy = false;
    if (fn == ColorFn::Rgb || fn == ColorFn::Hsl) {
        const char* m = lx.mark();
        if (lx.next().type == Tok::Comma) legacy = true;
        else lx.reset(m);
    }
    if (legacy) {
        if (!readComp(lx, c[1]) || lx.next().type != Tok::Comma || !readComp(lx, c[2])) return false;
        Token t = lx.next();
        if (t.type == Tok::Comma) {
            if (!readComp(lx, alpha)) return false;
            t = lx.next();
        }
        if (t.type != Tok::RParen && t.type != Tok::End) return false;
        if (alpha.kind == CompKind::None || alpha.kind == CompKind::Angle) return false;
        for (const Comp& k : c)
            if (k.kind == CompKind::None) return false;
        if (fn == ColorFn::Rgb) {
            if (c[0].kind == CompKind::Angle || c[1].kind != c[0].kind || c[2].kind != c[0].kind) return false;
        } else if (c[0].kind == CompKind::Percent || c[1].kind != CompKind::Percent || c[2].kind != CompKind::Percent) {
            return false;
        }
    } else {
        if (!readComp(lx, c[1]) || !readComp(lx, c[2])) return false;
        Token t = lx.next();
        if (t.type == Tok::Slash) {
            if (!readComp(lx, alpha)) return false;
            t = lx.next();
        }
        if (t.type != Tok::RParen && t.type != Tok::End) return false;
        int hue = (fn == ColorFn::Hsl || fn == ColorFn::Hwb) ? 0 : (fn == ColorFn::Lch || fn == ColorFn::Oklch) ? 2 : -1;
        for (int i = 0; i < 3; ++i) {
            if (i == hue ? c[i].kind == CompKind::Percent : c[i].kind == CompKind::Angle) return false;
        }
        if (alpha.kind == CompKind::Angle) return false;
    }
    std::uint8_t a8 = unitTo8(compValue(alpha, 1));
    Vec3 rgb{0, 0, 0};
    switch (fn) {
        case ColorFn::Rgb:
            out = Color(byteTo8(compValue(c[0], 255)), byteTo8(compValue(c[1], 255)), byteTo8(compValue(c[2], 255)), a8);
            return true;
        case ColorFn::Hsl: {
            double sat = compValue(c[1], 1, 0.01), light = compValue(c[2], 1, 0.01);
            if (c[1].kind == CompKind::Percent && c[2].kind == CompKind::Percent) {
                sat = clamp01(sat);  // Chrome's fast path for the common all-percentage form clamps both
                light = clamp01(light);
            } else {
                sat = std::max(sat, 0.0);  // otherwise only negative s (CSS Color 4 §7) and negative l (Chrome) clamp
                light = std::max(light, 0.0);
            }
            rgb = hslToSrgb(hueValue(c[0]), sat, light);
            break;
        }
        case ColorFn::Hwb: {  // negative w / b clamp to 0, > 100% does not (Chrome); w + b >= 1 is a gray
            double w = std::max(compValue(c[1], 1, 0.01), 0.0), b = std::max(compValue(c[2], 1, 0.01), 0.0);
            if (w + b >= 1) {
                double gray = w / (w + b);
                rgb = {gray, gray, gray};
            } else {
                rgb = hslToSrgb(hueValue(c[0]), 1, 0.5);
                rgb = {rgb.x * (1 - w - b) + w, rgb.y * (1 - w - b) + w, rgb.z * (1 - w - b) + w};
            }
            break;
        }
        case ColorFn::Lab:
            rgb = labToSrgb(std::min(std::max(compValue(c[0], 100), 0.0), 100.0), compValue(c[1], 125), compValue(c[2], 125));
            break;
        case ColorFn::Lch: {
            double C = std::max(compValue(c[1], 150), 0.0), h = hueValue(c[2]) * (kPi / 180);
            rgb = labToSrgb(std::min(std::max(compValue(c[0], 100), 0.0), 100.0), C * std::cos(h), C * std::sin(h));
            break;
        }
        case ColorFn::Oklab:
            rgb = oklabToSrgb(clamp01(compValue(c[0], 1)), compValue(c[1], 0.4), compValue(c[2], 0.4));
            break;
        case ColorFn::Oklch: {
            double C = std::max(compValue(c[1], 0.4), 0.0), h = hueValue(c[2]) * (kPi / 180);
            rgb = oklabToSrgb(clamp01(compValue(c[0], 1)), C * std::cos(h), C * std::sin(h));
            break;
        }
        case ColorFn::Color:
            rgb = predefinedToSrgb(space, Vec3{compValue(c[0], 1), compValue(c[1], 1), compValue(c[2], 1)});
            break;
    }
    out = Color(unitTo8(rgb.x), unitTo8(rgb.y), unitTo8(rgb.z), a8);
    return true;
}

// One <color> from the token stream.
inline bool parseColorToken(Lexer& lx, Color& out) {
    Token t = lx.next();
    switch (t.type) {
        case Tok::Hash: return !t.escaped && parseHexDigits(t.text, out);
        case Tok::Ident: return !t.escaped && lookupColorKeyword(t.text, out);
        case Tok::Function: {
            ColorFn fn;
            return !t.escaped && colorFunctionKind(t.text, fn) && parseColorFunctionBody(fn, lx, out);
        }
        default: return false;
    }
}

// Fast path for the commonest generated form, rgb[a](<n>, <n>, <n>[, <n>]) with unsigned plain numbers and optional
// spaces. Returns false to defer to the tokenizer, which gives the identical result for everything accepted here
// (same number scanner, same clamping and rounding).
inline bool fastLegacyRgb(std::string_view s, Color& out) {
    const char* p = s.data();
    const char* e = p + s.size();
    if (e - p < 10 || (p[0] | 0x20) != 'r' || (p[1] | 0x20) != 'g' || (p[2] | 0x20) != 'b') return false;
    p += 3;
    if ((*p | 0x20) == 'a') ++p;
    if (*p++ != '(') return false;
    double v[4];
    int n = 0;
    for (;;) {
        while (p < e && *p == ' ') ++p;
        if (p >= e || !(isDigit(*p) || (*p == '.' && p + 1 < e && isDigit(p[1])))) return false;
        v[n++] = scanNumber(p, e);
        if (p < e && (isNameStart(*p) || *p == '%' || *p == '-' || *p == '\\')) return false;  // a unit / percentage
        while (p < e && *p == ' ') ++p;
        if (p < e && *p == ',' && n < 4) {
            ++p;
            continue;
        }
        break;
    }
    if (n < 3 || p + 1 != e || *p != ')') return false;
    out = Color(byteTo8(v[0]), byteTo8(v[1]), byteTo8(v[2]), n == 4 ? unitTo8(v[3]) : 255);
    return true;
}

inline std::string_view trimWs(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && isWs(s[b])) ++b;
    while (e > b && isWs(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// ---- Fonts -------------------------------------------------------------------------------------------------------

inline bool isGenericFamily(std::string_view lower) {
    static constexpr std::string_view kGenerics[] = {"cursive",   "emoji",     "fangsong",      "fantasy",      "math",
                                                     "monospace", "sans-serif", "serif",        "system-ui",    "ui-monospace",
                                                     "ui-rounded", "ui-sans-serif", "ui-serif"};
    for (std::string_view g : kGenerics)
        if (g == lower) return true;
    return false;
}

// CSS-wide keywords and `default` cannot be unquoted family names on their own.
inline bool isReservedFamilyWord(std::string_view s) {
    return eqLower(s, "inherit") || eqLower(s, "initial") || eqLower(s, "unset") || eqLower(s, "revert") ||
           eqLower(s, "revert-layer") || eqLower(s, "default");
}

inline std::string asciiLowerCopy(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = toLower(c);
    return r;
}

// Chrome's serialization: a family is printed bare if it is a plain identifier that is not a keyword, else quoted.
inline bool familyNeedsQuotes(const std::string& name) {
    if (name.empty()) return true;
    std::size_t i = name[0] == '-' ? 1 : 0;
    if (i >= name.size() || !isNameStart(name[i])) return true;
    for (; i < name.size(); ++i)
        if (!isNameChar(name[i])) return true;
    std::string lower = asciiLowerCopy(name);
    return isGenericFamily(lower) || isReservedFamilyWord(lower);
}

inline void appendInt(std::string& out, long long v) {
    char buf[24];
    char* p = buf + sizeof buf;
    unsigned long long u = v < 0 ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        *--p = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    out.append(p, static_cast<std::size_t>(buf + sizeof buf - p));
}

// Up to 6 significant digits, as Chrome prints ctx.font sizes ("8.33333px").
inline void appendCssNumber(std::string& out, double v) {
    if (std::fabs(v) < 1e15 && v == std::floor(v)) {
        appendInt(out, static_cast<long long>(v));
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    for (char* p = buf; *p; ++p)
        if (*p == ',') *p = '.';  // a comma-decimal C locale
    out += buf;
}

}  // namespace css_impl

// ===================================================================================================================

// CSS Color 4 <color> as canvas fillStyle/strokeStyle/shadowColor accept it. Rounding: channel = floor(v * 255 + 0.5)
// (round half up, as Chrome: rgb(10.5, 0, 0) -> 11, 50% -> 128, alpha 0.5 -> 128). Also accepts the CSS system colours
// with Chrome's light-scheme values. Returns false (out untouched) for anything invalid.
inline bool parseCssColor(std::string_view text, Color& out) {
    using namespace css_impl;
    std::string_view s = trimWs(text);
    if (s.empty()) return false;
    // Fast paths for "#hex", a bare keyword and plain legacy rgb()/rgba(); anything else (and anything they reject) goes
    // through the tokenizer.
    if (s[0] == '#' ? parseHexDigits(s.substr(1), out) : (isAlpha(s[0]) && (lookupColorKeyword(s, out) || fastLegacyRgb(s, out))))
        return true;
    Lexer lx(s);
    Color c;
    if (!parseColorToken(lx, c) || lx.next().type != Tok::End) return false;
    out = c;
    return true;
}

// The CSS 'font' shorthand as CanvasRenderingContext2D.font accepts it (CSS Fonts 4 §2.7):
//   [ <font-style> || <font-variant-css2> || <font-weight> || <font-stretch-css3> ]? <font-size> [ / <line-height> ]?
//   <font-family>#
// em, ex, ch (0.5em), %, larger (x1.2), smaller (/1.2) are relative to parentSizePx; rem to 16px; line-height is
// validated and ignored. Generic families are reported lowercased with generic = true; quoted names and unquoted
// identifier sequences (joined by single spaces) keep their case.
// Like Chrome: sizes are clamped to 10000px; the weight number is truncated ("450.6" -> 450); `oblique` (with a
// non-zero angle in [-90deg, 90deg]) sets italic; `math` as a size is the parent size; a generic family keyword may not
// start a multi-word unquoted family ("serif Foo" is invalid, "Foo serif" is the family "Foo serif"); CSS-wide keywords
// and `default` are invalid only as a one-word family. Unlike Chrome, system fonts (caption, menu, ...) are rejected,
// stretch percentages are not accepted (the shorthand takes stretch keywords only), and calc() / viewport units fail.
inline bool parseCssFont(std::string_view text, CssFont& out, float parentSizePx = 10.0f) {
    using namespace css_impl;
    Lexer lx(text);
    CssFont f;
    bool haveStyle = false, haveVariant = false, haveWeight = false, haveStretch = false;
    // [ <font-style> || <font-variant-css2> || <font-weight> || <font-stretch-css3> ]?  (`normal` fills any slot)
    for (int item = 0; item < 4; ++item) {
        const char* m = lx.mark();
        Token t = lx.next();
        if (t.type == Tok::Number && t.num >= 1 && t.num <= 1000) {
            if (haveWeight) return false;
            haveWeight = true;
            f.weight = static_cast<int>(t.num);
            continue;
        }
        if (t.type == Tok::Ident && !t.escaped) {
            std::string_view k = t.text;
            if (eqLower(k, "normal")) continue;
            if (eqLower(k, "italic") || eqLower(k, "oblique")) {
                if (haveStyle) return false;
                haveStyle = true;
                f.italic = true;
                if (eqLower(k, "oblique")) {
                    const char* m2 = lx.mark();
                    double deg = 0;
                    if (angleToDegrees(lx.next(), deg)) {
                        if (!(deg >= -90 && deg <= 90)) return false;
                        f.italic = deg != 0;
                    } else {
                        lx.reset(m2);
                    }
                }
                continue;
            }
            if (eqLower(k, "small-caps")) {
                if (haveVariant) return false;
                haveVariant = true;
                f.smallCaps = true;
                continue;
            }
            int weight = eqLower(k, "bold") ? 700 : eqLower(k, "bolder") ? 700 : eqLower(k, "lighter") ? 100 : 0;
            if (weight) {
                if (haveWeight) return false;
                haveWeight = true;
                f.weight = weight;
                continue;
            }
            static constexpr std::pair<std::string_view, float> kStretch[] = {
                {"ultra-condensed", 0.5f}, {"extra-condensed", 0.625f}, {"condensed", 0.75f}, {"semi-condensed", 0.875f},
                {"semi-expanded", 1.125f}, {"expanded", 1.25f},         {"extra-expanded", 1.5f}, {"ultra-expanded", 2.0f}};
            bool stretched = false;
            for (const auto& s : kStretch) {
                if (eqLower(k, s.first)) {
                    if (haveStretch) return false;
                    haveStretch = stretched = true;
                    f.stretch = s.second;
                    break;
                }
            }
            if (stretched) continue;
        }
        lx.reset(m);
        break;
    }
    // <font-size>
    double size = 0, parent = parentSizePx;
    {
        Token t = lx.next();
        if (t.type == Tok::Percentage) {
            size = parent * t.num / 100;
        } else if (t.type == Tok::Dimension) {
            double factor = 0;
            if (t.escaped || !lengthUnitToPx(t.text, parent, factor)) return false;
            size = t.num * factor;
        } else if (t.type == Tok::Number) {
            if (t.num != 0) return false;  // unitless zero only
        } else if (t.type == Tok::Ident && !t.escaped) {
            static constexpr std::pair<std::string_view, double> kSizes[] = {
                {"xx-small", 9}, {"x-small", 10}, {"small", 13},     {"medium", 16},
                {"large", 18},   {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
            bool found = false;
            for (const auto& s : kSizes) {
                if (eqLower(t.text, s.first)) {
                    size = s.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (eqLower(t.text, "larger")) size = parent * 1.2;
                else if (eqLower(t.text, "smaller")) size = parent / 1.2;
                else if (eqLower(t.text, "math")) size = parent;
                else return false;
            }
        } else {
            return false;
        }
        if (!(size >= 0) || !std::isfinite(size)) return false;
        size = std::min(size, 10000.0);
    }
    // [ / <line-height> ]?  -- validated and ignored
    {
        const char* m = lx.mark();
        if (lx.next().type == Tok::Slash) {
            Token t = lx.next();
            double px = 0;
            bool ok = (t.type == Tok::Ident && !t.escaped && eqLower(t.text, "normal")) ||
                      ((t.type == Tok::Number || t.type == Tok::Percentage) && t.num >= 0) ||
                      (tokenToPx(t, parent, false, px) && px >= 0);
            if (!ok) return false;
        } else {
            lx.reset(m);
        }
    }
    // <font-family>#
    for (;;) {
        Token t = lx.next();
        CssFontFamily fam;
        if (t.type == Tok::String) {
            if (t.escaped) appendUnescaped(fam.name, t.text);
            else fam.name.assign(t.text);
        } else if (t.type == Tok::Ident) {
            int words = 0;
            bool firstGeneric = false;
            for (;;) {
                if (words) fam.name += ' ';
                std::size_t start = fam.name.size();
                if (t.escaped) appendUnescaped(fam.name, t.text);
                else fam.name.append(t.text);
                if (words == 0) firstGeneric = isGenericFamily(asciiLowerCopy(std::string_view(fam.name).substr(start)));
                ++words;
                const char* m = lx.mark();
                Token n = lx.next();
                if (n.type != Tok::Ident) {
                    lx.reset(m);
                    break;
                }
                t = n;
            }
            if (words == 1) {
                if (isReservedFamilyWord(fam.name)) return false;
                if (firstGeneric) {
                    fam.name = asciiLowerCopy(fam.name);
                    fam.generic = true;
                }
            } else if (firstGeneric) {
                return false;
            }
        } else {
            return false;
        }
        f.families.push_back(std::move(fam));
        Token sep = lx.next();
        if (sep.type == Tok::End) break;
        if (sep.type != Tok::Comma) return false;
    }
    f.sizePx = static_cast<float>(size);
    out = std::move(f);
    return true;
}

// The canvas font getter's string, as Chrome builds it: "[italic ][<weight> ][small-caps ]<size>px <families>", the
// weight as "bold" for 700 and a number for anything else but 400, families joined by ", " and double-quoted unless
// they are plain identifiers (generic families print bare). Chrome leaves stretch out of ctx.font (it is reported by
// ctx.fontStretch), so this does too.
inline std::string serializeCssFont(const CssFont& f) {
    using namespace css_impl;
    std::string s;
    s.reserve(32);
    if (f.italic) s += "italic ";
    if (f.weight == 700) {
        s += "bold ";
    } else if (f.weight != 400) {
        appendInt(s, f.weight);
        s += ' ';
    }
    if (f.smallCaps) s += "small-caps ";
    appendCssNumber(s, f.sizePx);
    s += "px";
    for (std::size_t i = 0; i < f.families.size(); ++i) {
        const CssFontFamily& fam = f.families[i];
        s += i ? ", " : " ";
        if (fam.generic || !familyNeedsQuotes(fam.name)) {
            s += fam.name;
            continue;
        }
        s += '"';
        for (char c : fam.name) {
            if (c == '"' || c == '\\') s += '\\';
            s += c;
        }
        s += '"';
    }
    return s;
}

// canvas.filter: "none" or a list of CSS filter functions (whitespace optional between them). Like Chrome, a negative
// amount or blur is invalid and a bare number is not an angle (except 0); amounts above 100% for grayscale / invert /
// opacity / sepia are accepted and clamped to 1. Unlike Chrome, url() filters are rejected.
inline bool parseCssFilter(std::string_view text, std::vector<CssFilterFunction>& out) {
    using namespace css_impl;
    using Kind = CssFilterFunction::Kind;
    constexpr double kEm = 10;
    Lexer lx(text);
    {
        Lexer probe = lx;
        Token t = probe.next();
        if (t.type == Tok::Ident && !t.escaped && eqLower(t.text, "none")) {
            if (probe.next().type != Tok::End) return false;
            out.clear();
            return true;
        }
    }
    static constexpr std::pair<std::string_view, Kind> kNames[] = {
        {"blur", Kind::Blur},       {"brightness", Kind::Brightness}, {"contrast", Kind::Contrast}, {"drop-shadow", Kind::DropShadow},
        {"grayscale", Kind::Grayscale}, {"hue-rotate", Kind::HueRotate}, {"invert", Kind::Invert}, {"opacity", Kind::Opacity},
        {"saturate", Kind::Saturate}, {"sepia", Kind::Sepia}};
    std::vector<CssFilterFunction> list;
    for (Token t = lx.next(); t.type != Tok::End; t = lx.next()) {
        if (t.type != Tok::Function || t.escaped) return false;
        const std::pair<std::string_view, Kind>* entry = nullptr;
        for (const auto& e : kNames)
            if (eqLower(t.text, e.first)) entry = &e;
        if (!entry) return false;  // unknown function, or url()
        CssFilterFunction fn{entry->second};
        const char* argStart = lx.mark();
        Token a = lx.next();
        bool empty = a.type == Tok::RParen || a.type == Tok::End;
        double v = 0;
        switch (fn.kind) {
            case Kind::Blur:  // <length>? (default 0), not negative
                if (!empty && (!tokenToPx(a, kEm, true, v) || v < 0 || !consumeClose(lx))) return false;
                fn.amount = static_cast<float>(v);
                break;
            case Kind::HueRotate:  // [ <angle> | <zero> ]? (default 0)
                if (!empty) {
                    if (!(a.type == Tok::Number && a.num == 0) && !angleToDegrees(a, v)) return false;
                    if (!consumeClose(lx)) return false;
                }
                fn.amount = static_cast<float>(v * (kPi / 180));
                break;
            case Kind::DropShadow: {  // [ <color>? && <length>{2,3} ]: the lengths contiguous, the colour before or after
                lx.reset(argStart);
                double len[3] = {0, 0, 0};
                int n = 0;
                bool haveColor = false, lengthsClosed = false;
                for (;;) {
                    const char* m = lx.mark();
                    Token x = lx.next();
                    if (x.type == Tok::RParen || x.type == Tok::End) break;
                    if (x.type == Tok::Number || x.type == Tok::Dimension) {
                        if (lengthsClosed || n == 3 || !tokenToPx(x, kEm, true, len[n])) return false;
                        ++n;
                        continue;
                    }
                    if (haveColor) return false;
                    lx.reset(m);
                    if (!parseColorToken(lx, fn.color)) return false;
                    haveColor = true;
                    lengthsClosed = n > 0;
                }
                if (n < 2 || len[2] < 0) return false;
                fn.dx = static_cast<float>(len[0]);
                fn.dy = static_cast<float>(len[1]);
                fn.amount = static_cast<float>(len[2]);
                break;
            }
            default:  // <number> | <percentage>, default 100%, not negative
                v = 1;
                if (!empty) {
                    if (a.type == Tok::Number) v = a.num;
                    else if (a.type == Tok::Percentage) v = a.num / 100;
                    else return false;
                    if (!(v >= 0) || !std::isfinite(v) || !consumeClose(lx)) return false;
                }
                if (fn.kind == Kind::Grayscale || fn.kind == Kind::Invert || fn.kind == Kind::Opacity || fn.kind == Kind::Sepia)
                    v = std::min(v, 1.0);
                fn.amount = static_cast<float>(v);
                break;
        }
        list.push_back(fn);
    }
    if (list.empty()) return false;
    out = std::move(list);
    return true;
}

// A CSS <length> in px, as canvas letterSpacing / wordSpacing take it: a number with a unit (px pt pc in cm mm q, em
// relative to emPx, rem to 16px, ex and ch 0.5em); negative allowed. Like Chrome's letterSpacing, a unitless number
// (even 0) and percentages are invalid.
inline bool parseCssLength(std::string_view text, float& outPx, float emPx = 10.0f) {
    using namespace css_impl;
    Lexer lx(text);
    double px = 0;
    if (!tokenToPx(lx.next(), emPx, false, px) || lx.next().type != Tok::End) return false;
    outPx = static_cast<float>(px);
    return true;
}

}  // namespace canvas_detail
