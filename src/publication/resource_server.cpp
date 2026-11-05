#include "resource_server.h"
#include "zip_resource.h"
#include "support/qt_compat.h"
#include "native_archive.h"
#include "image_policy.h"
#include "native_resources.h"
#include "publication.h"
#include "support/diagnostics.h"
#include "platform/api/loopback.h"
#include "resource_cache.h"
#include <cerrno>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QNetworkReply>
#ifndef KOBALT_READER_HARDWARE
#  include <QSaveFile>
#endif
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace kobalt::native {
namespace {
class ImageBuffer : public QIODevice
{
public:
    ImageBuffer() { open(QIODevice::WriteOnly); }

    QByteArray bytes;

protected:
    qint64 readData(char *, qint64) override { return -1; }

    qint64 writeData(const char *data, qint64 size) override
    {
        if (size < 0 || size > ResourceCache::budgetBytes - bytes.size())
            return -1;
        bytes.append(data, int(size));
        return size;
    }
};

// SVG still uses the buffered vector path. Raster images use ZipResource
// directly, so their encoded source is never accumulated by this server.
QByteArray plainEntry(const QString &archive, const QString &name, ZipResourceIndex &index)
{
    ZipResource stream;
    if (!stream.open(archive, name, index))
        return {};
    QByteArray bytes;
    while (stream.valid() && !stream.finished())
        bytes += stream.next();
    return stream.valid() ? bytes : QByteArray();
}

bool svgSource(const QByteArray &bytes)
{
    int at = bytes.startsWith("\xef\xbb\xbf") ? 3 : 0;
    while (at < bytes.size()
           && (bytes[at] == ' ' || bytes[at] == '\n' || bytes[at] == '\r' || bytes[at] == '\t'))
        ++at;
    return at < bytes.size() && bytes[at] == '<' && bytes.left(4096).contains("<svg");
}

class Transfer : public QObject
{
public:
    Transfer(QTcpSocket *socket, ResourceArchive &archive, const QString &route,
             const QString &path, const QMap<QString, qint64> &sizes, ZipResourceIndex &index,
             QObject *parent)
        : QObject(parent),
          socket_(socket),
          archive_(archive),
          route_(route),
          path_(path),
          sizes_(sizes),
          index_(index)
    {
        elapsed_.start();
        socket_->setParent(this);
        socket_->setReadBufferSize(16 * 1024);
        deadline_.setSingleShot(true);
        deadline_.start(5000);
        connect(&deadline_, &QTimer::timeout, this, [this] {
            trace("deadline");
            socket_->abort();
            deleteLater();
        });
        connect(socket_, &QTcpSocket::disconnected, this, &QObject::deleteLater);
        connect(socket_, &QTcpSocket::readyRead, this, [this] { request(); });
        connect(socket_, &QTcpSocket::bytesWritten, this, [this](qint64) { schedule(); });
        request();
    }

    ~Transfer() override
    {
        if (reply_) {
            reply_->abort();
            delete reply_;
        }
    }

private:
    QTcpSocket *socket_;
    ResourceArchive &archive_;
    QString route_, path_, name_;
    const QMap<QString, qint64> &sizes_;
    ZipResourceIndex &index_;
    QTimer deadline_;
    QElapsedTimer elapsed_;
    QNetworkReply *reply_ = nullptr;
    QByteArray header_, body_;
    bool requested_ = false, failed_ = false, scheduled_ = false, sending_ = false;
    bool dimensionsOnly_ = false;
    std::unique_ptr<ZipResource> zip_;
    qint64 maxBlock_ = 0;
    qint64 sent_ = 0, length_ = 0;

    void trace(const char *stage)
    {
        diagnostic("native-resource-transfer-failed",
                   { { "stage", stage },
                     { "elapsed_ms", double(elapsed_.elapsed()) },
                     { "image", imageFileName(name_) },
                     { "native_bytes", body_.size() },
                     { "reply_error", reply_ ? int(reply_->error()) : -1 } },
                   kobalt::DiagnosticSeverity::Error);
    }

    void reject(const char *stage = "request")
    {
        trace(stage);
        failed_ = true;
        socket_->write(
                "HTTP/1.1 404 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket_->disconnectFromHost();
    }

    void schedule()
    {
        if (scheduled_ || failed_ || !sending_)
            return;
        scheduled_ = true;
        qt::later(0, this, [this] {
            scheduled_ = false;
            pump();
        });
    }

    void begin(qint64 length)
    {
        sending_ = true;
        length_ = length;
        socket_->write("HTTP/1.1 206 Partial Content\r\nContent-Length: "
                       + QByteArray::number(length) + "\r\nConnection: close\r\n\r\n");
        schedule();
    }

    void sendDimensions(QSize size)
    {
        if (size.width() <= 0 || size.height() <= 0 || size.width() > 32768 || size.height() > 32768
            || quint64(size.width()) * size.height() > imagePixelLimit) {
            reject("image-dimensions");
            return;
        }
        socket_->write(
                "HTTP/1.1 206 Partial Content\r\nContent-Length: 0\r\nX-Kobalt-Image-Dimensions: 1 "
                + QByteArray::number(size.width()) + " " + QByteArray::number(size.height())
                + "\r\nConnection: close\r\n\r\n");
        socket_->disconnectFromHost();
    }

    void pump()
    {
        if (failed_ || socket_->state() != QAbstractSocket::ConnectedState)
            return;
        if (!sending_ || socket_->bytesToWrite() != 0)
            return;
        if (sent_ == length_) {
            if (zip_)
                diagnostic("zip-image-stream-complete",
                           { { "encoded_bytes", double(length_) },
                             { "max_block_bytes", double(maxBlock_) },
                             { "elapsed_ms", double(elapsed_.elapsed()) } },
                           false);
            socket_->disconnectFromHost();
            return;
        }
        const auto count = qMin<qint64>(ResourceCache::blockBytes, length_ - sent_);
        QByteArray bytes;
        if (zip_) {
            if (!body_.isEmpty()) {
                bytes = std::move(body_);
                body_.clear();
            } else
                bytes = zip_->next();
            if (!zip_->valid() || bytes.size() > count) {
                trace("zip-image-read");
                failed_ = true;
                socket_->abort();
                return;
            }
            if (bytes.isEmpty()) {
                schedule();
                return;
            }
        } else
            bytes = body_.mid(int(sent_), int(count));
        if (socket_->write(bytes) != bytes.size()) {
            failed_ = true;
            socket_->abort();
            return;
        }
        maxBlock_ = qMax<qint64>(maxBlock_, bytes.size());
        sent_ += bytes.size();
    }

    bool startZip(const QSize &expected = {})
    {
        auto stream = std::make_unique<ZipResource>();
        if (!stream->open(path_, name_, index_))
            return false;
        auto prefix = stream->next();
        if (!stream->valid())
            return false;
        auto header = inspectImageHeader(prefix.left(imageHeaderLimit));
        for (int blocks = 1; dimensionsOnly_ && header.action == ImageHeader::More
             && !stream->finished() && blocks < imageHeaderLimit / ResourceCache::blockBytes;
             ++blocks) {
            prefix += stream->next();
            if (!stream->valid())
                return false;
            header = inspectImageHeader(prefix.left(imageHeaderLimit));
        }
        if (header.action != ImageHeader::Keep || !header.size.isValid()
            || (expected.isValid() && header.size != expected))
            return false;
        if (dimensionsOnly_) {
            sendDimensions(header.size);
            return true;
        }
        body_ = std::move(prefix);
        zip_ = std::move(stream);
        begin(zip_->size());
        diagnostic("zip-image-stream", { { "encoded_bytes", double(zip_->size()) } }, false);
        return true;
    }

    void send(const QByteArray &bytes)
    {
        if (failed_ || socket_->state() != QAbstractSocket::ConnectedState)
            return;
        body_ = bytes;
        begin(body_.size());
    }

    void drain()
    {
        if (!reply_ || failed_)
            return;
        while (reply_->bytesAvailable()) {
            if (body_.size() == ResourceCache::budgetBytes) {
                reject("native-size-limit");
                reply_->abort();
                return;
            }
            const auto chunk = reply_->read(
                    qMin(ResourceCache::blockBytes, ResourceCache::budgetBytes - body_.size()));
            if (chunk.isEmpty())
                break;
            body_.append(chunk);
        }
    }

    void finish()
    {
        drain();
        if (failed_)
            return;
        if (reply_->error() != QNetworkReply::NoError) {
            reject("native-reply");
            return;
        }
        const auto length = reply_->header(QNetworkRequest::ContentLengthHeader);
        if (!length.isValid() || length.toLongLong() != body_.size()) {
            reject("native-length");
            return;
        }
        // kimg contains a native image reference, not portable image bytes.
        // Nickel's image plugin must consume it while its owners remain alive.
        if (reply_->header(QNetworkRequest::ContentTypeHeader).toByteArray() == "image/x-kobo") {
            // For an SVG file Nickel sends a reference to its own raster, which
            // loses the vector source and is empty in the simulator. A plain
            // book still holds the source. Serve it only if the entry is
            // unencrypted, passes its CRC and is SVG text; a protected book's
            // entries are encrypted and keep the native raster below.
            if (name_.endsWith(".svg", Qt::CaseInsensitive)) {
                const auto source = plainEntry(path_, name_, index_);
                if (svgSource(source)) {
                    send(source);
                    return;
                }
            }
            QBuffer source(&body_);
            source.open(QIODevice::ReadOnly);
            QImageReader image(&source);
            const auto size = image.size();
            // Nickel has accepted the image reference. Layout needs its size,
            // so defer the native pixel decode and PNG conversion to the full request.
            if (dimensionsOnly_) {
                sendDimensions(size);
                return;
            }
            if (!size.isValid()
                || quint64(size.width()) * quint64(size.height()) > imagePixelLimit) {
                send(imagePlaceholder(size));
                return;
            }
            // Only use plaintext which matches Nickel's accepted image. A
            // native filter keeps its native decode path, including for ZIPs.
            const bool filtered = body_.size() == 20 && body_.startsWith(QByteArray("GT\0", 3))
                    && body_.at(11) != 0;
            if (!filtered && startZip(size))
                return;
            QElapsedTimer conversion;
            conversion.start();
            const auto pixels = image.read();
            const auto decodeMs = conversion.elapsed();
            if (pixels.isNull()) {
                reject("image-decode");
                return;
            }
            ImageBuffer encoded;
            if (!pixels.save(&encoded, "PNG")) {
                reject("image-encode");
                return;
            }
            if (elapsed_.elapsed() >= 1000)
                diagnostic("native-image-slow",
                           { { "elapsed_ms", double(elapsed_.elapsed()) },
                             { "decode_ms", double(decodeMs) },
                             { "encode_ms", double(conversion.elapsed() - decodeMs) },
                             { "width", size.width() },
                             { "height", size.height() },
                             { "encoded_bytes", encoded.bytes.size() } },
                           false);
            send(encoded.bytes);
            return;
        }
        if (dimensionsOnly_) {
            const auto header = inspectImageHeader(body_.left(imageHeaderLimit));
            if (header.action != ImageHeader::Keep) {
                reject("image-dimensions");
                return;
            }
            sendDimensions(header.size);
            return;
        }
        send(body_);
    }

    void request()
    {
        if (requested_)
            return;
        header_ += socket_->read(16 * 1024 - header_.size());
        if (header_.size() >= 16 * 1024) {
            requested_ = true;
            reject();
            return;
        }
        const auto end = header_.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        requested_ = true;
        if (end + 4 != header_.size()) {
            reject();
            return;
        }
        const auto lines = header_.split('\n');
        if (lines.isEmpty() || lines.front() != "GET " + route_.toUtf8() + " HTTP/1.1\r") {
            reject();
            return;
        }
        QByteArray encodedName, secret;
        bool hasSecret = false;
        for (const auto &line : lines) {
            if (line.toLower().startsWith(QByteArray(resourceSecretHeader) + ':')) {
                if (hasSecret) {
                    reject();
                    return;
                }
                secret = line.mid(int(qstrlen(resourceSecretHeader)) + 1).trimmed();
                hasSecret = true;
                continue;
            }
            if (line.toLower().startsWith("x-kobalt-image-dimensions:")) {
                if (dimensionsOnly_ || line.mid(26).trimmed() != "1") {
                    reject();
                    return;
                }
                dimensionsOnly_ = true;
                continue;
            }
            if (!line.toLower().startsWith("filename:"))
                continue;
            if (!encodedName.isEmpty()) {
                reject();
                return;
            }
            encodedName = line.mid(9).trimmed();
        }
        if (!resourceSecretMatches(secret)) {
            reject("secret");
            return;
        }
        const QString name = QUrl::fromPercentEncoding(encodedName);
        if (!sizes_.contains(name)) {
            reject();
            return;
        }
        name_ = name;
        load();
    }

    void load()
    {
        if (failed_ || socket_->state() != QAbstractSocket::ConnectedState)
            return;
        const auto &name = name_;
        if (dimensionsOnly_
            && (!imageFileName(name) || name.endsWith(".svg", Qt::CaseInsensitive)
                || sizes_.value(name) > ResourceCache::budgetBytes)) {
            reject("image-dimensions");
            return;
        }
        if (imageFileName(name) && sizes_.value(name) > ResourceCache::budgetBytes) {
            send(imagePlaceholder());
            return;
        }
        if (imageFileName(name) && !name.endsWith(".svg", Qt::CaseInsensitive)
            && archive_.canStreamPlainImage(name) && startZip())
            return;
        reply_ = archive_.request(name);
        if (!reply_) {
            reject("native-request");
            return;
        }
        connect(reply_, &QNetworkReply::readyRead, this, [this] { drain(); });
        // Qt5's native reply emits this signal without setting isFinished().
        connect(reply_, &QNetworkReply::finished, this, [this] { finish(); });
    }
};
} // namespace

struct ResourceServer::Data
{
    std::unique_ptr<ResourceArchive> archive;
    QTcpServer listener;
    QMap<QString, qint64> sizes;
    // Transfers share the GUI thread and open each stream synchronously.
    ZipResourceIndex index;
    QString route, path;
};

ResourceServer::ResourceServer(QObject *parent) : QObject(parent) { }

ResourceServer::~ResourceServer()
{
    // QObject normally deletes children after members. Transfers refer to the
    // native archive and directory, so destroy them before those members.
    const auto owned = children();
    for (auto *child : owned)
        delete child;
}

bool ResourceServer::open(const QString &path, const OpenArchive &openArchive)
{
    if (data_)
        return false;
    if (resourceSecret().isEmpty()) {
        diagnostic("native-resource-failed", { { "stage", "secret" } },
                   kobalt::DiagnosticSeverity::Error);
        return false;
    }
    auto data = std::make_unique<Data>();
    QFile file(path);
    Publication publication;
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostic("native-resource-failed",
                   { { "stage", "file-open" }, { "error", int(file.error()) } },
                   kobalt::DiagnosticSeverity::Error);
        return false;
    }
    if (!publication.inspect(file, &error)) {
        diagnostic("native-resource-failed",
                   { { "stage", "publication-inspection" },
                     { "error_code", diagnosticErrorCode(error) } },
                   kobalt::DiagnosticSeverity::Error);
        return false;
    }
    if (!openArchive)
        return false;
    data->archive = openArchive();
    if (!data->archive)
        return false;
    data->sizes = publication.sizes();
    data->path = path;
    QUrl route;
    route.setPath("/EPUBPATH" + path, QUrl::DecodedMode);
    data->route = QString::fromLatin1(route.toEncoded());
    // An addressed but down loopback can accept bind() while connections fail.
    // Check it before listening, rather than waiting for a bind failure.
    int loopbackError = 0;
    const bool configured = platform::configureLoopback(&loopbackError);
    if (configured || (loopbackError && loopbackError != ENOTSUP)) {
        diagnostic("loopback-setup", { { "configured", configured }, { "error", loopbackError } });
    }
    if (loopbackError && loopbackError != ENOTSUP) {
        diagnostic("native-resource-failed",
                   { { "stage", "loopback-setup" }, { "error", loopbackError } },
                   kobalt::DiagnosticSeverity::Error);
        return false;
    }
    if (!data->listener.listen(QHostAddress::LocalHost, 0)) {
        diagnostic(
                "native-resource-failed",
                { { "stage", "loopback-listen" }, { "error", int(data->listener.serverError()) } },
                kobalt::DiagnosticSeverity::Error);
        return false;
    }
    data_ = std::move(data);
    diagnostic("native-resource-listening", { { "port", int(data_->listener.serverPort()) } },
               false);
    connect(&data_->listener, &QTcpServer::newConnection, this, [this] {
        while (auto *socket = data_->listener.nextPendingConnection()) {
            // Leave room for completed connections awaiting deferred deletion.
            if (children().size() >= 4) {
                socket->abort();
                delete socket;
                continue;
            }
            new Transfer(socket, *data_->archive, data_->route, data_->path, data_->sizes,
                         data_->index, this);
        }
    });
    return true;
}

int ResourceServer::port() const
{
    return data_ ? data_->listener.serverPort() : 0;
}
} // namespace kobalt::native
