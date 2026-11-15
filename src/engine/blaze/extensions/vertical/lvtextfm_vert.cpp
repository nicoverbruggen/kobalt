// =============================================================================
// Vertical-mode formatter layout code (vertical-rl / vertical-lr).
//
// Fork origin: this file was created in commit f8b0bbe1 ("vertical-rl
// Option C Phase 2b").  Its functions are vertical-writing-mode siblings
// of their `*Horizontal` counterparts in lvtextfm.cpp (which lived in
// a separate lvtextfm_layout_h.cpp from commit f8b0bbe1 through Phase
// C Step 2a, then folded back into lvtextfm.cpp in Steps 2b/2d).  They were initially ported
// (= cloned and adapted) from the same upstream lvtextfm.cpp originals,
// then specialised for column flow, kinsoku (line-breaking rules), and
// the Y=X coordinate swap.
//
// When reconciling an upstream change to lvtextfm.cpp that lands in a
// region covered by one of the *Horizontal functions, consider whether
// the analogous *Vertical function here needs the same fix.
// =============================================================================

// This file is #included at the end of lvtextfm.cpp, so MIN_WORD_LEN_TO_HYPHENATE
// / MAX_WORD_SIZE and the addLineHorizontal declaration are already in scope from
// the enclosing translation unit — no local re-#define or forward decl needed.
#include <vector>

// =============================================================================
// Small vertical-only hooks called from upstream-shaped lvtextfm.cpp code.
// Keeping their policy and explanatory comments here leaves only compact,
// mode-gated call sites in the shared formatter.
// =============================================================================

bool verticalTextDebugEnabled()
{
    return getenv("KO_DEBUG_VERT_BG") != NULL;
}

static bool verticalSpacingDebugEnabled()
{
    static int enabled = -1;
    if ( enabled < 0 )
        enabled = getenv("CRE_LOG_VERT_SPACING") ? 1 : 0;
    return enabled != 0;
}

bool isVerticalTextLineDebugClass(const lString32 &cls)
{
    return cls == "calibre10" || cls == "calibre11"
        || cls == "calibre4" || cls == "calibre7"
        || cls == "margin" || cls == "marginalia1"
        || cls == "jitsuki" || cls == "jitsuki1" || cls == "jitsuki2";
}

bool isVerticalInlineBorderDebugClass(const lString32 &cls)
{
    return cls == "marker" || cls == "marginalia1" || cls == "line";
}

int getVerticalImageInlineExtent(
    LVFormatter * fmt, ldomNode * node, int fallback)
{
    if ( !node )
        return fallback;
    int extent = fallback;
    if ( node->isEffectiveImage() ) {
        ldomNode * image_node = node->getEffectiveNode();
        int img_width = 0;
        int img_height = 0;
        getStyledImageSize(
            image_node, img_width, img_height, fmt->getLineExtent(), -1, true);
        if ( img_height > extent )
            extent = img_height;
    }
    for ( int i=0; i<node->getChildCount(); i++ ) {
        int child_extent = getVerticalImageInlineExtent(
            fmt, node->getChildNode(i), extent);
        if ( child_extent > extent )
            extent = child_extent;
    }
    return extent;
}

static bool isTcyDigitRun(
    const src_text_fragment_t * srcline,
    const formatted_word_t * word, int digit_limit)
{
    if ( !srcline || !srcline->t.text || word->t.len == 0
            || word->t.len > digit_limit )
        return false;
    for ( int i = 0; i < (int)word->t.len; i++ ) {
        lChar32 ch = srcline->t.text[word->t.start + i];
        if ( ch < U'0' || ch > U'9' )
            return false;
    }
    return true;
}

void applyVerticalTcyWord(
    LVFormatter * fmt, src_text_fragment_t * srcline,
    formatted_word_t * word, LVFont * font)
{
    if ( !css_wm_is_vertical(fmt->m_writing_mode)
            || !(srcline->flags & LTEXT_IS_TCY) )
        return;

    int tcu = getLTextExtraProperty(
        srcline, LTEXT_EXTRA_CSS_TEXT_COMBINE_UPRIGHT);
    if ( tcu >= css_tcu_digits_2 && tcu <= css_tcu_digits_4 ) {
        int limit = 2 + (tcu - css_tcu_digits_2);
        if ( !isTcyDigitRun(srcline, word, limit) )
            return;
    }

    int em = font->getSize();
    word->width = em;
    word->min_width = em;
    word->flags |= LTEXT_WORD_IS_TCY;
}

void trimVerticalLineEndSpaces(
    src_text_fragment_t * srcline, formatted_word_t * word)
{
    // A Latin word in vertical text is rendered as a rotated block.  Keeping a
    // trailing space in its string after layout discarded its width leaves a
    // visible blank at the bottom of the column.
    int trimmed_spaces = 0;
    while ( word->t.len > 0 ) {
        lChar32 ch = srcline->t.text[word->t.start + word->t.len - 1];
        if ( !(lGetCharProps(ch) & CH_PROP_SPACE) )
            break;
        word->t.len--;
        trimmed_spaces++;
    }
    if ( trimmed_spaces > 0 ) {
        ltext_vert_trailing_space_trim_count++;
        ltext_vert_trailing_space_trim_chars += trimmed_spaces;
    }
    word->flags &= ~LTEXT_WORD_CAN_ADD_SPACE_AFTER;
}

// =============================================================================
// Vertical-mode diagnostic globals (Phase C Step 1 + 2a relocation).
//
// These counters are spec oracles: regression specs read them via cre.cpp to
// assert vertical-rl layout invariants (ruby advance diff, column bleed, inline
// box layout gap, character overlap, inline image draw drift).  They live here
// so lvtextfm.cpp stays closer to upstream.  Their declarations and public
// reset/get API are in lvtextfm_vert_diag.h.
// Increment sites are:
//   - lvtextfm.cpp measureText (ruby_adv_diff)
//   - lvtextfm.cpp LFormattedText::Draw (bleed, ib_layout_gap, char_overlap)
//   - applyVerticalImageDraw below (inline image draw drift)
// Reset/getter functions are called from cre.cpp via extern linkage.
// =============================================================================

// Vertical-ruby: render_w − advance accumulated diff (was in lvtextfm.cpp).
int ltext_vert_ruby_adv_diff_total = 0;
int ltext_vert_ruby_adv_diff_max   = 0;
void ltext_reset_vert_ruby_adv_diff() {
    ltext_vert_ruby_adv_diff_total = 0;
    ltext_vert_ruby_adv_diff_max   = 0;
}
void ltext_get_vert_ruby_adv_diff(int *total_out, int *max_out) {
    *total_out = ltext_vert_ruby_adv_diff_total;
    *max_out   = ltext_vert_ruby_adv_diff_max;
}

// Vertical-rl bleed counters: accessible from cre.cpp via extern.
// Reset via ltext_reset_vert_bleed(); read via ltext_get_vert_bleed().
int ltext_vert_bleed_count = 0;
int ltext_vert_bleed_max_px = 0;

// Diagnostic: layout/draw position mismatch for vertical inline boxes.
// Fires when ib_word_x (from layout) > vert_min_next_x (draw tracking),
// meaning the layout placed the box further than the draw tracker expected —
// a gap is visible above the inline box.  With correct vert_layout_min_x
// tracking (e.g. after the CJK-only font_size fix for spaces) this is 0.
int ltext_vert_ib_layout_gap_total = 0;
int ltext_vert_ib_layout_gap_max   = 0;

void ltext_reset_vert_ib_layout_gap() {
    ltext_vert_ib_layout_gap_total = 0;
    ltext_vert_ib_layout_gap_max   = 0;
}
void ltext_get_vert_ib_layout_gap(int *total_out, int *max_out) {
    *total_out = ltext_vert_ib_layout_gap_total;
    *max_out   = ltext_vert_ib_layout_gap_max;
}

// Vertical-rl plain-character overlap counters.
// Fires when a CJK/plain character's y0 is less than the previous character's
// y0 + effective_width (= its slot end), meaning two characters overlap in the
// column (height) direction.  This is the character-overlap bug.
// Reset via ltext_reset_vert_char_overlap(); read via ltext_get_vert_char_overlap().
int ltext_vert_char_overlap_count = 0;
int ltext_vert_char_overlap_max_px = 0;

void ltext_reset_vert_char_overlap() {
    ltext_vert_char_overlap_count = 0;
    ltext_vert_char_overlap_max_px = 0;
}

void ltext_get_vert_char_overlap(int *count_out, int *max_px_out) {
    *count_out  = ltext_vert_char_overlap_count;
    *max_px_out = ltext_vert_char_overlap_max_px;
}

// Vertical-rl trailing-space trim counters.
// Fires when a line-ending word had trailing spaces removed from its rendered
// text, after its layout width had already discarded those spaces.
int ltext_vert_trailing_space_trim_count = 0;
int ltext_vert_trailing_space_trim_chars = 0;

void ltext_reset_vert_trailing_space_trim() {
    ltext_vert_trailing_space_trim_count = 0;
    ltext_vert_trailing_space_trim_chars = 0;
}

void ltext_get_vert_trailing_space_trim(int *count_out, int *chars_out) {
    *count_out = ltext_vert_trailing_space_trim_count;
    *chars_out = ltext_vert_trailing_space_trim_chars;
}

// Vertical-rl inline-image draw drift counters.
// Fires when an inline image is drawn at a different column-depth Y than the
// position implied by its formatted word->x. This catches the regression where
// a tiny gaiji image reserved space mid-column but was clamped back near the
// column top.
int ltext_vert_image_draw_count = 0;
int ltext_vert_image_draw_drift_count = 0;
int ltext_vert_image_draw_drift_max_px = 0;
int ltext_vert_image_cross_underreserve_count = 0;
int ltext_vert_image_cross_underreserve_max_px = 0;
int ltext_vert_mixed_image_axis_sample_count = 0;
int ltext_vert_mixed_image_axis_drift_count = 0;
int ltext_vert_mixed_image_axis_drift_max_px = 0;
int ltext_vert_single_image_placement_sample_count = 0;
int ltext_vert_single_image_clip_overflow_count = 0;
int ltext_vert_single_image_clip_overflow_max_px = 0;
int ltext_vert_single_image_center_error_max_px = 0;
// Exact-boundary hanging punctuation diagnostics. An attempt is a supported
// punctuation word whose slot crosses the regular clip bottom; recovery means
// the per-word overflow clip made it drawable; reject means it was still outside.
int ltext_vert_exact_hanging_attempt_count = 0;
int ltext_vert_hanging_layout_count = 0;
int ltext_vert_exact_hanging_clip_recovery_count = 0;
int ltext_vert_exact_hanging_clip_reject_count = 0;
int ltext_vert_exact_hanging_draw_count = 0;
int ltext_vert_exact_hanging_font_entry_count = 0;
int ltext_vert_exact_hanging_outside_regular_count = 0;
int ltext_vert_exact_hanging_active_clip_count = 0;
int ltext_vert_exact_hanging_regular_bottom = -1;
int ltext_vert_exact_hanging_last_regular_bottom = -1;
int ltext_vert_exact_hanging_last_active_bottom = -1;
int ltext_vert_exact_hanging_last_glyph_top = -1;
int ltext_vert_exact_hanging_last_glyph_bottom = -1;
int ltext_vert_fallback_size_sample_count = 0;
int ltext_vert_fallback_size_mismatch_count = 0;
int ltext_vert_fallback_size_mismatch_max_px = 0;

void ltext_reset_vert_fallback_size() {
    ltext_vert_fallback_size_sample_count = 0;
    ltext_vert_fallback_size_mismatch_count = 0;
    ltext_vert_fallback_size_mismatch_max_px = 0;
}

void ltext_get_vert_fallback_size(
    int *sample_count_out, int *mismatch_count_out, int *mismatch_max_px_out)
{
    *sample_count_out = ltext_vert_fallback_size_sample_count;
    *mismatch_count_out = ltext_vert_fallback_size_mismatch_count;
    *mismatch_max_px_out = ltext_vert_fallback_size_mismatch_max_px;
}

void ltext_reset_vert_exact_hanging_clip() {
    ltext_vert_exact_hanging_attempt_count = 0;
    ltext_vert_hanging_layout_count = 0;
    ltext_vert_exact_hanging_clip_recovery_count = 0;
    ltext_vert_exact_hanging_clip_reject_count = 0;
    ltext_vert_exact_hanging_draw_count = 0;
    ltext_vert_exact_hanging_font_entry_count = 0;
    ltext_vert_exact_hanging_outside_regular_count = 0;
    ltext_vert_exact_hanging_active_clip_count = 0;
    ltext_vert_exact_hanging_regular_bottom = -1;
    ltext_vert_exact_hanging_last_regular_bottom = -1;
    ltext_vert_exact_hanging_last_active_bottom = -1;
    ltext_vert_exact_hanging_last_glyph_top = -1;
    ltext_vert_exact_hanging_last_glyph_bottom = -1;
}

void ltext_get_vert_exact_hanging_clip(
    int *attempt_count_out, int *recovery_count_out, int *reject_count_out,
    int *draw_count_out, int *layout_count_out)
{
    *attempt_count_out = ltext_vert_exact_hanging_attempt_count;
    *recovery_count_out = ltext_vert_exact_hanging_clip_recovery_count;
    *reject_count_out = ltext_vert_exact_hanging_clip_reject_count;
    *draw_count_out = ltext_vert_exact_hanging_draw_count;
    *layout_count_out = ltext_vert_hanging_layout_count;
}

void ltext_get_vert_exact_hanging_glyph(
    int *font_entry_count_out, int *outside_regular_count_out,
    int *active_clip_count_out, int *regular_bottom_out,
    int *active_bottom_out, int *glyph_top_out, int *glyph_bottom_out)
{
    *font_entry_count_out = ltext_vert_exact_hanging_font_entry_count;
    *outside_regular_count_out = ltext_vert_exact_hanging_outside_regular_count;
    *active_clip_count_out = ltext_vert_exact_hanging_active_clip_count;
    *regular_bottom_out = ltext_vert_exact_hanging_last_regular_bottom;
    *active_bottom_out = ltext_vert_exact_hanging_last_active_bottom;
    *glyph_top_out = ltext_vert_exact_hanging_last_glyph_top;
    *glyph_bottom_out = ltext_vert_exact_hanging_last_glyph_bottom;
}

void ltext_reset_vert_image_draw_drift() {
    ltext_vert_image_draw_count = 0;
    ltext_vert_image_draw_drift_count = 0;
    ltext_vert_image_draw_drift_max_px = 0;
}

void ltext_get_vert_image_draw_drift(int *draw_count_out, int *drift_count_out, int *max_px_out) {
    *draw_count_out = ltext_vert_image_draw_count;
    *drift_count_out = ltext_vert_image_draw_drift_count;
    *max_px_out = ltext_vert_image_draw_drift_max_px;
}

void ltext_reset_vert_image_cross_underreserve() {
    ltext_vert_image_cross_underreserve_count = 0;
    ltext_vert_image_cross_underreserve_max_px = 0;
}

void ltext_get_vert_image_cross_underreserve(int *count_out, int *max_px_out) {
    *count_out = ltext_vert_image_cross_underreserve_count;
    *max_px_out = ltext_vert_image_cross_underreserve_max_px;
}

void ltext_reset_vert_mixed_image_axis() {
    ltext_vert_mixed_image_axis_sample_count = 0;
    ltext_vert_mixed_image_axis_drift_count = 0;
    ltext_vert_mixed_image_axis_drift_max_px = 0;
}

void ltext_get_vert_mixed_image_axis(int *sample_count_out, int *drift_count_out, int *max_px_out) {
    *sample_count_out = ltext_vert_mixed_image_axis_sample_count;
    *drift_count_out = ltext_vert_mixed_image_axis_drift_count;
    *max_px_out = ltext_vert_mixed_image_axis_drift_max_px;
}

void ltext_reset_vert_single_image_placement() {
    ltext_vert_single_image_placement_sample_count = 0;
    ltext_vert_single_image_clip_overflow_count = 0;
    ltext_vert_single_image_clip_overflow_max_px = 0;
    ltext_vert_single_image_center_error_max_px = 0;
}

void ltext_get_vert_single_image_placement(
    int *sample_count_out, int *clip_overflow_count_out,
    int *clip_overflow_max_px_out, int *center_error_max_px_out)
{
    *sample_count_out = ltext_vert_single_image_placement_sample_count;
    *clip_overflow_count_out = ltext_vert_single_image_clip_overflow_count;
    *clip_overflow_max_px_out = ltext_vert_single_image_clip_overflow_max_px;
    *center_error_max_px_out = ltext_vert_single_image_center_error_max_px;
}

