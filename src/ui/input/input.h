#pragma once
#include <QMouseEvent>
#include <QTouchEvent>
#include <QVector>
#include <utility>

namespace kobalt::ui {
// Keep the Qt 5/6 event API difference out of controls and reading gestures.
inline bool isPointerMouse(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 3, 0)
    return event->source() == Qt::MouseEventNotSynthesized;
#else
    // Qt 5.2 has no source field. Accepting touch events prevents its default
    // touch-to-mouse fallback; the firmware input adapter must honor that.
    Q_UNUSED(event);
    return true;
#endif
}

inline QPointF mousePosition(const QMouseEvent *event, bool global = false)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return global ? event->globalPosition() : event->position();
#else
    return global ? event->screenPos() : event->localPos();
#endif
}

struct TouchContact
{
    int id;
    QPointF position;
    bool released;
};

// A graphics canvas forwards the original event and supplies positions in the
// receiving widget. This avoids depending on Qt's private event-point setters.
class MappedTouchEvent final : public QTouchEvent
{
public:
    MappedTouchEvent(const QTouchEvent &source, QVector<TouchContact> positions)
        : QTouchEvent(source), contacts(std::move(positions))
    {
    }

    QVector<TouchContact> contacts;
};

inline QVector<TouchContact> touchContacts(const QTouchEvent *event)
{
    if (const auto *mapped = dynamic_cast<const MappedTouchEvent *>(event))
        return mapped->contacts;
    QVector<TouchContact> contacts;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    for (const auto &point : event->points()) {
        contacts.append(
                { point.id(), point.position(), point.state() == QEventPoint::State::Released });
    }
#else
    for (const auto &point : event->touchPoints()) {
        contacts.append({ point.id(), point.pos(), point.state() == Qt::TouchPointReleased });
    }
#endif
    return contacts;
}

inline QVector<QPointF> touchPositions(const QTouchEvent *event, bool global = false)
{
    QVector<QPointF> positions;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    for (const auto &point : event->points())
        positions.append(global ? point.globalPosition() : point.position());
#else
    for (const auto &point : event->touchPoints())
        positions.append(global ? point.screenPos() : point.pos());
#endif
    return positions;
}
} // namespace kobalt::ui
