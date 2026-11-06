#pragma once
#include "opening_result.h"
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <QIODevice>
#include <QMap>
#include <QSet>

namespace kobalt {
// Lists archive members and checks decoded obfuscated fonts with FreeType.
// Other payloads come from the selected resource adapter.
class Publication
{
public:
    bool protectedBook() const { return protected_; }

    bool inspect(const QByteArray &source, QString *error, OpeningResult *result = nullptr);
    bool inspect(QIODevice &source, QString *error, OpeningResult *result = nullptr);

    const QMap<QString, qint64> &sizes() const { return sizes_; }

    const QStringList &names() const { return names_; }

    const QSet<QString> &skippedFonts() const { return skippedFonts_; }

    // SHA-256 of the file length, the ZIP directory and its end record. The
    // directory holds each member's CRC-32 and sizes, so an edit to any member
    // changes it without reading the whole book.
    const QByteArray &fingerprint() const { return fingerprint_; }

    bool append(const QString &name, const QByteArray &data, QString *error);
    QByteArray finish(QString *error);
    static constexpr int maxBytes = 64 * 1024 * 1024;
    static constexpr int maxResource = 16 * 1024 * 1024;

private:
    bool protected_ = false;
    QStringList names_;
    QMap<QString, qint64> sizes_;
    QSet<QString> skippedFonts_;
    QByteArray archive_, directory_, fingerprint_;
    int received_ = 0;
};
} // namespace kobalt