void ltext_reset_vert_bleed() {
    ltext_vert_bleed_count = 0;
    ltext_vert_bleed_max_px = 0;
}

void ltext_get_vert_bleed(int *count_out, int *max_px_out) {
    *count_out  = ltext_vert_bleed_count;
    *max_px_out = ltext_vert_bleed_max_px;
}

struct VertJustifyGap {
    int word_index;
    int stretch_px;
    int shrink_px;
    int stretch_priority;
    int shrink_priority;
};

struct VertWordLayoutInfo {
    bool text;
    bool preformatted;
    bool object;
    bool inline_box;
    bool image;
    bool pad;
    bool cjk;
    bool ends_with_space;
    JLReqVertClass jfm_class;
    int em;
    int effective_width;
};

struct VertColumnFitChar {
    bool cjk;
    bool object;
    bool inline_box;
    JLReqVertClass jfm_class;
    int em;
    int effective_advance;
};

struct VertColumnFitState {
    int used;
    int prev_class;
    int prev_cjk_class;
};

static inline int vertEighthsToPx( int em, int eighths ) {
    return (em * eighths) / 8;
}

static inline int vertClampToInt16( int v ) {
    if ( v > 32767 )
        return 32767;
    if ( v < -32768 )
        return -32768;
    return v;
}

static inline int getVerticalInlineBoxDepth( src_text_fragment_t * srcline, int fallback_depth ) {
    return srcline && srcline->vert_inline_box_depth > 0
        ? (int)srcline->vert_inline_box_depth
        : fallback_depth;
}

static inline bool vertIsSpaceChar( lChar32 c ) {
    return c == ' ' || c == '\t' || c == 0x00A0 || c == 0x3000;
}

static inline int vertClampForward( int value, int lower_bound ) {
    return value < lower_bound ? lower_bound : value;
}

static inline bool isHalfEmJfmClass( JLReqVertClass cls ) {
    return getJLReqVertLayout(cls).width_halves == 1;
}

static inline int getVerticalEffectiveTextWidth( int width, int em, bool cjk, JLReqVertClass cls ) {
    return (cjk && !isHalfEmJfmClass(cls) && width < em) ? em : width;
}

static inline int getVerticalImageInlineAdvance( formatted_word_t * word ) {
    return word && word->o.height > 0 ? (int)word->o.height : (int)word->width;
}

// A vertical image advances down the inline axis by its physical height.
// addLineHorizontal() initially stores its physical width in frmline->width,
// which makes text-align center/right use the cross-axis size. Give alignment
// the vertical inline extent up front; applyVerticalFrmlineDimensions() will
// still reserve the physical image width across columns afterwards.
void prepareVerticalSingleImageLineAlignment(
    LVFormatter * fmt, formatted_line_t * frmline)
{
    if ( !css_wm_is_vertical(fmt->m_writing_mode)
            || frmline->word_count != 1 )
        return;
    formatted_word_t * word = &frmline->words[0];
    if ( word->flags & LTEXT_WORD_IS_IMAGE )
        frmline->width = getVerticalImageInlineAdvance(word);
}

static VertWordLayoutInfo getVerticalWordLayoutInfo( LVFormatter* fmt, formatted_word_t * word ) {
    VertWordLayoutInfo info;
    info.text = false;
    info.preformatted = false;
    info.object = false;
    info.inline_box = (word->flags & LTEXT_WORD_IS_INLINE_BOX) != 0;
    info.image = (word->flags & LTEXT_WORD_IS_IMAGE) != 0;
    info.pad = (word->flags & LTEXT_WORD_IS_PAD) != 0;
    info.cjk = false;
    info.ends_with_space = false;
    info.jfm_class = JLREQ_VERT_OTHER;
    info.em = fmt->m_pbuffer->strut_height > 0 ? fmt->m_pbuffer->strut_height : 20;
    info.effective_width = info.image ? getVerticalImageInlineAdvance(word) : (int)word->width;

    if ( (int)word->src_text_index >= fmt->m_pbuffer->srctextlen )
        return info;
    src_text_fragment_t * src = &fmt->m_pbuffer->srctext[word->src_text_index];
    info.object = (src->flags & LTEXT_SRC_IS_OBJECT) != 0;
    info.preformatted = (src->flags & LTEXT_FLAG_PREFORMATTED) != 0;
    if ( info.object )
        return info;
    info.text = src->t.text != NULL && word->t.len > 0;
    if ( src->t.font ) {
        int sz = ((LVFont *)src->t.font)->getSize();
        if ( sz > 0 )
            info.em = sz;
    }
    if ( !info.text )
        return info;

    bool word_acts_as_cjk = isWordAllVerticalUprightChars(src->t.text + word->t.start, (int)word->t.len);
    info.cjk = ((word->flags & (LTEXT_WORD_IS_CJK | LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK)) != 0)
               || word_acts_as_cjk;
    lChar32 first_char = src->t.text[word->t.start];
    lChar32 last_char = src->t.text[word->t.start + word->t.len - 1];
    info.ends_with_space = vertIsSpaceChar(last_char);
    if ( info.cjk )
        info.jfm_class = getJLReqVertClass(first_char);

    info.effective_width = getVerticalEffectiveTextWidth((int)word->width,
            info.em, info.cjk, info.jfm_class);
    return info;
}

static inline void resetVerticalColumnFitState( VertColumnFitState & state ) {
    state.used = 0;
    state.prev_class = 0;       // xkanjiskip tracker: 0 none, +1 non-CJK, -1 CJK
    state.prev_cjk_class = -1;  // JFM-glue tracker: JFM class of last CJK char
}

static VertColumnFitChar getVerticalColumnFitChar( LVFormatter* fmt, int index,
        int fallback_em ) {
    VertColumnFitChar item;
    item.cjk = (fmt->m_flags[index] & LCHAR_IS_CJK) != 0;
    item.object = (fmt->m_flags[index] & LCHAR_IS_OBJECT) != 0;
    item.inline_box = item.object && fmt->m_charindex[index] == INLINEBOX_CHAR_INDEX;
    item.jfm_class = JLREQ_VERT_OTHER;
    item.em = fallback_em;

    if ( !item.object && fmt->m_srcs[index] && fmt->m_srcs[index]->t.font ) {
        int sz = ((LVFont*)fmt->m_srcs[index]->t.font)->getSize();
        if ( sz > 0 )
            item.em = sz;
    }

    int adv_delta = fmt->m_advance[index] - (index > 0 ? fmt->m_advance[index-1] : 0);
    if ( item.inline_box ) {
        item.effective_advance = getVerticalInlineBoxDepth(fmt->m_srcs[index],
                fmt->m_srcs[index]->o.width);
        return item;
    }
    if ( item.object && fmt->m_srcs[index]
            && (fmt->m_srcs[index]->o.objflags & LTEXT_OBJECT_IS_IMAGE) ) {
        item.effective_advance = fmt->m_srcs[index]->o.height > 0
                ? fmt->m_srcs[index]->o.height : fmt->m_srcs[index]->o.width;
        return item;
    }

    if ( item.cjk && !item.object )
        item.jfm_class = getJLReqVertClass(fmt->m_text[index]);
    item.effective_advance = getVerticalEffectiveTextWidth(adv_delta,
            item.em, item.cjk, item.jfm_class);
    return item;
}

static int addVerticalColumnFitChar( VertColumnFitState & state,
        const VertColumnFitChar & item ) {
    int effective_advance = item.effective_advance;
    // Phase 5 inter-item spacing that Draw inserts but m_advance does not:
    // xkanjiskip at CJK<->non-CJK boundaries and JFM inter-class glue between
    // consecutive CJK chars.  Inline boxes reset the chain because their
    // surrounding spacing is folded into their measured depth.
    if ( !item.object &&
           ( (state.prev_class == +1 && item.cjk) || (state.prev_class == -1 && !item.cjk) ) ) {
        effective_advance += item.em / 4;
    }
    if ( item.cjk && state.prev_cjk_class >= 0 ) {
        JLReqVertGlueSpec spec = getJLReqVertGlueSpec(
                (JLReqVertClass)state.prev_cjk_class, item.jfm_class);
        effective_advance += vertEighthsToPx(item.em, spec.base_eighths);
    }

    state.used += effective_advance;
    if ( item.object ) {
        state.prev_class = 0;
        state.prev_cjk_class = -1;
    }
    else {
        state.prev_class = item.cjk ? -1 : +1;
        state.prev_cjk_class = item.cjk ? (int)item.jfm_class : -1;
    }
    return effective_advance;
}

static void shiftVerticalWordsFrom( formatted_line_t * frmline, int start_index, int delta ) {
    if ( delta == 0 )
        return;
    for ( int i=start_index; i<(int)frmline->word_count; i++ ) {
        int next_x = (int)frmline->words[i].x + delta;
        frmline->words[i].x = (lInt16)vertClampToInt16(next_x);
    }
}

static int distributeVerticalJustifyByPriority( formatted_line_t * frmline,
        const std::vector<VertJustifyGap> & gaps, int remaining, bool stretch ) {
    if ( remaining <= 0 || gaps.empty() )
        return 0;
    int total_applied = 0;
    for ( int priority=0; priority<=3 && remaining>0; priority++ ) {
        int total_capacity = 0;
        for ( size_t i=0; i<gaps.size(); i++ ) {
            int cap = stretch ? gaps[i].stretch_px : gaps[i].shrink_px;
            int gap_priority = stretch ? gaps[i].stretch_priority : gaps[i].shrink_priority;
            if ( gap_priority == priority && cap > 0 )
                total_capacity += cap;
        }
        if ( total_capacity <= 0 )
            continue;
        int to_apply = remaining < total_capacity ? remaining : total_capacity;
        int distributed = 0;
        int carry = 0;
        for ( size_t i=0; i<gaps.size(); i++ ) {
            int cap = stretch ? gaps[i].stretch_px : gaps[i].shrink_px;
            int gap_priority = stretch ? gaps[i].stretch_priority : gaps[i].shrink_priority;
            if ( gap_priority != priority || cap <= 0 )
                continue;
            int amount = (cap * to_apply + carry) / total_capacity;
            carry = (cap * to_apply + carry) % total_capacity;
            if ( amount > cap )
                amount = cap;
            if ( amount <= 0 && distributed < to_apply )
                amount = 1;
            if ( amount > to_apply - distributed )
                amount = to_apply - distributed;
            if ( amount <= 0 )
                continue;
            shiftVerticalWordsFrom(frmline, gaps[i].word_index, stretch ? amount : -amount);
            distributed += amount;
        }
        remaining -= distributed;
        total_applied += distributed;
    }
    if ( stretch && remaining > 0 ) {
        int count = 0;
        for ( size_t i=0; i<gaps.size(); i++ ) {
            int cap = stretch ? gaps[i].stretch_px : gaps[i].shrink_px;
            if ( cap > 0 )
                count++;
        }
        if ( count <= 0 )
            count = (int)gaps.size();
        int div = remaining / count;
        int mod = remaining % count;
        for ( size_t i=0; i<gaps.size() && remaining>0; i++ ) {
            int cap = stretch ? gaps[i].stretch_px : gaps[i].shrink_px;
            if ( cap <= 0 && count != (int)gaps.size() )
                continue;
            int amount = div;
            if ( mod > 0 ) {
                amount++;
                mod--;
            }
            if ( amount <= 0 )
                amount = 1;
            if ( amount > remaining )
                amount = remaining;
            shiftVerticalWordsFrom(frmline, gaps[i].word_index, stretch ? amount : -amount);
            remaining -= amount;
            total_applied += amount;
        }
    }
    return total_applied;
}

static void collectVerticalJustifyGaps( std::vector<VertJustifyGap> & gaps,
        int word_index, formatted_word_t * prev_word,
        const VertWordLayoutInfo & prev, const VertWordLayoutInfo & curr,
        const JLReqVertGlueSpec & jfm_spec, bool cjk_non_cjk_boundary ) {
    if ( prev.preformatted || curr.preformatted )
        return;
    if ( prev.object || curr.object || prev.inline_box || curr.inline_box
            || prev.image || curr.image || prev.pad || curr.pad )
        return;
    VertJustifyGap gap;
    gap.word_index = word_index;
    gap.stretch_px = 0;
    gap.shrink_px = 0;
    gap.stretch_priority = 1;
    gap.shrink_priority = 1;

    if ( prev.ends_with_space && (prev_word->flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER) ) {
        gap.stretch_px = prev.em / 2;
        if ( gap.stretch_px < 1 )
            gap.stretch_px = 1;
        gap.shrink_px = (int)prev_word->width - (int)prev_word->min_width;
        if ( gap.shrink_px < 0 )
            gap.shrink_px = 0;
        gap.stretch_priority = 0;
        gap.shrink_priority = 0;
    }
    else if ( prev.cjk && curr.cjk ) {
        gap.stretch_px = vertEighthsToPx(curr.em, jfm_spec.stretch_eighths);
        gap.shrink_px = vertEighthsToPx(curr.em, jfm_spec.shrink_eighths);
        if ( jfm_spec.kanjiskip_stretch || (jfm_spec.base_eighths == 0
                && jfm_spec.stretch_eighths == 0 && jfm_spec.shrink_eighths == 0
                && !jfm_spec.is_kern) ) {
            gap.stretch_px += curr.em / 4; // kanjiskip = {0, .25, 0}
        }
        gap.stretch_priority = 1 + (int)jfm_spec.stretch_priority;
        gap.shrink_priority = 1 + (int)jfm_spec.shrink_priority;
    }
    else if ( cjk_non_cjk_boundary ) {
        gap.stretch_px = vertEighthsToPx(curr.em, 2); // xkanjiskip stretch .25em
        gap.shrink_px = vertEighthsToPx(curr.em, 1);  // xkanjiskip shrink .125em
        gap.stretch_priority = 1;
        gap.shrink_priority = 1;
    }

    if ( gap.stretch_px > 0 || gap.shrink_px > 0 )
        gaps.push_back(gap);
}

static void applyVerticalJustification( formatted_line_t * frmline,
        const std::vector<VertJustifyGap> & gaps, int alignment, int usable_width ) {
    if ( alignment != LTEXT_ALIGN_WIDTH || usable_width <= 0 || gaps.empty() )
        return;
    int extra = usable_width - (int)frmline->width;
    if ( extra == 0 )
        return;
    if ( extra < 0 && frmline->x > 0 )
        return;
    int applied = distributeVerticalJustifyByPriority(frmline, gaps,
            extra > 0 ? extra : -extra, extra > 0);
    if ( applied > 0 ) {
        frmline->width = (lUInt16)(extra > 0
                ? (int)frmline->width + applied
                : (int)frmline->width - applied);
    }
}

static void syncVerticalInlineBoxPositionsAfterJustify( LVFormatter* fmt,
        formatted_line_t * frmline ) {
    int vert_layout_min_x = 0;
    for ( int i=0; i<(int)frmline->word_count; i++ ) {
        formatted_word_t * word = &frmline->words[i];
        if ( !(word->flags & LTEXT_WORD_IS_INLINE_BOX) ) {
            VertWordLayoutInfo info = getVerticalWordLayoutInfo(fmt, word);
            if ( info.object || info.image || info.pad ) {
                continue;
            }
            int clamped_x = vertClampForward((int)word->x, vert_layout_min_x);
            int next_x = clamped_x + info.effective_width;
            if ( next_x > vert_layout_min_x )
                vert_layout_min_x = next_x;
            continue;
        }

        if ( (int)word->src_text_index >= fmt->m_pbuffer->srctextlen )
            continue;
        src_text_fragment_t * srcline = &fmt->m_pbuffer->srctext[word->src_text_index];
        ldomNode * node = (ldomNode *) srcline->object;
        if ( !node )
            continue;
        RenderRectAccessor node_fmt( node );
        int ib_layout_depth = getVerticalInlineBoxDepth(srcline, (int)word->width);
        int clamped_ib_x = vertClampForward((int)word->x, vert_layout_min_x);
        node_fmt.setX( frmline->x + clamped_ib_x );
        node_fmt.push();
        vert_layout_min_x = clamped_ib_x + ib_layout_depth;
    }
}

