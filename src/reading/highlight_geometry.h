#pragma once
#include <QRect>
#include <QString>
#include <QVector>

namespace kobalt {
struct HighlightGeometry
{
    QVector<QRect> rectangles;
    QString color;
    QString style = QStringLiteral("shaded");

    bool operator==(const HighlightGeometry &other) const
    {
        return rectangles == other.rectangles && color == other.color && style == other.style;
    }
};
} // namespace kobalt
