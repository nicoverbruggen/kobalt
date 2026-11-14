#pragma once
#include <lvdocview.h>
#include <QtGlobal>
#include <algorithm>
#include <memory>
#include <vector>

namespace kobalt {
// Keep CREngine's pagination at one column. Public page numbers refer to whole
// screens, while source anchors remain the engine's original document positions.
class ColumnView final : public LVDocView
{
public:
    ColumnView() : LVDocView(32, true) { }

    struct CallbackTarget
    {
        ColumnView *view;
    };

    ~ColumnView() override
    {
        callbackTarget_->view = nullptr;
        setCallback(nullptr);
    }

    // Callback guards follow the view when its owner moves, and can detach
    // safely when a failed opening destroys the view before the guard exits.
    const std::shared_ptr<CallbackTarget> &callbackTarget() const { return callbackTarget_; }

    void setColumns(int count) { columns = count; }

    int columnWidth() const { return width + 2 * overflow; }

    void setHangingPunctuation(bool enabled) { hangingPunctuation = enabled; }

    int columnOffset(int column)
    {
        return (columns > 1 ? sideMargin - overflow : 0)
                + (rightToLeft() ? columns - 1 - column : column) * (width + gutter);
    }

    void setColumnSpacing(int pixels) { spacing = pixels; }

    void setPageMargins(lvRect value)
    {
        margins = value;
        Resize(viewportWidth, viewportHeight);
    }

    void Resize(int w, int h)
    {
        viewportWidth = w;
        viewportHeight = h;
        // Side margins belong to the screen; spacing separates the text columns.
        // Hanging punctuation also needs paper inside each engine viewport.
        // Share the gutter evenly and keep the text width and origins unchanged.
        // CREngine clamps its viewport to 80 pixels. Reduce whitespace first
        // on small screens so its text geometry still matches our columns.
        const int minimumWidth = 80;
        sideMargin = qMin(margins.left, qMax(0, (w - minimumWidth * columns) / 2));
        const int available = qMax(minimumWidth * columns, w - 2 * sideMargin);
        gutter = columns > 1 ? qMin(spacing, (available - minimumWidth * columns) / (columns - 1))
                             : 0;
        width = columns > 1 ? qMax(1, (available - gutter * (columns - 1)) / columns) : w;
        // CREngine gives these side margins to each paragraph as its usable overflow.
        // Its render context includes them, so a
        // changed allowance lays the book out again, and a cached or retained layout
        // with the same allowance is reused.
        overflow = hangingPunctuation && columns > 1 ? qMin(sideMargin, gutter / 2) : 0;
        LVDocView::setPageMargins(
                columns > 1 ? lvRect(overflow, margins.top, overflow, margins.bottom) : margins);
        LVDocView::Resize(columnWidth(), h);
    }

    // A non-linear flow must be reachable on its own screen when ordinary
    // turns skip it. A short last screen has an empty trailing column.
    void groupColumns(bool separateFlows)
    {
        screenStarts.clear();
        if (!separateFlows || columns == 1)
            return;
        auto *pages = getPageList();
        if (!pages->hasNonLinearFlows())
            return;
        for (int first = 0; first < pages->length();) {
            screenStarts.push_back(first);
            const auto flow = pages->get(first)->flow;
            int end = first + 1;
            while (end < pages->length() && end - first < columns && pages->get(end)->flow == flow)
                ++end;
            first = end;
        }
    }

    int getPageCount()
    {
        return screenStarts.empty() ? (LVDocView::getPageCount() + columns - 1) / columns
                                    : int(screenStarts.size());
    }

    int firstColumn(int screen) const
    {
        if (screenStarts.empty())
            return screen * columns;
        return screen >= 0 && screen < int(screenStarts.size()) ? screenStarts[screen] : -1;
    }

    int columnCount(int screen)
    {
        const int first = firstColumn(screen);
        if (first < 0)
            return 0;
        const int end =
                screen + 1 < getPageCount() ? firstColumn(screen + 1) : LVDocView::getPageCount();
        return end - first;
    }

    int screenOfColumn(int column) const
    {
        if (column < 0 || screenStarts.empty())
            return column < 0 ? -1 : column / columns;
        return int(std::upper_bound(screenStarts.begin(), screenStarts.end(), column)
                   - screenStarts.begin())
                - 1;
    }

    int getCurPage(bool internal = false)
    {
        const int page = LVDocView::getCurPage(true);
        return internal ? page : screenOfColumn(page);
    }

    bool goToPage(int page, bool internal = false, bool updateBookmark = true, bool regulate = true)
    {
        return LVDocView::goToPage(internal ? page : firstColumn(page), true, updateBookmark,
                                   regulate);
    }

    ldomXPointer getPageBookmark(int page, bool precise = true, bool internal = false)
    {
        return LVDocView::getPageBookmark(internal ? page : firstColumn(page), precise, true);
    }

    int getBookmarkPage(ldomXPointer pointer, bool internal = false)
    {
        const int page = LVDocView::getBookmarkPage(pointer, true);
        return internal ? page : screenOfColumn(page);
    }

    int getPageFlow(int page) { return LVDocView::getPageFlow(firstColumn(page)); }