// =============================================================================
// alignLineHorizontalVerticalPostPass
//
// Origin: extracted from upstream `LVFormatter::alignLine()` (the
// `if (hasInlineBoxes) { ... }` tail block, ~line 3645 in upstream).  The
// upstream block handled inline-box absolute positioning + MathML baseline
// adjustment.  The fork added a parallel branch for plain (non-inline-box)
// words in vertical writing mode that mirrors Draw's vert_min_next_x state
// (vert_layout_min_x), plus Phase 5 (LuaTeX-ja JFM) inter-class glue and
// xkanjiskip shifts so getRect() returns the same position Draw uses.
//
// Extracted to lvtextfm_vert.cpp during the upstream-merge-friendliness
// pass so lvtextfm.cpp's alignLine stays close to upstream.  Future upstream
// changes to the IB-positioning region must be ported here.
//
// Invocation guard: called whenever (hasInlineBoxes || vertical_mode).
// The Phase 5 plain-word post-pass MUST run on every vertical-mode line,
// not just lines with inline boxes — otherwise lines without ruby would
// have a different word->x layout from lines with ruby, breaking column
// stride consistency (see spec/unit/ruby_position_spec.lua).
// =============================================================================
void alignLineHorizontalVerticalPostPass( LVFormatter* fmt, formatted_line_t * frmline,
        bool hasInlineBoxes, int alignment, int usable_width ) {
    #if MATHML_SUPPORT==1
        lUInt16 needed_baseline = frmline->baseline;
        lUInt16 needed_height = frmline->height;
    #endif
    // In vertical mode we also mirror the vert_min_next_x clamping that
    // Draw() applies at render time.  Without this, getRect() returns the
    // pre-clamp layout position, causing sbox.y to sit above the rendered
    // glyph by clamp_delta (≈ 1/5 em when the ruby group nearly touches
    // the preceding character).
    bool is_vert_frmline = css_wm_is_vertical(fmt->m_pbuffer->writing_mode);
    int orthogonal_cross_extent = 0;
    if ( is_vert_frmline ) {
        for ( int i=0; i<frmline->word_count; i++ ) {
            formatted_word_t * word = &frmline->words[i];
            if ( (int)word->src_text_index >= fmt->m_pbuffer->srctextlen )
                continue;
            src_text_fragment_t * src = &fmt->m_pbuffer->srctext[word->src_text_index];
            if ( (src->flags & LTEXT_SRC_IS_OBJECT)
                    && (src->o.objflags & LTEXT_OBJECT_IS_ORTHOGONAL_SPACER)
                    && (int)word->o.height > orthogonal_cross_extent )
                orthogonal_cross_extent = word->o.height;
        }
    }
    int vert_layout_min_x = 0;  // mirrors vert_min_next_x in Draw()
    std::vector<VertJustifyGap> vert_justify_gaps;
    VertWordLayoutInfo prev_info;
    formatted_word_t * prev_word = NULL;
    bool have_prev_info = false;
    for ( int i=0; i<frmline->word_count; i++ ) {
        formatted_word_t * wi = &frmline->words[i];
        if ( is_vert_frmline && !(wi->flags & LTEXT_WORD_IS_INLINE_BOX) ) {
            VertWordLayoutInfo curr_info = getVerticalWordLayoutInfo(fmt, wi);
            JLReqVertGlueSpec jfm_spec;
            bool boundary_cjk_non_cjk = false;
            int base_glue = 0;
            if ( have_prev_info && curr_info.text && prev_info.text ) {
                boundary_cjk_non_cjk = (prev_info.cjk && !curr_info.cjk)
                                    || (!prev_info.cjk && curr_info.cjk);
                if ( boundary_cjk_non_cjk ) {
                    base_glue = vertEighthsToPx(curr_info.em, 2); // xkanjiskip = .25em
                }
                else if ( prev_info.cjk && curr_info.cjk ) {
                    jfm_spec = getJLReqVertGlueSpec(prev_info.jfm_class, curr_info.jfm_class);
                    base_glue = vertEighthsToPx(curr_info.em, jfm_spec.base_eighths);
                }
                if ( base_glue > 0 )
                    vert_layout_min_x += base_glue;
                collectVerticalJustifyGaps(vert_justify_gaps, i, prev_word,
                        prev_info, curr_info, jfm_spec, boundary_cjk_non_cjk);
            }
            // Mirror Draw()'s vert_min_next_x clamping: if a previous word's
            // effective advance pushed vert_layout_min_x past this word's layout
            // position, update word->x so getRect() returns the rendered position,
            // not the raw layout position. This prevents highlights from appearing
            // above the actual glyph when font CJK advances are slightly < font_size.
            if ( (int)wi->x < vert_layout_min_x )
                wi->x = (lInt16)vert_layout_min_x;
            int next_x = wi->x + curr_info.effective_width;
            if ( next_x > vert_layout_min_x )
                vert_layout_min_x = next_x;
            if ( curr_info.object || curr_info.image || curr_info.pad ) {
                have_prev_info = false;
                prev_word = NULL;
            }
            else {
                prev_info = curr_info;
                prev_word = wi;
                have_prev_info = true;
            }
            continue;
        }
        if ( frmline->words[i].flags & LTEXT_WORD_IS_INLINE_BOX ) {
            formatted_word_t * word = &frmline->words[i];
            src_text_fragment_t * srcline = &fmt->m_pbuffer->srctext[word->src_text_index];
            ldomNode * node = (ldomNode *) srcline->object;
            ldomNode * source_node = node->getEffectiveNode();
            if ( source_node != node ) {
                // We ended up positionning the cloneNode for an inlineBox on
                // the ::first-line of the paragraph, which means the original
                // inlineBox has not been positionned and won't be shown.
                // Flag that original node as not-rendered/discarded, so that
                // getRect(), when called on a xpointer to the original source,
                // know it has to climb to its clondeNode and use its rect.
                RenderRectAccessor source_fmt( source_node );
                RENDER_RECT_SET_FLAG(source_fmt, BOX_IS_DISCARDED);
            }
            RenderRectAccessor node_fmt( node );
            bool vert_mode = is_vert_frmline;
            // Inline-box advance MUST match the Draw side's ib_actual_depth
            // (lvtextfm.cpp applyVerticalInlineBoxDraw): the actual visual
            // column depth is stored in srcline->vert_inline_box_depth by
            // measureText (= base text horizontal advance for ruby), which can be
            // up to half an em SHORTER than word->width (the TTB-based advance) for
            // ruby boxes.  Advancing vert_layout_min_x by word->width here
            // while Draw advances vert_min_next_x by that depth would put
            // the following char's highlight rect (positioned by word->x) half
            // an em BELOW its glyph.  Use the same fallback chain as Draw.
            int ib_layout_depth = getVerticalInlineBoxDepth(srcline, (int)word->width);
            if ( RENDER_RECT_HAS_FLAG(node_fmt, BOX_IS_POSITIONNED) ) {
                if ( is_vert_frmline ) {
                    // The box was positioned in a previous pass against an
                    // earlier vert_layout_min_x.  Re-clamp to the current
                    // vert_layout_min_x so this branch mirrors what
                    // applyVerticalInlineBoxDraw does at render time
                    // (`clamped_ib_x = max(node_x − frmline->x, state)`).
                    // Without this clamp the next word's wi->x update bases
                    // on the stale, smaller value and ends up 0.5em (or
                    // whatever the inter-class glue / xkanjiskip added since
                    // the original positioning) behind where DRAW will place
                    // the glyph, producing a highlight rect above the glyph
                    // — visible on `<CJK><ruby><CJK>` sequences with a JFM
                    // glue boundary entering the ruby.
                    bool is_ruby_box = isRubyInlineBox(node);
                    int clamped_x = node_fmt.getX() - frmline->x;
                    if ( is_ruby_box )
                        clamped_x = vert_layout_min_x;
                    else
                        clamped_x = vertClampForward(clamped_x, vert_layout_min_x);
                    if ( node_fmt.getX() != frmline->x + clamped_x ) {
                        // Also push the recorded position so getRect() for
                        // characters inside the box reports the actually-
                        // drawn column position.
                        node_fmt.setX( frmline->x + clamped_x );
                        node_fmt.push();
                    }
                    word->x = (lInt16)vertClampToInt16(clamped_x);
                    int nx = clamped_x + ib_layout_depth;
                    if ( nx > vert_layout_min_x ) vert_layout_min_x = nx;
                    have_prev_info = false;
                    prev_word = NULL;
                }
                continue;
            }
            RENDER_RECT_SET_FLAG(node_fmt, BOX_IS_POSITIONNED);
            if ( is_vert_frmline ) {
                // Apply the same clamping that Draw() will use at render time
                // so getRect() returns the rendered (not pre-clamp) position.
                bool is_ruby_box = isRubyInlineBox(node);
                int ib_word_x = word->x;
                int clamped_ib_x = is_ruby_box
                                  ? vert_layout_min_x
                                  : vertClampForward(ib_word_x, vert_layout_min_x);
                word->x = (lInt16)vertClampToInt16(clamped_ib_x);
                node_fmt.setX( frmline->x + clamped_ib_x );
                vert_layout_min_x = clamped_ib_x + ib_layout_depth;
                have_prev_info = false;
                prev_word = NULL;
            } else {
                node_fmt.setX( frmline->x + word->x );
            }
            // In vertical mode, Y encodes the horizontal offset from the right edge.
            // Baseline alignment (a vertical concept in horizontal text) must not be
            // applied as a horizontal shift; doing so displaces ruby base characters
            // rightward by (baseline - ruby_baseline). Use frmline->y directly.
            bool is_vert = vert_mode;
            // For vertical mode: position the ruby block so the annotation
            // overhangs to the right of the column (per JLReq) for all
            // line-spacing values.
            // col_width is always strut_height (no inflation for ruby).
            //
            // For ruby inline boxes (display:ruby): annotation is at doc_y=0
            // (rightmost on screen), base at doc_y=annotation_h.  The body
            // font em is independent of line-height, so
            //   vert_y_adjust = em - box_h = -annotation_h
            // places the annotation to the right of the column boundary
            // regardless of col_w.  The base char is right-aligned within
            // the column (sbox.right = column right = line_x_col), which
            // keeps the annotation directly adjacent to the base (JLReq).
            //
            // For other small inline boxes: centre within strut.
            int vert_y_adjust = 0;
            if ( is_vert ) {
                int box_h = word->o.height;
                int col_w = fmt->m_pbuffer->strut_height;
                if ( orthogonal_cross_extent > col_w )
                    col_w = orthogonal_cross_extent;
                bool is_ruby_box = isRubyInlineBox(node);
                if ( is_ruby_box ) {
                    LVFontRef fnt = node->getFont();
                    int em = !fnt.isNull() ? fnt->getSize() : 0;
                    if ( em > 0 && em < col_w )
                        vert_y_adjust = (col_w + em) / 2 - box_h;  // centred
                    else
                        vert_y_adjust = col_w - box_h; // fallback
                } else if ( node->getChildCount() == 1
                        && node->getChildNode(0)->isEffectiveImage() ) {
                    // Keep a large inline-block image on the current column;
                    // the generic negative overhang below moves it off-page.
                    if ( col_w > box_h )
                        vert_y_adjust = (col_w - box_h) / 2;
                } else if ( col_w > box_h ) {
                    vert_y_adjust = (col_w - box_h) / 2;  // centre small box
                } else if ( box_h > col_w ) {
                    vert_y_adjust = col_w - box_h;  // negative: annotation overhangs right
                }
            }
            node_fmt.setY( (int)frmline->y + (is_vert ? vert_y_adjust : (int)frmline->baseline - (int)word->o.baseline + (int)word->y) );
            node_fmt.push();
            #if MATHML_SUPPORT==1
                ldomNode * unboxedParent = node->getUnboxedParent();
                if ( unboxedParent ) {
                    lUInt16 unboxedParentId = unboxedParent->getNodeId();
                    if ( unboxedParentId >= EL_MATHML_START && unboxedParentId <= EL_MATHML_END ) {
                        ensureMathMLVerticalStretch(node, frmline->y, frmline->baseline, frmline->height,
                                                                        needed_baseline, needed_height);
                    }
                }
            #endif
        }
    }
    if ( is_vert_frmline ) {
        if ( vert_layout_min_x > (int)frmline->width )
            frmline->width = (lUInt16)vert_layout_min_x;
        applyVerticalJustification(frmline, vert_justify_gaps, alignment, usable_width);
        if ( hasInlineBoxes )
            syncVerticalInlineBoxPositionsAfterJustify(fmt, frmline);
    }
    #if MATHML_SUPPORT==1
        if ( needed_height > frmline->height ) {
            frmline->height = needed_height;
        }
        if ( needed_baseline > frmline->baseline ) {
            int baseline_shift = needed_baseline - frmline->baseline;
            frmline->baseline = needed_baseline;
            // We need to update all the inlineBoxes absolute positions in the paragraph,
            // as they are all to be positionned relative to the baseline, which has moved.
            for ( int i=0; i<frmline->word_count; i++ ) {
                if ( frmline->words[i].flags & LTEXT_WORD_IS_INLINE_BOX ) {
                    formatted_word_t * word = &frmline->words[i];
                    src_text_fragment_t * srcline = &fmt->m_pbuffer->srctext[word->src_text_index];
                    ldomNode * node = (ldomNode *) srcline->object;
                    RenderRectAccessor node_fmt( node );
                    node_fmt.setY( node_fmt.getY() + baseline_shift );
                    node_fmt.push();
                }
            }
        }
    #endif
}

// Line-start kinsoku: characters that JLReq prohibits at the start of a line / column.
// In addition to cjkt_closing_bracket (handled separately), LuaTeX-ja's
// kinsoku_table prohibits: prolonged sound mark (ー), kanji iteration mark (々々),
// kana iteration marks (ヽヾゝゞ), vertical iteration mark (〻), and small kana
// (ぁぃぅぇぉっゃゅょゎ etc.).
// These are pulled back into the previous column when they would otherwise
// start a new column.
static inline bool isVertLineStartProhibitedExt(lChar32 ch) {
    switch (ch) {
        case 0x30FC: // ー KATAKANA-HIRAGANA PROLONGED SOUND MARK
        case 0x3005: // 々 IDEOGRAPHIC ITERATION MARK
        case 0x303B: // 〻 VERTICAL IDEOGRAPHIC ITERATION MARK
        case 0x309D: // ゝ HIRAGANA ITERATION MARK
        case 0x309E: // ゞ HIRAGANA VOICED ITERATION MARK
        case 0x30FD: // ヽ KATAKANA ITERATION MARK
        case 0x30FE: // ヾ KATAKANA VOICED ITERATION MARK
        case 0x3031: // 〱 VERTICAL KANA REPEAT MARK
        case 0x3032: // 〲 VERTICAL KANA REPEAT WITH VOICED SOUND MARK
        case 0x3033: // 〳 VERTICAL KANA REPEAT MARK UPPER HALF
        case 0x3034: // 〴 VERTICAL KANA REPEAT WITH VOICED SOUND MARK UPPER HALF
        case 0x3035: // 〵 VERTICAL KANA REPEAT MARK LOWER HALF
        // Small hiragana
        case 0x3041: // ぁ
        case 0x3043: // ぃ
        case 0x3045: // ぅ
        case 0x3047: // ぇ
        case 0x3049: // ぉ
        case 0x3063: // っ
        case 0x3083: // ゃ
        case 0x3085: // ゅ
        case 0x3087: // ょ
        case 0x308E: // ゎ
        case 0x3095: // ゕ
        case 0x3096: // ゖ
        // Small katakana
        case 0x30A1: // ァ
        case 0x30A3: // ィ
        case 0x30A5: // ゥ
        case 0x30A7: // ェ
        case 0x30A9: // ォ
        case 0x30C3: // ッ
        case 0x30E3: // ャ
        case 0x30E5: // ュ
        case 0x30E7: // ョ
        case 0x30EE: // ヮ
        case 0x30F5: // ヵ
        case 0x30F6: // ヶ
        // Kana sound marks (line-start prohibited)
        case 0x309B: // ゛ (combining voiced sound mark, sometimes line-start prohibited)
        case 0x309C: // ゜ (combining semi-voiced sound mark)
            return true;
        default:
            return false;
    }
}

// Returns true if 'ch' is one of the Japanese horizontal-mark characters
// that must be routed through the CJK +vert path in vertical mode.
// needsVerticalRotation90CW() also returns true for Latin/ASCII, so it cannot
// be used here — we need only the specific marks that are non-CJK by
// lStr_isCJK() but should still be rendered upright via +vert.
static inline bool isJapaneseHorizontalMark(lChar32 c) {
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
            return false;
    }
}

// JLReq 3.2.4 handles full-width fixed-pitch Latin letters and European
// numerals as ideographic characters in vertical composition: one em per
// character, upright, and with CJK spacing.  The generic classifier does not
// set the CJK word flag for these code points, so encode that rule here.
static inline bool isFullwidthLatinOrEuropeanNumeral(lChar32 c) {
    return (c >= 0xFF10 && c <= 0xFF19) // ０-９
        || (c >= 0xFF21 && c <= 0xFF3A) // Ａ-Ｚ
        || (c >= 0xFF41 && c <= 0xFF5A); // ａ-ｚ
}

