#pragma once
#include <QRectF>
#include <QVector>

namespace kobalt::tapZones {
enum class Layout { LeftRight, Reversed, TopBottom, OneHanded };
enum class Action { Previous, Menu, Next };
inline constexpr Layout layouts[] = {Layout::LeftRight, Layout::Reversed, Layout::TopBottom, Layout::OneHanded};
struct Region {
    QRectF bounds;
    Action action;
};
// Later regions cover earlier ones. The picker and hit test use the same geometry.
inline QVector<Region> regions(Layout layout) {
    switch (layout) {
    case Layout::LeftRight:
        return {{{0, 0, .33, 1}, Action::Previous}, {{.33, 0, .34, 1}, Action::Menu}, {{.67, 0, .33, 1}, Action::Next}};
    case Layout::Reversed:
        return {{{0, 0, .33, 1}, Action::Next}, {{.33, 0, .34, 1}, Action::Menu}, {{.67, 0, .33, 1}, Action::Previous}};
    case Layout::TopBottom:
        return {{{0, 0, 1, .4}, Action::Previous}, {{0, .4, 1, .2}, Action::Menu}, {{0, .6, 1, .4}, Action::Next}};
    case Layout::OneHanded:
        return {{{0, 0, 1, 1}, Action::Next}, {{.33, .4, .34, .2}, Action::Menu}};
    }
    return {};
}
inline Action action(Layout layout, qreal x, qreal y) {
    // Keep this allocation-free on the reading input path.
    if (layout == Layout::LeftRight)
        return x <= .33 ? Action::Previous : x >= .67 ? Action::Next : Action::Menu;
    if (layout == Layout::Reversed) {
        return x <= .33 ? Action::Next : x >= .67 ? Action::Previous : Action::Menu;
    }
    if (layout == Layout::TopBottom)
        return y <= .4 ? Action::Previous : y >= .6 ? Action::Next : Action::Menu;
    return x > .33 && x < .67 && y > .4 && y < .6 ? Action::Menu : Action::Next;
}
}
