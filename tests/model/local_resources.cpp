#include "check.h"
#include "publication/local_resources.h"
#include "publication/publication.h"
#include "support/strings.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <thread>

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

QByteArray zip(const QMap<QString, QByteArray> &files, bool deflated = false, bool badCrc = false)
{
    QByteArray bytes, directory;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const auto name = it.key().toUtf8(), data = it.value();
        QByteArray packed = data;
        if (deflated) {
            z_stream stream{};
            require(deflateInit2(&stream, 1, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK,
                    "deflate init");
            packed.resize(compressBound(data.size()));
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
            stream.avail_in = data.size();
            stream.next_out = reinterpret_cast<Bytef *>(packed.data());
            stream.avail_out = packed.size();
            require(deflate(&stream, Z_FINISH) == Z_STREAM_END, "deflate fixture");
            packed.resize(stream.total_out);
            deflateEnd(&stream);
        }
        const quint32 offset = bytes.size();
        const auto crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()), data.size())
                ^ (badCrc && it.key() == "data.bin" ? 1U : 0U);
        u32(bytes, 0x04034b50);
        u16(bytes, 20);
        u16(bytes, 0);
        u16(bytes, deflated ? 8 : 0);
        u32(bytes, 0);
        u32(bytes, crc);
        u32(bytes, packed.size());
        u32(bytes, data.size());
        u16(bytes, name.size());
        u16(bytes, 0);
        bytes += name;
        bytes += packed;
        u32(directory, 0x02014b50);
        u16(directory, 20);
        u16(directory, 20);
        u16(directory, 0);
        u16(directory, deflated ? 8 : 0);
        u32(directory, 0);
        u32(directory, crc);
        u32(directory, packed.size());
        u32(directory, data.size());
        u16(directory, name.size());
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, offset);
        directory += name;
    }
    const quint32 offset = bytes.size();
    bytes += directory;
    u32(bytes, 0x06054b50);
    u16(bytes, 0);
    u16(bytes, 0);
    u16(bytes, files.size());
    u16(bytes, files.size());
    u32(bytes, directory.size());
    u32(bytes, offset);
    u16(bytes, 0);
    return bytes;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const auto path = directory.path() + "/book#part?name%20.kepub.epub";
    QMap<QString, QByteArray> files{ { "mimetype", "application/epub+zip" },
                                     { "META-INF/container.xml", "<container/>" },
                                     { "book.opf", "<package/>" },
                                     { "chapter.xhtml", "<html><body><p>Recoverable HTML" },
                                     { "empty.bin", {} },
                                     { "data.bin",
                                       QByteArray(3 * ResourceCache::blockBytes + 9, 'a') } };
    auto write = [&](bool compressed = false, bool badCrc = false) {
        QFile file(path);
        const auto bytes = zip(files, compressed, badCrc);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && file.write(bytes) == bytes.size(),
                "write EPUB");
    };
    auto sizes = [&] {
        QMap<QString, qint64> result;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            result[it.key()] = it.value().size();
        }
        return result;
    };
    auto rejected = std::make_shared<std::atomic_bool>(false);
    for (bool compressed : { false, true }) {
        write(compressed);
        auto source = native::openLocalPublication(path);
        require(source && source->active() && source->port() == 0,
                "Local publication created a network endpoint");
        auto lease = std::make_shared<SourceLease>();
        auto factory = source->resources(lease);
        std::thread worker([&] {
            auto cache = factory(sizes(), "sans-serif", rejected);
            require(cache->readBounded("data.bin", ResourceCache::budgetBytes) == files["data.bin"],
                    "Block read differs");
            const auto requests = cache->statistics()["requests"].toInt();
            require(cache->readBounded("data.bin", ResourceCache::budgetBytes) == files["data.bin"]
                            && cache->statistics()["requests"].toInt() == requests,
                    "Cached bytes were read again");
            require(cache->readBounded("chapter.xhtml", ResourceCache::budgetBytes)
                                    == files["chapter.xhtml"]
                            && !rejected->load(),
                    "Recoverable chapter HTML was rejected as package XML");
            require(cache->size("empty.bin") == 0, "Empty resource failed");
            lease->revoke();
            require(cache->readBounded("data.bin", ResourceCache::budgetBytes).isNull()
                            && cache->failure() == ResourceCache::Failure::Cancelled,
                    "Revocation allowed cached bytes to be read");
        });
        worker.join();
    }
    write();
    {
        auto lease = std::make_shared<SourceLease>();
        lease->cancel();
        auto cache = localResourceFactory(path, lease)(sizes(), {}, rejected);
        require(cache->size("data.bin") < 0
                        && cache->failure() == ResourceCache::Failure::Cancelled,
                "Cancelled opening read the archive");
    }
    {
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->readBounded("data.bin", 100).isNull()
                        && cache->failure() == ResourceCache::Failure::Limit,
                "Local reads bypassed inspection limits");
        require(cache->resumeAfterInspection()
                        && cache->size("data.bin") == files["data.bin"].size(),
                "A bounded inspection prevented a normal read");
        cache->releaseBytes();
        files["data.bin"].fill('b');
        write();
        require(cache->size("data.bin") < 0 && cache->failure() == ResourceCache::Failure::Changed
                        && !cache->resumeAfterInspection(),
                "Changed resource bytes were reused");
        auto renewed =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(renewed->readBounded("data.bin", ResourceCache::budgetBytes) == files["data.bin"],
                "New lease kept old bytes");
    }
    for (bool compressed : { false, true }) {
        write(compressed, true);
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->size("data.bin") < 0 && cache->failure() == ResourceCache::Failure::Transfer,
                "Invalid resource CRC was accepted");
    }
    strings::showKeys(false);
    const auto xmlError = strings::get("error.publication_package_xml_is_invalid");
    require(xmlError != QStringLiteral("error.publication_package_xml_is_invalid"),
            "Malformed package XML has no display message");
    for (const auto &xml :
         { QByteArray(), QByteArray("<package>"), QByteArray("<package><broken></package>") }) {
        files["book.opf"] = xml;
        write();
        rejected->store(false);
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->size("book.opf") < 0 && rejected->load(),
                "Malformed package XML was accepted");
        require(cache->error() == xmlError, "Malformed package XML lost its display message");
    }
    files["book.opf"] = "<package/>";
    {
        QByteArray png = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452");
        auto dimension = [&](quint32 value) {
            value = qToBigEndian(value);
            png.append(reinterpret_cast<const char *>(&value), 4);
        };
        dimension(64);
        dimension(80);
        png.resize(300 * 1024);
        files["image.png"] = png;
        write();
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->deferredImageSize("image.png") == QSize(64, 80)
                        && cache->statistics()["allocated_bytes"].toDouble() == 0,
                "Image dimension probe filled the source cache");
        const auto huge = qToBigEndian(quint32(8000));
        png.replace(16, 4, reinterpret_cast<const char *>(&huge), 4);
        png.replace(20, 4, reinterpret_cast<const char *>(&huge), 4);
        files["image.png"] = png;
        write();
        cache = localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->readBounded("image.png", ResourceCache::budgetBytes).startsWith("<svg"),
                "Oversized bitmap escaped the pixel limit");
        auto oversized = sizes();
        oversized["image.png"] = ResourceCache::budgetBytes + 1;
        cache = localResourceFactory("/missing.epub",
                                     std::make_shared<SourceLease>())(oversized, {}, rejected);
        require(cache->readBounded("image.png", ResourceCache::budgetBytes).startsWith("<svg"),
                "Oversized image source was read instead of replaced");
    }
    files["META-INF/license.lcpl"] = "{}";
    write();
    require(!native::openLocalPublication(path), "Local backend accepted a protected publication");
    std::puts(
            "PASS: local worker reads, stored and deflated CRCs, cache reuse, limits, image probes, cancellation and revocation");
}
