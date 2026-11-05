#pragma once
#include "platform/api/reader_host_services.h"
#include "storage/reader_storage.h"
#include "support/strings.h"
#include <QStringList>

namespace kobalt {
// What the reader tells the user after profile preparation reset unusable
// saved data. `folder` is the backups folder as the user can find it.
inline ReaderNotice profileRecoveryNotice(const storage::ProfileRecovery &recovery, const QString &folder) {
    QStringList parts;
    if (recovery.settings)
        parts.append(strings::get("recovery.settings"));
    if (recovery.syncAccount)
        parts.append(strings::get("recovery.sync_account"));
    if (recovery.database)
        parts.append(strings::get("recovery.database"));
    parts.append(strings::get("recovery.backups").arg(folder));
    return {strings::get("recovery.title"), parts.join(QStringLiteral("\n\n")), QStringLiteral("rrRecoveryNotice")};
}
}
