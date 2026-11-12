#pragma once
#include <QString>
#include <QRect>
#include <QVector>
#include <limits>

namespace kobalt {
enum class LinkKind { None, Passage, Footnote, Website, Unavailable };

struct PageLink
{
    QString source;
    QVector<QRect> rectangles;
    LinkKind kind = LinkKind::None;

    bool tappable(const QString &mode) const
    {
        return mode == "all" || (mode == "footnotes" && kind == LinkKind::Footnote);
    }
};

struct LinkHit
{
    int index = -1;
    QRect bounds;
};

inline bool narrowFootnote(const QRect &rect)
{
    return rect.width() < rect.height();
}

inline QRectF footnoteTarget(const QRect &rect, int highlightPadding)
{
    const qreal diameter = rect.height() + 2 * highlightPadding;
    const auto center = QRectF(rect).center();
    return QRectF(center.x() - diameter / 2, center.y() - diameter / 2, diameter, diameter);
}

// Exact text wins over expanded targets, including text of a disabled link.
// Overlapping footnote circles select the nearest reference rectangle.
inline LinkHit linkAtPoint(const QVector<PageLink> &links, QPoint point, const QString &mode,
                           int highlightPadding, int ordinaryPadding = 0)
{
    for (int i = 0; i < links.size(); ++i)
        for (const auto &rect : links[i].rectangles)
            if (rect.contains(point))
                return links[i].tappable(mode) ? LinkHit{ i, rect } : LinkHit{};
    LinkHit hit;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (int i = 0; i < links.size(); ++i) {
        const auto &link = links[i];
        if (!link.tappable(mode))
            continue;
        for (const auto &rect : link.rectangles) {
            if (link.kind != LinkKind::Footnote || !narrowFootnote(rect)) {
                if (ordinaryPadding > 0
                    && rect.adjusted(-ordinaryPadding, -ordinaryPadding, ordinaryPadding,
                                     ordinaryPadding)
                               .contains(point))
                    return { i, rect };
                continue;
            }
            const auto circle = footnoteTarget(rect, highlightPadding);
            const auto delta = QPointF(point) - circle.center();
            const qreal radius = circle.width() / 2;
            if (delta.x() * delta.x() + delta.y() * delta.y() > radius * radius)
                continue;
            const auto bounds = QRectF(rect);
            const qreal dx = point.x() - qBound(bounds.left(), qreal(point.x()), bounds.right());
            const qreal dy = point.y() - qBound(bounds.top(), qreal(point.y()), bounds.bottom());
            const qreal distance = dx * dx + dy * dy;
            if (distance < nearest) {
                nearest = distance;
                hit = { i, rect };
            }
        }
    }
    return hit;
}

struct LinkDetails
{
    LinkKind kind = LinkKind::None;
    QString label, target, html, error;
    // The chapter that contains the target, for passages and notes.
    QString chapter;
};
} // namespace kobalt
