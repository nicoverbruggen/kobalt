#pragma once
#include "strings.h"
#include <QLocale>

namespace kobalt::strings {
struct PercentParts
{
    QString before;
    QString number;
    QString after;
};

// One rule for numbers in interface text: the interface is English, so
// numbers use English formatting on every device. When the interface is
// translated, this locale follows the interface language.
inline QLocale interfaceLocale()
{
    return QLocale(QLocale::English, QLocale::UnitedStates);
}

// Display format for layouts that give the sign and number different type sizes.
inline PercentParts percentParts(qint64 value)
{
    auto locale = interfaceLocale();
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    return { {}, locale.toString(value), QString(locale.percent()) };
}

inline QString localizedPercent(qint64 value)
{
    const auto parts = percentParts(value);
    return parts.before + parts.number + parts.after;
}

// A percentage argument for a label. The caller supplies percentage points.
inline QString percentArgument(qint64 value)
{
    const auto parts = percentParts(value);
    return keyDisplay().load() ? parts.number : parts.before + parts.number + parts.after;
}

inline QString percentArgument(double value, int decimals)
{
    auto locale = interfaceLocale();
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    const auto number = locale.toString(value, 'f', decimals);
    return keyDisplay().load() ? number : number + QString(locale.percent());
}

// A count in interface text, grouped as the interface language groups it.
// Page numbers stay plain digits.
inline QString count(qint64 value)
{
    return interfaceLocale().toString(value);
}

inline QString percentage(qint64 value)
{
    return get("common.percent").arg(percentArgument(value));
}

inline QString percentage(double value, int decimals)
{
    return get("common.percent").arg(percentArgument(value, decimals));
}
} // namespace kobalt::strings
