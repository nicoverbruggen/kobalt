#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include "engine/blaze/adapter/xml_entities.h"
#include "engine/blaze/extensions/embedded_font_cache.h"
#include "publication/image_policy.h"
#include "lvimg.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <cstring>
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
    files["OEBPS/label.png"] = files.value("OEBPS/colors.png");
    if (epub3) {
        opf.replace("version=\"2.0\"", "version=\"3.0\"");
        opf.replace(
                "</manifest>",
                "<item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/></manifest>");
        files["OEBPS/nav.xhtml"] = test::publisherHtml(
                "<nav epub:type='toc'><ol><li><a href='c1.xhtml'> <span><img alt='Map of the coast' src='label.png'/></span> </a></li>"
                "<li><span><img src='label.png' title='Coastal routes'/></span><ol><li><a href='c2.xhtml'>Passage <img alt='two' src='label.png'/></a></li></ol></li></ol></nav>"
                "<nav epub:type='landmarks z3998:frontmatter'><ol><li><a epub:type='bibliography' href='source.bin#kobo.10.1'>References</a></li></ol></nav>"
                "<nav epub:type='page-list'><ol><li><a href='source.bin#kobo.10.1'>xiv</a></li></ol></nav>");
    } else {
        files["OEBPS/toc.ncx"].replace(
                "</ncx>",
                "<pageList><pageTarget type='front' value='14'>"
                "<navLabel><text>xiv</text></navLabel><content src='source.bin#kobo.10.1'/>"
                "</pageTarget></pageList></ncx>");
    }
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
    const auto pages = model.printedPages();
    require(pages.size() == 1 && pages.first().title == "xiv", "Publisher label missing");
    require(model.goTo(pages.first().position), "Whole-book print jump failed");
    const auto expected = model.position();
    require(model.spineItemOf(expected) == 2, "Printed page jumped to the wrong chapter");
    require(model.showSpineItems(0, 0, &error), qPrintable(error));
    if (epub3) {
        const auto chapter = model.chapters().value(0);
        require(chapter.title == "Map of the coast", "Image text was lost");
        const auto preview = model.imageAt({}, chapter.labelImage);
        require(preview.size() == QSize(128, 64) && qGreen(preview.pixel(1, 1)) == 192,
                "Preview showed a placeholder instead of the artwork");
        const auto chapters = model.chapters();
        require(chapters.size() == 3 && chapters[1].title == "Coastal routes"
                        && !chapters[1].labelImage.isEmpty() && chapters[2].title == "Passage two",
                "Image-only grouping heading or mixed text label was lost");
        const auto marks = model.landmarks();
        require(marks.size() == 1 && marks.first().title == "References"
                        && model.goTo(marks.first().position) && model.position() == expected,
                "Landmark lost the destination outside the loaded chapter");
        require(model.showSpineItems(0, 0, &error), qPrintable(error));
    }
    const auto fromChapter = model.printedPages();
    require(fromChapter.size() == 1 && model.goTo(fromChapter.first().position),
            "Cross-chapter print jump failed");
    require(model.position() == expected, "Chapter mode lost the printed page anchor");
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

// Present the fixture as a seekable archive with a zero-filled gap after mimetype.
class SparseArchive : public QIODevice
{
public:
    SparseArchive(const QByteArray &bytes, qint64 first, qint64 gap)
        : bytes_(bytes), first_(first), gap_(gap)
    {
        open(ReadOnly | Unbuffered);
    }

    qint64 size() const override { return bytes_.size() + gap_; }

protected:
    qint64 readData(char *data, qint64 capacity) override
    {
        const qint64 start = pos();
        const qint64 count = qBound<qint64>(0, size() - start, capacity);
        const qint64 prefix = qBound<qint64>(0, first_ - start, count);
        const qint64 tail = qBound<qint64>(0, first_ + gap_ - start, count);
        if (prefix)
            std::memcpy(data, bytes_.constData() + start, prefix);
        if (tail > prefix)
            std::memset(data + prefix, 0, tail - prefix);
        if (tail < count)
            std::memcpy(data + tail, bytes_.constData() + (start + tail - gap_), count - tail);
        return count;
    }