// Returns true if every character in [text, text+len) must use the CJK
// vertical path: Japanese horizontal marks, or JLReq 3.2.4 full-width Latin
// letters/numerals.
//
// Used by LFormattedText::Draw's classification step to route such words
// through the CJK +vert glyph path instead of the Latin-in-vertical
// render+rotate-as-block path.
bool isWordAllVerticalUprightChars(const lChar32 * text, int len) {
    if (!text || len <= 0)
        return false;
    for (int i = 0; i < len; i++) {
        if (!isJapaneseHorizontalMark(text[i])
                && !isFullwidthLatinOrEuropeanNumeral(text[i]))
            return false;
    }
    return true;
}

// Returns true if 'ch' is a Japanese/CJK sentence-end character that must not
// start a new column (line-start kinsoku) and should hang at the bottom of the
// current column (burasagari / hanging punctuation) instead.
static inline bool isVerticalHangingChar(lChar32 ch) {
    switch (ch) {
        // JLReq burasagari (ぶら下げ) applies only to the half-em sentence
        // punctuation 、。 (and their fullwidth/middle-dot kin ，．・), whose
        // overhang past the column bottom is ≤ font_size/2 — matching the hang
        // allowance the break test assumes.  ！？ (full em) and … (often a 2-em
        // composite via +vrt2) would overhang far past clip.bottom and get
        // clipped, so they are deliberately NOT hung here.
        case 0x3001: // 、IDEOGRAPHIC COMMA (touten)
        case 0x3002: // 。IDEOGRAPHIC FULL STOP (kuten)
        case 0x30FB: // ・KATAKANA MIDDLE DOT
        case 0xFF0C: // ，FULLWIDTH COMMA
        case 0xFF0E: // ．FULLWIDTH FULL STOP
            return true;
        default:
            return false;
    }
}

// Step 2: Wrapper that delegates word placement to addLineHorizontal,
// then patches frmline coordinates for vertical layout.
//
// The horizontal function handles all word creation, CJK spacing,
// bidi, overlap correction, alignment, and justify unchanged.
// We only patch the 3 frmline-level coordinates and the m_line_advance
// direction after it returns.
//
// NOTE: addLineHorizontal calls alignLineHorizontal internally, which does
// space-based justify. For CJK-heavy text (each char is its own word),
// this is a reasonable approximation of vertical letter-spacing justify.
// Proper vertical justify (Step 3) will be implemented after Step 4
// (DrawVertical) provides visible output for verification.
// -----------------------------------------------------------------------------
// addLineVertical
// Vertical-rl sibling of addLineHorizontal (lvtextfm.cpp), originally
// ported from upstream `LVFormatter::addLine` and re-specialised for column
// flow.  Cross-check against addLineHorizontal when upstream changes the
// horizontal one.
// -----------------------------------------------------------------------------
void addLineVertical( LVFormatter* fmt, int start, int end, int x, src_text_fragment_t * para, bool first, bool last, bool preFormattedOnly, bool isLastPara, bool hasInlineBoxes )
{
    // Delegate all word placement to horizontal function.
    // This creates the frmline, words, sets word->x/width/y, calls alignLineHorizontal,
    // and updates m_line_advance and m_pbuffer->height.
    //
    // For vertical text: addLineHorizontal already sets everything correctly.
    // - frmline->y = old m_line_advance = column's accumulated X offset from block start.
    // - frmline->height = frmline->width = col_width (set in addLineHorizontal's vertical fix).
    // - m_line_advance += col_width, so m_pbuffer->height = N * col_width after N columns.
    // - word->x = cumulative glyph Y advance within column (used by Draw() as y-offset).
    // - word->y = baseline valign offset (used by Draw() as x-offset correction).
    // Do NOT undo m_line_advance; that would corrupt m_pbuffer->height used for block height.
    addLineHorizontal( fmt, start, end, x, para, first, last, preFormattedOnly, isLastPara, hasInlineBoxes );
}

    /// Split paragraph into lines (vertical layout - Step 1)
