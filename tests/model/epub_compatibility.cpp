#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include "publication/image_policy.h"
#include "lvimg.h"
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

void checkDtbookChapterWindow()
{
    auto files = test::engineBook(false, 2, 25);
    files["OEBPS/book.opf"].replace(R"(href="c2.xhtml" media-type="application/xhtml+xml")",
                                    R"(href="chapter.xml" media-type="application/x-dtbook+xml")");
    files["OEBPS/toc.ncx"].replace("c2.xhtml", "chapter.xml#harbour");
    files.remove("OEBPS/c2.xhtml");
    files["OEBPS/chapter.xml"] =
            "<dtbook xmlns='http://www.daisy.org/z3986/2005/dtbook/' version='2005-3' xml:lang='en'>"
            "<head/><book><bodymatter><level><hd>Estuary</hd><p>Water.</p>"
            "<level><hd>Harbour</hd><p id='harbour'>The cormorant returns.</p></level>"
            "</level></bodymatter></book></dtbook>";
    EngineDocument model(test::readerFonts());
    openPublication(model, files);
    const auto entries = model.chapters();
    require(entries.size() == 2 && model.goTo(entries.last().position),
            "Missing NCX destination in nested DTBook section");
    const auto expected = model.position();
    QString error;
    require(model.spineItemOf(expected) == 1 && model.showSpineItems(0, 0, &error),
            qPrintable(error));
    require(model.goTo(entries.last().position) && model.position() == expected,
            "DTBook fragment changed across chapter windows");
    require(model.searchText("cormorant", {}, 2).hits.size() == 1,
            "Text in nested DTBook section was lost");
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

class Pixels : public LVImageDecoderCallback
{
public:
    int rows = 0;
    bool reduced = false, offsetFrame = false;

    bool GetRasterTargetSize(int &width, int &height) const override
    {
        width = height = 5;
        return reduced;
    }

    void OnStartDecode(LVImageSource *) override { }

    bool OnLineDecoded(LVImageSource *, int, lUInt32 *row) override
    {
        if (offsetFrame)
            require(row[0] == 0xff000000 && row[2] == 0xde0c08,
                    "Wrong first-frame offset or alpha");
        else
            require(row[0] == 0xde0c08, "Unexpected WebP pixel");
        ++rows;
        return true;
    }

    void OnEndDecode(LVImageSource *, bool errors) override
    {
        require(!errors, "WebP decode failed");
    }
};

void checkCodecs()
{
    // A solid 20 x 20 red image and a two-glyph TrueType font with a triangular A.
    auto image = QByteArray::fromBase64("UklGRh4AAABXRUJQVlA4TBEAAAAvE8AEAAdQhnoXof+BiOh/AAA=");
    require(inspectImageHeader(image).size == QSize(20, 20), "Missing WebP layout dimensions");
    auto source =
            LVCreateStreamImageSource(LVCreateMemoryStream(image.data(), image.size(), false));
    Pixels pixels;
    require(!source.isNull() && source->Decode(&pixels) && pixels.rows == 20,
            "WebP pixels missing");

    Pixels thumbnail;
    thumbnail.reduced = true;
    require(source->Decode(&thumbnail) && thumbnail.rows == 5 && source->GetWidth() == 20,
            "WebP thumbnail allocated the original raster or changed layout dimensions");

    auto animated = QByteArray::fromBase64(
            "UklGRlYAAABXRUJQVlA4WAoAAAACAAAAHQAAEwAAQU5JTQYAAAAAAAAAAABBTk1GKgAAAAEAAAAAABMAABMAAGQAAABWUDhMEQAAAC8TwAQAB1CGeheh/4GI6H8AAA==");
    auto firstFrame = LVCreateStreamImageSource(
            LVCreateMemoryStream(animated.data(), animated.size(), false));
    Pixels canvas;
    canvas.offsetFrame = true;
    require(!firstFrame.isNull() && firstFrame->Decode(&canvas) && canvas.rows == 20,
            "Animated WebP first frame did not render");
    auto oversized = animated;
    oversized[24] = oversized[25] = char(0xff);
    require(inspectImageHeader(oversized).action == ImageHeader::Replace,
            "Oversized WebP was admitted");

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
    EngineDocument book(test::readerFonts());
    require(book.open(bytes, { 600, 800 }, {}, &error), qPrintable(error));
    require(book.searchText("kingfisher", {}, 2).hits.size() == 1, "DTBook paragraph lost");
    const auto links = book.chapters();
    require(links.size() == 1 && links.first().title == "The passage"
                    && book.goTo(links.first().position),
            "DTBook NCX anchor lost");
    checkLargeArchiveOffsets(bytes);
    auto broken = bytes;
    broken[broken.size() - 22 - 20 + 8] = char(0xff);
    require(!publication.inspect(broken, &error), "Invalid ZIP64 locator admitted");
    checkDtbookChapterWindow();
    checkCodecs();
    checkNavigation(false);
    return 0;
}
