// =============================================================================
// TTB (top-to-bottom) glyph metrics machinery for vertical-rl/lr rendering.
//
// Fork-only file.  No upstream counterpart.  Implements
// LVFontVertGlyphMetricsCache (declared in lvfntman_vert.h): a per-face
// cache of vertical glyph metrics read via FT_LOAD_VERTICAL_LAYOUT.
//
// Why this exists:
//   The default load flags give horizontal hmtx metrics (bitmap_top =
//   distance from baseline up to bitmap top).  For TTB layout we want
//   vmtx metrics (vertBearingX/Y = offsets from the *vertical* glyph
//   origin to the bitmap top-left).  Fonts designed primarily for TTB
//   (Hiragino, Yu Mincho) have consistent vmtx but per-glyph variable
//   hmtx — so positioning by hmtx in vertical mode produces uneven
//   inter-character spacing.  Querying vmtx fixes that.
//
//   Cache hit ratio is ~100% within a single book session (one entry
//   per glyph index per face), so the FT_Load_Glyph cost is paid once.
// =============================================================================

#include "../include/lvtextfm_vert_diag.h"
#include "../include/lvfntman_vert.h"
#include "../include/lvfntman.h"
#include "../include/lvdrawbuf.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

bool drawVerticalTextDecorations(
        LVDrawBuf * buf, int x, int y, int width, int advance,
        int font_size, int font_height, int thickness,
        int text_decoration_back_gap, int target_h, lUInt32 flags)
{
    bool rotated = flags & LFNT_HINT_RENDER_ROTATE_FOR_VERTICAL;
    bool upright = flags & LFNT_HINT_IS_VERTICAL;
    bool vertical_edge = flags & LFNT_HINT_VERTICAL_DECORATION_EDGE;
    // TCY keeps its glyphs horizontal, but its decoration still belongs to
    // the vertical line. The resolved decoration edge is therefore also an
    // explicit vertical-context hint, independent of glyph orientation.
    if ( !(flags & LFNT_DRAW_DECORATION_MASK)
            || (!rotated && !upright && !vertical_edge) )
        return false;

    int owner_thickness = (flags & LFNT_HINT_VERTICAL_DECORATION_THICKNESS_MASK)
            >> LFNT_HINT_VERTICAL_DECORATION_THICKNESS_SHIFT;
    if ( owner_thickness > 0 )
        thickness = owner_thickness;
    int cross_extent = rotated ? font_height : font_size;
    int extent = width >= 0 ? width : (rotated ? advance : font_size);
    int y0 = y - text_decoration_back_gap;
    int y1 = y + extent;
    lUInt32 color = buf->GetTextColor();
    if ( flags & LFNT_DRAW_UNDERLINE ) {
        int inline_end = (flags & LFNT_HINT_VERTICAL_DECORATION_EDGE)
                ? target_h : x + cross_extent;
        // In vertical-rl, an underline is painted on the right (under) side.
        // The caller resolves this start coordinate from the decoration owner
        // (including overlap with a coincident inline-end border).
        buf->FillRect(inline_end, y0, inline_end + thickness, y1, color);
        if ( !IS_COLOR_FULLY_TRANSPARENT(color) )
            ltext_record_vert_decoration_trace(1, inline_end, y0,
                    inline_end + thickness, y1);
    }
    if ( flags & LFNT_DRAW_OVERLINE ) {
        int inline_start = (flags & LFNT_HINT_VERTICAL_DECORATION_EDGE)
                ? target_h : x;
        // In vertical-rl, an overline is painted on the left (over) side.
        // Resolve it from the decoration owner just like the right-side
        // underline, so nested font-size changes cannot kink the rule.
        buf->FillRect(inline_start, y0, inline_start + thickness, y1, color);
        if ( !IS_COLOR_FULLY_TRANSPARENT(color) )
            ltext_record_vert_decoration_trace(2, inline_start, y0,
                    inline_start + thickness, y1);
    }
    if ( flags & LFNT_DRAW_LINE_THROUGH ) {
        int line_x = x + (cross_extent - thickness) / 2;
        buf->FillRect(line_x, y0, line_x + thickness, y1, color);
    }
    return true;
}

// Match the local macro in lvfntman.cpp (round + truncate from 26.6 fixed).
#define FONT_METRIC_TO_PX(x)    (((x)+32) >> 6)

