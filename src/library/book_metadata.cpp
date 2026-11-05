#include "book_metadata.h"
#include "publication/publication.h"
#include "publication/zip_resource.h"
#include <QBuffer>
#include <QDir>
#include <QImageReader>
#include <QUrl>
#include <QXmlStreamReader>

namespace kobalt::library {
namespace {
constexpr qint64 xmlLimit = 2 * 1024 * 1024, imageLimit = 8 * 1024 * 1024;

QByteArray member(QIODevice &source, const Publication &publication, const QString &name,
                  qint64 limit)
{
    if (publication.sizes().value(name, -1) < 0 || publication.sizes().value(name) > limit)
        return {};
    ZipResource stream;
    if (!stream.open(source, name, limit))
        return {};
    QByteArray bytes;
    while (stream.valid() && !stream.finished())
        bytes += stream.next();
    return stream.valid() && stream.finished() ? bytes : QByteArray();
}

// A package path relative to the package document, without escaping the archive.
QString resolve(const QString &package, const QString &href)
{
    const auto decoded = QUrl::fromPercentEncoding(href.section('#', 0, 0).toUtf8());
    if (decoded.isEmpty() || decoded.contains(':'))
        return {};
    const auto base = package.contains('/') ? package.section('/', 0, -2) + '/' : QString();
    const auto path = QDir::cleanPath(base + decoded);
    return path.startsWith("../") || path == ".." || path.startsWith('/') ? QString() : path;
}

QString clean(QString text)
{
    return text.simplified().left(1024);
}

struct Package
{
    QString title, author, coverHref, series;
    double seriesIndex = 0;
};

double position(const QString &text)
{
    bool ok = false;
    const double value = text.trimmed().toDouble(&ok);
    return ok && value > 0 && value < 1e6 ? value : 0;
}

Package readPackage(const QByteArray &bytes)
{
    Package result;
    QXmlStreamReader xml(bytes);
    QString coverId, calibreSeries, calibreIndex;
    // EPUB 3 collections: their names, types and positions by id.
    QMap<QString, QString> collections, collectionTypes, positions;
    QStringList collectionOrder;
    QMap<QString, QString> hrefs;
    while (!xml.atEnd()) {
        xml.readNext();
        // Entity expansion can multiply a small document; refuse it.
        if (xml.tokenType() == QXmlStreamReader::DTD
            || xml.tokenType() == QXmlStreamReader::EntityReference)
            return {};
        if (!xml.isStartElement())
            continue;
        const auto name = xml.name().toString();
        const auto attributes = xml.attributes();
        if (name == "title"
            && xml.namespaceUri() == QStringLiteral("http://purl.org/dc/elements/1.1/")) {
            const auto text = clean(xml.readElementText(QXmlStreamReader::IncludeChildElements));
            if (result.title.isEmpty())
                result.title = text;
        } else if (name == "creator"
                   && xml.namespaceUri() == QStringLiteral("http://purl.org/dc/elements/1.1/")) {
            const auto text = clean(xml.readElementText(QXmlStreamReader::IncludeChildElements));
            if (!text.isEmpty())
                result.author = result.author.isEmpty() ? text : result.author + ", " + text;
        } else if (name == "meta" && attributes.value("name") == QStringLiteral("cover")) {
            coverId = attributes.value("content").toString();
        } else if (name == "meta" && attributes.value("name") == QStringLiteral("calibre:series")) {
            calibreSeries = clean(attributes.value("content").toString());
        } else if (name == "meta"
                   && attributes.value("name") == QStringLiteral("calibre:series_index")) {
            calibreIndex = attributes.value("content").toString();
        } else if (name == "meta"
                   && attributes.value("property") == QStringLiteral("belongs-to-collection")) {
            const auto id = attributes.value("id").toString();
            const auto text = clean(xml.readElementText(QXmlStreamReader::IncludeChildElements));
            if (!id.isEmpty() && !text.isEmpty()) {
                collections.insert(id, text);
                collectionOrder.append(id);
            }
        } else if (name == "meta" && attributes.value("refines").startsWith(QLatin1Char('#'))) {
            const auto id = attributes.value("refines").mid(1).toString();
            const auto property = attributes.value("property");
            if (property == QStringLiteral("collection-type"))
                collectionTypes.insert(
                        id, xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed());
            else if (property == QStringLiteral("group-position"))
                positions.insert(id, xml.readElementText(QXmlStreamReader::IncludeChildElements));
        } else if (name == "item") {
            const auto href = attributes.value("href").toString();
            hrefs.insert(attributes.value("id").toString(), href);
            // EPUB 3 marks the cover in the manifest itself.
            if (attributes.value("properties").toString().split(' ').contains("cover-image"))
                result.coverHref = href;
        }
    }
    if (xml.hasError())
        return {};
    // A collection marked as a set is not a series.
    for (const auto &id : collectionOrder)
        if (collectionTypes.value(id, QStringLiteral("series")) == QStringLiteral("series")) {
            result.series = collections.value(id);
            result.seriesIndex = position(positions.value(id));
            break;
        }
    if (result.series.isEmpty() && !calibreSeries.isEmpty()) {
        result.series = calibreSeries;
        result.seriesIndex = position(calibreIndex);
    }
    if (result.coverHref.isEmpty() && !coverId.isEmpty())
        result.coverHref = hrefs.value(coverId);
    if (result.coverHref.isEmpty())
        for (auto it = hrefs.begin(); it != hrefs.end(); ++it)
            if (it.key().compare("cover", Qt::CaseInsensitive) == 0
                || it.key().compare("cover-image", Qt::CaseInsensitive) == 0)
                result.coverHref = it.value();
    return result;
}

QString packagePath(const QByteArray &container)
{
    QXmlStreamReader xml(container);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.tokenType() == QXmlStreamReader::DTD
            || xml.tokenType() == QXmlStreamReader::EntityReference)
            return {};
        if (xml.isStartElement() && xml.name() == QStringLiteral("rootfile"))
            return xml.attributes().value("full-path").toString();
    }
    return {};
}

QImage decodeCover(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto size = reader.size();
    // Refuse images that would need a very large decode buffer.
    if (!size.isValid() || size.width() > 8192 || size.height() > 8192 || size.isEmpty())
        return {};
    if (size.width() > coverSize.width() || size.height() > coverSize.height())
        reader.setScaledSize(size.scaled(coverSize, Qt::KeepAspectRatio));
    auto image = reader.read();
    return image.isNull() ? QImage() : image.convertToFormat(QImage::Format_RGB32);
}
} // namespace

BookMetadata readBookMetadata(QIODevice &source)
{
    BookMetadata metadata;
    Publication publication;
    QString error;
    if (!publication.inspect(source, &error) || publication.protectedBook()) {
        if (publication.protectedBook())
            metadata.result = BookMetadata::Result::Protected;
        return metadata;
    }
    const auto package =
            packagePath(member(source, publication, "META-INF/container.xml", xmlLimit));
    if (package.isEmpty())
        return metadata;
    const auto document = member(source, publication, package, xmlLimit);
    if (document.isEmpty())
        return metadata;
    const auto parsed = readPackage(document);
    metadata.result = BookMetadata::Result::Ok;
    metadata.title = parsed.title;
    metadata.author = parsed.author;
    metadata.series = parsed.series;
    metadata.seriesIndex = parsed.seriesIndex;
    const auto cover = resolve(package, parsed.coverHref);
    if (!cover.isEmpty())
        metadata.cover = decodeCover(member(source, publication, cover, imageLimit));
    return metadata;
}
} // namespace kobalt::library
