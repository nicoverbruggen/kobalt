// SPDX-License-Identifier: GPL-2.0-or-later
// Included after FlowState in lvrend.cpp. A flex line contributes one break
// opportunity. An item on a line of its own keeps its normal text breaks.
struct BlazeFlexItem {
    ldomNode *node;
    int width;
    int minimum, maximum;
    int base, grow, shrink;
};

// Flex sizes are border boxes. CSS lengths on content boxes exclude these insets.
static int flexWidthInsets(ldomNode *node, int width)
{
    css_style_ref_t css = node->getStyle();
    if (css->box_sizing != css_bs_content_box) return 0;
    return lengthToPx(node, css->padding[0], width) + lengthToPx(node, css->padding[1], width)
            + measureBorder(node, 3) + measureBorder(node, 1);
}

static bool renderFlexRows(FlowState *flow, ldomNode *parent, int left, int width,
                           int direction, lUInt32 flags, int content_height)
{
    if (width <= 0 || flow->isVertical() || flow->hasActiveFloats())
        return false;
    css_style_ref_t style = parent->getStyle();
    bool column = style->flex_direction == 1;
    if (style->flex_direction == 3 || (column && (content_height <= 0 ||
            content_height > flow->getPageHeight() || !style->justify_content)))
        return false;

    // Check eligibility before publishing any lines to the page splitter.
    std::vector<BlazeFlexItem> items;
    for (int i = 0; i < parent->getChildCount(); ++i) {
        ldomNode *node = parent->getChildNode(i);
        if (node->isText()) {
            if (node->isWhitespaceText()) continue;
            return false;
        }
        if (node->getRendMethod() == erm_invisible) continue;
        css_style_ref_t item = node->getStyle();
        if (item->display != css_d_block || node->getRendMethod() != erm_final ||
            item->page_break_before != css_pb_auto || item->page_break_after != css_pb_auto ||
            css_wm_is_vertical(item->writing_mode))
            return false;
        // The independent item flow cannot resolve percentage insets against
        // the full row. Keep these and auto margins on the normal block path.
        for (int side = 0; side < 4; ++side) {
            if (item->padding[side].type == css_val_percent ||
                item->margin[side].type == css_val_percent || item->margin[side].value != 0)
                return false;
        }
        if (item->max_width.type == css_val_percent ||
            item->min_width.type == css_val_percent)
            return false;
        if (column && (item->flex_grow || item->flex_basis.type != css_val_unspecified))
            return false;
        items.push_back({node, 0, 0, width, 0, item->flex_grow, item->flex_shrink});
    }
    if (items.empty()) return true;
    if (items.size() == 1 && content_height <= 0 && items[0].grow == 0 &&
        items[0].node->getStyle()->flex_basis.type == css_val_unspecified) return false;
    if (column) {
        // Keep a short title stack together. Taller stacks take the normal
        // paginated column path, which retains each child's text breaks.
        int gap = lengthToPx(parent, style->row_gap, 0);
        int total = 0;
        std::vector<lString32Collection> links;
        std::vector<int> heights;
        for (auto &item : items) {
            css_style_ref_t css = item.node->getStyle();
            int item_width = width;
            if (css->width.type != css_val_unspecified || style->align_items != flex_stretch) {
                int minimum = 0;
                item_width = 0;
                getRenderedWidths(item.node, item_width, minimum, direction, true, flags);
                if (css->width.type == css_val_percent)
                    item_width = lengthToPx(item.node, css->width, width) + flexWidthInsets(item.node, width);
                item_width = std::min(width, item_width);
            }
            int spare = width - item_width;
            bool right = style->align_items == flex_end;
            if (direction == REND_DIRECTION_RTL) right = !right;
            int x = style->align_items == flex_center ? spare / 2 : right ? spare : 0;
            LVRendPageContext context(NULL, flow->getPageHeight(), 0, false);
            int height = renderBlockElement(context, item.node, left + x, 0, item_width,
                                            0, 0, direction, NULL, flags, item_width);
            links.emplace_back();
            links.back().addAll(*context.getLinkIds());
            if (!heights.empty()) total += gap;
            heights.push_back(height);
            total += height;
            if (total > content_height) {
                // Ordinary layout must format these nodes again at its own width.
                for (size_t i = 0; i < heights.size(); ++i)
                    parent->getDocument()->getRendBlockCache().remove(items[i].node);
                return false;
            }
        }
        // Slice admission records the first visible block. Leave that state
        // untouched until we know this stack can use the fixed-height path.
        bool visible = false;
        for (auto &item : items) {
            auto *doc = item.node->getDocument();
            if (flow->isMainFlow() && !doc->koblazeEnterBlock(item.node, false, false))
                doc->koblazeChangeRendMethod(item.node, erm_invisible);
            else
                visible = true;
        }
        if (!visible) return true;
        flow->pushVerticalMargin();
        int spare = content_height - total;
        int y = flow->getCurrentRelativeY();
        if (style->justify_content == 1) y += spare / 2;
        if (style->justify_content == 2) y += spare;
        for (size_t i = 0; i < items.size(); ++i) {
            RenderRectAccessor rect(items[i].node);
            rect.setY(y + rect.getY());
            y += heights[i] + gap;
            if (style->justify_content == 3 && i + 1 < items.size())
                y += spare / (items.size()-1) + (i < spare % (items.size()-1));
        }
        flow->addContentLine(content_height, RN_SPLIT_BOTH_AUTO);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].node->getRendMethod() == erm_invisible) continue;
            for (int j = 0; j < links[i].length(); ++j)
                flow->getPageContext()->addLink(links[i][j]);
        }
        return true;
    }
    if (flow->isMainFlow()) {
        items.erase(std::remove_if(items.begin(), items.end(), [](const BlazeFlexItem &item) {
            auto *doc = item.node->getDocument();
            if (doc->koblazeEnterBlock(item.node, false, false)) return false;
            doc->koblazeChangeRendMethod(item.node, erm_invisible);
            return true;
        }), items.end());
    }
    for (auto &item : items) {
        koblazeCheckCancel();
        koblazePausePoint();
        css_style_ref_t css = item.node->getStyle();
        int insets = flexWidthInsets(item.node, width);
        getRenderedWidths(item.node, item.width, item.minimum, direction, false, flags,
                          true);
        if (css->width.type != css_val_unspecified) {
            item.width = lengthToPx(item.node, css->width, width) + insets;
            item.minimum = std::min(item.minimum, item.width);
        }
        if (css->min_width.type != css_val_unspecified)
            item.minimum = lengthToPx(item.node, css->min_width, width) + insets;
        item.minimum = std::min(width, std::max(1, item.minimum));
        if (css->max_width.type != css_val_unspecified)
            item.maximum = std::max(item.minimum, std::min(width, lengthToPx(item.node, css->max_width, width) + insets));
        item.base = css->flex_basis.type == css_val_unspecified ? item.width :
                    lengthToPx(item.node, css->flex_basis, width) + insets;
        item.width = std::max(item.minimum, std::min(item.maximum, item.base));
    }

    int gap = lengthToPx(parent, style->column_gap, 0);
    int row_gap = lengthToPx(parent, style->row_gap, 0);
    bool reversed = (direction == REND_DIRECTION_RTL) != (style->flex_direction == 2);
    for (size_t first = 0; first < items.size();) {
        if (first && row_gap > 0) flow->addContentSpace(row_gap, 1, true, true, false);
        size_t end = first;
        long long used = 0;
        do {
            if (end > first) used += gap;
            used += items[end++].width;
        } while (end < items.size() && (!style->flex_wrap || used + gap + items[end].width <= width));

        // Freeze items at their minimum and divide the remaining main size
        // among the unfrozen items. Shrink factors scale with the flex basis.
        bool growing = used <= width;
        long long remaining = width - (long long)gap * (long long)(end - first - 1);
        long double factors = 0;
        long long unscaled = 0;
        for (size_t i = first; i < end; ++i) {
            auto &item = items[i];
            if (growing ? item.grow == 0 : item.shrink == 0 || item.base == 0) {
                remaining -= item.width;
                continue;
            }
            unscaled += growing ? item.grow : item.shrink;
            factors += growing ? item.grow : (long double)item.shrink * item.base;
            remaining -= item.base;
            item.width = -1;
        }
        const long long initial_free = remaining;
        while (factors > 0) {
            bool froze = false;
            long double free_space = remaining;
            if (unscaled < 256) {
                long double limit = (long double)initial_free * unscaled / 256;
                if (std::abs(limit) < std::abs(free_space)) free_space = limit;
            }
            const long double scale = free_space / factors;
            for (size_t i = first; i < end; ++i) {
                auto &item = items[i];
                if (item.width >= 0) continue;
                const long double weight = growing ? item.grow : (long double)item.shrink * item.base;
                const long double target = item.base + scale * weight;
                if (target < item.minimum || target > item.maximum) {
                    item.width = target < item.minimum ? item.minimum : item.maximum;
                    remaining -= item.width - item.base;
                    factors -= weight;
                    unscaled -= growing ? item.grow : item.shrink;
                    froze = true;
                }
            }
            if (froze) {
                koblazeCheckCancel();
                koblazePausePoint();
                continue;
            }
            long double assigned = 0;
            for (size_t i = first; i < end; ++i) {
                auto &item = items[i];
                if (item.width >= 0) continue;
                long double next = assigned + scale * (growing ? item.grow : (long double)item.shrink * item.base);
                item.width = std::max(item.minimum, item.base + (int)(std::round(next) - std::round(assigned)));
                assigned = next;
            }
            break;
        }
        used = (long long)gap * (end - first - 1);
        for (size_t i = first; i < end; ++i) used += items[i].width;
        const int free = (int)std::max<long long>(0, width - used);
        int offset = style->justify_content == 1 ? free / 2 : style->justify_content == 2 ? free : 0;
        bool stack = used > width;
        const bool definite_line = !style->flex_wrap && content_height > 0 && content_height <= flow->getPageHeight();
        int height = definite_line ? content_height : 0;
        lString32Collection links;
        if (!stack && (end > first + 1 || definite_line)) {
            for (size_t i = first; i < end; ++i) {
                auto &item = items[i];
                LVRendPageContext context(NULL, flow->getPageHeight(), 0, false);
                int x = reversed ? width - offset - item.width : offset;
                int h = renderBlockElement(context, item.node, left + x, 0, item.width,
                                           0, 0, direction, NULL, flags, item.width);
                height = std::max(height, h);
                if (height > flow->getPageHeight()) {
                    for (size_t j = first; j <= i; ++j)
                        parent->getDocument()->getRendBlockCache().remove(items[j].node);
                    stack = true;
                    break;
                }
                links.addAll(*context.getLinkIds());
                if (i + 1 < end) offset += item.width + gap;
                if (style->justify_content == 3 && i + 1 < end)
                    offset += free / (end-first-1) + (i-first < free % (end-first-1));
            }
        }
        if (stack || (end == first + 1 && !definite_line)) {
            int alignment = style->justify_content == 3 ? 0 : style->justify_content;
            if (reversed) alignment = 2 - alignment;
            for (size_t i = first; i < end; ++i)
                renderBlockElementEnhanced(flow, items[i].node, left, width, flags, stack ? -1 : alignment,
                                           stack ? -1 : items[i].width);
        } else {
            flow->pushVerticalMargin();
            int y = flow->getCurrentRelativeY();
            for (size_t i = first; i < end; ++i) {
                RenderRectAccessor rect(items[i].node);
                int spare = height - rect.getHeight();
                int shift = style->align_items == flex_center ? spare / 2 : style->align_items == flex_end ? spare : 0;
                rect.setY(y + rect.getY() + shift);
                if (style->align_items == flex_stretch && items[i].node->getStyle()->height.type == css_val_unspecified)
                    rect.setHeight(height);
            }
            flow->addContentLine(height, RN_SPLIT_BOTH_AUTO);
            for (int i = 0; i < links.length(); ++i) flow->getPageContext()->addLink(links[i]);
        }
        first = end;
    }
    return true;
}
