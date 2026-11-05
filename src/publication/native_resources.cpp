#include "support/qt_compat.h"
#include "support/strings.h"
#include "native_resources.h"
#include "support/diagnostics.h"
#include "image_policy.h"
#include <QEventLoop>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>
#include <vector>

namespace kobalt {
namespace {
struct TransferTimings
{
    qint64 queueNs = 0, firstByteNs = 0, requestNs = 0, sinkNs = 0, xmlNs = 0;
    qint64 requests = 0, failures = 0;

    QJsonObject statistics() const
    {
        return kobalt::qt::object({ { "request_queue_ms", double(queueNs) / 1000000.0 },
                                    { "request_first_byte_ms", double(firstByteNs) / 1000000.0 },
                                    { "request_ms", double(requestNs) / 1000000.0 },
                                    { "resource_sink_ms", double(sinkNs) / 1000000.0 },
                                    { "resource_xml_ms", double(xmlNs) / 1000000.0 },
                                    { "requests", requests },
                                    { "request_failures", failures } });
    }
};

thread_local bool transferring = false;
thread_local std::vector<std::function<void()>> pendingReleases;
std::atomic_bool networkReady{ false };

QObject *resourceNetwork()
{
    // A nested event loop on the engine worker would run other queued engine
    // jobs during a transfer and corrupt CREngine's process-global font state.
    // Keep network events on a separate thread; the engine waits synchronously.
    static auto *network = [] {
        auto *thread = new QThread(qApp);
        auto *worker = new QObject;
        worker->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread] {
            thread->quit();
            thread->wait();
        });
        thread->start();
        networkReady.store(true);
        return worker;
    }();
    return network;
}

constexpr qint64 headerLimit = 16 * 1024;

bool responseLength(const QByteArray &header, qint64 *length, QSize *dimensions)
{
    const auto lines = header.split('\n');
    if (lines.isEmpty())
        return false;
    const auto status = lines.front();
    if (!status.endsWith('\r')
        || (!status.startsWith("HTTP/1.1 206 ") && !status.startsWith("HTTP/1.0 206 ")))
        return false;
    for (const auto c : status.left(status.size() - 1))
        if (uchar(c) < 32 || uchar(c) == 127)
            return false;
    bool hasLength = false, hasDimensions = false;
    for (int i = 1; i < lines.size(); ++i) {
        auto line = lines[i];
        if (line.isEmpty() && i == lines.size() - 1)
            break;
        if (!line.endsWith('\r'))
            return false;
        line.chop(1);
        if (line.isEmpty())
            break;
        const auto colon = line.indexOf(':');
        if (colon <= 0)
            return false;
        const auto name = line.left(colon).toLower();
        for (const auto c : name)
            if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9')
                && !QByteArray("!#$%&'*+-.^_`|~").contains(c))
                return false;
        const auto value = line.mid(colon + 1).trimmed();
        for (const auto c : value)
            if ((uchar(c) < 32 && c != '\t') || uchar(c) == 127)
                return false;
        if (name == "transfer-encoding")
            return false;
        if (name == "content-encoding" && value.toLower() != "identity")
            return false;
        if (name == "content-length") {
            if (hasLength || value.isEmpty())
                return false;
            for (const auto c : value)
                if (c < '0' || c > '9')
                    return false;
            bool ok = false;
            *length = value.toLongLong(&ok);
            if (!ok || *length < 0)
                return false;
            hasLength = true;
        }
        if (name == "x-kobalt-image-dimensions") {
            if (!dimensions || hasDimensions)
                return false;
            const auto parts = value.split(' ');
            if (parts.size() != 3 || parts[0] != "1")
                return false;
            int size[2];
            for (int i = 0; i < 2; ++i) {
                if (parts[i + 1].isEmpty())
                    return false;
                for (const auto c : parts[i + 1])
                    if (c < '0' || c > '9')
                        return false;
                bool ok = false;
                size[i] = parts[i + 1].toInt(&ok);
                if (!ok || size[i] <= 0 || size[i] > 32768)
                    return false;
            }
            if (quint64(size[0]) * size[1] > imagePixelLimit)
                return false;
            *dimensions = QSize(size[0], size[1]);
            hasDimensions = true;
        }
    }
    return hasLength && (!hasDimensions || *length == 0);
}
} // namespace

