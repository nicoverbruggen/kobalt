#include "support/qt_compat.h"
#include "ui/input/brightness_gesture.h"
#include <QCoreApplication>
#include <cmath>
#define CHECK(x)                                 \
    do {                                         \
        if (!(x))                                \
            qFatal("line %d: %s", __LINE__, #x); \
    } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    kobalt::ui::BrightnessGesture gesture;
    for (const QSize size : { QSize(1072, 1448), QSize(1448, 1072) }) {
        gesture.begin({ 20, 600 }, size);
        CHECK(!gesture.move({ 20, 570 }) && !gesture.active());
        CHECK(gesture.move({ 24, 500 }) && gesture.active());
        CHECK(qAbs(gesture.moved({ 24, 500 }) - 100. / size.height()) < 1e-9);
        CHECK(gesture.moved({ 20, 900 }) < 0);
        gesture.cancel();
        CHECK(!gesture.active() && !gesture.move({ 20, 400 }));
        gesture.begin({ size.width() * .3, 600 }, size);
        CHECK(!gesture.move({ size.width() * .3, 400 }));
        gesture.begin({ 20, 600 }, size);
        CHECK(!gesture.move({ 80, 602 }));
        CHECK(!gesture.move({ 20, 300 }));
        gesture.begin({ 20, 600 }, size);
        gesture.cancel();
        CHECK(!gesture.move({ 20, 400 }));
        using Edge = kobalt::ui::BrightnessGesture::Edge;
        const double right = size.width() - 20;
        // The right edge claims warmth only when it is enabled.
        gesture.begin({ right, 600 }, size);
        CHECK(gesture.edge() == Edge::None && !gesture.move({ right, 400 }));
        gesture.begin({ right, 600 }, size, true, true);
        CHECK(gesture.edge() == Edge::Right);
        CHECK(gesture.move({ right - 4, 500 }) && gesture.active()
              && gesture.edge() == Edge::Right);
        CHECK(qAbs(gesture.moved({ right, 500 }) - 100. / size.height()) < 1e-9);
        gesture.begin({ right, 600 }, size, true, true);
        CHECK(!gesture.move({ right - 80, 602 }));
        CHECK(!gesture.move({ right, 300 }));
        gesture.begin({ size.width() * .7, 600 }, size, true, true);
        CHECK(!gesture.move({ size.width() * .7, 400 }));
        // With both edges off, as when the setting is off, no edge claims a drag.
        gesture.begin({ 20, 600 }, size, false, false);
        CHECK(gesture.edge() == Edge::None && !gesture.move({ 20, 400 }));
        gesture.begin({ 20, 600 }, size, true, true);
        CHECK(gesture.edge() == Edge::Left);
    }
    using Gesture = kobalt::ui::BrightnessGesture;
    // The approved swipe moves 150 percentage points per page height. Small
    // movements round to the nearest level instead of forcing a change.
    CHECK(Gesture::change(0) == 0 && Gesture::change(.001) == 0 && Gesture::change(-.001) == 0);
    CHECK(Gesture::change(.01) == 2 && Gesture::change(-.01) == -2);
    CHECK(Gesture::change(.03) == 5 && Gesture::change(-.03) == -5);
    CHECK(Gesture::change(.05) == 8 && Gesture::change(-.05) == -8);
    const double belowHalf = std::nextafter(.01, 0.);
    const double aboveHalf = std::nextafter(.01, .02);
    CHECK(Gesture::change(belowHalf) == 1 && Gesture::change(-belowHalf) == -1);
    CHECK(Gesture::change(aboveHalf) == 2 && Gesture::change(-aboveHalf) == -2);
    CHECK(Gesture::change(.1) == 15 && Gesture::change(-.1) == -15);
    CHECK(Gesture::change(.4) == 60 && Gesture::change(-.4) == -60);
    CHECK(Gesture::change(.8) == 120 && Gesture::change(1) == 150 && Gesture::change(-1) == -150);
    for (double moved = 0; moved < 1; moved += .01) {
        CHECK(Gesture::change(moved) <= Gesture::change(moved + .01));
        CHECK(Gesture::change(-moved) == -Gesture::change(moved));
    }
    // A drag and a reversal use the original level, so event frequency never
    // accumulates rounding errors and returning to the start restores it.
    for (const QSize size : { QSize(1072, 1448), QSize(1448, 1072) }) {
        const QPointF start(20, size.height() * .75);
        gesture.begin(start, size);
        CHECK(gesture.move(start - QPointF(0, size.height() * .1)));
        const auto level = [&](double distance) {
            const QPointF point = start - QPointF(0, size.height() * distance);
            CHECK(gesture.move(point));
            return qBound(0, 40 + Gesture::change(gesture.moved(point)), 100);
        };
        CHECK(level(.1) == 55 && level(.2) == 70 && level(.3) == 85);
        CHECK(level(.5) == 100 && level(.6) == 100);
        CHECK(level(.2) == 70 && level(0) == 40);
        CHECK(level(-.1) == 25 && level(-.3) == 0);
        CHECK(level(-.1) == 25 && level(0) == 40);
        gesture.cancel();
        gesture.begin(start, size);
        CHECK(level(.3) == 85);
        gesture.cancel();
    }
    // Nickel's reader claims the outer 13% of the width at either edge.
    for (const QSize size : { QSize(1072, 1448), QSize(1448, 1072) }) {
        gesture.begin({ size.width() * .12, 600 }, size, true, true);
        CHECK(gesture.edge() == Gesture::Edge::Left);
        gesture.begin({ size.width() * .14, 600 }, size, true, true);
        CHECK(gesture.edge() == Gesture::Edge::None);
        gesture.begin({ size.width() * .88, 600 }, size, true, true);
        CHECK(gesture.edge() == Gesture::Edge::Right);
        gesture.begin({ size.width() * .86, 600 }, size, true, true);
        CHECK(gesture.edge() == Gesture::Edge::None);
    }
    qInfo("PASS: enabled edges, vertical intent, horizontal rejection, cancellation, linear sensitivity, reversal, clamping, event-independent levels and orientation scaling");
}
