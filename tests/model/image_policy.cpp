#include "support/qt_compat.h"
#include "support/strings.h"
#include "publication/image_policy.h"
#include "publication/native_resources.h"
#include "support/diagnostics.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <QXmlStreamReader>
#include <cstdio>
#include <thread>
#include "check.h"

using namespace kobalt;

static QByteArray png(quint32 width, quint32 height)
{
    QByteArray data(33, '\0');
    data.replace(0, 8, QByteArray("\x89PNG\r\n\x1a\n", 8));
    qToBigEndian<quint32>(13, reinterpret_cast<uchar *>(data.data()) + 8);
    data.replace(12, 4, "IHDR");
    qToBigEndian(width, reinterpret_cast<uchar *>(data.data()) + 16);
    qToBigEndian(height, reinterpret_cast<uchar *>(data.data()) + 20);
    return data;
}

static QByteArray jpeg(quint16 width, quint16 height, bool progressive)
{
    QByteArray data = QByteArray::fromHex(progressive ? "ffd8ffc2001108" : "ffd8ffc0001108");
    char size[4];
    qToBigEndian(height, reinterpret_cast<uchar *>(size));
    qToBigEndian(width, reinterpret_cast<uchar *>(size) + 2);
    data.append(size, 4);
    data.append(QByteArray::fromHex("03011100021100031100"));
    return data;
}

