// =============================================================================
// Fork-only declarations used by the split formatter files.
//
// lvtextfm.cpp and lvtextfm_vert.cpp form one translation unit
// (the latter is #include'd at the bottom of lvtextfm.cpp).  This
// header concentrates fork-only forward declarations and small
// ruby-detection helpers in one place so lvtextfm.cpp stays closer
// to upstream.  It must be included after lvtinydom.h / fb2def.h /
// cssdef.h are visible (i.e. include it from lvtextfm.cpp only).
//
// Created during Phase C Step 1 (soft-fork hygiene) — see CLAUDE.md.
// =============================================================================

#ifndef LVTEXTFM_FORK_H_INCLUDED
#define LVTEXTFM_FORK_H_INCLUDED

class LVFormatter;

// Fork-only vertical debug selectors (defined in lvtextfm_vert.cpp).
bool verticalTextDebugEnabled();
bool isVerticalTextLineDebugClass(const lString32 &cls);
bool isVerticalInlineBorderDebugClass(const lString32 &cls);

// Horizontal layout free functions (defined in lvtextfm.cpp,
// called from LVFormatter::splitParagraphs).
void processParagraphHorizontal( LVFormatter* fmt, int start, int end, bool isLastPara );
void processEmbeddedBlockHorizontal( LVFormatter* fmt, int idx );

// Vertical layout free functions (defined in lvtextfm_vert.cpp).
void processParagraphVertical( LVFormatter* fmt, int start, int end, bool isLastPara );
void processEmbeddedBlockVertical( LVFormatter* fmt, int idx );

// Vertical-mode Draw() entry setup (defined in lvtextfm_vert.cpp).  Handles
// the Y=X coordinate swap (DrawDocument passes (actual_Y, actual_X) as
// (x, y) due to the pipeline's coordinate mapping), increments fork
// diagnostic counters, and resolves line_x using the stored column anchor
// (which may differ from clip.right when content_overflow_clip is widened
// for ruby annotation overhang).  Modifies x and y in place.  Returns the
// resolved line_x.
int initVerticalDrawSetup(
    formatted_text_fragment_t * pbuffer, LVDrawBuf * buf,
    int & x_inout, int & y_inout, const lvRect & clip);

// State maintained across the per-word loop in LFormattedText::Draw when
// rendering in vertical-rl / vertical-lr mode.  Bundled into a struct so
// it can be passed by reference to fork-only positioning helpers in
// lvtextfm_vert.cpp.  Reset at the start of each frmline (column).
struct VerticalDrawState {
    int vert_min_next_x;             // minimum next-allowed word->x position
    int vert_prev_plain_y0;          // y0 of last drawn plain/CJK char (-1 = none yet)
    int vert_prev_effective_width;   // effective_width of that char (= slot height)
    bool vert_prev_was_non_cjk_word; // for CJK↔non-CJK xkanjiskip
    int vert_prev_cjk_class;         // JLReqVertClass of prev CJK word (-1 = none)

    VerticalDrawState()
        : vert_min_next_x(0), vert_prev_plain_y0(-1),
          vert_prev_effective_width(0), vert_prev_was_non_cjk_word(false),
          vert_prev_cjk_class(-1) {}
    void resetForNewFrmline() {
        vert_min_next_x = 0;
        vert_prev_plain_y0 = -1;
        vert_prev_effective_width = 0;
        vert_prev_was_non_cjk_word = false;
        vert_prev_cjk_class = -1;
    }
};

// Per-ruby-inline-box metrics computed up-front in measureText() (defined
// in lvtextfm_vert.cpp).  Used to size the inner ruby table (render_w),
// override its TTB advance to match the actual visual depth, and resolve
// Latin-base ruby letter_spacing.
struct VertRubyInlineBoxMetrics {
    int render_w;                 // chosen max(base_depth, annot_depth)
    int annot_depth;              // annot_char_count × annot_font_size
    int base_horiz_advance_pre;   // Σ getCharWidth(c) over base chars (for Latin)
    int base_char_count_pre;
    int annot_char_count_pre;
    int annot_font_size_pre;
    int advance_per_char_pre;
    bool is_ruby_inline_pre;      // true ⇔ vert_inline_box && isRubyInlineBox(node)
    bool vert_inline_box;
};
VertRubyInlineBoxMetrics computeVertRubyInlineBoxMetrics(
    formatted_text_fragment_t * pbuffer, ldomNode * node, LVFont * lastFont);

