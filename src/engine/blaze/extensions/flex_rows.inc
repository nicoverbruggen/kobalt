// SPDX-License-Identifier: GPL-2.0-or-later
// Included after FlowState in lvrend.cpp. A flex line contributes one break
// opportunity. An item on a line of its own keeps its normal text breaks.
struct BlazeFlexItem {
    ldomNode *node;
    int width;
    int minimum, maximum;
    int base, grow, shrink;
    long long margins = 0;
    bool auto_margin[2] = {};
    long long outerWidth() const { return (long long)width + margins; }
};

static bool isAutoFlexMargin(css_length_t margin)
{
    return margin.type == css_val_unspecified && margin.value == css_generic_auto;
}

// Flex sizes are border boxes. CSS lengths on content boxes exclude these insets.
static int flexWidthInsets(ldomNode *node, int width)
{
    css_style_ref_t css = node->getStyle();
    if (css->box_sizing != css_bs_content_box) return 0;
    return lengthToPx(node, css->padding[0], width) + lengthToPx(node, css->padding[1], width)
            + measureBorder(node, 3) + measureBorder(node, 1);
}

// A zero containing width means intrinsic measurement of a definite flex basis.
static void measureFlexItem(BlazeFlexItem &item, int width, int direction, lUInt32 flags)
{
    css_style_ref_t css = item.node->getStyle();
    int insets = flexWidthInsets(item.node, width);
    getRenderedWidths(item.node, item.width, item.minimum, direction, true, flags,
                      true);
    if (css->width.type != css_val_unspecified && (width > 0 || css->width.type != css_val_percent)) {
        item.width = lengthToPx(item.node, css->width, width) + insets;
        item.minimum = std::min(item.minimum, item.width);
    }
    if (css->min_width.type != css_val_unspecified)
        item.minimum = lengthToPx(item.node, css->min_width, width) + insets;
    const int limit = width > 0 ? width : std::numeric_limits<int>::max();
    item.minimum = std::min(limit, std::max(1, item.minimum));
    item.maximum = limit;
    if (css->max_width.type != css_val_unspecified)
        item.maximum = std::max(item.minimum, std::min(limit, lengthToPx(item.node, css->max_width, width) + insets));
    item.base = css->flex_basis.type == css_val_unspecified ? item.width :
                lengthToPx(item.node, css->flex_basis, width) + insets;
    item.width = std::max(item.minimum, std::min(item.maximum, item.base));
}