///
/// This is the incremental Step 1 implementation: line splitting with
/// coordinate swap. Uses page_height as the line extent instead of width.
///
/// Key differences from processParagraphHorizontal():
/// - maxWidth -> maxHeight (using the current column's available inline extent)
/// - addLineHorizontal -> addLineVertical
///
/// addLineVertical currently delegates to addLineHorizontal, so line creation
/// is still horizontal. Step 2 will implement proper vertical addLine.
///
// -----------------------------------------------------------------------------
// processParagraphVertical
// Vertical-rl sibling of processParagraphHorizontal (lvtextfm.cpp).
// Originally ported from upstream `LVFormatter::processParagraph` and adapted
// for column-wise layout + kinsoku.  Cross-check when the horizontal one moves.
// -----------------------------------------------------------------------------
void processParagraphVertical( LVFormatter* fmt, int start, int end, bool isLastPara )
    {
        TR("processParagraphVertical(%d, %d)", start, end);

        // ensure buffer size is ok for paragraph
        fmt->allocate( start, end );
        // copy paragraph text to buffer
        fmt->copyText( start, end );
        // measure paragraph text (with LFNT_HINT_IS_VERTICAL, uses y_advance)
        fmt->measureText();

        // We keep as 'para' the first source text, as it carries
        // the text alignment to use with all added lines.
        src_text_fragment_t * para = &fmt->m_pbuffer->srctext[start];
        const int alignment = para->flags & LTEXT_FLAG_NEWLINE;

        // detect case with inline preformatted text inside block with line feeds
        bool preFormattedOnly = true;
        for ( int i=start; i<end; i++ ) {
            if ( !(fmt->m_pbuffer->srctext[i].flags & LTEXT_FLAG_PREFORMATTED) ) {
                preFormattedOnly = false;
                break;
            }
        }
        if ( preFormattedOnly ) {
            bool lfFound = false;
            for ( int i=0; i<fmt->m_length; i++ ) {
                if ( fmt->m_text[i]=='\n' ) {
                    lfFound = true;
                    break;
                }
            }
            preFormattedOnly = preFormattedOnly && lfFound;
        }

        // For vertical text, the line extent is the page height (block direction).
        // Characters advance vertically (y_advance from HarfBuzz), and lines
        // (columns) are stacked horizontally (right to left for vertical-rl).
        //
        // IMPORTANT: m_advance[] here uses HarfBuzz VERTICAL advances with cluster
        // semantics — cluster members get advance=0, cluster end gets the full
        // cluster advance. This causes line-breaking to fire later than expected,
        // allowing too many characters into a column.
        //
        // In Draw(), word->x accumulates HORIZONTAL glyph widths (one per character,
        // not cluster-level), so the column height in pixels = sum(word->width).
        // We must limit that sum to (page_height - y), not m_advance.
        //
        // Fix: get the font's per-character advance (≈ font size) and use
        // char_count × font_advance for the column-height limit check.
        int fullHeight = fmt->m_pbuffer->page_height;
        // Per-character horizontal advance (≈ font size for CJK), used to limit
        // column height based on actual character count rather than cluster advances.
        int avg_char_advance = 0;
        for (int k = 0; k < fmt->m_length; k++) {
            if (fmt->m_srcs[k] && !(fmt->m_srcs[k]->flags & LTEXT_SRC_IS_OBJECT)) {
                if (fmt->m_srcs[k]->t.font)
                    avg_char_advance = ((LVFont*)fmt->m_srcs[k]->t.font)->getSize();
                break;
            }
        }
        if (avg_char_advance <= 0)
            avg_char_advance = fmt->m_pbuffer->strut_height;

        // split paragraph into lines, export lines
        int pos = 0;

        bool is_css_first_line = fmt->m_srcs[0] ? (fmt->m_srcs[0]->flags & LTEXT_IS_FIRST_LINE_CLONE) : false;

        #if (USE_LIBUNIBREAK!=1)
        int upSkipPos = -1;
        #endif

        while ( pos<fmt->m_length ) { // each loop makes a line (column)
            // y is this line indent (top offset in vertical layout).
            // In vertical-rl, this is the offset from the top of the column.
            int y;
            if (para->flags & LTEXT_LEGACY_RENDERING) {
                y = para->indent > 0 ? (pos == 0 ? para->indent : 0 ) : (pos==0 ? 0 : -para->indent);
            } else {
                y = fmt->m_indent_current;
                if ( !fmt->m_indent_first_line_done ) {
                    fmt->m_indent_first_line_done = true;
                    fmt->m_indent_current = fmt->m_indent_after_first_line;
                }
            }
            int w0 = pos>0 ? fmt->m_advance[pos-1] : 0; // measured cumulative vertical advance at start of this line
            int lastNormalWrap = -1;
            int lastDeprecatedWrap = -1;
            int lastHyphWrap = -1;
            int lastMandatoryWrap = -1;
            int cjkReduceWidth = 0;
            int firstInlineBoxPos = -1;
            // Faithful column-fill estimate, a running mirror of Draw's
            // vstate.vert_min_next_x: the rendered column depth of the chars
            // placed so far on this line.  Replaces the old "every char = one em"
            // approximation, which over-counted narrow Latin / numeral chars and
            // half-em JFM punctuation (、。「」・ etc.) and therefore broke mixed
            // CJK/Latin columns well before the column bottom (issue #17).
            VertColumnFitState fit;
            resetVerticalColumnFitState(fit);
            // Faithful rendered depth (px from column start) at the char chosen as
            // the normal wrap point.  Snapshotted from fit.used whenever a
            // position becomes lastNormalWrap, so the burasagari fit check (below)
            // can use the JFM-aware depth instead of raw m_advance, which omits
            // xkanjiskip / inter-class glue and under-counts the real column fill.
            int col_used_est_at_normal_wrap = -1;

            int maxHeight = fmt->getCurrentLineWidth();
            if ( maxHeight <= avg_char_advance ) {
                int unused_x;
                int new_y = fmt->getYWithAvailableWidth(fmt->m_line_advance,
                        avg_char_advance, fmt->m_pbuffer->strut_height, unused_x);
                fmt->fillAndMoveToY( new_y );
                maxHeight = fmt->getCurrentLineWidth();
            }
            if ( maxHeight <= 0 )
                maxHeight = fullHeight;

            // Find candidates where end of line is possible
            bool seen_non_collapsed_space = false;
            bool seen_first_rendered_char = false;
            bool first_line_sequance_end_reached = false;
            int i;
            for ( i=pos; i<fmt->m_length; i++ ) {
                if ( fmt->m_text[i]=='\n' ) {
                    lastMandatoryWrap = i;
                    break;
                }
                if ( is_css_first_line && !(fmt->m_srcs[i]->flags & LTEXT_IS_FIRST_LINE_CLONE) ) {
                    first_line_sequance_end_reached = true;
                    lastMandatoryWrap = i;
                    break;
                }
                lUInt16 flags = fmt->m_flags[i];
                if ( flags & LCHAR_IS_OBJECT ) {
                    if ( fmt->m_charindex[i] == FLOAT_CHAR_INDEX ) {
                        src_text_fragment_t * src = fmt->m_srcs[i];
                        if ( !(src->o.objflags & LTEXT_OBJECT_IS_FLOAT_DONE) ) {
                            int currentWidth = y + fit.used;
                            fmt->addFloat( src, currentWidth );
                            src->o.objflags |= LTEXT_OBJECT_IS_FLOAT_DONE;
                            maxHeight = fmt->getCurrentLineWidth();
                        }
                        if ( i==fmt->m_length-1 ) {
                            lastNormalWrap = i;
                        }
                        continue;
                    }
                    if ( fmt->m_charindex[i] == INLINEBOX_CHAR_INDEX ) {
                        if ( firstInlineBoxPos < 0 )
                            firstInlineBoxPos = i;
                        // The box's rendered column depth is added to fit.used
                        // at the accumulation point below (eff_adv branch).
                    }
                }
                if (!seen_non_collapsed_space) {
                    if (flags & LCHAR_IS_COLLAPSED_SPACE)
                        continue;
                    seen_non_collapsed_space = true;
                }
                if ( !seen_first_rendered_char ) {
                    seen_first_rendered_char = true;
                    // For vertical: images/inline-boxes that are too tall for page_height
                    // are handled by addLineVertical (Step 2).
                }
                // The fit estimate includes the spacing drawn at CJK/non-CJK boundaries.

                bool grabbedExceedingSpace = false;
                // Two-part break check:
                // 1. m_advance >= maxHeight: cumulative HarfBuzz TTB advance reached the limit.
                //    Use >= so a char whose start equals maxHeight (y0 = clip.bottom = invisible)
                //    goes to the next column instead.
                // 2. char_count_adv > maxHeight: font_size-based safety that mirrors Draw()'s
                //    effective_width = max(word->width, font_size) spacing.  When TTB advances
                //    are slightly smaller than font_size, the draw positions accumulate faster
                //    than m_advance predicts, placing glyphs past clip.bottom.
                //    IMPORTANT: only apply this safety check for CJK characters.  For Latin
                //    characters in vertical mode, m_advance is computed from HarfBuzz x_advance
                //    (fallback when vmtx is absent), so the actual advance IS m_advance.
                //    Using avg_char_advance (≈ em_size from CJK chars) for Latin chars
                //    (x_advance ≈ em/3) inflates char_count_adv by ~3×, causing premature
                //    column breaks inside Latin words (e.g. "answ|er" in "answer").
                VertColumnFitChar fit_item = getVerticalColumnFitChar(fmt, i, avg_char_advance);
                int eff_adv = addVerticalColumnFitChar(fit, fit_item);
                // Single break test: the running rendered depth (em floor, xkanjiskip and
                // JFM glue all included) has reached the column bottom.  This replaces the
                // old two-part test (m_advance for the real advance + an "every char = one
                // em" safety estimate); that estimate over-counted narrow Latin and half-em
                // punctuation, breaking mixed CJK/Latin columns early (issue #17).
                // Wrap against the drawn depth. A potential justification shrink
                // is not guaranteed at draw time and can hide the last glyph.
                if ( y + fit.used > maxHeight ) {
                    // burasagari / end-of-line punctuation hanging: if the overflowing character is
                    // a sentence-end punctuation that must not start a new column (line-start kinsoku),
                    // include it in the current column and stop here.  The glyph will draw
                    // at the column bottom with its ink in the upper portion of the em-square
                    // (where +vert places 。/、), so it remains fully visible even though the
                    // trailing blank of the em-square may be clipped at clip.bottom.
                    if ( fmt->m_hanging_punctuation && isVerticalHangingChar(fmt->m_text[i]) ) {
                        int prev_adv = fit.used - eff_adv;
                        if ( y + prev_adv <= maxHeight ) {
                            ltext_vert_hanging_layout_count++;
                            lastNormalWrap = i;  // include this char in current column
                            i++;
                            break;
                        }
                    }
                    if ( (flags & LCHAR_IS_SPACE) && (flags & LCHAR_ALLOW_WRAP_AFTER) )
                        grabbedExceedingSpace = true;
                    else if ( flags & LCHAR_IS_CJK && lastNormalWrap < i-1 ) {
                        int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                        bool does_fit = false;
                        if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                            bool can_add_space_before, can_add_space_after;
                            int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, i+1, can_add_space_before, can_add_space_after);
                            if ( wa8 != 8 ) {
                                if ( wa8 < 0 )
                                    wa8 = -wa8;
                                w = w * wa8 / 8;
                                if ( y + fmt->m_advance[i-1]-w0 + w <= maxHeight ) {
                                    does_fit = true;
                                }
                                if ( !does_fit ) {
                                    bool can_add_space_before, can_add_space_after;
                                    wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, i+2 > fmt->m_length ? fmt->m_length : i+2, can_add_space_before, can_add_space_after);
                                    if ( wa8 != 8 ) {
                                        if ( wa8 < 0 )
                                            wa8 = -wa8;
                                        w = w * wa8 / 8;
                                        if ( y + fmt->m_advance[i-1]-w0 + w <= maxHeight ) {
                                            does_fit = true;
                                        }
                                    }
                                }
                            }
                        }
                        if ( !does_fit && w <= cjkReduceWidth ) {
                            if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                                bool can_add_space_before, can_add_space_after;
                                int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, fmt->m_length, can_add_space_before, can_add_space_after);
                                if ( wa8 != 8 ) {
                                    if ( wa8 < 0 )
                                        wa8 = -wa8;
                                    w = w * wa8 / 8;
                                }
                            }
                            cjkReduceWidth -= w;
                            does_fit = true;
                        }
                        if ( !does_fit ) {
                            break;
                        }
                    }
                    else
                        break;
                }
                #if (USE_LIBUNIBREAK==1)
                if (flags & LCHAR_ALLOW_WRAP_AFTER) {
                    if (flags & LCHAR_DEPRECATED_WRAP_AFTER) {
                        lastDeprecatedWrap = i;
                    }
                    else {
                        lastNormalWrap = i;
                        col_used_est_at_normal_wrap = fit.used;
                    }
                }
                #else
                if ((flags & LCHAR_ALLOW_WRAP_AFTER) || (fmt->m_flags[i] & LCHAR_IS_CJK)) {
                    bool avoidWrap = false;
                    for (int j = i+1; j < fmt->m_length; j++) {
                        if ( fmt->m_flags[j] & LCHAR_IS_OBJECT ) {
                            if (fmt->m_charindex[j] == FLOAT_CHAR_INDEX)
                                continue;
                            else
                                break;
                        }
                        if ( !(fmt->m_flags[j] & LCHAR_ALLOW_WRAP_AFTER) ) {
                            avoidWrap = lGetCharProps(fmt->m_text[j]) & CH_PROP_AVOID_WRAP_BEFORE;
                            break;
                        }
                    }
                    if (!avoidWrap && i < fmt->m_length-1) {
                        for (int j = i-1; j >= 0; j--) {
                            if ( fmt->m_flags[j] & LCHAR_IS_OBJECT ) {
                                if (fmt->m_charindex[j] == FLOAT_CHAR_INDEX)
                                    continue;
                                else
                                    break;
                            }
                            if ( !(fmt->m_flags[j] & LCHAR_ALLOW_WRAP_AFTER) ) {
                                avoidWrap = lGetCharProps(fmt->m_text[j]) & CH_PROP_AVOID_WRAP_AFTER;
                                break;
                            }
                        }
                    }
                    if (!avoidWrap)
                        lastNormalWrap = i;
                }
                else if ( flags & LCHAR_DEPRECATED_WRAP_AFTER ) {
                    lastDeprecatedWrap = i;
                }
                #endif
                if ( i==fmt->m_length-1 ) {
                    lastNormalWrap = i;
                    col_used_est_at_normal_wrap = fit.used;
                }
                if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                    bool can_add_space_before, can_add_space_after;
                    int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, fmt->m_length, can_add_space_before, can_add_space_after);
                    if ( wa8 != 8 ) {
                        if ( wa8 < 0 ) {
                            wa8 = -wa8;
                            int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                            cjkReduceWidth += w - (w * wa8 / 8);
                        }
                    }
                }
                if (grabbedExceedingSpace)
                    break;
            }

            // Glyph at i exceeds available height, or mandatory break.
            if (i<=pos)
                i = pos + 1;
            int wordpos = i-1;

            #if (USE_LIBUNIBREAK==1)
                if ( lastNormalWrap < 0 && lastDeprecatedWrap > 0 ) {
                    lastNormalWrap = lastDeprecatedWrap;
                }
            #endif
            int normalWrapWidth = lastNormalWrap > 0 ? y + fmt->m_advance[lastNormalWrap]-w0 : 0;
            int unusedSpace = maxHeight - normalWrapWidth;
            int unusedPercent = maxHeight > 0 ? unusedSpace * 100 / maxHeight : 0;
            #if (USE_LIBUNIBREAK!=1)
                int deprecatedWrapWidth = lastDeprecatedWrap > 0 ? y + fmt->m_advance[lastDeprecatedWrap]-w0 : 0;
                if ( deprecatedWrapWidth > normalWrapWidth && unusedPercent > 3 ) {
                    lastNormalWrap = lastDeprecatedWrap;
                }
            #endif

            // Hyphenation
            tryHyphenBreak(fmt, pos, wordpos, lastNormalWrap, lastMandatoryWrap,
                           y, w0, maxHeight,
                           0,
                           unusedPercent, lastHyphWrap);

            // Decide best position to end this line
            int wrapPos = lastHyphWrap;
            if ( lastMandatoryWrap>=0 )
                wrapPos = lastMandatoryWrap;
            else {
                if ( wrapPos < lastNormalWrap )
                    wrapPos = lastNormalWrap;
                if ( wrapPos < 0 ) {
                    // Emergency break: no wrap opportunity found in the column.
                    // Before breaking mid-word, try to find the start of the
                    // current Latin word so we can push the whole word to the
                    // next column.  libunibreak marks AL×AL (e.g. inside
                    // "answer") as prohibited, leaving no lastNormalWrap within
                    // the word.  If the word starts after pos (there are chars
                    // before it in this column), breaking just before the word
                    // keeps the word intact.  If the word starts at pos
                    // (nothing precedes it), we cannot push it back and fall
                    // through to the per-character emergency break.
                    int word_start = i;
                    while ( word_start > pos ) {
                        lUInt16 prev_flags = fmt->m_flags[word_start - 1];
                        if ( (prev_flags & LCHAR_IS_CJK)
                                || (prev_flags & LCHAR_IS_SPACE)
                                || (prev_flags & LCHAR_IS_OBJECT) )
                            break;
                        word_start--;
                    }
                    if ( word_start > pos )
                        wrapPos = word_start - 1;   // break before the Latin word
                    else
                        wrapPos = i - 1;            // nothing before the word; split mid-word
                }
                #if (USE_LIBUNIBREAK!=1)
                if ( wrapPos <= upSkipPos ) {
                    wrapPos = upSkipPos+1;
                    upSkipPos = -1;
                }
                #endif
            }
            int endp = wrapPos + (lastMandatoryWrap<0 ? 1 : 0);

            // burasage-kinsoku: if the first char of the next column is a
            // trailing punctuation that cannot start a column (line-start kinsoku: 。、etc.),
            // hang it at the end of the current column rather than pushing it to the next.
            // This is standard Japanese typography: these chars may overflow the column bottom
            // by their compressed width (≤ font_size/2) rather than leaving a gap.
            #if (USE_LIBUNIBREAK==1)
            if ( lastMandatoryWrap < 0 && endp > pos && endp < fmt->m_length ) {
                lChar32 next_ch = fmt->m_text[endp];
                cjk_type_t next_type = getCJKCharType(next_ch);
                if ( next_type == cjkt_full_stop || next_type == cjkt_comma ) {
                    // Estimate how much the hung char occupies: its compressed advance
                    // is at most avg_char_advance/2 (full_stop/comma are ~50% at line end).
                    int hung_w = avg_char_advance / 2;
                    // Prefer the JFM-aware faithful depth (xkanjiskip + inter-class
                    // glue included) when wrapPos is the snapshotted normal wrap;
                    // raw m_advance omits that spacing and under-counts the fill.
                    int col_used = ( wrapPos == lastNormalWrap && col_used_est_at_normal_wrap >= 0 )
                                   ? y + col_used_est_at_normal_wrap
                                   : y + fmt->m_advance[wrapPos] - w0;
                    // col_used must be STRICTLY less than maxHeight so the hung
                    // char's slot start (y0 = col_used + clip.top) is strictly
                    // below clip.bottom = maxHeight + clip.top.  When col_used
                    // equals maxHeight the glyph would start exactly at clip.bottom
                    // and be clipped to zero pixels — include it in the next column
                    // instead (it will appear at the column top, per normal layout).
                    if ( col_used < maxHeight
                            && col_used + hung_w <= maxHeight + avg_char_advance / 2 ) {
                        wrapPos = endp;
                        endp = endp + 1;
                    }
                }
            }
            // Cascading oidashi (push-out) wrap-back: chained line-start / line-end kinsoku
            // resolution.  A single wrap-back can leave another prohibited char
            // exposed at the new boundary (e.g. 」」 at column head needs two
            // wrap-backs; 漢「 at column end leaves 「 at next column head, which
            // is then a closing-bracket-like situation if cascaded with another
            // line-start kinsoku char).  Iterate until both ends are clean or the safety
            // limit is hit — LuaTeX-ja's TeX-based optimizer handles this via
            // paragraph-level badness search; we approximate with a bounded
            // greedy cascade.
            //
            // Safety cap: at most 5 cascade iterations.  Each iteration can move
            // both the line-start and the line-end boundary back by one char, so
            // the true bound is up to 10 chars pushed back from a single column
            // break.  Cascading further would shorten the column disproportionately
            // to fix a punctuation pile-up.
            int kinsoku_cascade = 5;
            while ( kinsoku_cascade > 0 ) {
                bool moved = false;
                // line-start kinsoku wrap-back: chars JLReq prohibits at column start
                // (closing brackets, prolonged sound mark ー, 々, ヽヾゝゞ〻, small kana, etc.).
                if ( lastMandatoryWrap < 0 && endp > pos + 1 && endp < fmt->m_length ) {
                    lChar32 endp_ch = fmt->m_text[endp];
                    cjk_type_t endp_type = getCJKCharType(endp_ch);
                    // Also treat 。/、 (full_stop/comma) as line-start-prohibited here:
                    // a wrap-back that exposed a closing bracket can in turn expose a
                    // 。/、 at the new column head (e.g. …だ。」と), which the burasagari
                    // pass above no longer re-checks.  Without this the cascade exits
                    // and 。/、 starts the column, violating 行頭禁則 and reverting the
                    // burasagari hang.
                    if ( endp_type == cjkt_closing_bracket
                         || endp_type == cjkt_full_stop
                         || endp_type == cjkt_comma
                         || isVertLineStartProhibitedExt(endp_ch) ) {
                        wrapPos--;
                        endp--;
                        moved = true;
                    }
                }
                // line-end kinsoku wrap-back: opening brackets (「（【〔 etc.) must not end a column.
                if ( lastMandatoryWrap < 0 && wrapPos > pos ) {
                    if ( getCJKCharType(fmt->m_text[wrapPos]) == cjkt_opening_bracket ) {
                        wrapPos--;
                        endp--;
                        moved = true;
                    }
                }
                if ( !moved ) break;
                kinsoku_cascade--;
            }
            #endif

            // CJK punctuation handling at line boundaries (same as horizontal, per jlreq)
            #if (USE_LIBUNIBREAK!=1)
            int downSkipCount = 0;
            int upSkipCount = 0;
            if (endp > 1 && isCJKLeftPunctuation(*(fmt->m_text + endp))) {
            } else if (endp > 1 && endp < fmt->m_length - 1 && isCJKLeftPunctuation(*(fmt->m_text + endp - 1))) {
                upSkipPos = endp;
                endp--; wrapPos--;
            } else if (endp > 1 && isCJKPunctuation(*(fmt->m_text + endp))) {
                for (int epos = endp; epos<fmt->m_length; epos++, downSkipCount++) {
                   if ( !isCJKPunctuation(*(fmt->m_text + epos)) ) break;
                }
                for (int epos = endp; epos>=start; epos--, upSkipCount++) {
                   if ( !isCJKPunctuation(*(fmt->m_text + epos)) ) break;
                }
                if (downSkipCount <= upSkipCount && downSkipCount <= 2 && false ) {
                   endp += downSkipCount;
                   wrapPos += downSkipCount;
                } else if (upSkipCount <= 2) {
                   upSkipPos = endp;
                   endp -= upSkipCount;
                   wrapPos -= upSkipCount;
                }
            }
            #endif
            if (endp > fmt->m_length)
                endp = fmt->m_length;

            if ( is_css_first_line ) {
                is_css_first_line = false;
                if ( first_line_sequance_end_reached ) {
                    wrapPos = fmt->m_length-1;
                }
                else {
                    lUInt16 orig_offset = 0;
                    ldomNode * orig_node = (ldomNode *) fmt->m_srcs[wrapPos]->object;
                    if ( orig_node ) {
                        orig_node = orig_node->getCloneNodeSource();
                        orig_offset = fmt->m_charindex[wrapPos];
                    }
                    int i = wrapPos;
                    while (i < fmt->m_length && fmt->m_srcs[i]->flags & LTEXT_IS_FIRST_LINE_CLONE)
                        i++;
                    int normal_sequence_start = i;
                    bool found = false;
                    if ( orig_node ) {
                        while (i < fmt->m_length) {
                            ldomNode * node = (ldomNode *) fmt->m_srcs[i]->object;
                            if ( node == orig_node ) {
                                if ( fmt->m_charindex[i] == orig_offset ) {
                                    found = true;
                                    break;
                                }
                            }
                            i++;
                        }
                    }
                    if ( !found ) {
                        i = normal_sequence_start + wrapPos;
                    }
                    wrapPos = i;
                    if (wrapPos >= fmt->m_length)
                        wrapPos = fmt->m_length-1;
                }
            }

            bool hasInlineBoxes = firstInlineBoxPos >= 0 && firstInlineBoxPos < endp;
            addLineVertical( fmt, pos, endp, y, para, pos==0, wrapPos>=fmt->m_length-1, preFormattedOnly, isLastPara, hasInlineBoxes);
            pos = wrapPos + 1;

            #if (USE_LIBUNIBREAK==1)
            if ( fmt->m_srcs[wrapPos]->lang_cfg->duplicateRealHyphenOnNextLine() && pos > 0 && pos < fmt->m_length-1 ) {
                if ( fmt->m_text[wrapPos] == '-' || fmt->m_text[wrapPos] == UNICODE_HYPHEN ) {
                    pos--;
                    fmt->m_flags[pos] &= ~LCHAR_ALLOW_WRAP_AFTER;
                }
            }
            #endif
        }
    }

    /// handle embedded block for vertical layout
// -----------------------------------------------------------------------------
// processEmbeddedBlockVertical
// Vertical-rl sibling of processEmbeddedBlockHorizontal (lvtextfm.cpp),
// from upstream `LVFormatter::processEmbeddedBlock`.
// -----------------------------------------------------------------------------
void processEmbeddedBlockVertical( LVFormatter* fmt, int idx )
{
    // TODO: Implement proper vertical embedded block handling
    processEmbeddedBlockHorizontal( fmt, idx );
}

// =============================================================================
// initVerticalDrawSetup
//
// Extracted from LFormattedText::Draw entry.  In vertical-rl/lr mode:
//   - DrawDocument passes (actual_Y, actual_X) as (x, y) due to the Y=X
//     coordinate mapping used throughout the rendering/page-split pipeline.
//     Swap them back to screen coordinates.
//   - line_x is the right edge of the first (rightmost) column.  The
//     pipeline-x (= draw_y = doc_y + y0) encodes the horizontal document
//     offset; clip.right − x maps it to the column's screen-X position so
//     doc_y=0 lands near clip.right (rightmost column) and increasing
//     doc_y shifts columns leftward.
//   - When the buffer's draw_extra_info has a stored vert_column_clip_right,
//     use that as the anchor instead of clip.right (which may be widened
//     by content_overflow_clip for ruby annotation overhang).
// =============================================================================
int initVerticalDrawSetup(
    formatted_text_fragment_t * pbuffer, LVDrawBuf * buf,
    int & x_inout, int & y_inout, const lvRect & clip)
{
    bool is_vertical = css_wm_is_vertical(pbuffer->writing_mode);
    if ( !is_vertical )
        return x_inout;
    int tmp = x_inout;
    x_inout = y_inout;
    y_inout = tmp;
    draw_extra_info_t * dei = (draw_extra_info_t*)buf->GetDrawExtraInfo();
    int vert_anchor = (dei && dei->vert_column_clip_right)
        ? dei->vert_column_clip_right : clip.right;
    return vert_anchor - x_inout;
}

// =============================================================================
// computeVertRubyInlineBoxMetrics
//
// Extracted from measureText().  Inspects a ruby inline box's child
// structure (post-boxing: rbox1=erm_table → [rbox2_base, rbox2_annot]) to
// count base / annotation chars, find the annotation font size, and sum
// the base text's actual horizontal advances (needed for Latin-base ruby).
// Returns metrics in one struct so the caller can use them in both the
// renderBlockElement call (render_w) and the post-render advance override.
//
// Post-boxing structure:
//   inlineBox → rbox1(erm_table) → [rbox2_base(T="rbc"), rbox2_annot(T="rtc")]
// Both rbox2 elements have nodeId el_rubyBox, so we distinguish them by
// first child: rbox2_annot first child = el_rt/el_rtc → annotation row.
// =============================================================================
static bool isSyntheticRubyRtBox(ldomNode * node)
{
    return node
        && node->isElement()
        && node->getNodeId() == el_rubyBox
        && node->hasAttribute(attr_T)
        && node->getAttributeValue(attr_T) == U"rt";
}

static void collectVisibleRubyAnnotationMetrics(ldomNode * node,
        int & char_count, int & font_size)
{
    if ( !node )
        return;
    if ( node->isText() ) {
        lString32 t = node->getText();
        for ( int k = 0; k < t.length(); k++ ) {
            if ( t[k] > 0x20 )
                char_count++;
        }
        return;
    }
    if ( !node->isElement() )
        return;

    lUInt16 id = node->getNodeId();
    if ( id == el_rp || node->getRendMethod() == erm_invisible )
        return;

    if ( font_size == 0 && (id == el_rt || isSyntheticRubyRtBox(node)) ) {
        LVFontRef f = node->getFont();
        if ( !f.isNull() )
            font_size = f->getSize();
    }

    int child_count = node->getChildCount();
    for ( int i = 0; i < child_count; i++ )
        collectVisibleRubyAnnotationMetrics(node->getChildNode(i), char_count, font_size);
}