// Returns true for characters that need explicit 90° CW rotation when
// drawn in vertical-rl mode (CSS text-orientation: mixed).
//
// CJK, Hiragana, Katakana, and Fullwidth/Halfwidth Forms are naturally
// upright (or substituted via +vert) and therefore NOT rotated.
// All other scripts (Latin, Greek, Cyrillic, digits, ASCII punctuation …)
// are "horizontal" scripts and must be laid sideways in vertical text.
bool isUniformVerticalIdeograph(lChar32 c)
{
    // Hiragana
    if (c >= 0x3041 && c <= 0x309F) return true;
    // Katakana, excluding ー (0x30FC) which is handled as a vert mark
    if (c >= 0x30A0 && c <= 0x30FB) return true;
    if (c >= 0x30FD && c <= 0x30FF) return true;
    // Bopomofo
    if (c >= 0x3105 && c <= 0x312F) return true;
    // CJK Unified Ideographs Extension A
    if (c >= 0x3400 && c <= 0x4DBF) return true;
    // CJK Unified Ideographs
    if (c >= 0x4E00 && c <= 0x9FFF) return true;
    // Hangul Syllables
    if (c >= 0xAC00 && c <= 0xD7A3) return true;
    // CJK Compatibility Ideographs
    if (c >= 0xF900 && c <= 0xFAFF) return true;
    // CJK Unified Ideographs Extension B and beyond
    if (c >= 0x20000) return true;
    return false;
}

// LuaTeX-ja JFM vertical character class.
//
// Char lists ported verbatim from LuaTeX-ja src/jfm-ujisv.lua
// (commit verified 2026-05-27).  Any deviation from that file should
// be treated as a bug — keep the two in sync.
//
// Note: JLREQ_VERT_VERT_MARK does NOT correspond to a class in jfm-ujisv.
// It is a fork-only auxiliary signal (see LFNT_HINT_VERTICAL_MARK in
// lvfntman.h) used to compensate for fonts whose +vert glyph variants
// have buggy hmtx/vmtx (e.g. Hiragino's vBX=0 on ー).  Per LuaTeX-ja's
// own taxonomy, ー 〜 ～ are class [0] body CJK; ‥ … are class [5]
// DASH; this function returns those classes per the source.  Callers
// who want the fork compensation must dispatch on
// LFNT_HINT_VERTICAL_MARK *before* consulting JLReqVertClass.
JLReqVertClass getJLReqVertClass(lChar32 c)
{
    // --- Opening brackets (class [1] in jfm-ujisv.lua) ---
    switch (c) {
        case 0x2018: // ‘
        case 0x201C: // “
        case 0x3008: // 〈
        case 0x300A: // 《
        case 0x300C: // 「
        case 0x300E: // 『
        case 0x3010: // 【
        case 0x3014: // 〔
        case 0x3016: // 〖
        case 0x3018: // 〘
        case 0x301D: // 〝
        case 0xFF08: // （
        case 0xFF3B: // ［
        case 0xFF5B: // ｛
        case 0xFF5F: // ｟
            return JLREQ_VERT_OPEN_BRACKET;
        default:
            break;
    }

    // --- Closing brackets + ideographic comma (class [2]) ---
    switch (c) {
        case 0x2019: // ’
        case 0x201D: // ”
        case 0x3009: // 〉
        case 0x300B: // 》
        case 0x300D: // 」
        case 0x300F: // 』
        case 0x3011: // 】
        case 0x3015: // 〕
        case 0x3017: // 〗
        case 0x3019: // 〙
        case 0x301F: // 〟
        case 0xFF09: // ）
        case 0xFF3D: // ］
        case 0xFF5D: // ｝
        case 0xFF60: // ｠
        case 0x3001: // 、
        case 0xFF0C: // ，
            return JLREQ_VERT_CLOSE_BRACKET_COMMA;
        default:
            break;
    }

    // --- Middle dot (class [3]) ---
    switch (c) {
        case 0x30FB: // ・
        case 0xFF1A: // ：
        case 0xFF1B: // ；
        case 0x00B7: // ·
            return JLREQ_VERT_MIDDLE_DOT;
        default:
            break;
    }

    // --- Period / full stop (class [4]) ---
    if (c == 0x3002 /* 。 */ || c == 0xFF0E /* ． */)
        return JLREQ_VERT_PERIOD;

    // --- Inseparable characters (class [5] in jfm-ujisv — em dashes and leaders) ---
    switch (c) {
        case 0x2014: // —
        case 0x2015: // ―
        case 0x2025: // ‥
        case 0x2026: // …
        case 0x3033: // 〳
        case 0x3034: // 〴
        case 0x3035: // 〵
            return JLREQ_VERT_DASH;
        default:
            break;
    }

    // --- Half-width dash (class [105]) ---
    if (c == 0x30A0 /* ゠ */ || c == 0x2013 /* – */)
        return JLREQ_VERT_HALF_DASH;

    // --- Exclamation / question marks (class [6]) ---
    switch (c) {
        case 0xFF01: // ！
        case 0xFF1F: // ？
        case 0x203C: // ‼
        case 0x2047: // ⁇
        case 0x2048: // ⁈
        case 0x2049: // ⁉
            return JLREQ_VERT_EXCLAM_QUEST;
        default:
            break;
    }

    // --- Halfwidth katakana (class [7]) ---
    if (c >= 0xFF61 && c <= 0xFF9F)
        return JLREQ_VERT_HALF_KANA;

    // --- Box drawing characters (class [8]) ---
    if (c >= 0x2500 && c <= 0x257F)
        return JLREQ_VERT_BOX_DRAWING;

    // --- Combining dakuten / handakuten (class [307]) ---
    if (c == 0x3099 || c == 0x309A)
        return JLREQ_VERT_COMBINING_MARK;

    // --- Vertical iteration marks (class [200]) ---
    // jfm-ujisv: t[200] = copy(t[0]) with width = 2.0; chars = {'〱', '〲'}.
    // These vertical kana repeat marks occupy two column slots and have
    // a 2em-tall glyph designed by the font.
    if (c == 0x3031 || c == 0x3032)
        return JLREQ_VERT_KANA_REPEAT;

    // --- Body CJK (class [0], default) ---
    // jfm-ujisv treats ー (U+30FC), 〜 (U+301C), ～ (U+FF5E) as default body
    // CJK — relying on font's vBX/vBY for positioning.  fork callers may
    // override via LFNT_HINT_VERTICAL_MARK if the font's +vert form is
    // mis-positioned (Hiragino-style).
    if (isUniformVerticalIdeograph(c))
        return JLREQ_VERT_CJK_BODY;

    return JLREQ_VERT_OTHER;
}

