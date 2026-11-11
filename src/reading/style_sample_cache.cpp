#include "style_sample_cache.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <utility>

namespace kobalt {
namespace {
qint64 imageBytes(const QImage &image)
{
    return qint64(image.bytesPerLine()) * image.height();
}

void keepProperties(const QImage &from, QImage &to)
{
    to.setDevicePixelRatio(from.devicePixelRatio());
    to.setDotsPerMeterX(from.dotsPerMeterX());
    to.setDotsPerMeterY(from.dotsPerMeterY());
    for (const auto &key : from.textKeys())
        to.setText(key, from.text(key));
}

// Indexed8 marks a packed page: the renderer never produces that format.
QImage packed(const QImage &image)
{
    if (image.format() != QImage::Format_RGB32 || !image.allGray())
        return image;
    QImage grey(image.size(), QImage::Format_Indexed8);
    if (grey.isNull())
        return image;
    QVector<QRgb> table(256);
    for (int level = 0; level < 256; ++level)
        table[level] = qRgb(level, level, level);
    grey.setColorTable(table);
    for (int y = 0; y < image.height(); ++y) {
        const auto *from = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        auto *to = grey.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
            to[x] = uchar(qRed(from[x]));
    }
    keepProperties(image, grey);
    return grey;
}

QImage unpacked(const QImage &image)
{
    if (image.format() != QImage::Format_Indexed8)
        return image;
    QImage page(image.size(), QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        const auto *from = image.constScanLine(y);
        auto *to = reinterpret_cast<QRgb *>(page.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            to[x] = qRgb(from[x], from[x], from[x]);
    }
    keepProperties(image, page);
    return page;
}

template <class Convert>
qint64 convert(StyleSample &sample, Convert change)
{
    qint64 bytes = 0;
    const auto each = [&](QImage &image) {
        image = change(image);
        bytes += imageBytes(image);
    };
    for (auto &spread : sample.spreads)
        for (auto &image : spread)
            each(image);
    for (auto &image : sample.crops)
        each(image);
    for (auto &image : sample.details)
        each(image);
    return bytes;
}
} // namespace

QByteArray styleSampleKey(const EngineFontInputs &fonts, const StyleSampleRequest &request)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer);
    // The same bytes on every Qt version are not needed: the key lives only
    // as long as the reader that made it.
    QJsonArray values;
    for (const auto &value : request.values)
        values.append(value);
    stream << QByteArray(KOBALT_RENDERING_POLICY)
           << QJsonDocument(request.settings).toJson(QJsonDocument::Compact) << request.key
           << QJsonDocument(values).toJson(QJsonDocument::Compact) << request.page
           << request.thumbnail << request.crop << request.cropOrigin << request.marginGuides
           << double(request.cropRatio) << request.readingArea << request.detail
           << request.boldHeading << request.firstPageOnly << fonts.sources.bundled
           << fonts.sources.systemDirectory << fonts.sources.sideloadDirectory
           << fonts.sources.systemFiles << fonts.fallbackFiles << fonts.hyphenationDirectory;
    buffer.close();
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool StyleSampleCache::find(bool landscape, const QByteArray &key, StyleSample *sample)
{
    auto &list = entries[landscape];
    for (int index = 0; index < list.size(); ++index) {
        if (list[index].key != key)
            continue;
        list.move(index, 0);
        *sample = list.first().sample;
        convert(*sample, unpacked);
        return true;
    }
    return false;
}

void StyleSampleCache::insert(bool landscape, const QByteArray &key, const StyleSample &sample)
{
    if (!sample.valid())
        return;
    auto &list = entries[landscape];
    for (int index = 0; index < list.size(); ++index)
        if (list[index].key == key) {
            list.removeAt(index);
            break;
        }
    Entry entry{ key, sample, 0 };
    entry.bytes = convert(entry.sample, packed);
    if (entry.bytes > limit)
        return;
    list.prepend(std::move(entry));
    while (bytes(landscape) > limit)
        list.removeLast();
}

void StyleSampleCache::clear()
{
    entries[0].clear();
    entries[1].clear();
}

qint64 StyleSampleCache::bytes(bool landscape) const
{
    qint64 total = 0;
    for (const auto &entry : entries[landscape])
        total += entry.bytes;
    return total;
}
} // namespace kobalt
