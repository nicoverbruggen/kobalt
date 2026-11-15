// Vertical box rendering from the Blaze fork of CoolReader Engine lvrend.cpp.
// (c) Vadim Lopatin, 2000-2006. Distributed under the GNU General Public License.
#include "vertical_boxes.h"
#include "lvtinydom.h"
#include "fb2def.h"
#include "lvrend.h"
#include "drawutil.h"
#include "lvlogical.h"

namespace blaze {

int resolveEffectiveWritingMode(ldomNode *enode)
{
    int writing_mode = enode->getStyle()->writing_mode;
    if ( writing_mode == css_wm_inherit ) {
        for (ldomNode * p = enode->getParentNode(); p && p->isElement(); p = p->getParentNode()) {
            css_style_ref_t ps = p->getStyle();
            if ( !ps.isNull() && ps->writing_mode != css_wm_inherit ) {
                writing_mode = ps->writing_mode;
                break;
            }
        }
    }
    return writing_mode;
}

// A document-wide vertical flow can be selected from another spine item's
// writing-mode declaration. In that case a later XHTML body may legitimately
// keep `writing-mode: inherit` on every DOM node, even though the current page
// is laid out and painted vertically. DrawDocument receives that resolved page
// mode through draw_extra_info; use it only when the DOM has no local answer.
int resolveEffectiveDrawWritingMode(ldomNode *enode, LVDrawBuf &drawbuf)
{
    int writing_mode = resolveEffectiveWritingMode(enode);
    if (writing_mode != css_wm_inherit)
        return writing_mode;
    draw_extra_info_t *extra = (draw_extra_info_t *)drawbuf.GetDrawExtraInfo();
    if (extra && extra->writing_mode != css_wm_inherit)
        return extra->writing_mode;
    return writing_mode;
}

static bool forkVerticalBoxDebugEnabled()
{
    return getenv("KO_DEBUG_VERT_BG") != NULL;
}

static bool isForkVerticalBoxDebugClass(ldomNode *enode)
{
    if ( !enode || !enode->isElement() )
        return false;
    lString32 cls = enode->getAttributeValue(attr_class);
    return cls == "sec1" || cls == "figure1" || cls == "column" || cls == "column1"
        || cls == "figure2" || cls == "figure3" || cls == "figure4"
        || cls == "figure5" || cls == "jitsuki" || cls == "jitsuki1"
        || cls == "jitsuki2" || cls == "formula" || cls == "calibre13"
        || cls == "margin1" || cls == "columntitle" || cls == "calibre20"
        || cls == "line";
}

bool shouldDebugForkVerticalBoxNode(ldomNode *enode)
{
    return forkVerticalBoxDebugEnabled() && isForkVerticalBoxDebugClass(enode);
}

static bool forkNodeContainsEffectiveImage(ldomNode *node)
{
    if ( !node )
        return false;
    if ( node->isEffectiveImage() )
        return true;
    for ( int i=0; i<node->getChildCount(); i++ ) {
        if ( forkNodeContainsEffectiveImage(node->getChildNode(i)) )
            return true;
    }
    return false;
}

static bool forkFormattedWordContainsImage(LFormattedTextRef txform, const formatted_word_t *word)
{
    if ( !word )
        return false;
    if ( word->flags & LTEXT_WORD_IS_IMAGE )
        return true;
    if ( !(word->flags & LTEXT_WORD_IS_INLINE_BOX) )
        return false;
    const src_text_fragment_t * src = txform->GetSrcInfo(word->src_text_index);
    return src && forkNodeContainsEffectiveImage((ldomNode*)src->object);
}

bool shouldStartForkVerticalImageLineOnFreshPage(bool is_vertical, LFormattedTextRef txform,
                                                        const formatted_line_t *line)
{
    return is_vertical
        && line
        && line->word_count == 1
        && line->x > 0
        && forkFormattedWordContainsImage(txform, &line->words[0]);
}

bool isForkVerticalSingleImageLine(bool is_vertical, LFormattedTextRef txform,
                                          const formatted_line_t *line)
{
    return is_vertical
        && line
        && line->word_count == 1
        && forkFormattedWordContainsImage(txform, &line->words[0]);
}

static int getVerticalInlineEndPadding(ldomNode *enode, RenderRectAccessor fmt, int debug_padding)
{
    int padding_end = 0;
    if ( RENDER_RECT_HAS_FLAG(fmt, INNER_FIELDS_SET) ) {
        padding_end = fmt.getWidth() - fmt.getInnerX() - fmt.getInnerWidth();
    }
    else {
        css_style_ref_t style = enode->getStyle();
        CSSLogical DL((css_writing_mode_t)resolveEffectiveWritingMode(enode));
        padding_end = lengthToPx( enode, style->padding[DL.padIE()], fmt.getWidth() )
                + debug_padding + measureBorder(enode, DL.brdIE());
    }
    return padding_end > 0 ? padding_end : 0;
}

// In vertical layout, RenderRectAccessor::width is the block-flow allocation
// (screen width), while the inline extent is the page height used by the text
// formatter.  For an auto-height block, CSS therefore makes its background
// and borders span that inline extent.  Using fmt.width here made a centered
// heading use the page height while its enclosing rectangle stopped at the
// page width.
static int getVerticalPaintedInlineSize(ldomNode *enode, RenderRectAccessor fmt,
                                        int doc_x)
{
    if ( RENDER_RECT_HAS_FLAG(fmt, VERTICAL_USED_INLINE_SIZE_SET) ) {
        int used_inline_size = fmt.getVerticalUsedInlineSize();
        if ( used_inline_size > 0 )
            return used_inline_size;
    }
    css_style_ref_t style = enode->getStyle();
    if ( style->height.type == css_val_unspecified
            && style->height.value == css_generic_auto ) {
        int page_height = enode->getDocument()->getPageHeight();
        int remaining_inline_size = page_height - doc_x;
        if ( remaining_inline_size > 0 )
            return remaining_inline_size;
    }
    return fmt.getWidth();
}

static lvRect verticalBoxRect(LVDrawBuf &drawbuf, int x0, int y0, int doc_x, int doc_y,
                              int block_size, int inline_size)
{
    lvRect clip;
    drawbuf.GetClipRect(&clip);
    draw_extra_info_t *info = (draw_extra_info_t *)drawbuf.GetDrawExtraInfo();
    const int anchor = info && info->vert_column_clip_right ? info->vert_column_clip_right : clip.right;
    const int right = anchor - (y0 + doc_y);
    const int top = x0 + doc_x;
    return lvRect(right - block_size, top, right, top + inline_size);
}

// DrawBorderVertical: draw a block's CSS borders in vertical-rl/lr writing mode.
//
// Upstream DrawBorder() draws straight to the buffer from the doc-space
// (Y=X swapped) rect (x0+doc_x, y0+doc_y, fmt.getWidth/Height) without the
// swap+mirror that LFormattedText::Draw applies to text.  In vertical mode that
// misplaces block borders: e.g. a "border-top" comes out as a full-width
// horizontal line near the page top instead of a short cap at the box's screen
// top edge.  We map the doc-space border box to screen coordinates exactly as
// initVerticalDrawSetup() maps text:
//     screen_y = x0 + doc_x              (block's doc-X axis -> screen Y)
//     screen_x = anchor - (y0 + doc_y)   (block's doc-Y axis -> screen X, mirrored)
// with anchor = draw_extra_info->vert_column_clip_right (fallback clip.right).
// So getHeight() (doc-Y extent) is the box's screen WIDTH and getWidth() its
// screen HEIGHT.  CSS physical borders stay physical on screen (as browsers do).
// Only solid borders are drawn precisely (FillRect per edge); other styles are
// approximated as solid and border-radius corners are squared - acceptable for
// vertical heading decorations in EPUBs. The caller dispatches vertical boxes here;
// DrawBorder handles horizontal boxes.
void DrawBorderVertical(ldomNode *enode, LVDrawBuf & drawbuf,
                               int x0, int y0, int doc_x, int doc_y, RenderRectAccessor fmt)
{
    css_style_ref_t style = enode->getStyle();
    bool hasTop    = (style->border_style_top    >= css_border_solid);
    bool hasRight  = (style->border_style_right  >= css_border_solid);
    bool hasBottom = (style->border_style_bottom >= css_border_solid);
    bool hasLeft   = (style->border_style_left   >= css_border_solid);

    css_length_t bw;
    bw = style->border_width[0]; hasTop    = hasTop    & !(bw.value==0 && bw.type>css_val_unspecified);
    bw = style->border_width[1]; hasRight  = hasRight  & !(bw.value==0 && bw.type>css_val_unspecified);
    bw = style->border_width[2]; hasBottom = hasBottom & !(bw.value==0 && bw.type>css_val_unspecified);
    bw = style->border_width[3]; hasLeft   = hasLeft   & !(bw.value==0 && bw.type>css_val_unspecified);

    lUInt32 colTop    = style->border_color[0].type != css_val_unspecified ? style->border_color[0].value : style->color.value;
    lUInt32 colRight  = style->border_color[1].type != css_val_unspecified ? style->border_color[1].value : style->color.value;
    lUInt32 colBottom = style->border_color[2].type != css_val_unspecified ? style->border_color[2].value : style->color.value;
    lUInt32 colLeft   = style->border_color[3].type != css_val_unspecified ? style->border_color[3].value : style->color.value;
    hasTop    = hasTop    & !IS_COLOR_FULLY_TRANSPARENT(colTop);
    hasRight  = hasRight  & !IS_COLOR_FULLY_TRANSPARENT(colRight);
    hasBottom = hasBottom & !IS_COLOR_FULLY_TRANSPARENT(colBottom);
    hasLeft   = hasLeft   & !IS_COLOR_FULLY_TRANSPARENT(colLeft);

    if ( !(hasTop || hasRight || hasBottom || hasLeft) )
        return;

    int wpct = 0; // % border widths are invalid, so this is unused
    int twidth = lengthToPx(enode, style->border_width[0], wpct); if (twidth==0) twidth = DEFAULT_BORDER_WIDTH;
    int rwidth = lengthToPx(enode, style->border_width[1], wpct); if (rwidth==0) rwidth = DEFAULT_BORDER_WIDTH;
    int bwidth = lengthToPx(enode, style->border_width[2], wpct); if (bwidth==0) bwidth = DEFAULT_BORDER_WIDTH;
    int lwidth = lengthToPx(enode, style->border_width[3], wpct); if (lwidth==0) lwidth = DEFAULT_BORDER_WIDTH;

    const int screen_height = getVerticalPaintedInlineSize(enode, fmt, doc_x);
    const lvRect box = verticalBoxRect(drawbuf, x0, y0, doc_x, doc_y, fmt.getHeight(), screen_height);

    if ( shouldDebugForkVerticalBoxNode(enode) ) {
        fprintf(stderr,
            "KO_DEBUG_VERT_BG border path=%s class=%s fmt_width=%d fmt_height=%d "
            "draw_width=%d doc=(%d,%d) screen=(%d,%d,%d,%d) borders=(%d,%d,%d,%d)\n",
            LCSTR(ldomXPointer(enode, 0).toString()),
            LCSTR(enode->getAttributeValue(attr_class)),
            fmt.getWidth(), fmt.getHeight(), screen_height, doc_x, doc_y,
            box.left, box.top, box.right, box.bottom,
            twidth, rwidth, bwidth, lwidth);
    }

    if (hasTop)
        drawbuf.FillRect(box.left, box.top, box.right, box.top + twidth, colTop);
    if (hasBottom)
        drawbuf.FillRect(box.left, box.bottom - bwidth, box.right, box.bottom, colBottom);
    if (hasRight)
        drawbuf.FillRect(box.right - rwidth, box.top, box.right, box.bottom, colRight);
    if (hasLeft)
        drawbuf.FillRect(box.left, box.top, box.left + lwidth, box.bottom, colLeft);
}

void DrawBackgroundColorVertical(LVDrawBuf & drawbuf, ldomNode *enode,
                               int x0, int y0, int doc_x, int doc_y, RenderRectAccessor fmt, lUInt32 bg_color, int debug_padding)
{
    int screen_height = getVerticalPaintedInlineSize(enode, fmt, doc_x); // doc-X extent -> screen height
    if ( shouldDebugForkVerticalBoxNode(enode) ) {
        fprintf(stderr,
            "KO_DEBUG_VERT_BG draw path=%s class=%s fmt_width=%d draw_width=%d fmt_height=%d "
            "doc=(%d,%d) screen_top=%d screen_bottom=%d "
            "padding_inline_end=%d\n",
            LCSTR(ldomXPointer(enode, 0).toString()),
            LCSTR(enode->getAttributeValue(attr_class)),
            fmt.getWidth(), screen_height, fmt.getHeight(), doc_x, doc_y,
            x0 + doc_x, x0 + doc_x + screen_height,
            getVerticalInlineEndPadding(enode, fmt, debug_padding));
    }
    const lvRect box = verticalBoxRect(drawbuf, x0, y0, doc_x, doc_y, fmt.getHeight(), screen_height);
    drawbuf.FillRect(box.left, box.top, box.right, box.bottom, bg_color);
}

lvRect verticalBackgroundImageRect(ldomNode *enode, LVDrawBuf &drawbuf, int x0, int y0,
                                   int doc_x, int doc_y, int block_size)
{
    const int inline_size = getVerticalPaintedInlineSize(enode, RenderRectAccessor(enode), doc_x);
    return verticalBoxRect(drawbuf, x0, y0, doc_x, doc_y, block_size, inline_size);
}

} // namespace blaze