// Per-class layout — mirrors LuaTeX-ja jfm-ujisv.lua's char_data
// (width / align fields).  See enum JLReqVertClass in lvfntman_vert.h.
JLReqVertLayout getJLReqVertLayout(JLReqVertClass cls)
{
    JLReqVertLayout out;
    out.width_halves = 2;            // full em by default
    out.align        = JLREQ_ALIGN_MIDDLE;
    switch (cls) {
        case JLREQ_VERT_CJK_BODY:
            // full em, centred — natural placement for ideographs/kana
            break;
        case JLREQ_VERT_OPEN_BRACKET:
            // 「『（〈《【〔〘 — half-em slot, glyph at slot bottom
            //   (compaction whitespace BEFORE the bracket, JLReq 3.1.10)
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_RIGHT;
            break;
        case JLREQ_VERT_CLOSE_BRACKET_COMMA:
            // 」』）〉》】〕〙、，— half-em, glyph at slot top
            //   (compaction whitespace AFTER, JLReq 3.1.10)
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_MIDDLE_DOT:
            // ・：； — half-em, centred
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_MIDDLE;
            break;
        case JLREQ_VERT_PERIOD:
            // 。．— half-em, glyph at slot top (JLReq 3.1.5: top-right corner)
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_DASH:
            // — ― ‥ … 〳 〴 〵 — full em, glyph at slot top (= align='left')
            // per jfm-ujisv class [5].
            out.width_halves = 2;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_HALF_DASH:
            // ゠ – — half-em, centred; jfm-ujisv class [105].
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_MIDDLE;
            break;
        case JLREQ_VERT_EXCLAM_QUEST:
            // ？！‼⁇⁈⁉ — full em, glyph at slot top (= align='left')
            // per jfm-ujisv class [6].
            out.width_halves = 2;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_HALF_KANA:
            // halfwidth katakana — half-em slot, glyph at slot top
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_BOX_DRAWING:
            // Box drawing — full em, glyph at slot top; jfm-ujisv class [8].
            out.width_halves = 2;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_COMBINING_MARK:
            // Combining dakuten/handakuten — zero-width, right-aligned.
            out.width_halves = 0;
            out.align        = JLREQ_ALIGN_RIGHT;
            break;
        case JLREQ_VERT_VERT_MARK:
            // ー — ‥ … 〜 ～ — full em, centred (vertical bar/dot pattern)
            break;
        case JLREQ_VERT_KANA_REPEAT:
            // 〱 〲 — two-em wide (= 2-slot-tall in column), centred.
            // jfm-ujisv class [200] inherits class [0] except width=2.0.
            out.width_halves = 4;            // 2em (= 4 half-ems)
            out.align        = JLREQ_ALIGN_MIDDLE;
            break;
        case JLREQ_VERT_OTHER:
            // Latin/numerals routed elsewhere; treat as full-em fallback
            break;
    }
    return out;
}

