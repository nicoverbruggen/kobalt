#pragma once
#include <QPointF>
#include <QSize>
#include <QtGlobal>

namespace kobalt::ui {
// Only a vertical drag that began at an enabled edge of the reading page can
// claim a light control: the left edge for brightness, the right edge for
// warmth. Once horizontal intent wins, it cannot change its mind later.
class BrightnessGesture {
public:
    enum class Edge { None, Left, Right };
    void begin(QPointF point, QSize viewport, bool left = true, bool right = false) {
        origin = point;
        height = qMax(1, viewport.height());
        // Nickel's reader claims the outer 13% of the width.
        const double band = viewport.width() * .13;
        edge_ = left && point.x() >= 0 && point.x() <= band                                      ? Edge::Left
                : right && point.x() >= viewport.width() - band && point.x() <= viewport.width() ? Edge::Right
                                                                                                 : Edge::None;
        active_ = false;
    }
    bool move(QPointF point) {
        if (edge_ == Edge::None)
            return false;
        const auto delta = point - origin;
        if (!active_) {
            if (qAbs(delta.x()) > 32 && qAbs(delta.x()) >= qAbs(delta.y()))
                edge_ = Edge::None;
            else if (qAbs(delta.y()) > 32 && qAbs(delta.y()) > qAbs(delta.x()) * 1.5)
                active_ = true;
        }
        return active_;
    }
    // Share of the page height moved upwards since the drag began.
    double moved(QPointF point) const {
        return (origin.y() - point.y()) / height;
    }
    // A constant sensitivity keeps each step tied to the finger's distance,
    // regardless of how many pointer updates arrive during the swipe. The
    // light service clamps the resulting level to its supported range.
    static int change(double moved) {
        // Qt 5 rounds negative half steps towards zero. Round the magnitude
        // first so both directions behave the same on Qt 5 and Qt 6.
        const int change = qRound(150 * qAbs(moved));
        return moved < 0 ? -change : change;
    }
    bool active() const {
        return active_;
    }
    Edge edge() const {
        return edge_;
    }
    void cancel() {
        edge_ = Edge::None;
        active_ = false;
    }

private:
    QPointF origin;
    int height = 1;
    Edge edge_ = Edge::None;
    bool active_ = false;
};
}
