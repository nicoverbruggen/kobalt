#pragma once
#include "platform/api/annotation_export.h"
#include "export_file.h"
#include <QUrl>

namespace kobalt::platform {
// Writers run on the storage worker. Android supplies its document-provider
// writer; desktop and Kobo use atomic local-file replacement.
using ExportWriter = std::function<bool(const QUrl &, const QByteArray &)>;
std::unique_ptr<AnnotationExporter>
directoryAnnotationExporter(QWidget *reader, QString directory,
                            ExportWriter write = writeLocalExport);
std::unique_ptr<AnnotationExporter> pickerAnnotationExporter(QWidget *reader, QWidget *nativeParent,
                                                             ExportWriter write = writeLocalExport);
} // namespace kobalt::platform