int getJLReqVertSlotWidth(lChar32 c, int em_px, int natural_advance_px)
{
    JLReqVertClass cls = getJLReqVertClass(c);
    if (cls == JLREQ_VERT_OTHER)
        return natural_advance_px;        // not in jfm-ujisv → font's natural
    // Multi-em composite glyph: when +vrt2 substitutes a char with a
    // composite (e.g. Hiragino's gid8857 nibu-dashi / double-em dash for U+2014, with
    // vertAdvance == 2em), the font's natural_advance is what positions
    // the next char so consecutive composites chain end-to-end.  Force-
    // overriding to JFM-class em width would mangle this: subsequent
    // chars would overlap the composite.  Threshold of 1.5 × em safely
    // catches 2em / 3em composites while leaving normal-advance glyphs
    // unaffected.
    if (natural_advance_px > (em_px * 3) / 2)
        return natural_advance_px;
    JLReqVertLayout layout = getJLReqVertLayout(cls);
    return (em_px * layout.width_halves) / 2;
}

int getJLReqVertCwa(lChar32 c, int em_px, int vadv_px)
{
    JLReqVertLayout layout = getJLReqVertLayout(getJLReqVertClass(c));
    // cwa = align_num * (fwidth - vadv) per ltj-setwidth.lua:269.
    // align_num: 0 (left), 0.5 (middle), 1 (right) — our enum maps to
    // 0, 1, 2; divide by 2 to recover the fractional value.
    // fwidth is the JFM slot width, taken from getJLReqVertSlotWidth so the
    // multi-em composite preservation (2em kana-repeat / dash composites) is
    // honoured; vadv_px is the font's natural vertical advance for this glyph.
    // For a font-supplied 2em glyph fwidth == vadv_px → cwa = 0 (the glyph
    // already fills its slot), avoiding the spurious half-em overshoot that a
    // hardcoded vadv = em produced.
    int fwidth = getJLReqVertSlotWidth(c, em_px, vadv_px);
    return (((int)layout.align) * (fwidth - vadv_px)) / 2;
}

lChar32 getVertPresentationForm(lChar32 c)
{
    // LuaTeX-ja vert_form_table port (ltj-jfont.lua:948-957).
    // 23 codepoint mappings to U+FE10-FE48 (CJK Compatibility Forms block).
    //
    // NOTE: dashes / leaders (U+2014, U+2013, U+2025, U+2026) are DELIBERATELY
    // omitted here, even though LuaTeX-ja's vert_form_table lists them.
    // LuaTeX-ja nullifies vform entries that the font's `vert`/`vrt2` feature
    // already handles (ltj-jfont.lua:1011-1014 `if w==i then vform[j]=k` and
    // `vform[j]=nil` paths), so for fonts whose `vrt2` substitutes — / ‥ / …
    // with multi-em composite glyphs (Hiragino's gid8857 nibu-dashi / double-em dash, etc.),
    // LuaTeX-ja does NOT substitute via vform first.  We don't replicate the
    // dynamic per-font nullification logic; instead we simply skip these chars
    // unconditionally so the font's `vrt2` feature can produce composites.
    // For fonts without composite vrt2 mappings, +vert + the
    // needsVerticalRotation90CW fallback still gives correct vertical glyphs.
    switch (c) {
        // Punctuation
        case 0x3001: return 0xFE11; // 、 → ︑
        case 0x3002: return 0xFE12; // 。 → ︒
        // Underscore
        case 0xFF3F: return 0xFE33; // ＿ → ︳
        // Angle brackets
        case 0x3008: return 0xFE3F; // 〈 → ︿
        case 0x3009: return 0xFE40; // 〉 → ﹀
        case 0x300A: return 0xFE3D; // 《 → ︽
        case 0x300B: return 0xFE3E; // 》 → ︾
        // Corner brackets
        case 0x300C: return 0xFE41; // 「 → ﹁
        case 0x300D: return 0xFE42; // 」 → ﹂
        case 0x300E: return 0xFE43; // 『 → ﹃
        case 0x300F: return 0xFE44; // 』 → ﹄
        // Lenticular / hollow brackets
        case 0x3010: return 0xFE3B; // 【 → ︻
        case 0x3011: return 0xFE3C; // 】 → ︼
        case 0x3014: return 0xFE39; // 〔 → ︹
        case 0x3015: return 0xFE3A; // 〕 → ︺
        case 0x3016: return 0xFE17; // 〖 → ︗
        case 0x3017: return 0xFE18; // 〗 → ︘
        // Fullwidth ASCII brackets
        case 0xFF08: return 0xFE35; // （ → ︵
        case 0xFF09: return 0xFE36; // ） → ︶
        case 0xFF3B: return 0xFE47; // ［ → ﹇
        case 0xFF3D: return 0xFE48; // ］ → ﹈
        case 0xFF5B: return 0xFE37; // ｛ → ︷
        case 0xFF5D: return 0xFE38; // ｝ → ︸
        default:     return c;
    }
}

