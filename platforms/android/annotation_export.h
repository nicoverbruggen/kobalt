#pragma once
#include <QByteArray>
#include <QUrl>

namespace kobalt::android {
bool writeAnnotationExport(const QUrl &destination, const QByteArray &archive);
} // namespace kobalt::android
