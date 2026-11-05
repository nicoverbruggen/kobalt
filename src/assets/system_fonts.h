#pragma once

#include <QByteArray>
#include <QStringList>

namespace kobalt {
class FontCopies;

// Plain fonts retain their original paths. Kobo's wrapped fonts get private
// copies that must outlive both the engine and Qt's registered font faces.
// Encrypted fonts are skipped.
QStringList systemFontFiles(const QString &directory, FontCopies &copies);
} // namespace kobalt
