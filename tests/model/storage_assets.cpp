#include "assets/assets.h"
#include "storage/reader_storage.h"
#include "storage/bookmark_store.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <limits>
#include <utime.h>

using namespace kobalt;

static void expect(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly), "open fixture");
    expect(file.write(bytes) == bytes.size(), "write fixture");
}

static void bundle(const QString &root, const QByteArray &font)
{
    expect(QDir().mkpath(root + "/fonts"), "asset directory");
    write(root + "/fonts/IBMPlexSans-Regular.ttf", font);
    QJsonObject entry;
    entry["path"] = QStringLiteral("fonts/IBMPlexSans-Regular.ttf");
    entry["size"] = font.size();
    entry["sha256"] =
            QString::fromLatin1(QCryptographicHash::hash(font, QCryptographicHash::Sha256).toHex());
    QJsonObject manifest;
    QJsonArray entries;
    entries.append(entry);
    manifest["schema"] = 1;
    manifest["version"] = QStringLiteral("asset-test");
    manifest["files"] = entries;
    write(root + "/manifest.json", QJsonDocument(manifest).toJson());
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    expect(temporary.isValid(), "temporary directory");
    storage::Paths paths{ temporary.path() + "/state" };
    expect(paths.preferences() == paths.directory + "/reader-settings.json",
           "settings filename compatibility");
    expect(paths.selection() == paths.directory + "/config.json",
           "selection filename compatibility");
    expect(paths.annotations() == paths.directory + "/annotations",
           "annotations directory compatibility");
    expect(paths.database() == paths.directory + "/kobalt.sqlite",
           "database filename and profile location");
    expect(paths.layoutCache() == paths.directory + "/cache/reopen-layout.json",
           "layout filename compatibility");
    expect(paths.bookState("store-book-123")
                   == paths.directory
                           + "/b2810aac23895640e06ad1f67e8f46d5fa93a57e9fdc2b2f72e8ffaff93699fd.json",
           "purchase identity hashing compatibility");
    expect(paths.bookState("file:///mnt/onboard/example.epub")
                   == paths.directory
                           + "/7a30ef75230e48f63c6f4f2ee9621416b4a5334e5a6ca607b0ba9570961fcd7c.json",
           "file URL hashing compatibility");
    expect(storage::Paths{}.preferences().isEmpty() && storage::Paths{}.bookState("book").isEmpty(),
           "missing root does not select working directory");
    expect(QDir().mkpath(paths.directory), "state directory");
    QJsonObject saved;
    saved["book"] = QStringLiteral("store-book-123");
    saved["position"] = QStringLiteral("/body/p[1]");
    expect(storage::writeJson(paths.bookState("store-book-123"), saved),
           "alternate-root state write");
    expect(storage::readJson(paths.bookState("store-book-123")) == saved,
           "state fields survive round trip");
    // Each way a read can fail is reported, and every failure reads as empty.
    using storage::ReadStatus;
    const auto status = [](const QString &path, qint64 limit = storage::maxJsonBytes) {
        const auto read = storage::readJsonFile(path, limit);
        expect(read.status == ReadStatus::Ok || read.object.isEmpty(),
               "a failed read returned data");
        return read.status;
    };
    expect(status(paths.bookState("store-book-123")) == ReadStatus::Ok, "a valid state file");
    expect(status(paths.directory + "/absent.json") == ReadStatus::Missing, "a missing file");
    write(paths.directory + "/damaged.json", "{\"book\":");
    expect(status(paths.directory + "/damaged.json") == ReadStatus::Invalid, "damaged JSON");
    write(paths.directory + "/array.json", "[1,2]");
    expect(status(paths.directory + "/array.json") == ReadStatus::Invalid,
           "a top level that is not an object");
    // A valid object of exactly the given size.
    const auto sized = [](qint64 bytes) {
        const QByteArray frame = "{\"text\":\"\"}";
        return QByteArray("{\"text\":\"") + QByteArray(bytes - frame.size(), 'x') + "\"}";
    };
    static_assert(storage::maxJsonBytes == 16 * 1024, "ordinary state files are limited to 16 KiB");
    write(paths.directory + "/limit.json", sized(storage::maxJsonBytes));
    expect(status(paths.directory + "/limit.json") == ReadStatus::Ok, "a file of exactly 16 KiB");
    write(paths.directory + "/large.json", sized(storage::maxJsonBytes + 1));
    expect(status(paths.directory + "/large.json") == ReadStatus::TooLarge,
           "a file one byte over 16 KiB");
    static_assert(BookmarkStore::maxFileBytes == 1024 * 1024,
                  "bookmark files are limited to 1 MiB");
    write(paths.directory + "/bookmarks-limit.json", sized(BookmarkStore::maxFileBytes));
    expect(status(paths.directory + "/bookmarks-limit.json", BookmarkStore::maxFileBytes)
                   == ReadStatus::Ok,
           "a file of exactly 1 MiB");
    write(paths.directory + "/bookmarks-large.json", sized(BookmarkStore::maxFileBytes + 1));
    expect(status(paths.directory + "/bookmarks-large.json", BookmarkStore::maxFileBytes)
                   == ReadStatus::TooLarge,
           "a file one byte over 1 MiB");
    expect(status(paths.directory + "/limit.json", std::numeric_limits<qint64>::max())
                   == ReadStatus::Ok,
           "the largest limit does not overflow");
    // A directory exists but cannot be read as a file, whoever runs the check.
    expect(QDir().mkpath(paths.directory + "/folder.json"), "directory fixture");
    expect(status(paths.directory + "/folder.json") == ReadStatus::Unreadable,
           "an unreadable path");
    expect(storage::readJson(paths.directory + "/damaged.json").isEmpty(),
           "readJson keeps its empty fallback");

    const QString first = temporary.path() + "/first", second = temporary.path() + "/second";
    bundle(first, "first synthetic font");
    bundle(second, "second synthetic font");
    AssetBundle a(first, "asset-test"), b(second, "asset-test");
    {
        const auto root = temporary.path() + "/explicit";
        bundle(root, "verified font");
        AssetBundle assets(root, "asset-test", AssetBundle::Verification::Explicit);
        expect(!assets.available(), "explicit assets wait for startup verification");
        expect(!assets.verify([] { return false; }) && !assets.available(),
               "a superseded lifecycle check cannot enable assets");
        expect(assets.verify() && assets.available(), "startup verifies the complete asset bundle");
        write(root + "/fonts/IBMPlexSans-Regular.ttf", "damaged font");
        expect(assets.available(), "book opening retains the lifecycle verification result");
        expect(assets.fontData("IBMPlexSans-Regular.ttf").isEmpty(),
               "font reads still reject changed bytes");
        assets.invalidate();
        expect(!assets.available(), "USB release closes the asset gate");
        expect(!assets.verify() && !assets.available(), "remount rejects damaged assets");
        bundle(root, "repaired font");
        expect(!assets.available(), "repair does not trigger verification during book opening");
        expect(assets.verify() && assets.available()
                       && assets.fontData("IBMPlexSans-Regular.ttf") == "repaired font",
               "a later lifecycle check recovers repaired assets");
    }
    expect(a.available() && b.available(), "independent asset roots verify");
    expect(a.fontData("IBMPlexSans-Regular.ttf") == "first synthetic font"
                   && b.fontData("IBMPlexSans-Regular.ttf") == "second synthetic font",
           "bundle data cannot cross roots");
    std::function<QByteArray()> deferred;
    {
        AssetBundle temporaryBundle(first, "asset-test");
        deferred = temporaryBundle.deferredFontData("IBMPlexSans-Regular.ttf");
    }
    expect(deferred && deferred() == "first synthetic font",
           "deferred font reader owns its verified metadata");
    {
        const auto root = temporary.path() + "/batch";
        bundle(root, "first batch font");
        auto manifest = storage::readJson(root + "/manifest.json");
        auto entries = manifest["files"].toArray();
        const QByteArray other("second batch font");
        entries.append(qt::object(
                { { "path", "fonts/IBMPlexMono-Regular.ttf" },
                  { "size", other.size() },
                  { "sha256",
                    QString::fromLatin1(QCryptographicHash::hash(other, QCryptographicHash::Sha256)
                                                .toHex()) } }));
        manifest["files"] = entries;
        write(root + "/fonts/IBMPlexMono-Regular.ttf", other);
        expect(storage::writeJson(root + "/manifest.json", manifest), "write batch manifest");
        QMap<QString, std::function<QByteArray()>> readers;
        {
            AssetBundle batch(root, "asset-test");
            readers = batch.deferredFontData(QStringList{ "IBMPlexSans-Regular.ttf",
                                                          "IBMPlexMono-Regular.ttf",
                                                          "IBMPlexSans-Regular.ttf" });
            expect(readers.size() == 2 && readers["IBMPlexSans-Regular.ttf"]() == "first batch font"
                           && readers["IBMPlexMono-Regular.ttf"]() == other,
                   "batch returns the requested verified members without duplicate readers");
            expect(batch.deferredFontData(QStringList{ "IBMPlexSans-Regular.ttf", "absent.ttf" })
                           .isEmpty(),
                   "one missing requested member rejects the whole batch");
            write(root + "/fonts/IBMPlexSans-Regular.ttf", "other batch font");
            expect(readers["IBMPlexSans-Regular.ttf"]().isEmpty(),
                   "batch reader rejects same-size changed bytes");
            write(root + "/fonts/IBMPlexSans-Regular.ttf", "first batch font");
            expect(QFile::remove(root + "/fonts/IBMPlexMono-Regular.ttf"),
                   "remove unrelated batch member");
            expect(readers["IBMPlexSans-Regular.ttf"]() == "first batch font"
                           && readers["IBMPlexMono-Regular.ttf"]().isEmpty(),
                   "deferred readers validate their own members without retaining source bytes");
            expect(!batch.available()
                           && batch.deferredFontData(QStringList{ "IBMPlexSans-Regular.ttf" })
                                      .isEmpty(),
                   "whole-bundle boundary rejects an unrelated missing member");
            write(root + "/fonts/IBMPlexMono-Regular.ttf", other);
            batch.invalidate();
            expect(batch.available() && readers["IBMPlexSans-Regular.ttf"]() == "first batch font",
                   "invalidation does not revoke unchanged verified metadata");
            bundle(root, "replacement batch font");
            batch.invalidate();
            const auto replacement =
                    batch.deferredFontData(QStringList{ "IBMPlexSans-Regular.ttf" });
            expect(replacement.size() == 1
                           && replacement["IBMPlexSans-Regular.ttf"]() == "replacement batch font"
                           && readers["IBMPlexSans-Regular.ttf"]().isEmpty(),
                   "a replacement manifest cannot make an old reader accept new bytes");
            readers = replacement;
        }
        expect(readers["IBMPlexSans-Regular.ttf"]() == "replacement batch font",
               "batch metadata outlives its bundle owner");
        expect(QDir(root).removeRecursively(), "remove batch storage for USB handoff");
        expect(readers["IBMPlexSans-Regular.ttf"]().isEmpty(),
               "batch reader rejects removed storage after owner destruction");
    }
    write(first + "/fonts/IBMPlexSans-Regular.ttf", "other synthetic font");
    expect(deferred().isEmpty(), "deferred font reader rejects bytes changed after verification");
    write(first + "/fonts/IBMPlexSans-Regular.ttf", "first synthetic font");
    write(first + "/fonts/unlisted.ttf", "not in the verified manifest");
    expect(a.available() && a.fontData("unlisted.ttf").isEmpty(),
           "unlisted font bytes are never returned");
    {
        // The opening view reads its fonts before the whole bundle is checked.
        // One damaged member must not hide the others, nor be returned itself.
        const QString partial = temporary.path() + "/partial";
        bundle(partial, "partial synthetic font");
        auto manifest = QJsonDocument::fromJson([&] {
                            QFile file(partial + "/manifest.json");
                            expect(file.open(QIODevice::ReadOnly), "read partial manifest");
                            return file.readAll();
                        }())
                                .object();
        auto entries = manifest["files"].toArray();
        entries.append(qt::object({ { "path", "fonts/NotoSerif-Bold.ttf" },
                                    { "size", 4 },
                                    { "sha256", QString(64, QLatin1Char('0')) } }));
        manifest["files"] = entries;
        write(partial + "/manifest.json", QJsonDocument(manifest).toJson());
        write(partial + "/fonts/NotoSerif-Bold.ttf", "damn");
        write(partial + "/fonts/unlisted.ttf", "not in the manifest");
        AssetBundle opening(partial, "asset-test");
        expect(opening.manifestFontData("IBMPlexSans-Regular.ttf") == "partial synthetic font",
               "a listed font is read without verifying the whole bundle");
        expect(opening.manifestFontData("NotoSerif-Bold.ttf").isEmpty(),
               "a damaged listed font is rejected");
        expect(opening.manifestFontData("unlisted.ttf").isEmpty(), "an unlisted font is rejected");
        expect(opening.manifestFontData("../manifest.json").isEmpty(),
               "a path outside fonts is rejected");
        expect(AssetBundle(partial, "wrong-version")
                       .manifestFontData("IBMPlexSans-Regular.ttf")
                       .isEmpty(),
               "a font from another version is rejected");
        expect(!opening.available(), "the damaged member still fails full verification");
        write(partial + "/fonts/IBMPlexSans-Regular.ttf", "changed synthetic font");
        expect(opening.manifestFontData("IBMPlexSans-Regular.ttf").isEmpty(),
               "a changed font is rejected");
    }
    expect(!AssetBundle(first, "wrong-version").available(),
           "version mismatch rejected independently of another bundle cache");
    expect(!AssetBundle({}, "asset-test").available(),
           "missing asset root does not select working directory");
    expect(a.file("../outside").isEmpty() && a.file("/outside").isEmpty(),
           "invalid asset members rejected");
    expect(!a.hasFilesystemRecoveryFiles(), "clean assets have no recovery files");
    expect(QFile::rename(first + "/fonts/IBMPlexSans-Regular.ttf", first + "/fonts/FSCK0000.000"),
           "model recovered filename");
    expect(a.hasFilesystemRecoveryFiles(), "detect recovered filename");
    expect(a.available() && !a.hasFilesystemRecoveryFiles(),
           "matching recovery file is restored automatically");
    expect(a.fontData("IBMPlexSans-Regular.ttf") == "first synthetic font",
           "repair preserves verified font bytes");
    write(first + "/fonts/FSCK0001.000", "unmatched recovery");
    expect(QFile::remove(first + "/fonts/IBMPlexSans-Regular.ttf"), "remove expected asset");
    expect(!a.available() && QFile::exists(first + "/fonts/FSCK0001.000"),
           "unmatched recovery stays untouched");
    write(first + "/fonts/IBMPlexSans-Regular.ttf", "first synthetic font");
    write(first + "/fonts/FSCK0002.000", "first synthetic font");
    a.invalidate();
    expect(a.available() && QFile::exists(first + "/fonts/FSCK0002.000"),
           "existing asset is never replaced");
    expect(QFile::remove(first + "/fonts/FSCK0001.000")
                   && QFile::remove(first + "/fonts/FSCK0002.000"),
           "remove recovery fixtures");
    write(first + "/fonts/FSCK-not-a-recovery-file", "unrelated");
    expect(!a.hasFilesystemRecoveryFiles(), "ordinary filenames do not imply recovery");
    FontCopies copies;
    const auto copied =
            copies.file("IBMPlexSans-Regular.ttf", a.fontData("IBMPlexSans-Regular.ttf"));
    expect(!copied.isEmpty() && !copied.startsWith(first),
           "Qt font copy lives outside asset storage");
    expect(QDir(first).removeRecursively(), "remove source to model storage export");
    expect(deferred().isEmpty(), "deferred font reader does not retain the source bytes");
    a.invalidate();
    expect(!a.available(), "removed assets fail after invalidation");
    QFile retained(copied);
    expect(retained.open(QIODevice::ReadOnly), "Qt copy survives asset invalidation");
    expect(retained.readAll() == "first synthetic font", "retained Qt copy keeps verified bytes");
    const auto changed = copies.file("IBMPlexSans-Regular.ttf", "replacement synthetic font");
    expect(changed != copied, "changed font bytes must not overwrite a Qt-mapped copy");
    expect(retained.seek(0) && retained.readAll() == "first synthetic font",
           "older Qt copy remains unchanged");
    expect(b.available(), "other bundle cache remains valid");
    write(second + "/fonts/IBMPlexSans-Regular.ttf", "corrupt");
    expect(!b.available(), "changed source is reverified");
    expect(b.fontData("IBMPlexSans-Regular.ttf").isEmpty(), "corrupt font bytes are not returned");
    expect(copies.file("../outside", "bad").isEmpty(), "font copy cannot escape temporary owner");
    const auto linked = temporary.path() + "/linked";
    bundle(linked, "linked synthetic font");
    AssetBundle linkBundle(linked, "asset-test");
    expect(linkBundle.available(), "ordinary source verifies before symlink substitution");
    expect(QDir().rename(linked + "/fonts", temporary.path() + "/external-fonts"),
           "move fonts outside asset root");
    expect(QFile::link(temporary.path() + "/external-fonts", linked + "/fonts"),
           "create parent symlink fixture");
    expect(!linkBundle.available() && linkBundle.fontData("IBMPlexSans-Regular.ttf").isEmpty(),
           "symlink parent invalidates cached source");
    {
        // Only the interface and default reading fonts are required. Another
        // asset that fails its check is set aside on its own.
        const auto root = temporary.path() + "/optional";
        bundle(root, "required font");
        auto manifest = storage::readJson(root + "/manifest.json");
        auto entries = manifest["files"].toArray();
        const QByteArray optional("optional font");
        entries.append(
                qt::object({ { "path", "fonts/ebook-core/Optional.ttf" },
                             { "size", optional.size() },
                             { "sha256",
                               QString::fromLatin1(QCryptographicHash::hash(
                                                           optional, QCryptographicHash::Sha256)
                                                           .toHex()) } }));
        manifest["files"] = entries;
        expect(storage::writeJson(root + "/manifest.json", manifest), "write optional manifest");
        expect(QDir().mkpath(root + "/fonts/ebook-core"), "optional font directory");
        write(root + "/fonts/ebook-core/Optional.ttf", "damaged optional font");
        AssetBundle assets(root, "asset-test", AssetBundle::Verification::Explicit);
        // USB can revoke storage during a cold check. The check must stop at
        // the next file or block, because the main thread waits for it.
        int checks = 0;
        expect(!assets.verify([&checks] { return ++checks < 2; }) && checks >= 2 && checks <= 3
                       && !assets.available() && !QFileInfo(root + "/.verified.json").exists(),
               "a cancelled check stops at once and records nothing");
        expect(assets.verify() && assets.available()
                       && assets.setAside("fonts/ebook-core/Optional.ttf")
                       && !assets.setAside("fonts/IBMPlexSans-Regular.ttf"),
               "a damaged optional asset is set aside and the bundle stays available");
        const auto readers = assets.deferredFontData(
                QStringList{ "IBMPlexSans-Regular.ttf", "ebook-core/Optional.ttf" });
        expect(readers.size() == 1 && readers["IBMPlexSans-Regular.ttf"]() == "required font",
               "a batch leaves out a set-aside font");
        write(root + "/fonts/ebook-core/Optional.ttf", optional);
        assets.invalidate();
        expect(assets.verify() && !assets.setAside("fonts/ebook-core/Optional.ttf")
                       && assets.fontData("ebook-core/Optional.ttf") == optional,
               "a repaired optional asset is used again");
        expect(QFileInfo(root + "/.verified.json").isFile(), "the check records verified files");

        // A file with its recorded size and time is trusted without hashing.
        // Same-size changed bytes with the old time therefore pass the check.
        const auto required = root + "/fonts/IBMPlexSans-Regular.ttf";
        const auto stamp = QFileInfo(required).lastModified().toMSecsSinceEpoch() / 1000;
        const auto setTime = [&required](qint64 seconds) {
            utimbuf times{ time_t(seconds), time_t(seconds) };
            return utime(QFile::encodeName(required).constData(), &times) == 0;
        };
        // Record a whole-second time first; the reset below restores seconds.
        expect(setTime(stamp), "set a whole-second modification time");
        assets.invalidate();
        expect(assets.verify(), "record the whole-second time");
        write(required, "requires font");
        expect(setTime(stamp), "restore the recorded modification time");
        AssetBundle trusted(root, "asset-test", AssetBundle::Verification::Explicit);
        expect(trusted.verify(), "an unchanged size and time skip the hash");
        // A changed time makes the next check hash the file again.
        expect(setTime(stamp + 60), "change the modification time");
        AssetBundle rehashed(root, "asset-test", AssetBundle::Verification::Explicit);
        expect(!rehashed.verify(), "a changed time hashes again and rejects changed bytes");
    }
}
