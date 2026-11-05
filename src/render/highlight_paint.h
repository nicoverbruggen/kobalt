#pragma once
#include <QColor>
#include <QPainter>
#include <QVector>
#include <QRegion>
#include <QString>

namespace kobalt::ui {
inline QColor highlightTint(const QString &key) {
    if (key == "yellow") {
        return QColor("#e8df9b");
    }
    if (key == "green") {
        return QColor("#b8d2aa");
    }
    if (key == "blue") {
        return QColor("#aecbdc");
    }
    if (key == "pink") {
        return QColor("#dfb5c2");
    }
    return QColor("#d7d7d7");
}
// The page and its thumbnails use the same tint without obscuring text.
inline void paintHighlightTint(QPainter &painter, const QRegion &region, QColor color, bool dark) {
    if (region.isEmpty())
        return;
    painter.save();
    painter.setClipRegion(region);
    painter.setCompositionMode(dark ? QPainter::CompositionMode_Screen : QPainter::CompositionMode_Multiply);
    painter.fillRect(region.boundingRect(), dark ? (color == highlightTint("gray")
                                                        ? QColor(64, 64, 64)
                                                        : QColor(qRound(color.red() * 0.3), qRound(color.green() * 0.3),
                                                                 qRound(color.blue() * 0.3)))
                                                 : color);
    painter.restore();
}
inline void paintHighlightMark(QPainter &painter, const QVector<QRect> &rectangles, const QRegion &visible,
                               QColor color, const QString &style, bool dark) {
    if (style != "underline" && style != "dotted") {
        paintHighlightTint(painter, visible, color, dark);
        return;
    }
    painter.save();
    painter.setClipRegion(visible, Qt::IntersectClip);
    const QColor ink = color == highlightTint("gray") ? QColor(dark ? Qt::white : Qt::black) : color;
    painter.setCompositionMode(dark ? QPainter::CompositionMode_Screen : QPainter::CompositionMode_Multiply);
    // One line metric fixes the baseline and stroke height across the passage.
    int lineHeight = 0;
    for (const auto &rect : rectangles)
        lineHeight = qMax(lineHeight, rect.height());
    const int thickness = qMax(1, qRound(lineHeight / 18.0));
    QRegion lines;
    for (const auto &rect : rectangles)
        lines += QRect(rect.left(), rect.bottom() - thickness + 1, rect.width(), thickness);
#if QT_VERSION < QT_VERSION_CHECK(5, 8, 0)
    for (const auto &line : lines.rects()) {
#else
    for (const auto &line : lines) {
#endif
        if (style == "underline") {
            painter.fillRect(line, ink);
        } else {
            const int period = thickness * 3;
            // Align dots to page pixels, so adjoining ranges share one pattern.
            const int first = line.left() - line.left() % period;
            for (int x = first; x <= line.right(); x += period)
                painter.fillRect(QRect(x, line.top(), thickness, line.height()).intersected(line), ink);
        }
    }
    painter.restore();
}
}