// Vertical-only image and line-alignment adaptations.  Keeping these out of
// LVFormatter's upstream-shaped methods limits future rebase conflicts to the
// small call sites in lvtextfm.cpp.
int getVerticalImageInlineExtent(
    LVFormatter * fmt, ldomNode * node, int fallback);
bool prepareVerticalRubyLineAlignment(
    LVFormatter * fmt, formatted_line_t * frmline, int & width_inout);
void applyVerticalRubyCenterAlignment(
    LVFormatter * fmt, formatted_line_t * frmline,
    int extra_width);
void prepareVerticalSingleImageLineAlignment(
    LVFormatter * fmt, formatted_line_t * frmline);

// Vertical word-formation hooks used by addLineHorizontal().
void applyVerticalTcyWord(
    LVFormatter * fmt, src_text_fragment_t * srcline,
    formatted_word_t * word, LVFont * font);
void trimVerticalLineEndSpaces(
    src_text_fragment_t * srcline, formatted_word_t * word);

// Adjust frmline->width / height for vertical-mode lines (defined in
// lvtextfm_vert.cpp).  In vertical-rl/lr the column WIDTH on screen lives
// in both fields (the formatter computes them as if horizontal).  Single-
// image lines use the image physical width.
void applyVerticalFrmlineDimensions(LVFormatter * fmt, formatted_line_t * frmline);

// N:N (mono-ruby) and N:M (jukugo-ruby) annotation centering
// for vertical ruby inner cells (defined in lvtextfm_vert.cpp).  Per-char
// distribution applies when the source fragment lives inside <rt>/<rtc>/<rp>
// (verified via DOM ancestor walk).  N == M produces JLReq mono-ruby
// alignment (each annot[i] over base[i]); N != M distributes annot chars
// evenly across the base width with overflow allowed (jukugo-style overhang).
// Returns true when distribution was applied (caller must skip default
// block centering).
bool applyVerticalNNRubyCenterDistribution(
    formatted_line_t * frmline, int slot_width, int extra_width,
    LVFormatter * fmt);
bool isVerticalRubyInnerLine(LVFormatter * fmt, formatted_line_t * frmline);
bool isVerticalRubyAnnotationLine(LVFormatter * fmt, formatted_line_t * frmline);

// Geometry and identity inherited from the inline box that establishes a
// vertical text decoration. Descendant font-size changes must not alter them.
struct VerticalDecorationMetrics {
    int inline_start;
    int inline_end;
    int thickness;
    int inline_end_border_width;
    ldomNode * owner;
};
bool getVerticalDecorationMetrics(
    formatted_text_fragment_t * pbuffer, formatted_line_t * frmline,
    ldomNode * node, lUInt32 decoration_flags, int line_x,
    VerticalDecorationMetrics & metrics);

// Vertical-mode PAD border drawing (defined in lvtextfm_vert.cpp).  Handles
// both right-pad and left-pad cases.  Scans frmline->words[] for the paired
// PAD to determine the border's inline-direction extent.
void drawVerticalPadBorders(
    LVDrawBuf * buf, formatted_text_fragment_t * pbuffer,
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, int j,
    int y, int line_x, ldomNode * node,
    bool is_right_pad, bool is_mirrored);

// Update vert state after a Latin-in-vertical word's DrawTextString call.
// Advances vert_min_next_x by word->width (NOT _adv — see comment in
// lvtextfm_vert.cpp for the LTR-shape vs TTB-shape h_advance discrepancy).
// Uses _adv as fallback only when word->width is 0.
void applyVerticalLatinPostDraw(
    int _adv, int word_width, VerticalDrawState & state,
    const lvRect & clip, int y);

// Draw vertical-mode emphasis marks (kenten / bouten — JLReq §3.3.10).  In
// vertical-rl the marks sit in the inter-column space to the right of each
// char, one per character in the word.
void drawVerticalEmphasisMarks(
    LVDrawBuf * buf, formatted_line_t * frmline,
    src_text_fragment_t * srcline, formatted_word_t * word,
    LVFont * font, int y, int line_x, const lvRect & clip,
    const VerticalDrawState & state);

// Vertical-mode image draw positioning (defined in lvtextfm_vert.cpp).
// Computes (x0, y0) for an image word in vertical mode and advances the
// per-column vert_min_next_x tracker past the image so the following word
// is placed after it (otherwise the next CJK char clamps back to the
// pre-image tracker value and overlaps the image).  Image is centred on the
// column axis with the left edge clamped to >= 0.
void applyVerticalImageDraw(
    formatted_line_t * frmline, formatted_word_t * word,
    int y, int line_x, int column_clip_right, const lvRect & clip,
    VerticalDrawState & state,
    int & x0_out, int & y0_out);