static void checkHeaders()
{
    for (const auto &data : { png(3000, 4000), jpeg(3000, 4000, false), jpeg(3000, 4000, true) }) {
        for (int count = 0; count < 8; ++count)
            require(inspectImageHeader(data.left(count)).action == ImageHeader::More,
                    "Partial signature rejected");
        const auto header = inspectImageHeader(data);
        require(header.action == ImageHeader::Keep && header.size == QSize(3000, 4000),
                "Ordinary high-resolution cover rejected");
    }
    for (const auto &data : { png(8000, 8000), jpeg(8000, 8000, false), jpeg(8000, 8000, true) })
        require(inspectImageHeader(data).action == ImageHeader::Replace,
                "Oversized bitmap accepted");
    require(inspectImageHeader(png(4096, 4096)).action == ImageHeader::Keep
                    && inspectImageHeader(png(4096, 4097)).action == ImageHeader::Replace,
            "Pixel limit boundary is wrong");
    require(inspectImageHeader(png(0, 50)).action == ImageHeader::Replace
                    && inspectImageHeader(png(0xffffffffU, 0xffffffffU)).action
                            == ImageHeader::Replace
                    && inspectImageHeader(png(32769, 1)).action == ImageHeader::Replace,
            "Invalid dimensions accepted");
    QByteArray gif = "GIF89a";
    char size[4];
    qToLittleEndian<quint16>(8000, reinterpret_cast<uchar *>(size));
    qToLittleEndian<quint16>(8000, reinterpret_cast<uchar *>(size) + 2);
    gif.append(size, 4);
    require(inspectImageHeader(gif).action == ImageHeader::Replace, "Oversized GIF accepted");
    QByteArray metadata = QByteArray::fromHex("ffe1ffff") + QByteArray(65533, 'x');
    QByteArray extended = QByteArray::fromHex("ffd8") + metadata + jpeg(3000, 4000, true).mid(2);
    require(inspectImageHeader(extended.left(500)).action == ImageHeader::More
                    && inspectImageHeader(extended).action == ImageHeader::Keep,
            "JPEG metadata blocked a valid cover");
    QByteArray excessive = QByteArray::fromHex("ffd8");
    while (excessive.size() < imageHeaderLimit)
        excessive += metadata;
    require(inspectImageHeader(excessive.left(imageHeaderLimit)).action == ImageHeader::Replace,
            "Unbounded JPEG header accepted");
    require(inspectImageHeader(QByteArray::fromHex("ffd8ffda000801020304")).action
                    == ImageHeader::Replace,
            "JPEG without dimensions accepted");
    require(inspectImageHeader(QByteArray("<html/>\n")).action == ImageHeader::Keep,
            "Non-image content changed");
    require(imageFileName("images/COVER.JPEG") && !imageFileName("chapter.xhtml"),
            "Image extension detection failed");
    QXmlStreamReader xml(imagePlaceholder(QSize(600, 900), "System & \"font\""));
    bool label = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("text")) {
            require(xml.attributes().value("font-family") == QStringLiteral("System & \"font\""),
                    "Resolved system font was not preserved");
            label = xml.readElementText() == "Image could not be loaded";
        }
    }
    require(!xml.hasError() && label, "Placeholder label or XML escaping failed");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    checkHeaders();
    initializeResourceNetwork();
    QTemporaryDir logs;
    require(logs.isValid(), "Cannot create diagnostic directory");
    initializeDiagnostics(logs.path());
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "Cannot listen for fake native service");
    std::atomic_int requests{ 0 }, skippedRequests{ 0 }, hugeBytesSent{ 0 };
    auto barrierCancelled = std::make_shared<SourceLease>();
    std::atomic_bool barrierReleased{ false }, releasedBeforeCancel{ false };
    const QByteArray normal = png(3000, 4000) + QByteArray(70000, 'x');
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            auto pending = std::make_shared<QByteArray>();
            auto served = std::make_shared<bool>(false);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, pending, served] {
                if (*served)
                    return;
                *pending += socket->readAll();
                if (!pending->contains("\r\n\r\n"))
                    return;
                *served = true;
                ++requests;
                require(pending->startsWith("GET /EPUBPATH/test.epub HTTP/1.1\r\n"),
                        "Native resource route changed");
                require(pending->toLower().contains("\r\nconnection: close\r\n"),
                        "Native resource requested a persistent response");
                require(pending->toLower().contains("\r\nrange: bytes=0-0\r\n"),
                        "Native resource range header changed");
                require(!resourceSecret().isEmpty()
                                && pending->toLower().contains(
                                        "\r\nx-kobalt-secret: " + resourceSecret() + "\r\n"),
                        "Native resource request omitted the loopback secret");
                QByteArray name;
                for (auto line : pending->split('\n'))
                    if (line.toLower().startsWith("filename:"))
                        name = line.mid(9).trimmed();
                if (name.startsWith("metadata-")
                    && pending->toLower().contains("\r\nx-kobalt-image-dimensions: 1\r\n")) {
                    QByteArray value = "1 3000 4000", extra;
                    if (name == "metadata-version.png")
                        value = "2 3000 4000";
                    if (name == "metadata-zero.png")
                        value = "1 0 4000";
                    if (name == "metadata-negative.png")
                        value = "1 -1 4000";
                    if (name == "metadata-overflow.png")
                        value = "1 999999999999999999999 1";
                    if (name == "metadata-wide.png")
                        value = "1 32769 1";
                    if (name == "metadata-pixels.png")
                        value = "1 8000 8000";
                    if (name == "metadata-spacing.png")
                        value = "1  3000 4000";
                    if (name == "metadata-duplicate.png")
                        extra = "X-Kobalt-Image-Dimensions: 1 3000 4000\r\n";
                    const bool body = name == "metadata-body.png";
                    socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: "
                                  + QByteArray(body ? "1" : "0") + "\r\nX-Kobalt-Image-Dimensions: "
                                  + value + "\r\n" + extra + "\r\n" + (body ? "x" : ""));
                    socket->disconnectFromHost();
                    return;
                }
                if (name == "skip.png")
                    ++skippedRequests;
                if (name == "redirect.png") {
                    socket->write(
                            "HTTP/1.1 302 Found\r\nLocation: /other\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                if (name == "denied.png") {
                    socket->write(
                            "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                if (name == "empty.bin") {
                    socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: 0\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                if (name == "bad-status.bin" || name == "bad-crlf.bin") {
                    socket->write(
                            name == "bad-status.bin"
                                    ? "HTTP/1.1 206 Bad\tStatus\r\nContent-Length: 1\r\n\r\nx"
                                    : "HTTP/1.1 206 Partial Content\nContent-Length: 1\r\n\r\nx");
                    socket->disconnectFromHost();
                    return;
                }
                QByteArray invalid;
                if (name == "missing.bin")
                    invalid = "X-Test: missing\r\n";
                if (name == "duplicate.bin")
                    invalid = "Content-Length: 1\r\nContent-Length: 1\r\n";
                if (name == "negative.bin")
                    invalid = "Content-Length: -1\r\n";
                if (name == "overflow.bin")
                    invalid = "Content-Length: 999999999999999999999\r\n";
                if (name == "chunked.bin")
                    invalid = "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n";
                if (name == "compressed.bin")
                    invalid = "Content-Length: 1\r\nContent-Encoding: gzip\r\n";
                if (name == "oversized-header.bin")
                    invalid = "Content-Length: 1\r\nX-Long: " + QByteArray(17000, 'a') + "\r\n";
                if (name == "truncated.bin" || name == "truncated.xhtml")
                    invalid = "Content-Length: 100\r\n";
                if (!invalid.isEmpty()) {
                    socket->write(
                            "HTTP/1.1 206 Partial Content\r\n" + invalid + "\r\n"
                            + (name == "truncated.xhtml" ? QByteArray("<html>") : QByteArray("x")));
                    socket->disconnectFromHost();
                    return;
                }
                if (name == "barrier.bin") {
                    socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: 10\r\n\r\nx");
                    releaseResourceConnections([&] {
                        releasedBeforeCancel.store(!barrierCancelled->revoked());
                        barrierReleased.store(true);
                    });
                    kobalt::qt::later(100, socket, [&] {
                        require(!barrierReleased.load(),
                                "Release callback ran during an active transfer");
                        barrierCancelled->revoke();
                    });
                    return;
                }
                if (name == "stall.bin") {
                    socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: 10\r\n\r\nx");
                    return;
                }
                if (name == "fragmented.bin") {
                    socket->write("HTTP/1.1 206 Partial Content\r\nContent-Len");
                    kobalt::qt::later(10, socket, [socket] { socket->write("gth: 3\r\n\r\na"); });
                    kobalt::qt::later(20, socket, [socket] {
                        socket->write("bc");
                        socket->disconnectFromHost();
                    });
                    return;
                }
                const QByteArray body = name == "huge.png" ? png(8000, 8000)
                        : (name == "normal.png" || name == "deferred.png"
                           || name.startsWith("metadata-"))
                        ? normal
                        : (name == "malformed.xhtml" || name == "malformed.opf")
                        ? QByteArray("<html><p>Broken</html>")
                        : (name == "unfinished.xhtml" || name == "unfinished.opf")
                        ? QByteArray("<html><p>")
                        : QByteArray("<html><p>Small text</p></html>");
                const auto total = name == "huge.png" ? 4 * 1024 * 1024 : body.size();
                socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: "
                              + QByteArray::number(total) + "\r\nConnection: close\r\n\r\n");
                socket->write(body);
                if (name != "huge.png") {
                    socket->disconnectFromHost();
                    return;
                }
                hugeBytesSent += body.size();
                // A slow body makes it observable that the client stops after
                // the header, rather than buffering and discarding the image.
                auto *timer = new QTimer(socket);
                timer->setInterval(20);
                QObject::connect(timer, &QTimer::timeout, socket, [&, socket, timer, total] {
                    if (socket->state() != QAbstractSocket::ConnectedState
                        || hugeBytesSent >= total) {
                        timer->stop();
                        return;
                    }
                    const int count = qMin(65536, int(total) - hugeBytesSent.load());
                    socket->write(QByteArray(count, 'x'));
                    hugeBytesSent += count;
                });
                timer->start();
            });
        }
    });
    const auto port = server.serverPort();
    std::thread worker([&] {
        auto cancelled = std::make_shared<SourceLease>();
        auto cache = nativeResources({ { "skip.png", ResourceCache::budgetBytes + 1 },
                                       { "huge.png", 4 * 1024 * 1024 },
                                       { "normal.png", normal.size() },
                                       { "chapter.xhtml", 40 } },
                                     "/test.epub", port, cancelled);
        auto read = [&](const QString &name) {
            const auto size = cache->size(name);
            require(size >= 0, qPrintable(cache->error()));
            QByteArray data(size, '\0');
            require(cache->read(name, 0, data.data(), size) == size, "Cached resource truncated");
            return data;
        };
        require(read("skip.png") == imagePlaceholder() && requests == 0,
                "Oversized source was requested");
        require(read("huge.png") == imagePlaceholder(QSize(8000, 8000)),
                "Oversized dimensions did not produce a placeholder");
        require(read("normal.png") == normal, "Ordinary image data changed");
        require(read("chapter.xhtml") == "<html><p>Small text</p></html>", "XML transfer changed");
        require(hugeBytesSent < 1024 * 1024 && skippedRequests == 0,
                "Oversized image body was loaded");
        const auto beforeProbe = requests.load();
        auto deferred =
                nativeResources({ { "deferred.png", 512 * 1024 }, { "normal.png", normal.size() } },
                                "/test.epub", port, cancelled);
        require(!deferred->deferredImageSize("normal.png").isValid() && requests == beforeProbe,
                "Small image made a separate dimensions request");
        require(deferred->deferredImageSize("deferred.png") == QSize(3000, 4000)
                        && requests == beforeProbe + 1,
                "Large image dimensions were not read");
        require(deferred->statistics()["allocated_bytes"].toDouble() == 0
                        && deferred->error().isEmpty(),
                "Dimensions probe retained the full image or reported early close as a failure");
        require(deferred->deferredImageSize("deferred.png") == QSize(3000, 4000)
                        && requests == beforeProbe + 1,
                "Dimensions probe was repeated");
        require(!deferred->imageReady("deferred.png") && deferred->loadNextImage()
                        && deferred->readBounded("deferred.png", normal.size()) == normal,
                "Full image did not load after the dimensions probe");
        auto metadata = nativeResources({ { "metadata-good.png", 512 * 1024 } }, "/test.epub", port,
                                        cancelled);
        require(metadata->deferredImageSize("metadata-good.png") == QSize(3000, 4000),
                "Metadata response did not provide dimensions");
        require(metadata->statistics()["transferred_bytes"].toDouble() == 0
                        && metadata->statistics()["allocated_bytes"].toDouble() == 0,
                "Metadata response fetched a bitmap body");
        require(metadata->readBounded("metadata-good.png", normal.size()) == normal,
                "Full request kept the metadata-only header");
        for (const QString suffix : { "version", "zero", "negative", "overflow", "wide", "pixels",
                                      "spacing", "duplicate", "body" }) {
            const auto name = "metadata-" + suffix + ".png";
            auto malformed =
                    nativeResources({ { name, 512 * 1024 } }, "/test.epub", port, cancelled);
            require(!malformed->deferredImageSize(name).isValid(),
                    "Malformed image metadata was trusted");
            require(malformed->readBounded(name, normal.size()) == normal
                            && malformed->error().isEmpty(),
                    "Malformed metadata prevented the ordinary loading fallback");
        }
        for (const QString name :
             { "denied.png", "redirect.png", "missing.bin", "duplicate.bin", "negative.bin",
               "overflow.bin", "chunked.bin", "compressed.bin", "oversized-header.bin",
               "truncated.bin", "truncated.xhtml", "malformed.opf", "unfinished.opf",
               "bad-status.bin", "bad-crlf.bin" }) {
            auto rejected = std::make_shared<std::atomic_bool>(false);
            auto denied = nativeResources({ { name, 100 } }, "/test.epub", port, cancelled,
                                          "sans-serif", rejected);
            require(denied->size(name) == -1 && !denied->error().isEmpty(),
                    "Failed native response accepted");
            require(rejected->load() == (name == "malformed.opf" || name == "unfinished.opf"),
                    "Resource transfer failure confused with malformed publication content");
        }
        auto empty = nativeResources({ { "empty.bin", 0 } }, "/test.epub", port, cancelled);
        require(empty->size("empty.bin") == 0 && empty->error().isEmpty(),
                "Empty native response rejected");
        auto fragmented =
                nativeResources({ { "fragmented.bin", 3 } }, "/test.epub", port, cancelled);
        require(fragmented->readBounded("fragmented.bin", 3) == "abc",
                "Fragmented native response was lost");
        for (const auto &name : { "malformed.xhtml", "unfinished.xhtml" }) {
            auto access = std::make_shared<SourceLease>();
            auto rejected = std::make_shared<std::atomic_bool>(false);
            auto content = nativeResourceFactory("/test.epub", port, access)({ { name, 64 } }, {},
                                                                             rejected);
            const auto bytes = content->readBounded(name, 100);
            require(!bytes.isEmpty() && content->error().isEmpty() && !rejected->load(),
                    "Recoverable chapter HTML did not reach the engine intact");
        }
        auto oldLease = std::make_shared<SourceLease>();
        auto rebound = nativeResources({ { "chapter.xhtml", 40 }, { "fragmented.bin", 3 } },
                                       "/test.epub", port, oldLease);
        require(rebound->readBounded("chapter.xhtml", 100) == "<html><p>Small text</p></html>",
                "Initial lease failed");
        oldLease->revoke();
        const int beforeClosed = requests;
        require(rebound->size("chapter.xhtml") == -1 && requests == beforeClosed,
                "Revoked source supplied cached bytes");
        auto newLease = std::make_shared<SourceLease>();
        auto fresh = nativeResources({ { "chapter.xhtml", 40 }, { "fragmented.bin", 3 } },
                                     "/test.epub", port, newLease);
        require(rebound->rebind(*fresh), "Fresh source could not replace revoked cache access");
        require(rebound->readBounded("fragmented.bin", 3) == "abc" && requests > beforeClosed,
                "Rebound source kept its cancelled access");
        auto interrupted = std::make_shared<SourceLease>();
        auto stalled = nativeResources({ { "stall.bin", 10 } }, "/test.epub", port, interrupted);
        kobalt::qt::invoke(
                &app,
                [&app, interrupted] {
                    kobalt::qt::later(100, &app, [interrupted] { interrupted->revoke(); });
                },
                Qt::QueuedConnection);
        QElapsedTimer elapsed;
        elapsed.start();
        require(stalled->size("stall.bin") == -1 && elapsed.elapsed() < 1500,
                "Stalled native response ignored cancellation");
        require(stalled->statistics()["allocated_bytes"].toDouble() == 0,
                "Cancelled response retained partial content");
        auto barrier =
                nativeResources({ { "barrier.bin", 10 } }, "/test.epub", port, barrierCancelled);
        require(barrier->size("barrier.bin") == -1, "Release barrier fixture did not cancel");
        kobalt::qt::invoke(
                &app,
                [&] {
                    require(barrierReleased.load() && !releasedBeforeCancel.load(),
                            "Release barrier did not drain the active transfer");
                },
                Qt::BlockingQueuedConnection);
        auto timedOut = nativeResources({ { "stall.bin", 10 } }, "/test.epub", port, cancelled);
        elapsed.restart();
        require(timedOut->size("stall.bin") == -1
                        && timedOut->error()
                                == kobalt::strings::get(
                                        "error.native_publication_transfer_timed_out")
                        && elapsed.elapsed() < 6500,
                "Stalled native response ignored deadline");
        const int before = requests;
        cancelled->revoke();
        auto stopped = nativeResources({ { "skip.png", ResourceCache::budgetBytes + 1 } },
                                       "/test.epub", port, cancelled);
        require(stopped->size("skip.png") == -1 && requests == before,
                "Cancellation ignored by placeholder shortcut");
        kobalt::qt::invoke(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    kobalt::qt::later(30000, &app, [] { qFatal("Native resource checks timed out"); });
    app.exec();
    worker.join();
    diagnostic("session", { { "status", "closed" } });
    flushDiagnostics();
    QFile log(logs.path() + "/events.jsonl");
    require(log.open(QIODevice::ReadOnly), "Cannot read diagnostics");
    const auto records = log.readAll();
    require(!records.contains("test.epub") && !records.contains("stall.bin"),
            "Resource log contains a private path");
    bool timeoutRecorded = false;
    for (const auto &line : records.split('\n')) {
        const auto entry = QJsonDocument::fromJson(line).object();
        if (entry.value("error_code").toString() != "error.native_publication_transfer_timed_out")
            continue;
        require(entry.value("event").toString() == "resource-transfer-failed"
                        && entry.value("headers_received").toBool()
                        && entry.value("received_bytes").toInt() == 1
                        && entry.value("expected_bytes").toInt() == 10,
                "Transfer failure lost response state");
        timeoutRecorded = true;
    }
    require(timeoutRecorded, "Timeout diagnostic lost after session closure");
    std::puts(
            "PASS: image headers, ordinary covers, source skip, early body abort, XML, HTTP failures and cancellation");
}