void initializeResourceNetwork()
{
    resourceNetwork();
}

QByteArray resourceSecret()
{
    static const QByteArray secret = [] {
        QFile source("/dev/urandom");
        if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
            return QByteArray();
        const auto bytes = source.read(16);
        return bytes.size() == 16 ? bytes.toHex() : QByteArray();
    }();
    return secret;
}

bool resourceSecretMatches(const QByteArray &candidate)
{
    const auto secret = resourceSecret();
    if (secret.isEmpty() || candidate.size() != secret.size())
        return false;
    // Compare every byte so the time taken does not reveal a matching prefix.
    uchar difference = 0;
    for (int i = 0; i < secret.size(); ++i)
        difference |= uchar(secret[i] ^ candidate[i]);
    return difference == 0;
}

void releaseResourceConnections(std::function<void()> finished)
{
    if (!networkReady.load()) {
        kobalt::qt::invoke(qApp, finished, Qt::QueuedConnection);
        return;
    }
    auto *network = resourceNetwork();
    kobalt::qt::invoke(
            network,
            [finished] {
                // Every response closes its socket. This barrier waits for an active
                // transfer to finish before the GUI releases the native service.
                if (transferring)
                    pendingReleases.push_back(finished);
                else
                    kobalt::qt::invoke(qApp, finished, Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

std::shared_ptr<ResourceCache> nativeResources(QMap<QString, qint64> sizes, const QString &path,
                                               int port, std::shared_ptr<SourceLease> lease,
                                               const QString &systemFont,
                                               std::shared_ptr<std::atomic_bool> contentRejected)
{
    auto *network = resourceNetwork();
    auto timings = std::make_shared<TransferTimings>();
    auto transfer = [path, port, lease, sizes, systemFont, timings,
                     contentRejected](const QString &name, const ResourceCache::Sink &sink,
                                      QString *error, QSize *dimensions) {
        if (!lease || lease->revoked() || port <= 0) {
            *error = strings::get("error.native_publication_is_closed");
            return false;
        }
        const auto secret = resourceSecret();
        if (secret.isEmpty()) {
            *error = strings::get("error.cannot_request_native_publication_resource");
            return false;
        }
        if (imageFileName(name) && sizes.value(name) > ResourceCache::budgetBytes) {
            // Directory metadata is enough here. Do not ask Nickel to read the
            // asset when its source cannot fit in the resource cache.
            const auto placeholder = imagePlaceholder({}, systemFont);
            return sink(placeholder.constData(), placeholder.size());
        }
        QUrl url;
        url.setPath("/EPUBPATH" + path, QUrl::DecodedMode);
        // This adapter speaks only the inspected loopback filename route. The
        // general Qt HTTP path added overhead to the measured small reads.
        const auto request = "GET " + url.toEncoded() + " HTTP/1.1\r\nHost: 127.0.0.1:"
                + QByteArray::number(port) + "\r\n" + resourceSecretHeader + ": " + secret
                + "\r\nfilename: " + QUrl::toPercentEncoding(name, "/")
                + (dimensions ? "\r\nX-Kobalt-Image-Dimensions: 1" : "")
                + "\r\nRange: bytes=0-0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
        QElapsedTimer requestTimer;
        requestTimer.start();
        ++timings->requests;
        QTcpSocket socket;
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.setReadBufferSize(ResourceCache::blockBytes);
        QEventLoop loop;
        QTimer deadline, cancellation;
        deadline.setSingleShot(true);
        deadline.start(5000);
        cancellation.start(50);
        bool probed = false;
        bool accepted = true, inspected = false, replaced = false, received = false,
             finished = false, headersDone = false;
        qint64 length = -1, bodyBytes = 0;
        QByteArray prefix, header;
        // CREngine parses reading content as HTML and can recover from errors
        // that a strict XML parser rejects. Only validate the package structure
        // here; chapters and navigation remain the engine's responsibility.
        const bool validateXml =
                name.endsWith(".opf", Qt::CaseInsensitive) || name == "META-INF/container.xml";
        QXmlStreamReader xml;
        auto finish = [&] {
            finished = true;
            loop.quit();
        };
        auto fail = [&](const QString &reason) {
            if (error->isEmpty())
                *error = reason;
            accepted = false;
            finish();
        };
        auto deliver = [&](const char *data, qint64 count) {
            QElapsedTimer work;
            work.start();
            const bool delivered = lease && !lease->revoked() && sink(data, count);
            timings->sinkNs += work.nsecsElapsed();
            if (!delivered) {
                accepted = false;
                return;
            }
            if (validateXml) {
                work.restart();
                xml.addData(QByteArray(data, count));
                while (!xml.atEnd())
                    xml.readNext();
                timings->xmlNs += work.nsecsElapsed();
                if (xml.hasError()
                    && xml.error() != QXmlStreamReader::PrematureEndOfDocumentError) {
                    if (contentRejected)
                        contentRejected->store(true);
                    *error = strings::get("error.nickel_did_not_supply_readable_publication_xml");
                    accepted = false;
                }
            }
        };
        auto drain = [&] {
            if (finished)
                return;
            if (!headersDone) {
                while (socket.bytesAvailable()) {
                    const auto line = socket.readLine(headerLimit + 1 - header.size());
                    header += line;
                    if (header.size() >= headerLimit) {
                        fail(strings::get("error.native_publication_response_header_is_too_large"));
                        return;
                    }
                    if (header.endsWith("\r\n\r\n")) {
                        if (!responseLength(header, &length, dimensions)) {
                            fail(strings::get("error.unsupported_native_publication_response"));
                            return;
                        }
                        headersDone = true;
                        break;
                    }
                    if (line.isEmpty())
                        break;
                }
                if (!headersDone)
                    return;
            }
            if (dimensions && dimensions->isValid()) {
                if (socket.bytesAvailable()) {
                    fail(strings::get("error.native_publication_response_has_extra_data"));
                    return;
                }
                probed = true;
                finish();
                return;
            }
            if (!received && socket.bytesAvailable()) {
                timings->firstByteNs += requestTimer.nsecsElapsed();
                received = true;
            }
            std::array<char, ResourceCache::blockBytes> block;
            while (accepted && !replaced && socket.bytesAvailable() && bodyBytes < length) {
                const auto limit = inspected
                        ? qint64(block.size())
                        : qMin(qint64(block.size()), qint64(imageHeaderLimit - prefix.size()));
                const auto count = socket.read(block.data(), qMin(limit, length - bodyBytes));
                if (count <= 0) {
                    accepted = false;
                    break;
                }
                bodyBytes += count;
                if (!inspected) {
                    prefix.append(block.data(), count);
                    const auto image = inspectImageHeader(prefix);
                    if (image.action == ImageHeader::More)
                        continue;
                    if (dimensions) {
                        if (image.action == ImageHeader::Keep)
                            *dimensions = image.size;
                        probed = true;
                        finish();
                        return;
                    }
                    if (image.action == ImageHeader::Replace) {
                        const auto placeholder = imagePlaceholder(image.size, systemFont);
                        deliver(placeholder.constData(), placeholder.size());
                        replaced = true;
                        prefix.clear();
                        break;
                    }
                    inspected = true;
                    deliver(prefix.constData(), prefix.size());
                    prefix.clear();
                } else
                    deliver(block.data(), count);
            }
            if (headersDone && bodyBytes == length && socket.bytesAvailable()) {
                fail(strings::get("error.native_publication_response_has_extra_data"));
                return;
            }
            if (!accepted || replaced || bodyBytes == length)
                finish();
        };
        QObject::connect(&socket, &QTcpSocket::connected, &loop, [&] {
            if (socket.write(request) != request.size())
                fail(strings::get("error.cannot_request_native_publication_resource"));
        });
        QObject::connect(&socket, &QTcpSocket::readyRead, &loop, drain);
        QObject::connect(&socket, &QTcpSocket::disconnected, &loop, [&] {
            drain();
            finish();
        });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        QObject::connect(&socket, &QTcpSocket::errorOccurred, &loop, [&] {
            drain();
            finish();
        });
#else
        QObject::connect(&socket,
                         static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                                 &QAbstractSocket::error),
                         &loop, [&] {
                             drain();
                             finish();
                         });
#endif
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
            fail(strings::get("error.native_publication_transfer_timed_out"));
        });
        QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
            if (!lease || lease->revoked())
                finish();
        });
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!finished)
            loop.exec();
        const int socketError = int(socket.error());
        socket.abort();
        if (!dimensions && !inspected && !replaced && accepted)
            deliver(prefix.constData(), prefix.size());
        if (contentRejected && accepted && lease && !lease->revoked() && headersDone
            && bodyBytes == length && validateXml && xml.hasError())
            contentRejected->store(true);
        const bool ok = accepted && lease && !lease->revoked() && headersDone
                && (probed || replaced || bodyBytes == length) && (!validateXml || !xml.hasError());
        if (!ok && dimensions)
            *dimensions = {};
        if (!ok && error->isEmpty())
            *error = strings::get("error.native_publication_transfer_failed");
        timings->requestNs += requestTimer.nsecsElapsed();
        if (!ok)
            ++timings->failures;
        if (!ok || requestTimer.elapsed() >= 1000)
            diagnostic(ok ? "resource-transfer-slow" : "resource-transfer-failed",
                       { { "error_code", diagnosticErrorCode(*error) },
                         { "elapsed_ms", double(requestTimer.elapsed()) },
                         { "request", double(timings->requests) },
                         { "image", imageFileName(name) },
                         { "xml", validateXml },
                         { "headers_received", headersDone },
                         { "expected_bytes", double(length) },
                         { "received_bytes", double(bodyBytes) },
                         { "socket_error", socketError },
                         { "cancelled", (!lease || lease->revoked()) } },
                       !ok);
        return ok;
    };
    auto loader = [network, transfer, timings](const QString &name, const ResourceCache::Sink &sink,
                                               QString *error) {
        bool result = false;
        QElapsedTimer queued;
        queued.start();
        kobalt::qt::invoke(
                network,
                [&] {
                    timings->queueNs += queued.nsecsElapsed();
                    transferring = true;
                    result = transfer(name, sink, error, nullptr);
                    transferring = false;
                    for (const auto &finished : pendingReleases)
                        kobalt::qt::invoke(qApp, finished, Qt::QueuedConnection);
                    pendingReleases.clear();
                },
                Qt::BlockingQueuedConnection);
        return result;
    };
    auto probe = [network, transfer, sizes](const QString &name) {
        // Small resources take the existing single-request path. Unknown and
        // vector formats keep their normal decoder and loading behavior.
        if (!imageFileName(name) || name.endsWith(".svg", Qt::CaseInsensitive)
            || sizes.value(name) < 256 * 1024 || sizes.value(name) > ResourceCache::budgetBytes)
            return QSize();
        QSize dimensions;
        kobalt::qt::invoke(
                network,
                [&] {
                    transferring = true;
                    QString error;
                    transfer(name, [](const char *, qint64) { return true; }, &error, &dimensions);
                    transferring = false;
                    for (const auto &finished : pendingReleases)
                        kobalt::qt::invoke(qApp, finished, Qt::QueuedConnection);
                    pendingReleases.clear();
                },
                Qt::BlockingQueuedConnection);
        return dimensions;
    };
    return std::make_shared<ResourceCache>(
            sizes.keys(),
            ResourceCache::Access{ std::move(loader),
                                   [lease] { return !lease || lease->revoked(); },
                                   [timings] { return timings->statistics(); }, std::move(probe) });
}

ResourceFactory nativeResourceFactory(QString path, int port, std::shared_ptr<SourceLease> lease)
{
    initializeResourceNetwork();
    return [path = std::move(path), port,
            lease = std::move(lease)](const QMap<QString, qint64> &sizes, const QString &font,
                                      std::shared_ptr<std::atomic_bool> rejected) {
        return nativeResources(sizes, path, port, lease, font, std::move(rejected));
    };
}
} // namespace kobalt