static inline JLReqVertGlueSpec jfmGlue(int base, int stretch, int shrink,
                                        int priority=0, bool kanjiskip_stretch=false,
                                        bool kanjiskip_shrink=false, bool kern=false)
{
    return JLReqVertGlueSpec((lInt8)base, (lInt8)stretch, (lInt8)shrink,
                             (lUInt8)priority, (lUInt8)priority,
                             kanjiskip_stretch, kanjiskip_shrink, kern);
}

static inline JLReqVertGlueSpec jfmKern(int base)
{
    return jfmGlue(base, 0, 0, 0, false, false, true);
}

static inline JLReqVertGlueSpec jfmNone()
{
    return JLReqVertGlueSpec();
}

JLReqVertGlueSpec getJLReqVertGlueSpec(JLReqVertClass prev_class, JLReqVertClass next_class)
{
    // Encodes jfm-ujisv.lua [N].glue[M] in eighths of em.
    // Switch on prev (rows of the matrix), then next (columns).
    // Pairs not listed default to 0.
    switch (prev_class) {
        case JLREQ_VERT_KANA_REPEAT:  // jfm-ujisv t[200] = copy(t[0]) — same as BODY row
        case JLREQ_VERT_CJK_BODY:
            switch (next_class) {
                case JLREQ_VERT_OPEN_BRACKET:        return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_CLOSE_BRACKET_COMMA: return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(2, 0, 2, 1);
                case JLREQ_VERT_PERIOD:              return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_EXCLAM_QUEST:        return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_HALF_KANA:           return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_BOX_DRAWING:         return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_COMBINING_MARK:      return jfmKern(0);
                default: return jfmNone();
            }
        case JLREQ_VERT_OPEN_BRACKET:
            switch (next_class) {
                case JLREQ_VERT_CJK_BODY:            return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_OPEN_BRACKET:        return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_CLOSE_BRACKET_COMMA: return jfmGlue(0, 0, 0, 0, true, true);
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(2, 0, 2, 1);
                case JLREQ_VERT_PERIOD:              return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_DASH:
                case JLREQ_VERT_HALF_DASH:
                case JLREQ_VERT_EXCLAM_QUEST:
                case JLREQ_VERT_HALF_KANA:
                case JLREQ_VERT_BOX_DRAWING:         return jfmGlue(0, 0, 0, 0, false, true);
                default: return jfmNone();
            }
        case JLREQ_VERT_CLOSE_BRACKET_COMMA:
            switch (next_class) {
                case JLREQ_VERT_CJK_BODY:            return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_OPEN_BRACKET:        return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_CLOSE_BRACKET_COMMA: return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(2, 0, 2, 1);
                case JLREQ_VERT_PERIOD:              return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_DASH:
                case JLREQ_VERT_HALF_DASH:
                case JLREQ_VERT_EXCLAM_QUEST:
                case JLREQ_VERT_HALF_KANA:
                case JLREQ_VERT_BOX_DRAWING:         return jfmGlue(4, 0, 4, 0, true, false);
                default: return jfmNone();
            }
        case JLREQ_VERT_MIDDLE_DOT:
            switch (next_class) {
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(4, 0, 2, 1);
                // Every other class gets 0.25em around middle dot.
                default:                             return jfmGlue(2, 0, 2, 1);
            }
        case JLREQ_VERT_PERIOD:
            switch (next_class) {
                case JLREQ_VERT_CJK_BODY:            return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_OPEN_BRACKET:        return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(6, 0, 2, 1, true, false);
                case JLREQ_VERT_DASH:
                case JLREQ_VERT_HALF_DASH:
                case JLREQ_VERT_EXCLAM_QUEST:
                case JLREQ_VERT_HALF_KANA:
                case JLREQ_VERT_BOX_DRAWING:         return jfmGlue(4, 0, 4, 0, true, false);
                default: return jfmNone();
            }
        case JLREQ_VERT_DASH:
        case JLREQ_VERT_HALF_DASH:
            switch (next_class) {
                case JLREQ_VERT_OPEN_BRACKET:        return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_CLOSE_BRACKET_COMMA: return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(2, 0, 2, 1);
                case JLREQ_VERT_PERIOD:              return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_EXCLAM_QUEST:        return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_DASH:
                case JLREQ_VERT_HALF_DASH:           return jfmKern(0);
                default: return jfmNone();
            }
        case JLREQ_VERT_EXCLAM_QUEST:
            switch (next_class) {
                case JLREQ_VERT_CJK_BODY:            return jfmGlue(8, 0, 4, 0, true, false);
                case JLREQ_VERT_OPEN_BRACKET:        return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_CLOSE_BRACKET_COMMA: return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(6, 0, 2, 1);
                case JLREQ_VERT_PERIOD:              return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_DASH:
                case JLREQ_VERT_HALF_DASH:           return jfmKern(0);
                case JLREQ_VERT_EXCLAM_QUEST:        return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_HALF_KANA:           return jfmGlue(8, 0, 4, 0, true, false);
                case JLREQ_VERT_BOX_DRAWING:         return jfmGlue(0, 0, 0, 0, false, true);
                default: return jfmNone();
            }
        case JLREQ_VERT_HALF_KANA:
        case JLREQ_VERT_COMBINING_MARK:
        case JLREQ_VERT_BOX_DRAWING:
            switch (next_class) {
                case JLREQ_VERT_OPEN_BRACKET:        return jfmGlue(4, 0, 4, 0, true, false);
                case JLREQ_VERT_CLOSE_BRACKET_COMMA: return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_MIDDLE_DOT:          return jfmGlue(2, 0, 2, 1);
                case JLREQ_VERT_PERIOD:              return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_EXCLAM_QUEST:        return jfmGlue(0, 0, 0, 0, false, true);
                case JLREQ_VERT_HALF_KANA:
                case JLREQ_VERT_BOX_DRAWING:         return jfmGlue(0, 0, 0, 0, false, true);
                default: return jfmNone();
            }
        case JLREQ_VERT_VERT_MARK:    // not in jfm-ujisv; treat as default body
        case JLREQ_VERT_OTHER:
            return jfmNone();
    }
    return jfmNone();
}

