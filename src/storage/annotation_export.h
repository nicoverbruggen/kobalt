#pragma once
#include "highlight_store.h"
#include <QByteArray>
#include <QUrl>

namespace kobalt {
enum class AnnotationExportFormat { Markdown, Html };
QString annotationExportFileName(const QString &identity, const QString &title,
                                 AnnotationExportFormat format);
// One UTF-8 Markdown or searchable HTML file in a compressed ZIP. Rows are in source order.
// Empty on invalid or incomplete payloads. Build before asking for a destination.
QByteArray annotationExportArchive(const QString &title, const QString &author,
                                   const QVector<Highlight> &rows, AnnotationExportFormat format,
                                   const QDateTime &exportedAt = QDateTime::currentDateTimeUtc(),
                                   const QByteArray &font = {});
// Local files use atomic replacement. Android document providers receive a direct write.
bool writeAnnotationExport(const QUrl &destination, const QByteArray &archive);
// Replaces this book's previous export atomically. Returns its path, or empty on failure.
QString exportAnnotations(const QString &directory, const QString &identity, const QString &title,
                          const QString &author, const QVector<Highlight> &rows,
                          AnnotationExportFormat format, const QByteArray &font = {});
} // namespace kobalt
