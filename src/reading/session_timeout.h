#pragma once
#include <QJsonObject>
#include <array>

namespace kobalt::sessionTimeout {
inline constexpr const char *preferenceKey = "session_timeout_minutes";
inline constexpr std::array<int, 6> choices = { 15, 60, 240, 360, 720, 0 };
inline constexpr int defaultMinutes = 15;

inline bool valid(const QJsonValue &value)
{
    if (!value.isDouble())
        return false;
    for (const auto minutes : choices)
        if (value.toDouble() == minutes)
            return true;
    return false;
}

inline int minutes(const QJsonObject &preferences)
{
    const auto value = preferences.value(preferenceKey);
    return valid(value) ? value.toInt() : defaultMinutes;
}

inline qint64 milliseconds(const QJsonObject &preferences)
{
    return qint64(minutes(preferences)) * 60000;
}
} // namespace kobalt::sessionTimeout
