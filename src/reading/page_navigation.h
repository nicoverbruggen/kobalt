#pragma once

#include "ui/tap_zones.h"
#include <QJsonObject>

namespace kobalt::pageNavigation {
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
