#pragma once
#include <QRegularExpression>
#include <QString>

class QSettings;

namespace kobalt::nickel {
// Nickel imports every image and book it finds on user storage, also below
// `.adds`. Its `ExcludeSyncFolders` setting holds one regular expression for
// folder paths relative to the storage root. Kobalt adds its own folder to it,
// so wallpapers and PNGs in `.adds/kobalt` stay out of My Books.
inline constexpr auto kobaltFolderPattern = R"(\.adds/kobalt(?:/.*)?)";

// Whether `value` already keeps Kobalt's folder and its subfolders out.
inline bool excludesKobaltFolder(const QString &value)
{
    if (value.isEmpty())
        return false;
    const QRegularExpression expression(QStringLiteral("^(?:") + value + QStringLiteral(")$"));
    if (!expression.isValid())
        return false;
    for (const auto *path :
         { ".adds/kobalt", ".adds/kobalt/wallpapers", ".adds/kobalt/screensaver" })
        if (!expression.match(QString::fromLatin1(path)).hasMatch())
            return false;
    return true;
}

// The setting with Kobalt's folder added. A value that already excludes it,
// such as one that hides every dot folder, stays as it is. An invalid value
// is kept too: Nickel ignores it, and replacing it would lose what the reader
// meant to exclude.
inline QString withKobaltFolder(const QString &value)
{
    const QString own = QString::fromLatin1(kobaltFolderPattern);
    if (value.isEmpty())
        return own;
    if (excludesKobaltFolder(value)
        || !QRegularExpression(QStringLiteral("^(?:") + value + QStringLiteral(")$")).isValid())
        return value;
    return QStringLiteral("(?:") + value + QStringLiteral("|") + own + QStringLiteral(")");
}

// Adds or removes Kobalt's folder in Nickel's configuration file. Returns
// false when the file could not be written.
bool excludeKobaltFolder(QSettings &settings, bool exclude);
bool excludeKobaltFolder(bool exclude);
} // namespace kobalt::nickel
