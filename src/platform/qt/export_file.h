#pragma once
#include <QByteArray>
#include <QUrl>

namespace kobalt::platform {
// Atomic replacement of a local destination. Other URL schemes are unsupported.
bool writeLocalExport(const QUrl &destination, const QByteArray &archive);
} // namespace kobalt::platform
