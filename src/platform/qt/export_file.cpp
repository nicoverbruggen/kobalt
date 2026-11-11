#include "export_file.h"
#include "support/main_thread.h"
#include <QSaveFile>

namespace kobalt::platform {
bool writeLocalExport(const QUrl &destination, const QByteArray &archive)
{
    if (archive.isEmpty() || !destination.isValid() || !destination.isLocalFile())
        return false;
    mainthread::BlockingOperation operation("annotation-export-write");
    QSaveFile file(destination.toLocalFile());
    return file.open(QIODevice::WriteOnly) && file.write(archive) == archive.size()
            && file.commit();
}
} // namespace kobalt::platform
