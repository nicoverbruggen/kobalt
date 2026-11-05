#pragma once
#include <QColor>
#include <QPainter>
#include <QPolygonF>
#include <QRectF>

namespace kobalt::ui {
inline void paintBookmarkRibbon(QPainter &painter, const QRectF &rect, const QColor &ink) {
    if (rect.isEmpty()) {
        return;
    }
    QPolygonF ribbon;
    ribbon << rect.topLeft() << rect.topRight() << rect.bottomRight()
           << QPointF(rect.center().x(), rect.top() + rect.height() * .78) << rect.bottomLeft();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPolygon(ribbon);
    painter.restore();
}
}
