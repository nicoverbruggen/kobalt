// =============================================================================
// TTB (top-to-bottom) glyph metrics cache for vertical-rl/lr rendering.
//
// Fork-only header.  No upstream counterpart.  Extracted to keep the
// per-face vertical-metrics machinery out of lvfntman.cpp and reduce
// soft-fork divergence.
//
// Used by DrawTextString in vertical mode when the font has vhea/vmtx
// tables (FT_HAS_VERTICAL): glyphs are positioned by vertBearingX/Y +
// vertAdvance instead of the horizontal hmtx bearings.  This yields
// uniform placement across all fonts, including ones (like Hiragino
// Mincho Pro) whose horizontal bitmap_top varies per glyph.
// =============================================================================
#ifndef __LV_FNT_MAN_VERT_H_INCLUDED__
#define __LV_FNT_MAN_VERT_H_INCLUDED__

#include "lvtypes.h"
#include "lvhashtable.h"

// Forward-declare FT_Face so this header doesn't pull in FreeType.
struct FT_FaceRec_;
typedef struct FT_FaceRec_ * FT_Face;
typedef struct hb_font_t hb_font_t;
class LVDrawBuf;

// Paint CSS text decorations for a run laid out on a vertical inline axis.
// Kept in the fork-only implementation so the upstream font manager only
// retains its call sites. `target_h` is the decoration owner's physical edge
// when the corresponding hint is set.
bool drawVerticalTextDecorations(
        LVDrawBuf * buf, int x, int y, int width, int advance,
        int font_size, int font_height, int thickness,
        int text_decoration_back_gap, int target_h, lUInt32 flags);

struct VertGlyphMetrics {
    lInt16  origin_x;   // vertBearingX in pixels
    lInt16  origin_y;   // vertBearingY in pixels
    lUInt16 advance;    // vertAdvance in pixels
};

// Raw 26.6 metrics used by the LuaTeX-ja-style ordinary-glyph capsule.
// These come from the HarfBuzz font because it carries the exact same
// FreeType load/hinting flags as the raster cache.
struct VertBodyGlyphMetrics {
    lInt32 x_bearing;
    lInt32 y_bearing;
    lInt32 h_advance;
    lInt32 ascender;
};

// True for characters that should be placed by the central-baseline /
// virtual-body model in vertical-rl/lr layout (JLReq, CSS Writing Modes 3,
// upTeX/LuaTeX-ja JFM): ideographs and syllabic kana/Hangul that occupy a
// uniform 1em virtual body centred on the column axis.  The caller centres
// horizontal advance (not the bitmap rectangle) and uses a common ascender
// baseline, matching LuaTeX-ja's ordinary-glyph capsule.
//
// Excludes:
//   - CJK punctuation block (U+3001..U+303F): position is by font design
//   - Halfwidth/Fullwidth Forms (U+FF00..U+FFEF): mixed semantics
//   - Vertical marks (ー — — ‥ … etc.): handled via LFNT_HINT_VERTICAL_MARK
bool isUniformVerticalIdeograph(lChar32 c);

