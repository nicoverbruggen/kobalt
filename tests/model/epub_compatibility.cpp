#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QtEndian>
#include "publication/zip_resource.h"
#include <QDir>
#include "publication/publication.h"

using namespace kobalt;

namespace {
void openPublication(EngineDocument &model, const test::BookFiles &files)
{
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto source = std::make_shared<ResourceCache>(
            files.keys(),
            [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false), ResourceCache::Statistics{},
            [](const QString &name) {
                return name.endsWith("label.png") ? QSize(128, 64) : QSize();
            });
    QString error;
    require(model.openResources(source, sizes, { 600, 800 }, {}, &error), qPrintable(error));
}

void checkNavigation(bool epub3)
{
    auto files = test::engineBook(true, 3, 12);
    auto &opf = files["OEBPS/book.opf"];
    opf.replace(
            "</manifest>",
            "<item id='foreign' href='source.bin' media-type='application/octet-stream' fallback='via'/>"
            "<item id='via' href='middle.bin' media-type='application/octet-stream' fallback='c3'/></manifest>");
    opf.replace("idref=\"c3\"", "idref=\"foreign\"");
    // The same XHTML also appears in its own spine slot. Both anchors must survive.
    opf.replace("</spine>", "<itemref idref='c3'/></spine>");
    files["OEBPS/source.bin"] = "foreign";
    files["OEBPS/middle.bin"] = "middle";
    EngineDocument model(test::readerFonts());
    openPublication(model, files);
    QString error;
    require(model.spineItems().value(2) == "OEBPS/c3.xhtml", "Manifest fallback did not resolve");
    const auto names = model.spineLocationNames();
    require(names.value(2) == "OEBPS/source.bin" && names.value(3) == "OEBPS/c3.xhtml",
            "Shared fallback lost its original spine identity");
    for (int slot : { 2, 3 }) {
        Location start;
        start.href = names[slot];
        const auto position = model.positionOf(start);
        Location restored;
        require(model.spineItemOf(position) == slot && model.locationOf(position, &restored)
                        && restored == start,
                "Shared fallback location did not round trip");
    }
    // Direct spine links must keep their own slot when a later item falls back to it.
    if (!epub3) {
        auto reverse = files;
        reverse["OEBPS/book.opf"].replace("idref='c3'", "idref='foreign'");
        reverse["OEBPS/book.opf"].replace("idref=\"foreign\"", "idref=\"c3\"");
        EngineDocument direct(test::readerFonts());
        openPublication(direct, reverse);
        require(direct.goTo(direct.chapters().last().position)
                        && direct.spineItemOf(direct.position()) == 2,
                "A later fallback took over a direct NCX destination");
        auto middle = files;
        middle["OEBPS/book.opf"].replace("idref='c3'", "idref='via'");
        middle["OEBPS/toc.ncx"].replace("c3.xhtml", "middle.bin");
        EngineDocument intermediate(test::readerFonts());
        openPublication(intermediate, middle);
        require(intermediate.goTo(intermediate.chapters().last().position)
                        && intermediate.spineItemOf(intermediate.position()) == 3,
                "A fallback chain took over an intermediate item's own spine slot");
    }
}

// Move the central directory and all resources after mimetype past the ZIP32
// offset limit. A sparse file gives the reader real 64-bit seeks without a large fixture.
void checkLargeArchiveOffsets(QByteArray bytes)
{
    const auto read32 = [&](int p) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + p));
    };
    const auto read64 = [&](int p) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(bytes.constData() + p));
    };
    const auto set64 = [&](int p, quint64 value) {
        qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + p));
    };
    const auto set32 = [&](int p, quint32 value) {
        qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + p));
    };
    const int eocd = bytes.size() - 22, locator = eocd - 20, record = int(read64(locator + 8));
    const int directory = int(read64(record + 48));
    const quint64 gap = (quint64(1) << 32) + 7;
    int first = bytes.indexOf("PK\x03\x04", 4);
    require(first > 0, "Missing second ZIP fixture member");
    for (int p = directory; p < record;) {
        const auto read16 = [&](int offset) {
            return qFromLittleEndian<quint16>(
                    reinterpret_cast<const uchar *>(bytes.constData() + offset));
        };
        const int extra = p + 46 + read16(p + 28);
        if (read32(p + 42) == 0xffffffff) {
            require(read16(extra) == 1, "Fixture must place the ZIP64 field first");
            const int at = extra + 4 + 8 * (read32(p + 24) == 0xffffffff)
                    + 8 * (read32(p + 20) == 0xffffffff);
            set64(at, read64(at) + gap);
        } else {
            require(read32(p + 42) == 0, "Only mimetype may use a small offset in this fixture");
        }
        p = extra + read16(p + 30) + read16(p + 32);
    }
    set64(record + 48, gap + directory);
    set64(locator + 8, gap + record);
    set32(eocd + 16, 0xffffffff);
    QTemporaryFile large;
    require(large.open() && large.write(bytes.constData(), first) == first
                    && large.seek(gap + first)
                    && large.write(bytes.constData() + first, bytes.size() - first)
                            == bytes.size() - first
                    && large.flush(),
            "Could not create sparse ZIP64 fixture");
    QString error;
    Publication publication;
    require(publication.inspect(large, &error), qPrintable(error));
    ZipResource chapter;
    require(chapter.open(large, "chapter.xml") && chapter.next().contains("kingfisher")
                    && chapter.valid() && chapter.finished(),
            "ZIP64 local offset was truncated");
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // This independent ZIP writer fixture has ZIP64 member sizes, offsets, end record and locator.
    QFile fixture(
            QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("../fixtures/zip64-dtbook.epub"));
    require(fixture.open(QIODevice::ReadOnly), "Missing ZIP64 fixture");
    const auto bytes = fixture.readAll();
    QString error;
    Publication publication;
    require(publication.inspect(bytes, &error) && publication.zip64(), qPrintable(error));
    checkLargeArchiveOffsets(bytes);
    auto broken = bytes;
    broken[broken.size() - 22 - 20 + 8] = char(0xff);
    require(!publication.inspect(broken, &error), "Invalid ZIP64 locator admitted");
    checkNavigation(false);
    return 0;
}
