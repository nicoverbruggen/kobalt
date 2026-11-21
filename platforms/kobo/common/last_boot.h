#pragma once
#include "storage/reader_storage.h"
#include <QDir>

namespace kobalt::nickel {
inline QString lastBootPath(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral("last-boot.json"));
}

inline storage::JsonRead readLastBoot(const QString &directory)
{
    auto result = storage::readJsonFile(lastBootPath(directory));
    if (result.status == storage::ReadStatus::Ok
        && (result.object.value("kobalt_version").toString().isEmpty()
            || !result.object.value("kobo_firmware_version").isString()))
        return { {}, storage::ReadStatus::Invalid };
    return result;
}

inline bool recordLastBoot(const QString &directory, const QString &kobaltVersion,
                           const QString &firmwareVersion)
{
    const auto previous = readLastBoot(directory);
    if (kobaltVersion.isEmpty()
        || (previous.status != storage::ReadStatus::Missing
            && previous.status != storage::ReadStatus::Ok))
        return false;

    auto next = previous.object;
    next["kobalt_version"] = kobaltVersion;
    next["kobo_firmware_version"] = firmwareVersion;
    return next == previous.object || storage::writeJson(lastBootPath(directory), next);
}
} // namespace kobalt::nickel