// =============================================================================
// JLReq vertical character class — LuaTeX-ja-style JFM (Japanese Font Metric).
//
// LuaTeX-ja groups characters into JFM classes that share the same in-slot
// placement rules (slot width in halves of em, alignment within slot).  This
// makes punctuation/bracket placement font-independent: rather than trusting
// each font's vmtx (which differs vendor-to-vendor for 、。「」), the renderer
// applies JLReq-conformant rules per class.
//
// Class set mirrors jfm-ujisv.lua (canonical vertical JFM, jis-derived).
// Numeric order matches the reference for documentation purposes; do not
// rely on it programmatically.
//
// Reference: github.com/luatexja/luatexja  src/jfm-ujisv.lua
// =============================================================================
// Class list and layout fields ported from LuaTeX-ja src/jfm-ujisv.lua.
// JLREQ_VERT_VERT_MARK has no counterpart in jfm-ujisv — it is a fork-only
// auxiliary class for font-quality compensation, dispatched on
// LFNT_HINT_VERTICAL_MARK rather than the classifier.
enum JLReqVertClass {
    JLREQ_VERT_CJK_BODY = 0,        // jfm-ujisv [0]: default body CJK
                                    //   width = 1.0 em, align = middle
                                    //   (includes ー 〜 ～ which are not
                                    //    explicitly listed in jfm-ujisv)
    JLREQ_VERT_OPEN_BRACKET,        // jfm-ujisv [1]: ‘ “ 〈 《 「 『 【 〔 〖 〘 〝 （ ［ ｛ ｟
                                    //   width = 0.5 em, align = right
    JLREQ_VERT_CLOSE_BRACKET_COMMA, // jfm-ujisv [2]: ’ ” 〉 》 」 』 】 〕 〗 〙 〟 ） ］ ｝ ｠ 、 ，
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_MIDDLE_DOT,          // jfm-ujisv [3]: ・ ： ； ·
                                    //   width = 0.5 em, align = middle
    JLREQ_VERT_PERIOD,              // jfm-ujisv [4]: 。 ．
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_DASH,                // jfm-ujisv [5]: — ― ‥ … 〳 〴 〵
                                    //   width = 1.0 em, align = left
    JLREQ_VERT_HALF_DASH,           // jfm-ujisv [105]: ゠ – (half-width dash)
                                    //   width = 0.5 em, align = middle
    JLREQ_VERT_EXCLAM_QUEST,        // jfm-ujisv [6]: ？ ！ ‼ ⁇ ⁈ ⁉
                                    //   width = 1.0 em, align = left
    JLREQ_VERT_HALF_KANA,           // jfm-ujisv [7]: U+FF61..U+FF9F (halfwidth)
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_BOX_DRAWING,         // jfm-ujisv [8]: box drawing characters
                                    //   width = 1.0 em, align = left
    JLREQ_VERT_COMBINING_MARK,      // jfm-ujisv [307]: combining dakuten/handakuten
                                    //   width = 0, align = right
    JLREQ_VERT_VERT_MARK,           // fork-only compensation — NOT in jfm-ujisv.
                                    //   Dispatched on LFNT_HINT_VERTICAL_MARK,
                                    //   not on classifier output.  See lvfntman.cpp.
    JLREQ_VERT_KANA_REPEAT,         // jfm-ujisv [200]: 〱 〲 (vertical kana repeat marks)
                                    //   width = 2.0 em, align = middle.
                                    //   These spans TWO column slots; the font's
                                    //   glyph is designed to fill 2em of column depth.
    JLREQ_VERT_OTHER                // Latin/numerals/etc — rotation path or default
};

// Returns the JLReq vertical class for `c`.  Note: this classifier never
// returns JLREQ_VERT_VERT_MARK — that fork-only class is signalled separately
// via LFNT_HINT_VERTICAL_MARK / isJapaneseHorizontalMark() at the call sites,
// and only appears as a defensive case in the layout/glue switches below.
JLReqVertClass getJLReqVertClass(lChar32 c);

// Per-class JFM layout (LuaTeX-ja jfm-ujisv.lua port).
//
//   width_halves    1 = half-em (= em/2), 2 = full em.  This is the slot's
//                   advance dimension (Y in vertical-rl).  Half-em classes
//                   are JLReq's "compacted" punctuation/brackets that sit
//                   close to their target glyph.
//   align           Alignment within the slot in the advance direction:
//                   JLREQ_ALIGN_LEFT   = start of slot (top in tate)
//                   JLREQ_ALIGN_MIDDLE = centre
//                   JLREQ_ALIGN_RIGHT  = end of slot (bottom in tate)
//                   For full-em slots whose glyph is also full em, align is
//                   moot (no shift).  For half-em slots, align decides where
//                   the glyph sits within the smaller slot, leaving the
//                   other half as compaction whitespace.
//
// X position (perpendicular to advance, = column axis in vertical-rl) is
// NOT controlled by JFM — that comes from the font's vmtx vBX, applied
// uniformly for all classes.  JFM only governs advance-direction layout.
enum JLReqVertAlign {
    JLREQ_ALIGN_LEFT   = 0,
    JLREQ_ALIGN_MIDDLE = 1,
    JLREQ_ALIGN_RIGHT  = 2,
};

struct JLReqVertLayout {
    lUInt8 width_halves;
    lUInt8 align;
};

JLReqVertLayout getJLReqVertLayout(JLReqVertClass cls);

// JFM slot width (advance dimension) for `c`, in pixels.  For named JFM
// classes returns `em_px * width_halves / 2` (em or em/2).  For
// JLREQ_VERT_OTHER (chars not in jfm-ujisv — e.g. Latin glyphs reaching
// this path via a buggy classifier or future extension), returns
// `natural_advance_px` so the font's own advance is preserved.
int getJLReqVertSlotWidth(lChar32 c, int em_px, int natural_advance_px);

