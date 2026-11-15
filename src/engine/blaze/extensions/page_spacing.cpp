// SPDX-License-Identifier: GPL-2.0-or-later
#include "page_spacing.h"
#include "lvtinydom.h"
#include "fb2def.h"
#include "renderutil.h"

namespace blaze {

BlockSpacing::BlockSpacing(ldomNode *enode, bool vertical)
{
    // Page-spacing eligibility is independent of horizontal alignment.
    // Known decorations and captions may move as rigid groups. A
    // floated initial anchors the whole opening prefix instead.
    bool spacing_heading = false, spacing_figure = false, spacing_decoration = false;
    ldomNode *opening = NULL, *spacing_group = NULL;
    for (ldomNode *ancestor = enode; ancestor; ancestor = ancestor->getParentNode()) {
        if (!ancestor->isElement()) continue;
        // Element and attribute ids, not names: this runs for
        // every ancestor of every block of text.
        lUInt16 ancestor_id = ancestor->getNodeId();
        spacing_heading |= (ancestor_id >= el_h1 && ancestor_id <= el_h6) ||
            ancestor->getAttributeValue(attr_role) == U"heading";
        if (ancestor_id == el_figure) {
            spacing_figure = true;
            spacing_group = ancestor;
        }
        spacing_decoration |= ancestor->getAttributeValue(attr_aria_hidden) == U"true" &&
            ancestor->getText().trim().empty();
        if (ancestor_id == el_p && ancestor->hasAttribute(attr_HasFirstLetter)) {
            // Color, font and other inline first-letter styles need
            // no anchored prefix. Only an actual initial exclusion does.
            LVArray<ldomNode *> initials;
            initials.add(ancestor);
            for (int n=0; n<initials.length() && !opening; ++n) {
                ldomNode *node = initials[n];
                if (node->getEffectiveNodeId() == el_pseudoElem && node->hasEffectiveAttribute(attr_FirstLetter)) {
                    css_style_ref_t initial = node->getStyle();
                    if (!initial.isNull() && initial->display != css_d_none &&
                        (initial->float_ != css_f_none || hasAppliedInitialLetter(initial.get()))) opening = ancestor;
                }
                for (int c=0; c<node->getChildCount(); ++c) initials.add(node->getChildNode(c));
            }
        }
    }
    bool justify_prose = enode->getAttributeValue("data-kobalt-prose") == U"1";
    bool justify_fixed = spacing_heading || spacing_figure || spacing_decoration || opening;
    bool spacing_safe = !vertical && (justify_prose || justify_fixed);
    for (ldomNode *ancestor = enode; spacing_safe && ancestor; ancestor = ancestor->getParentNode()) {
        if (!ancestor->isElement()) continue;
        css_style_ref_t a = ancestor->getStyle();
        if (a.isNull()) continue;
        lUInt16 ancestor_id = ancestor->getNodeId();
        bool initial_float = opening && ancestor->getText().trim().length() <= 3 &&
            (ancestor_id == el_floatBox || ancestor_id == el_pseudoElem || ancestor_id == el_span);
        if (ancestor_id == el_table || ancestor_id == el_pre ||
            ancestor_id == el_li || ancestor_id == el_blockquote ||
            (a->white_space != css_ws_normal && a->white_space != css_ws_inherit && a->white_space != css_ws_nowrap) ||
            (a->float_ != css_f_none && !initial_float) || !a->background_image.empty() ||
            (a->background_color.type == css_val_color && !IS_COLOR_FULLY_TRANSPARENT(a->background_color.value)) ||
            (a->border_style_top != css_border_none && a->border_width[0].value > 0) ||
            (a->border_style_right != css_border_none && a->border_width[1].value > 0) ||
            (a->border_style_bottom != css_border_none && a->border_width[2].value > 0) ||
            (a->border_style_left != css_border_none && a->border_width[3].value > 0)) spacing_safe = false;
    }
    LVArray<ldomNode *> spacing_nodes;
    if (spacing_safe) spacing_nodes.add(enode);
    for (int n=0; spacing_safe && n<spacing_nodes.length(); ++n) {
        ldomNode *node=spacing_nodes[n];
        lUInt16 node_id = node->getNodeId(); // 0 for text
        if (node_id == el_br || node_id == el_code || node_id == el_math || node_id == el_ruby) {
            spacing_safe = false;
            break;
        }
        for (int c=0; c<node->getChildCount(); ++c) spacing_nodes.add(node->getChildNode(c));
    }
    // Record the whole figure's boundaries before page splitting.
    // A figure split across columns must retain its authored layout.
    ldomNode *group_first = NULL, *group_last = NULL;
    if (spacing_group) {
        std::vector<ldomNode *> pending{spacing_group};
        while (!pending.empty()) {
            ldomNode *node = pending.back();
            pending.pop_back();
            if (node->getRendMethod() == erm_invisible) continue;
            if (node->getRendMethod() == erm_final) {
                if (!group_first) group_first = node;
                group_last = node;
            } else {
                for (int c=node->getChildCount()-1; c>=0; --c) {
                    ldomNode *child = node->getChildNode(c);
                    if (child->isElement()) pending.push_back(child);
                }
            }
        }
    }
    if (spacing_safe) {
        text_flags = justify_prose && !justify_fixed ? RN_LINE_JUSTIFIABLE_TEXT : RN_LINE_FIXED_SPACING;
        if (opening) text_flags |= RN_LINE_ANCHORED_SPACING;
    }
    allow_images = spacing_figure || spacing_decoration;
    if (enode == group_first) group_flags |= RN_LINE_SPACING_GROUP_START;
    if (enode == group_last) group_flags |= RN_LINE_SPACING_GROUP_END;
}

int BlockSpacing::lineFlags(const formatted_line_t &line, int index, int count, bool beside_float) const
{
    int flags = 0;
    if (index == 0) flags |= group_flags & RN_LINE_SPACING_GROUP_START;
    if (index == count - 1) flags |= group_flags & RN_LINE_SPACING_GROUP_END;
    if (!text_flags || !line.word_count || beside_float) return flags;
    for (int w = 0; w < line.word_count; ++w) {
        if ((line.words[w].flags & LTEXT_WORD_IS_IMAGE) && !allow_images) return flags;
        if ((line.words[w].flags & LTEXT_WORD_IS_INLINE_BOX) && !(text_flags & RN_LINE_ANCHORED_SPACING))
            return flags;
    }
    return flags | text_flags;
}

} // namespace blaze