    lString32 getPageText(bool wrap, int page = -1)
    {
        if (page < 0)
            page = getCurPage();
        lString32 text;
        for (int i = 0; i < columnCount(page); ++i) {
            if (i)
                text += U"\n";
            text += LVDocView::getPageText(wrap, firstColumn(page) + i);
        }
        return text;
    }

    void drawColumn(LVDrawBuf &buffer, int page, int column)
    {
        const int physical =
                column < columnCount(page) ? firstColumn(page) + column : LVDocView::getPageCount();
        LVDocView::Draw(buffer, -1, physical, false, false);
    }

    ldomXPointer getNodeByPoint(lvPoint point, bool strict = false, bool selection = false)
    {
        const int screen = getCurPage();
        const int first = firstColumn(screen);
        const int count = columnCount(screen);
        for (int column = 0; column < count; ++column) {
            const auto &page = *getPageList()->operator[](first + column);
            const int left = columns > 1 ? columnOffset(column) + overflow : margins.left;
            const int right = columns > 1 ? left + width : viewportWidth - margins.right;
            if (point.x < left || point.x >= right)
                continue;
            int y = noteTop(page);
            for (int i = 0; i < page.footnotes.length(); ++i) {
                const auto &note = page.footnotes[i];
                if (point.y >= y && point.y < y + note.height)
                    return getDocument()->createXPointer(
                            lvPoint(point.x - left, note.start + point.y - y), PT_DIR_EXACT,
                            strict);
                y += note.height;
            }
        }
        if (columns == 1)
            return LVDocView::getNodeByPoint(point, strict, selection);
        point.x -= sideMargin;
        const int physical = qBound(0, (point.x + gutter / 2) / (width + gutter), columns - 1);
        const int local = point.x - physical * (width + gutter);
        if (local < -overflow || local >= width + overflow)
            return {};
        const int column = rightToLeft() ? columns - 1 - physical : physical;
        const int target = first + column;
        if (column >= count)
            return {};
        Position restore(*this, target);
        point.x = local + overflow;
        return LVDocView::getNodeByPoint(point, strict, selection);
    }

    bool docToWindowPoint(lvPoint &point, bool bottom = false, bool fit = false)
    {
        // Page notes use their source coordinates in the DOM, but the engine
        // draws their fragments at the bottom of each referencing page.
        const int screen = getCurPage();
        const int first = firstColumn(screen);
        const int count = columnCount(screen);
        for (int column = 0; column < count; ++column) {
            const auto &page = *getPageList()->operator[](first + column);
            int y = noteTop(page);
            for (int i = 0; i < page.footnotes.length(); ++i) {
                const auto &note = page.footnotes[i];
                if (point.y >= note.start
                    && (point.y < note.start + note.height
                        || (bottom && point.y == note.start + note.height))) {
                    point.y += y - note.start;
                    point.x += columns > 1 ? columnOffset(column) + overflow : margins.left;
                    return true;
                }
                y += note.height;
            }
        }
        if (columns == 1)
            return LVDocView::docToWindowPoint(point, bottom, fit);
        const int target = getPageList()->FindNearestPage(point.y - (bottom ? 1 : 0), 0);
        if (target < first || target >= first + count)
            return false;
        Position restore(*this, target);
        if (!LVDocView::docToWindowPoint(point, bottom, fit))
            return false;
        point.x += columnOffset(target - first);
        return true;
    }

    void getCurrentPageLinks(ldomXRangeList &list) override
    {
        if (columns == 1) {
            LVDocView::getCurrentPageLinks(list);
            return;
        }
        list.clear();
        const int screen = getCurPage();
        const int first = firstColumn(screen);
        const int count = columnCount(screen);
        for (int i = 0; i < count; ++i) {
            Position restore(*this, first + i);
            ldomXRangeList links;
            LVDocView::getCurrentPageLinks(links);
            for (int j = 0; j < links.length(); ++j)
                list.add(new ldomXRange(*links[j]));
        }
    }

private:
    int noteTop(const LVRendPageInfo &page) const
    {
        int y = viewportHeight - margins.bottom;
        for (int i = 0; i < page.footnotes.length(); ++i)
            y -= page.footnotes[i].height;
        return y;
    }

    bool rightToLeft()
    {
        return getDocument()
                && getDocument()->getProps()->getStringDef("doc.page-progression-direction")
                == U"rtl";
    }

    std::vector<int> screenStarts;
    int columns = 1, width = 1, gutter = 0, spacing = 0, overflow = 0;
    bool hangingPunctuation = false;
    int viewportWidth = 1, viewportHeight = 1, sideMargin = 0;
    const std::shared_ptr<CallbackTarget> callbackTarget_ =
            std::make_shared<CallbackTarget>(CallbackTarget{ this });
    lvRect margins;

    struct Position
    {
        ColumnView &view;
        ldomXPointer bookmark;
        bool moved;

        Position(ColumnView &view, int page)
            : view(view),
              bookmark(view.getBookmark()),
              moved(page != view.LVDocView::getCurPage(true))
        {
            if (moved)
                view.LVDocView::goToPage(page, true);
        }

        ~Position()
        {
            if (moved)
                view.goToBookmark(bookmark);
        }
    };
};
} // namespace kobalt