int getJLReqGlueKernEighths(JLReqVertClass prev_class, JLReqVertClass next_class)
{
    return getJLReqVertGlueSpec(prev_class, next_class).base_eighths;
}

int getJLReqCjkToNonCjkEighths(JLReqVertClass curr_cls)
{
    // CJK_curr.glue[100] = CJK_curr.glue[0] (per LuaTeX-ja ltj-jfont.lua:294-302),
    // EXCEPT class [6] (EXCLAM_QUEST) which is explicitly excluded from this
    // propagation.  Where no entry exists, xkanjiskip = 0.25em (= 2 eighths) applies.
    switch (curr_cls) {
        case JLREQ_VERT_CJK_BODY:            return 2; // [0]: no glue[0]   → xkanjiskip
        case JLREQ_VERT_OPEN_BRACKET:        return 0; // [1].glue[0]=0     (explicit)
        case JLREQ_VERT_CLOSE_BRACKET_COMMA: return 4; // [2].glue[0]=0.5em
        case JLREQ_VERT_MIDDLE_DOT:          return 2; // [3].glue[0]=0.25em
        case JLREQ_VERT_PERIOD:              return 4; // [4].glue[0]=0.5em
        case JLREQ_VERT_DASH:                return 2; // [5]: no glue[0]   → xkanjiskip
        case JLREQ_VERT_HALF_DASH:           return 2; // [105]: no glue[0] → xkanjiskip
        case JLREQ_VERT_EXCLAM_QUEST:        return 2; // [6] EXCLUDED from prop. → xkanjiskip
        case JLREQ_VERT_HALF_KANA:           return 2; // [7]: no glue[0]   → xkanjiskip
        case JLREQ_VERT_BOX_DRAWING:         return 2; // [8]: no glue[0]   → xkanjiskip
        case JLREQ_VERT_COMBINING_MARK:      return 0; // [307]: zero-width combining mark
        case JLREQ_VERT_VERT_MARK:           return 2; // fork-only, treat as body
        case JLREQ_VERT_KANA_REPEAT:         return 2; // [200] inherits [0]
        case JLREQ_VERT_OTHER:               return 0; // shouldn't happen (both non-CJK)
    }
    return 2;
}

