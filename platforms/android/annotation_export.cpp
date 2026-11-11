#include "annotation_export.h"
#include "platform/qt/export_file.h"
#include "support/main_thread.h"
#include <QFile>

namespace kobalt::android {
bool writeAnnotationExport(const QUrl &destination, const QByteArray &archive)
{
    if (destination.isLocalFile())
        return platform::writeLocalExport(destination, archive);
    if (archive.isEmpty() || !destination.isValid() || destination.scheme() != "content")
        return false;
    mainthread::BlockingOperation operation("annotation-export-write");
    QFile file(destination.toString(QUrl::FullyEncoded));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool written = file.write(archive) == archive.size() && file.flush();
    file.close();
    return written && file.error() == QFileDevice::NoError;
}
} // namespace kobalt::android
