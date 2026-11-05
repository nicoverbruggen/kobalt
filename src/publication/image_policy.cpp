#include "image_policy.h"
#include "support/strings.h"
#include <QtEndian>
#include <cstring>

namespace kobalt {
namespace {
ImageHeader dimensions(quint32 width, quint32 height)
{
    const bool valid = width && height && width <= 32768 && height <= 32768;
    const bool fits = valid && quint64(width) * height <= imagePixelLimit;
    // Keep layout dimensions representable even for an invalid PNG header.
    const QSize size = width <= 32768 && height <= 32768 ? QSize(int(width), int(height)) : QSize();
    return { fits ? ImageHeader::Keep : ImageHeader::Replace, size };
}
} // namespace

ImageHeader inspectImageHeader(const QByteArray &data)
{
    const auto *bytes = reinterpret_cast<const uchar *>(data.data());
    if (data.size() < 8)
        return {};
    if (std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0) {
        if (data.size() < 33)
            return {};
        if (qFromBigEndian<quint32>(bytes + 8) != 13 || std::memcmp(bytes + 12, "IHDR", 4) != 0)
            return { ImageHeader::Replace, {} };
        return dimensions(qFromBigEndian<quint32>(bytes + 16), qFromBigEndian<quint32>(bytes + 20));
    }
    if (std::memcmp(bytes, "GIF87a", 6) == 0 || std::memcmp(bytes, "GIF89a", 6) == 0) {
        if (data.size() < 10)
            return {};
        return dimensions(qFromLittleEndian<quint16>(bytes + 6),
                          qFromLittleEndian<quint16>(bytes + 8));
    }
    if (bytes[0] == 0xff && bytes[1] == 0xd8) {
        qsizetype at = 2;
        while (at < data.size()) {
            if (bytes[at++] != 0xff)
                return { ImageHeader::Replace, {} };
            while (at < data.size() && bytes[at] == 0xff)
                ++at;
            if (at == data.size())
                break;
            const auto marker = bytes[at++];
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
                continue;
            if (marker == 0xda || marker == 0xd9 || marker == 0xd8 || marker == 0)
                return { ImageHeader::Replace, {} };
            if (data.size() - at < 2)
                break;
            const auto length = qFromBigEndian<quint16>(bytes + at);
            if (length < 2)
                return { ImageHeader::Replace, {} };
            const bool frame = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8
                    && marker != 0xcc;
            if (frame) {
                if (length < 8)
                    return { ImageHeader::Replace, {} };
                if (data.size() - at < 7)
                    break;
                return dimensions(qFromBigEndian<quint16>(bytes + at + 5),
                                  qFromBigEndian<quint16>(bytes + at + 3));
            }
            if (data.size() - at < length)
                break;
            at += length;
        }
        return { data.size() >= imageHeaderLimit ? ImageHeader::Replace : ImageHeader::More, {} };
    }
    return { ImageHeader::Keep, {} };
}

bool imageFileName(const QString &name)
{
    const auto suffix = name.mid(name.lastIndexOf('.') + 1).toLower();
    return suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "gif"
            || suffix == "svg";
}

QByteArray imagePlaceholder(QSize original, const QString &systemFont)
{
    if (original.width() <= 0 || original.height() <= 0)
        original = QSize(600, 450);
    const double height = 600.0 * original.height() / original.width();
    const double border = qMin(3.0, height / 20);
    const double font = qMin(32.0, height / 4);
    return QString("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1\" height=\"%2\" viewBox=\"0 0 600 %3\">"
                   "<rect x=\"%4\" y=\"%4\" width=\"%5\" height=\"%6\" fill=\"#eeeeee\" stroke=\"black\" stroke-width=\"%7\"/>"
                   "<text x=\"300\" y=\"%8\" text-anchor=\"middle\" font-family=\"%10\" font-size=\"%9\" fill=\"black\">%11</text>"
                   "</svg>")
            .arg(original.width())
            .arg(original.height())
            .arg(height)
            .arg(border / 2)
            .arg(600 - border)
            .arg(height - border)
            .arg(border)
            .arg(height / 2 + font / 3)
            .arg(font)
            .arg(systemFont.toHtmlEscaped())
            .arg(strings::get("image.unavailable").toHtmlEscaped())
            .toUtf8();
}
} // namespace kobalt
