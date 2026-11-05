#include "system_fonts.h"

#include "assets.h"

#include <QDir>
#include <QFile>
#include <QtEndian>

#include <zlib.h>

namespace kobalt {
namespace {
constexpr auto maximumFontBytes = 32 * 1024 * 1024;

QByteArray decompressFont(const QByteArray &bytes)
{
    if (bytes.size() <= 7) {
        return {};
    }
    const auto size =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 3));
    if (!size || size > maximumFontBytes) {
        return {};
    }
    QByteArray result(int(size), '\0');
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.constData() + 7));
    stream.avail_in = uInt(bytes.size() - 7);
    stream.next_out = reinterpret_cast<Bytef *>(result.data());
    stream.avail_out = size;
    if (inflateInit(&stream) != Z_OK) {
        return {};
    }
    const bool valid = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.total_out == size
            && !stream.avail_in;
    inflateEnd(&stream);
    return valid ? result : QByteArray();
}
} // namespace

QStringList systemFontFiles(const QString &directory, FontCopies &copies)
{
    if (directory.isEmpty()) {
        return {};
    }
    QStringList result;
    const QDir source(directory);
    for (const auto &name :
         source.entryList({ "*.ttf", "*.otf", "*.ttc" }, QDir::Files, QDir::Name)) {
        const auto path = source.filePath(name);
        QFile font(path);
        if (!font.open(QIODevice::ReadOnly) || font.size() <= 0 || font.size() > maximumFontBytes) {
            continue;
        }
        const auto header = font.peek(3);
        if (header == "QTE") {
            continue;
        }
        if (header != "QTD") {
            result.append(path);
            continue;
        }
        const auto decoded = decompressFont(font.readAll());
        if (decoded.isEmpty()) {
            continue;
        }
        const auto copy = copies.file("system/" + name, decoded);
        if (!copy.isEmpty()) {
            result.append(copy);
        }
    }
    return result;
}
} // namespace kobalt
