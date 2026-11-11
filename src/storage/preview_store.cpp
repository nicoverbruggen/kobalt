#include "preview_store.h"
#include "sleep_snapshot.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonObject>
#include <QPointer>
#include <QRunnable>
#include <QSaveFile>
#include <QThreadPool>
#include <algorithm>
#include <utility>

namespace kobalt::storage {
namespace {
// Bump when the file contents change incompatibly. Older files then never
// match, and the size limit removes them.
constexpr auto formatVersion = "kobalt-preview-1";
constexpr qint64 maxFileBytes = 1024 * 1024;
constexpr qint64 maxPixels = 2 * 1024 * 1024;

QString fileName(const QByteArray &key)
{
    return QString::fromLatin1(QCryptographicHash::hash(QByteArray(formatVersion) + '\0' + key,
                                                        QCryptographicHash::Sha256)
                                       .toHex())
            + QStringLiteral(".png");
}

qint64 imageBytes(const QImage &image)
{
    return qint64(image.bytesPerLine()) * image.height();
}

// A tile that is grey in every pixel, with one byte per pixel. Other tiles
// stay as they are. The tiles are opaque.
QImage packed(const QImage &image)
{
    if (image.format() != QImage::Format_ARGB32_Premultiplied
        && image.format() != QImage::Format_RGB32)
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
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = from[x];
            if (qAlpha(pixel) != 255 || qRed(pixel) != qGreen(pixel) || qRed(pixel) != qBlue(pixel))
                return image;
            to[x] = uchar(qRed(pixel));
        }
    }
    return grey;
}

QImage unpacked(const QImage &image)
{
    if (image.format() != QImage::Format_Indexed8)
        return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto table = image.colorTable();
    QImage tile(image.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        const auto *from = image.constScanLine(y);
        auto *to = reinterpret_cast<QRgb *>(tile.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            to[x] = from[x] < table.size() ? (table[from[x]] | 0xff000000u) : 0xff000000u;
    }
    return tile;
}

// One worker for every store, so reads and writes keep their order. A
// reader that closes never waits for it; the application joins it on exit.
QThreadPool &storageWorker()
{
    struct Pool
    {
        QThreadPool threads;

        Pool()
        {
            threads.setMaxThreadCount(1);
            threads.setExpiryTimeout(-1);
            QObject::connect(
                    QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &threads,
                    [this] { threads.waitForDone(); }, Qt::DirectConnection);
            QObject::connect(
                    QCoreApplication::instance(), &QObject::destroyed, &threads,
                    [this] { threads.waitForDone(); }, Qt::DirectConnection);
        }
    };

    static Pool pool;
    return pool.threads;
}

class Job final : public QRunnable
{
public:
    explicit Job(std::function<void()> work) : work(std::move(work)) { }

    void run() override { work(); }

private:
    std::function<void()> work;
};

void runLater(std::function<void()> work)
{
    storageWorker().start(new Job(std::move(work)));
}
} // namespace

bool writePreview(const QString &directory, const QByteArray &key, const QImage &image,
                  qint64 limit)
{
    mainthread::BlockingOperation blockingOperation("preview-write");
    if (image.isNull() || qint64(image.width()) * image.height() > maxPixels
        || !QDir().mkpath(directory))
        return false;
    const auto stored = packed(image);
    QSaveFile file(QDir(directory).filePath(fileName(key)));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QImageWriter writer(&file, "png");
    QJsonObject codec;
    if (!configureSleepSnapshotCodec(writer,
                                     writer.supportsOption(QImageIOHandler::CompressionRatio),
                                     writer.supportsOption(QImageIOHandler::Quality), codec)
        || !writer.write(stored) || file.size() > maxFileBytes || !file.commit())
        return false;
    // The files written longest ago leave first.
    auto files = QDir(directory).entryInfoList({ QStringLiteral("*.png") }, QDir::Files,
                                               QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const auto &info : files)
        total += info.size();
    // Storage keeps times to two seconds on FAT, so a tie can name the new
    // file as old. It always stays.
    const auto written = QFileInfo(file.fileName()).fileName();
    for (const auto &info : files) {
        if (total <= limit)
            break;
        if (info.fileName() != written && QFile::remove(info.absoluteFilePath()))
            total -= info.size();
    }
    return true;
}

QImage readPreview(const QString &directory, const QByteArray &key)
{
    QFile file(QDir(directory).filePath(fileName(key)));
    if (!file.exists() || file.size() > maxFileBytes || !file.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&file, "png");
    const auto size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > maxPixels)
        return {};
    return reader.read();
}

PreviewStore::PreviewStore(QString directory)
    : directory(std::move(directory)), memory(std::make_shared<Memory>())
{
}

void PreviewStore::Memory::keep(const QByteArray &key, const QImage &image)
{
    for (int index = 0; index < entries.size(); ++index)
        if (entries[index].key == key) {
            entries.removeAt(index);
            break;
        }
    const auto stored = packed(image);
    const qint64 bytes = imageBytes(stored);
    if (stored.isNull() || bytes > memoryLimit)
        return;
    entries.prepend({ key, stored, bytes });
    qint64 total = 0;
    for (int index = 0; index < entries.size(); ++index) {
        total += entries[index].bytes;
        if (total > memoryLimit) {
            entries.erase(entries.begin() + index, entries.end());
            break;
        }
    }
}

bool PreviewStore::find(const QByteArray &key, QImage *image)
{
    auto &entries = memory->entries;
    for (int index = 0; index < entries.size(); ++index) {
        if (entries[index].key != key)
            continue;
        entries.move(index, 0);
        *image = unpacked(entries.first().image);
        return true;
    }
    return false;
}

void PreviewStore::load(const QVector<QByteArray> &keys, QObject *context,
                        std::function<void()> done)
{
    QVector<QByteArray> missing;
    for (const auto &key : keys) {
        bool held = false;
        for (const auto &entry : memory->entries)
            held = held || entry.key == key;
        if (!held)
            missing.append(key);
    }
    QPointer<QObject> guard(context);
    std::weak_ptr<Memory> weak = memory;
    const auto deliver = [guard, weak, done](QVector<QPair<QByteArray, QImage>> found) {
        if (auto kept = weak.lock())
            for (const auto &tile : found)
                kept->keep(tile.first, tile.second);
        if (guard && done)
            done();
    };
    if (missing.isEmpty()) {
        deliver({});
        return;
    }
    runLater([directory = directory, missing, deliver] {
        QVector<QPair<QByteArray, QImage>> found;
        for (const auto &key : missing) {
            const auto image = readPreview(directory, key);
            if (!image.isNull())
                found.append({ key, image });
        }
        kobalt::qt::invoke(qApp, [deliver, found] { deliver(found); }, Qt::QueuedConnection);
    });
}

void PreviewStore::insert(const QByteArray &key, const QImage &image)
{
    if (image.isNull())
        return;
    // Pack once: memory and the worker share the packed tile.
    const auto stored = packed(image);
    memory->keep(key, stored);
    runLater([directory = directory, key, stored] {
        writePreview(directory, key, stored, storageLimit);
    });
}

qint64 PreviewStore::memoryBytes() const
{
    qint64 total = 0;
    for (const auto &entry : memory->entries)
        total += entry.bytes;
    return total;
}
} // namespace kobalt::storage