static bool hasFixedFlexBasis(ldomNode *node)
{
    if (node->getRendMethod() != erm_final) return false;
    css_style_ref_t style = node->getStyle();
    const auto zeroMargin = [](css_length_t margin) {
        return !margin.value || isAutoFlexMargin(margin);
    };
    return !style->flex_grow && !style->flex_shrink &&
            style->flex_basis.type != css_val_unspecified && style->flex_basis.type != css_val_percent &&
            style->min_width.type != css_val_percent && style->max_width.type != css_val_percent &&
            zeroMargin(style->margin[0]) && zeroMargin(style->margin[1]) &&
            style->padding[0].type != css_val_percent && style->padding[1].type != css_val_percent;
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
        // the full row. Only horizontal margins on rows are handled here.
        for (int side = 0; side < 4; ++side) {
            const auto margin = item->margin[side];
            if (item->padding[side].type == css_val_percent || margin.type == css_val_percent)
                return false;
            if (margin.value && (column || side >= 2 ||
                    (!isAutoFlexMargin(margin) && (margin.type == css_val_unspecified || margin.value < 0))))
                return false;
        }
        // A block row measures both limits against its content width.
        // Columns and shrink-to-fit inline rows use different sizing paths.
        if ((column || style->display == css_d_inline_flex) &&
            (item->max_width.type == css_val_percent || item->min_width.type == css_val_percent))
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
        auto css = item.node->getStyle();
        item.margins = (long long)lengthToPx(item.node, css->margin[0], width)
                     + lengthToPx(item.node, css->margin[1], width);
        item.auto_margin[0] = isAutoFlexMargin(css->margin[0]);
        item.auto_margin[1] = isAutoFlexMargin(css->margin[1]);
        measureFlexItem(item, width, direction, flags);
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
            used += items[end++].outerWidth();
        } while (end < items.size() && (!style->flex_wrap || used + gap + items[end].outerWidth() <= width));

        // Freeze items at their minimum and divide the remaining main size
        // among the unfrozen items. Shrink factors scale with the flex basis.
        bool growing = used < width;
        long long remaining = width - (long long)gap * (long long)(end - first - 1);
        long double factors = 0;
        long long unscaled = 0;
        for (size_t i = first; i < end; ++i) {
            auto &item = items[i];
            remaining -= item.margins;
            if (growing ? item.grow == 0 || item.base > item.width
                        : item.shrink == 0 || item.base < item.width || item.base == 0) {
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
            // Opposing min/max constraints can cancel. Freeze only the side
            // that limits the line, then divide the remaining space again.
            const auto weightOf = [growing](const BlazeFlexItem &item) -> long double {
                return growing ? item.grow : (long double)item.shrink * item.base;
            };
            long double violation = 0;
            for (size_t i = first; i < end; ++i) {
                const auto &item = items[i];
                if (item.width >= 0) continue;
                const long double target = item.base + scale * weightOf(item);
                if (target < item.minimum) violation += item.minimum - target;
                if (target > item.maximum) violation += item.maximum - target;
            }
            for (size_t i = first; i < end; ++i) {
                auto &item = items[i];
                if (item.width >= 0) continue;
                const long double weight = weightOf(item);
                const long double target = item.base + scale * weight;
                if ((target < item.minimum && violation >= 0) ||
                    (target > item.maximum && violation <= 0)) {
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
                long double next = assigned + scale * weightOf(item);
                item.width = std::max(item.minimum, item.base + (int)(std::round(next) - std::round(assigned)));
                assigned = next;
            }
            break;
        }
        used = (long long)gap * (end - first - 1);
        int auto_margins = 0;
        for (size_t i = first; i < end; ++i) {
            used += items[i].outerWidth();
            auto_margins += items[i].auto_margin[0] + items[i].auto_margin[1];
        }
        int free = (int)std::max<long long>(0, width - used);
        // Main-axis auto margins take positive free space before justification.
        const int auto_margin_size = auto_margins ? free / auto_margins : 0;
        int auto_margin_remainder = auto_margins ? free % auto_margins : 0;
        const auto takeAutoMargin = [&] {
            return auto_margin_size + (auto_margin_remainder-- > 0);
        };
        if (auto_margins) free = 0;
        int offset = style->justify_content == 1 ? free / 2 : style->justify_content == 2 ? free : 0;
        bool stack = used > width;
        const bool definite_line = !style->flex_wrap && content_height > 0 && content_height <= flow->getPageHeight();
        int height = definite_line ? content_height : 0;
        lString32Collection links;
        if (!stack && (end > first + 1 || definite_line)) {
            for (size_t i = first; i < end; ++i) {
                auto &item = items[i];
                LVRendPageContext context(NULL, flow->getPageHeight(), 0, false);
                // The block formatter applies fixed margins inside the allocated box.
                // Leave the auto margin outside so it does not change the item width.
                int outer = (int)item.outerWidth();
                if (item.auto_margin[reversed ? 1 : 0]) offset += takeAutoMargin();
                int x = reversed ? width - offset - outer : offset;
                int h = renderBlockElement(context, item.node, left + x, 0, outer,
                                           0, 0, direction, NULL, flags, item.width);
                height = std::max(height, h);
                if (height > flow->getPageHeight()) {
                    for (size_t j = first; j <= i; ++j)
                        parent->getDocument()->getRendBlockCache().remove(items[j].node);
                    stack = true;
                    break;
                }
                links.addAll(*context.getLinkIds());
                if (i + 1 < end) offset += outer + gap;
                if (item.auto_margin[reversed ? 0 : 1]) offset += takeAutoMargin();
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
