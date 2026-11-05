#include "plain_archive.h"
#include "publication/layout_compatibility.h"
#include "publication/resource_cache.h"
#include "publication/publication.h"
#include "platforms/kobo/common/local_book_path.h"
#include "check.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QtEndian>
#include <zlib.h>
using namespace kobalt;

namespace {
void u16(QByteArray &bytes, quint16 value)
{
    value = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&value), 2);
}

void u32(QByteArray &bytes, quint32 value)
{
    value = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&value), 4);
}

QByteArray zip(const QMap<QString, QByteArray> &files, bool badCrc = false)
{
    QByteArray bytes, directory;
    QList<QPair<QByteArray, QByteArray>> entries;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        entries.append({ it.key().toUtf8(), it.value() });
    for (const auto &entry : entries) {
        const auto offset = bytes.size();
        const auto crc = crc32(0, reinterpret_cast<const Bytef *>(entry.second.constData()),
                               entry.second.size())
                ^ (badCrc && entry.first == "later.xhtml" ? 1U : 0U);
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
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    const auto path = dir.path() + "/12345678-1234-1234-1234-123456789abc";
    QMap<QString, QByteArray> files = {
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container><rootfiles><rootfile full-path='book.opf'/></rootfiles></container>" },
        { "book.opf",
          "<package><manifest><item id='first' href='first.xhtml' media-type='application/xhtml+xml'/><item id='later' href='later.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='first'/><itemref idref='later'/></spine></package>" },
        { "first.xhtml", "<html><body><p>Plain first chapter.</p></body></html>" },
        { "later.xhtml", "<html><body><p>Plain later chapter.</p></body></html>" }
    };
    for (const auto &relative : { QStringLiteral(".sync/.books/.hidden.epub"),
                                  QStringLiteral(".kobo/dropbox/.nested/book.epub"),
                                  QStringLiteral(".kobo/google_drive/book.epub"),
                                  QStringLiteral(".adds/kobocloud/books/book.epub"),
                                  QStringLiteral("third-party-sync/books/book#part?%20.epub"),
                                  QStringLiteral("third-party-sync/books/extensionless") }) {
        const auto bookPath = dir.path() + '/' + relative;
        require(QDir().mkpath(QFileInfo(bookPath).absolutePath()), "create sync folder");
        QFile book(bookPath);
        const auto bytes = zip(files);
        require(book.open(QIODevice::WriteOnly) && book.write(bytes) == bytes.size(),
                "write sync EPUB");
        book.close();
        QFile source(localReaderBookPath("file://" + bookPath));
        Publication publication;
        QString error;
        require(source.open(QIODevice::ReadOnly) && publication.inspect(source, &error),
                "EPUB in a hidden or sync folder failed inspection");
        require(!publication.protectedBook(), "Unprotected sync EPUB was treated as protected");
        source.close();
        const auto linkedPath = bookPath + ".link";
        require(QFile::link(bookPath, linkedPath), "create EPUB symlink");
        QFile linked(localReaderBookPath("file://" + linkedPath));
        require(linked.open(QIODevice::ReadOnly) && publication.inspect(linked, &error),
                "Local EPUB symlink failed inspection");
        linked.close();

        auto protectedFiles = files;
        protectedFiles["META-INF/license.lcpl"] = "{}";
        const auto protectedBytes = zip(protectedFiles);
        require(book.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && book.write(protectedBytes) == protectedBytes.size(),
                "write protected sync EPUB");
        book.close();
        require(source.open(QIODevice::ReadOnly) && !publication.inspect(source, &error)
                        && publication.protectedBook(),
                "Protected EPUB in a hidden or sync folder escaped protection checks");
    }
    std::unique_ptr<native::ResourceArchive> archive;
    auto save = [&](bool badCrc = false) {
        archive.reset();
        QFile file(path);
        const auto bytes = zip(files, badCrc);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                "write fixture");
        file.close();
        // No Nickel objects or keys are needed by the default implementation.
        archive = native::openPlainArchive(path);
        require(bool(archive), "open plain archive");
    };
    auto inspect = [&] {
        QMap<QString, qint64> sizes;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            sizes[it.key()] = it.value().size();
        ResourceCache cache(
                files.keys(),
                [&](const QString &name, const ResourceCache::Sink &sink, QString *) {
                    std::unique_ptr<QNetworkReply> reply(archive->request(name));
                    if (!reply || reply->error() != QNetworkReply::NoError)
                        return false;
                    const auto bytes = reply->readAll();
                    return sink(bytes.constData(), bytes.size());
                },
                std::make_shared<std::atomic_bool>(false));
        return inspectLayout(cache, sizes).verdict;
    };
    save();
    require(inspect() == LayoutCompatibility::Supported,
            "extensionless store book must pass without Nickel");
    require(!archive->request("../first.xhtml") && !archive->request("missing.xhtml"),
            "unlisted paths rejected");
    files["later.xhtml"] = QByteArray(256, char(0x93));
    save();
    require(inspect() == LayoutCompatibility::Incomplete,
            "unreadable later chapter must cause fallback");
    files["later.xhtml"] = "<html><body>Plain again.</body></html>";
    save(true);
    std::unique_ptr<QNetworkReply> corrupt(archive->request("later.xhtml"));
    require(corrupt && corrupt->error() != QNetworkReply::NoError && corrupt->readAll().isEmpty(),
            "CRC failure exposes no partial bytes");
    require(inspect() == LayoutCompatibility::Incomplete,
            "corrupt later chapter must cause fallback");
    corrupt.reset();
    archive.reset();
    require(!native::openPlainArchive(dir.path() + "/missing.epub"), "missing archive rejected");
    qInfo("PASS: direct ZIP reading, later unreadable chapter, CRC failure, paths and provider lifetime");
}
