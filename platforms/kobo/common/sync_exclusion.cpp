#include "sync_exclusion.h"
#include <QSettings>

namespace kobalt::nickel {
bool excludeKobaltFolder(bool exclude)
{
    // Nickel reads this file through QSettings too. Settings objects for one
    // file share their contents within the process, so Nickel sees the change.
    QSettings settings(QStringLiteral("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf"),
                       QSettings::IniFormat);
    return excludeKobaltFolder(settings, exclude);
}

bool excludeKobaltFolder(QSettings &settings, bool exclude)
{
    const auto key = QStringLiteral("FeatureSettings/ExcludeSyncFolders");
    const auto receipt = QStringLiteral("Kobalt/InstalledSyncExclusion");
    const auto current = settings.value(key).toString();
    const auto installed = settings.value(receipt).toStringList();
    if (exclude) {
        const auto next = withKobaltFolder(current);
        if (next != current) {
            QStringList values{ next };
            if (settings.contains(key))
                values.append(current);
            settings.setValue(receipt, values);
            settings.setValue(key, next);
        } else if (!installed.isEmpty() && installed.first() != current) {
            settings.remove(receipt);
        }
    } else if (!installed.isEmpty()) {
        if (current == installed.first()) {
            if (installed.size() == 2)
                settings.setValue(key, installed.last());
            else if (installed.size() == 1)
                settings.remove(key);
        }
        settings.remove(receipt);
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}
} // namespace kobalt::nickel
