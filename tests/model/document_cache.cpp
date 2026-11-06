#include "check.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include "engine/crengine/document_cache.h"
#include "support/qt_compat.h"
#include <lvtinydom.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdio>

static QByteArray hash(const QImage &image)
{
    return QCryptographicHash::hash(
            QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()),
                                    image.bytesPerLine() * image.height()),
            QCryptographicHash::Sha256);
}

// Checks parsed and cached pixel equality, chapters, typography changes, Bionic modes, fingerprints and cache policies.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 2, "Pass the document-cache EPUB fixture");
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "Cannot read document-cache fixture");
    const auto bytes = file.readAll();
    QString error;
    {
        // A book saved to the document cache opens without parsing. It must
        // match a parsed book, including Kobalt's prose markers, and a changed
        // fingerprint or policy must parse again.
        QTemporaryDir cacheRoot;
        require(cacheRoot.isValid(), "Cannot create a document cache directory");
        const auto cacheDirectory = cacheRoot.path() + "/.cache";
        const QByteArray fingerprint(64, 'a'), policy("document-cache-test");
        require(kobalt::documentCacheChecksum(policy, 1)
                        == kobalt::documentCacheChecksum(policy, 1, 11),
                "The document cache did not use its declared format version");
        require(kobalt::documentCacheChecksum(policy, 1, 11)
                        != kobalt::documentCacheChecksum(policy, 1, 10),
                "An incompatible cache format reused the document cache identity");
        require(kobalt::documentCacheChecksum(policy, 1)
                        != kobalt::documentCacheChecksum(policy, 2),
                "An incompatible DOM reused the document cache identity");
        // CREngine leaves books under 30 kB out of its cache.
        const qint64 cachedBytes = 1024 * 1024;
        const auto settings =
                kobalt::qt::object({ { "text_alignment", "left" }, { "bold_reading", "off" } });
        const auto opened = [&](kobalt::EngineDocument &target, const QByteArray &key,
                                const QByteArray &rules, const QJsonObject &saved) {
            target.setDocumentCache(cacheDirectory, key, rules, cachedBytes);
            require(target.open(bytes, QSize(1072, 1328), saved, &error), qPrintable(error));
        };
        {
            kobalt::EngineDocument uncached(kobalt::test::readerFonts());
            require(uncached.open(bytes, QSize(1072, 1328), {}, &error), qPrintable(error));
            require(!uncached.openedFromCache() && !uncached.saveDocumentCache(),
                    "A book without a cache directory used the cache");
        }
        kobalt::EngineDocument parsed(kobalt::test::readerFonts());
        opened(parsed, fingerprint, policy, settings);
        require(!parsed.openedFromCache(), "An empty cache restored a book");
        require(parsed.saveDocumentCache(), "Cannot save a parsed book to the document cache");
        kobalt::EngineDocument restored(kobalt::test::readerFonts());
        opened(restored, fingerprint, policy, settings);
        require(restored.openedFromCache(), "A saved book was parsed again");
        const auto cacheFiles = QDir(cacheDirectory).entryList({ "*.cr3" }, QDir::Files);
        require(cacheFiles.size() == 1, "A cold save did not create exactly one document cache");
        const auto cachePath = cacheDirectory + "/" + cacheFiles.first();
        const auto oldTime = QDateTime::fromSecsSinceEpoch(946684800);
        {
            QFile cacheFile(cachePath);
            require(cacheFile.open(QIODevice::ReadWrite)
                            && cacheFile.setFileTime(oldTime, QFileDevice::FileModificationTime),
                    "Cannot set the document cache timestamp");
        }
        require(restored.saveDocumentCache() && restored.saveDocumentCache(),
                "Cannot save an unchanged document cache");
        require(QFileInfo(cachePath).lastModified() == oldTime,
                "An unchanged document cache was rewritten");
        require(restored.pages() == parsed.pages() && restored.title() == parsed.title(),
                "A restored book changed its pages or title");
        for (int i = 0; i < parsed.pages(); ++i)
            require(parsed.go(i) && restored.go(i)
                            && hash(parsed.image(i)) == hash(restored.image(i)),
                    "A restored page differs from the parsed page");
        const auto parsedChapters = parsed.chapters(), restoredChapters = restored.chapters();
        require(parsedChapters.size() == restoredChapters.size(), "A restored book lost chapters");
        for (int i = 0; i < parsedChapters.size(); ++i)
            require(parsedChapters[i].title == restoredChapters[i].title
                            && parsedChapters[i].page == restoredChapters[i].page
                            && parsedChapters[i].position == restoredChapters[i].position,
                    "A restored chapter moved");
        const int middle = parsed.pages() / 2;
        require(restored.goTo(parsed.pageStart(middle)) && restored.page() == middle,
                "A parsed position did not resolve in the restored book");
        const auto prefixSettings =
                kobalt::qt::object({ { "text_alignment", "left" }, { "bold_reading", "bold" } });
        kobalt::EngineDocument emphasized(kobalt::test::readerFonts()),
                emphasizedAgain(kobalt::test::readerFonts()),
                plainAgain(kobalt::test::readerFonts());
        opened(emphasized, fingerprint, policy, prefixSettings);
        require(!emphasized.openedFromCache(), "Bionic reading reused cached normal pagination");
        const auto prefixImage = hash(emphasized.image(0));
        require(emphasized.saveDocumentCache(), "Cannot cache Bionic reading");
        opened(emphasizedAgain, fingerprint, policy, prefixSettings);
        require(emphasizedAgain.openedFromCache() && hash(emphasizedAgain.image(0)) == prefixImage,
                "Cached Bionic reading changed its rendering");
        require(emphasizedAgain.applyAppearance(settings) && emphasizedAgain.saveDocumentCache(),
                "Cannot cache a book after disabling Bionic reading");
        opened(plainAgain, fingerprint, policy, settings);
        require(plainAgain.openedFromCache(),
                "A mode change saved the wrong document cache identity");
        require(hash(plainAgain.image(0)) == hash(parsed.image(0)),
                "A mode change saved different cached pages");
        // A different typography reuses the parsed book but lays it out again.
        const auto larger =
                kobalt::qt::object({ { "text_alignment", "left" }, { "font_pixels", 44 } });
        kobalt::EngineDocument parsedLarger(kobalt::test::readerFonts()),
                restoredLarger(kobalt::test::readerFonts());
        parsedLarger.setDocumentCache({}, {}, {}, 0);
        require(parsedLarger.open(bytes, QSize(1072, 1328), larger, &error), qPrintable(error));
        opened(restoredLarger, fingerprint, policy, larger);
        require(restoredLarger.openedFromCache() && restoredLarger.pages() == parsedLarger.pages(),
                "Cached book did not lay out new typography");
        for (int i = 0; i < parsedLarger.pages(); i += 3)
            require(parsedLarger.go(i) && restoredLarger.go(i)
                            && hash(parsedLarger.image(i)) == hash(restoredLarger.image(i)),
                    "A cached book drew different pages with new typography");
        require(restoredLarger.saveDocumentCache(),
                "Cannot save changed typography to the existing cache");
        kobalt::EngineDocument largerAgain(kobalt::test::readerFonts());
        opened(largerAgain, fingerprint, policy, larger);
        require(largerAgain.openedFromCache() && largerAgain.pages() == parsedLarger.pages(),
                "Changed typography was not saved in the document cache");
        for (int i = 0; i < parsedLarger.pages(); i += 3)
            require(largerAgain.go(i) && hash(largerAgain.image(i)) == hash(parsedLarger.image(i)),
                    "A saved typography change restored different pages");
        kobalt::EngineDocument otherPolicy(kobalt::test::readerFonts()),
                otherBook(kobalt::test::readerFonts());
        opened(otherBook, QByteArray(64, 'b'), policy, settings);
        require(!otherBook.openedFromCache(), "Another fingerprint reused a parsed book");
        require(otherBook.saveDocumentCache(), "Cannot cache another book");
        const auto oldEntries = QDir(cacheDirectory).entryList({ "*.cr3" }, QDir::Files);
        require(oldEntries.size() == 2, "Cache retained superseded reading modes");
        {
            kobalt::DocumentCacheScope cache(cacheDirectory);
            require(cache.enabled(), "Cannot initialize cache cleanup check");
            require(!ldomDocCache::removeObsolete(
                            Utf8ToUnicode(lString8(fingerprint.constData())),
                            Utf8ToUnicode(lString8(
                                    (cacheDirectory + "/" + oldEntries.first() + ".missing")
                                            .toUtf8()
                                            .constData()))),
                    "Cache cleanup accepted a missing replacement");
        }
        require(QDir(cacheDirectory).entryList({ "*.cr3" }, QDir::Files) == oldEntries,
                "A missing replacement removed old caches");
        opened(otherPolicy, fingerprint, "changed-policy", settings);
        require(!otherPolicy.openedFromCache(), "A changed cache policy reused a parsed book");
        require(otherPolicy.saveDocumentCache(), "Cannot save the changed cache policy");
        const auto newEntries = QDir(cacheDirectory).entryList({ "*.cr3" }, QDir::Files);
        require(newEntries.size() == 2 && newEntries != oldEntries,
                "Saving a new cache retained the old policy or removed another book");
        opened(otherPolicy, fingerprint, "changed-policy", settings);
        opened(otherBook, QByteArray(64, 'b'), policy, settings);
        require(otherPolicy.openedFromCache() && otherBook.openedFromCache(),
                "Cache cleanup lost the replacement, another book or their index entries");
    }
    puts("PASS: document-cache pixels, chapters, typography, Bionic modes, fingerprints and obsolete entries");
}
