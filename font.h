#pragma once

// sfnt (TrueType and CFF/OpenType) glyph outlines, decoded to Path2D.
//
// Text is produced as PATHS rather than as a rasterized bitmap cache. That is
// what makes fillText and strokeText fall out of the code that already exists:
// a glyph is filled and stroked by the same routines as any other path, so
// outlined text, transforms, clipping and alpha all work with no extra machinery.
//
// What is read: TrueType 'glyf' outlines (simple and composite) and CFF ('OTTO'
// with a 'CFF ' table, including CID-keyed fonts) Type 2 charstrings; cmap
// formats 0/4/6/12/13; hmtx advances; kerning from whichever source HarfBuzz
// (so Chrome) would use: the GPOS 'kern' feature (PairPos formats 1 and 2 and
// SinglePos, also behind Extension lookups), AAT 'kerx' (formats 0 and 2) or
// the legacy 'kern' table (OpenType and Apple headers, formats 0, 2 and 3);
// hhea/OS/2/post metrics; 'name' family and style; every face of a .ttc.
//
// What is not: CFF2 (variable CFF), variations (fvar/gvar/HVAR: the default
// instance is drawn), 'trak' tracking, hinting, WOFF/WOFF2, GSUB/morx
// (ligatures, contextual forms), contextual/state-machine kerning (kerx/kern
// format 1, GPOS contextual lookups), mark positioning, vertical layout, seac
// accent composition in CFF.
//
// THREAD SAFETY: none. Glyph outlines are decoded lazily into a cache that the
// const accessors fill in, so one Font must not be used from two threads at once
// without external locking. Separate Font objects are independent.

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "canvas.h"

class Font {
public:
    // Parses a .ttf / .otf / .ttc file. Returns false on anything it cannot
    // read; fonts are user-supplied data and a malformed one must not crash.
    // The one-argument forms load face 0 of a collection.
    bool loadFromFile(const std::string& path);
    bool loadFromMemory(std::vector<std::uint8_t> data);
    bool loadFromFile(const std::string& path, int faceIndex);
    bool loadFromMemory(std::vector<std::uint8_t> data, int faceIndex);
    bool valid() const { return valid_; }

    // Faces in `data`: the collection's count for a .ttc, 1 for a lone sfnt,
    // 0 for anything else. No face is parsed; a face that is counted may still
    // fail to load.
    static int faceCount(const std::vector<std::uint8_t>& data);
    int faceIndex() const { return faceIndex_; }

    // Searches the platform's usual font directories for the first family in
    // `preferred` that exists, then any usable fallback. Names are matched
    // case-insensitively against the file name.
    static bool findSystemFont(const std::vector<std::string>& preferred, std::string& pathOut);

    // As above, but by FAMILY NAME (the 'name' table's, matched ignoring case,
    // spaces, '-' and '_'; a name that equals a file's own name also matches
    // every face in it, so the file names the old overload takes still work).
    // Every face of every candidate file is considered -- each face of a .ttc,
    // and sibling files such as "Georgia Bold Italic.ttf" -- and the best match
    // for `weight` (100..900) and `italic` is chosen roughly as CSS does:
    // normal width first, then the right slant, then the nearest weight in the
    // CSS search direction. The first preferred family with any face wins; if
    // none exists a platform fallback is chosen the same way.
    static bool findSystemFont(const std::vector<std::string>& preferred, int weight, bool italic,
                               std::string& pathOut, int& faceIndexOut);

    // Appends `text` to `path`, laid out left to right starting at the text
    // origin (x, baseline y), scaled so that em == pixelSize. Pair kerning is
    // applied between consecutive glyphs while kerningEnabled() (the default)
    // -- except across a word delimiter (see isWordDelimiter()), because
    // Chrome shapes canvas text one space-delimited word at a time and so
    // never kerns a space against its neighbours (Helvetica's space-A, -112
    // units, is in its kern table and absent from Chrome's measureText).
    void appendText(Path2D& path, const std::string& utf8, float x, float y, float pixelSize) const;

    // Sum of advances plus pair kerning (while enabled), as appendText lays out.
    float measure(const std::string& utf8, float pixelSize) const;

