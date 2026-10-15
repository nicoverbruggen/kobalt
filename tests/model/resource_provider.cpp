#include "publication/resource_server.h"
#include "publication/publication.h"
#include "publication/native_resources.h"
#include "publication/resource_cache.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include "check.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QtEndian>
#include <zlib.h>
#include <cstring>
#include <vector>
using namespace kobalt;
namespace {
void u16(QByteArray &bytes, quint16 value) {
    value = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&value), 2);
}
void u32(QByteArray &bytes, quint32 value) {
    value = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&value), 4);
}
QByteArray zip(const QMap<QString, QByteArray> &files) {
    QByteArray bytes, directory;
    QList<QPair<QByteArray, QByteArray>> entries;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        entries.append({it.key().toUtf8(), it.value()});
    for (const auto &entry : entries) {
        const auto offset = bytes.size();
        const auto crc = crc32(0, reinterpret_cast<const Bytef *>(entry.second.constData()), entry.second.size());
        u32(bytes, 0x04034b50);
        u16(bytes, 20);
        u16(bytes, 0);
        u16(bytes, 0);
        u32(bytes, 0);
        u32(bytes, crc);
        u32(bytes, entry.second.size());
        u32(bytes, entry.second.size());
        u16(bytes, entry.first.size());
        u16(bytes, 0);
        bytes += entry.first;
        bytes += entry.second;
        u32(directory, 0x02014b50);
        u16(directory, 20);
        u16(directory, 20);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, crc);
        u32(directory, entry.second.size());
        u32(directory, entry.second.size());
        u16(directory, entry.first.size());
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, offset);
        directory += entry.first;
    }
    const auto offset = bytes.size();
    bytes += directory;
    u32(bytes, 0x06054b50);
    u16(bytes, 0);
    u16(bytes, 0);
    u16(bytes, entries.size());
    u16(bytes, entries.size());
    u32(bytes, directory.size());
    u32(bytes, offset);
    u16(bytes, 0);
    return bytes;
}
struct Counters {
    int opened = 0, requested = 0, aborted = 0, replies = 0, destroyed = 0;
};
class PendingReply final : public QNetworkReply {
public:
    explicit PendingReply(Counters &counts) : counts(counts) {
        ++counts.replies;
        open(QIODevice::ReadOnly);
    }
    ~PendingReply() override {
        --counts.replies;
    }
    void abort() override {
        ++counts.aborted;
        close();
    }

protected:
    qint64 readData(char *, qint64) override {
        return 0;
    }

private:
    Counters &counts;
};
class Archive final : public native::ResourceArchive {
public:
    explicit Archive(Counters &counts, bool streamImages = false) : counts(counts), streamImages(streamImages) {
        ++counts.opened;
    }
    ~Archive() override {
        require(counts.replies == 0, "replies destroyed before provider");
        ++counts.destroyed;
    }
    QNetworkReply *request(const QString &) override {
        ++counts.requested;
        return new PendingReply(counts);
    }
    bool canStreamPlainImage(const QString &) const override {
        return streamImages;
    }

private:
    Counters &counts;
    bool streamImages;
};

class ControlledReply final : public QNetworkReply {
public:
    explicit ControlledReply(Counters &counts) : counts(counts) {
        ++counts.replies;
        open(QIODevice::ReadOnly);
    }
    ~ControlledReply() override {
        --counts.replies;
    }
    void abort() override {
        ++counts.aborted;
        close();
    }
    bool isSequential() const override {
        return true;
    }
    qint64 bytesAvailable() const override {
        return bytes.size() - consumed + QNetworkReply::bytesAvailable();
    }
    void append(const QByteArray &chunk) {
        bytes += chunk;
        emit readyRead();
    }
    void complete(const QVariant &length, NetworkError error = NoError, const QByteArray &type = {}) {
        if (length.isValid())
            setHeader(QNetworkRequest::ContentLengthHeader, length);
        if (!type.isEmpty())
            setHeader(QNetworkRequest::ContentTypeHeader, type);
        if (error != NoError)
            setError(error, "controlled provider failure");
        // The native Qt5 reply emits finished without setting isFinished().
        emit finished();
    }

protected:
    qint64 readData(char *destination, qint64 maximum) override {
        const auto count = qMin<qint64>(maximum, bytes.size() - consumed);
        if (count <= 0)
            return 0;
        std::memcpy(destination, bytes.constData() + consumed, size_t(count));
        consumed += count;
        return count;
    }

private:
    Counters &counts;
    QByteArray bytes;
    qint64 consumed = 0;
};

