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

// Run the writer with the previous engine and the checker with its replacement.
// The checker requires a real cache hit, then compares it with a fresh parse.
static void checkCompatibility(const QByteArray &bytes, const QString &root, bool write)
{
    QString variableFamily;
    for (const auto &font : kobalt::discoverReadingFonts(kobalt::test::readerFonts().sources))
        if (font.opticalSizeAxis.available()) {
            variableFamily = font.family;
            break;
        }
    require(!variableFamily.isEmpty(), "Missing compatibility variable-font fixture");
    const auto fingerprint = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    const QByteArray policy(KOBALT_LAYOUT_POLICY);
    for (int scene = 0; scene < 5; ++scene) {
        const auto directory = root + "/" + QString::number(scene) + "/.cache";
        require(QDir().mkpath(QFileInfo(directory).absolutePath()),
                "Cannot create compatibility cache root");
        const QSize viewport(1072, 1328);
        auto settings = kobalt::qt::object(
                { { "text_alignment", "left" }, { "bold_reading", scene == 1 ? "bold" : "off" } });
        if (scene == 2 || scene == 3) {
            settings["font_face"] = variableFamily;
            settings["font_optical_size"] = scene == 2 ? 12 : 36;
        }
        const QByteArray css =
                scene == 4 ? QByteArray("body{writing-mode:vertical-rl}") : QByteArray();
        QString error;
        const auto prepare = [&](kobalt::EngineDocument &document, bool cached) {
            document.setUserStyleSheet(css);
            if (cached)
                document.setDocumentCache(directory, fingerprint, policy, 1024 * 1024);
            require(document.open(bytes, viewport, settings, &error), qPrintable(error));
        };
        if (write) {
            kobalt::EngineDocument document(kobalt::test::readerFonts());
            prepare(document, true);
            require(!document.openedFromCache(), "Compatibility writer requires an empty cache");
            require(document.saveDocumentCache(), "Cannot write compatibility cache");
        } else {
            const auto before = QDir(directory).entryList({ "*.cr3" }, QDir::Files);
            require(before.size() == 1, "Missing previous-engine compatibility cache");
            {
                kobalt::EngineDocument cached(kobalt::test::readerFonts()),
                        fresh(kobalt::test::readerFonts());
                prepare(cached, true);
                require(cached.openedFromCache(),
                        "The upgraded engine discarded the previous cache");
                prepare(fresh, false);
                require(cached.pages() == fresh.pages() && cached.title() == fresh.title(),
                        "Previous-engine cache changed page count or title");
                for (int page = 0; page < fresh.pages(); ++page) {
                    require(fresh.go(page) && cached.go(page), "Cannot select compatibility page");
                    const auto freshImage = fresh.image(page), cachedImage = cached.image(page);
                    require(hash(freshImage) == hash(cachedImage),
                            "Previous-engine cached page differs from a fresh upgraded parse");
                }
                const int middle = fresh.pages() / 2;
                require(cached.goTo(fresh.pageStart(middle)) && cached.page() == middle,
                        "Previous-engine cache changed a reading anchor");
                const auto a = cached.chapters(), b = fresh.chapters();
                require(a.size() == b.size(), "Previous-engine cache lost chapters");
                for (int i = 0; i < a.size(); ++i)
                    require(a[i].title == b[i].title && a[i].page == b[i].page
                                    && a[i].position == b[i].position,
                            "Previous-engine cache changed a chapter anchor");
                require(cached.saveDocumentCache(), "Cannot save the upgraded cached layout");
            }
            require(QDir(directory).entryList({ "*.cr3" }, QDir::Files) == before,
                    "A compatible update replaced the parsed-book cache identity");
            const auto path = directory + "/" + before.first();
            const auto oldTime = QDateTime::fromSecsSinceEpoch(946684800);
            QFile file(path);
            require(file.open(QIODevice::ReadWrite)
                            && file.setFileTime(oldTime, QFileDevice::FileModificationTime),
                    "Cannot set compatibility cache timestamp");
            file.close();
            kobalt::EngineDocument reopened(kobalt::test::readerFonts());
            prepare(reopened, true);
            require(reopened.openedFromCache() && reopened.saveDocumentCache(),
                    "Cannot reopen the upgraded cache");
            require(QFileInfo(path).lastModified() == oldTime,
                    "The upgraded cache rebuilt its layout on every reopening");
        }
    }
    puts(write ? "PASS: previous-engine caches written"
               : "PASS: previous-engine cache reuse, pixels, anchors and stable reopening");
}

// Checks parsed and cached pixel equality, chapters, typography changes, Bionic modes, fingerprints and cache policies.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const bool compatibility = argc == 4
            && (QByteArray(argv[1]) == "--write-compatibility"
                || QByteArray(argv[1]) == "--check-compatibility");
    require(argc == 2 || compatibility,
            "Pass the document-cache EPUB fixture or compatibility mode");
    QFile file(QString::fromLocal8Bit(argv[compatibility ? 2 : 1]));
    require(file.open(QIODevice::ReadOnly), "Cannot read document-cache fixture");
    const auto bytes = file.readAll();
    if (compatibility) {
        checkCompatibility(bytes, QString::fromLocal8Bit(argv[3]),
                           QByteArray(argv[1]) == "--write-compatibility");
        return 0;
    }
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
    QTemporaryDir compatibilityRoot;
    require(compatibilityRoot.isValid(), "Cannot create the cache compatibility fixture");
    checkCompatibility(bytes, compatibilityRoot.path(), true);
    checkCompatibility(bytes, compatibilityRoot.path(), false);
    puts("PASS: document-cache pixels, chapters, typography, Bionic modes, fingerprints and obsolete entries");
}
