#pragma once
#include "resource_source.h"
#include "source_lease.h"
#include <QMap>

namespace kobalt {
// Initialize once on the GUI thread, after the engine worker.
void initializeResourceNetwork();
void releaseResourceConnections(std::function<void()> finished);

// Construct and use resources on the engine worker. The host keeps its GUI
// endpoint alive through the drain barrier. Each opening gets a fresh lease;
// neither its port nor its cancellation state can be reused for another open.
std::shared_ptr<ResourceCache>
nativeResources(QMap<QString, qint64> sizes, const QString &path, int port,
                std::shared_ptr<SourceLease> lease, const QString &systemFont = "sans-serif",
                std::shared_ptr<std::atomic_bool> contentRejected = {});
// Prepare on the GUI thread, after constructing the engine session.
ResourceFactory nativeResourceFactory(QString path, int port, std::shared_ptr<SourceLease> lease);

// The loopback resource server accepts only requests that carry this secret,
// so no other process on the device can read decrypted book content from it.
// It lives only in this process's memory, for the life of the process. It is
// empty when the random source is unavailable, and then every request fails.
inline constexpr auto resourceSecretHeader = "x-kobalt-secret";
QByteArray resourceSecret();
bool resourceSecretMatches(const QByteArray &candidate);
} // namespace kobalt
