#include "support/strings.h"
#include "publication.h"
#include "zip_resource.h"
#include <QXmlStreamReader>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QSet>
#include <QBuffer>
#include <QUrl>
#include <QtEndian>
#include <zlib.h>

namespace kobalt {
namespace {
bool fail(QString *error, const QString &message) {
    *error = message;
    return false;
}
bool inspectionFailure(QString *error, OpeningResult *result, OpeningFailureKind kind, const char *reason) {
    if (result)
        *result = OpeningResult::failed(kind, reason);
    if (error)
        *error = strings::get(reason); // Compatibility for existing archive callers.
    return false;
}
void u16(QByteArray &out, quint16 n) {
    const auto v = qToLittleEndian(n);
    out.append(reinterpret_cast<const char *>(&v), 2);
}
void u32(QByteArray &out, quint32 n) {
    const auto v = qToLittleEndian(n);
    out.append(reinterpret_cast<const char *>(&v), 4);
}
bool safeName(const QString &name) {
    if (name.isEmpty() || name.size() > 4096 || name.contains(QChar(0)) || name.contains('\\') ||
        name.startsWith('/') || name.contains(':'))
        return false;
    for (const auto &part : name.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    // These prefixes belong to Nickel's other HTTP handlers.
    return !name.startsWith("TENG/") && !name.startsWith("FONT_ROUTE/") && !name.startsWith("EPUBPATH");
}
// Font obfuscation is an EPUB embedding convention, not content encryption.
// CREngine decodes IDPF and Adobe fonts. Admit only metadata for those paths.
bool supportedFontMetadata(QIODevice &source, const QMap<QString, qint64> &sizes) {
    constexpr qint64 limit = 2 * 1024 * 1024;
    auto read = [&](const QString &name) -> QByteArray {
        ZipResource stream;
        if (sizes.value(name, -1) < 0 || sizes.value(name) > limit || !stream.open(source, name, limit))
            return {};
        QByteArray bytes;
        while (stream.valid() && !stream.finished())
            bytes += stream.next();
        return stream.valid() && stream.finished() ? bytes : QByteArray();
    };
    auto cleanXml = [](QXmlStreamReader &xml) {
        return xml.tokenType() != QXmlStreamReader::DTD && xml.tokenType() != QXmlStreamReader::EntityReference;
    };
    QXmlStreamReader descriptor(read("META-INF/container.xml"));
    QString packagePath;
    int rootfiles = 0, descriptorDepth = 0;
    while (!descriptor.atEnd()) {
        descriptor.readNext();
        if (!cleanXml(descriptor))
            return false;
        if (descriptor.isEndElement()) {
            --descriptorDepth;
            continue;
        }
        if (!descriptor.isStartElement())
            continue;
        ++descriptorDepth;
        if (descriptor.namespaceUri() != QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:container") ||
            (descriptorDepth == 1 && descriptor.name() != QStringLiteral("container")) ||
            (descriptorDepth == 2 && descriptor.name() != QStringLiteral("rootfiles")))
            return false;
        if (descriptorDepth == 3 && descriptor.name() == QStringLiteral("rootfile")) {
            if (++rootfiles != 1)
                return false;
            packagePath = descriptor.attributes().value("full-path").toString();
            if (descriptor.attributes().value("media-type") != QStringLiteral("application/oebps-package+xml"))
                return false;
        }
    }
    if (descriptor.hasError() || !safeName(packagePath))
        return false;
    QXmlStreamReader package(read(packagePath));
    QString identifierId, identifier;
    bool hasAdobeIdentifier = false;
    const QRegularExpression adobeIdentifier(
        "^(?:urn:uuid:)?(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$");
    int matchingIdentifiers = 0, depth = 0, metadataDepth = -1, manifestDepth = -1;
    QSet<QString> fonts, ids, paths;
    const QSet<QString> fontTypes = {"application/vnd.ms-opentype",
                                     "application/x-font-ttf",
                                     "application/x-font-opentype",
                                     "application/font-sfnt",
                                     "font/otf",
                                     "font/ttf"};
    while (!package.atEnd()) {
        package.readNext();
        if (!cleanXml(package))
            return false;
        if (package.isEndElement()) {
            if (depth == metadataDepth)
                metadataDepth = -1;
            if (depth == manifestDepth)
                manifestDepth = -1;
            --depth;
            continue;
        }
        if (!package.isStartElement())
            continue;
        ++depth;
        if (depth == 1) {
            if (package.name() != QStringLiteral("package") ||
                package.namespaceUri() != QStringLiteral("http://www.idpf.org/2007/opf"))
                return false;
            identifierId = package.attributes().value("unique-identifier").toString();
        } else if (depth == 2 && package.namespaceUri() == QStringLiteral("http://www.idpf.org/2007/opf") &&
                   package.name() == QStringLiteral("metadata"))
            metadataDepth = depth;
        else if (depth == 2 && package.namespaceUri() == QStringLiteral("http://www.idpf.org/2007/opf") &&
                 package.name() == QStringLiteral("manifest"))
            manifestDepth = depth;
        else if (depth == metadataDepth + 1 && metadataDepth == 2 && package.name() == QStringLiteral("identifier") &&
                 package.namespaceUri() == QStringLiteral("http://purl.org/dc/elements/1.1/")) {
            const bool unique = package.attributes().value("id") == identifierId && !identifierId.isEmpty();
            const auto text = package.readElementText();
            if (unique) {
                ++matchingIdentifiers;
                identifier = text;
            }
            // Adobe uses a UUID identifier, which can differ from the IDPF
            // unique identifier. CREngine checks every metadata identifier.
            hasAdobeIdentifier |= adobeIdentifier.match(text.trimmed()).hasMatch();
            --depth;
        } else if (depth == manifestDepth + 1 && manifestDepth == 2 &&
                   package.namespaceUri() == QStringLiteral("http://www.idpf.org/2007/opf") &&
                   package.name() == QStringLiteral("item")) {
            const auto attributes = package.attributes();
            const auto id = attributes.value("id").toString();
            const auto href = attributes.value("href").toString();
            const QUrl url(href);
            if (id.isEmpty() || ids.contains(id) || href.isEmpty() || !url.isValid() || !url.scheme().isEmpty() ||
                !url.authority().isEmpty() || url.hasQuery() || url.hasFragment() || href.contains('%') ||
                href.contains('\\') || href.startsWith('/'))
                return false;
            const auto path = QDir::cleanPath(QFileInfo(packagePath).path() + '/' + href);
            if (!safeName(path) || paths.contains(path))
                return false;
            ids.insert(id);
            paths.insert(path);
            if (fontTypes.contains(attributes.value("media-type").toString()) && sizes.contains(path))
                fonts.insert(path);
        }
    }
    identifier.remove(QRegularExpression("[ \t\r\n]"));
    if (package.hasError() || matchingIdentifiers != 1 || identifier.isEmpty())
        return false;
    const auto metadata = read("META-INF/encryption.xml");
    QXmlStreamReader check(metadata);
    while (!check.atEnd()) {
        check.readNext();
        if (!cleanXml(check))
            return false;
    }
    if (check.hasError())
        return false;
    QXmlStreamReader xml(metadata);
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("encryption") ||
        xml.namespaceUri() != QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:container"))
        return false;
    QSet<QString> targets;
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("EncryptedData") ||
            xml.namespaceUri() != QStringLiteral("http://www.w3.org/2001/04/xmlenc#"))
            return false;
        QString algorithm, target;
        int methods = 0, cipherData = 0, references = 0;
        while (xml.readNextStartElement()) {
            if (xml.namespaceUri() != QStringLiteral("http://www.w3.org/2001/04/xmlenc#"))
                return false;
            if (xml.name() == QStringLiteral("EncryptionMethod")) {
                if (++methods != 1)
                    return false;
                algorithm = xml.attributes().value("Algorithm").toString();
                if (xml.readNextStartElement())
                    return false;
            } else if (xml.name() == QStringLiteral("CipherData")) {
                if (++cipherData != 1)
                    return false;
                while (xml.readNextStartElement()) {
                    if (xml.name() != QStringLiteral("CipherReference") ||
                        xml.namespaceUri() != QStringLiteral("http://www.w3.org/2001/04/xmlenc#") || ++references != 1)
                        return false;
                    target = xml.attributes().value("URI").toString();
                    if (xml.readNextStartElement())
                        return false;
                }
            } else
                return false;
        }
        const bool supported = algorithm == QStringLiteral("http://www.idpf.org/2008/embedding") ||
                               (algorithm == QStringLiteral("http://ns.adobe.com/pdf/enc#RC") && hasAdobeIdentifier);
        if (methods != 1 || cipherData != 1 || references != 1 || !supported || !safeName(target) ||
            target.contains('%') || !fonts.contains(target) || targets.contains(target))
            return false;
        targets.insert(target);
    }
    while (!xml.atEnd()) {
        xml.readNext();
        if (!cleanXml(xml))
            return false;
    }
    return !xml.hasError() && !targets.isEmpty();
}
}
bool Publication::inspect(const QByteArray &source, QString *error, OpeningResult *result) {
    if (result)
        *result = {};
    if (source.size() > maxBytes) {
        *this = {};
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_size"));
    }
    QBuffer buffer;
    buffer.setData(source);
    buffer.open(QIODevice::ReadOnly);
    return inspect(buffer, error, result);
}
bool Publication::inspect(QIODevice &source, QString *error, OpeningResult *result) {
    if (result)
        *result = {};
    names_.clear();
    sizes_.clear();
    archive_.clear();
    directory_.clear();
    fingerprint_.clear();
    received_ = 0;
    protected_ = false;
    const qint64 length = source.size();
    if (source.isSequential() || length < 22 || length > 1024LL * 1024 * 1024)
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_size"));
    const auto tailStart = qMax(qint64(0), length - 65557);
    if (!source.seek(tailStart))
        return inspectionFailure(error, result, OpeningFailureKind::Io,
                                 strings::key("error.cannot_read_publication_directory"));
    const auto tail = source.read(length - tailStart);
    if (tail.size() != length - tailStart)
        return inspectionFailure(error, result, OpeningFailureKind::Io,
                                 strings::key("error.incomplete_publication_directory"));
    auto tail16 = [&](int p) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(tail.constData() + p));
    };
    auto tail32 = [&](int p) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(tail.constData() + p));
    };
    int end = -1;
    for (int p = tail.size() - 22; p >= 0; --p)
        if (tail32(p) == 0x06054b50 && p + 22 + tail16(p + 20) == tail.size()) {
            end = p;
            break;
        }
    if (end < 0 || tail16(end + 4) || tail16(end + 6) || tail16(end + 8) != tail16(end + 10))
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_directory"));
    const int count = tail16(end + 10);
    const quint32 offset = tail32(end + 16), directorySize = tail32(end + 12);
    if (!count || count > 4096 || directorySize > 4 * 1024 * 1024 ||
        quint64(offset) + directorySize != quint64(tailStart + end))
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_directory_size"));
    if (!source.seek(offset))
        return inspectionFailure(error, result, OpeningFailureKind::Io,
                                 strings::key("error.unsupported_publication_directory_size"));
    const auto directory = source.read(directorySize);
    if (directory.size() != qint64(directorySize))
        return inspectionFailure(error, result, OpeningFailureKind::Io,
                                 strings::key("error.incomplete_publication_directory"));
    auto read16 = [&](int p) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(directory.constData() + p));
    };
    auto read32 = [&](int p) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(directory.constData() + p));
    };
    int at = 0;
    QSet<QString> seen;
    quint64 expanded = 0;
    bool unsupportedMetadata = false;
    for (int i = 0; i < count; ++i) {
        if (at > int(directorySize) - 46 || read32(at) != 0x02014b50)
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.invalid_publication_directory"));
        const int nameLength = read16(at + 28);
        const quint64 next = quint64(at) + 46 + nameLength + read16(at + 30) + read16(at + 32);
        if (next > directorySize || read16(at + 34) || read32(at + 20) == 0xffffffffU ||
            read32(at + 24) == 0xffffffffU || read32(at + 42) >= offset)
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.unsupported_publication_member"));
        if (read16(at + 8) & 0x41) {
            protected_ = true;
            return inspectionFailure(error, result, OpeningFailureKind::Protected,
                                     strings::key("error.unsupported_publication_member"));
        }
        const auto encoded = directory.mid(at + 46, nameLength);
        auto name = QString::fromUtf8(encoded);
        if (name.toUtf8() != encoded)
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.publication_member_name_is_not_utf_8"));
        const bool isDirectory = name.endsWith('/');
        if (isDirectory)
            name.chop(1);
        if (!safeName(name) || seen.contains(name))
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.ambiguous_publication_member_path"));
        seen.insert(name);
        expanded += read32(at + 24);
        if (expanded > 4ULL * 1024 * 1024 * 1024)
            return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                     strings::key("error.expanded_publication_is_too_large"));
        // Kobalt does not open LCP-protected books. Their encryption metadata
        // would reject them below, but that check is about fonts and may change.
        if (name == "META-INF/license.lcpl") {
            protected_ = true;
            return inspectionFailure(error, result, OpeningFailureKind::Protected,
                                     strings::key("error.this_publication_is_lcp_protected"));
        }
        // Native content records remain Nickel's responsibility. Font and EPUB
        // encryption metadata need a separate, explicitly supported path.
        if (name == "META-INF/encryption.xml")
            unsupportedMetadata = true;
        if (!isDirectory) {
            names_.append(name);
            sizes_[name] = read32(at + 24);
        }
        at = int(next);
    }
    if (at != int(directorySize) || !names_.contains("META-INF/container.xml") || !names_.contains("mimetype"))
        return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                 strings::key("error.missing_publication_container"));
    // Scan the whole directory before admitting supported font metadata, so an
    // LCP license or encrypted member is rejected regardless of entry order.
    if (unsupportedMetadata && !supportedFontMetadata(source, sizes_))
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.this_publication_needs_unsupported_font_or_resource_handling"));
    names_.removeOne("mimetype");
    names_.prepend("mimetype");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray size;
    u32(size, quint32(length));
    u32(size, quint32(quint64(length) >> 32));
    hash.addData(size);
    hash.addData(directory);
    hash.addData(tail.mid(end));
    fingerprint_ = hash.result().toHex();
    return true;
}
bool Publication::append(const QString &name, const QByteArray &data, QString *error) {
    if (received_ >= names_.size() || name != names_[received_])
        return fail(error, strings::get("error.publication_session_resource_mismatch"));
    if (data.size() > maxResource || (name == "mimetype" && data != "application/epub+zip"))
        return fail(error, strings::get("error.invalid_publication_resource_response"));
    const auto encoded = name.toUtf8();
    if (quint64(archive_.size()) + directory_.size() + data.size() + encoded.size() * 2 + 98 > maxBytes)
        return fail(error, strings::get("error.publication_exceeds_the_memory_limit"));
    const quint32 offset = archive_.size(),
                  crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()), data.size());
    u32(archive_, 0x04034b50);
    u16(archive_, 20);
    u16(archive_, 0x800);
    u16(archive_, 0);
    u16(archive_, 0);
    u16(archive_, 0);
    u32(archive_, crc);
    u32(archive_, data.size());
    u32(archive_, data.size());
    u16(archive_, encoded.size());
    u16(archive_, 0);
    archive_.append(encoded);
    archive_.append(data);
    u32(directory_, 0x02014b50);
    u16(directory_, 20);
    u16(directory_, 20);
    u16(directory_, 0x800);
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u32(directory_, crc);
    u32(directory_, data.size());
    u32(directory_, data.size());
    u16(directory_, encoded.size());
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u32(directory_, 0);
    u32(directory_, offset);
    directory_.append(encoded);
    ++received_;
    return true;
}
QByteArray Publication::finish(QString *error) {
    if (names_.isEmpty() || received_ != names_.size()) {
        fail(error, strings::get("error.publication_transfer_is_incomplete"));
        return {};
    }
    const quint32 offset = archive_.size(), length = directory_.size();
    archive_.append(directory_);
    directory_.clear();
    u32(archive_, 0x06054b50);
    u16(archive_, 0);
    u16(archive_, 0);
    u16(archive_, received_);
    u16(archive_, received_);
    u32(archive_, length);
    u32(archive_, offset);
    u16(archive_, 0);
    names_.clear();
    received_ = 0;
    return std::move(archive_);
}
}