class ProtocolArchive final : public native::ResourceArchive {
public:
    explicit ProtocolArchive(Counters &counts) : counts(counts) {
        ++counts.opened;
    }
    ~ProtocolArchive() override {
        require(counts.replies == 0, "protocol replies destroyed before provider");
        ++counts.destroyed;
    }
    QNetworkReply *request(const QString &name) override {
        ++counts.requested;
        lastName = name;
        if (refuse)
            return nullptr;
        latest = new ControlledReply(counts);
        return latest;
    }
    bool canStreamPlainImage(const QString &) const override {
        return false;
    }
    QPointer<ControlledReply> latest;
    QString lastName;
    bool refuse = false;

private:
    Counters &counts;
};

void waitFor(const std::function<bool()> &done, const char *message) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
    require(done(), message);
}

void connectRequest(QTcpSocket &socket, const native::ResourceServer &server, const QByteArray &request) {
    socket.connectToHost(QHostAddress::LocalHost, server.port());
    require(socket.waitForConnected(), "connect protocol request");
    require(socket.write(request) == request.size(), "send protocol request");
    socket.flush();
}

QByteArray response(QTcpSocket &socket) {
    QByteArray bytes;
    waitFor(
        [&] {
            bytes += socket.readAll();
            return socket.state() == QAbstractSocket::UnconnectedState;
        },
        "resource response closes connection");
    bytes += socket.readAll();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return bytes;
}