VertRubyInlineBoxMetrics computeVertRubyInlineBoxMetrics(
    formatted_text_fragment_t * pbuffer, ldomNode * node, LVFont * lastFont)
{
    VertRubyInlineBoxMetrics m;
    m.render_w = pbuffer->width;
    m.annot_depth = 0;
    m.base_horiz_advance_pre = 0;
    m.base_char_count_pre = 0;
    m.annot_char_count_pre = 0;
    m.annot_font_size_pre = 0;
    m.advance_per_char_pre = lastFont ? lastFont->getSize()
        : (pbuffer->strut_height > 0 ? pbuffer->strut_height : 1);
    m.vert_inline_box = css_wm_is_vertical(pbuffer->writing_mode);
    m.is_ruby_inline_pre = m.vert_inline_box && isRubyInlineBox(node);

    if ( m.is_ruby_inline_pre && m.advance_per_char_pre > 0 ) {
        ldomNode * rbox1_pre = node->getChildNode(0);
        int cc_pre = rbox1_pre->getChildCount();
        for ( int ci = 0; ci < cc_pre; ci++ ) {
            ldomNode * rbox2 = rbox1_pre->getChildNode(ci);
            if ( !rbox2 || !rbox2->isElement() ) continue;
            lUInt16 cid = rbox2->getNodeId();
            bool is_annot = isRubyAnnotId(cid);
            if ( !is_annot && rbox2->getChildCount() > 0 ) {
                ldomNode * fc = rbox2->getChildNode(0);
                if ( fc && fc->isElement() )
                    is_annot = isRubyAnnotId(fc->getNodeId());
            }
            if ( is_annot ) {
                collectVisibleRubyAnnotationMetrics(rbox2,
                        m.annot_char_count_pre, m.annot_font_size_pre);
            } else {
                lString32 t = rbox2->getText();
                LVFontRef base_font = rbox2->getFont();
                bool has_font = !base_font.isNull();
                for ( int k = 0; k < t.length(); k++ ) {
                    lChar32 c = t[k];
                    if ( c > 0x20 ) {
                        m.base_char_count_pre++;
                        if ( has_font )
                            m.base_horiz_advance_pre += base_font->getCharWidth(c);
                    }
                }
            }
        }
        if ( m.base_char_count_pre < 1 ) m.base_char_count_pre = 1;
        if ( m.annot_font_size_pre == 0 && m.advance_per_char_pre > 1 )
            m.annot_font_size_pre = m.advance_per_char_pre / 2;
    } else if ( m.vert_inline_box && m.advance_per_char_pre > 0 ) {
        m.base_char_count_pre = 1;
    }
    m.annot_depth = m.annot_char_count_pre * m.annot_font_size_pre;
    if ( m.is_ruby_inline_pre && m.advance_per_char_pre > 0 ) {
        int base_depth = m.base_char_count_pre * m.advance_per_char_pre;
        m.render_w = m.annot_depth > base_depth ? m.annot_depth : base_depth;
    }
    return m;
}

// =============================================================================
// applyVerticalFrmlineDimensions
//
// Extracted from addLineHorizontal.  In vertical-rl/-lr mode, both
// frmline->height and frmline->width represent the column WIDTH on screen
// (cross-column extent) for text lines.  The formatter computes them as if
// horizontal, so we override them here.  Single-image lines are different:
// the physical image width is the cross-column span, while the physical
// image height is the inline advance down the vertical column.
//
// Per JLReq, ruby annotations overhang into the inter-column gap rather
// than inflating the column.  The inter-column gap (strut − em) is wide
// enough to accommodate a half-em annotation without overlapping
// adjacent column text.
// =============================================================================
void applyVerticalFrmlineDimensions(LVFormatter * fmt, formatted_line_t * frmline)
{
    if ( !css_wm_is_vertical(fmt->m_writing_mode) )
        return;
    // Preserve the formatted inline-axis extent before frmline->width is
    // repurposed below as the physical column width. Parent vertical boxes
    // need this layout quantity for auto inline sizing; deriving it later by
    // painting into a measurement buffer is both circular and font-cache
    // dependent.
    frmline->inline_end = frmline->x;
    int col_width = fmt->m_pbuffer->strut_height;
    for ( int i=0; i<frmline->word_count; i++ ) {
        formatted_word_t * word = &frmline->words[i];
        int advance = (int)word->width;
        if ( word->flags & (LTEXT_WORD_IS_IMAGE | LTEXT_WORD_IS_INLINE_BOX) )
            advance = (int)word->o.height;
        int end = (int)frmline->x + (int)word->x + advance;
        if ( end > frmline->inline_end )
            frmline->inline_end = end;

        // Every replaced inline establishes cross-axis space. In particular,
        // a decorative image mixed with text may be wider than the strut.
        // word->width is its physical screen-X extent; o.height is its
        // inline screen-Y advance.
        if ( (word->flags & LTEXT_WORD_IS_IMAGE) && (int)word->width > col_width )
            col_width = (int)word->width;

        // A width-specified orthogonal inline similarly reserves cross-axis
        // space while remaining transparent to inline-axis wrapping.
        if ( (int)word->src_text_index >= fmt->m_pbuffer->srctextlen )
            continue;
        src_text_fragment_t * src = &fmt->m_pbuffer->srctext[word->src_text_index];
        if ( (src->flags & LTEXT_SRC_IS_OBJECT)
                && (src->o.objflags & LTEXT_OBJECT_IS_ORTHOGONAL_SPACER)
                && (int)word->o.height > col_width )
            col_width = word->o.height;
    }
    if ( frmline->word_count == 1 ) {
        formatted_word_t * w0 = &frmline->words[0];
        if ( w0->flags & LTEXT_WORD_IS_IMAGE ) {
            frmline->height = col_width;
            frmline->width = getVerticalImageInlineAdvance(w0);
            frmline->flags |= LTEXT_LINE_SPLIT_AVOID_BEFORE | LTEXT_LINE_SPLIT_AVOID_AFTER;
            return;
        }
        else if ( w0->flags & LTEXT_WORD_IS_INLINE_BOX ) {
            // A display:inline-block element that wraps a wide image (e.g. EPUBs
            // that box a full-width illustration in an inline-block, as Momo does
            // with img.pagebox/leftbox) reaches Draw as an inline box, not a bare
            // image, so the LTEXT_WORD_IS_IMAGE branch above misses it.  Without
            // inflating the column to the box's block-direction width, the column
            // only reserves one strut and the illustration is drawn on top of the
            // following text columns.  Mirror the image branch.
            // Ruby annotation boxes are EXCLUDED: per JLReq they overhang into the
            // inter-column gap and must not widen the column (their word->width
            // carries the screen-Y depth, not a block-direction width).
            bool is_ruby = false;
            if ( (int)w0->src_text_index < fmt->m_pbuffer->srctextlen ) {
                src_text_fragment_t * sx = &fmt->m_pbuffer->srctext[w0->src_text_index];
                is_ruby = sx->object && isRubyInlineBox((ldomNode*)sx->object);
            }
            if ( !is_ruby && (int)w0->width > col_width )
                col_width = (int)w0->width;
        }
    }
    frmline->height = col_width;
    frmline->width = col_width;
}

// Walks up the DOM from a source text fragment to detect whether the
// fragment lives inside a ruby annotation cell (rt / rtc / rp).  Used by
// applyVerticalNNRubyCenterDistribution to gate per-char annotation
// distribution: applies only when this frmline IS a ruby annotation
// cell, not when it's some other narrow centred block (e.g. headings).
bool isVerticalRubyInnerLine(LVFormatter * fmt, formatted_line_t * frmline)
{
    if ( frmline->word_count < 1 ) return false;
    formatted_word_t * w0 = &frmline->words[0];
    if ( (int)w0->src_text_index >= fmt->m_pbuffer->srctextlen ) return false;
    src_text_fragment_t * src = &fmt->m_pbuffer->srctext[w0->src_text_index];
    if ( src->flags & LTEXT_SRC_IS_OBJECT ) return false;
    ldomNode * n = (ldomNode *)src->object;
    if ( !n ) return false;
    // Limit traversal depth so a deeply nested fragment doesn't burn time.
    for ( int depth = 0; depth < 8 && n; depth++ ) {
        if ( n->isElement() ) {
            lUInt16 id = n->getNodeId();
            if ( id == el_rubyBox || id == el_rbc || id == el_rb
                    || id == el_rtc || id == el_rt || id == el_rp )
                return true;
        }
        n = n->getParentNode();
    }
    return false;
}

bool isVerticalRubyAnnotationLine(LVFormatter * fmt, formatted_line_t * frmline)
{
    if ( frmline->word_count < 1 ) return false;
    formatted_word_t * w0 = &frmline->words[0];
    if ( (int)w0->src_text_index >= fmt->m_pbuffer->srctextlen ) return false;
    src_text_fragment_t * src = &fmt->m_pbuffer->srctext[w0->src_text_index];
    if ( src->flags & LTEXT_SRC_IS_OBJECT ) return false;
    ldomNode * n = (ldomNode *)src->object;
    if ( !n ) return false;
    // Limit traversal depth so a deeply nested fragment doesn't burn time.
    for ( int depth = 0; depth < 8 && n; depth++ ) {
        if ( n->isElement() && isRubyAnnotId(n->getNodeId()) )
            return true;
        n = n->getParentNode();
    }
    return false;
}

bool prepareVerticalRubyLineAlignment(
    LVFormatter * fmt, formatted_line_t * frmline, int & width_inout)
{
    if ( !css_wm_is_vertical(fmt->m_pbuffer->writing_mode)
            || !isVerticalRubyInnerLine(fmt, frmline)
            || fmt->m_pbuffer->width <= 0
            || fmt->m_pbuffer->width >= width_inout )
        return false;

    // Inner ruby cells align against their declared slot rather than the page
    // height returned by getAvailableWidthAtY() in vertical mode.  HarfBuzz
    // rounding can make the rendered line a pixel wider than that estimate;
    // using the real width then preserves symmetric annotation overhang.
    width_inout = fmt->m_pbuffer->width;
    if ( (int)frmline->width > width_inout )
        width_inout = (int)frmline->width;
    return true;
}

void applyVerticalRubyCenterAlignment(
    LVFormatter * fmt, formatted_line_t * frmline,
    int extra_width)
{
    // N:N mono-ruby uses per-character distribution.  Other ruby forms are
    // centred as one block using round-half-up so base and annotation cells
    // land on the same integer-pixel centre.
    if ( applyVerticalNNRubyCenterDistribution(
            frmline, fmt->m_pbuffer->width, extra_width, fmt) )
        return;

    // If annotation is wider than its base, avoid a negative start shift: an
    // overflow after the ruby group is less disruptive than bleeding into the
    // preceding character.
    int center_shift = (extra_width + 1) / 2;
    if ( center_shift < 0 )
        center_shift = 0;
    frmline->x += center_shift;
}

// =============================================================================
// applyVerticalNNRubyCenterDistribution
//
// Per-char distribution for MONO-RUBY annotation cells in
// vertical-rl: when the annotation char count equals the base char count
// (N:N) and the font ratio is 2:1, distribute the annotation chars so each
// sits centred over its corresponding base char.
//
// The `sub_slot == 2 * char_w` test is the mono-ruby signature:
//   slot_width = N_base × base_em  (annotation cell spans the base width)
//   sub_slot   = slot_width / N_annot
//   char_w     = annot_em = base_em / 2  (2:1 ruby font ratio)
//   sub_slot == 2 × char_w  ⇔  slot_width == N_annot × base_em
//                           ⇔  N_annot == N_base   (mono-ruby)
//
// For group-ruby (single <ruby>base<rt>annot</rt></ruby> where the
// annotation count differs from the base, e.g. 蝶《ちょう》 = 1 base, 3
// annot), this test is FALSE, so the function returns false and the caller
// centres the annotation as a block over the base — which is the correct
// JLReq group-ruby placement.  Distributing those chars per-sub-slot
// instead would spread them out and break the centred appearance.
//
// A DOM check (isInsideRubyAnnotation) additionally guards against any
// non-ruby narrow centred content that happens to match the metric
// signature (e.g. a short heading whose chars are coincidentally half the
// container width).
//
// Returns true when mono-ruby distribution was applied; caller falls back
// to block centering otherwise (group-ruby / N:M).
// =============================================================================
bool applyVerticalNNRubyCenterDistribution(
    formatted_line_t * frmline, int slot_width, int extra_width,
    LVFormatter * fmt)
{
    if ( (int)frmline->word_count < 2 || extra_width <= 0 || slot_width <= 0 )
        return false;
    int N = (int)frmline->word_count;
    if ( slot_width % N != 0 )
        return false;
    int sub_slot = slot_width / N;
    int char_w = (int)frmline->words[0].width;
    if ( char_w <= 0 )
        return false;
    for ( int wi = 1; wi < N; wi++ ) {
        if ( (int)frmline->words[wi].width != char_w )
            return false;
    }
    // Mono-ruby signature: sub_slot is exactly twice the annotation char
    // width (2:1 ruby font ratio, N_annot == N_base).  Group-ruby (N:M)
    // fails this and falls through to block centering — the correct
    // JLReq placement (annotation block centred over the base block).
    if ( sub_slot != 2 * char_w )
        return false;
    // Belt-and-braces: also require the fragment to actually be inside
    // <rt>/<rtc>/<rp>, so non-ruby content matching the metric signature
    // (e.g. a short centred heading) is never per-char distributed.
    if ( !isVerticalRubyAnnotationLine(fmt, frmline) )
        return false;
    int next_x = 0;
    for ( int wi = 0; wi < N; wi++ ) {
        int sub_start = (wi * slot_width) / N;
        int sub_end   = ((wi + 1) * slot_width) / N;
        int sub_mid   = (sub_start + sub_end) / 2;
        int w_start   = sub_mid - char_w / 2;
        if ( w_start < next_x ) w_start = next_x;
        frmline->words[wi].x = w_start;
        next_x = w_start + char_w;
    }
    frmline->width = next_x;
    return true;
}

// =============================================================================
// drawBorderVertical
//
// Fork-only helper for vertical-rl mode.  CSS physical left/right borders
// map to thin stripes at the column's right (bdidx=1) or left (bdidx=3)
// screen-X edge, spanning y_start..y_end in screen-Y (= the element's
// inline extent).  Delegates to drawBorder() (in lvtextfm.cpp) for the
// actual draw so color/style/dot/interval resolution stays single-sourced
// — any upstream change to drawBorder() is picked up automatically.
// =============================================================================
static void drawBorderVertical(LVDrawBuf * buf, int line_x, int col_width,
                               int y_start, int y_end,
                               ldomNode * borderNode, int bdidx) {
    int bw = measureBorder(borderNode, bdidx);
    int x0, x1;
    if ( bdidx == 1 ) {          // right border → right edge of column
        x0 = line_x - bw;
        x1 = line_x;
    } else if ( bdidx == 3 ) {   // left border → left edge of column
        x0 = line_x - col_width;
        x1 = line_x - col_width + bw;
    } else {
        assert(0); // vertical helper only handles left/right
        return;
    }
    drawBorder(buf, x0, x1, y_start, y_end - y_start, borderNode, bdidx);
    if ( bdidx == 1 && bw > 0 && y_end > y_start ) {
        css_style_ref_t style = borderNode->getStyle();
        css_length_t border_color = style->border_color[bdidx];
        lUInt32 color = border_color.type == css_val_color
                ? border_color.value : style->color.value;
        color = buf->getInvertColors() ? invertNonGrayscaleColor(color) : color;
        if ( !IS_COLOR_FULLY_TRANSPARENT(color) )
            ltext_record_vert_border_trace(borderNode->getDataIndex(),
                    x0, y_start, x1, y_end);
    }
}