int getJLReqNonCjkToCjkEighths(JLReqVertClass next_cls)
{
    // [100].glue[next] = [0].glue[next] (full body CJK propagation).
    // Entries from jfm-ujisv [0].glue (line 18-28).  No entry → xkanjiskip.
    switch (next_cls) {
        case JLREQ_VERT_CJK_BODY:            return 2; // [0]→[0] no entry  → xkanjiskip
        case JLREQ_VERT_OPEN_BRACKET:        return 4; // [0]→[1] = 0.5em
        case JLREQ_VERT_CLOSE_BRACKET_COMMA: return 0; // [0]→[2] = 0       (explicit)
        case JLREQ_VERT_MIDDLE_DOT:          return 2; // [0]→[3] = 0.25em
        case JLREQ_VERT_PERIOD:              return 0; // [0]→[4] = 0       (explicit)
        case JLREQ_VERT_DASH:                return 2; // [0]: no [5] entry → xkanjiskip
        case JLREQ_VERT_HALF_DASH:           return 2; // [0]: no [105] entry → xkanjiskip
        case JLREQ_VERT_EXCLAM_QUEST:        return 0; // [0]→[6] = 0       (explicit)
        case JLREQ_VERT_HALF_KANA:           return 0; // [0]→[7] = 0       (explicit)
        case JLREQ_VERT_BOX_DRAWING:         return 0; // [0]→[8] = 0       (explicit)
        case JLREQ_VERT_COMBINING_MARK:      return 0; // [0]→[307] kern 0
        case JLREQ_VERT_VERT_MARK:           return 2; // fork-only, treat as body
        case JLREQ_VERT_KANA_REPEAT:         return 2; // [200]: no [0]→[200] → xkanjiskip
        case JLREQ_VERT_OTHER:               return 0; // shouldn't happen
    }
    return 2;
}

bool needsVerticalRotation90CW(lChar32 c)
{
    // --- Horizontal-script characters: ROTATE ---
    if (c >= 0x0021 && c <= 0x007E) return true; // ASCII printable
    if (c >= 0x00A1 && c <= 0x024F) return true; // Latin-1 Supp + Latin Extended-A/B
    if (c >= 0x0250 && c <= 0x036F) return true; // IPA + Spacing Modifier + Diacritics
    if (c >= 0x0370 && c <= 0x03FF) return true; // Greek and Coptic
    if (c >= 0x0400 && c <= 0x04FF) return true; // Cyrillic

    // Special Japanese horizontal marks:
    switch (c) {
        case 0x30FC: // ー KATAKANA-HIRAGANA PROLONGED SOUND MARK
        case 0x301C: // 〜 WAVE DASH
        case 0xFF5E: // ～ FULLWIDTH TILDE
        case 0x2014: // — EM DASH
        case 0x2015: // ― HORIZONTAL BAR
        case 0xFF0D: // － FULLWIDTH HYPHEN-MINUS
        case 0x2025: // ‥ TWO DOT LEADER
        case 0x2026: // … HORIZONTAL ELLIPSIS
            return true;
        default:
            break;
    }

    // --- CJK / East Asian scripts: do NOT rotate ---
    if (c >= 0x2E80 && c <= 0x9FFF) return false; // CJK radicals … CJK Unified
    if (c >= 0xAC00 && c <= 0xD7A3) return false; // Hangul syllables
    if (c >= 0xF900 && c <= 0xFAFF) return false; // CJK Compat Ideographs
    if (c >= 0xFF00 && c <= 0xFFEF) return false; // Halfwidth / Fullwidth Forms
    if (c >= 0x20000)               return false; // CJK Extension B–F, etc.

    return false;
}