void LVRendPageInfo::clearJustification()
{
    justificationLines.clear();
    justificationGaps.clear();
    justificationProseLines = justificationProseHeight = justificationPrefix = 0;
}

int LVRendPageInfo::justificationExtra(int available) const
{
    int count = justificationLines.length(), gaps = justificationGaps.length();
    if (!count || !gaps || justificationProseLines < 6 || footnotes.length() ||
        (flags & RN_PAGE_KEEP_VERTICAL_SPACING)) return 0;
    int extra = available - justificationLines[count-1].y;
    int average = justificationProseHeight / justificationProseLines;
    return extra > 0 && extra <= available / 8 && extra <= gaps * average / 8 ? extra : 0;
}

int LVRendPageInfo::centeringOffset(int available) const
{
    if (!justificationLines.length() || !justificationProseLines || footnotes.length() || available <= justificationPrefix) return 0;
    int first = 0;
    while (first < justificationLines.length() && justificationLines[first].x < justificationPrefix) ++first;
    if (first == justificationLines.length()) return 0;
    int top = justificationLines[first].x;
    int occupied = justificationLines[justificationLines.length()-1].y - top;
    int room = available - justificationPrefix;
    if (occupied > room || occupied * 10 < room * 9) return 0;
    int offset = (room - occupied) / 2 - (top - justificationPrefix);
    // An opening float stays attached to its paragraph. Do not pull the
    // following text into the protected prefix or its authored margin.
    return justificationPrefix && offset < 0 ? 0 : offset;
}

int LVRendPageInfo::justificationOffset(int y, int available, int mode, bool bottom) const
{
    if (bottom) --y;
    if (y < justificationPrefix) return 0;
    if (mode == 2) return centeringOffset(available);
    int extra = justificationExtra(available);
    if (!extra) return 0;
    int lo=0, hi=justificationGaps.length();
    while (lo < hi) {
        int mid=(lo+hi)/2;
        if (justificationGaps[mid] <= y) lo=mid+1;
        else hi=mid;
    }
    return extra * lo / justificationGaps.length();
}

