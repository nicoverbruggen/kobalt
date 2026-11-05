#pragma once
#include "support/strings.h"
#include <QJsonObject>

namespace kobalt::remainingTime {
inline constexpr auto preferenceKey = "time_remaining_format";

inline bool approximate(const QJsonObject &preferences)
{
    return preferences.value(preferenceKey).toString() == "approximate";
}

inline QString approximateDuration(qint64 milliseconds)
{
    if (milliseconds <= 0)
        return {};
    constexpr qint64 hour = 3600000, quarter = 15 * 60000;
    const auto step = milliseconds < hour ? quarter : hour;
    const auto count = milliseconds / step + (milliseconds % step != 0);
    if (milliseconds < hour && count < 4)
        return strings::get("time_remaining.about_minutes").arg(count * 15);
    return count == 1 || milliseconds < hour
            ? strings::get("time_remaining.about_hour")
            : strings::get("time_remaining.about_hours").arg(count);
}
} // namespace kobalt::remainingTime
