#pragma once
#include <QJsonObject>
#include "ui/controls/icons.h"

namespace kobalt::reader {
struct InterfaceStyle
{
    QString mainFace;
    ui::IconStyle icons = ui::IconStyle::Monochrome;
    QString palette = QStringLiteral("grayscale");
};

inline InterfaceStyle themePreset(const QString &id)
{
    InterfaceStyle style;
    if (id == "playful") {
        style.mainFace = QStringLiteral("Kalam");
        style.icons = ui::IconStyle::Pastel;
        style.palette = QStringLiteral("blue");
    } else if (id == "notebook") {
        style.mainFace = QStringLiteral("Playpen Sans");
        style.icons = ui::IconStyle::Color;
        style.palette = QStringLiteral("blue");
    }
    return style;
}

inline QString savedThemeId(const QJsonObject &preferences)
{
    const auto id = preferences.value("ui_theme").toString();
    if (id == "classic" || id == "playful" || id == "notebook" || id == "custom")
        return id;
    if (preferences.value("playful_theme").toBool())
        return QStringLiteral("playful");
    // The prototype's font override and colorful icons are custom choices.
    return preferences.value("ui_font_face").toString().isEmpty()
                    && !preferences.value("ui_colorful_icons").toBool()
            ? QStringLiteral("classic")
            : QStringLiteral("custom");
}

inline InterfaceStyle customInterfaceStyle(const QJsonObject &preferences)
{
    InterfaceStyle style;
    style.mainFace = preferences.value("ui_font_face").toString();
    style.icons = ui::iconStyleFromId(
            preferences.value("ui_icon_style")
                    .toString(preferences.value("ui_colorful_icons").toBool() ? "pastel"
                                                                              : "monochrome"));
    style.palette = preferences.value("ui_palette").toString("grayscale");
    if (style.palette == "plum")
        style.palette = QStringLiteral("grayscale");
    return style;
}

inline InterfaceStyle savedInterfaceStyle(const QJsonObject &preferences)
{
    const auto id = savedThemeId(preferences);
    return id == "custom" ? customInterfaceStyle(preferences) : themePreset(id);
}

inline QString savedPalette(const QJsonObject &preferences)
{
    return savedInterfaceStyle(preferences).palette;
}
} // namespace kobalt::reader