    // U+0020 and U+0009: where Chrome's word-by-word shaping splits a run, so
    // no kerning pair involving one is applied. A layout built on kerning()
    // should skip such pairs to agree with measure().
    static bool isWordDelimiter(std::uint32_t codepoint);

    // Whether every character of `utf8` has a glyph of its own in this face.
    // One that does not is drawn as .notdef here -- and by a browser in some
    // other font entirely, at a width this face knows nothing about.
    bool covers(const std::string& utf8) const;

    // Vertical metrics scaled to pixelSize, for baseline placement.
    float ascent(float pixelSize) const;
    float descent(float pixelSize) const;   // negative, as in the font
    float lineHeight(float pixelSize) const;

    // ---- Glyph level -------------------------------------------------------
    // What a glyph-mask cache and a text layout sit on. Glyph ids are the
    // font's own; 0 is .notdef. All of these are cheap after the first use of
    // a glyph (its outline and ink box are decoded once and cached).

    // The glyph `codepoint` maps to through the cmap; 0 (.notdef) if none.
    std::uint16_t glyphIndex(std::uint32_t codepoint) const;
    // Number of glyphs in the face (maxp.numGlyphs).
    std::uint16_t glyphCount() const { return valid_ ? numGlyphs_ : 0; }
    // Horizontal advance in pixels at em == pixelSize (hmtx, unhinted). While
    // kerning is enabled this includes any per-glyph XAdvance the 'kern'
    // feature applies through GPOS SinglePos (rare; e.g. Euphemia UCAS), so a
    // pen walk of glyphAdvance + kerning always equals measure().
    float glyphAdvance(std::uint16_t glyph, float pixelSize) const;
    // Pair adjustment to add to the pen between `left` and `right`, in pixels.
    // 0 when the pair has none or kerning is disabled. From GPOS this is the
    // first glyph's XAdvance plus the second glyph's XPlacement (the latter is
    // applied as advance, i.e. it also moves every following glyph -- exact
    // for the usual value2-less kerning, an approximation otherwise).
    float kerning(std::uint16_t left, std::uint16_t right, float pixelSize) const;
    // The same in font units, unscaled.
    int kerningUnits(std::uint16_t left, std::uint16_t right) const;
    // Appends one glyph with its pen at (x, baseline y), y down, em ==
    // pixelSize, and marks `path` as glyph outlines (Path2D::markGlyphOutlines).
    void appendGlyph(Path2D& path, std::uint16_t glyph, float x, float y, float pixelSize) const;
    // The glyph's ink box at pixelSize, y down, relative to the pen at the
    // baseline origin -- tight (curve extrema, not control points). False and
    // all zero for a glyph with no outline (a space, or an id out of range).
    bool glyphBounds(std::uint16_t glyph, float pixelSize, float& x0, float& y0, float& x1, float& y1) const;
    // The cached outline in FONT UNITS, y already flipped to screen down, or
    // nullptr for a glyph without one. The pointer stays valid until the font
    // is reloaded or destroyed (or, only once a pathological font has filled
    // the cache's memory budget, until the next glyph is decoded).
    const Path2D* glyphOutline(std::uint16_t glyph) const;
    // Pair kerning on (default) or off for measure(), appendText(), kerning().
    void setKerning(bool enabled) { kerning_ = enabled; }
    bool kerningEnabled() const { return kerning_; }
    // Whether the face carries any pair-kerning data this decoder applies.
    bool hasKerning() const;
    // Whether outlines come from CFF charstrings (cubic) rather than glyf.
    bool isCFF() const { return cff_.charStrings != 0; }

