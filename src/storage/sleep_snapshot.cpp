#include "sleep_snapshot.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QDateTime>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <random>

namespace kobalt::storage {
namespace {
constexpr qint64 maxPixels = 4 * 1024 * 1024;
constexpr qint64 maxBytes = 20 * 1024 * 1024;

bool validSize(QSize size)
{
    return size.width() > 0 && size.height() > 0
            && qint64(size.width()) * size.height() <= maxPixels;
}

bool validOrientation(int value)
{
    return value == Qt::PortraitOrientation || value == Qt::LandscapeOrientation
            || value == Qt::InvertedPortraitOrientation
            || value == Qt::InvertedLandscapeOrientation;
}

QString referencePath(const QString &path)
{
    return (path.endsWith(".png") ? path.left(path.size() - 4) : path) + ".json";
}

QJsonObject metadataFor(const sleep::Snapshot &snapshot, const QString &token)
{
    return qt::object({ { "version", 1 },
                        { "token", token },
                        { "mode", sleep::value(snapshot.mode) },
                        { "orientation", int(snapshot.orientation) },
                        { "progress", snapshot.progress },
                        { "remaining", snapshot.remaining },
                        { "dark", snapshot.dark },
                        { "dim", snapshot.dimPage },
                        { "percent", snapshot.percent } });
}

bool integerField(const QJsonValue &value, double minimum, double maximum, qint64 &result)
{
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < minimum || number > maximum
        || std::floor(number) != number)
        return false;
    result = qint64(number);
    return true;
}

bool writeSnapshot(const QString &path, const sleep::Snapshot &snapshot, const QString &token,
                   QJsonObject *measurement)
{
    QElapsedTimer total;
    if (measurement)
        total.start();
    if (path.isEmpty() || token.isEmpty() || snapshot.mode == sleep::Mode::Stock
        || !validSize(snapshot.image.size()) || !validOrientation(snapshot.orientation))
        return false;
    const auto metadata = metadataFor(snapshot, token);
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (bytes.size() > 16384 || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QImageWriter writer(&file, "png");
    writer.setText("kobalt", QString::fromUtf8(bytes));
    QJsonObject codec;
    if (!configureSleepSnapshotCodec(writer,
                                     writer.supportsOption(QImageIOHandler::CompressionRatio),
                                     writer.supportsOption(QImageIOHandler::Quality),
                                     measurement ? *measurement : codec))
        return false;
    QElapsedTimer stage;
    if (measurement)
        stage.start();
    const bool encoded = writer.write(snapshot.image);
    if (measurement) {
        (*measurement)["encode_write_ms"] = stage.nsecsElapsed() / 1e6;
        (*measurement)["bytes"] = double(file.size());
        stage.restart();
    }
    const bool committed = encoded && file.size() <= maxBytes && file.commit();
    if (measurement) {
        (*measurement)["commit_ms"] = stage.nsecsElapsed() / 1e6;
        (*measurement)["total_ms"] = total.nsecsElapsed() / 1e6;
    }
    return committed;
}

bool persistSnapshot(const QString &path, const sleep::Snapshot &snapshot, const QString &token,
                     QJsonObject *measurement)
{
    QElapsedTimer total;
    total.start();
    if (measurement)
        *measurement = qt::object(
                { { "width", snapshot.image.width() }, { "height", snapshot.image.height() } });
    if (path.isEmpty() || token.isEmpty() || !validSize(snapshot.image.size())
        || !validOrientation(snapshot.orientation))
        return false;
    if (currentSleepCover(snapshot)) {
        QImageReader source(snapshot.coverPath);
        if (validSize(source.size()) && source.canRead()) {
            auto metadata = metadataFor(snapshot, token);
            metadata["kind"] = QStringLiteral("native-cover");
            metadata["cover_path"] = snapshot.coverPath;
            metadata["cover_bytes"] = double(snapshot.coverBytes);
            metadata["cover_modified"] = double(snapshot.coverModified);
            metadata["width"] = snapshot.image.width();
            metadata["height"] = snapshot.image.height();
            const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
            if (bytes.size() > 16384 || !QDir().mkpath(QFileInfo(path).absolutePath()))
                return false;
            QSaveFile file(referencePath(path));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
                return false;
            QElapsedTimer commit;
            commit.start();
            const bool saved = file.commit();
            if (measurement) {
                (*measurement)["persistence_kind"] = QStringLiteral("reference");
                (*measurement)["bytes"] = bytes.size();
                (*measurement)["commit_ms"] = commit.nsecsElapsed() / 1e6;
                (*measurement)["total_ms"] = total.nsecsElapsed() / 1e6;
                (*measurement)["saved"] = saved;
            }
            if (saved)
                QFile::remove(path);
            return saved;
        }
    }
    if (measurement)
        (*measurement)["persistence_kind"] = QStringLiteral("png");
    bool saved = writeSnapshot(path, snapshot, token, measurement);
    if (saved && QFile::exists(referencePath(path)) && !QFile::remove(referencePath(path)))
        saved = false;
    if (measurement)
        (*measurement)["saved"] = saved;
    return saved;
}
} // namespace

bool writeSleepSnapshot(const QString &path, const sleep::Snapshot &snapshot, const QString &token)
{
    mainthread::BlockingOperation blockingOperation("sleep-snapshot-write");
    return persistSnapshot(path, snapshot, token, nullptr);
}

QImage loadSleepOverlay(const QStringList &directories)
{
    QStringList candidates;
    for (const auto &directory : directories) {
        if (directory.isEmpty())
            continue;
        for (const auto &file :
             QDir(directory).entryInfoList({ QStringLiteral("*.png") },
                                           QDir::Files | QDir::Readable | QDir::NoSymLinks)) {
            if (file.size() > 0 && file.size() <= maxBytes)
                candidates.append(file.absoluteFilePath());
        }
    }
    if (candidates.isEmpty())
        return {};
    std::mt19937 random(std::random_device{}());
    std::shuffle(candidates.begin(), candidates.end(), random);
    for (const auto &path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > maxBytes)
            continue;
        QImageReader reader(&file, "png");
        if (!validSize(reader.size()))
            continue;
        auto image = reader.read();
        if (validSize(image.size()) && image.hasAlphaChannel())
            return image;
    }
    return {};
}

