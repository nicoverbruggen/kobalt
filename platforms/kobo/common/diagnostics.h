#pragma once
#include "platform/posix/diagnostics.h"
#include <QFile>
#include <QRegularExpression>

namespace kobalt::nickel {
inline QJsonObject diagnosticDeviceFields(const QByteArray &version)
{
    const auto fields = version.trimmed().split(',');
    if (fields.size() != 6) {
        return {};
    }
    QJsonObject result;
    const auto firmware = QString::fromLatin1(fields[2].trimmed());
    const auto hardware = QString::fromLatin1(fields[5].trimmed());
    // Persist only model and firmware fields. The first field is the serial.
    static const QRegularExpression firmwarePattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+){2,}$"));
    static const QRegularExpression hardwarePattern(
            QStringLiteral("^00000000-0000-0000-0000-[0-9]{12}$"));
    if (firmwarePattern.match(firmware).hasMatch()) {
        result["firmware"] = firmware;
    }
    if (hardwarePattern.match(hardware).hasMatch()) {
        result["hardware_uuid"] = hardware;
    }
    return result;
}

inline DiagnosticSystem diagnosticSystem()
{
    auto system = platform::processDiagnosticSystem([](const QString &path) {
        const QLatin1String nickel("/mnt/onboard/.kobo/");
        if (path.startsWith("/mnt/onboard/.adds/")
            || (path.startsWith(nickel) && !path.mid(nickel.size()).contains('/')))
            return path;
        return platform::diagnosticDescriptorName(path);
    });
    system.startup = [] {
        QFile version("/mnt/onboard/.kobo/version");
        return version.open(QIODevice::ReadOnly) ? diagnosticDeviceFields(version.read(512))
                                                 : QJsonObject();
    };
    return system;
}
} // namespace kobalt::nickel