// Convenience: JFM in-slot shift (cwa) in the advance direction for `c`,
// in pixels.  Implements `cwa = align * (fwidth - vadv)` from LuaTeX-ja
// ltj-setwidth.lua capsule_glyph_tate() line 269.  `vadv_px` is the font's
// natural vertical advance for the glyph (em for ordinary CJK, but 2em+ for
// composite kana-repeat / dash glyphs supplied by the font's +vrt2 feature),
// and fwidth is taken from getJLReqVertSlotWidth so it matches the slot width
// Phase 3 applied.  Returns a NEGATIVE value for half-em classes with
// align >= MIDDLE (shifting the glyph "backward" = up in tate to overlap the
// previous slot), and 0 for a glyph that already fills its slot.
int getJLReqVertCwa(lChar32 c, int em_px, int vadv_px);

// LuaTeX-ja vform table port (ltj-jfont.lua:945-957, ltj-pretreat.lua:171-175).
//
// Returns the Unicode "presentation form for vertical" codepoint that should
// replace `c` in vertical writing modes, or `c` itself if no mapping exists.
//
// The standard JLReq mapping for the CJK Compatibility Forms block also covers
// the dashes/leaders below, but THIS IMPLEMENTATION DELIBERATELY OMITS them so
// the font's +vrt2 feature can produce continuous-stroke composites instead
// (see the NOTE in getVertPresentationForm):
//   2014 EM DASH                   → FE31 PRES. FORM FOR VERT EM DASH      (omitted)
//   2013 EN DASH                   → FE32 PRES. FORM FOR VERT EN DASH      (omitted)
//   2025 TWO DOT LEADER            → FE30 PRES. FORM FOR VERT TWO DOT LEADER (omitted)
//   2026 HORIZONTAL ELLIPSIS       → FE19 PRES. FORM FOR VERT HORIZ ELLIPSIS (omitted)
// The 23 mappings actually applied are:
//   3001 IDEOGRAPHIC COMMA         → FE11 PRES. FORM FOR VERT IDEOG COMMA
//   3002 IDEOGRAPHIC FULL STOP     → FE12 PRES. FORM FOR VERT IDEOG FULL STOP
//   3008..3011 angle/lenticular brackets → FE3F..FE3C
//   3014/3015 〔〕                  → FE39/FE3A
//   3016/3017 〖〗                  → FE17/FE18
//   300A..300F 《》「」『』           → FE3D..FE44
//   FF08/FF09 fullwidth ( )         → FE35/FE36
//   FF3B/FF3D fullwidth [ ]         → FE47/FE48
//   FF3F fullwidth _                → FE33
//   FF5B/FF5D fullwidth { }         → FE37/FE38
//
// IMPORTANT: caller must apply this substitution to a SEPARATE buffer used
// only for HarfBuzz shaping.  JFM class lookup, line-break logic, and
// highlight-rect computation must still see the ORIGINAL character.  See
// LuaTeX-ja's ltjs.orig_char_table mechanism for the same separation.
//
// Returns the same `c` if `c` is not in the table; callers should pass the
// result to HarfBuzz directly.  Font-availability check (does the font
// actually have a glyph for the FE-form?) is the caller's responsibility,
// since this header has no FT_Face dependency.
lChar32 getVertPresentationForm(lChar32 c);

// JFM inter-class glue in eighths of em.
//
// Implements LuaTeX-ja jfm-ujisv.lua [prev].glue[next] values
// ({base, stretch, shrink}) plus the kanjiskip_stretch /
// kanjiskip_shrink hints used by line adjustment.
//
// Examples (BODY = class [0] CJK, OPEN = [1], CLOSE = [2] (incl. 、，)):
//   getJLReqGlueKernEighths(CLOSE, BODY) → 4 (0.5em after 、 before next char)
//   getJLReqGlueKernEighths(BODY, OPEN)  → 4 (0.5em before 「)
//   getJLReqGlueKernEighths(CLOSE, OPEN) → 4 (between 」 and 「)
//   getJLReqGlueKernEighths(MIDDLE_DOT, MIDDLE_DOT) → 4 (0.5em between ・・)
//   getJLReqGlueKernEighths(PERIOD, MIDDLE_DOT)   → 6 (0.75em between 。and ・)
//
// Apply during layout as: `m_advance[i] += em_px * getJLReqGlueKernEighths(c_prev_class, c_class) / 8`
struct JLReqVertGlueSpec {
    lInt8  base_eighths;
    lInt8  stretch_eighths;
    lInt8  shrink_eighths;
    lUInt8 stretch_priority;
    lUInt8 shrink_priority;
    bool   kanjiskip_stretch;
    bool   kanjiskip_shrink;
    bool   is_kern;