// CSS text decorations are established by an inline box and propagated
// through its descendants. A child span with a different font-size therefore
// changes glyph geometry, but not the decoration line's cross-axis position.
// Resolve that position from the establishing box so underline painting and
// a coincident inline border use the same coordinate model.
bool getVerticalDecorationMetrics(formatted_text_fragment_t * pbuffer,
                                  formatted_line_t * frmline,
                                  ldomNode * node, lUInt32 decoration_flags,
                                  int line_x,
                                  VerticalDecorationMetrics & metrics)
{
    if ( !node )
        return false;
    if ( node->isEffectiveText() )
        node = node->getParentNode();
    ldomNode * decoration_node = NULL;
    for ( ldomNode * current = node; current; current = current->getParentNode() ) {
        css_text_decoration_t decoration = current->getStyle()->text_decoration;
        if ( ((decoration_flags & LTEXT_TD_UNDERLINE)
                    && (decoration & (css_td_underline | css_td_blink)))
                || ((decoration_flags & LTEXT_TD_OVERLINE) && (decoration & css_td_overline))
                || ((decoration_flags & LTEXT_TD_LINE_THROUGH)
                    && (decoration & css_td_line_through)) ) {
            // The computed style carried by descendant inline nodes includes
            // propagated text decorations. Keep walking through ancestors so
            // we retain the box that established the decoration instead of
            // mistaking a differently-sized descendant for a new owner.
            decoration_node = current;
        }
    }
    if ( !decoration_node || decoration_node->getFont().isNull() )
        return false;
    metrics.thickness = decoration_node->getFont()->getUnderlineThickness();
    metrics.inline_end_border_width = (decoration_flags & LTEXT_TD_UNDERLINE)
            ? measureBorder(decoration_node, 1) : 0;
    metrics.owner = decoration_node;
    // Resolve the largest font in the establishing inline box. A TOC link
    // commonly switches between 90% and 140% descendants: the left sideline
    // must follow the largest shared character frame, rather than either the
    // current run's x (which kinks) or the full line box (which is too far
    // away when font height exceeds em size).
    int reference_em = decoration_node->getFont()->getSize();
    for ( int i = 0; i < pbuffer->srctextlen; i++ ) {
        ldomNode * candidate = (ldomNode *)pbuffer->srctext[i].object;
        if ( candidate && candidate->isEffectiveText() )
            candidate = candidate->getParentNode();
        for ( ldomNode * current = candidate; current;
                current = current->getParentNode() ) {
            if ( current == decoration_node ) {
                if ( !candidate->getFont().isNull()
                        && candidate->getFont()->getSize() > reference_em )
                    reference_em = candidate->getFont()->getSize();
                break;
            }
        }
    }
    int column_width = (int)frmline->height;
    if ( column_width < pbuffer->strut_height )
        column_width = pbuffer->strut_height;
    if ( column_width < reference_em )
        column_width = reference_em;
    metrics.inline_start = line_x - (column_width + reference_em) / 2;
    // Decorations hug the character frame, not the outer line-height box.
    // A 24px glyph in a 36px vertical line is centred with 6px on either
    // side; its right sideline belongs at line_x - 6. Ruby bases use their
    // own tight line box, so this gives identical coordinates with or
    // without ruby instead of creating a 6px step at every ruby boundary.
    metrics.inline_end = line_x - (column_width - reference_em) / 2;
    return true;
}

// =============================================================================
// drawVerticalPadBorders
//
// Extracted from LFormattedText::Draw's PAD border branch.  In vertical-rl
// mode, x and y are swapped: x+frmline->x+word->x is a screen-Y value
// (inline position), not screen-X.  The border spans the element's inline
// (screen-Y) extent; we scan frmline->words[] for the paired PAD to find
// the other boundary.
// =============================================================================
void drawVerticalPadBorders(
    LVDrawBuf * buf, formatted_text_fragment_t * pbuffer,
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, int j,
    int y, int line_x, ldomNode * node,
    bool is_right_pad, bool is_mirrored)
{
    if ( is_right_pad != is_mirrored ) {
        // Right PAD marks element end; scan backward for the left PAD.
        // Use y+frmline->x (not y+x+frmline->x): x = draw_y0 is block-direction
        // (screen-X), adding it to a screen-Y value gives a wrong coordinate.
        int y_end = y + (int)frmline->x + (int)word->x;
        int y_start = y + (int)frmline->x; // fallback: frmline inline start
        for ( int k = j - 1; k >= 0; k-- ) {
            formatted_word_t * pw = &frmline->words[k];
            if ( (pw->flags & LTEXT_WORD_IS_PAD)
                  && (pbuffer->srctext[pw->src_text_index].object == srcline->object) ) {
                y_start = y + (int)frmline->x + (int)pw->x + (int)pw->width;
                break;
            }
        }
        VerticalDecorationMetrics metrics = {
            line_x - (int)frmline->height, line_x, 0, 0, NULL
        };
        getVerticalDecorationMetrics(pbuffer, frmline, node,
                LTEXT_TD_UNDERLINE, line_x, metrics);
        drawBorderVertical(buf, metrics.inline_end, (int)frmline->height,
                y_start, y_end, node, 1);
    }
    else {
        // Left PAD marks element start; scan forward for the right PAD.
        int y_start = y + (int)frmline->x + (int)word->x + (int)word->width;
        int y_end = -1;
        for ( int k = j + 1; k < frmline->word_count; k++ ) {
            formatted_word_t * pw = &frmline->words[k];
            if ( (pw->flags & LTEXT_WORD_IS_PAD)
                  && (pbuffer->srctext[pw->src_text_index].object == srcline->object) ) {
                y_end = y + (int)frmline->x + (int)pw->x;
                break;
            }
        }
        if ( y_end >= y_start )
            drawBorderVertical(buf, line_x, (int)frmline->height, y_start, y_end, node, 3);
    }
}

// =============================================================================
// applyVerticalLatinPostDraw
//
// For Latin-in-vertical words: after DrawTextString returns, advance
// vert_min_next_x by word->width.  We deliberately do NOT use the _adv
// returned by DrawTextString: measureText shapes the full line as TTB,
// DrawTextString shapes only the single word as LTR, and the resulting
// per-glyph codepoints can differ (e.g. vert-form substitution), producing
// h_advance values that disagree by ±1 px.  Using word->width keeps DRAW's
// vert_min_next_x in lockstep with LAYOUT's vert_layout_min_x mirror —
// without that lockstep, the highlight rect (positioned by word->x set in
// the post-pass) drifts away from the actual rendered glyph.
// =============================================================================
void applyVerticalLatinPostDraw(
    int _adv, int word_width, VerticalDrawState & state,
    const lvRect & clip, int y)
{
    int latin_adv = word_width;
    if ( latin_adv <= 0 )
        latin_adv = _adv;  // fallback only if word->width is unset
    state.vert_min_next_x += latin_adv;
    if ( state.vert_min_next_x > clip.bottom - y )
        state.vert_min_next_x = clip.bottom - y;
    state.vert_prev_effective_width = latin_adv;
    // Mark "prev was non-CJK" so the next CJK word inserts the Phase 5 /
    // xkanjiskip gap between Latin and CJK.  Reset CJK-class tracker since
    // the chain broke.
    state.vert_prev_was_non_cjk_word = true;
    state.vert_prev_cjk_class = -1;
}

// =============================================================================
// drawVerticalEmphasisMarks
//
// Draws kenten / bouten (text-emphasis marks, JLReq §3.3.10) in vertical-rl
// mode.  Each char in the word gets one mark in the inter-column space
// to the right of the column (= x_mark = line_x).  CSS text-emphasis-style
// values are mapped to Unicode characters per the CSS3 Text Decoration spec.
// =============================================================================
void drawVerticalEmphasisMarks(
    LVDrawBuf * buf, formatted_line_t * frmline,
    src_text_fragment_t * srcline, formatted_word_t * word,
    LVFont * font, int y, int line_x, const lvRect & clip,
    const VerticalDrawState & state)
{
    if ( !(srcline->flags & LTEXT_HAS_EXTRA) )
        return;
    int em_style = getLTextExtraProperty(srcline, LTEXT_EXTRA_CSS_TEXT_EMPHASIS);
    if ( em_style <= 0 || em_style == css_tes_inherit )
        return; // none, or unresolved inherit (treated as none)
    // Map style enum to Unicode mark character, per CSS Text Decoration L3
    // (§ text-emphasis-style): dot = • / ◦, circle = ● / ○, double-circle =
    // ◉ / ◎, triangle = ▲ / △, sesame = ﹅ / ﹆.
    lChar32 mark;
    switch ( em_style ) {
        case css_tes_open_dot:      mark = 0x25E6; break; // ◦
        case css_tes_open_circle:   mark = 0x25CB; break; // ○
        case css_tes_filled_sesame: mark = 0xFE45; break; // ﹅
        case css_tes_open_sesame:   mark = 0xFE46; break; // ﹆
        case css_tes_filled_dc:     mark = 0x25C9; break; // ◉
        case css_tes_open_dc:       mark = 0x25CE; break; // ◎
        case css_tes_filled_tri:    mark = 0x25B2; break; // ▲
        case css_tes_open_tri:      mark = 0x25B3; break; // △
        default:                    mark = 0x2022; break; // • filled dot (initial)
    }
    int em = font->getSize();
    // In vertical-rl, marks go to the right of each char (= "before" dir).
    int x_mark = line_x;
    int char_count = word->t.len;
    // Per-char column advance: half-em JFM punctuation (、。「」・ etc.) occupies
    // em/2, not a full em.  Assuming a uniform em grid drifts the marks off
    // their glyphs once the word contains compacted punctuation.  Sum the
    // actual JFM slot widths so each mark sits over its own char.
    int total_adv = 0;
    for ( int mc = 0; mc < char_count; mc++ ) {
        lChar32 ch = srcline->t.text[word->t.start + mc];
        total_adv += em * getJLReqVertLayout(getJLReqVertClass(ch)).width_halves / 2;
    }
    int clamped_x = word->x;
    if ( clamped_x < state.vert_min_next_x - total_adv )
        clamped_x = state.vert_min_next_x - total_adv;
    int adv = 0;
    for ( int mc = 0; mc < char_count; mc++ ) {
        lChar32 ch = srcline->t.text[word->t.start + mc];
        int slot = em * getJLReqVertLayout(getJLReqVertClass(ch)).width_halves / 2;
        // Center the mark within this char's slot (so a half-em punctuation's
        // mark is not nudged toward the next char).
        int y_mark = y + frmline->x + clamped_x + adv + (slot - em) / 2;
        // Draw whenever the slot's top is inside the clip band; a char straddling
        // clip.bottom still gets its mark (the glyph drawer clips it) rather than
        // being silently skipped.
        if ( y_mark + em > clip.top && y_mark < clip.bottom ) {
            font->DrawTextString(buf, x_mark, y_mark, &mark, 1, '?',
                NULL, false, NULL, 0, 0, em, 0, 0, 0);
        }
        adv += slot;
    }
}

// =============================================================================
// applyVerticalImageDraw
//
// Extracted from LFormattedText::Draw's image-word branch.  Right-aligns
// the image to the column right edge (line_x) and clamps left to 0.
// =============================================================================
void applyVerticalImageDraw(
    formatted_line_t * frmline, formatted_word_t * word,
    int y, int line_x, int column_clip_right, const lvRect & clip,
    VerticalDrawState & state,
    int & x0_out, int & y0_out)
{
    // In vertical-rl after the x/y swap at Draw() entry:
    //   line_x = clip.right − x = right edge of current column group.
    // word->width  = image physical width  = block-direction (screen-X) extent.
    // word->o.height = image physical height = inline-direction (screen-Y) extent.
    // Center the image across the column the same way plain glyphs are centered
    // (x0 = line_x − strut + (strut − em)/2).  Right-aligning to line_x instead
    // pushed a narrow inline image (e.g. a 1em ● gaiji bullet, narrower than the
    // strut) toward the column's right edge, off the column axis and visually
    // colliding with the text.  Clamp left to 0.
    int strut = (int)frmline->height;
    int img_w = (int)word->width;
    if ( img_w > strut ) {
        int underreserve = img_w - strut;
        ltext_vert_image_cross_underreserve_count++;
        if ( underreserve > ltext_vert_image_cross_underreserve_max_px )
            ltext_vert_image_cross_underreserve_max_px = underreserve;
    }
    int line_right = line_x;
    if ( column_clip_right > 0 && line_right > column_clip_right )
        line_right = column_clip_right;
    x0_out = line_right - strut + (strut - img_w) / 2;
    int line_left = line_right - strut;
    if ( img_w <= strut ) {
        if ( x0_out < line_left )
            x0_out = line_left;
        if ( x0_out + img_w > line_right )
            x0_out = line_right - img_w;
    }
    else if ( x0_out < 0 ) {
        x0_out = 0;
    }
    // Clamp the in-column position to the running tracker like the other branches
    // (plain CJK / Latin / inline-box) do, so an image after a wider preceding
    // glyph is not drawn back on top of it.
    int clamped_x = vertClampForward((int)word->x, state.vert_min_next_x);
    int expected_y = y + (int)frmline->x + clamped_x;
    y0_out = expected_y;
    ltext_vert_image_draw_count++;
    int drift = y0_out - expected_y;
    if ( drift < 0 )
        drift = -drift;
    if ( drift > 0 ) {
        ltext_vert_image_draw_drift_count++;
        if ( drift > ltext_vert_image_draw_drift_max_px )
            ltext_vert_image_draw_drift_max_px = drift;
    }
    // A line containing only an image is the shape used by full-page
    // illustrations. Keep the same numeric evidence as KO_DEBUG_VERT_BG so a
    // regression test can verify clipping and centering without comparing
    // screenshots or parsing human-readable stderr output.
    if ( frmline->word_count == 1 ) {
        ltext_vert_single_image_placement_sample_count++;
        int overflow = clip.left - x0_out;
        if ( overflow < 0 )
            overflow = 0;
        int edge_overflow = x0_out + img_w - clip.right;
        if ( edge_overflow > overflow )
            overflow = edge_overflow;
        edge_overflow = clip.top - y0_out;
        if ( edge_overflow > overflow )
            overflow = edge_overflow;
        edge_overflow = y0_out + (int)word->o.height - clip.bottom;
        if ( edge_overflow > overflow )
            overflow = edge_overflow;
        if ( overflow > 0 ) {
            ltext_vert_single_image_clip_overflow_count++;
            if ( overflow > ltext_vert_single_image_clip_overflow_max_px )
                ltext_vert_single_image_clip_overflow_max_px = overflow;
        }
        int leading_gap = y0_out - clip.top;
        int trailing_gap = clip.bottom - (y0_out + (int)word->o.height);
        int center_error = leading_gap - trailing_gap;
        if ( center_error < 0 )
            center_error = -center_error;
        if ( center_error > ltext_vert_single_image_center_error_max_px )
            ltext_vert_single_image_center_error_max_px = center_error;
    }
    // Advance the per-column tracker past the image.  Plain CJK words derive
    // their draw position SOLELY from state.vert_min_next_x (see
    // applyVerticalWordDraw: clamped_x = state.vert_min_next_x), so a word
    // that does not advance it leaves the following char clamped to the
    // pre-image value — drawing it on top of the image (the "● overlaps 「 by
    // 1em" gaiji-bullet bug).  Mirror the LAYOUT side
    // (alignLineHorizontalVerticalPostPass): the image's effective advance is
    // word->width, and word->x already carries the CJK→non-CJK xkanjiskip the
    // layout inserted before it.  The image is non-CJK, so the next CJK char
    // must get its own xkanjiskip and no inter-class glue from the image.
    state.vert_min_next_x = clamped_x + getVerticalImageInlineAdvance(word);
    state.vert_prev_plain_y0 = y0_out;
    state.vert_prev_effective_width = getVerticalImageInlineAdvance(word);
    state.vert_prev_was_non_cjk_word = true;
    state.vert_prev_cjk_class = -1;
}
bool centerVerticalImageOnlyFragment(
    ldomNode * node, formatted_line_t * frmline,
    draw_extra_info_t * draw_extra_info, int image_width, int & x_inout)
{
    if ( !node || !node->isImage() || !frmline || frmline->word_count != 1
            || !draw_extra_info || !draw_extra_info->is_page_mode )
        return false;
    lvRect page = draw_extra_info->content_overflow_clip;
    int page_width = page.right - page.left;
    if ( page_width <= 0 || image_width > page_width )
        return false;
    ldomNode * image = node;
    ldomNode * parent = image->getParentNode();
    while ( parent && parent->getNodeId() != el_body ) {
        bool found_image = false;
        for ( int i = 0; i < parent->getChildCount(); i++ ) {
            ldomNode * child = parent->getChildNode(i);
            if ( child->isWhitespaceText() )
                continue;
            if ( child != image || found_image )
                return false;
            found_image = true;
        }
        if ( !found_image )
            return false;
        image = parent;
        parent = parent->getParentNode();
    }
    if ( !parent || !parent->getParentNode()
            || parent->getParentNode()->getNodeId() != el_DocFragment )
        return false;
    bool found_image = false;
    for ( int i = 0; i < parent->getChildCount(); i++ ) {
        ldomNode * child = parent->getChildNode(i);
        if ( child->isWhitespaceText() )
            continue;
        if ( child != image || found_image )
            return false;
        found_image = true;
    }
    if ( !found_image )
        return false;
    x_inout = page.left + (page_width - image_width) / 2;
    return true;
}