// Center image-only vertical EPUB fragments across their physical page.
bool centerVerticalImageOnlyFragment(
    ldomNode * node, formatted_line_t * frmline,
    draw_extra_info_t * draw_extra_info, int image_width, int & x_inout);

// Vertical-mode inline-box (ruby) draw positioning (defined in lvtextfm_vert.cpp).
// Sets x0, y0, doc_x_ib, doc_y_ib for the inner DrawDocument call, applies
// vert_min_next_x clamping, updates state, and increments fork diagnostic
// counters for layout-vs-draw gap and ruby-bleed overlap.
void applyVerticalInlineBoxDraw(
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, int node_x, int node_y,
    int x, int y,
    VerticalDrawState & state,
    int & x0_out, int & y0_out, int & doc_x_ib_out, int & doc_y_ib_out);

// Vertical-mode word draw positioning (defined in lvtextfm_vert.cpp).
// Computes (x0,y0) for a word in vertical mode and updates drawFlags +
// state.  Handles three sub-cases internally: TCY (tate-chu-yoko),
// Latin-rotated-as-block, and plain CJK.  Caller must have checked
// is_vertical && !(srcline->flags & LTEXT_MATH_TRANSFORM).
//
// Out:
//   x0_out, y0_out      — final pen position for DrawTextString
//   vert_skip_draw_out  — true if the word starts past clip.bottom
//   word_is_latin_in_vertical_out, word_is_vert_mark_out — needed by caller
//     to drive post-DrawTextString state updates and ruby mark drawing.
//   word_is_exact_hanging_out — true only for supported hanging punctuation
//     whose draw origin is exactly clip.bottom.
void applyVerticalWordDraw(
    formatted_text_fragment_t * pbuffer,
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, LVFont * font,
    int y, int line_x, const lvRect & clip, bool line_has_image,
    lUInt32 & drawFlags,
    VerticalDrawState & state,
    int & x0_out, int & y0_out, bool & vert_skip_draw_out,
    bool & word_is_latin_in_vertical_out, bool & word_is_vert_mark_out,
    bool & word_is_exact_hanging_out);

// Vertical + inline-box post-pass (defined in lvtextfm_vert.cpp).
// Handles inline-box absolute positioning, vertical-mode column clamping,
// and Phase 5 (LuaTeX-ja JFM) inter-class glue + xkanjiskip mirrors to
// plain words' word->x so getRect() matches Draw()'s vert_min_next_x.
// Called from alignLineHorizontal() in lvtextfm.cpp for any line that
// either has inline boxes OR is in vertical writing mode (the Phase 5
// mirror must run on every vertical line, not just those with ruby).
// formatted_line_t is declared in lvtextfm.h; forward via the typedef name.
void alignLineHorizontalVerticalPostPass( LVFormatter* fmt, formatted_line_t * frmline,
        bool hasInlineBoxes, int alignment=0, int usable_width=-1 );

// Punctuation helpers (defined in lvtextfm.cpp; used from
// measureText() in lvtextfm.cpp, which is earlier in the TU).
bool isLeftPunctuation( lChar32 c );
#if (USE_LIBUNIBREAK!=1)
bool isCJKPunctuation( lChar32 c );
bool isCJKLeftPunctuation( lChar32 c );
#endif

// True if every char in the word belongs on the upright CJK vertical path
// (Japanese horizontal marks or fullwidth fixed-pitch Latin/numerals).
bool isWordAllVerticalUprightChars(const lChar32 * text, int len);

// True if node is a vertical-ruby inline box: the boxing algorithm wraps
// the ruby table in an el_inlineBox whose parent has display:ruby.
static inline bool isRubyInlineBox(ldomNode * node) {
    return node
        && node->getParentNode()
        && node->getParentNode()->getStyle()->display == css_d_ruby
        && node->getChildCount() > 0
        && node->getChildNode(0)->getRendMethod() == erm_table;
}

// True if a ruby element ID belongs to the annotation side (rt, rp, rtc).
static inline bool isRubyAnnotId(lUInt16 id) {
    return id == el_rt || id == el_rp || id == el_rtc;
}

#endif // LVTEXTFM_FORK_H_INCLUDED
