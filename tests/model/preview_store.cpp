#include "storage/preview_store.h"
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include "check.h"

using namespace kobalt;
using kobalt::storage::PreviewStore;

namespace {
// A preview tile: opaque, with a few shades of grey, or one coloured pixel.
QImage tile(int width, int height, QRgb paper, bool colour = false)
{
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(paper);
    for (int x = 0; x < width; ++x)
        image.setPixel(x, height / 2, qRgb(x % 256, x % 256, x % 256));
    if (colour)
        image.setPixel(2, 2, qRgb(200, 40, 40));
    return image;
}

qint64 directoryBytes(const QString &path)
{
    qint64 total = 0;
    for (const auto &info : QDir(path).entryInfoList(QDir::Files))
        total += info.size();
    return total;
}

bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return condition();
}

QString cacheName(const QByteArray &key, const char *version = "kobalt-preview-1")
{
    return QString::fromLatin1(
            QCryptographicHash::hash(QByteArray(version) + '\0' + key, QCryptographicHash::Sha256)
                    .toHex());
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "Could not write fixture");
}
} // namespace

// Checks the preview store: exact pixels through storage, misses, the storage and memory limits,
// and that a new store reads what an earlier one wrote.
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "minimal");
    QGuiApplication app(argc, argv);
    QTemporaryDir root;
    require(root.isValid(), "No temporary directory");
    const auto directory = root.filePath("cache/previews");

    // Grey and coloured tiles come back with the same pixels.
    const auto grey = tile(300, 200, qRgb(255, 255, 255));
    const auto dark = tile(300, 200, qRgb(18, 18, 18));
    const auto colour = tile(300, 200, qRgb(250, 245, 230), true);
    require(storage::writePreview(directory, "grey", grey, PreviewStore::storageLimit)
                    && storage::writePreview(directory, "dark", dark, PreviewStore::storageLimit)
                    && storage::writePreview(directory, "colour", colour,
                                             PreviewStore::storageLimit),
            "A preview was not written");
    require(storage::readPreview(directory, "grey")
                            .convertToFormat(QImage::Format_ARGB32_Premultiplied)
                    == grey,
            "A grey preview changed through storage");
    require(storage::readPreview(directory, "colour")
                            .convertToFormat(QImage::Format_ARGB32_Premultiplied)
                    == colour,
            "A coloured preview changed through storage");
    require(storage::readPreview(directory, "missing").isNull(), "A missing key was found");
    require(QDir(directory).entryList({ "*.png" }, QDir::Files).isEmpty()
                    && QFile::exists(QDir(directory).filePath(cacheName("grey") + ".preview")),
            "Previews still have importable image names");
    require(storage::readPreview(directory, "grey-with-changed-inputs").isNull(),
            "Changed rendering inputs reused a tile");

    // Every regular PNG in this disposable directory is obsolete, including
    // damaged files. Other extensions, directories and links stay untouched.
    const auto legacy = root.filePath("legacy/cache/previews");
    require(QDir().mkpath(legacy), "Could not create legacy cache");
    const QDir old(legacy);
    const auto stale = old.filePath(cacheName("deleted-preset") + ".png");
    const auto obsolete = old.filePath(cacheName("old-format", "kobalt-preview-0") + ".png");
    require(grey.save(stale, "PNG") && colour.save(obsolete, "PNG"),
            "Could not create old generated tiles");
    const auto unrelated = root.filePath("personal.png");
    require(grey.save(unrelated, "PNG"), "Could not create unrelated image");
    const auto malformed = old.filePath("damaged.png");
    writeBytes(malformed, "damaged PNG bytes");
    const auto oversized = old.filePath(cacheName("oversized") + ".png");
    writeBytes(oversized, QByteArray(1024 * 1024 + 1, 'x'));
    const auto linked = old.filePath(cacheName("linked") + ".png");
    require(QFile::link(unrelated, linked), "Could not create legacy symlink");
    const auto subdirectory = old.filePath(cacheName("directory") + ".png");
    require(QDir().mkpath(subdirectory), "Could not create legacy subdirectory");
    require(storage::writePreview(legacy, "current", grey, PreviewStore::storageLimit),
            "Could not create current tile");
    require(storage::migratePreviews(legacy) && storage::migratePreviews(legacy),
            "Legacy cleanup failed or was not repeatable");
    require(!QFile::exists(stale) && !QFile::exists(obsolete) && !QFile::exists(malformed)
                    && !QFile::exists(oversized),
            "Stale generated PNGs survived cleanup");
    require(QFile::exists(unrelated) && QFileInfo(linked).isSymLink()
                    && QFileInfo(subdirectory).isDir()
                    && !storage::readPreview(legacy, "current").isNull(),
            "Cleanup touched unrelated files or current previews");
    writeBytes(old.filePath("personal.preview"), "keep this");
    require(storage::writePreview(legacy, "another", grey, 1)
                    && QFile::exists(old.filePath("personal.preview")) && QFile::exists(unrelated)
                    && QFileInfo(linked).isSymLink(),
            "Cache eviction touched unrelated files");

    // Completion is persistent. Later startups must not enumerate PNGs again.
    const auto late = old.filePath("added-after-migration.png");
    writeBytes(late, "keep after completed migration");
    require(storage::migratePreviews(legacy, true) && QFile::exists(late)
                    && !storage::previewMigrationNeedsScan(legacy),
            "Completed migration ran again");
    const auto pendingDirectory = root.filePath("pending");
    require(QDir().mkpath(pendingDirectory), "Could not create pending migration");
    const auto pendingPng = QDir(pendingDirectory).filePath("old.png");
    writeBytes(pendingPng, "damaged PNG");
    require(storage::migratePreviews(pendingDirectory, true) && !QFile::exists(pendingPng)
                    && storage::previewMigrationNeedsScan(pendingDirectory),
            "Removal did not retain a pending native scan");
    require(storage::migratePreviews(pendingDirectory, true)
                    && storage::previewMigrationNeedsScan(pendingDirectory),
            "Restart lost the pending scan after PNG removal");
    writeBytes(QDir(pendingDirectory).filePath("png-migration"), "remove\n");
    require(storage::migratePreviews(pendingDirectory, true)
                    && storage::previewMigrationNeedsScan(pendingDirectory),
            "Interruption after removal lost the pending native scan");
    storage::completePreviewMigration(pendingDirectory);
    require(waitFor([&] { return !storage::previewMigrationNeedsScan(pendingDirectory); }),
            "Native scan completion was not saved");
    writeBytes(pendingPng, "added after native scan");
    require(storage::migratePreviews(pendingDirectory, true) && QFile::exists(pendingPng),
            "Native migration completion did not survive reopening");

    const auto blockedDirectory = root.filePath("blocked-migration");
    require(QDir().mkpath(blockedDirectory), "Could not create blocked migration");
    const auto blockedPng = QDir(blockedDirectory).filePath("old.png");
    writeBytes(blockedPng, "old tile");
    require(QFile::link(unrelated, QDir(blockedDirectory).filePath("png-migration")),
            "Could not link migration marker");
    require(!storage::migratePreviews(blockedDirectory, true) && QFile::exists(blockedPng),
            "Cleanup deleted files without first saving its pending scan");

    // A linked directory or ancestor must never redirect cache cleanup or IO.
    const auto alias = root.filePath("alias");
    require(QFile::link(root.filePath("legacy"), alias), "Could not link profile");
    const auto redirected = QDir(alias).filePath("cache/previews");
    require(!storage::migratePreviews(redirected)
                    && !storage::writePreview(redirected, "escape", grey, 1)
                    && storage::readPreview(redirected, "another").isNull()
                    && QFile::exists(unrelated),
            "A linked ancestor redirected preview IO");
    const auto destinationLink = old.filePath(cacheName("destination-link") + ".preview");
    require(QFile::link(unrelated, destinationLink), "Could not link destination");
    require(!storage::writePreview(legacy, "destination-link", grey, 1)
                    && storage::readPreview(legacy, "destination-link").isNull()
                    && QFileInfo(destinationLink).isSymLink(),
            "A linked tile redirected preview IO");

    // A damaged file is a miss.
    for (const auto &info : QDir(directory).entryInfoList(QDir::Files)) {
        QFile file(info.absoluteFilePath());
        require(file.open(QIODevice::WriteOnly) && file.write("not a png") > 0,
                "A file could not be damaged");
    }
    require(storage::readPreview(directory, "grey").isNull(), "A damaged file was read");

    // Storage keeps the newest files within its limit. Noise does not compress, so each file
    // is about 120 KiB and the limit holds two of them.
    QDir(directory).removeRecursively();
    const qint64 small = 300 * 1024;
    quint32 seed = 1;
    for (int index = 0; index < 6; ++index) {
        auto noisy = tile(400, 300, qRgb(255, 255, 255));
        for (int y = 0; y < noisy.height(); ++y)
            for (int x = 0; x < noisy.width(); ++x) {
                seed = seed * 1664525u + 1013904223u;
                const int level = int(seed >> 24);
                noisy.setPixel(x, y, qRgb(level, level, level));
            }
        require(storage::writePreview(directory, QByteArray::number(index), noisy, small),
                "A preview was not written under a small limit");
    }
    require(directoryBytes(directory) <= small, "Storage went past its limit");
    require(!storage::readPreview(directory, "5").isNull(), "The newest preview was removed first");
    require(QDir(directory).entryList(QDir::Files).size() <= 2,
            "Storage kept more previews than its limit holds");

    // The store keeps tiles in memory and writes them in the background; a later store reads
    // them back.
    const auto storeDirectory = root.filePath("store");
    {
        PreviewStore store(storeDirectory);
        store.insert("a", grey);
        store.insert("b", colour);
        QImage found;
        require(store.find("a", &found) && found == grey, "Memory lost a grey tile");
        require(store.find("b", &found) && found == colour, "Memory lost a coloured tile");
        require(store.memoryBytes() < 2 * qint64(grey.bytesPerLine()) * grey.height(),
                "A grey tile was not packed in memory");
    }
    PreviewStore later(storeDirectory);
    QImage found;
    require(!later.find("a", &found), "A new store started with tiles in memory");
    bool loaded = false;
    later.load({ "a", "b", "absent" }, &app, [&loaded] { loaded = true; });
    require(waitFor([&] { return loaded; }), "Loading from storage did not finish");
    require(later.find("a", &found) && found == grey, "A grey tile did not come back from storage");
    require(later.find("b", &found) && found == colour,
            "A coloured tile did not come back from storage");
    require(!later.find("absent", &found), "A missing tile was found");

    // Memory keeps the tiles used most recently within its limit.
    PreviewStore bounded(root.filePath("bounded"));
    const auto large = tile(1000, 1000, qRgb(255, 255, 255));
    for (int index = 0; index < 8; ++index)
        bounded.insert(QByteArray::number(index), large);
    require(bounded.memoryBytes() <= PreviewStore::memoryLimit, "Memory went past its limit");
    require(bounded.find("7", &found) && !bounded.find("0", &found),
            "Memory did not keep the newest tiles");
    return 0;
}
