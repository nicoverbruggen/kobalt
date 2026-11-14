#include "sync_exclusion.h"
#include <QSettings>

namespace kobalt::nickel {
bool excludeKobaltFolder(bool exclude)
{
    // Nickel reads this file through QSettings too. Settings objects for one
    // file share their contents within the process, so Nickel sees the change.
    QSettings settings(QStringLiteral("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf"),
                       QSettings::IniFormat);
    const auto key = QStringLiteral("FeatureSettings/ExcludeSyncFolders");
    const auto current = settings.value(key).toString();
    const auto next = exclude ? withKobaltFolder(current) : withoutKobaltFolder(current);
    if (next == current)
        return true;
    if (next.isEmpty())
        settings.remove(key);
    else
        settings.setValue(key, next);
    settings.sync();
    return settings.status() == QSettings::NoError;
}
} // namespace kobalt::nickel
