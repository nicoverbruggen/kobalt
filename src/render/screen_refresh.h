#pragma once
#include <QString>

namespace kobalt {
// When a page turn flashes the whole screen to clear ghosting. Closing the
// menu with a tap on the page flashes in every mode; this covers turns only.
enum class ScreenRefresh { Smart, Chapter, Pages6, Pages10, Pages20, Manual, EveryPage };

// Smart flashes at a chapter start after at least this many turns since the
// last flash, so a run of short chapters does not flash on almost every turn.
inline constexpr int smartRefreshTurns = 10;

inline constexpr ScreenRefresh screenRefreshModes[] = {
    ScreenRefresh::Smart,   ScreenRefresh::EveryPage, ScreenRefresh::Chapter, ScreenRefresh::Pages6,
    ScreenRefresh::Pages10, ScreenRefresh::Pages20,   ScreenRefresh::Manual,
};

// The value saved in reader-settings.json.
inline QString screenRefreshValue(ScreenRefresh mode) {
    switch (mode) {
    case ScreenRefresh::Smart:
        return QStringLiteral("smart");
    case ScreenRefresh::EveryPage:
        return QStringLiteral("1");
    case ScreenRefresh::Chapter:
        return QStringLiteral("chapter");
    case ScreenRefresh::Pages6:
        return QStringLiteral("6");
    case ScreenRefresh::Pages10:
        return QStringLiteral("10");
    case ScreenRefresh::Pages20:
        return QStringLiteral("20");
    case ScreenRefresh::Manual:
        return QStringLiteral("manual");
    }
    return QStringLiteral("smart");
}

// A missing or unknown value is Smart, the default.
inline ScreenRefresh screenRefresh(const QString &value) {
    for (const auto mode : screenRefreshModes)
        if (screenRefreshValue(mode) == value)
            return mode;
    return ScreenRefresh::Smart;
}

// turnsSinceFlash counts this turn. chapterStart is a forward turn onto the
// first page of a chapter.
inline bool refreshOnTurn(ScreenRefresh mode, int turnsSinceFlash, bool chapterStart) {
    switch (mode) {
    case ScreenRefresh::Smart:
        return chapterStart && turnsSinceFlash >= smartRefreshTurns;
    case ScreenRefresh::EveryPage:
        return turnsSinceFlash >= 1;
    case ScreenRefresh::Chapter:
        return chapterStart;
    case ScreenRefresh::Pages6:
        return turnsSinceFlash >= 6;
    case ScreenRefresh::Pages10:
        return turnsSinceFlash >= 10;
    case ScreenRefresh::Pages20:
        return turnsSinceFlash >= 20;
    case ScreenRefresh::Manual:
        return false;
    }
    return false;
}
}
