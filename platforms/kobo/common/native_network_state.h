#pragma once
#include <QAbstractSocket>
#include <QHostAddress>
#include <QNetworkInterface>

namespace kobalt {
namespace nickel {
inline bool usableLocalNetwork()
{
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : interface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isNull()
                && !entry.ip().isLoopback()
                && (entry.ip().toIPv4Address() & 0xffff0000u) != 0xa9fe0000u)
                return true;
    }
    return false;
}
} // namespace nickel
} // namespace kobalt
