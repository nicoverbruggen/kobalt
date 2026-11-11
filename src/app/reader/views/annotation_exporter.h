#pragma once
#include "platform/api/annotation_export.h"
#include "platform/qt/export_file.h"
#include <QUrl>

namespace kobalt::reader {
// Writers run on the storage worker. Android supplies its document-provider
// writer; desktop and Kobo use atomic local-file replacement.
using ExportWriter = std::function<bool(const QUrl &, const QByteArray &)>;
std::unique_ptr<AnnotationExporter>
directoryAnnotationExporter(QWidget *reader, QString directory,
                            ExportWriter write = platform::writeLocalExport);
std::unique_ptr<AnnotationExporter>
pickerAnnotationExporter(QWidget *reader, QWidget *nativeParent,
                         ExportWriter write = platform::writeLocalExport);
} // namespace kobalt::reader