QByteArray body(const QByteArray &response) {
    const int end = response.indexOf("\r\n\r\n");
    require(end >= 0, "HTTP response has complete headers");
    return response.mid(end + 4);
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    initializeDiagnostics(dir.path());
    const auto logPath = dir.path() + "/events.jsonl";
    const auto path = dir.path() + "/book#part?name%20.kepub.epub";
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create malformed fixture");
    file.write("not a ZIP");
    file.close();
    Counters counts;
    auto factory = [&]() -> std::unique_ptr<native::ResourceArchive> {
        return std::make_unique<Archive>(counts);
    };
    auto server = std::make_unique<native::ResourceServer>();
    require(!server->open(path, factory), "reject malformed publication");
    require(counts.opened == 0, "inspect before invoking provider factory");
    auto inspectionError = [&] {
        QFile log(logPath);
        require(log.open(QIODevice::ReadOnly), "read archive inspection log");
        const auto bytes = log.readAll();
        require(!bytes.contains(path.toUtf8()), "archive inspection log disclosed book path");
        QJsonObject result;
        for (const auto &line : bytes.split('\n')) {
            const auto record = QJsonDocument::fromJson(line).object();
            if (record.value("event").toString() == "native-resource-failed" &&
                record.value("stage").toString() == "publication-inspection") {
                result = record;
            }
        }
        return result.value("error_code").toString();
    };
    require(inspectionError() == "error.unsupported_publication_size", "malformed archive lost inspection reason");
    const auto encrypted = zip({{"mimetype", "application/epub+zip"},
                                {"META-INF/container.xml", "<container/>"},
                                {"META-INF/encryption.xml", "<encryption/>"}});
    require(file.open(QIODevice::WriteOnly) && file.write(encrypted) == encrypted.size(),
            "write encryption metadata fixture");
    file.close();
    require(!server->open(path, factory) && counts.opened == 0, "encryption metadata reached resource provider");
    require(inspectionError() == "error.this_publication_needs_unsupported_font_or_resource_handling",
            "encryption metadata lost inspection reason");
    QMap<QString, QByteArray> obfuscatedFiles = {
        {"mimetype", "application/epub+zip"},
        {"META-INF/container.xml",
         "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>"},
        {"book.opf",
         "<package xmlns='http://www.idpf.org/2007/opf' unique-identifier='book-id'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='book-id'> test identifier </dc:identifier></metadata><manifest><item id='font' href='font.otf' media-type='application/vnd.ms-opentype'/></manifest></package>"},
        {"font.otf", "obfuscated font bytes"},
        {"META-INF/encryption.xml",
         "<encryption xmlns='urn:oasis:names:tc:opendocument:xmlns:container' xmlns:enc='http://www.w3.org/2001/04/xmlenc#'><enc:EncryptedData><enc:EncryptionMethod Algorithm='http://www.idpf.org/2008/embedding'/><enc:CipherData><enc:CipherReference URI='font.otf'/></enc:CipherData></enc:EncryptedData></encryption>"}};
    Publication inspection;
    QString reason;
    require(inspection.inspect(zip(obfuscatedFiles), &reason), "accept standard IDPF font metadata");
    const auto validMetadata = obfuscatedFiles.value("META-INF/encryption.xml");
    const auto validPackage = obfuscatedFiles.value("book.opf");
    auto adobeMetadata = validMetadata;
    adobeMetadata.replace("http://www.idpf.org/2008/embedding", "http://ns.adobe.com/pdf/enc#RC");
    auto adobePackage = validPackage;
    adobePackage.replace(
        "</metadata>",
        "<dc:identifier id='font-id'>urn:uuid:01234567-89ab-cdef-0123-456789abcdef</dc:identifier></metadata>");
    obfuscatedFiles["META-INF/encryption.xml"] = adobeMetadata;
    obfuscatedFiles["book.opf"] = adobePackage;
    require(inspection.inspect(zip(obfuscatedFiles), &reason), "accept Adobe font metadata with a separate UUID");
    obfuscatedFiles["book.opf"] = validPackage;
    require(!inspection.inspect(zip(obfuscatedFiles), &reason), "reject Adobe font metadata without a UUID");
    for (const auto &replacement :
         {QPair<QByteArray, QByteArray>{"font.otf", "book.opf"},
          {"font.otf", "missing.otf"},
          {"http://ns.adobe.com/pdf/enc#RC", "http://www.w3.org/2001/04/xmlenc#aes128-cbc"}}) {
        obfuscatedFiles["book.opf"] = adobePackage;
        obfuscatedFiles["META-INF/encryption.xml"] = adobeMetadata;
        obfuscatedFiles["META-INF/encryption.xml"].replace(replacement.first, replacement.second);
        require(!inspection.inspect(zip(obfuscatedFiles), &reason), "reject unsupported Adobe resource handling");
    }
    obfuscatedFiles["book.opf"] = validPackage;
    for (const auto &replacement : {QPair<QByteArray, QByteArray>{"http://www.idpf.org/2008/embedding", "urn:unknown"},
                                    {"font.otf", "book.opf"},
                                    {"font.otf", "missing.otf"},
                                    {"font.otf", "../font.otf"},
                                    {"font.otf", "font%2eotf"},
                                    {"font.otf", "https://invalid.test/font.otf"},
                                    {"http://www.w3.org/2001/04/xmlenc#", "urn:wrong-namespace"},
                                    {"enc:CipherReference", "enc:CipherValue"}}) {
        obfuscatedFiles["META-INF/encryption.xml"] = validMetadata;
        obfuscatedFiles["META-INF/encryption.xml"].replace(replacement.first, replacement.second);
        require(!inspection.inspect(zip(obfuscatedFiles), &reason),
                "reject unsupported or ambiguous encryption metadata");
    }
    obfuscatedFiles["META-INF/encryption.xml"] = "<!DOCTYPE encryption [<!ENTITY target 'font.otf'>]>" + validMetadata;
    require(!inspection.inspect(zip(obfuscatedFiles), &reason), "reject encryption metadata DTD");
    auto repeated = validMetadata;
    const auto declaration =
        validMetadata.mid(validMetadata.indexOf("<enc:EncryptedData>"),
                          validMetadata.indexOf("</encryption>") - validMetadata.indexOf("<enc:EncryptedData>"));
    repeated.replace("</encryption>", declaration + "</encryption>");
    obfuscatedFiles["META-INF/encryption.xml"] = repeated;
    require(!inspection.inspect(zip(obfuscatedFiles), &reason), "reject repeated obfuscated font target");
    obfuscatedFiles["META-INF/encryption.xml"] = validMetadata;
    for (const auto &replacement : {QPair<QByteArray, QByteArray>{"http://www.idpf.org/2007/opf", "urn:wrong"},
                                    {"<manifest>", "<manifest xmlns='urn:wrong'>"},
                                    {"<metadata ", "<outside><metadata "},
                                    {"application/vnd.ms-opentype", "application/xhtml+xml"}}) {
        obfuscatedFiles["book.opf"] = validPackage;
        obfuscatedFiles["book.opf"].replace(replacement.first, replacement.second);
        if (replacement.first == "<metadata ")
            obfuscatedFiles["book.opf"].replace("</metadata>", "</metadata></outside>");
        require(!inspection.inspect(zip(obfuscatedFiles), &reason), "reject invalid package font declarations");
    }
    obfuscatedFiles["book.opf"] = validPackage;
    obfuscatedFiles["META-INF/encryption.xml"] = validMetadata;
    obfuscatedFiles["book.opf"].replace("id='book-id'", "id='other'");
    require(!inspection.inspect(zip(obfuscatedFiles), &reason), "reject missing unique identifier");
    obfuscatedFiles["META-INF/license.lcpl"] = "{}";
    OpeningResult protectedResult;
    require(!inspection.inspect(zip(obfuscatedFiles), &reason, &protectedResult) && inspection.protectedBook() &&
                protectedResult.failure() && protectedResult.failure()->kind == OpeningFailureKind::Protected,
            "font metadata must not bypass LCP detection");
    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(Qt::black);
    QByteArray png;
    QBuffer imageOutput(&png);
    require(imageOutput.open(QIODevice::WriteOnly) && image.save(&imageOutput, "PNG"), "create public image fixture");
    const auto largePng = png + QByteArray(ResourceCache::blockBytes * 2, 'x');
    const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' width='3' height='2'></svg>";
    const auto bytes = zip(
        {{"mimetype", "application/epub+zip"},
         {"META-INF/container.xml", "<container><rootfiles><rootfile full-path='book.opf'/></rootfiles></container>"},
         {"book.opf",
          "<package><manifest><item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='chapter'/></spine></package>"},
         {"chapter.xhtml", "<html><body>Test</body></html>"},
         {QString::fromUtf8("notes/caf\xc3\xa9.xhtml"), "encoded filename"},
         {"image.png", largePng},
         {"image.svg", svg}});
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write publication fixture");
    file.close();
    int refused = 0;
    require(!server->open(path,
                          [&]() -> std::unique_ptr<native::ResourceArchive> {
                              ++refused;
                              return {};
                          }),
            "provider refusal stops opening");
    require(refused == 1 && !server->active() && server->port() == 0, "refused provider exposes no endpoint");
    auto token = std::make_shared<int>(1);
    std::weak_ptr<int> borrowed = token;
    require(server->open(path,
                         [&, token] {
                             return factory();
                         }),
            "open accepted provider");
    token.reset();
    require(borrowed.expired(), "factory callback is not retained");
    require(counts.opened == 1 && server->active(), "one provider owns active endpoint");
    require(!server->open(path, factory) && counts.opened == 1, "duplicate open does not create provider");
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, server->port());
    require(socket.waitForConnected(), "connect pending request");
    QUrl route;
    route.setPath("/EPUBPATH" + path, QUrl::DecodedMode);
    socket.write("GET " + route.toEncoded() +
                 " HTTP/1.1\r\nfilename: chapter.xhtml\r\nX-Kobalt-Secret: " + resourceSecret() + "\r\n\r\n");
    socket.flush();
    QElapsedTimer elapsed;
    elapsed.start();
    while (!counts.requested && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(counts.requested == 1 && counts.replies == 1, "request remains pending during release");
    server.reset();
    require(counts.destroyed == 1 && counts.replies == 0 && counts.aborted == 1,
            "cancel reply before releasing provider");

    Counters streamedCounts;
    native::ResourceServer streamed;
    require(streamed.open(path,
                          [&] {
                              return std::make_unique<Archive>(streamedCounts, true);
                          }),
            "open provider that permits plaintext image streaming");
    for (bool dimensions : {false, true}) {
        QTcpSocket client;
        connectRequest(client, streamed,
                       "GET " + route.toEncoded() +
                           " HTTP/1.1\r\nfilename: image.png\r\nX-Kobalt-Secret: " + resourceSecret() + "\r\n" +
                           (dimensions ? QByteArray("X-Kobalt-Image-Dimensions: 1\r\n") : QByteArray()) + "\r\n");
        const auto reply = response(client);
        require(reply.startsWith("HTTP/1.1 206 "), "plaintext image request succeeds");
        require(streamedCounts.requested == 0, "plaintext image stream bypasses whole-file provider replies");
        if (dimensions) {
            require(body(reply).isEmpty() && reply.contains("X-Kobalt-Image-Dimensions: 1 3 2\r\n"),
                    "plaintext image dimensions do not transfer the body");
        } else {
            require(body(reply) == largePng, "plaintext image stream preserves every byte across blocks");
        }
    }

    Counters protocolCounts;
    ProtocolArchive *provider = nullptr;
    native::ResourceServer protocol;
    require(protocol.open(path,
                          [&] {
                              auto archive = std::make_unique<ProtocolArchive>(protocolCounts);
                              provider = archive.get();
                              return archive;
                          }),
            "open controlled protocol provider");
    const QByteArray firstLine = "GET " + route.toEncoded() + " HTTP/1.1\r\n";
    const QByteArray auth = "X-Kobalt-Secret: " + resourceSecret() + "\r\n";
    const QByteArray filename = "filename: chapter.xhtml\r\n";
    const QList<QByteArray> rejected = {firstLine + filename + "\r\n",
                                        firstLine + filename + "X-Kobalt-Secret: wrong\r\n\r\n",
                                        firstLine + filename + auth + auth + "\r\n",
                                        "POST " + route.toEncoded() + " HTTP/1.1\r\n" + filename + auth + "\r\n",
                                        "GET /other HTTP/1.1\r\n" + filename + auth + "\r\n",
                                        firstLine + auth + "\r\n",
                                        firstLine + "filename: missing.xhtml\r\n" + auth + "\r\n",
                                        firstLine + filename + filename + auth + "\r\n",
                                        firstLine + filename + auth + "\r\ntrailing-body",
                                        firstLine + filename + auth + "X-Kobalt-Image-Dimensions: 2\r\n\r\n",
                                        firstLine + filename + auth +
                                            "X-Kobalt-Image-Dimensions: 1\r\nX-Kobalt-Image-Dimensions: 1\r\n\r\n",
                                        firstLine + filename + auth + "X-Kobalt-Image-Dimensions: 1\r\n\r\n",
                                        QByteArray(16 * 1024, 'x')};
    for (const auto &request : rejected) {
        QTcpSocket client;
        connectRequest(client, protocol, request);
        const auto reply = response(client);
        require(reply.startsWith("HTTP/1.1 404 ") && body(reply).isEmpty(),
                "invalid or unauthenticated request rejected without content");
        require(protocolCounts.requested == 0, "rejected request does not reach provider");
    }

    const QByteArray validRequest = firstLine + filename + auth + "\r\n";
    {
        QTcpSocket client;
        connectRequest(client, protocol, validRequest.left(validRequest.size() - 2));
        QCoreApplication::processEvents();
        require(protocolCounts.requested == 0, "incomplete headers do not reach provider");
        client.write("\r\n");
        client.flush();
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "fragmented request reaches provider after terminator");
        const QByteArray payload(ResourceCache::blockBytes * 2 + 17, 'p');
        provider->latest->append(payload.left(51));
        provider->latest->append(payload.mid(51));
        provider->latest->complete(payload.size());
        const auto reply = response(client);
        require(reply.startsWith("HTTP/1.1 206 ") && body(reply) == payload,
                "stream provider chunks into exact multi-block response");
        require(reply.contains("Content-Length: " + QByteArray::number(payload.size()) + "\r\n"),
                "response declares exact payload length");
        require(protocolCounts.replies == 0, "completed transfer releases native reply");
    }
    {
        QTcpSocket client;
        const QString name = QString::fromUtf8("notes/caf\xc3\xa9.xhtml");
        connectRequest(client, protocol,
                       firstLine + "FiLeNaMe: " + QUrl::toPercentEncoding(name) + "\r\n" + auth.toLower() + "\r\n");
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "encoded filename reaches provider");
        require(provider->lastName == name, "provider receives decoded Unicode filename");
        provider->latest->complete(0);
        const auto reply = response(client);
        require(reply.startsWith("HTTP/1.1 206 ") && body(reply).isEmpty(),
                "empty successful resource is distinct from failure");
    }
    for (int failure = 0; failure < 3; ++failure) {
        QTcpSocket client;
        connectRequest(client, protocol, validRequest);
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "failure request reaches provider");
        provider->latest->append("partial");
        if (failure == 0)
            provider->latest->complete({}, QNetworkReply::ContentNotFoundError);
        else
            provider->latest->complete(failure == 1 ? QVariant() : QVariant(99));
        const auto reply = response(client);
        require(reply.startsWith("HTTP/1.1 404 ") && body(reply).isEmpty(),
                "provider error or invalid length never exposes partial bytes");
    }
    {
        provider->refuse = true;
        QTcpSocket client;
        connectRequest(client, protocol, validRequest);
        require(response(client).startsWith("HTTP/1.1 404 "), "missing native reply rejected");
        provider->refuse = false;
    }
    {
        QTcpSocket client;
        connectRequest(client, protocol,
                       firstLine + "filename: image.png\r\n" + auth + "X-Kobalt-Image-Dimensions: 1\r\n\r\n");
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "image dimension request reaches provider");
        provider->latest->append(png);
        provider->latest->complete(png.size());
        const auto reply = response(client);
        require(reply.startsWith("HTTP/1.1 206 ") && reply.contains("X-Kobalt-Image-Dimensions: 1 3 2\r\n") &&
                    body(reply).isEmpty(),
                "dimension probe returns dimensions without image body");
    }
    {
        QTcpSocket client;
        connectRequest(client, protocol, firstLine + "filename: image.svg\r\n" + auth + "\r\n");
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "SVG request reaches provider");
        provider->latest->append("native-image-reference");
        provider->latest->complete(22, QNetworkReply::NoError, "image/x-kobo");
        require(body(response(client)) == svg, "native SVG reference uses validated plaintext vector source");
    }
    {
        const int aborted = protocolCounts.aborted;
        QTcpSocket client;
        connectRequest(client, protocol, validRequest);
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "pending request reaches provider before disconnect");
        client.abort();
        waitFor(
            [&] {
                return protocolCounts.replies == 0;
            },
            "client disconnect releases provider reply");
        require(protocolCounts.aborted == aborted + 1, "client disconnect aborts pending reply once");
    }
    {
        QTcpSocket client;
        connectRequest(client, protocol, validRequest);
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "bounded transfer reaches provider");
        provider->latest->append(QByteArray(ResourceCache::budgetBytes + 1, 'x'));
        const auto reply = response(client);
        require(reply.startsWith("HTTP/1.1 404 ") && body(reply).isEmpty(),
                "over-budget native reply rejected without partial content");
        require(protocolCounts.replies == 0, "over-budget transfer releases its reply");
    }
    {
        const int requests = protocolCounts.requested;
        std::vector<std::unique_ptr<QTcpSocket>> clients;
        for (int i = 0; i < 4; ++i) {
            auto client = std::make_unique<QTcpSocket>();
            connectRequest(*client, protocol, validRequest);
            waitFor(
                [&] {
                    return protocolCounts.requested == requests + i + 1;
                },
                "allowed concurrent transfer reaches provider");
            clients.push_back(std::move(client));
        }
        require(protocolCounts.replies == 4, "four transfers can remain pending together");
        QTcpSocket excess;
        connectRequest(excess, protocol, validRequest);
        require(response(excess).isEmpty(), "excess connection closed before resource transfer");
        require(protocolCounts.requested == requests + 4, "excess connection does not invoke provider");
        for (const auto &client : clients)
            client->abort();
        waitFor(
            [&] {
                return protocolCounts.replies == 0;
            },
            "disconnected concurrent transfers release all replies");
        QTcpSocket recovered;
        connectRequest(recovered, protocol, validRequest);
        waitFor(
            [&] {
                return bool(provider->latest);
            },
            "released transfer slots accept another request");
        provider->latest->append("recovered");
        provider->latest->complete(9);
        require(body(response(recovered)) == "recovered", "server remains usable after limits and cancellation");
    }
    qInfo(
        "PASS: provider lifetime, authentication, request validation, streamed replies, failures, image probes and cancellation");
}
