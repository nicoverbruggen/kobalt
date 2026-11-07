#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <Qt>

namespace kobalt::pageButtons {
// IDs describe physical controls, independent of rotation and native direction.
struct Button
{
    QString id, labelKey;
    int key = 0;
    int defaultDirection = 1;
};

using Buttons = QVector<Button>;
inline constexpr auto preferenceKey = "button_actions";

inline Buttons paging()
{
    return { { "page_upper", "buttons.upper", Qt::Key_F23, -1 },
             { "page_lower", "buttons.lower", Qt::Key_F24, 1 } };
}

inline Buttons volume(int available)
{
    Buttons result;
    if (available & 1)
        result.append({ "volume_up", "buttons.volume_up", Qt::Key_VolumeUp, -1 });
    if (available & 2)
        result.append({ "volume_down", "buttons.volume_down", Qt::Key_VolumeDown, 1 });
    return result;
}

inline int direction(const QJsonObject &preferences, const QString &id, int nativeDirection)
{
    const auto action = preferences.value(preferenceKey).toObject().value(id).toString();
    if (action == "previous")
        return -1;
    if (action == "next")
        return 1;
    if (action == "none")
        return 0;
    // Missing and invalid values retain the host's rotation and inversion policy.
    return nativeDirection;
}

inline QString action(int direction)
{
    return direction < 0    ? QStringLiteral("previous")
            : direction > 0 ? QStringLiteral("next")
                            : QStringLiteral("none");
}
} // namespace kobalt::pageButtons
