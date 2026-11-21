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

namespace detail {
// Qt provides the digits, decimal separator and percent sign, but no percent pattern.
// These are the exceptions to the unspaced suffix used in CLDR 48.
inline PercentParts percentParts(const QLocale &locale, const QString &number)
{
    const auto language = locale.bcp47Name().section(QLatin1Char('-'), 0, 0);
    const auto country = locale.name().section(QLatin1Char('_'), 1, 1);
    const QString sign(locale.percent());
    if (language == "tr" || (language == "ku" && locale.script() == QLocale::LatinScript))
        return { sign, number, {} };
    if (language == "eu" || language == "blo")
        return { sign + QChar(0x00a0), number, {} };
    if (language == "oc")
        return { {}, number, QChar(0x202f) + sign };

    static const QStringList spacedLanguages =
            QStringLiteral(
                    "bas be br bua ca ce ckb cs cv da de dsb dua dz fi fo fr gl gsw hr hsb ht kl ksh "
                    "lb lt mk nb nds nn no qu rn ro ru se sk sl smn sv tk tt zgh")
                    .split(QLatin1Char(' '));
    static const QStringList spacedEnglish =
            QStringLiteral("AT DE DK FI NO SE").split(QLatin1Char(' '));
    static const QStringList spacedSpanish =
            QStringLiteral("EA ES GQ IC PH").split(QLatin1Char(' '));
    const bool swissFrenchOrGerman = country == "CH" && (language == "fr" || language == "de");
    const bool liechtensteinGerman = country == "LI" && language == "de";
    const bool spaced =
            (spacedLanguages.contains(language) && !swissFrenchOrGerman && !liechtensteinGerman)
            || (language == "en" && spacedEnglish.contains(country))
            || (language == "es" && spacedSpanish.contains(country));
    return { {}, number, spaced ? QChar(0x00a0) + sign : sign };
}
} // namespace detail

// Display format for layouts that give the sign and number different type sizes.
inline PercentParts percentParts(qint64 value, QLocale locale = QLocale())
{
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    return detail::percentParts(locale, locale.toString(value));
}

inline QString localizedPercent(qint64 value, QLocale locale = QLocale())
{
    const auto parts = percentParts(value, locale);
    return parts.before + parts.number + parts.after;
}

// A percentage argument for a label. The caller supplies percentage points.
inline QString percentArgument(qint64 value, QLocale locale = QLocale())
{
    const auto parts = percentParts(value, locale);
    return keyDisplay().load() ? parts.number : parts.before + parts.number + parts.after;
}

inline QString percentArgument(double value, int decimals)
{
    QLocale locale;
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    const auto number = locale.toString(value, 'f', decimals);
    if (keyDisplay().load())
        return number;
    const auto parts = detail::percentParts(locale, number);
    return parts.before + parts.number + parts.after;
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