// =============================================================================
// applyVerticalInlineBoxDraw
//
// Extracted from LFormattedText::Draw's inline-box branch.  Sets the four
// coordinate values needed by the inner DrawDocument call, applies the
// vert_min_next_x clamping, updates per-column state, and increments fork
// diagnostic counters (ib_layout_gap, bleed) when the layout-side placement
// disagreed with the running Draw-side tracker.
//
// Coordinate semantics in vertical-rl: after the x/y swap at Draw() entry,
// `x` represents the column advance (doc_y) and `y` represents the
// screen-Y origin of the block.  For a ruby box at column offset node_y
// from the parent right edge:
//   line_x_child = parent_line_x − node_y = (clip.right − x) − node_y
//   ⇒ y0_inline = x + node_y           (so inner Draw's line_x derives correctly)
//   x0_inline   = y + node_x           (carries the screen-Y position)
//   doc_x_ib    = −node_x              (cancel inline_box.getX() in inner DrawDocument)
//   doc_y_ib    = −node_y              (cancel inline_box.getY())
// =============================================================================
void applyVerticalInlineBoxDraw(
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, int node_x, int node_y,
    int x, int y,
    VerticalDrawState & state,
    int & x0_out, int & y0_out, int & doc_x_ib_out, int & doc_y_ib_out)
{
    // Clamp the inline box start to vert_min_next_x so it never starts
    // before the preceding character's visual end.  ib_word_x (TTB-advance-
    // based) is kept as a lower bound: if it exceeds vert_min_next_x (which
    // can happen when the inline box has its own layout position further
    // into the column), use it.
    int ib_word_x = node_x - frmline->x;
    int clamped_ib_x = vertClampForward(ib_word_x, state.vert_min_next_x);
    int clamp_delta = clamped_ib_x - ib_word_x;  // ≥ 0
    // Diagnostic: ib_word_x > vert_min_next_x means the layout placed this
    // box further than the draw tracker expected — a gap above the box.
    // Should be 0 when vert_layout_min_x correctly mirrors vert_min_next_x.
    if ( ib_word_x > state.vert_min_next_x ) {
        int gap = ib_word_x - state.vert_min_next_x;
        ltext_vert_ib_layout_gap_total += gap;
        if ( gap > ltext_vert_ib_layout_gap_max )
            ltext_vert_ib_layout_gap_max = gap;
    }
    // P14 overlap diagnostic: save the OLD vert_min_next_x (= end of the
    // preceding character) BEFORE updating it.  After update,
    // vert_min_next_x = end of THIS inline box.
    int preceding_end = y + (int)frmline->x + state.vert_min_next_x;
    x0_out = y + node_x + clamp_delta;  // draw_x_rb = y + frmline->x + clamped_ib_x
    // Use actual vertical depth (render_w from vert_inline_box_depth) so the next
    // character starts after the ruby group's visual end, preventing
    // character-overlap.  Fall back to word->width if vert_inline_box_depth was
    // not set (non-ruby inline boxes, horizontal mode).
    int ib_actual_depth = getVerticalInlineBoxDepth(srcline, (int)word->width);
    state.vert_min_next_x = clamped_ib_x + ib_actual_depth;
    state.vert_prev_plain_y0 = y + (int)frmline->x + clamped_ib_x;
    state.vert_prev_effective_width = ib_actual_depth;
    // Ruby / inline box ends the JFM class chain; the next CJK word
    // starts with no class predecessor.
    state.vert_prev_cjk_class = -1;
    state.vert_prev_was_non_cjk_word = false;
    doc_x_ib_out = 0 - node_x;       // anchor to original node_x
    // y0 = x + node_y places the inner Draw at the correct column offset
    // so the ruby base column lands at clip.right − node_y − annot_width.
    // doc_y_ib = −node_y cancels inline_box.getY()=node_y in DrawDocument
    // so children see doc_y = 0 at this level.
    y0_out = x + node_y;
    doc_y_ib_out = 0 - node_y;
    // draw_x_inner = x0 + doc_x_ib + node_x = y + node_x + clamp_delta
    // (DrawDocument accumulates doc_x += inline_box.getX() = node_x).
    // If draw_x_inner < preceding_end, ruby overlaps the char above.
    int draw_x_inner = x0_out + doc_x_ib_out + node_x;
    if ( draw_x_inner < preceding_end ) {
        ltext_vert_bleed_count++;
        int overlap_px = preceding_end - draw_x_inner;
        if ( overlap_px > ltext_vert_bleed_max_px )
            ltext_vert_bleed_max_px = overlap_px;
    }
}

// =============================================================================
// applyVerticalWordDraw
//
// Extracted from LFormattedText::Draw's per-word loop (~190 lines of
// fork-only code).  Handles all three vertical-mode positioning paths
// (TCY, Latin-rotated, plain CJK) plus the drawFlags setup that selects
// which DrawTextString path the caller will invoke.  Maintains the
// per-frmline VerticalDrawState (vert_min_next_x, vert_prev_*) which is
// also touched by inline-box draw and post-DrawTextString Latin advance
// in the caller.
//
// Vertical positioning is fundamentally different from upstream's
// horizontal `x0 = x + frmline->x + word->x; y0 = line_y + ...`:
//   - vertical-rl uses line_x (column right edge) and frmline->height
//     as the column slot, with x0 = line_x − frmline->height (column left).
//   - y in the column is driven by vert_min_next_x (a running tracker)
//     instead of word->x, because Phase 5 inter-class glue + xkanjiskip
//     shifts are applied between chars without being baked into word->x.
//
// Caller responsibilities (still in lvtextfm.cpp Draw):
//   - drawFlags &= LTEXT_TD_MASK + WORD_FLAGS_TO_FNT_FLAGS before call
//   - check is_vertical && !LTEXT_MATH_TRANSFORM before call
//   - after DrawTextString, update state.vert_min_next_x for Latin words
//     (using word->width — see comment at the call site)
// =============================================================================
void applyVerticalWordDraw(
    formatted_text_fragment_t * pbuffer,
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, LVFont * font,
    int y, int line_x, const lvRect & clip, bool line_has_image,
    lUInt32 & drawFlags,
    VerticalDrawState & state,
    int & x0_out, int & y0_out, bool & vert_skip_draw_out,
    bool & word_is_latin_in_vertical_out, bool & word_is_vert_mark_out,
    bool & word_is_exact_hanging_out)
{
    vert_skip_draw_out = false;
    word_is_exact_hanging_out = false;

    // Classify the word: vertical mark, Latin-in-vertical, TCY, or plain CJK.
    // Japanese horizontal marks (―, —, …, 〜, etc.) are below U+2E80 and would
    // default to the Latin-rotated path; route them through the CJK +vert path.
    bool word_is_vert_mark =
        !(word->flags & LTEXT_WORD_IS_TCY)
        && isWordAllVerticalUprightChars(srcline->t.text + word->t.start, (int)word->t.len);
    bool word_is_latin_in_vertical =
           !(word->flags & LTEXT_WORD_IS_TCY)
        && !(word->flags & LTEXT_WORD_IS_CJK)
        && !(word->flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK)
        && !(word->flags & LTEXT_WORD_IS_IMAGE)
        && !(word->flags & LTEXT_WORD_IS_INLINE_BOX)
        && !word_is_vert_mark;
    word_is_latin_in_vertical_out = word_is_latin_in_vertical;
    word_is_vert_mark_out         = word_is_vert_mark;

    // Set vertical drawFlags (mutually exclusive after the first if).
    if ( !(word->flags & LTEXT_WORD_IS_TCY) && !word_is_latin_in_vertical )
        drawFlags |= LFNT_HINT_IS_VERTICAL;
    if ( word_is_latin_in_vertical )
        drawFlags |= LFNT_HINT_RENDER_ROTATE_FOR_VERTICAL;
    if ( word_is_vert_mark )
        drawFlags |= LFNT_HINT_VERTICAL_MARK;

    if ( word->flags & LTEXT_WORD_IS_TCY ) {
        // TCY (tate-chu-yoko): draw text horizontally within vertical column.
        // The span occupies 1 em of column depth; set its glyphs left-to-right
        // and centre the run in the column.  Do not stretch glyph bitmaps to
        // force them into the slot: that distorts half-width numerals both
        // horizontally and vertically.
        int em = font->getSize();
        int clamped_x = vertClampForward((int)word->x, state.vert_min_next_x);
        int tcy_run_width = (int)font->getTextWidth(
                srcline->t.text + word->t.start, (int)word->t.len,
                srcline->lang_cfg);
        int em_x = line_x - frmline->height + (frmline->height - em) / 2;
        x0_out = em_x + (em - tcy_run_width) / 2;
        int y_slot_start = y + frmline->x + clamped_x;
        y0_out = y_slot_start + (em - font->getHeight()) / 2;
        // Advance the DRAW tracker the same way the LAYOUT post-pass does for a
        // TCY word (it treats TCY as a non-CJK word advancing by word->width) and
        // set the prev-word trackers, so a following CJK char inserts the same
        // xkanjiskip LAYOUT inserted and does not apply JFM glue against a stale
        // class.  Without this, word->x (LAYOUT) and vert_min_next_x (DRAW) drift
        // for CJK→TCY→CJK (e.g. 平成23年) and the highlight/sbox is offset.
        int tcy_adv = (int)word->width > 0 ? (int)word->width : em;
        state.vert_min_next_x = clamped_x + tcy_adv;
        state.vert_prev_plain_y0 = y_slot_start;
        state.vert_prev_was_non_cjk_word = true;
        state.vert_prev_cjk_class = -1;
        state.vert_prev_effective_width = tcy_adv;
        if (y_slot_start + em > clip.bottom)
            vert_skip_draw_out = true;
        return;
    }
    if ( word_is_latin_in_vertical ) {
        // Non-CJK word in vertical column: render horizontally then rotate
        // 90° CW as a single block.  vert_min_next_x is updated to the start
        // of the rotated block; the caller updates it past the block end
        // after DrawTextString returns (using word->width).
        int font_h = font->getHeight();
        // Honour LAYOUT post-pass position (word->x) while keeping the
        // previous visual end as a lower bound to avoid overlap.
        int clamped_x = vertClampForward((int)word->x, state.vert_min_next_x);
        // Synchronise state.vert_min_next_x with where the glyph actually
        // starts so applyVerticalLatinPostDraw, which adds word->width onto
        // state.vert_min_next_x after DrawTextString, accumulates from the
        // correct base.  Without this, a Latin word whose word->x was pushed
        // forward by LAYOUT (clamped_x > state.vert_min_next_x) leaves the
        // next word's clamping lower bound short of the Latin block's
        // visual end.
        state.vert_min_next_x = clamped_x;
        x0_out = line_x - frmline->height + (frmline->height - font_h) / 2;
        y0_out = y + frmline->x + clamped_x;
        state.vert_prev_plain_y0 = y0_out;
        if ( y0_out >= clip.bottom )
            vert_skip_draw_out = true;
        return;
    }
    // Plain CJK (and vertical-mark) word in vertical column.
    //
    // For vertical-rl: line_x is the column's RIGHT edge.  frmline->height
    // is the column width.  Using line_x - frmline->height places the
    // glyph's LEFT edge at the column's LEFT boundary, so full-width CJK
    // glyphs fit exactly [line_x - col_width, line_x] with no overflow.
    //
    // word->y encodes horizontal-mode baseline alignment (small chars
    // pushed downward in horizontal text).  In vertical-rl this must NOT
    // shift the glyph leftward inside the column.  Plain-text words ignore
    // it; only inline-box words use word->y to select sub-column position.
    x0_out = line_x - frmline->height;
    // Centre plain-text chars on the column axis (JLReq typesetting).  Skip when
    // the column is inflated by a ruby inline box (frmline->height > strut)
    // — adding (strut-em)/2 would push the glyph into the annotation zone.
    {
        int em    = font->getSize();
        int strut = pbuffer->strut_height;
        // A replaced inline can widen a mixed line beyond the text strut.
        // Keep ordinary CJK on the same central column axis as the image,
        // TCY and rotated Latin branches. Ruby-only inflation is different:
        // its annotation deliberately occupies one side of the base column.
        if ( line_has_image && (int)frmline->height > em )
            x0_out += ((int)frmline->height - em) / 2;
        else if ( (int)frmline->height <= strut && em < strut )
            x0_out += (strut - em) / 2;

        if ( line_has_image ) {
            int expected_x0 = line_x - (int)frmline->height
                    + ((int)frmline->height - em) / 2;
            int drift = x0_out - expected_x0;
            if ( drift < 0 )
                drift = -drift;
            ltext_vert_mixed_image_axis_sample_count++;
            if ( drift > 0 ) {
                ltext_vert_mixed_image_axis_drift_count++;
                if ( drift > ltext_vert_mixed_image_axis_drift_max_px )
                    ltext_vert_mixed_image_axis_drift_max_px = drift;
            }
        }
    }
    JLReqVertClass curr_cjk_class = JLREQ_VERT_OTHER;
    if ( srcline->t.text && word->t.len > 0 ) {
        curr_cjk_class = getJLReqVertClass(srcline->t.text[word->t.start]);
    }
    // word->x carries the LAYOUT post-pass position, including JFM base glue,
    // xkanjiskip and justification stretch/shrink. getRect() uses word->x to
    // position the highlight rect, so DRAW must place the glyph at the same
    // column position.
    //
    // Keep the previous visual end as a lower bound to avoid overlap.
    int prev_end = state.vert_min_next_x;
    int clamped_x = vertClampForward((int)word->x, prev_end);
    y0_out = y + frmline->x + clamped_x;
    // Advance vert_min_next_x.  Skip the font_size clamp for half-em JFM
    // chars: Phase 3 intentionally sets word->width = em/2 for class
    // [1][2][3][4][7], and clamping would re-introduce a half-em gap.
    int effective_width = getVerticalEffectiveTextWidth((int)word->width,
            font->getSize(), true, curr_cjk_class);
    if ( verticalSpacingDebugEnabled() && srcline->t.text && word->t.len > 0 ) {
        fprintf(stderr,
            "VERT_SPACING U+%04X raw_x=%d prev_end=%d clamped=%d "
            "effective=%d gap_layout=%d gap_draw=%d y0=%d word_width=%d "
            "prev_class=%d curr_class=%d\n",
            (unsigned int)srcline->t.text[word->t.start],
            (int)word->x, prev_end, clamped_x, effective_width,
            (int)word->x - prev_end, clamped_x - prev_end, y0_out,
            (int)word->width, state.vert_prev_cjk_class,
            (int)curr_cjk_class);
    }
    state.vert_min_next_x = clamped_x + effective_width;
    // CJK word processed: reset "prev was non-CJK" flag and remember this
    // CJK char's JFM class for the next char's inter-class glue lookup.
    state.vert_prev_was_non_cjk_word = false;
    state.vert_prev_cjk_class = (int)curr_cjk_class;
    // Cap vert_min_next_x at the column height so compressed punctuation
    // (。、 with TTB < font_size) cannot push the next char past clip.bottom.
    if ( state.vert_min_next_x > clip.bottom - y )
        state.vert_min_next_x = clip.bottom - y;
    // Character overlap detection in column direction (P14).
    if ( state.vert_prev_plain_y0 >= 0 && y0_out < clip.bottom ) {
        int slot_end_prev = state.vert_prev_plain_y0 + state.vert_prev_effective_width;
        int overlap_px = slot_end_prev - y0_out;
        if ( overlap_px > 0 ) {
            ltext_vert_char_overlap_count++;
            if ( overlap_px > ltext_vert_char_overlap_max_px )
                ltext_vert_char_overlap_max_px = overlap_px;
        }
    }
    state.vert_prev_plain_y0 = y0_out;
    state.vert_prev_effective_width = effective_width;
    // A hanging punctuation slot may cross the regular content clip bottom:
    // its ink belongs in the bottom margin. LFormattedText::Draw
    // has already widened the active DrawBuf clip to content_overflow_clip for
    // a fully contained vertical column, so let DrawTextString use that clip.
    // Keep the regular early-out for every other character.
    bool is_hanging_punctuation = srcline->t.text && word->t.len == 1
            && isVerticalHangingChar(srcline->t.text[word->t.start]);
    // JFM compression and inter-class glue can place the punctuation origin a
    // few pixels before the regular bottom while its allocated slot still
    // crosses that boundary. Treat that as hanging too: checking y0 alone
    // misses the common long-paragraph case and leaves its lower ink clipped.
    word_is_exact_hanging_out = is_hanging_punctuation
            && y0_out + effective_width > clip.bottom;
    if ( word_is_exact_hanging_out )
        ltext_vert_exact_hanging_attempt_count++;
    if ( y0_out >= clip.bottom
            && !word_is_exact_hanging_out )
        vert_skip_draw_out = true;
}
