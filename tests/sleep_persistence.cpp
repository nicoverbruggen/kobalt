#include "storage/sleep_snapshot.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QBuffer>
#include <QImageWriter>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

// Checks the sleep-image writer: exact pixels and metadata, stale tokens, lossless encoding and native-cover references.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const auto path = directory.path() + "/snapshot.png";
    kobalt::sleep::Snapshot snapshot;
    snapshot.image = QImage(127, 173, QImage::Format_RGB32);
    for (int y = 0; y < snapshot.image.height(); ++y)
        for (int x = 0; x < snapshot.image.width(); ++x)
            snapshot.image.setPixel(x, y,
                                    qRgb((x * 37 + y) % 256, (y * 17) % 256, (x + y * 31) % 256));
    snapshot.mode = kobalt::sleep::Mode::Cover;
    snapshot.orientation = Qt::InvertedLandscapeOrientation;
    snapshot.progress = QString::fromUtf8("42% lu · 読書");
    snapshot.remaining = "37 min";
    snapshot.dark = true;
    snapshot.dimPage = false;
    snapshot.percent = 42;
    require(kobalt::storage::writeSleepSnapshot(path, snapshot, "book-token"),
            "production PNG write");
    const auto read = kobalt::storage::readSleepSnapshot(path, "book-token", snapshot.mode);
    require(read.image == snapshot.image && read.orientation == snapshot.orientation
                    && read.progress == snapshot.progress && read.remaining == snapshot.remaining
                    && read.dark == snapshot.dark && read.dimPage == snapshot.dimPage
                    && read.percent == snapshot.percent,
            "lossless pixels and metadata");
    require(kobalt::storage::readSleepSnapshot(path, "stale-token", snapshot.mode).image.isNull(),
            "stale-token rejection");
    // The shipped level must work with both modern Qt and Qt 5.2's quality API.
    for (bool compressionApi : { true, false }) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        require(buffer.open(QIODevice::WriteOnly), "codec test buffer");
        QImageWriter writer(&buffer, "png");
        QJsonObject selected;
        require(kobalt::storage::configureSleepSnapshotCodec(writer, compressionApi, true,
                                                             selected),
                "production codec supported");
        require(compressionApi ? writer.compression() == 11 : writer.quality() == 89,
                "codec argument selects zlib level 1");
        require(writer.write(snapshot.image), "production codec writes");
        require(QImage::fromData(bytes, "png") == snapshot.image,
                "both codec APIs remain lossless");
    }
    QImageWriter unsupportedWriter;
    QJsonObject unsupported;
    require(!kobalt::storage::configureSleepSnapshotCodec(unsupportedWriter, false, false,
                                                          unsupported),
            "unsupported codec fails explicitly");
    const auto coverPath = directory.path() + "/native-cover.png";
    QImage cover(64, 96, QImage::Format_ARGB32);
    cover.fill(Qt::transparent);
    cover.setPixel(32, 48, qRgb(255, 0, 0));
    require(cover.save(coverPath, "PNG"), "native cover fixture");
    const QFileInfo source(coverPath);
    snapshot.coverPath = source.absoluteFilePath();
    snapshot.coverBytes = source.size();
    snapshot.coverModified = source.lastModified().toMSecsSinceEpoch();
    QImage opaque(cover.size(), QImage::Format_RGB32);
    opaque.fill(Qt::white);
    QPainter painter(&opaque);
    painter.drawImage(0, 0, cover);
    painter.end();
    snapshot.image = kobalt::sleep::coverBackground(opaque, { 127, 173 });
    require(snapshot.image.pixelColor(63, 20) == Qt::white
                    && snapshot.image.pixelColor(0, 20) == Qt::black,
            "transparent cover is white inside black letterbox");
    require(kobalt::storage::writeSleepSnapshot(path, snapshot, "cover-token"),
            "native cover reference save");
    require(!QFile::exists(path) && QFile::exists(directory.path() + "/snapshot.json"),
            "cover save writes metadata instead of another PNG");
    const auto loaded = kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode);
    require(loaded.image == snapshot.image && loaded.progress == snapshot.progress
                    && loaded.coverPath == snapshot.coverPath,
            "reference restores pixels and metadata");
    require(kobalt::storage::readSleepSnapshot(path, "stale", snapshot.mode).image.isNull(),
            "stale reference rejected");
    QFile reference(directory.path() + "/snapshot.json");
    require(reference.open(QIODevice::ReadOnly), "reference JSON");
    const auto original = QJsonDocument::fromJson(reference.readAll()).object();
    reference.close();
    const auto rewrite = [&](QJsonObject json) {
        require(reference.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "rewrite reference fixture");
        reference.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
        reference.close();
    };
    // A stale PNG must not defeat rejection of a damaged newer reference.
    auto legacy = snapshot;
    legacy.coverPath.clear();
    require(kobalt::storage::writeSleepSnapshot(path, legacy, "cover-token"), "older PNG fixture");
    rewrite(original);
    for (const auto &field : { QStringLiteral("cover_bytes"), QStringLiteral("cover_modified") }) {
        for (double value : { -1.0, 0.5, 1e300 }) {
            auto damaged = original;
            damaged[field] = value;
            rewrite(damaged);
            require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode)
                            .image.isNull(),
                    "malformed reference integer rejected without PNG fallback");
        }
    }
    auto future = original;
    future["version"] = 2;
    rewrite(future);
    require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode).image.isNull(),
            "future reference version rejected");
    rewrite(original);
    QFile changed(coverPath);
    require(changed.open(QIODevice::Append) && changed.write("changed") == 7,
            "change cover fixture");
    changed.close();
    require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode).image.isNull(),
            "changed source rejected");
    require(QFile::remove(coverPath), "remove native source");
    require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode).image.isNull(),
            "missing source rejected");
    require(kobalt::storage::writeSleepSnapshot(path, snapshot, "fallback-token")
                    && QFile::exists(path) && !reference.exists(),
            "missing source uses PNG fallback");
    require(kobalt::storage::readSleepSnapshot(path, "fallback-token", snapshot.mode).image
                    == snapshot.image,
            "fallback keeps captured cover pixels");
    kobalt::storage::removeSleepSnapshot(path);
    require(!QFile::exists(path) && !reference.exists(), "both cache forms removed");
    require(directory.remove(), "scratch cleanup");
}