// Draw a glyph rotated 90° clockwise into buf.
// Used as a fallback when the font lacks a +vert OpenType substitution for
// characters that need vertical orientation (e.g. ー drawn as a horizontal
// dash must become a vertical bar).
// Only works for 8-bit grayscale glyphs (bmp_pixelformat != 4).
// The visual centre of the glyph is preserved at the original (glyph_x, glyph_y)
// position, keeping it centred in its em-square column.
void drawGlyphItemRotated90CW(LVDrawBuf * buf, int glyph_x, int glyph_y,
        LVFontGlyphCacheItem * item, const lUInt32 * palette)
{
    if (item->bmp_pixelformat == 4)
        return; // colour glyphs cannot be rotated; caller should guard against this
    int orig_w = item->bmp_width;
    int orig_h = item->bmp_height;
    if (orig_w <= 0 || orig_h <= 0)
        return;
    // After 90° CW rotation the dimensions are swapped.
    int rot_w = orig_h;
    int rot_h = orig_w;
    // Use a stack buffer for glyphs up to 64×64 px; heap otherwise.
    lUInt8 stack_buf[64 * 64];
    lUInt8 * rot = (rot_w * rot_h <= (int)sizeof(stack_buf))
                 ? stack_buf : new lUInt8[rot_w * rot_h];
    // 90° CW: dst[ny][nx] = src[orig_h - 1 - nx][ny]
    // Use bmp_pitch (row stride) for source indexing; it may exceed bmp_width.
    int src_pitch = item->bmp_pitch > 0 ? item->bmp_pitch : orig_w;
    const lUInt8 * src = item->bmp;
    for (int ny = 0; ny < rot_h; ny++) {
        for (int nx = 0; nx < rot_w; nx++) {
            rot[ny * rot_w + nx] = src[(orig_h - 1 - nx) * src_pitch + ny];
        }
    }
    // Keep the visual centre of the bitmap at the same screen position.
    int adj_x = glyph_x + (orig_w - rot_w) / 2;
    int adj_y = glyph_y + (orig_h - rot_h) / 2;
    buf->Draw(adj_x, adj_y, rot, rot_w, rot_h, palette);
    if (rot != stack_buf)
        delete[] rot;
}

bool LVFontVertGlyphMetricsCache::get(FT_Face face, lUInt32 glyph_index,
                                      VertGlyphMetrics & out)
{
    if (!face)
        return false;

    // Cache the FT_HAS_VERTICAL probe once per face.  Fonts without
    // a vhea table (DroidSansMono, Latin-only fonts, etc.) skip TTB
    // metrics entirely; the caller falls back to horizontal-metric
    // placement.  No hash table is allocated for such faces.
    if (!_checked) {
        _has_vert = FT_HAS_VERTICAL(face) != 0;
        _checked = true;
    }
    if (!_has_vert)
        return false;

    // Lazy allocation: defer the hash table until the first vertical
    // lookup actually arrives, so faces never used in vertical mode
    // (most fonts in horizontal-only documents) cost nothing.
    if (!_cache)
        _cache = new LVHashTable<lUInt32, VertGlyphMetrics>(256);

    if (_cache->get(glyph_index, out))
        return true;

    // FT_LOAD_NO_BITMAP: we only want metrics — the bitmap was already
    // rendered and cached via the standard horizontal load path.
    int err = FT_Load_Glyph(face, glyph_index,
                            FT_LOAD_DEFAULT | FT_LOAD_VERTICAL_LAYOUT |
                            FT_LOAD_NO_BITMAP);
    if (err)
        return false;

    out.origin_x = (lInt16) FONT_METRIC_TO_PX(face->glyph->metrics.vertBearingX);
    out.origin_y = (lInt16) FONT_METRIC_TO_PX(face->glyph->metrics.vertBearingY);
    out.advance  = (lUInt16) FONT_METRIC_TO_PX(face->glyph->metrics.vertAdvance);
    _cache->set(glyph_index, out);
    return true;
}

void LVFontVertGlyphMetricsCache::clear()
{
    if (_cache)
        _cache->clear();
    _checked = false;
    _has_vert = false;
}

bool LVFontVertBodyMetricsCache::get(hb_font_t * font, lUInt32 glyph_index,
                                     VertBodyGlyphMetrics & out)
{
    if (!font)
        return false;
    if (!_cache)
        _cache = new LVHashTable<lUInt32, VertBodyGlyphMetrics>(256);
    if (_cache->get(glyph_index, out))
        return true;

    hb_glyph_extents_t glyph_extents;
    hb_font_extents_t font_extents;
    if (!hb_font_get_glyph_extents(font, glyph_index, &glyph_extents)
            || !hb_font_get_h_extents(font, &font_extents))
        return false;

    out.x_bearing = glyph_extents.x_bearing;
    out.y_bearing = glyph_extents.y_bearing;
    out.h_advance = hb_font_get_glyph_h_advance(font, glyph_index);
    out.ascender = font_extents.ascender;
    _cache->set(glyph_index, out);
    return true;
}

void LVFontVertBodyMetricsCache::clear()
{
    if (_cache)
        _cache->clear();
}