int LVRendPageInfo::justificationSourceY(int y, int available, int mode, bool pull) const
{
    if (mode == 2) {
        if (y < justificationPrefix) return y;
        int offset = centeringOffset(available);
        if (justificationPrefix && y < justificationPrefix + offset)
            return pull ? justificationPrefix : -1;
        return y - offset;
    }
    int extra = justificationExtra(available);
    if (!extra) return y;
    int previous = 0;
    for (int i=0; i<justificationGaps.length(); ++i) {
        int offset = extra * (i+1) / justificationGaps.length();
        int edge = justificationGaps[i];
        if (y < edge + previous) return y - previous;
        if (y < edge + offset) return pull ? edge : -1;
        previous = offset;
    }
    return y - previous;
}

void LVRendPageContext::collectJustificationLines()
{
    LVArray<int> unsuitable_pages;
    LVArray<int> previous_prose;
    for (int i=0; i<page_list->length(); ++i) { unsuitable_pages.add(0); previous_prose.add(0); }
    // Retain text boxes while split lines still exist. Unknown content disables
    // filling for its entire page. Footnotes and forced endings retain their spacing.
    int spacing_group_page = -1;
    for (int i=0; i<lines.length(); ++i) {
        LVRendLineInfo *line = lines[i];
        if ((line->flags & RN_SPLIT_FOOT_NOTE) || line->getHeight() <= 0) continue;
        int lo=0, hi=page_list->length();
        while (lo < hi) {
            int mid=(lo+hi)/2;
            if ((*page_list)[mid]->start + (*page_list)[mid]->height <= line->getStart()) lo=mid+1;
            else hi=mid;
        }
        if (lo >= page_list->length()) continue;
        LVRendPageInfo *page=(*page_list)[lo];
        if (line->flags & RN_LINE_SPACING_GROUP_START) spacing_group_page = lo;
        if (spacing_group_page >= 0 && spacing_group_page != lo) {
            for (int p=spacing_group_page; p<=lo; ++p) {
                unsuitable_pages[p] = 1;
                (*page_list)[p]->flags |= RN_PAGE_KEEP_VERTICAL_SPACING;
                (*page_list)[p]->clearJustification();
            }
        }
        if (line->flags & RN_LINE_SPACING_GROUP_END) spacing_group_page = -1;
        if (line->getEnd() > page->start + page->height) {
            // An oversized object may span several engine pages. None of its
            // fragments may acquire a map made only from neighbouring prose.
            for (int p=lo; p<page_list->length() && (*page_list)[p]->start < line->getEnd(); ++p) {
                unsuitable_pages[p] = 1;
                (*page_list)[p]->flags |= RN_PAGE_KEEP_VERTICAL_SPACING;
                (*page_list)[p]->clearJustification();
            }
        }
        if (unsuitable_pages[lo]) continue;
        if (line->flags & RN_LINE_WHITESPACE) continue;
        bool anchored = (line->flags & RN_LINE_ANCHORED_SPACING) != 0;
        bool prose = (line->flags & RN_LINE_JUSTIFIABLE_TEXT) != 0;
        bool fixed = (line->flags & RN_LINE_FIXED_SPACING) != 0;
        if ((!prose && !fixed) || line->getStart() < page->start ||
            line->getEnd() > page->start + page->height ||
            (page->justificationLines.length() && line->getStart()-page->start < page->justificationLines[page->justificationLines.length()-1].y &&
             !anchored && line->getStart()-page->start >= page->justificationPrefix)) {
            page->flags |= RN_PAGE_KEEP_VERTICAL_SPACING;
            unsuitable_pages[lo] = 1;
            page->clearJustification();
            continue;
        }
        int top = line->getStart()-page->start, bottom = line->getEnd()-page->start;
        if (anchored) {
            page->justificationPrefix = bottom > page->justificationPrefix ? bottom : page->justificationPrefix;
            page->justificationGaps.clear();
            page->justificationProseLines = page->justificationProseHeight = 0;
        }
        if (top < page->justificationPrefix) prose = false;
        if (prose) {
            if (previous_prose[lo]) page->justificationGaps.add(top);
            ++page->justificationProseLines;
            page->justificationProseHeight += bottom-top;
        }
        previous_prose[lo] = prose;
        int count = page->justificationLines.length();
        if (count && top < page->justificationLines[count-1].y) {
            if (bottom > page->justificationLines[count-1].y) page->justificationLines[count-1].y = bottom;
        } else page->justificationLines.add(lvPoint(top, bottom));
    }
}