    // ---- Face metrics ------------------------------------------------------
    struct FaceMetrics {
        float ascent = 0, descent = 0;     // descent positive, below baseline
        float lineGap = 0;
        float xHeight = 0, capHeight = 0;
        float underlinePosition = 0;       // top of the stroke; positive = below baseline
        float underlineThickness = 0;
        float strikeoutPosition = 0;       // top of the stroke; positive = above baseline
        float strikeoutThickness = 0;
    };
    // Scaled to em == pixelSize. ascent/descent are what Chrome on macOS
    // reports as fontBoundingBoxAscent/Descent: CoreText's (= hhea's) ascender
    // and descender, each rounded to a whole pixel (unrounded below 3px, as
    // Blink does for canvas), with Blink's +15% ascent hack for the Times,
    // Helvetica and Courier families. CoreText -- hence Chrome on the Mac --
    // ignores OS/2 USE_TYPO_METRICS; see useTypoMetrics() for the bit itself.
    // lineGap is hhea's, unrounded. xHeight/capHeight come from OS/2 when it
    // has them, else from the ink of 'x' and 'H'; underline from 'post';
    // strikeout from OS/2 (each with a plausible default when absent).
    FaceMetrics faceMetrics(float pixelSize) const;
    // OS/2 fsSelection bit 7. Not used by faceMetrics() (see there).
    bool useTypoMetrics() const { return useTypoMetrics_; }

    // ---- Face identity -----------------------------------------------------
    // 'name' typographic family (ID 16), else family (ID 1); style likewise
    // (17, else 2). UTF-8.
    std::string familyName() const { return familyName_; }
    std::string styleName() const { return styleName_; }
    // OS/2 usWeightClass (100..900), guessed from the style name without OS/2.
    int weight() const { return weight_; }
    // OS/2 fsSelection italic or oblique, else head.macStyle, else the style name.
    bool italic() const { return italic_; }

    // The face's raw numbers, in font units, for a tool that writes them out
    // ahead of time rather than reading the file at run time. What measure(),
    // covers(), ascent() and descent() compute is a function of exactly these.
    struct GlyphMetrics {
        std::uint16_t advance = 0;
        // The glyf header's box, y up (for CFF, the outline's ink box rounded
        // outwards). All zero for a blank glyph (a space).
        std::int16_t xMin = 0, yMin = 0, xMax = 0, yMax = 0;
    };
    std::uint16_t unitsPerEm() const { return unitsPerEm_; }
    // The em box's split at the baseline, which ascent() and descent() scale.
    std::int16_t emAscent() const { return ascent_; }
    std::int16_t emDescent() const { return descent_; }
    // Every codepoint the face maps to a glyph of its own, ascending.
    std::vector<std::uint32_t> mappedCodepoints() const;
    // The glyph `codepoint` is set in -- .notdef for one the face lacks.
    GlyphMetrics metrics(std::uint32_t codepoint) const;
    // The codepoint at `i`, advancing past it: how measure() and covers() read
    // a string. Invalid bytes come out as U+FFFD, one byte at a time.
    static std::uint32_t nextCodepoint(const std::string& utf8, std::size_t& i);

private:
    struct Glyph {
        Path2D path;          // in font units, y already flipped to screen down
        float advance = 0;    // font units
        float xMin = 0, yMin = 0, xMax = 0, yMax = 0;   // ink box, font units, y down
        float shift = 0;      // x moved to honour hmtx's lsb (TrueType); undone in composites
        bool ink = false;
        bool loaded = false;
    };
    // A CFF INDEX, located: item i is [data + off(i), data + off(i+1)).
    struct CffIndex {
        std::uint32_t count = 0, offsets = 0, data = 0;
        std::uint8_t offSize = 0;
    };
    struct Cff {
        std::uint32_t base = 0, end = 0;          // the 'CFF ' table
        std::uint32_t charStrings = 0;            // nonzero once parsed
        CffIndex charStringIndex, globalSubrs, localSubrs;
        std::vector<CffIndex> fdSubrs;            // CID-keyed: per Font DICT
        std::uint32_t fdSelect = 0;               // absolute; 0 if not CID
        std::uint8_t fdSelectFormat = 0;
        float scale = 1;                          // FontMatrix vs unitsPerEm
    };
    enum class KernSource : unsigned char { None, Gpos, Kern, Kerx };
    struct KernSubtable {
        std::uint32_t offset = 0, end = 0;        // the subtable's body
        std::uint8_t format = 0;
        bool apple = false, override_ = false;
    };
    struct GposLookup {
        std::uint16_t type = 0;                   // 1 SinglePos, 2 PairPos
        std::vector<std::uint32_t> subtables;     // absolute, Extension resolved
    };
    struct CharstringState;