    JLReqVertGlueSpec()
        : base_eighths(0), stretch_eighths(0), shrink_eighths(0),
          stretch_priority(0), shrink_priority(0),
          kanjiskip_stretch(false), kanjiskip_shrink(false),
          is_kern(false) {}
    JLReqVertGlueSpec(lInt8 base, lInt8 stretch, lInt8 shrink,
                      lUInt8 stretch_prio, lUInt8 shrink_prio,
                      bool k_stretch=false, bool k_shrink=false, bool kern=false)
        : base_eighths(base), stretch_eighths(stretch), shrink_eighths(shrink),
          stretch_priority(stretch_prio), shrink_priority(shrink_prio),
          kanjiskip_stretch(k_stretch), kanjiskip_shrink(k_shrink),
          is_kern(kern) {}
};

JLReqVertGlueSpec getJLReqVertGlueSpec(JLReqVertClass prev_class, JLReqVertClass next_class);
int getJLReqGlueKernEighths(JLReqVertClass prev_class, JLReqVertClass next_class);

// JFM inter-class spacing for CJK ↔ non-CJK boundary, eighths of em.
//
// LuaTeX-ja models non-CJK chars in CJK context as a virtual class [100]
// (nox_alchar).  The class [100] propagation logic (ltj-jfont.lua:294-302)
// copies the explicit [curr].glue[0] entry to [curr].glue[100] for each
// class except [6] (exclamation/question, explicitly excluded).  When no
// entry exists (= the default), the **xkanjiskip = 0.25em** fallback
// applies (jfm-ujisv.lua line 14: xkanjiskip = { 0.25, 0.25, .125 }).
//
// We model this with two direction-specific helpers:
//
//   getJLReqCjkToNonCjkEighths(curr_cls)  for CJK followed by non-CJK
//                                           (e.g., 「あ A」)
//   getJLReqNonCjkToCjkEighths(next_cls)  for non-CJK followed by CJK
//                                           (e.g., 「A あ」)
//
// Both return the base value in eighths of em.  Caller multiplies by
// em / 8 to get pixel spacing.
int getJLReqCjkToNonCjkEighths(JLReqVertClass curr_cls);
int getJLReqNonCjkToCjkEighths(JLReqVertClass next_cls);

// Per-face TTB glyph-metrics cache.  Lazily populated via
// FT_LOAD_VERTICAL_LAYOUT on first lookup of each glyph.  Held as a
// member of LVFreeTypeFace so lifetime tracks the face.
//
// Memory: the underlying hash table is allocated lazily on first
// successful lookup, so fonts without vhea (DroidSansMono, Latin-only
// fonts) and faces never used in vertical mode pay no allocation cost.
class LVFontVertGlyphMetricsCache {
public:
    LVFontVertGlyphMetricsCache() : _cache(NULL), _has_vert(false), _checked(false) {}
    ~LVFontVertGlyphMetricsCache() { delete _cache; }

    // Returns true and fills `out` if `face` has vhea/vmtx and the
    // glyph's vertical metrics are available; false otherwise.
    // Caller falls back to horizontal-metric placement when this
    // returns false.
    bool get(FT_Face face, lUInt32 glyph_index, VertGlyphMetrics & out);

    void clear();

private:
    LVHashTable<lUInt32, VertGlyphMetrics> * _cache;  // lazily allocated
    bool _has_vert;   // FT_HAS_VERTICAL(face) result, cached after first check
    bool _checked;
};

class LVFontVertBodyMetricsCache {
public:
    LVFontVertBodyMetricsCache() : _cache(NULL) {}
    ~LVFontVertBodyMetricsCache() { delete _cache; }

    bool get(hb_font_t * font, lUInt32 glyph_index,
             VertBodyGlyphMetrics & out);
    void clear();

private:
    LVHashTable<lUInt32, VertBodyGlyphMetrics> * _cache;
};

#endif  // __LV_FNT_MAN_VERT_H_INCLUDED__
