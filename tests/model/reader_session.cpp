#include "reader_inputs.h"
#include "reading/reader_session.h"
#include "sync/kosync.h"
#include "app/messages/opening_message.h"
#include "publication/native_resources.h"
#include "plain_archive.h"
#include "publication/resource_server.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include "publication/zip_resource.h"
#include "support/strings.h"
#include "check.h"
#include <QGuiApplication>
#include <QJsonDocument>
#include <QEventLoop>
#include <QFileInfo>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QUrl>
#include <functional>

using namespace kobalt;

int run(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 3, "Expected text and non-linear synthetic EPUBs");
    const auto path = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const auto publisherPath = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    QString servedPath = path;
    // Serve the synthetic archive with the same framing as the resource
    // adapter, so the GUI facade and engine worker both run in this check.
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "Could not start fixture server");
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            auto request = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(
                    socket, &QTcpSocket::readyRead, socket, [socket, request, &servedPath] {
                        *request += socket->readAll();
                        if (!request->contains("\r\n\r\n"))
                            return;
                        QString name;
                        for (const auto &line : request->split('\n'))
                            if (line.startsWith("filename: "))
                                name = QUrl::fromPercentEncoding(line.mid(10).trimmed());
                        ZipResource resource;
                        require(resource.open(servedPath, name), "Fixture resource was not found");
                        QByteArray bytes;
                        while (resource.valid() && !resource.finished())
                            bytes += resource.next();
                        require(resource.valid(), "Fixture resource was damaged");
                        socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: "
                                      + QByteArray::number(bytes.size())
                                      + "\r\nConnection: close\r\n\r\n" + bytes);
                        socket->disconnectFromHost();
                    });
        }
    });
    kobalt::qt::later(0, &app, [&] {
        readingPpi.store(212);
        auto session =
                std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        auto lease = std::make_shared<SourceLease>();
        PositionCodec firstPositions{ [](const EngineDocument &model, const QString &, QString *) {
                                         return model.position();
                                     },
                                      [](const EngineDocument &model, QString *) {
                                          return "first:" + model.position();
                                      },
                                      [](const QString &location) { return location; } };
        auto secondPositions = firstPositions;
        secondPositions.encode = [](const EngineDocument &model, QString *) {
            return "second:" + model.position();
        };

        QEventLoop opened;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &opened,
                         [] { require(false, "Session check timed out"); });
        timeout.start(60000);
        QTemporaryDir malformed;
        require(malformed.isValid(), "Cannot create opening fixtures");
        const auto reject = [&](OpenRequest request, OpeningFailureKind kind, const char *reason,
                                bool remembered = false) {
            bool finished = false;
            session->open(std::move(request), [&](OpeningResult result) {
                require(!result.ready() && result.failure(), "Failed opening reported ready");
                const auto mismatch =
                        QString("Opening failure: expected kind %1, reason %2; got kind %3, reason %4")
                                .arg(int(kind))
                                .arg(QString::fromLatin1(reason))
                                .arg(int(result.failure()->kind))
                                .arg(QString::fromLatin1(result.failure()->reason));
                require(result.failure()->kind == kind && result.failure()->reason == reason,
                        qPrintable(mismatch));
                require(result.rememberRejection() == remembered,
                        "Opening failure changed fallback caching");
                require(!openingMessage(result).isEmpty(), "Opening failure lost its presentation");
                finished = true;
                opened.quit();
            });
            opened.exec();
            require(finished && session->image().isNull() && !session->retained(),
                    "Failed opening retained a readable session");
        };
        int factoryCalls = 0;
        ResourceFactory unavailable = [&](const QMap<QString, qint64> &, const QString &,
                                          std::shared_ptr<std::atomic_bool>) {
            ++factoryCalls;
            return std::shared_ptr<ResourceCache>{};
        };
        const auto request = [&](const QString &file, std::shared_ptr<SourceLease> access,
                                 ResourceFactory factory) {
            return OpenRequest{ file, file, QSize(758, 900),   {},
                                {},   {},   std::move(access), std::move(factory) };
        };
        for (bool keyMode : { false, true }) {
            strings::showKeys(keyMode);
            auto cancelled = std::make_shared<SourceLease>();
            cancelled->cancel();
            cancelled->revoke();
            reject(request(path, cancelled, unavailable), OpeningFailureKind::Cancelled,
                   "error.opening_interrupted");
            auto revoked = std::make_shared<SourceLease>();
            revoked->revoke();
            revoked->cancel();
            reject(request(path, revoked, unavailable), OpeningFailureKind::Revoked,
                   "error.opening_interrupted");
            reject(request(path, std::make_shared<SourceLease>(), {}),
                   OpeningFailureKind::HostUnavailable, "error.opening_interrupted");
            reject(request(malformed.path() + QStringLiteral("/missing.epub"),
                           std::make_shared<SourceLease>(), unavailable),
                   OpeningFailureKind::Io, "error.cannot_read_the_publication");
            require(factoryCalls == 0, "Invalid source invoked a resource factory");
        }
        strings::showKeys(false);
        QFile fixtureFile(path);
        require(fixtureFile.open(QIODevice::ReadOnly), "Cannot read synthetic archive");
        const auto archiveBytes = fixtureFile.readAll();
        const int directory = archiveBytes.indexOf(QByteArray("PK\1\2", 4));
        require(directory >= 0, "Synthetic archive has no directory");
        const auto damaged = [&](QByteArray bytes, OpeningFailureKind kind, const char *reason) {
            const auto file = malformed.path() + QStringLiteral("/damaged.epub");
            {
                QFile output(file);
                require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size(),
                        "Cannot write synthetic opening fixture");
            }
            reject(request(file, std::make_shared<SourceLease>(), unavailable), kind, reason);
            require(factoryCalls == 0, "Rejected archive invoked a resource factory");
        };
        auto encrypted = archiveBytes;
        encrypted[directory + 8] = char(uchar(encrypted[directory + 8]) | 1);
        damaged(encrypted, OpeningFailureKind::Protected, "error.unsupported_publication_member");
        auto invalid = archiveBytes;
        invalid[directory] = 'X';
        damaged(invalid, OpeningFailureKind::InvalidContent, "error.invalid_publication_directory");
        damaged(QByteArray(21, 'x'), OpeningFailureKind::Unsupported,
                "error.unsupported_publication_size");
        reject(request(path, std::make_shared<SourceLease>(), unavailable),
               OpeningFailureKind::HostUnavailable, "error.cannot_read_the_publication");
        require(factoryCalls == 1, "Unavailable resource factory was not called once");
        for (bool rejected : { false, true }) {
            auto access = std::make_shared<SourceLease>();
            ResourceFactory broken = [access,
                                      rejected](const QMap<QString, qint64> &sizes, const QString &,
                                                std::shared_ptr<std::atomic_bool> contentRejected) {
                return std::make_shared<ResourceCache>(
                        sizes.keys(),
                        ResourceCache::Access{
                                [contentRejected, rejected](const QString &,
                                                            const ResourceCache::Sink &,
                                                            QString *error) {
                                    contentRejected->store(rejected);
                                    *error =
                                            strings::get("error.this_publication_is_lcp_protected");
                                    return false;
                                },
                                [access] { return access->revoked(); },
                                {},
                                {} });
            };
            // Even protection wording cannot establish protection. Only the
            // validation flag distinguishes a content rejection from failed I/O.
            reject(request(path, access, broken),
                   rejected ? OpeningFailureKind::InvalidContent : OpeningFailureKind::Io,
                   "error.publication_layout_inspection_is_incomplete", rejected);
        }
        OpeningResult ready;
        ready.rememberContentRejection();
        require(ready.ready() && !ready.failure() && !ready.rememberRejection(),
                "Ready result acquired a failure disposition");
        for (auto kind : { OpeningFailureKind::Cancelled, OpeningFailureKind::Protected,
                           OpeningFailureKind::Unsupported, OpeningFailureKind::InvalidContent,
                           OpeningFailureKind::Io, OpeningFailureKind::Revoked,
                           OpeningFailureKind::HostUnavailable }) {
            QByteArray reason("owned-opening-reason");
            const auto original =
                    OpeningResult::failed(kind, reason.constData(), "original detail");
            reason.fill('x');
            auto copy = original;
            copy.rememberContentRejection();
            require(!original.ready() && original.failure()->kind == kind
                            && !original.rememberRejection()
                            && original.failure()->reason == "owned-opening-reason"
                            && original.failure()->detail == "original detail",
                    "Mutating an opening result copy changed the original failure");
            require(copy.rememberRejection()
                            && copy.failure()->kind
                                    == (kind == OpeningFailureKind::Io
                                                ? OpeningFailureKind::InvalidContent
                                                : kind),
                    "Remembering a copied rejection changed its failure category");
            ready = copy;
            require(!ready.ready() && ready.rememberRejection()
                            && ready.failure()->kind == copy.failure()->kind,
                    "Failure assignment lost the opening result");
            copy = OpeningResult{};
            copy.rememberContentRejection();
            require(copy.ready() && !copy.failure() && !copy.rememberRejection()
                            && ready.rememberRejection(),
                    "Ready assignment retained a failure or changed another result");
        }
        {
            QTemporaryDir diagnostics;
            require(diagnostics.isValid(), "Cannot create warning diagnostics directory");
            initializeDiagnostics(diagnostics.path());
            const auto warningPath =
                    QFileInfo(path).dir().filePath("formatting-unsupported-writing.epub");
            servedPath = warningPath;
            auto warning =
                    std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
            const auto openWarning = [&](ReaderSession &reader) {
                auto access = std::make_shared<SourceLease>();
                bool finished = false;
                auto opening =
                        request(warningPath, access,
                                nativeResourceFactory(warningPath, server.serverPort(), access));
                if (!reader.sourceHash().isEmpty()) {
                    opening.saved = reader.state();
                    opening.saved["fingerprint"] = QString::fromLatin1(reader.sourceHash());
                }
                reader.open(std::move(opening), [&](OpeningResult outcome) {
                    require(outcome.ready(), qPrintable(openingMessage(outcome)));
                    require(!outcome.rememberRejection(),
                            "Readable imperfect book was remembered as rejected");
                    finished = true;
                    opened.quit();
                });
                opened.exec();
                require(finished && reader.formattingWarning() && !reader.image().isNull(),
                        "Readable imperfect book lost its warning or frame");
            };
            openWarning(*warning);
            auto retainedWarning = warning->retained();
            require(bool(retainedWarning) && retainedWarning->formattingWarning(),
                    "Retention lost the formatting warning");
            warning->cancelSource();
            ReaderSession::drain([&] { opened.quit(); });
            opened.exec();
            warning.reset();
            openWarning(*retainedWarning);
            flushDiagnostics();
            QFile log(diagnostics.path() + "/events.jsonl");
            require(log.open(QIODevice::ReadOnly), "Cannot read formatting diagnostics");
            int warnings = 0;
            for (const auto &line : log.readAll().split('\n')) {
                const auto entry = QJsonDocument::fromJson(line).object();
                if (entry.value("event").toString()
                    != QStringLiteral("opening-formatting-warning")) {
                    continue;
                }
                require(entry.value("resource").toString().endsWith(".xhtml")
                                && entry.value("issue").toString().contains("writing")
                                && entry.value("excerpt").toString().contains("vertical-lr"),
                        "Formatting warning log lost its cause, resource or excerpt");
                require(entry.value("retained_book_hit").toBool() == (warnings == 1),
                        "Retained warning log lost its origin");
                ++warnings;
            }
            require(warnings == 2, "Expected initial and retained formatting warning diagnostics");
            retainedWarning->cancelSource();
            ReaderSession::drain([&] { opened.quit(); });
            opened.exec();
            servedPath = path;
        }
        lease = std::make_shared<SourceLease>();
        session->open({ path,
                        path,
                        QSize(758, 900),
                        {},
                        {},
                        {},
                        lease,
                        nativeResourceFactory(path, server.serverPort(), lease),
                        firstPositions },
                      [&](OpeningResult result) {
                          require(result.ready(), qPrintable(openingMessage(result)));
                          opened.quit();
                      });
        require(!session->canTurn() && !session->turn(1), "An opening accepted a page turn");
        opened.exec();
        require(!session->image().isNull() && session->pages() > 0,
                "Session did not open a readable frame");
        require(session->syncFilenameDigest() == kosync::filenameDigest(path),
                "Opened session exposes the original filename digest");
        require(session->syncDigest().size() == 32,
                "Opened session exposes KOReader's document digest");
        {
            const auto anchor = session->state().value("position").toString();
            const auto originalFrame = session->image().cacheKey();
            session->describeSyncedPosition(anchor, [&](SyncedPositionDescription value) {
                require(value.comparison.status == PositionComparison::Status::Compared
                                && value.comparison.direction == 0,
                        "Sync preview compares the current position exactly without navigating");
                require(value.chapter == session->chapterTitle(),
                        "Sync preview resolves the same chapter as the reading header");
                opened.quit();
            });
            opened.exec();
            session->describeSyncedPosition(
                    "/not/a/real/position", [&](SyncedPositionDescription value) {
                        require(value.comparison.status == PositionComparison::Status::Failed
                                        && value.chapter.isEmpty(),
                                "An invalid remote position has no guessed chapter or direction");
                        opened.quit();
                    });
            opened.exec();
            QString later;
            session->pageSummary(session->pages() - 1, [&](QString value, QString) {
                later = value;
                opened.quit();
            });
            opened.exec();
            session->describeSyncedPosition(later, [&](SyncedPositionDescription value) {
                require(value.comparison.status == PositionComparison::Status::Compared
                                && value.comparison.direction > 0,
                        "A later source position is identified as forward");
                opened.quit();
            });
            opened.exec();
            require(session->state().value("position").toString() == anchor
                            && session->image().cacheKey() == originalFrame,
                    "Sync previews preserve the local anchor and rendered image");
            require(session->goToSyncedPosition(later,
                                                [&](QString error) {
                                                    require(error.isEmpty(), "Open later position");
                                                    opened.quit();
                                                }),
                    "Start later position import");
            opened.exec();
            session->describeSyncedPosition(anchor, [&](SyncedPositionDescription value) {
                require(value.comparison.status == PositionComparison::Status::Compared
                                && value.comparison.direction < 0,
                        "An earlier source position is identified as backward");
                opened.quit();
            });
            opened.exec();
            require(session->goToSyncedPosition(anchor,
                                                [&](QString error) {
                                                    require(error.isEmpty(),
                                                            "Restore local anchor");
                                                    opened.quit();
                                                }),
                    "Restore after direction checks");
            opened.exec();
            bool completed = false;
            require(session->goToSyncedPosition(
                            "/not/a/real/position",
                            [&](QString error) {
                                require(!error.isEmpty()
                                                && session->state().value("position").toString()
                                                        == anchor
                                                && session->error().isEmpty(),
                                        "Invalid sync position preserves the readable frame");
                                completed = true;
                                opened.quit();
                            }),
                    "Sync import starts on the engine worker");
            opened.exec();
            require(completed, "Invalid sync position completes");
            require(session->goToSyncedPosition(anchor,
                                                [&](QString error) {
                                                    require(error.isEmpty(),
                                                            "Valid sync position imports");
                                                    opened.quit();
                                                }),
                    "Valid sync import starts");
            opened.exec();
        }
        {
            const auto rawImage = session->image();
            const auto emptyGeneration = session->markupGeneration();
            require(session->setMarkup({}) == emptyGeneration,
                    "Unchanged empty markup changed generation");
            bool completed = false, accepted = false;
            session->captureMarkup([&](bool ready) {
                completed = true;
                accepted = ready;
            });
            require(completed && accepted && session->markup().resolvedRanges == 0,
                    "Empty markup must complete without a worker job");
            require(session->image().cacheKey() == rawImage.cacheKey(),
                    "Markup capture rerendered raw pixels");
            QVector<SearchHit> hits;
            completed = false;
            session->search("reader", [&](SearchBatch batch) {
                hits += batch.hits;
                if (batch.done) {
                    completed = true;
                    opened.quit();
                }
            });
            if (!completed) {
                opened.exec();
            }
            require(hits.size() > 1, "Markup fixture needs source ranges on several pages");
            ReaderMarkupRequest request;
            request.saved.append({ hits.first().start, hits.last().end, "blue" });
            request.draftStart = hits.first().start;
            request.draftEnd = hits.last().end;
            const auto generation = session->setMarkup(request);
            require(generation > emptyGeneration && session->setMarkup(request) == generation,
                    "Markup snapshot must change generation only when its content changes");
            auto invalid = request;
            invalid.draftStart = QString(4097, 'x');
            require(session->setMarkup(invalid) == generation,
                    "Oversized source anchor changed markup");
            invalid = request;
            invalid.saved.resize(2001);
            require(session->setMarkup(invalid) == generation,
                    "Oversized saved range set changed markup");
            auto capture = [&] {
                completed = false;
                accepted = false;
                session->captureMarkup([&](bool ready) {
                    completed = true;
                    accepted = ready;
                    opened.quit();
                });
                if (!completed) {
                    opened.exec();
                }
                return accepted;
            };
            require(capture(), "Could not capture current-page markup");
            require(session->markup().request == request
                            && session->markup().generation == generation
                            && session->markup().imageKey == rawImage.cacheKey()
                            && session->markup().resolvedRanges == 2,
                    "Markup did not belong to the exact image/request or resolved ranges twice");
            require(!session->markup().saved.first().rectangles.isEmpty()
                            && !session->markup().draft.rectangles.isEmpty(),
                    "Current visible saved and draft markup must both be captured");
            int callbacks = 0;
            bool staleAccepted = true, latestAccepted = false;
            session->captureMarkup([&](bool ready) {
                staleAccepted = ready;
                if (++callbacks == 2)
                    opened.quit();
            });
            request.saved.first().color = "pink";
            session->setMarkup(request);
            session->captureMarkup([&](bool ready) {
                latestAccepted = ready;
                if (++callbacks == 2)
                    opened.quit();
            });
            if (callbacks != 2) {
                opened.exec();
            }
            require(!staleAccepted && latestAccepted && session->markup().request == request,
                    "An obsolete markup capture replaced the latest request or lost its completion");
            session->updated = [&] {
                if (!session->busy())
                    opened.quit();
            };
            require(session->turn(1), "Could not turn an atomically marked page");
            opened.exec();
            require(session->markup().request == request
                            && session->markup().imageKey == session->image().cacheKey()
                            && session->markup().generation == session->markupGeneration()
                            && session->markup().resolvedRanges == 2,
                    "Page-turn frame published without matching complete markup");
            require(session->go(0), "Could not restore first page after markup turn");
            opened.exec();
            ReaderMarkupRequest elsewhere;
            elsewhere.saved.append({ hits.last().start, hits.last().end, "gray" });
            session->setMarkup(elsewhere);
            require(capture() && session->markup().resolvedRanges == 0,
                    "An unmarked destination resolved off-page geometry");
            require(session->markup().saved.isEmpty(),
                    "Off-page source range fabricated visible geometry");
            auto appearance = session->state();
            session->setMarkup(request);
            auto changed = appearance;
            changed["font_pixels"] = appearance.value("font_pixels").toInt() + 4;
            session->apply(changed);
            opened.exec();
            require(session->markup().request == request
                            && session->markup().imageKey == session->image().cacheKey(),
                    "Reflow published geometry from an earlier image");
            session->setMarkup({});
            session->apply(appearance);
            opened.exec();
            require(session->markup().resolvedRanges == 0,
                    "Cleared markup queued visible geometry after reflow");
            session->updated = {};
        }
        {
            const auto image = session->image();
            const int page = session->page(), pages = session->pages();
            bool searchFinished = false;
            bool searchContinued = false, metadataDuringSearch = false;
            int searchHits = 0;
            QEventLoop searched;
            session->updated = [&] {
                if (!searchFinished && session->chapters().size() == 1
                    && session->chapters().first().title == "Search contents") {
                    metadataDuringSearch = true;
                }
            };
            session->search("passage", [&](SearchBatch batch) {
                searchHits += batch.hits.size();
                searchContinued |= !batch.done;
                if (batch.done) {
                    searchFinished = true;
                    searched.quit();
                }
            });
            // Both requests are queued in the same GUI turn. The search must
            // continue after the chapter metadata reaches the facade.
            session->setChapters({ { "Search contents", "chapter", 0, 12 } },
                                 { { "Search graph", "chapter", 1, 12 } });
            searched.exec();
            QEventLoop metadata;
            ReaderSession::drain([&] { metadata.quit(); });
            metadata.exec();
            require(searchFinished && searchHits > 0, "Chapter metadata cancelled a valid search");
            require(searchContinued && metadataDuringSearch,
                    "Chapter metadata did not arrive during a multi-batch search");
            require(session->image() == image && session->page() == page
                            && session->pages() == pages,
                    "Chapter metadata replaced the readable frame");
            require(session->chapters().size() == 1
                            && session->chapters().first().title == "Search contents"
                            && session->chapterGraph().size() == 1
                            && session->chapterGraph().first().title == "Search graph",
                    "Chapter metadata did not update both navigation lists");

            QVector<int> publishedPages;
            session->updated = [&] { publishedPages.append(session->page()); };
            session->setChapters({ { "Before navigation", "chapter", 0, 18 } }, {});
            require(session->go(1), "Cannot supersede a queued chapter update with navigation");
            ReaderSession::drain([&] { metadata.quit(); });
            metadata.exec();
            require(!publishedPages.isEmpty(), "Navigation did not publish its frame");
            for (const int published : publishedPages) {
                require(published == 1, "Stale chapter metadata published the previous page");
            }
            require(!session->busy() && session->page() == 1 && session->chapters().size() == 1
                            && session->chapters().first().title == "Before navigation",
                    "Navigation lost the chapters resolved by an earlier worker job");

            publishedPages.clear();
            require(session->go(2), "Cannot start navigation before a chapter update");
            session->setChapters({ { "During navigation", "chapter", 0, 24 } }, {});
            ReaderSession::drain([&] { metadata.quit(); });
            metadata.exec();
            require(!publishedPages.isEmpty(), "Pending navigation did not publish its frame");
            for (const int published : publishedPages) {
                require(published == 2, "Chapter metadata replaced a newer navigation frame");
            }
            require(!session->busy() && !session->image().isNull()
                            && session->chapters().size() == 1
                            && session->chapters().first().title == "During navigation",
                    "Chapter metadata queued during navigation was lost");
            session->updated = {};
        }
        // The comparison status must not depend on how strings are displayed.
        for (const bool keys : { false, true }) {
            strings::showKeys(keys);
            PositionComparison located;
            session->compareNativePosition("any", [&](PositionComparison result) {
                located = result;
                opened.quit();
            });
            opened.exec();
            require(located.status == PositionComparison::Status::Compared && located.direction == 0
                            && located.error.isEmpty(),
                    "A position the codec places was not compared");
            // A layout change accepted in the same GUI turn makes the reply stale.
            PositionComparison stale;
            bool staleFinished = false;
            session->compareNativePosition("any", [&](PositionComparison result) {
                stale = result;
                staleFinished = true;
                opened.quit();
            });
            require(session->style(session->fontSize() + 1, session->spacing()),
                    "Could not change the layout during a comparison");
            opened.exec();
            require(staleFinished && stale.status == PositionComparison::Status::Stale,
                    "A comparison across a layout change was not reported as stale");
            QEventLoop settled;
            session->updated = [&] {
                if (!session->busy())
                    settled.quit();
            };
            if (session->busy())
                settled.exec();
            session->updated = {};
        }
        strings::showKeys(false);
        const int smallest = halfPointPixels(minimumHalfPoints);
        QEventLoop rendered;
        session->updated = [&] {
            if (!session->busy())
                rendered.quit();
        };
        require(session->style(smallest, 120, QString(), 0),
                "A valid 5pt size on a 212ppi display was refused");
        rendered.exec();
        require(session->error().isEmpty() && session->fontSize() == smallest
                        && !session->image().isNull(),
                "The session did not render the selected small font");
        QVector<RenderProgress> renderReports;
        session->renderProgressChanged = [&] { renderReports.append(session->renderProgress()); };
        require(session->style(smallest + 1, 120, QString(), 0), "Cannot start progress relayout");
        const auto firstRender = session->renderProgress();
        require(firstRender.active && firstRender.reason == RenderReason::Appearance
                        && firstRender.elapsedMilliseconds >= 0,
                "Appearance request did not start its progress clock");
        require(session->style(smallest + 2, 120, QString(), 0),
                "Cannot replace progress relayout");
        const auto latestRender = session->renderProgress();
        require(latestRender.active && latestRender.revision > firstRender.revision
                        && latestRender.percent == 0,
                "A newer appearance request reused progress from the old request");
        renderReports.clear();
        rendered.exec();
        bool formatted = false;
        int overall = 0;
        for (const auto &report : renderReports) {
            if (!report.active)
                continue;
            require(report.revision == latestRender.revision && report.percent >= 0
                            && report.percent <= 100,
                    "Stale relayout progress reached the replacement job");
            require(report.overallPercent() >= overall && report.overallPercent() <= 100,
                    "Relayout progress moved backwards at a phase boundary");
            overall = report.overallPercent();
            formatted |= report.stage == OpeningStage::Pages;
        }
        require(formatted && !session->renderProgress().active,
                "Completed relayout lost formatting progress or kept its progress active");
        auto bionicSettings = session->state();
        const auto applyBionic = [&](const QString &mode) {
            bionicSettings["bold_reading"] = mode;
            session->apply(bionicSettings);
        };
        applyBionic("bold");
        require(session->renderProgress().reason == RenderReason::EnableBionic,
                "Bionic enabling lost its relayout reason");
        applyBionic("off");
        require(session->renderProgress().reason == RenderReason::DisableBionic,
                "Disabling bionic before enabling completes reused the enabling reason");
        rendered.exec();
        applyBionic("bold");
        rendered.exec();
        applyBionic("off");
        require(session->renderProgress().reason == RenderReason::DisableBionic,
                "Bionic disabling lost its relayout reason");
        rendered.exec();
        require(session->style(smallest, 120, QString(), 0),
                "Cannot restore text after progress relayout");
        rendered.exec();
        require(!session->style(minimumFontPixels() - 1, 120, QString(), 0)
                        && !session->style(maximumFontPixels() + 1, 120, QString(), 0)
                        && !session->style(smallest, 120, QString(), maximumMarginPixels() + 1),
                "Session accepted settings outside this display's limits");
        require(session->style(smallest, 140, QString(), maximumMarginPixels()),
                "Small text prevented later spacing and margin changes");
        rendered.exec();
        require(session->error().isEmpty() && session->spacing() == 140
                        && session->margin() == maximumMarginPixels(),
                "The session lost the later typography changes");
        // Turns must keep applied appearance, but a viewport or pending
        // settings change must still reach the worker without a menu.
        const auto appearanceBeforeTurn = session->state();
        require(session->go(1), "Could not turn after changing typography");
        require(!session->renderProgress().active,
                "An ordinary page jump started appearance progress");
        rendered.exec();
        require(session->page() == 1 && appearance::same(appearanceBeforeTurn, session->state()),
                "An ordinary turn changed appearance");
        const auto rotationAnchor = session->state().value("position");
        const auto portraitImage = session->image();
        session->resize(QSize(900, 758));
        require(session->renderProgress().active
                        && session->renderProgress().reason == RenderReason::PageLayout,
                "Viewport resize did not start layout progress");
        rendered.exec();
        require(session->error().isEmpty() && session->image().size() == QSize(900, 758),
                "A viewport change without a menu reused the old layout");
        const auto landscapeImage = session->image();
        for (const auto size : { QSize(758, 900), QSize(900, 758) }) {
            renderReports.clear();
            session->resize(size);
            rendered.exec();
            require(session->error().isEmpty() && session->state()["position"] == rotationAnchor,
                    "A cached session rotation lost its source position");
            for (const auto &report : renderReports)
                require(!report.active || report.stage != OpeningStage::Pages,
                        "A cached session rotation paginated the whole book");
            require(session->image()
                            == (size.width() < size.height() ? portraitImage : landscapeImage),
                    "A cached session rotation changed page pixels");
        }
        session->apply(kobalt::qt::object({ { "font_face", "missing-performance-test-font" } }));
        rendered.exec();
        require(!session->error().isEmpty() && !session->renderProgress().active,
                "Rejected appearance bypassed validation or kept its progress active");
        session->renderProgressChanged = {};
        require(session->go(2), "Could not turn after rejected appearance");
        rendered.exec();
        require(session->error().isEmpty() && session->page() == 2
                        && appearance::same(appearanceBeforeTurn, session->state()),
                "Rejected settings leaked into the next turn");
        // Submit the burst before delivering any GUI completions, so the
        // active turn and its one pending slot are exercised deterministically.
        require(session->pages() > 5, "Burst fixture has too few pages");
        require(session->go(2), "Cannot reach burst starting page");
        rendered.exec();
        QVector<int> turnFrames;
        std::function<void()> checkBurstCompletion;
        session->updated = [&] {
            if (!session->busy()) {
                turnFrames.append(session->page());
                // Check the deliberate idle gap before buffered dispatch is queued.
                if (checkBurstCompletion)
                    checkBurstCompletion();
            }
        };
        const auto finishBurst = [&](const std::function<bool()> &completed) {
            if (!session->busy() && completed())
                return;
            QEventLoop idle;
            QTimer poll, deadline;
            poll.setInterval(10);
            deadline.setSingleShot(true);
            bool expired = false;
            checkBurstCompletion = [&] {
                if (!session->busy() && completed())
                    idle.quit();
            };
            QObject::connect(&poll, &QTimer::timeout, &idle, [&] { checkBurstCompletion(); });
            QObject::connect(&deadline, &QTimer::timeout, &idle, [&] {
                expired = true;
                idle.quit();
            });
            poll.start();
            deadline.start(10000);
            idle.exec();
            checkBurstCompletion = {};
            require(!expired,
                    "Buffered turn did not reach its expected completion within ten seconds");
        };
        require(!session->turn(0) && !session->turn(2),
                "Session accepted an invalid turn direction");
        require(session->turn(1), "First burst turn was refused");
        for (int i = 0; i < 1000; ++i)
            require(session->turn(1), "A buffered burst turn was refused");
        require(session->page() == 2 && session->busy(),
                "Burst changed the GUI frame before its completion");
        finishBurst([&] { return turnFrames.size() >= 2; });
        require(session->page() == 4 && turnFrames == QVector<int>({ 3, 4 }),
                "A burst skipped the first frame or kept more than one pending turn");
        turnFrames.clear();
        require(session->turn(1) && session->turn(1) && session->turn(-1),
                "Cannot reverse a pending turn");
        finishBurst([&] { return turnFrames.size() >= 2; });
        require(session->page() == 4 && turnFrames == QVector<int>({ 5, 4 }),
                "The latest pending direction did not replace the earlier direction");
        turnFrames.clear();
        bool cancelledFromFrame = false;
        session->updated = [&] {
            if (!session->busy()) {
                turnFrames.append(session->page());
                if (!cancelledFromFrame) {
                    cancelledFromFrame = true;
                    session->cancelPendingTurn();
                }
            }
        };
        require(session->turn(1) && session->turn(1), "Cannot queue callback cancellation turn");
        finishBurst([&] { return cancelledFromFrame && turnFrames.size() >= 1; });
        require(cancelledFromFrame && session->page() == 5 && turnFrames == QVector<int>({ 5 }),
                "A frame callback could not cancel the pending turn before dispatch");
        session->updated = [&] {
            if (!session->busy())
                rendered.quit();
        };
        require(session->go(1), "Cannot reach first-page boundary");
        rendered.exec();
        int boundary = 0, boundaryCalls = 0;
        session->turnBoundary = [&](int direction) {
            boundary = direction;
            ++boundaryCalls;
        };
        session->updated = {};
        require(session->turn(-1) && session->turn(-1), "Cannot buffer a first-page turn");
        finishBurst([&] { return boundaryCalls > 0; });
        require(session->page() == 0 && boundary == -1 && boundaryCalls == 1 && !session->turn(-1),
                "A buffered backward boundary moved past the first page or reported twice");
        session->updated = [&] {
            if (!session->busy())
                rendered.quit();
        };
        require(session->go(session->pages() - 2), "Cannot reach final-page boundary");
        rendered.exec();
        session->updated = {};
        boundary = 0;
        boundaryCalls = 0;
        require(session->turn(1) && session->turn(1), "Cannot buffer a final-page turn");
        finishBurst([&] { return boundaryCalls > 0; });
        require(session->nextPage() == session->page() && boundary == 1 && boundaryCalls == 1,
                "A buffered forward boundary did not report the explicit finish request once");
        session->turnBoundary = {};
        session->updated = [&] {
            if (!session->busy())
                rendered.quit();
        };
        require(session->go(2), "Cannot restore page after boundary tests");
        require(!session->canTurn() && !session->turn(1), "A jump accepted a pending page turn");
        rendered.exec();
        require(session->turn(1) && session->turn(1),
                "Cannot queue before settings supersede a turn");
        session->setDarkMode(!session->state().value("dark_mode").toBool());
        require(!session->canTurn() && !session->turn(1),
                "A layout change accepted a pending page turn");
        rendered.exec();
        finishBurst([&] { return session->page() == 3; });
        require(session->page() == 3, "A pending turn survived a settings request");
        session->updated = {};
        auto saved = session->state();
        saved["fingerprint"] = QString::fromLatin1(session->sourceHash());
        int retiredMetadataUpdates = 0;
        session->updated = [&] { ++retiredMetadataUpdates; };
        session->setChapters({ { "Retired contents", "chapter", 0, 30 } }, {});
        auto retained = session->retained();
        require(bool(retained), "Readable session did not retain its layout");
        require(retained->syncFilenameDigest() == session->syncFilenameDigest(),
                "Retaining a session preserves filename identity");
        QEventLoop drained;
        ReaderSession::drain([&] { drained.quit(); });
        drained.exec();
        require(retiredMetadataUpdates == 0 && session->image().isNull(),
                "Queued chapter metadata reached a retired facade");
        session.reset();
        ReaderSession::drain([&] { drained.quit(); });
        drained.exec();
        require(lease->revoked(), "Retiring a session kept its old source active");
        const auto previousPort = server.serverPort();
        server.close();
        require(server.listen(QHostAddress::LocalHost), "Could not create fresh endpoint");
        Q_UNUSED(previousPort);
        lease = std::make_shared<SourceLease>();
        session = std::move(retained);
        OpenRequest renamedRequest{ path,
                                    path,
                                    QSize(758, 900),
                                    saved,
                                    {},
                                    {},
                                    lease,
                                    nativeResourceFactory(path, server.serverPort(), lease),
                                    secondPositions };
        renamedRequest.syncFilename = "Original book.epub";
        session->open(std::move(renamedRequest), [&](OpeningResult result) {
            require(result.ready(), qPrintable(openingMessage(result)));
            opened.quit();
        });
        opened.exec();
        require(session->syncFilenameDigest() == kosync::filenameDigest("Original book.epub"),
                "A materialized archive uses its supplied original filename");
        require(!session->image().isNull() && session->spacing() == 140
                        && session->margin() == maximumMarginPixels(),
                "Retained reopen through fresh access lost the readable layout");
        auto preview =
                std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        preview->openPreview(*session, QSize(758, 900), [&](QString error) {
            require(error.isEmpty(), qPrintable(error));
            opened.quit();
        });
        opened.exec();
        require(!preview->image().isNull(),
                "Reopened preview could not read through the new lease");
        require(preview->cachedPages() == 1, "A settings preview rendered unused neighbour pages");
        preview->updated = [&] {
            if (!preview->busy())
                rendered.quit();
        };
        require(preview->setWeight(preview->weight() == 0 ? 1 : 0),
                "Could not change preview weight");
        rendered.exec();
        QEventLoop previewDrained;
        ReaderSession::drain([&] { previewDrained.quit(); });
        previewDrained.exec();
        require(preview->cachedPages() == 1,
                "Updating a settings preview rendered unused neighbours");
        preview->updated = {};
        preview->nativePosition([&](QString location, QString error) {
            require(error.isEmpty() && location.startsWith("second:"),
                    "Retained/preview facade kept its old host codec");
            opened.quit();
        });
        opened.exec();

        // A preview can outlive its window while the next opening is queued.
        // Its source must stay separate from the newly authorized reader.
        const auto previousPages = session->pages();
        auto previewSaved = session->state();
        previewSaved["fingerprint"] = QString::fromLatin1(session->sourceHash());
        const auto previewLease = lease;
        retained = session->retained();
        require(bool(retained), "A live preview prevented retaining its reader");
        session.reset();
        ReaderSession::drain([&] { drained.quit(); });
        drained.exec();
        require(previewLease->revoked(), "Retiring the preview's reader left old access active");
        lease = std::make_shared<SourceLease>();
        session = std::move(retained);
        OpenRequest unnamedRequest{ path,
                                    path,
                                    QSize(758, 900),
                                    previewSaved,
                                    {},
                                    {},
                                    lease,
                                    nativeResourceFactory(path, server.serverPort(), lease),
                                    secondPositions };
        unnamedRequest.syncFilename = QStringLiteral("");
        session->open(std::move(unnamedRequest), [&](OpeningResult result) {
            require(result.ready(), qPrintable(openingMessage(result)));
            opened.quit();
        });
        opened.exec();
        require(session->syncFilenameDigest().isEmpty() && session->syncDigest().size() == 32,
                "A missing source name disables filename matching without affecting binary identity");
        require(session->pages() == previousPages && !session->image().isNull()
                        && session->error().isEmpty(),
                "Reopening with a live old preview lost the readable document");
        preview.reset();
        ReaderSession::drain([&] { drained.quit(); });
        drained.exec();
        require(!lease->revoked(), "Closing a preview revoked its parent's source");

        {
            const auto colorPath = QFileInfo(path).absolutePath() + "/later-images.epub";
            auto imageLease = std::make_shared<SourceLease>();
            auto resourceOwner = std::make_shared<int>(0);
            std::weak_ptr<int> resourceLifetime = resourceOwner;
            ResourceFactory images = [colorPath, imageLease, resourceOwner](
                                             const QMap<QString, qint64> &sizes, const QString &,
                                             std::shared_ptr<std::atomic_bool>) {
                return std::make_shared<ResourceCache>(
                        sizes.keys(),
                        ResourceCache::Access{ [colorPath, resourceOwner](
                                                       const QString &name,
                                                       const ResourceCache::Sink &sink, QString *) {
                                                  ZipResource resource;
                                                  if (!resource.open(colorPath, name))
                                                      return false;
                                                  while (resource.valid() && !resource.finished()) {
                                                      const auto bytes = resource.next();
                                                      if (!sink(bytes.constData(), bytes.size()))
                                                          return false;
                                                  }
                                                  return resource.valid();
                                              },
                                               [imageLease] { return imageLease->revoked(); },
                                               {},
                                               [](const QString &name) {
                                                   return name.endsWith(".png") ? QSize(128, 64)
                                                                                : QSize();
                                               } });
            };
            auto illustrated =
                    std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
            illustrated->open(
                    { colorPath, colorPath, QSize(758, 900), {}, {}, {}, imageLease, images },
                    [&](OpeningResult result) {
                        require(result.ready(), qPrintable(openingMessage(result)));
                        opened.quit();
                    });
            opened.exec();
            require(illustrated->resourceStats().value("ready_images").toInt() == 0,
                    "Neighbour image regression loaded the future image during opening");
            const auto chapterLinks = illustrated->chapters();
            require(chapterLinks.size() == 3 && chapterLinks[1].page > 2,
                    "Neighbour image fixture has no text before its image");
            illustrated->updated = [&] {
                if (!illustrated->busy())
                    rendered.quit();
            };
            require(illustrated->go(chapterLinks[1].page - 2),
                    "Cannot jump before the illustrated chapter");
            rendered.exec();
            ReaderSession::drain([&] { drained.quit(); });
            drained.exec();
            require(illustrated->resourceStats().value("ready_images").toInt() == 0,
                    "Neighbour image regression reached the image too early");
            illustrated->updated = [&] {
                if (illustrated->busy())
                    return;
                // Cancel after the frame callback returns, with the pending
                // dispatch already scheduled and idle image work postponed.
                QTimer::singleShot(0, illustrated.get(), [&] { illustrated->cancelPendingTurn(); });
                rendered.quit();
            };
            require(illustrated->turn(1) && illustrated->turn(1),
                    "Cannot buffer a turn beside the future image");
            rendered.exec();
            illustrated->updated = {};
            QEventLoop loaded;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loaded, [&] {
                const auto stats = illustrated->resourceStats();
                if (!illustrated->busy() && stats.value("pending_images").toInt() == 0
                    && stats.value("ready_images").toInt() > 0 && illustrated->cachedPages() > 1)
                    loaded.quit();
            });
            poll.start(10);
            loaded.exec();
            poll.stop();
            require(illustrated->page() == chapterLinks[1].page - 1,
                    "Cancelled image-page turn still ran");
            require(illustrated->cachedPages() > 1,
                    "Cancelling a scheduled turn did not resume deferred images and neighbours");
            images = {};
            resourceOwner.reset();
            require(illustrated->turn(1) && illustrated->turn(1),
                    "Could not queue a burst before closing");
            illustrated.reset();
            ReaderSession::drain([&] {
                require(resourceLifetime.expired(),
                        "A queued GUI frame kept resources alive after draining the worker");
                drained.quit();
            });
            drained.exec();
        }

        saved = session->state();
        saved["fingerprint"] = QString::fromLatin1(session->sourceHash());
        retained = session->retained();
        session.reset();
        ReaderSession::drain([&] { drained.quit(); });
        drained.exec();
        require(bool(retained), "Second retained candidate missing");
        session = std::move(retained);
        lease = std::make_shared<SourceLease>();
        ResourceFactory denied = [lease](const QMap<QString, qint64> &sizes, const QString &,
                                         std::shared_ptr<std::atomic_bool>) {
            return std::make_shared<ResourceCache>(
                    sizes.keys(),
                    ResourceCache::Access{ [](const QString &, const ResourceCache::Sink &,
                                              QString *) { return false; },
                                           [lease] { return lease->revoked(); },
                                           {},
                                           {} });
        };
        bool deniedFinished = false;
        session->open({ path, path, QSize(758, 900), saved, {}, {}, lease, denied },
                      [&](OpeningResult result) {
                          require(!result.ready(), "Denied fresh access reused retained text");
                          deniedFinished = true;
                          opened.quit();
                      });
        opened.exec();
        require(deniedFinished && session->image().isNull(),
                "Failed fresh probe published an old frame");

        // Ready progress is queued immediately before the opening completion.
        // Start another opening there, then deliver the old queued completion.
        bool replacementStarted = false, staleFinished = false, replacementFinished = false;
        lease = std::make_shared<SourceLease>();
        session->open(
                { path,
                  path,
                  QSize(758, 900),
                  {},
                  {},
                  {},
                  lease,
                  nativeResourceFactory(path, server.serverPort(), lease) },
                [&](OpeningResult) { staleFinished = true; },
                [&](OpeningStage stage, int percent) {
                    if (replacementStarted || stage != OpeningStage::Ready || percent != 100)
                        return;
                    replacementStarted = true;
                    lease = std::make_shared<SourceLease>();
                    session->open({ path,
                                    path,
                                    QSize(758, 900),
                                    {},
                                    {},
                                    {},
                                    lease,
                                    nativeResourceFactory(path, server.serverPort(), lease) },
                                  [&](OpeningResult result) {
                                      require(result.ready(), qPrintable(openingMessage(result)));
                                      replacementFinished = true;
                                      opened.quit();
                                  });
                });
        opened.exec();
        require(replacementStarted && replacementFinished && !staleFinished
                        && !session->image().isNull(),
                "Old queued opening completion reached a newer source generation");
        {
            auto cancelled =
                    std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
            auto cancelledLease = std::make_shared<SourceLease>();
            cancelled->open({ path,
                              path,
                              QSize(758, 900),
                              {},
                              {},
                              {},
                              cancelledLease,
                              nativeResourceFactory(path, server.serverPort(), cancelledLease) },
                            [&](OpeningResult result) {
                                require(result.ready(), qPrintable(openingMessage(result)));
                                opened.quit();
                            });
            opened.exec();
            require(cancelled->style(cancelled->fontSize() + 1, cancelled->spacing()),
                    "Cannot start a cancelled relayout");
            require(cancelled->renderProgress().active,
                    "Cancelled relayout never started progress");
            cancelled->cancelSource();
            require(!cancelled->renderProgress().active,
                    "Source cancellation kept relayout progress active");
            bool staleProgress = false;
            cancelled->renderProgressChanged = [&] {
                staleProgress |= cancelled->renderProgress().active;
            };
            ReaderSession::drain([&] { drained.quit(); });
            drained.exec();
            require(!staleProgress && !cancelled->renderProgress().active,
                    "Queued progress revived a cancelled relayout");
        }
        session->nativePosition([&](QString location, QString error) {
            require(location.isEmpty() && !error.isEmpty(),
                    "A later codec-free opening inherited platform mapping");
            opened.quit();
        });
        opened.exec();
        for (const bool keys : { false, true }) {
            strings::showKeys(keys);
            PositionComparison unplaced;
            session->compareNativePosition("any", [&](PositionComparison result) {
                unplaced = result;
                opened.quit();
            });
            opened.exec();
            require(unplaced.status == PositionComparison::Status::Failed
                            && !unplaced.error.isEmpty(),
                    "A position without a codec was not reported as failed");
        }
        strings::showKeys(false);
        {
            native::ResourceServer publisherServer;
            require(publisherServer.open(
                            publisherPath,
                            [publisherPath] { return native::openPlainArchive(publisherPath); }),
                    "Cannot serve the non-linear fixture");
            auto absolute =
                    std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
            auto absoluteLease = std::make_shared<SourceLease>();
            absolute->open(
                    { publisherPath,
                      publisherPath,
                      QSize(758, 900),
                      {},
                      {},
                      {},
                      absoluteLease,
                      nativeResourceFactory(publisherPath, publisherServer.port(), absoluteLease) },
                    [&](OpeningResult result) {
                        require(result.ready(), qPrintable(openingMessage(result)));
                        opened.quit();
                    });
            opened.exec();
            int nonLinearPage = -1;
            QEventLoop found;
            absolute->search("Nonlinearspine", [&](SearchBatch batch) {
                if (!batch.hits.isEmpty()) {
                    nonLinearPage = batch.hits.first().page;
                    found.quit();
                } else if (batch.done)
                    found.quit();
            });
            found.exec();
            require(nonLinearPage > 0 && nonLinearPage < absolute->pages(),
                    "Non-linear fixture has no searchable page");
            absolute->updated = [&] {
                if (!absolute->busy())
                    opened.quit();
            };
            require(absolute->go(nonLinearPage - 1), "Cannot go before the non-linear section");
            opened.exec();
            require(absolute->nextPage() == nonLinearPage,
                    "Default paging skipped a non-linear section");
            require(absolute->turn(1), "Cannot turn into the non-linear section");
            opened.exec();
            require(absolute->page() == nonLinearPage, "Default turn skipped a non-linear section");
            absolute->apply(kobalt::qt::object({ { "hide_nonlinear_fragments", true } }));
            opened.exec();
            require(absolute->go(0), "Cannot go to the first physical page");
            opened.exec();
            while (absolute->nextPage() <= nonLinearPage) {
                require(absolute->turn(1), "Cannot reach the non-linear section");
                opened.exec();
            }
            require(absolute->page() < nonLinearPage && absolute->nextPage() > nonLinearPage,
                    "Page turns did not expose the non-linear skip");
            require(absolute->turn(1), "Cannot turn across the non-linear section");
            opened.exec();
            require(absolute->page() > nonLinearPage, "Page turn did not skip the non-linear page");
            require(absolute->go(nonLinearPage), "Cannot go to the non-linear page by number");
            opened.exec();
            require(absolute->page() == nonLinearPage,
                    "Absolute navigation skipped the non-linear page");
            absolute.reset();
            ReaderSession::drain([&] { drained.quit(); });
            drained.exec();
        }
        // A closed book can stay cached until the application destroys its
        // children, after aboutToQuit has stopped the engine worker.
        auto shutdownSession = session->retained();
        require(bool(shutdownSession), "Shutdown candidate did not retain its model");
        shutdownSession->setParent(&app);
        shutdownSession.release();
        lease->revoke();
        session.reset();
        ReaderSession::drain([&] { app.quit(); });
    });
    const auto result = app.exec();
    return result;
}

// Checks the reader session: opening failures, sync previews and imports, stale results, appearance progress, buffered turns, previews and non-linear pages.
int main(int argc, char **argv)
{
    const int result = run(argc, argv);
    std::puts(
            "PASS: position comparison status, session typography, retained fresh access, preview lifetime, denied reopen, stale completion and application shutdown");
    return result;
}
