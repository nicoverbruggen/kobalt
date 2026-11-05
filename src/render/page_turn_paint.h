#pragma once
#include "reading/page_turn_animation.h"
#include <QImage>
#include <QPainter>
#include <QtMath>

namespace kobalt::render {
inline int pageSweepCount(QSize size)
{
    return qMax(size.width(), size.height()) >= 1600 ? 12 : 10;
}

inline int pageSweepStep(qreal progress, QSize size)
{
    const int count = pageSweepCount(size);
    return qBound(0, qCeil(progress * count), count);
}

inline void paintPageTurn(QPainter &painter, const QImage &outgoing, const QImage &incoming,
                          qreal progress, int direction, pageTurn::Style style)
{
    if (style == pageTurn::Style::Off || progress >= 1) {
        painter.drawImage(QPoint(), incoming);
    } else {
        painter.save();
        painter.drawImage(QPoint(), outgoing);
        painter.setClipRect(incoming.rect(), Qt::IntersectClip);
        // Match the e-paper sweep's whole-band order. The text stays
        // in place while columns of the destination replace the old page.
        const int count = pageSweepCount(incoming.size());
        const int step = pageSweepStep(progress, incoming.size());
        const int left = direction > 0 ? incoming.width() * (count - step) / count : 0;
        const int right = direction > 0 ? incoming.width() : incoming.width() * step / count;
        const QRect revealed(left, 0, right - left, incoming.height());
        painter.drawImage(revealed, incoming, revealed);
        painter.restore();
    }
}

} // namespace kobalt::render
