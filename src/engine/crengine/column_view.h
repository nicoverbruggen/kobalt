#pragma once
#include <lvdocview.h>
#include <QtGlobal>

namespace kobalt {
// Keep CREngine's pagination at one column. Public page numbers refer to whole
// screens, while source anchors remain the engine's original document positions.
class ColumnView final : public LVDocView
{
public:
    ColumnView() : LVDocView(32, true) { }

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
        // Its render context includes them (crengine-render-overflow.patch), so a
        // changed allowance lays the book out again, and a cached or retained layout
        // with the same allowance is reused.
        overflow = hangingPunctuation && columns > 1 ? qMin(sideMargin, gutter / 2) : 0;
        LVDocView::setPageMargins(
                columns > 1 ? lvRect(overflow, margins.top, overflow, margins.bottom) : margins);
        LVDocView::Resize(columnWidth(), h);
    }

    int getPageCount() { return (LVDocView::getPageCount() + columns - 1) / columns; }

    int getCurPage(bool internal = false)
    {
        const int page = LVDocView::getCurPage(true);
        return internal ? page : page / columns;
    }

    bool goToPage(int page, bool internal = false, bool updateBookmark = true, bool regulate = true)
    {
        return LVDocView::goToPage(internal ? page : page * columns, true, updateBookmark,
                                   regulate);
    }

    ldomXPointer getPageBookmark(int page, bool precise = true, bool internal = false)
    {
        return LVDocView::getPageBookmark(internal ? page : page * columns, precise, true);
    }

    int getBookmarkPage(ldomXPointer pointer, bool internal = false)
    {
        const int page = LVDocView::getBookmarkPage(pointer, true);
        return internal || page < 0 ? page : page / columns;
    }

    int getPageFlow(int page) { return LVDocView::getPageFlow(page * columns); }

    lString32 getPageText(bool wrap, int page = -1)
    {
        if (page < 0)
            page = getCurPage();
        lString32 text;
        for (int i = 0; i < columns && page * columns + i < LVDocView::getPageCount(); ++i) {
            if (i)
                text += U"\n";
            text += LVDocView::getPageText(wrap, page * columns + i);
        }
        return text;
    }

    void drawColumn(LVDrawBuf &buffer, int page, int column)
    {
        LVDocView::Draw(buffer, -1, page * columns + column, false, false);
    }

    ldomXPointer getNodeByPoint(lvPoint point, bool strict = false, bool selection = false)
    {
        if (columns == 1)
            return LVDocView::getNodeByPoint(point, strict, selection);
        point.x -= sideMargin;
        const int physical = qBound(0, (point.x + gutter / 2) / (width + gutter), columns - 1);
        const int local = point.x - physical * (width + gutter);
        if (local < -overflow || local >= width + overflow)
            return {};
        const int column = rightToLeft() ? columns - 1 - physical : physical;
        const int target = getCurPage() * columns + column;
        if (target >= LVDocView::getPageCount())
            return {};
        Position restore(*this, target);
        point.x = local + overflow;
        return LVDocView::getNodeByPoint(point, strict, selection);
    }

    bool docToWindowPoint(lvPoint &point, bool bottom = false, bool fit = false)
    {
        if (columns == 1)
            return LVDocView::docToWindowPoint(point, bottom, fit);
        const int first = getCurPage() * columns;
        const int target = getPageList()->FindNearestPage(point.y - (bottom ? 1 : 0), 0);
        if (target < first || target >= first + columns)
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
        const int first = getCurPage() * columns;
        for (int i = 0; i < columns && first + i < LVDocView::getPageCount(); ++i) {
            Position restore(*this, first + i);
            ldomXRangeList links;
            LVDocView::getCurrentPageLinks(links);
            for (int j = 0; j < links.length(); ++j)
                list.add(new ldomXRange(*links[j]));
        }
    }

private:
    bool rightToLeft()
    {
        return getDocument()
                && getDocument()->getProps()->getStringDef("doc.page-progression-direction")
                == U"rtl";
    }

    int columns = 1, width = 1, gutter = 0, spacing = 0, overflow = 0;
    bool hangingPunctuation = false;
    int viewportWidth = 1, viewportHeight = 1, sideMargin = 0;
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