void removeSleepSnapshot(const QString &path)
{
    QFile::remove(path);
    QFile::remove(referencePath(path));
}

bool currentSleepCover(const sleep::Snapshot &snapshot)
{
    const QFileInfo source(snapshot.coverPath);
    return snapshot.mode == sleep::Mode::Cover && !snapshot.dimPage && !snapshot.coverPath.isEmpty()
            && source.isAbsolute() && source.isFile() && snapshot.coverBytes > 0
            && snapshot.coverBytes <= maxBytes && source.size() == snapshot.coverBytes
            && source.lastModified().toMSecsSinceEpoch() == snapshot.coverModified;
}

bool configureSleepSnapshotCodec(QImageWriter &writer, bool supportsCompression,
                                 bool supportsQuality, QJsonObject &measurement)
{
    measurement.remove("error");
    measurement["supports_compression"] = supportsCompression;
    measurement["supports_quality"] = supportsQuality;
    measurement["codec_api"] = QStringLiteral("default");
    measurement["codec_argument"] = -1;
    if (!supportsCompression && !supportsQuality) {
        measurement["codec_api"] = QStringLiteral("unsupported");
        measurement["error"] = QStringLiteral("PNG compression control is unavailable");
        return false;
    }
    // Level 1 is lossless. PNG maps its 0..100 option with value * 9 / 91.
    constexpr int argument = 11;
    if (supportsCompression) {
        writer.setCompression(argument);
        measurement["codec_api"] = QStringLiteral("compression");
        measurement["codec_argument"] = argument;
    } else {
        // Qt 5.2 PNG maps quality to zlib level with (100 - quality) * 9 / 91.
        // Round up the inverse so level 1 uses quality 89, not 90 (level 0).
        const int quality = 100 - argument;
        writer.setQuality(quality);
        measurement["codec_api"] = QStringLiteral("quality");
        measurement["codec_argument"] = quality;
    }
    return true;
}
#ifdef KOBALT_SCENARIO_PLAYER
bool measureSleepPersistence(const QString &path, const sleep::Snapshot &snapshot,
                             const QString &token, QJsonObject &measurement)
{
    return persistSnapshot(path, snapshot, token, &measurement);
}
#endif
sleep::Snapshot readSleepSnapshot(const QString &path, const QString &token, sleep::Mode mode)
{
    if (token.isEmpty() || mode == sleep::Mode::Stock)
        return {};
    const bool reference = QFile::exists(referencePath(path));
    QFile file(reference ? referencePath(path) : path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > (reference ? 16384 : maxBytes))
        return {};
    QImageReader reader(&file, "png");
    reader.setDecideFormatFromContent(false);
    if (!reference && !validSize(reader.size()))
        return {};
    const auto text = reference ? file.readAll() : reader.text("kobalt").toUtf8();
    if (text.size() > 16384)
        return {};
    const auto metadata = QJsonDocument::fromJson(text).object();
    const int orientation = metadata.value("orientation").toInt();
    if (metadata.value("version") != 1 || metadata.value("token") != token
        || metadata.value("mode") != sleep::value(mode) || !validOrientation(orientation)
        || !metadata.value("progress").isString() || !metadata.value("remaining").isString()
        || !metadata.value("dark").isBool() || !metadata.value("dim").isBool())
        return {};
    // Older images have no percentage. Tiles that draw it leave it out.
    const int percent = metadata.value("percent").toInt(-1);
    sleep::Snapshot snapshot{ {},
                              Qt::ScreenOrientation(orientation),
                              metadata.value("progress").toString(),
                              metadata.value("remaining").toString(),
                              metadata.value("dark").toBool(),
                              mode,
                              metadata.value("dim").toBool(),
                              percent >= 0 && percent <= 100 ? percent : -1 };
    if (!reference) {
        snapshot.image = reader.read();
        return validSize(snapshot.image.size()) ? snapshot : sleep::Snapshot{};
    }
    if (metadata.value("kind") != QStringLiteral("native-cover"))
        return {};
    snapshot.coverPath = metadata.value("cover_path").toString();
    if (!integerField(metadata.value("cover_bytes"), 1, maxBytes, snapshot.coverBytes)
        || !integerField(metadata.value("cover_modified"), 0, 9007199254740991.0,
                         snapshot.coverModified))
        return {};
    const QSize size(metadata.value("width").toInt(), metadata.value("height").toInt());
    if (!validSize(size) || !currentSleepCover(snapshot))
        return {};
    QImageReader coverReader(snapshot.coverPath);
    if (!validSize(coverReader.size()))
        return {};
    auto cover = coverReader.read();
    if (cover.isNull() || !currentSleepCover(snapshot))
        return {};
    // Match the engine's native-cover decode, which flattens alpha on white.
    if (cover.hasAlphaChannel()) {
        QImage opaque(cover.size(), QImage::Format_RGB32);
        if (opaque.isNull())
            return {};
        opaque.fill(Qt::white);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, cover);
        painter.end();
        cover = std::move(opaque);
    }
    snapshot.image = sleep::coverBackground(cover, size);
    return snapshot;
}
} // namespace kobalt::storage