    qint64 writeData(const char *, qint64) override { return -1; }

private:
    const QByteArray &bytes_;
    const qint64 first_;
    const qint64 gap_;
};

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
    SparseArchive large(bytes, first, gap);
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

    auto font = QByteArray::fromBase64(
            "d09GMgABAAAAAAFQAAoAAAAAAtQAAAEIAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAABmAANAoYNQE2AiQDBgsGAAQgBY"
            "FGByYbNALIBIJa0z0IgopiUAiSS45YAQoZVed5fKfGtsJU/9Q5/SB+EIhVciUNsC3qfETDgY6GMGtXxkqeSvJA0qYl"
            "XB73R5RcixdMaNxEkNoFxbU+fQkOqxJQeUGU2Auiu55G137fMOLZ+FquS5h6I5OaJpKubArl8FzYp7ntUgiPr7Xe7q"
            "6n9W9V6k9Xv4mf7wTsmdyRO9MsKYhV2Kjhhlxjpyp3PshY3tXt1pTnS6eN+MDMlIkErlfVEguu7JsJvsDVNgpCpDWR"
            "uBm8N5NmPrjafz4OT84fb++uj5fHp3dX+9etaMwCPjHgin2eOSbMhHMeueWOa45ZKvWUGDcYGbFzBWyRP4BxWhwA");
    auto stream = LVCreateMemoryStream(font.data(), font.size(), false);
    FT_Library library = nullptr;
    require(!FT_Init_FreeType(&library), "FreeType unavailable");
    blaze::EmbeddedFontCache cache;
    auto bytes = cache.read(library, lString8("face"), stream, font.size());
    require(!bytes.isNull() && bytes->get()[0] == 0, "WOFF2 did not reconstruct an SFNT");
    require(cache.read(library, lString8("face"), stream, font.size()).get() == bytes.get(),
            "A second font size reconstructed the WOFF2 again");
    FT_Face face = nullptr;
    require(!FT_New_Memory_Face(library, bytes->get(), bytes->length(), 0, &face)
                    && FT_Get_Char_Index(face, 'A'),
            "Decoded font is missing its glyph");
    FT_Done_Face(face);
    FT_Done_FreeType(library);

    auto files = test::publisherPackage(
            "en", "Codec fixture",
            { { "c.xhtml",
                "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
                "@font-face{font-family:Fixture;src:url(face.woff2)}p{font-family:Fixture}"
                "</style></head><body><p>AAAA</p><img src='cover.webp'/></body></html>" },
              { "face.woff2", font },
              { "cover.webp", image } },
            "<itemref idref='c.xhtml'/>");
    files["book.opf"].replace("face.woff2\" media-type=\"font/ttf",
                              "face.woff2\" media-type=\"font/woff2");
    files["book.opf"].replace("cover.webp\" media-type=\"font/ttf",
                              "cover.webp\" media-type=\"image/webp");
    EngineDocument model(test::readerFonts());
    openPublication(model, files);
    require(model.embeddedFontCount() == 1 && !model.image(0).isNull(),
            "EPUB with WOFF2 publisher font and WebP illustration did not render");
}
} // namespace

namespace {
// Entity declarations that Qt 5.2 could expand without a limit are refused.
bool entitiesRefused(const QByteArray &doctype)
{
    QXmlStreamReader xml(doctype + "<html/>");
    while (!xml.atEnd())
        if (xml.readNext() == QXmlStreamReader::DTD)
            return unsafeEntities(xml);
    return false;
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(!entitiesRefused(
                    "<!DOCTYPE html [<!ENTITY nbsp \"&#160;\"><!ENTITY mdash \"&#x2014;\">]>"),
            "Short character entities are refused");
    require(entitiesRefused("<!DOCTYPE html [<!ENTITY a \"&#38;b;\">]>"),
            "An entity that starts another reference is accepted");
    require(entitiesRefused("<!DOCTYPE html [<!ENTITY a \"x\"><!ENTITY b \"&a;&a;\">]>"),
            "Nested entities are accepted");
    require(entitiesRefused("<!DOCTYPE html [<!ENTITY a \"0123456789abcdefg\">]>"),
            "A long entity is accepted");
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
    for (int i = 1; i < argc; ++i) {
        // Optional independent fixtures nav-non-text_img and nav-non-text_img_title from W3C.
        QFile input(QString::fromLocal8Bit(argv[i]));
        require(input.open(QIODevice::ReadOnly), "Missing W3C navigation fixture");
        EngineDocument conformance(test::readerFonts());
        require(conformance.open(input.readAll(), { 600, 800 }, {}, &error), qPrintable(error));
        const auto toc = conformance.chapters();
        require(toc.size() == 2 && toc[0].title == "Start page"
                        && toc[1].title
                                == QString::fromUtf8("Description of the Abbey of Sénanque"),
                "W3C non-text navigation labels");
        for (int j = 0; j < 2; ++j)
            require(conformance.goTo(toc[j].position)
                            && conformance.spineItemOf(conformance.position()) == j,
                    "W3C TOC navigation target");
    }
    auto hidden = test::engineBook(false, 2, 10);
    hidden["OEBPS/c1.xhtml"].replace("</body>",
                                     "<span style='display:none' id='lastMarker'>8</span></body>");
    hidden["OEBPS/toc.ncx"].replace(
            "</ncx>",
            "<pageList><pageTarget type='normal' value='8'><navLabel><text>8</text></navLabel><content src='c1.xhtml#lastMarker'/></pageTarget></pageList></ncx>");
    EngineDocument markers(test::readerFonts());
    openPublication(markers, hidden);
    for (int mode = 0; mode < 2; ++mode) {
        if (mode)
            require(markers.showSpineItems(0, 0, &error), qPrintable(error));
        const auto pages = markers.printedPages();
        require(pages.size() == 1 && markers.goTo(pages.first().position)
                        && markers.spineItemOf(markers.position()) == 0,
                "Trailing hidden page marker left its chapter");
    }
    ResourceCache optional(
            { "missing", "present" },
            [](const QString &name, const ResourceCache::Sink &sink, QString *) {
                return name == "present" && sink("ok", 2);
            },
            std::make_shared<std::atomic_bool>(false));
    require(optional.readOptional("missing", 8192).isEmpty()
                    && optional.readBounded("present", 8) == "ok",
            "Optional chapter probe blocked another member");
    checkDtbookChapterWindow();
    checkCodecs();
    checkNavigation(false);
    checkNavigation(true);
    return 0;
}