    void reset();
    bool parse(int faceIndex);
    void parseMetricsTables();
    void parseNames();
    void parseKerning();
    bool parseCff();
    std::uint32_t tableOffset(const char* tag) const;
    std::uint32_t tableLength(const char* tag) const;
    const Glyph& glyph(std::uint16_t index) const;
    void buildGlyph(std::uint16_t index, Glyph& out) const;
    void buildSimpleGlyph(std::uint32_t offset, std::uint32_t length, Glyph& out) const;
    void buildCompositeGlyph(std::uint32_t offset, std::uint32_t length, Glyph& out, int depth) const;
    void buildCffGlyph(std::uint16_t index, Glyph& out) const;
    int runCharstring(CharstringState& st, std::uint32_t start, std::uint32_t end, int depth) const;
    bool cffItem(const CffIndex& index, std::uint32_t i, std::uint32_t& start, std::uint32_t& end) const;
    bool readCffIndex(std::uint32_t at, CffIndex& out, std::uint32_t& next) const;
    std::uint32_t glyphOffset(std::uint32_t index) const;
    std::int16_t leftSideBearing(std::uint16_t index) const;
    std::uint16_t advanceWidth(std::uint16_t index) const;
    std::uint16_t lookupCmap(std::uint32_t codepoint) const;
    int gposPairValue(std::uint32_t subtable, std::uint16_t left, std::uint16_t right, bool& matched) const;
    int singleAdjustUnits(std::uint16_t glyph) const;
    int legacyKernValue(const KernSubtable& st, std::uint16_t left, std::uint16_t right, bool& matched) const;
    int kerxValue(const KernSubtable& st, std::uint16_t left, std::uint16_t right, bool& matched) const;

    std::vector<std::uint8_t> data_;
    bool valid_ = false;
    int faceIndex_ = 0;
    bool kerning_ = true;

    std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> tables_;
    // Hot tables, located once.
    std::uint32_t glyf_ = 0, loca_ = 0, hmtx_ = 0, hmtxEnd_ = 0;
    std::uint16_t unitsPerEm_ = 1000;
    std::int16_t indexToLocFormat_ = 0;
    std::uint16_t numGlyphs_ = 0;
    std::uint16_t numberOfHMetrics_ = 0;
    std::int16_t ascent_ = 800, descent_ = -200, lineGap_ = 0;
    std::uint32_t cmapOffset_ = 0;
    std::uint16_t cmapFormat_ = 0;
    std::uint8_t cmapKind_ = 0;     // 0 Unicode, 1 Windows Symbol, 2 Mac Roman

    // hhea, OS/2 and post, raw font units.
    std::int16_t hheaAscender_ = 0, hheaDescender_ = 0, hheaLineGap_ = 0;
    bool haveHhea_ = false, haveOs2_ = false, useTypoMetrics_ = false;
    std::int16_t typoAscender_ = 0, typoDescender_ = 0, typoLineGap_ = 0;
    std::uint16_t winAscent_ = 0, winDescent_ = 0;
    std::int16_t xHeight_ = 0, capHeight_ = 0;
    std::int16_t strikeoutSize_ = 0, strikeoutPosition_ = 0;
    std::int16_t underlinePosition_ = 0, underlineThickness_ = 0;
    bool havePost_ = false;

    std::string familyName_, styleName_;
    int weight_ = 400;
    bool italic_ = false;

    Cff cff_;

    KernSource kernSource_ = KernSource::None;
    std::vector<GposLookup> gposKernLookups_;                   // the 'kern' feature's lookups
    bool gposSingle_ = false;                                   // any SinglePos among them
    std::vector<KernSubtable> kernSubtables_;                   // 'kern' or 'kerx'

    // Glyph cache: slot_[glyph] is 0 (not decoded), kBuilding, or 1 + the
    // glyph's position in cache_ (a deque, so references stay put).
    mutable std::vector<std::uint32_t> slot_;
    mutable std::deque<Glyph> cache_;
    mutable std::size_t cachedSegments_ = 0;
    mutable Glyph scratch_;
    mutable int buildDepth_ = 0;
};
