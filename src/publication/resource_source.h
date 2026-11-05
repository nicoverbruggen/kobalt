#pragma once
#include "resource_cache.h"
#include <QMap>

namespace kobalt {
// Construct bounded resource access on the engine worker. The factory owns
// its source values and access lifetime, never borrowed platform objects.
using ResourceFactory = std::function<std::shared_ptr<ResourceCache>(
        const QMap<QString, qint64> &, const QString &, std::shared_ptr<std::atomic_bool>)>;
} // namespace kobalt
