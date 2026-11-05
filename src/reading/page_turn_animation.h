#pragma once
#include <QJsonObject>

namespace kobalt::pageTurn {
enum class Style { Off, Sweep };
inline constexpr auto preferenceKey = "page_turn_animation";

inline Style parse(const QString &value)
{
    // Preserve enabled animations from the earlier Android style picker.
    return value == "sweep" || value == "curl" || value == "fade" ? Style::Sweep : Style::Off;
}

inline Style selected(const QJsonObject &preferences)
{
    return parse(preferences.value(preferenceKey).toString());
}

inline QString value(Style style)
{
    return style == Style::Sweep ? QStringLiteral("sweep") : QStringLiteral("off");
}
} // namespace kobalt::pageTurn
