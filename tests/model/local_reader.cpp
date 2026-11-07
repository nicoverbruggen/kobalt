#include "check.h"
#include "publication/local_resources.h"
#include "reader_inputs.h"
#include "reading/reader_session.h"
#include "support/diagnostics.h"
#include "support/strings.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace kobalt;

namespace {
QByteArray pixels(const QImage &image)
{
    return QCryptographicHash::hash(
            QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()),
            QCryptographicHash::Sha256);
}

int scanEpubFolder(const QString &path)
{
    const QFileInfo root(path);
    if (!root.isDir() || !root.isReadable() || !root.isExecutable()) {
        qCritical() << "Cannot scan EPUB folder:" << path;
        return 2;
    }
    const QDir folder(root.absoluteFilePath());
    QStringList books;
    bool incomplete = false;
    QDirIterator entries(folder.absolutePath(),
                         QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot
                                 | QDir::NoSymLinks,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        entries.next();
        const auto info = entries.fileInfo();
        if (info.isDir()) {
            if (!info.isReadable() || !info.isExecutable()) {
                qWarning() << "Cannot scan subfolder:" << folder.relativeFilePath(info.filePath());
                incomplete = true;
            }
        } else if (info.isFile() && info.suffix().compare("epub", Qt::CaseInsensitive) == 0) {
            books.append(info.absoluteFilePath());
        }
    }
    if (books.isEmpty()) {
        qCritical() << "No EPUB files found in" << folder.absolutePath();
        return 2;
    }
    if (!test::readerAssets().available()) {
        qCritical() << "Kobalt test assets are unavailable.";
        return 2;
    }
    std::sort(books.begin(), books.end());
    const auto fonts = test::readerFonts();
    qsizetype failed = 0;
    for (const auto &book : books) {
        const auto relative = folder.relativeFilePath(book);
        qInfo() << "Checking" << relative;
        auto session = std::make_unique<ReaderSession>(fonts, "Libron");
        auto lease = std::make_shared<SourceLease>();
        // The local backend and ReaderSession use the same archive checks as
        // desktop opening. ReaderSession also returns the typed failure.
        OpenRequest request{ book, book, QSize(758, 900), {},
                             {},   {},   lease,           localResourceFactory(book, lease) };
        OpeningResult outcome;
        QEventLoop opened;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool finished = false;
        QObject::connect(&timeout, &QTimer::timeout, &opened, [&] {
            finished = true;
            outcome =
                    OpeningResult::failed(OpeningFailureKind::Cancelled, "epub-folder.open-timeout",
                                          "Opening exceeded 120 seconds.");
            session->cancelSource();
            opened.quit();
        });
        timeout.start(120000);
        session->open(std::move(request), [&](OpeningResult result) {
            if (!finished) {
                finished = true;
                outcome = std::move(result);
                opened.quit();
            }
        });
        if (!finished)
            opened.exec();
        timeout.stop();
        if (outcome.ready() && session->image().isNull())
            outcome = OpeningResult::failed(OpeningFailureKind::InvalidContent,
                                            "epub-folder.no-first-page",
                                            "The reader produced no first page.");
        if (const auto *failure = outcome.failure()) {
            ++failed;
            const auto code = QString::fromLatin1(failure->reason);
            const auto detail = failure->detail.isEmpty()
                    ? strings::get(failure->reason.constData())
                    : failure->detail;
            auto line = qInfo();
            line << "FAIL" << relative << code;
            if (detail != code)
                line << detail;
        } else {
            qInfo() << "PASS" << relative;
        }

        // Finish any neighbour rendering and release the document before
        // opening the next book in the same process.
        QEventLoop drained;
        QTimer::singleShot(10000, &drained, [&] {
            qCritical() << "Scan incomplete. The engine did not stop after checking" << relative;
            std::_Exit(2);
        });
        ReaderSession::drain([&] {
            session.reset();
            ReaderSession::drain([&] { drained.quit(); });
        });
        drained.exec();
    }
    qInfo().nospace() << "EPUB scan: " << books.size() - failed << " opened, " << failed
                      << " failed, " << books.size() << " checked.";
    if (incomplete) {
        qWarning() << "Scan incomplete because some subfolders could not be read.";
        return 2;
    }
    return failed ? 1 : 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && qstrcmp(argv[1], "--epub-folder") == 0) {
        if (argc != 3 || !argv[2][0]) {
            std::fputs("Usage: kobalt-local-reader-tests --epub-folder DIRECTORY\n", stderr);
            return 2;
        }
        const auto folder = QString::fromLocal8Bit(argv[2]);
        // Keep folder arguments out of Qt's platform argument parser.
        int qtArgc = 1;
        char *qtArgv[]{ argv[0], nullptr };
        qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
        QGuiApplication app(qtArgc, qtArgv);
        QTimer::singleShot(0, &app, [&] { app.exit(scanEpubFolder(folder)); });
        return app.exec();
    }
    QGuiApplication app(argc, argv);
    require(argc == 3, "Expected local-cache and illustrated EPUB fixtures");
    QTemporaryDir directory;
    require(directory.isValid(), "temporary cache directory");
    initializeDiagnostics(directory.path() + "/diagnostics");
    QTimer::singleShot(0, &app, [&] {
        std::unique_ptr<ReaderSession> session;
        std::unique_ptr<native::PublicationEndpoint> source;
        const auto cache = directory.path() + "/documents";
        const auto stylePath = directory.path() + "/userstyle.css";
        auto writeStyle = [&](const QByteArray &css) {
            QFile file(stylePath);
            require(file.open(QIODevice::WriteOnly) && file.write(css) == css.size(),
                    "Cannot write user CSS");
        };
        QJsonObject retainedSettings;
        auto retainedHit = [&] {
            flushDiagnostics();
            QFile log(directory.path() + "/diagnostics/events.jsonl");
            require(log.open(QIODevice::ReadOnly), "Cannot read opening diagnostics");
            QJsonObject last;
            while (!log.atEnd()) {
                const auto entry = QJsonDocument::fromJson(log.readLine()).object();
                if (entry.value("event").toString() == "opening-phases") {
                    last = entry;
                }
            }
            require(!last.isEmpty(), "No opening phases were recorded");
            return last.value("retained_book_hit").toBool();
        };
        auto release = [&](bool keep = false) {
            retainedSettings = keep ? session->state() : QJsonObject{};
            if (keep) {
                retainedSettings["fingerprint"] = QString::fromLatin1(session->sourceHash());
            }
            auto retained = session->retained();
            QEventLoop drained;
            ReaderSession::drain([&] {
                session.reset();
                if (!keep) {
                    retained.reset();
                }
                ReaderSession::drain([&] {
                    source.reset();
                    drained.quit();
                });
            });
            drained.exec();
            session = std::move(retained);
        };
        auto open = [&](const QString &path, bool cancelled = false) {
            source = native::openLocalPublication(path);
            require(source && source->port() == 0, "Local reader requires a socket");
            auto lease = std::make_shared<SourceLease>();
            if (cancelled) {
                lease->cancel();
            }
            if (!session) {
                session = std::make_unique<ReaderSession>(test::readerFonts(), "Libron");
            }
            OpenRequest request{ path,
                                 path,
                                 QSize(758, 900),
                                 retainedSettings,
                                 {},
                                 directory.path() + "/layout.json",
                                 lease,
                                 source->resources(lease) };
            request.documentCachePath = cache;
            request.userStyleSheetPath = stylePath;
            QEventLoop opened;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &opened,
                             [] { require(false, "Local reader timed out"); });
            timeout.start(15000);
            bool finished = false;
            OpeningResult outcome;
            session->open(std::move(request), [&](OpeningResult result) {
                finished = true;
                outcome = std::move(result);
                opened.quit();
            });
            opened.exec();
            require(finished, "Local reader did not report its outcome");
            return outcome;
        };
        const auto path = QString::fromLocal8Bit(argv[1]);
        require(open(path).ready() && !session->image().isNull(), "Local EPUB did not render");
        require(!session->resourceStats()["model_cache_hit"].toBool(),
                "Cold opening reused a document");
        require(!QFile::exists(stylePath), "Opening created an unsolicited user stylesheet");
        const auto first = pixels(session->image());
        const auto pages = session->state()["pages"].toInt();
        release();
        require(!QDir(cache).entryList({ "*.cr3" }, QDir::Files).isEmpty(),
                "Local opening did not save a document cache");
        require(open(path).ready() && session->resourceStats()["model_cache_hit"].toBool(),
                "Local EPUB did not reopen from its document cache");
        require(session->state()["pages"].toInt() == pages && pixels(session->image()) == first,
                "Local cached reopening changed the page");
        require(session->resourceStats()["peak_bytes"].toDouble() <= ResourceCache::budgetBytes,
                "Local opening exceeded the source-cache budget");
        release();
        writeStyle("* { color: #777777 !important; } p { line-height: 250% !important; }");
        require(open(path).ready(), "User CSS prevented opening");
        const auto styled = pixels(session->image());
        require(styled != first && session->state()["pages"].toInt() != pages,
                "User CSS did not override text colour and the Text line-spacing setting");
        require(!session->resourceStats()["model_cache_hit"].toBool(),
                "Changed CSS reused stale disk pages");
        release();
        require(open(path).ready() && pixels(session->image()) == styled
                        && session->resourceStats()["model_cache_hit"].toBool(),
                "Unchanged CSS did not reuse matching cached pages");
        release(true);
        require(open(path).ready() && pixels(session->image()) == styled && retainedHit(),
                "Unchanged CSS did not reuse matching retained pages");
        release(true);
        writeStyle("* { color: #aaaaaa !important; }");
        require(open(path).ready() && pixels(session->image()) != styled
                        && pixels(session->image()) != first && !retainedHit(),
                "Edited CSS reused the retained book's styles");
        release(true);
        require(QFile::remove(stylePath), "Cannot remove user CSS");
        require(open(path).ready() && pixels(session->image()) == first && !retainedHit(),
                "Removing CSS did not restore default rendering on a retained book");
        release(true);
        writeStyle({});
        require(open(path).ready() && pixels(session->image()) == first,
                "Empty CSS changed rendering");
        release();
        QByteArray maximumStyle("* { color: #777777 !important; }");
        maximumStyle += QByteArray(256 * 1024 - maximumStyle.size(), ' ');
        writeStyle(maximumStyle);
        require(open(path).ready() && pixels(session->image()) != first,
                "Maximum-size CSS was ignored");
        release();
        writeStyle(QByteArray(256 * 1024 + 1, 'x'));
        require(open(path).ready() && pixels(session->image()) == first,
                "Oversized CSS prevented normal rendering");
        release();
        writeStyle(QByteArray("invalid\0CSS", 11));
        require(open(path).ready() && pixels(session->image()) == first,
                "NUL-containing CSS changed rendering");
        release();
        require(QFile::remove(stylePath) && QDir().mkdir(stylePath),
                "Cannot create unreadable CSS fixture");
        require(open(path).ready() && pixels(session->image()) == first,
                "Unreadable CSS prevented normal rendering");
        release();
        require(QDir().rmdir(stylePath), "Cannot remove unreadable CSS fixture");
        writeStyle("/* an unfinished comment");
        require(open(path).ready(), "Malformed CSS caused an installation-style opening failure");
        release();
        require(QFile::remove(stylePath), "Cannot remove malformed CSS");
        const auto cancelled = open(path, true);
        require(!cancelled.ready() && cancelled.failure()->kind == OpeningFailureKind::Cancelled
                        && session->image().isNull(),
                "Cancelled local opening restored cached pages");
        release();
        require(open(QString::fromLocal8Bit(argv[2])).ready() && !session->image().isNull(),
                "Illustrated EPUB did not render through local resources");
        release();
        std::puts(
                "PASS: local EPUB rendering, parsed-cache reopening, user CSS edits, retained/disk cache invalidation, source budget and cancellation");
        app.quit();
    });
    return app.exec();
}
