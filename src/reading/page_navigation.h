#pragma once

#include "ui/tap_zones.h"
#include <QJsonObject>

namespace kobalt::pageNavigation {
// Compare press times, not delivery times: a slow paint can queue both taps.
// Rejected taps do not extend the window after the last accepted navigation.
class TapFilter {
public:
    bool accept(quint32 sourceTime, qint64 deliveredTime) {
        if (seen) {
            const auto elapsed = sourceTime && previousSource
                ? qint64(quint32(sourceTime - previousSource)) : deliveredTime - previousDelivery;
            if (elapsed < 200 || (sourceTime && previousSource && elapsed >= 0x80000000LL))
                return false;
        }
        seen = true;
        previousSource = sourceTime;
        previousDelivery = deliveredTime;
        return true;
    }
private:
    bool seen = false;
    quint32 previousSource = 0;
    qint64 previousDelivery = 0;
};
enum class BarButtons { Pages, Chapters };
inline constexpr BarButtons barButtonsModes[] = {BarButtons::Chapters, BarButtons::Pages};
inline constexpr auto barButtonsKey = "navigation_bar_buttons";
inline constexpr auto barVisibleKey = "show_navigation_bar";
inline constexpr auto thumbnailsKey = "show_navigation_thumbnails";
enum class MenuView { None, Seek, Stats };
inline constexpr MenuView menuViews[] = {MenuView::None, MenuView::Seek, MenuView::Stats};
inline constexpr auto menuViewKey = "default_menu_view";
inline QString value(MenuView mode) {
    switch (mode) {
    case MenuView::Seek:
        return QStringLiteral("seek");
    case MenuView::Stats:
        return QStringLiteral("stats");
    default:
        return QStringLiteral("none");
    }
}
inline MenuView menuView(const QJsonObject &preferences) {
    if (preferences.contains(menuViewKey)) {
        for (const auto view : menuViews) {
            if (preferences.value(menuViewKey) == value(view)) {
                return view;
            }
        }
        return MenuView::None;
    }
    // Explicit choices from older profiles keep their intent. New profiles start with None.
    if (preferences.value("pin_stats").toBool(false)) {
        return MenuView::Stats;
    }
    return preferences.value(thumbnailsKey).toBool(false) ? MenuView::Seek : MenuView::None;
}
inline QString value(BarButtons mode) {
    return mode == BarButtons::Chapters ? QStringLiteral("chapters") : QStringLiteral("pages");
}
inline BarButtons barButtons(const QJsonObject &preferences) {
    return preferences.value(barButtonsKey) == value(BarButtons::Pages) ? BarButtons::Pages : BarButtons::Chapters;
}
enum class Mode { TapsAndSwipes, TapsOnly, SwipesOnly, ButtonsOnly };
inline constexpr Mode modes[] = {Mode::TapsAndSwipes, Mode::TapsOnly, Mode::SwipesOnly, Mode::ButtonsOnly};
inline constexpr auto preferenceKey = "page_navigation";
inline QString value(Mode mode) {
    switch (mode) {
    case Mode::TapsAndSwipes:
        return QStringLiteral("taps_and_swipes");
    case Mode::TapsOnly:
        return QStringLiteral("taps_only");
    case Mode::SwipesOnly:
        return QStringLiteral("swipes_only");
    case Mode::ButtonsOnly:
        return QStringLiteral("buttons_only");
    }
    return {};
}
inline constexpr auto tapPreferenceKey = "tap_zones";
inline QString tapValue(tapZones::Layout layout) {
    switch (layout) {
    case tapZones::Layout::LeftRight:
        return QStringLiteral("left_right");
    case tapZones::Layout::Reversed:
        return QStringLiteral("reversed");
    case tapZones::Layout::TopBottom:
        return QStringLiteral("top_bottom");
    case tapZones::Layout::OneHanded:
        return QStringLiteral("one_handed");
    }
    return {};
}
inline tapZones::Layout tapLayout(const QJsonObject &preferences) {
    for (const auto layout : tapZones::layouts)
        if (preferences.value(tapPreferenceKey) == tapValue(layout))
            return layout;
    return tapZones::Layout::LeftRight;
}
// A profile can move to a host without page buttons. Keep touch navigation usable there.
inline Mode mode(const QJsonObject &preferences, bool pageButtons) {
    const auto saved = preferences.value(preferenceKey);
    for (const auto choice : modes) {
        if (saved == value(choice) && (choice != Mode::ButtonsOnly || pageButtons)) {
            if (tapLayout(preferences) == tapZones::Layout::OneHanded &&
                (choice == Mode::TapsOnly || choice == Mode::ButtonsOnly))
                return Mode::TapsAndSwipes;
            return choice;
        }
    }
    return Mode::TapsAndSwipes;
}
inline bool taps(Mode mode) {
    return mode == Mode::TapsAndSwipes || mode == Mode::TapsOnly;
}
inline bool swipes(Mode mode) {
    return mode == Mode::TapsAndSwipes || mode == Mode::SwipesOnly;
}
}
