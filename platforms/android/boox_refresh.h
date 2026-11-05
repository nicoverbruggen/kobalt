#pragma once
#include "app/reader/views/reading_page.h"
#include "platforms/android/refresh.h"
#include "render/image_damage.h"

#include <QPointer>

#include <utility>

namespace kobalt::android {
// BOOX display regions differ from the shared page's paint damage. Keep
// its display policy here; the shared paint policy also serves Kobo.
class BooxRefreshPolicy
{
public:
    void reset() { previous = {}; }

    template <class Prepare>
    bool prepare(QWidget *reader, QWidget *viewport, ReadingPage *page, const QRegion &damage,
                 Prepare submit)
    {
        return prepareRefreshFrame(reader, viewport, damage, [&](RefreshFrame frame) {
            State next;
            if (page && page->isVisible()) {
                const auto origin = viewport->mapToGlobal(QPoint()) * viewport->devicePixelRatioF()
                        + page->mapTo(reader, QPoint());
                next.page = page;
                next.bounds = QRect(origin, page->size());
                next.top = page->topHeight();
                next.bottom = page->bottomHeight();
                next.image = page->image.cacheKey();
                const bool compatible = previous.page == page && previous.bounds == next.bounds
                        && previous.top == next.top && previous.bottom == next.bottom
                        && previous.valid;
                const auto bars = QRegion(next.headerRect()) | QRegion(next.footerRect());
                if (compatible && (frame.screenPixels & bars).isEmpty()) {
                    next.header = previous.header;
                    next.footer = previous.footer;
                    next.valid = true;
                } else {
                    capture(page, next);
                }
                auto *controls = reader->findChild<QWidget *>("rrControlsTop");
                const bool reading = reader->property("reader_page_turn").toBool()
                        && (!controls || !controls->isVisible());
                if (reading && compatible && next.valid && previous.image != next.image) {
                    auto changed = stripDamage(previous.header, next.header)
                                           .translated(next.headerRect().topLeft());
                    changed += stripDamage(previous.footer, next.footer)
                                       .translated(next.footerRect().topLeft());
                    const auto pixels = (frame.screenPixels - QRegion(next.bounds))
                            | (frame.screenPixels & QRegion(next.bodyRect()))
                            | (frame.screenPixels & changed);
                    if (!pixels.isEmpty()) {
                        frame.screenPixels = pixels;
                    }
                }
                if (reading) {
                    frame.textPixels =
                            frame.text ? frame.screenPixels & QRegion(next.bodyRect()) : QRegion();
                }
            }
            if (!submit(frame)) {
                return false;
            }
            previous = std::move(next);
            return true;
        });
    }

private:
    struct State
    {
        QPointer<ReadingPage> page;
        QRect bounds;
        int top = 0, bottom = 0;
        qint64 image = 0;
        QImage header, footer;
        bool valid = false;

        QRect headerRect() const { return QRect(bounds.topLeft(), QSize(bounds.width(), top)); }

        QRect footerRect() const
        {
            return QRect(bounds.left(), bounds.bottom() + 1 - bottom, bounds.width(), bottom);
        }

        QRect bodyRect() const
        {
            return QRect(bounds.left(), bounds.top() + top, bounds.width(),
                         bounds.height() - top - bottom);
        }
    } previous;

    static void capture(ReadingPage *page, State &next)
    {
        // At most 4 MiB for each set of retained strips. The old and new
        // strips overlap until submit accepts. Never copy the page body.
        // Closing the reader or resizing its viewport releases the cache.
        const qint64 width = next.bounds.width(), height = next.bounds.height();
        if (width <= 0 || height <= 0 || next.top < 0 || next.bottom < 0
            || next.top + next.bottom > height || width * height * 4 > 16 * 1024 * 1024
            || width * (next.top + next.bottom) * 4 > 4 * 1024 * 1024) {
            return;
        }
        if (next.top)
            next.header = page->stripSnapshot(false);
        if (next.bottom)
            next.footer = page->stripSnapshot(true);
        next.valid = (!next.top || next.header.size() == QSize(int(width), next.top))
                && (!next.bottom || next.footer.size() == QSize(int(width), next.bottom));
    }

    static QRegion stripDamage(const QImage &before, const QImage &after)
    {
        if (before.cacheKey() == after.cacheKey())
            return {};
        if (before.size() != after.size()) {
            return QRegion(before.rect().united(after.rect()));
        }
        QRegion changed;
        // Compare the three slot columns separately. A changed clock and page
        // number must not join across the unchanged chapter title between them.
        for (int slot = 0; slot < 3; ++slot) {
            const int left = after.width() * slot / 3;
            const QRect column(left, 0, after.width() * (slot + 1) / 3 - left, after.height());
            if (!column.isEmpty()) {
                changed += render::imageDamage(before.copy(column), after.copy(column))
                                   .translated(column.topLeft());
            }
        }
        return changed;
    }
};
} // namespace kobalt::android
