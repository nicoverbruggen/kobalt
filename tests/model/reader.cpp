#include "platforms/kobo/common/engine_positions.h"
#include "reader_inputs.h"
#include "support/qt_compat.h"
#include "assets/bundled_fonts.h"
#include "assets/assets.h"
#include <QTemporaryDir>
#include <QDir>
#include "engine/crengine/engine_document.h"
#include "engine/crengine/document_cache.h"
#include "engine/crengine/kobo_styles.h"
#include "engine/crengine/page_pixels.h"
#include "publication/publication.h"
#include "support/strings.h"
#include "publication/image_policy.h"
#include "publication/local_resources.h"
#include <lvstream.h>
#include <lvstring.h>
#include <lvimg.h>
#include <lvdrawbuf.h>
#include <lvfntman.h>
#include <lvbionic.h>
#include <QCryptographicHash>
#include <QColor>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegion>
#include <QTemporaryFile>
#include <QtEndian>
#include <QtMath>
#include <zlib.h>
#include <QGuiApplication>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>
#include "check.h"

// A stored EPUB with the given members, enough for directory inspection.
static QByteArray storedEpub(const QList<QPair<QByteArray, QByteArray>> &members)
{
    auto u16 = [](QByteArray &out, quint16 n) {
        const auto v = qToLittleEndian(n);
        out.append(reinterpret_cast<const char *>(&v), 2);
    };
    auto u32 = [](QByteArray &out, quint32 n) {
        const auto v = qToLittleEndian(n);
        out.append(reinterpret_cast<const char *>(&v), 4);
    };
    QByteArray bytes, directory;
    for (const auto &member : members) {
        const auto offset = quint32(bytes.size());
        u32(bytes, 0x04034b50);
        u16(bytes, 20);
        u16(bytes, 0);
        u16(bytes, 0);
        u32(bytes, 0);
        const auto crc = crc32(0, reinterpret_cast<const Bytef *>(member.second.constData()),
                               member.second.size());
        u32(bytes, crc);
        u32(bytes, member.second.size());
        u32(bytes, member.second.size());
        u16(bytes, member.first.size());
        u16(bytes, 0);
        bytes += member.first + member.second;
        u32(directory, 0x02014b50);
        u16(directory, 20);
        u16(directory, 20);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, crc);
        u32(directory, member.second.size());
        u32(directory, member.second.size());
        u16(directory, member.first.size());
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, offset);
        directory += member.first;
    }
    const auto start = quint32(bytes.size());
    bytes += directory;
    u32(bytes, 0x06054b50);
    u16(bytes, 0);
    u16(bytes, 0);
    u16(bytes, members.size());
    u16(bytes, members.size());
    u32(bytes, directory.size());
    u32(bytes, start);
    u16(bytes, 0);
    return bytes;
}

static void checkKoboStyleHacks()
{
    const QByteArray css =
            "div#book-inner p, div#book-inner div { font-size: 1.0em; }"
            "a { color: black; } a:link, a:visited, a:hover, a:active { color: blue; }"
            "div#book-inner * { margin-top: 0 !important; margin-bottom: 0 !important;}";
    const QByteArray head = "<html xmlns='http://www.w3.org/1999/xhtml'><head>"
                            "<style>h2{margin:1em 0 2em}p{margin:1em 0}</style>";
    const QByteArray body =
            "</head><body><div id='book-inner'><h2>Chapter title</h2>"
            "<p>First paragraph, caf\xc3\xa9.</p><p>Second paragraph.</p></div></body></html>";
    const auto style = [](const QByteArray &attributes, const QByteArray &contents) {
        return "<style " + attributes + ">" + contents + "</style>";
    };
    const auto empty = style("id='kobostylehacks'", {});
    const auto injected = style("id='kobostylehacks'", css);
    const auto cleaned = head + empty + body;
    require(kobalt::withoutKoboStyleHacks(head + injected + body) == cleaned,
            "Known Kobo injection did not leave publisher styles and text intact");
    require(kobalt::withoutKoboStyleHacks(head + style("class='extra kobostylehacks'", css) + body)
                    == head + style("class='extra kobostylehacks'", {}) + body,
            "Class-marked Kobo injection was retained");
    require(kobalt::withoutKoboStyleHacks(
                    head + style("id='kobostylehacks'", "<![CDATA[" + css + "]]>") + body)
                    == cleaned,
            "CDATA Kobo injection was retained");
    auto spaced = css;
    spaced.replace(";", ";\n\t");
    require(kobalt::withoutKoboStyleHacks(head + style("id='kobostylehacks'", spaced) + body)
                    == cleaned,
            "Whitespace prevented recognition of Kobo CSS");
    const QList<QByteArray> preserved{
        style("id='kobostylehacks'", "div#book-inner { margin-top: 0; margin-bottom: 0; }"),
        style("id='kobostylehacks'", css + "p{hyphens:none}"),
        style("id='publisher'", css),
        "<!--" + injected + "-->",
        "<script><![CDATA[" + injected + "]]></script>",
    };
    for (const auto &markup : preserved) {
        const auto original = head + markup + body;
        require(kobalt::withoutKoboStyleHacks(original) == original,
                "Custom or non-style markup was modified");
    }
    const auto malformed = head + injected + "</head><body><p>Unclosed";
    require(kobalt::withoutKoboStyleHacks(malformed) == malformed,
            "Malformed markup was rewritten");
    const QList<QPair<QByteArray, QByteArray>> files{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Spacing</title></metadata><manifest>"
          "<item id='c' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest>"
          "<spine><itemref idref='c'/></spine></package>" },
        { "chapter.xhtml", head + injected + body },
    };
    auto publisher = files;
    publisher.last().second = head + body;
    const auto settings = kobalt::qt::object({ { "font_pixels", 36 } });
    QString error;
    kobalt::EngineDocument expected(kobalt::test::readerFonts()),
            actual(kobalt::test::readerFonts());
    require(expected.open(storedEpub(publisher), QSize(800, 1000), settings, &error),
            qPrintable(error));
    require(actual.open(storedEpub(files), QSize(800, 1000), settings, &error), qPrintable(error));
    require(actual.image(0) == expected.image(0), "ZIP opening did not restore publisher spacing");
    QMap<QString, QByteArray> data;
    QMap<QString, qint64> sizes;
    for (const auto &file : files) {
        data[QString::fromUtf8(file.first)] = file.second;
        sizes[QString::fromUtf8(file.first)] = file.second.size();
    }
    auto resources = std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    kobalt::EngineDocument streamed(kobalt::test::readerFonts()),
            preview(kobalt::test::readerFonts());
    require(streamed.openResources(resources, sizes, QSize(800, 1000), settings, &error),
            qPrintable(error));
    require(streamed.image(0) == expected.image(0), "Resource opening retained Kobo style hacks");
    require(preview.openPreview(streamed, QSize(800, 1000), &error), qPrintable(error));
    require(preview.image(0) == expected.image(0), "Preview retained Kobo style hacks");
}

static void checkDarkModeTextColors()
{
    struct Case
    {
        const char *ink;
        const char *paper;
        QRgb nightInk;
        QRgb nightPaper = 0;
    };

    const Case cases[] = {
        { "#12120d", "transparent", qRgb(237, 237, 242) },
        { "#0d1212", "transparent", qRgb(242, 237, 237) },
        { "#120d12", "transparent", qRgb(237, 242, 237) },
        { "#20202a", "transparent", qRgb(223, 223, 213) },
        { "#20202b", "transparent", qRgb(32, 32, 43) },
        { "#121212", "transparent", qRgb(237, 237, 237) },
        { "#12120d", "#dddddd", qRgb(237, 237, 242), qRgb(34, 34, 34) },
        { "#e8f0f0", "#222222", qRgb(23, 15, 15), qRgb(221, 221, 221) },
        { "#0000ff", "transparent", qRgb(0, 0, 255) },
        { "#cc2200", "transparent", qRgb(204, 34, 0) },
        { "#12120d", "#ffff00", qRgb(18, 18, 13), qRgb(255, 255, 0) },
        { "#121212", "#ffff00", qRgb(18, 18, 18), qRgb(255, 255, 0) },
        { "#12120d", "#fffef5", qRgb(18, 18, 13), qRgb(255, 254, 245) },
        { "#e8f0f0", "#224466", qRgb(232, 240, 240), qRgb(34, 68, 102) },
        { "#eeeeee", "#224466", qRgb(238, 238, 238), qRgb(34, 68, 102) },
    };
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    for (const auto &test : cases) {
        for (const bool inlinePaper : { false, true }) {
            const QByteArray ink(test.ink), paper(test.paper);
            const auto background = " style='background-color:" + paper + "'";
            const auto body = "<p style='margin:0;text-indent:0;color:" + ink
                    + (inlinePaper ? "'" : ";background-color:" + paper + "'") + ">"
                    + (inlinePaper ? "<span" + background + ">" : "<span>")
                    + "Readable publisher text.</span></p>";
            const auto bytes = storedEpub({
                    { "mimetype", "application/epub+zip" },
                    { "META-INF/container.xml",
                      "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                    { "book.opf",
                      "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Text colors</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
                    { "c.xhtml",
                      "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Text colors</title></head><body>"
                              + body + "</body></html>" },
            });
            QString error;
            require(model.open(bytes, QSize(800, 300), { { "font_pixels", 48 } }, &error),
                    qPrintable(error));
            const auto day = model.image(0);
            const auto position = model.position();
            const auto pages = model.pages();
            model.setDarkMode(true);
            const auto night = model.image(0);
            const auto dayInk = QColor(test.ink).rgb();
            const bool hasPaper = paper != "transparent";
            const auto dayPaper = QColor(test.paper).rgb();
            const bool coloredPaper = hasPaper && dayPaper == test.nightPaper;
            QRect paperBounds;
            int checked = 0, checkedPaper = 0;
            for (int y = 0; y < day.height(); ++y) {
                for (int x = 0; x < day.width(); ++x) {
                    if (day.pixel(x, y) == dayInk) {
                        require(night.pixel(x, y) == test.nightInk,
                                qPrintable(
                                        QString("Dark text color changed incorrectly: %1 on %2 (%3 background)")
                                                .arg(test.ink, test.paper,
                                                     inlinePaper ? "inline" : "block")));
                        ++checked;
                    }
                    if (hasPaper && day.pixel(x, y) == dayPaper) {
                        const auto after = night.pixel(x, y);
                        // A faint glyph edge can round to the paper color only in day mode.
                        require(qAbs(qRed(after) - qRed(test.nightPaper)) <= 2
                                        && qAbs(qGreen(after) - qGreen(test.nightPaper)) <= 2
                                        && qAbs(qBlue(after) - qBlue(test.nightPaper)) <= 2,
                                qPrintable(
                                        QString("Dark paper changed incorrectly: %1 on %2 (%3 background), got %4")
                                                .arg(test.ink, test.paper,
                                                     inlinePaper ? "inline" : "block")
                                                .arg(night.pixel(x, y), 8, 16, QChar('0'))));
                        paperBounds |= QRect(x, y, 1, 1);
                        ++checkedPaper;
                    }
                }
            }
            require(checked > 100, "Text-color fixture did not render solid glyph pixels");
            require(!hasPaper || checkedPaper > 100,
                    "Text-color fixture did not render its background");
            if (coloredPaper) {
                for (int y = paperBounds.top(); y <= paperBounds.bottom(); ++y)
                    for (int x = paperBounds.left(); x <= paperBounds.right(); ++x) {
                        const auto before = day.pixel(x, y), after = night.pixel(x, y);
                        // Integer glyph blending can differ by two levels after inversion.
                        require(qAbs(qRed(before) - qRed(after)) <= 2
                                        && qAbs(qGreen(before) - qGreen(after)) <= 2
                                        && qAbs(qBlue(before) - qBlue(after)) <= 2,
                                "Dark mode changed text contrast on colored paper");
                    }
            }
            require(model.pages() == pages && model.position() == position,
                    "Dark colors moved the reading position");
            model.setDarkMode(false);
            require(model.image(0) == day, "Returning to light mode changed publisher text colors");
        }
    }
}

static void checkPagePixelConversion()
{
    for (const auto size :
         { QSize(1, 1), QSize(31, 33), QSize(65, 63), QSize(1264, 1680), QSize(2049, 2049) }) {
        QImage original(size, QImage::Format_RGB32);
        quint32 seed = 17;
        for (int y = 0; y < size.height(); ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(original.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                seed = seed * 1664525U + 1013904223U;
                // Cover grayscale, both sides of the visibility threshold,
                // arbitrary colour and arbitrary incoming alpha bytes.
                switch ((x + y) % 5) {
                case 0:
                    pixels[x] = seed;
                    break;
                case 1:
                    pixels[x] = (seed & 255U) * 0x010101U;
                    break;
                case 2:
                    pixels[x] = 0x0080808aU;
                    break;
                case 3:
                    pixels[x] = 0x0080808bU;
                    break;
                default:
                    pixels[x] = 0xffffffffU;
                    break;
                }
            }
        }
        for (const bool gray : { false, true })
            for (const bool dark : { false, true }) {
                auto before = original.copy();
                if (gray)
                    before.fill(0x00757575U);
                auto expected = before.copy(), actual = before.copy();
                const int columns = (size.width() + 31) / 32, rows = (size.height() + 31) / 32;
                const bool bounded = qint64(columns) * rows <= 4096;
                QVector<uchar> expectedTiles(bounded ? columns * rows : 0, 0),
                        actualTiles = expectedTiles;
                bool expectedColor = false;
                for (int y = 0; y < size.height(); ++y) {
                    auto *pixels = reinterpret_cast<QRgb *>(expected.scanLine(y));
                    for (int x = 0; x < size.width(); ++x) {
                        const QRgb pixel = pixels[x];
                        expectedColor |= kobalt::hasVisibleColor(pixel);
                        if (bounded
                            && (qRed(pixel) != qGreen(pixel) || qGreen(pixel) != qBlue(pixel)))
                            expectedTiles[(y / 32) * columns + x / 32] = 1;
                        pixels[x] = (pixel ^ (dark ? 0x00ffffffU : 0)) | 0xff000000U;
                    }
                }
                const bool actualColor = kobalt::normalizePagePixels(
                        actual, dark, bounded ? &actualTiles : nullptr, columns);
                require(actual == expected && actualColor == expectedColor
                                && actualTiles == expectedTiles,
                        "Page pixel conversion changed pixels or colour refresh classification");
                // RGBA byte order shows the same colours and classifies them alike.
                auto rgba = before.copy();
                QVector<uchar> rgbaTiles(expectedTiles.size(), 0);
                const bool rgbaColor = kobalt::normalizePagePixels(
                        rgba, dark, bounded ? &rgbaTiles : nullptr, columns, true);
                rgba.reinterpretAsFormat(QImage::Format_RGBX8888);
                require(rgba.convertToFormat(QImage::Format_RGB32) == expected
                                && rgbaColor == expectedColor && rgbaTiles == expectedTiles,
                        "RGBA page pixels changed colours or colour refresh classification");
            }
    }
}

// Kobo screens that store RGBA bytes receive pages in that byte order. They
// must show the same pixels and metadata as RGB32 pages, including pages drawn
// into a retired buffer.
static void checkPageFormat()
{
    QByteArray body;
    for (int i = 0; i < 60; ++i)
        body += "<p style='color:#cc2200;background-color:#fffef5'>Coloured publisher text that fills several "
                "pages so retired buffers are drawn again.</p>";
    const auto bytes = storedEpub({
            { "mimetype", "application/epub+zip" },
            { "META-INF/container.xml",
              "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
            { "book.opf",
              "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Page format</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
            { "c.xhtml",
              "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Page format</title></head><body>"
                      + body + "</body></html>" },
    });
    const auto pages = [&bytes](QImage::Format format, bool dark) {
        kobalt::EngineDocument::setPageFormat(format);
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        QString error;
        require(model.open(bytes, QSize(400, 300), { { "font_pixels", 24 } }, &error),
                qPrintable(error));
        model.setDarkMode(dark);
        require(model.pages() > 4, "Page format fixture is too short");
        QVector<QImage> result;
        for (int page = 0; page < 5; ++page) {
            model.go(page);
            result.append(model.image(page));
        }
        return result;
    };
    for (const bool dark : { false, true }) {
        const auto plain = pages(QImage::Format_RGB32, dark);
        const auto rgba = pages(QImage::Format_RGBX8888, dark);
        kobalt::EngineDocument::setPageFormat(QImage::Format_RGB32);
        for (int page = 0; page < plain.size(); ++page) {
            require(plain[page].format() == QImage::Format_RGB32
                            && rgba[page].format() == QImage::Format_RGBX8888,
                    "Page images did not use the requested format");
            require(rgba[page].convertToFormat(QImage::Format_RGB32) == plain[page],
                    "RGBA page images changed pixels");
            for (const auto *key : { "kobalt.image_regions", "kobalt.colour_regions",
                                     "kobalt.animation_text_only", "kobalt.image_only" })
                require(rgba[page].text(key) == plain[page].text(key),
                        "RGBA page images changed page metadata");
        }
        require(plain[0].text("kobalt.colour_regions") != "[]",
                "Page format fixture has no colour");
    }
}

static QByteArray hash(const QImage &image)
{
    return QCryptographicHash::hash(
            QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()),
                                    image.bytesPerLine() * image.height()),
            QCryptographicHash::Sha256);
}

// Dictionary hyphenation breaks only where a line would otherwise be loose, so
// hyphenation checks compare the first pages rather than one.
static QByteArray leadingPages(kobalt::EngineDocument &model)
{
    QByteArray result;
    for (int page = 0; page < qMin(5, model.pages()); ++page)
        result += hash(model.image(page));
    return result;
}

static void checkJpeg(bool progressive)
{
    jpeg_compress_struct codec{};
    jpeg_error_mgr errors{};
    codec.err = jpeg_std_error(&errors);
    jpeg_create_compress(&codec);
    unsigned char *data = nullptr;
    unsigned long length = 0;
    jpeg_mem_dest(&codec, &data, &length);
    codec.image_width = codec.image_height = 64;
    codec.input_components = 3;
    codec.in_color_space = JCS_RGB;
    jpeg_set_defaults(&codec);
    if (progressive)
        jpeg_simple_progression(&codec);
    jpeg_start_compress(&codec, TRUE);
    unsigned char row[64 * 3];
    for (int x = 0; x < 64; ++x) {
        row[x * 3] = x < 32 ? 240 : 16;
        row[x * 3 + 1] = 16;
        row[x * 3 + 2] = x < 32 ? 16 : 240;
    }
    JSAMPROW rows[] = { row };
    while (codec.next_scanline < codec.image_height)
        jpeg_write_scanlines(&codec, rows, 1);
    jpeg_finish_compress(&codec);
    auto stream = LVCreateMemoryStream(data, length, true, LVOM_READ);
    jpeg_destroy_compress(&codec);
    std::free(data);
    auto image = LVCreateStreamImageSource(stream);
    require(!image.isNull() && image->GetWidth() == 64 && image->GetHeight() == 64,
            "CREngine rejected JPEG dimensions");
    LVColorDrawBuf pixels(64, 64, 32);
    pixels.Clear(0xffffff);
    pixels.Draw(image, 0, 0, 64, 64, false);
    const auto red = pixels.GetPixel(8, 32), blue = pixels.GetPixel(56, 32);
    require(qRed(red) > 224 && qGreen(red) < 32 && qBlue(red) < 32 && qBlue(blue) > 224
                    && qGreen(blue) < 32 && qRed(blue) < 32,
            "CREngine did not preserve JPEG colors");
}

static void checkSvg()
{
    QByteArray svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"32\" viewBox=\"0 0 64 32\">"
            "<rect width=\"32\" height=\"32\" fill=\"#ff0000\"/>"
            "<rect x=\"32\" width=\"16\" height=\"32\" fill=\"#0000ff\" fill-opacity=\"0.5\"/></svg>";
    auto image = LVCreateStreamImageSource(
            LVCreateMemoryStream(svg.data(), svg.size(), false, LVOM_READ));
    require(!image.isNull() && image->GetWidth() == 64 && image->GetHeight() == 32,
            "SVG dimensions were lost");
    for (lUInt32 background : { 0xffffffU, 0U }) {
        LVColorDrawBuf pixels(64, 32, 32);
        pixels.Clear(background);
        pixels.Draw(image, 0, 0, 64, 32, false);
        const auto red = pixels.GetPixel(8, 16), blend = pixels.GetPixel(40, 16),
                   blank = pixels.GetPixel(56, 16);
        require((red & 0xffffffU) == 0xff0000U && (blank & 0xffffffU) == background,
                "SVG colors or transparency changed");
        require(qAbs(qRed(blend) - (background ? 127 : 0)) <= 2
                        && qAbs(qGreen(blend) - (background ? 127 : 0)) <= 2
                        && qAbs(qBlue(blend) - (background ? 255 : 128)) <= 2,
                "SVG opacity was not composited correctly");
    }
}

static void checkCssDensity()
{
    QImage art(30, 20, QImage::Format_RGB32);
    art.fill(Qt::red);
    QByteArray png;
    QBuffer output(&png);
    require(output.open(QIODevice::WriteOnly) && art.save(&output, "PNG"),
            "Cannot encode CSS density fixture");
    const QMap<QString, QByteArray> base{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata/><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/><item id='i' href='art.png' media-type='image/png'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "art.png", png }
    };
    const char *styles[]{ "", "width:30px;height:20px", "width:0.5in;height:0.25in",
                          "width:2em;height:1em", "width:25%;height:auto" };
    QSize sizesAt212[5], sizesAt300[5];
    QString imageId;
    const int originalPpi = kobalt::readingPpi.load();
    for (const int ppi : { 212, 300 }) {
        kobalt::readingPpi.store(ppi);
        for (int i = 0; i < 5; ++i) {
            auto data = base;
            data["c.xhtml"] =
                    QByteArray(
                            "<html xmlns='http://www.w3.org/1999/xhtml'><body><p style='margin:0'><img src='art.png' style='")
                    + styles[i] + "'/></p></body></html>";
            QMap<QString, qint64> lengths;
            for (auto it = data.cbegin(); it != data.cend(); ++it)
                lengths[it.key()] = it.value().size();
            auto resources = std::make_shared<kobalt::ResourceCache>(
                    data.keys(),
                    [data](const QString &name, const kobalt::ResourceCache::Sink &sink,
                           QString *) {
                        const auto bytes = data.value(name);
                        return sink(bytes.constData(), bytes.size());
                    },
                    std::make_shared<std::atomic_bool>(false));
            kobalt::EngineDocument model(kobalt::test::readerFonts());
            QString error;
            const auto settings = kobalt::qt::object({ { "font_pixels", 40 },
                                                       { "margin_pixels", 0 },
                                                       { "top_margin_pixels", 0 },
                                                       { "bottom_margin_pixels", 0 } });
            require(model.openResources(resources, lengths, QSize(800, 1000), settings, &error),
                    qPrintable(error));
            require(model.fontSize() == 40 && model.margin() == 0,
                    "CSS density rescaled saved reading pixels");
            QRect ink;
            const auto rendered = model.image(0);
            for (int y = 0; y < rendered.height(); ++y)
                for (int x = 0; x < rendered.width(); ++x) {
                    const auto pixel = rendered.pixel(x, y);
                    if (qRed(pixel) > 200 && qGreen(pixel) < 30 && qBlue(pixel) < 30)
                        ink = ink.united(QRect(x, y, 1, 1));
                }
            require(!ink.isEmpty(), "CSS density fixture image is missing");
            const auto originalImage = model.imageAt(ink.center());
            const auto id = originalImage.text("kobalt.image_id");
            require(id.size() == 64, "Image viewer has no stable content identity");
            if (imageId.isEmpty())
                imageId = id;
            require(id == imageId, "Image identity changed with display size or reading density");
            require(originalImage.size() == art.size()
                            && originalImage.pixel(0, 0) == qRgb(255, 0, 0),
                    "Image viewer did not recover the low-resolution source image");
            require(model.imageAt({ 799, 999 }).isNull(), "Blank paper resolved to an image");
            (ppi == 212 ? sizesAt212 : sizesAt300)[i] = ink.size();
            if (i == 0) {
                kobalt::EngineDocument preview(kobalt::test::readerFonts());
                require(preview.openPreview(model, QSize(800, 1000), &error), qPrintable(error));
                require(preview.image(0) == rendered, "Preview used a different CSS density");
            }
        }
    }
    const QByteArray illustration = "<img src='art.png' style='width:150px;height:80px'/>";
    for (const auto &body :
         { illustration, illustration + "<p>A caption</p>", illustration + illustration }) {
        auto data = base;
        data["c.xhtml"] =
                "<html xmlns='http://www.w3.org/1999/xhtml'><body>" + body + "</body></html>";
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        QString error;
        QList<QPair<QByteArray, QByteArray>> members;
        for (auto it = data.cbegin(); it != data.cend(); ++it)
            members.append({ it.key().toUtf8(), it.value() });
        require(model.open(storedEpub(members), QSize(800, 1000), { { "font_pixels", 24 } },
                           &error),
                qPrintable(error));
        const auto rendered = model.image(0);
        require(!kobalt::pageSingleImage(rendered).isEmpty() == (body == illustration),
                "Image-only detection included captions or multiple images, or lost the single image");
    }
    kobalt::readingPpi.store(originalPpi);
    for (int i : { 0, 1 }) {
        require(qAbs(sizesAt212[i].width() - 66) <= 1 && qAbs(sizesAt212[i].height() - 44) <= 1,
                "Intrinsic or CSS pixel size did not use 212 ppi");
        require(qAbs(sizesAt300[i].width() - 94) <= 1 && qAbs(sizesAt300[i].height() - 63) <= 1,
                "Intrinsic or CSS pixel size did not use 300 ppi");
    }
    require(qAbs(sizesAt212[2].width() - 106) <= 1 && qAbs(sizesAt212[2].height() - 53) <= 1
                    && qAbs(sizesAt300[2].width() - 150) <= 1
                    && qAbs(sizesAt300[2].height() - 75) <= 1,
            "Physical inch size did not follow screen density");
    require(sizesAt212[3] == sizesAt300[3] && qAbs(sizesAt300[3].width() - 80) <= 1
                    && qAbs(sizesAt300[3].height() - 40) <= 1,
            "Font-relative image size changed with CSS density");
    require(sizesAt212[4] == sizesAt300[4] && qAbs(sizesAt300[4].width() - 200) <= 1,
            "Percentage image width changed with CSS density");
}

static void checkBionicWidth()
{
    QString family;
    int defaultWeight = 400;
    for (const auto &font : kobalt::discoverReadingFonts(kobalt::test::readerFonts().sources))
        for (const auto &file : font.files)
            if (QFileInfo(file).fileName() == "AmstelvarAlpha-VF.ttf") {
                family = font.family;
                defaultWeight = font.weightAxis.defaultValue;
            }
    require(!family.isEmpty(), "Missing variable-width font fixture");
    const auto publication = [&](int width, bool explicitPrefix) {
        const QByteArray chapter =
                "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0;"
                "font-family:'"
                + family.toHtmlEscaped().toUtf8() + "';font-weight:400;font-width:"
                + QByteArray::number(width) + "%}</style></head><body><p>"
                + (explicitPrefix ? QByteArray("<span style='font-weight:700'>MMMM</span>MMMM")
                                  : QByteArray("MMMMMMMM"))
                + "</p></body></html>";
        return storedEpub(
                { { "mimetype", "application/epub+zip" },
                  { "META-INF/container.xml",
                    "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                  { "book.opf",
                    "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata/><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
                  { "c.xhtml", chapter } });
    };
    QImage normalWidth;
    for (const int width : { 100, 75 }) {
        kobalt::EngineDocument actual(kobalt::test::readerFonts()),
                expected(kobalt::test::readerFonts());
        QString error;
        auto settings = kobalt::qt::object({ { "font_face", family },
                                             { "font_weight", defaultWeight },
                                             { "font_pixels", 40 },
                                             { "margin_pixels", 0 },
                                             { "bold_reading", "bold" } });
        require(actual.open(publication(width, false), QSize(800, 1000), settings, &error),
                qPrintable(error));
        settings["bold_reading"] = "off";
        require(expected.open(publication(width, true), QSize(800, 1000), settings, &error),
                qPrintable(error));
        require(actual.setOpticalSize(14) && expected.setOpticalSize(14),
                "Cannot set fixture optical size");
        require(actual.image(0) == expected.image(0),
                "Bold Reading lost authored font width or optical size on emphasized runs");
        if (width == 100)
            normalWidth = actual.image(0);
        else
            require(actual.image(0) != normalWidth,
                    "The variable-width fixture did not change its glyph widths");
    }
}

static void checkObfuscatedFonts()
{
    QFile font(kobalt::test::readerAssets().file("fonts/InstrumentSerif-Regular.ttf"));
    require(font.open(QIODevice::ReadOnly), "open synthetic embedded font fixture");
    const auto original = font.readAll();
    const QByteArray idpfAlgorithm = "http://www.idpf.org/2008/embedding";
    const QByteArray adobeAlgorithm = "http://ns.adobe.com/pdf/enc#RC";
    auto book = [&](const QByteArray &fontBytes, const QByteArray &algorithm,
                    const QByteArray &identifier, const QByteArray &extraIdentifiers = QByteArray(),
                    const QByteArray &mediaType = "font/ttf") {
        QList<QPair<QByteArray, QByteArray>> members = {
            { "mimetype", "application/epub+zip" },
            { "META-INF/container.xml",
              "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
            { "book.opf",
              "<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='book-id'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='book-id'>"
                      + identifier + "</dc:identifier>" + extraIdentifiers
                      + "</metadata><manifest><item id='font' href='font.ttf' media-type='"
                      + mediaType
                      + "'/><item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='chapter'/></spine></package>" },
            { "chapter.xhtml",
              "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>@font-face {font-family: Embedded; src: url(font.ttf);} body {font-family: Embedded;}</style></head><body><p>Embedded font glyphs should match the unobfuscated original.</p></body></html>" },
            { "font.ttf", fontBytes }
        };
        if (!algorithm.isEmpty()) {
            members.append(QPair<QByteArray, QByteArray>{
                    "META-INF/encryption.xml",
                    "<encryption xmlns='urn:oasis:names:tc:opendocument:xmlns:container' xmlns:enc='http://www.w3.org/2001/04/xmlenc#'><enc:EncryptedData><enc:EncryptionMethod Algorithm='"
                            + algorithm
                            + "'/><enc:CipherData><enc:CipherReference URI='font.ttf'/></enc:CipherData></enc:EncryptedData></encryption>" });
        }
        return storedEpub(members);
    };
    QString error;
    QByteArray expected, expectedPreview;
    {
        kobalt::EngineDocument plain(kobalt::test::readerFonts());
        require(plain.open(book(original, {}, "test-identifier"), QSize(800, 1000), {}, &error),
                qPrintable(error));
        require(plain.embeddedFontCount() == 1 && plain.style(32, 120, "", 36),
                "load original publisher font");
        expected = hash(plain.image(0));
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(plain, QSize(800, 1000), &error), qPrintable(error));
        expectedPreview = hash(preview.image(0));
    }

    struct FontCase
    {
        QByteArray algorithm, identifier, extraIdentifiers, key;
        int prefixBytes;
        QByteArray mediaType = "font/ttf";
    };

    const auto idpfKey = QCryptographicHash::hash("test-identifier", QCryptographicHash::Sha1);
    const auto adobeKey = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
    {
        kobalt::EngineDocument fallback(kobalt::test::readerFonts());
        require(fallback.open(book({}, {}, "test-identifier"), QSize(800, 1000), {}, &error),
                qPrintable(error));
        auto oversized = original
                + QByteArray(LVFontManager::MaxEmbeddedFontBytes + 1 - original.size(), '\0');
        const auto checkFallback = [&](const QByteArray &bytes, const QByteArray &algorithm,
                                       const QByteArray &identifier) {
            const auto archive = book(bytes, algorithm, identifier);
            const auto matchesFallback = [&](kobalt::EngineDocument &reader) {
                require(reader.embeddedFontCount() == 0 && !reader.image(0).isNull()
                                && reader.image(0) == fallback.image(0),
                        "oversized embedded fonts should use the same page as a missing font");
            };
            {
                kobalt::EngineDocument reader(kobalt::test::readerFonts());
                require(reader.open(archive, QSize(800, 1000), {}, &error), qPrintable(error));
                matchesFallback(reader);
            }
            QTemporaryFile file;
            require(file.open() && file.write(archive) == archive.size() && file.flush(),
                    "write oversized-font EPUB");
            kobalt::Publication publication;
            require(publication.inspect(file, &error), qPrintable(error));
            const auto factory = kobalt::localResourceFactory(
                    file.fileName(), std::make_shared<kobalt::SourceLease>());
            const auto resources = factory(publication.sizes(), {}, {});
            kobalt::EngineDocument reader(kobalt::test::readerFonts());
            require(reader.openResources(resources, publication.sizes(), QSize(800, 1000), {},
                                         &error),
                    qPrintable(error));
            matchesFallback(reader);
            require(resources->statistics()["transferred_bytes"].toDouble() < bytes.size(),
                    "an oversized embedded font should not reach the resource loader");
        };
        checkFallback(oversized, {}, "test-identifier");
        for (int i = 0; i < 1024; ++i)
            oversized[i] = char(uchar(oversized[i]) ^ uchar(adobeKey[i % adobeKey.size()]));
        checkFallback(oversized, adobeAlgorithm, "urn:uuid:01234567-89ab-cdef-0123-456789abcdef");
    }
    for (const auto &fixture :
         { FontCase{ idpfAlgorithm, "test-identifier", {}, idpfKey, 1040 },
           FontCase{ idpfAlgorithm, "test-identifier", {}, idpfKey, 1040, "font/sfnt" },
           FontCase{ idpfAlgorithm, "test-identifier", {}, idpfKey, 1040, "vendor/unknown" },
           FontCase{ idpfAlgorithm, " test-\tiden\n tifier\r ", {}, idpfKey, 1040 },
           FontCase{ adobeAlgorithm,
                     "urn:uuid:01234567-89ab-cdef-0123-456789abcdef",
                     {},
                     adobeKey,
                     1024 },
           FontCase{
                   adobeAlgorithm, "test-identifier",
                   "<dc:identifier id='font-id'>urn:uuid:01234567-89ab-cdef-0123-456789abcdef</dc:identifier>",
                   adobeKey, 1024, "application/x-font-truetype" },
           FontCase{ adobeAlgorithm,
                     "urn:uuid:01234567-89ab-cdef-0123-456789abcdef",
                     {},
                     adobeKey,
                     1024,
                     "application/octet-stream" } }) {
        auto obfuscated = original;
        for (int i = 0; i < qMin(fixture.prefixBytes, int(obfuscated.size())); ++i) {
            obfuscated[i] = char(uchar(obfuscated[i]) ^ uchar(fixture.key[i % fixture.key.size()]));
        }
        const auto bytes = book(obfuscated, fixture.algorithm, fixture.identifier,
                                fixture.extraIdentifiers, fixture.mediaType);
        QTemporaryDir cache;
        require(cache.isValid(), "create obfuscated font cache");
        const auto fingerprint =
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        for (bool cached : { false, true }) {
            kobalt::EngineDocument decoded(kobalt::test::readerFonts());
            decoded.setDocumentCache(cache.path(), fingerprint, "obfuscated-font-test",
                                     bytes.size());
            require(decoded.open(bytes, QSize(800, 1000), {}, &error), qPrintable(error));
            require(decoded.openedFromCache() == cached, "obfuscated font cache was not reused");
            require(decoded.embeddedFontCount() == 1 && decoded.style(32, 120, "", 36),
                    "load obfuscated publisher font");
            require(hash(decoded.image(0)) == expected,
                    "obfuscated font glyphs differ from the original font");
            kobalt::EngineDocument preview(kobalt::test::readerFonts());
            require(preview.openPreview(decoded, QSize(800, 1000), &error), qPrintable(error));
            require(preview.embeddedFontCount() == 1 && hash(preview.image(0)) == expectedPreview,
                    "obfuscated font preview changed publisher glyphs");
            require(decoded.saveDocumentCache(), "save obfuscated font cache");
        }
    }
}

static void checkDeferredPublisherStyles()
{
    const QByteArray prefix =
            "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>body{margin:0}p{margin:0;text-indent:0}";
    auto book = [&](bool deferred) {
        const QByteArray css = deferred
                ? "img:only-of-type{margin-top:48px}p.caption:nth-of-type(1){text-align:center}"
                : "img{margin-top:48px}p.caption{text-align:center}";
        return storedEpub(
                { { "mimetype", "application/epub+zip" },
                  { "META-INF/container.xml",
                    "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                  { "book.opf",
                    "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata/><manifest><item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/><item id='image' href='image.svg' media-type='image/svg+xml'/></manifest><spine><itemref idref='chapter'/></spine></package>" },
                  { "chapter.xhtml",
                    prefix + css
                            + "</style></head><body><img src='image.svg' width='160' height='80'/><p class='caption'>Centered caption</p><p>Ordinary prose remains left aligned.</p></body></html>" },
                  { "image.svg",
                    "<svg xmlns='http://www.w3.org/2000/svg' width='160' height='80'><rect width='160' height='80' fill='black'/></svg>" } });
    };
    QString error;
    QByteArray page, preview;
    for (bool deferred : { false, true }) {
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(book(deferred), QSize(800, 1000), {}, &error), qPrintable(error));
        kobalt::EngineDocument section(kobalt::test::readerFonts());
        require(section.openPreview(model, QSize(800, 1000), &error), qPrintable(error));
        if (!deferred) {
            page = hash(model.image(0));
            preview = hash(section.image(0));
        } else {
            require(hash(model.image(0)) == page,
                    "Deferred publisher styles changed image spacing or centering");
            require(hash(section.image(0)) == preview,
                    "Deferred publisher styles changed the section preview");
        }
    }
}

static void checkVerticalJustification()
{
    const QByteArray paragraph =
            "<p>A reader follows <a href='#end'>the passage</a> across the page. "
            "<em>Careful spacing</em> keeps words clear and preserves every selection. "
            "We can return to this sentence after a change in appearance.</p>";
    const auto publication = [](const QByteArray &body) {
        return storedEpub(
                { { "mimetype", "application/epub+zip" },
                  { "META-INF/container.xml",
                    "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                  { "book.opf",
                    "<package xmlns='http://www.idpf.org/2007/opf' version='2.0' unique-identifier='id'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>vertical-fill</dc:identifier><dc:title>Vertical fill</dc:title><dc:language>en</dc:language></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
                  { "c.xhtml",
                    "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}</style></head><body>"
                            + body + "</body></html>" } });
    };
    const auto bytes =
            publication("<h1>Chapter</h1>" + paragraph.repeated(90) + "<p id='end'>The end.</p>");
    QString error;
    QTemporaryDir cache;
    require(cache.isValid(), "create vertical justification cache");
    QByteArray expected;
    for (bool cached : { false, true }) {
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        model.setDocumentCache(cache.path(), "vertical-fill-fingerprint", "vertical-fill",
                               bytes.size());
        require(model.open(bytes, QSize(800, 973), {}, &error), qPrintable(error));
        require(model.openedFromCache() == cached, "vertical fill cache was not reused");
        const auto pages = model.pages();
        require(pages > 5, "vertical fill fixture is too short");
        const auto headings = model.searchPage("Chapter", 0).hits;
        require(headings.size() == 1, "missing chapter heading");
        require(model.go(0), "open chapter heading page");
        const auto headingRects = model.rangeRects(headings.first().start, headings.first().end);
        require(!headingRects.isEmpty(), "missing chapter heading geometry");
        const auto opening = model.image(0);
        require(model.applyAppearance({ { "vertical_justification", true } }),
                "enable vertical fill");
        const auto expandedOpening = model.image(0);
        require(model.rangeRects(headings.first().start, headings.first().end) == headingRects,
                "vertical fill moved the chapter heading");
        for (const auto &rect : headingRects)
            require(expandedOpening.copy(rect) == opening.copy(rect),
                    "vertical fill changed chapter heading pixels");
        // Headings keep their spacing while the prose below them can expand.
        require(expandedOpening != opening, "chapter heading disabled prose expansion");
        require(model.applyAppearance({ { "vertical_justification", false } }),
                "disable vertical fill");
        require(model.go(1), "open prose page");
        const auto before = model.image(1);
        const auto edges = model.annotationPageEdges();
        const auto beforeFirst = model.rangeRects(edges.first.start, edges.first.end);
        const auto beforeLast = model.rangeRects(edges.last.start, edges.last.end);
        const auto beforeHandles = model.annotationGeometry(edges.last.start, edges.last.end);
        const auto position = model.position();
        const auto links = model.pageLinks();
        require(!beforeFirst.isEmpty() && !beforeLast.isEmpty() && !links.isEmpty(),
                "missing prose geometry");
        require(model.applyAppearance({ { "vertical_justification", true } }), "enable prose fill");
        const auto after = model.image(1);
        require(after != before, "vertical justification did not change prose pixels");
        require(model.pages() == pages && model.position() == position,
                "vertical fill changed page breaks or position");
        const auto afterFirst = model.rangeRects(edges.first.start, edges.first.end);
        const auto afterLast = model.rangeRects(edges.last.start, edges.last.end);
        require(afterFirst == beforeFirst, "first prose line moved");
        const int shift = afterLast.last().top() - beforeLast.last().top();
        require(shift > 0 && afterLast.last().size() == beforeLast.last().size(),
                "last word was not translated intact");
        auto inkBounds = [](const QImage &image) {
            QRect ink;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qGray(image.pixel(x, y)) < 100)
                        ink |= QRect(x, y, 1, 1);
            return ink;
        };
        const auto beforeInk = inkBounds(before), afterInk = inkBounds(after);
        require(afterInk.top() == beforeInk.top()
                        && afterInk.bottom() - beforeInk.bottom() == shift,
                "painted text and selection geometry moved by different amounts");

        require(model.wordAt(afterLast.last().center()).start == edges.last.start,
                "shifted word hit test missed");
        const auto geometry = model.annotationGeometry(edges.last.start, edges.last.end);
        require(geometry.rectangles == afterLast, "annotation did not follow shifted word");
        require(geometry.start.first.y() == beforeHandles.start.first.y() + shift
                        && geometry.end.first.y() == beforeHandles.end.first.y() + shift,
                "annotation handles did not follow shifted word");
        require(model.pageLinks().last().rectangles.last().top()
                        >= links.last().rectangles.last().top(),
                "link geometry did not follow text");
        auto imageHash = [](const QImage &image) {
            return QCryptographicHash::hash(
                    QByteArray(reinterpret_cast<const char *>(image.constBits()),
                               image.sizeInBytes()),
                    QCryptographicHash::Sha256);
        };
        if (cached)
            require(imageHash(after) == expected, "cache reload changed vertical fill");
        else
            expected = imageHash(after);
        require(model.go(pages - 1), "open ending page");
        const auto ending = model.image(pages - 1);
        require(model.applyAppearance({ { "vertical_justification", false } }),
                "disable ending fill");
        require(model.image(pages - 1) == ending, "short ending page was stretched");
        require(model.applyAppearance({ { "vertical_justification", true }, { "columns", 2 } },
                                      QSize(1280, 783)),
                "enable two-column fill");
        require(model.go(1), "open two-column prose");
        const auto columnEdges = model.annotationPageEdges();
        const auto lastRects = model.rangeRects(columnEdges.last.start, columnEdges.last.end);
        require(!lastRects.isEmpty()
                        && model.wordAt(lastRects.last().center()).start == columnEdges.last.start,
                "second-column inverse mapping missed the word");
        require(model.applyAppearance({ { "columns", 1 } }, QSize(800, 973)),
                "restore single-column viewport");
        require(model.saveDocumentCache(), "save vertical justification line metadata");
    }
    // These books fill several pages, so an unchanged first page proves the
    // exclusion rather than merely hitting the short-ending-page rule.
    for (const auto &body :
         { QByteArray("<p>First verse<br/>Second verse<br/>Third verse<br/>Fourth verse</p>")
                   .repeated(60),
           QByteArray("<pre>First code line\nSecond code line\nThird code line\n</pre>")
                   .repeated(60),
           QByteArray("<div style='border:1px solid black'>") + paragraph.repeated(90)
                   + "</div>" }) {
        kobalt::EngineDocument excluded(kobalt::test::readerFonts());
        require(excluded.open(publication(body), QSize(800, 973), {}, &error), qPrintable(error));
        require(excluded.pages() > 2, "exclusion fixture did not fill a page");
        const auto before = excluded.image(0);
        require(excluded.applyAppearance({ { "vertical_justification", true } }),
                "enable excluded layout fill");
        require(excluded.image(0) == before,
                "vertical fill changed an authored or decorated layout");
    }
}

// Checks the restored reader assertions: rendering, embedded fonts, Bold Reading, variable axes, source positions and previews.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    checkPageFormat();
    checkDeferredPublisherStyles();
    checkObfuscatedFonts();
    const auto positions = kobalt::nickel::positionCodec();
    const QVector<kobalt::ReadingFont> axisFixture{ { "Variable fixture",
                                                      {},
                                                      kobalt::FontOrigin::Sideloaded,
                                                      { 300, 650, 450 },
                                                      { 6.25, 72.75, 14 } } };
    require(kobalt::fontWeightAxis(axisFixture, "VARIABLE FIXTURE").defaultValue == 450
                    && !kobalt::fontWeightAxis(axisFixture, "Missing font").available(),
            "Weight axis lookup lost family matching");
    const auto opticalFixture = kobalt::fontOpticalSizeAxis(axisFixture, "Variable fixture");
    require(kobalt::normalizedOpticalSize(opticalFixture, 0) == 14
                    && kobalt::normalizedOpticalSize(opticalFixture, 6) == 7
                    && kobalt::normalizedOpticalSize(opticalFixture, 73) == 72
                    && kobalt::normalizedOpticalSize(opticalFixture, 14.6) == 15,
            "Optical size normalization lost defaults or whole-point bounds");
    checkVerticalJustification();
    checkCssDensity();
    checkBionicWidth();
    {
        kobalt::EngineDocument unopened(kobalt::test::readerFonts());
        const kobalt::PositionCodec synthetic{ [](const kobalt::EngineDocument &, const QString &,
                                                  QString *) {
                                                  return QStringLiteral("/body/p[1]");
                                              },
                                               [](const kobalt::EngineDocument &, QString *) {
                                                   return QStringLiteral("external-position");
                                               },
                                               [](const QString &location) { return location; } };
        QString error;
        require(unopened.comparePlatformPosition(synthetic, "external-position", &error) == 0
                        && !error.isEmpty(),
                "Unopened model accepted a decoded position without a document");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        error.clear();
        require(!preview.openPreview(unopened, QSize(600, 800), &error) && !error.isEmpty()
                        && preview.pages() == 0 && preview.image(0).isNull(),
                "Previewing an unopened model published a readable document");
    }
    {
        auto initial = kobalt::test::readerFonts();
        initial.sources.systemDirectory.clear();
        initial.sources.sideloadDirectory.clear();
        auto changed = initial;
        changed.hyphenationDirectory += "/.";
        for (const auto &file : initial.fallbackFiles)
            if (QFileInfo(file).fileName().startsWith("NotoSerif-"))
                changed.fallbackFiles.removeOne(file);
        {
            kobalt::EngineDocument first(initial);
            require(first.initializeFonts(), "Initial explicit font inputs failed");
            const auto faces = first.fontFaces();
            require(faces.contains("Noto Serif"), "Font cache fixture has no supplemental serif");
            auto catalog = first.fontCatalog();
            bool serif = false;
            for (const auto &font : catalog)
                serif |= font.family == "Noto Serif";
            require(serif, "Font catalog omitted the registered supplemental serif");
            auto copy = faces;
            copy.clear();
            catalog.clear();
            require(first.fontFaces() == faces && !first.fontCatalog().isEmpty(),
                    "Returned metadata modified the shared font cache");
            kobalt::EngineDocument conflicting(changed);
            require(!conflicting.initializeFonts(),
                    "Different live font inputs replaced the shared manager");
            require(conflicting.fontFaces().isEmpty() && conflicting.fontCatalog().isEmpty(),
                    "Conflicting inputs received another model's font cache");
            require(first.fontFaces() == faces, "Rejected font inputs changed an active model");
        }
        kobalt::EngineDocument afterDrain(changed);
        require(afterDrain.initializeFonts(),
                "Font inputs could not change after the last model released them");
        require(!afterDrain.fontFaces().contains("Noto Serif"),
                "Closed font manager left stale cached families");
        for (const auto &font : afterDrain.fontCatalog())
            require(font.family != "Noto Serif",
                    "Closed font manager left a stale cached catalog entry");
    }
    {
        const auto bundled = kobalt::bundledFontFiles(kobalt::test::readerAssets());
        QTemporaryDir temp;
        require(temp.isValid(), "Cannot create font catalog fixture");
        const auto side = temp.path() + "/sideloaded";
        require(QDir().mkpath(side), "Cannot create sideloaded font fixture");
        QString regular;
        for (const auto &file : bundled) {
            if (file.endsWith("/Libron-Regular.ttf"))
                regular = file;
        }
        require(QFile::copy(kobalt::test::readerAssets().file("fonts/InstrumentSerif-Regular.ttf"),
                            side + "/Interface.ttf"),
                "Cannot copy sideloaded family");
        require(!regular.isEmpty() && QFile::copy(regular, side + "/Duplicate.ttf"),
                "Cannot copy duplicate font");
        QFile invalid(side + "/Broken.ttf");
        require(invalid.open(QIODevice::WriteOnly), "Cannot create invalid font");
        invalid.write("not a font");
        invalid.close();
        const auto catalog =
                kobalt::discoverReadingFonts({ bundled, temp.path() + "/absent", side });
        int core = 0, variableCore = 0, sideloaded = 0, interface = 0, arabicFallback = 0,
            hebrewFallback = 0;
        for (const auto &font : catalog) {
            if (font.origin == kobalt::FontOrigin::Recommended) {
                ++core;
                if (font.weightAxis.available())
                    ++variableCore;
                else
                    require(font.files.size() == 4,
                            "Core family lost a style or accepted a duplicate");
                for (const auto &file : font.files)
                    require(!file.startsWith(side), "Duplicate sideload replaced a bundled font");
            } else if (font.origin == kobalt::FontOrigin::Interface) {
                ++interface;
                if (font.family == "IBM Plex Sans") {
                    require(font.files.size() == 3, "Bundled control family lost a style");
                } else if (font.family == "Noto Naskh Arabic") {
                    ++arabicFallback;
                    require(font.files.size() == 2
                                    && font.files.first().endsWith("/NotoNaskhArabic-Regular.ttf")
                                    && font.files.last().endsWith("/NotoNaskhArabic-Bold.ttf"),
                            "Arabic fallback lost its interface classification or regular and bold faces");
                } else if (font.family == "Noto Serif Hebrew") {
                    ++hebrewFallback;
                    require(font.files.size() == 2
                                    && font.files.first().endsWith("/NotoSerifHebrew-Regular.ttf")
                                    && font.files.last().endsWith("/NotoSerifHebrew-Bold.ttf"),
                            "Hebrew fallback lost its interface classification or regular and bold faces");
                } else {
                    QStringList scriptFiles;
                    for (const auto *name : kobalt::scriptFallbackFontFiles)
                        scriptFiles.append(QString::fromLatin1(name));
                    require(font.files.size() == (font.family == "Noto Serif Tamil" ? 4 : 2),
                            "Script fallback lost a style");
                    for (const auto &file : font.files) {
                        require(scriptFiles.contains(QFileInfo(file).fileName()),
                                "Unexpected bundled interface font family");
                    }
                }
            } else {
                ++sideloaded;
                require(font.origin == kobalt::FontOrigin::Sideloaded
                                && font.family == "Instrument Serif",
                        "Sideloaded source was misclassified");
            }
        }
        QFile collection(kobalt::test::readerAssets().file("fonts/ebook-core.json"));
        require(collection.open(QIODevice::ReadOnly), "Cannot read core font collection");
        const auto coreFiles =
                QJsonDocument::fromJson(collection.readAll()).object().value("files").toArray();
        require(!coreFiles.isEmpty() && coreFiles.size() % 4 == 0, "Core collection lost a style");
        require(core == coreFiles.size() / 4 + variableCore && sideloaded == 1 && interface == 9
                        && arabicFallback == 1 && hebrewFallback == 1,
                "Core collection or font catalog incomplete");
        require(kobalt::fontFamilyKey("KF Libron") == kobalt::fontFamilyKey("Libron"),
                "Legacy font alias changed");
    }
    checkJpeg(false);
    checkJpeg(true);
    checkSvg();
    checkKoboStyleHacks();
    checkDarkModeTextColors();
    checkPagePixelConversion();
    require(argc == 3, "Pass the text and color test EPUB paths");
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "Cannot read fixture");
    auto bytes = file.readAll();
    QString error;
    kobalt::Publication publication;
    require(publication.inspect(bytes, &error), qPrintable(error));
    {
        require(publication.fingerprint().size() == 64, "Publication has no fingerprint");
        kobalt::Publication same, edited;
        require(same.inspect(bytes, &error) && same.fingerprint() == publication.fingerprint(),
                "Fingerprint is not stable");
        // An edited member has a new CRC-32 in the directory.
        auto changed = bytes;
        const int member = changed.indexOf(QByteArray("PK\1\2", 4));
        require(member >= 0, "Missing test ZIP directory");
        changed[member + 16] = char(changed[member + 16] ^ 1);
        require(edited.inspect(changed, &error)
                        && edited.fingerprint() != publication.fingerprint(),
                "Fingerprint ignored a member change");
    }
    {
        // A sparse archive tests directory inspection beyond the old whole-book
        // limit without allocating or reading a large payload in the test.
        const int end = bytes.size() - 22;
        const auto directory = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar *>(bytes.constData() + end + 16));
        auto tail = bytes.mid(directory);
        const quint32 largeOffset = 320 * 1024 * 1024;
        qToLittleEndian<quint32>(largeOffset,
                                 reinterpret_cast<uchar *>(tail.data() + tail.size() - 6));
        QTemporaryFile large;
        require(large.open() && large.write(bytes.constData(), directory) == directory
                        && large.seek(largeOffset) && large.write(tail) == tail.size()
                        && large.flush(),
                "Cannot create large directory fixture");
        kobalt::Publication indexed;
        require(indexed.inspect(large, &error) && indexed.names() == publication.names(),
                "Large file-backed directory inspection failed");
        require(indexed.fingerprint() != publication.fingerprint(),
                "Fingerprint ignored the file length");
        require(large.resize(large.size() - 5) && !indexed.inspect(large, &error),
                "Truncated large directory accepted");
    }
    require(publication.finish(&error).isEmpty(), "Incomplete native publication accepted");
    require(!publication.append("wrong-session-resource", "data", &error),
            "Resource identity mismatch accepted");
    auto source =
            LVOpenArchieve(LVCreateMemoryStream(bytes.data(), bytes.size(), false, LVOM_READ));
    require(!source.isNull(), "Cannot open original fixture archive");
    const auto names = publication.names();
    for (const auto &name : names) {
        auto resource =
                source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
        require(!resource.isNull(), "Missing fixture resource");
        QByteArray content(int(resource->GetSize()), '\0');
        lvsize_t read = 0;
        require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                        && read == lvsize_t(content.size()),
                "Cannot read fixture resource");
        require(publication.append(name, content, &error), qPrintable(error));
    }
    const auto assembled = publication.finish(&error);
    require(!assembled.isEmpty(), qPrintable(error));
    require(publication.finish(&error).isEmpty(), "Publication finalized twice");
    auto invalidArchive = bytes;
    int firstDirectory = bytes.indexOf(QByteArray("PK\1\2", 4));
    int chapterName = bytes.indexOf("c1.xhtml", firstDirectory);
    require(chapterName >= 0, "Fixture chapter name missing");
    invalidArchive.replace(chapterName, 8, "../x.xml");
    require(!publication.inspect(invalidArchive, &error), "Traversal resource accepted");
    invalidArchive = bytes;
    chapterName = bytes.indexOf("c2.xhtml", firstDirectory);
    require(chapterName >= 0, "Second fixture chapter missing");
    invalidArchive.replace(chapterName, 8, "c1.xhtml");
    require(!publication.inspect(invalidArchive, &error), "Duplicate resource accepted");
    {
        // Kobalt must leave LCP-protected books to Nickel, whatever else it supports.
        QList<QPair<QByteArray, QByteArray>> members = {
            { "mimetype", "application/epub+zip" }, { "META-INF/container.xml", "<container/>" }
        };
        kobalt::Publication plain, protectedBook;
        require(plain.inspect(storedEpub(members), &error), qPrintable(error));
        members.append(qMakePair(QByteArray("META-INF/license.lcpl"), QByteArray("{}")));
        require(!protectedBook.inspect(storedEpub(members), &error),
                "LCP-protected publication accepted");
        require(error == kobalt::strings::get("error.this_publication_is_lcp_protected"),
                "LCP publication rejected for another reason");
    }
    require(!kobalt::EngineDocument::supportedArchive(QByteArray(100, 'x'), &error),
            "Invalid ZIP accepted");
    require(kobalt::EngineDocument::supportedArchive(bytes, &error), qPrintable(error));
    {
        // Empty or false EPUB media types must fail before an engine document
        // can be published through a resource provider.
        for (const auto &mediaType : { QByteArray(), QByteArray("application/epub+xml") }) {
            auto resources = std::make_shared<kobalt::ResourceCache>(
                    QStringList{ "mimetype" },
                    [mediaType](const QString &, const kobalt::ResourceCache::Sink &sink,
                                QString *) {
                        return sink(mediaType.constData(), mediaType.size());
                    },
                    std::make_shared<std::atomic_bool>(false));
            kobalt::EngineDocument rejected(kobalt::test::readerFonts());
            kobalt::OpeningResult result;
            error.clear();
            require(!rejected.openResources(resources, { { "mimetype", mediaType.size() } },
                                            QSize(600, 800), {}, &error, {}, &result)
                            && !result.ready()
                            && result.failure()->kind == kobalt::OpeningFailureKind::Unsupported
                            && !error.isEmpty() && rejected.pages() == 0
                            && rejected.image(0).isNull(),
                    "Non-EPUB media type opened through the resource provider");
        }
    }
    {
        auto unavailable = std::make_shared<kobalt::ResourceCache>(
                QStringList{ "mimetype" },
                [](const QString &, const kobalt::ResourceCache::Sink &, QString *failure) {
                    *failure = "Synthetic MIME timeout";
                    return false;
                },
                std::make_shared<std::atomic_bool>(false));
        kobalt::EngineDocument recovered(kobalt::test::readerFonts());
        kobalt::OpeningResult result;
        error.clear();
        require(!recovered.openResources(unavailable, { { "mimetype", 20 } }, QSize(600, 800), {},
                                         &error, {}, &result)
                        && !result.ready()
                        && result.failure()->kind == kobalt::OpeningFailureKind::Io
                        && result.failure()->reason == "opening.resource-read"
                        && result.failure()->detail == "Synthetic MIME timeout"
                        && error == result.failure()->detail && !result.rememberRejection()
                        && recovered.pages() == 0 && recovered.image(0).isNull(),
                "Unavailable media type was treated as unsupported content or lost its I/O detail");

        kobalt::Publication directory;
        require(directory.inspect(bytes, &error), qPrintable(error));
        auto available = std::make_shared<kobalt::ResourceCache>(
                directory.names(),
                [source](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                    auto stream = source->OpenStream(
                            Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
                    if (stream.isNull())
                        return false;
                    QByteArray content(int(stream->GetSize()), '\0');
                    lvsize_t read = 0;
                    return stream->Read(content.data(), content.size(), &read) == LVERR_OK
                            && read == lvsize_t(content.size())
                            && sink(content.constData(), content.size());
                },
                std::make_shared<std::atomic_bool>(false));
        error.clear();
        require(recovered.openResources(available, directory.sizes(), QSize(600, 800), {}, &error,
                                        {}, &result)
                        && result.ready() && !result.failure() && !result.rememberRejection()
                        && recovered.resourceError().isEmpty() && recovered.pages() > 0
                        && !recovered.image(0).isNull(),
                "Fresh resources could not reopen a model after media-type I/O failure");
    }
    auto corrupt = bytes;
    corrupt.truncate(corrupt.size() - 1);
    require(!kobalt::EngineDocument::supportedArchive(corrupt, &error), "Truncated ZIP accepted");
    corrupt = bytes;
    int central = corrupt.indexOf(QByteArray("PK\1\2", 4));
    require(central >= 0, "Missing test ZIP directory");
    corrupt[central + 8] = char(corrupt[central + 8] | 1);
    require(!kobalt::EngineDocument::supportedArchive(corrupt, &error), "Encrypted ZIP accepted");
    // Use current line-height units and a bundled comparison face on both firmware versions.
    // The boolean hyphenation value still exercises the legacy setting.
    const QJsonObject legacySettings = kobalt::qt::object({ { "font_face", "NV Bitter" },
                                                            { "font_pixels", 32 },
                                                            { "line_spacing", 120 },
                                                            { "line_spacing_version", 1 },
                                                            { "margin_pixels", 36 },
                                                            { "hyphenation", true } });
    for (int cycle = 0; cycle < 2; ++cycle) {
        auto book = std::make_unique<kobalt::EngineDocument>(kobalt::test::readerFonts());
        require(book->open(bytes, QSize(1072, 1328), legacySettings, &error), qPrintable(error));
        auto preview = std::make_unique<kobalt::EngineDocument>(kobalt::test::readerFonts());
        require(preview->openPreview(*book, QSize(1072, 1328), &error), qPrintable(error));
        book.reset();
        require(fontMan != nullptr && !preview->image(preview->page()).isNull(),
                "Closing a book invalidated its live preview fonts");
        preview.reset();
        require(fontMan == nullptr, "Last closed reader retained global font files");
    }
    {
        auto wrongType = bytes;
        const auto mime = wrongType.indexOf("application/epub+zip");
        require(mime >= 0, "Fixture does not have an uncompressed media type");
        wrongType[mime] = 'x';
        kobalt::EngineDocument invalid(kobalt::test::readerFonts());
        require(!invalid.open(wrongType, QSize(1072, 1328), {}, &error),
                "Container import accepted a non-EPUB media type");
    }
    kobalt::EngineDocument defaults(kobalt::test::readerFonts());
    QVector<QPair<kobalt::OpeningStage, int>> progress;
    require(defaults.open(bytes, QSize(1072, 1328), {}, &error,
                          [&](kobalt::OpeningStage stage, int percent) {
                              progress.append({ stage, percent });
                          }),
            qPrintable(error));
    require(!progress.isEmpty(), "Opening did not report engine progress");
    require(defaults.image(0).text("kobalt.animation_text_only") == "true",
            "Text page not eligible for animation");
    bool text = false, styles = false, pagination = false;
    auto previous = kobalt::OpeningStage::Text;
    for (const auto &step : progress) {
        require(step.second >= 0 && step.second <= 100, "Opening progress outside its range");
        require(int(step.first) >= int(previous), "Opening stages moved backwards");
        previous = step.first;
        text |= step.first == kobalt::OpeningStage::Text;
        styles |= step.first == kobalt::OpeningStage::Styles;
        pagination |= step.first == kobalt::OpeningStage::Pages;
    }
    std::printf("Opening callbacks: text=%d styles=%d pages=%d count=%d\n", text, styles,
                pagination, int(progress.size()));
    std::fflush(stdout);
    require(text && pagination, "Missing parsing or pagination progress");
    const auto progressCount = progress.size();
    require(defaults.fontFace() == "Libron" && defaults.fontSize() == 46
                    && defaults.spacing() == 150 && defaults.margin() == 48
                    && defaults.alignment() == "left"
                    && defaults.state().value("hyphenation").toString() == "off",
            "New books do not use the bundled typography defaults");
    {
        kobalt::readingPpi.store(212);
        kobalt::EngineDocument lowerDensity(kobalt::test::readerFonts());
        require(lowerDensity.open(bytes, QSize(758, 940), {}, &error), qPrintable(error));
        require(lowerDensity.state().value("top_margin_pixels").toInt() == kobalt::readingPixels(24)
                        && lowerDensity.state().value("bottom_margin_pixels").toInt()
                                == kobalt::readingPixels(24),
                "Fresh vertical margins changed when restoring untouched defaults at 212 ppi");
        require(lowerDensity.open(bytes, QSize(758, 940),
                                  kobalt::qt::object({ { "margin_pixels", 12 } }), &error),
                qPrintable(error));
        require(lowerDensity.state().value("top_margin_pixels").toInt() == 12
                        && lowerDensity.state().value("bottom_margin_pixels").toInt() == 12,
                "Older saved side margins did not cap default vertical margins");
        kobalt::readingPpi.store(300);
    }
    const auto defaultImage = hash(defaults.image(0));
    require(defaults.style(45, 121) && defaults.fontSize() == 45 && defaults.spacing() == 121,
            "Single-unit typography changes were rounded");
    kobalt::EngineDocument granular(kobalt::test::readerFonts());
    require(granular.open(bytes, QSize(600, 800), defaults.state(), &error), qPrintable(error));
    require(granular.fontSize() == 45 && granular.spacing() == 121,
            "Single-unit typography was not restored");
    require(defaults.style(kobalt::halfPointPixels(80), 121), "Cannot select 40 point text");
    const auto largeImage = hash(defaults.image(0));
    require(defaults.style(kobalt::maximumFontPixels(), 121)
                    && hash(defaults.image(0)) != largeImage,
            "48 point text rendered at the same size as 40 point text");
    require(granular.open(bytes, QSize(600, 800), defaults.state(), &error)
                    && granular.fontSize() == kobalt::maximumFontPixels(),
            "Maximum text size was lost on reopening");
    require(defaults.style(44, 120), "Cannot restore defaults after granular typography check");
    require(defaults.style(44, 120, "NV Bitter", 48) && hash(defaults.image(0)) != defaultImage,
            "Bundled Libron did not change rendered text from bundled NV Bitter");
    require(progress.size() == progressCount, "Opening callback survived into later reflow");
    require(defaults.open(bytes, QSize(1072, 1328), legacySettings, &error)
                    && defaults.fontFace() == "NV Bitter" && defaults.fontSize() == 32
                    && defaults.margin() == 36
                    && defaults.state().value("hyphenation").toString() == "auto",
            "New defaults replaced saved preferences");
    {
        auto archive =
                LVOpenArchieve(LVCreateMemoryStream(bytes.data(), bytes.size(), false, LVOM_READ));
        kobalt::Publication directory;
        require(directory.inspect(bytes, &error), qPrintable(error));
        auto loader = [archive](const QString &name, const kobalt::ResourceCache::Sink &sink,
                                QString *) {
            auto stream = archive->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(),
                                              LVOM_READ);
            if (stream.isNull())
                return false;
            std::array<char, 65536> block;
            while (!stream->Eof()) {
                lvsize_t read = 0;
                if (stream->Read(block.data(), block.size(), &read) != LVERR_OK || !read
                    || !sink(block.data(), read))
                    return false;
            }
            return true;
        };
        auto oldAccess = std::make_shared<std::atomic_bool>(false);
        auto source = std::make_shared<kobalt::ResourceCache>(directory.names(), loader, oldAccess);
        int freshReads = 0;
        auto fresh = std::make_shared<kobalt::ResourceCache>(
                directory.names(),
                [loader, &freshReads](const QString &name, const kobalt::ResourceCache::Sink &sink,
                                      QString *error) {
                    ++freshReads;
                    return loader(name, sink, error);
                },
                std::make_shared<std::atomic_bool>(false));
        kobalt::EngineDocument lazy(kobalt::test::readerFonts());
        require(lazy.openResources(source, directory.sizes(), QSize(1072, 1328), legacySettings,
                                   &error),
                qPrintable(error));
        require(lazy.pages() == defaults.pages() && hash(lazy.image(0)) == hash(defaults.image(0)),
                "On-demand resources changed pagination or rendered pixels");
        {
            kobalt::EngineDocument preview(kobalt::test::readerFonts());
            require(preview.openPreview(lazy, QSize(1072, 1328), &error), qPrintable(error));
            require(hash(preview.image(0)) == hash(lazy.image(0)),
                    "Resource-backed preview changed rendered pixels");
            require(!lazy.rebindResources(fresh, directory.sizes()),
                    "Live preview allowed its parent access to change");
        }
        oldAccess->store(true);
        require(lazy.rebindResources(fresh, directory.sizes()),
                "Drained source could not bind fresh access");
        {
            kobalt::EngineDocument preview(kobalt::test::readerFonts());
            require(preview.openPreview(lazy, QSize(1072, 1328), &error), qPrintable(error));
            require(freshReads > 0 && hash(preview.image(0)) == hash(lazy.image(0)),
                    "Retained container or preview read its old cancelled source");
        }
        require(lazy.resourceStats()["peak_bytes"].toDouble() <= kobalt::ResourceCache::budgetBytes,
                "Model exceeded source cache budget");
    }
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    require(model.open(bytes, QSize(1072, 1328), legacySettings, &error), qPrintable(error));
    {
        QImage pattern(2, 2, QImage::Format_RGB32);
        pattern.fill(Qt::white);
        pattern.setPixel(0, 0, qRgb(0, 0, 0));
        pattern.setPixel(1, 1, qRgb(0, 0, 0));
        QByteArray png;
        QBuffer buffer(&png);
        require(buffer.open(QIODevice::WriteOnly) && pattern.save(&buffer, "PNG"),
                "Cannot encode scaling fixture");
        const QMap<QString, QByteArray> data = {
            { "mimetype", "application/epub+zip" },
            { "META-INF/container.xml",
              "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\"><rootfiles><rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>" },
            { "book.opf",
              "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\"><metadata/><manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"i\" href=\"pattern.png\" media-type=\"image/png\"/></manifest><spine><itemref idref=\"c\"/></spine></package>" },
            { "c.xhtml",
              "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Image scaling</title></head><body><img src=\"pattern.png\" width=\"157\" height=\"157\"/></body></html>" },
            { "pattern.png", png }
        };
        auto resources = std::make_shared<kobalt::ResourceCache>(
                data.keys(),
                [&data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                    const auto bytes = data.value(name);
                    return sink(bytes.constData(), bytes.size());
                },
                std::make_shared<std::atomic_bool>(false));
        QMap<QString, qint64> sizes;
        for (auto it = data.begin(); it != data.end(); ++it)
            sizes[it.key()] = it.value().size();
        kobalt::EngineDocument scaled(kobalt::test::readerFonts());
        require(scaled.openResources(resources, sizes, QSize(600, 800), {}, &error),
                qPrintable(error));
        require(!scaled.state().contains("smooth_images"),
                "Retired smoothing option was persisted");
        require(scaled.image(0).text("kobalt.animation_text_only") == "false",
                "Grayscale image page eligible for animation");
        const auto smooth = scaled.image(0);
        const auto position = scaled.position();
        const auto pages = scaled.pages();
        const auto cached = scaled.cachedPages();
        require(scaled.applyAppearance(kobalt::qt::object({ { "smooth_images", false } }))
                        && scaled.cachedPages() == cached && hash(scaled.image(0)) == hash(smooth),
                "Retired smoothing option changed output or discarded cached pages");
        auto grayPixels = [](const QImage &image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const int gray = qGray(image.pixel(x, y));
                    count += gray > 10 && gray < 245;
                }
            return count;
        };
        require(grayPixels(smooth) > 100, "Image smoothing did not interpolate the enlarged image");
        require(scaled.pages() == pages && scaled.position() == position,
                "Image smoothing changed layout or position");
        kobalt::EngineDocument preview(kobalt::test::readerFonts()),
                reopened(kobalt::test::readerFonts());
        require(preview.openPreview(scaled, QSize(600, 800), &error)
                        && hash(preview.image(0)) == hash(smooth),
                "Preview lost image smoothing");
        require(reopened.openResources(resources, sizes, QSize(600, 800),
                                       kobalt::qt::object({ { "smooth_images", false } }), &error)
                        && hash(reopened.image(0)) == hash(smooth),
                "An old saved false value disabled image smoothing");
        require(scaled.applyAppearance(kobalt::qt::object({ { "smooth_images", true } }))
                        && hash(scaled.image(0)) == hash(smooth),
                "Retired smoothing value changed its output");
    }
    {
        auto placeholder = kobalt::imagePlaceholder(QSize(8000, 8000));
        auto image = LVCreateStreamImageSource(
                LVCreateMemoryStream(placeholder.data(), placeholder.size(), false, LVOM_READ));
        require(!image.isNull() && image->IsScalable() && image->GetWidth() == 8000
                        && image->GetHeight() == 8000,
                "Placeholder lost original image dimensions or vector rendering");
        LVColorDrawBuf pixels(300, 300, 32);
        pixels.Clear(0xffffff);
        pixels.Draw(image, 0, 0, 300, 300, false);
        require((pixels.GetPixel(50, 100) & 0xffffff) == 0xeeeeee,
                "Placeholder background is not light gray");
        require(qGray(pixels.GetPixel(0, 150)) < 100, "Placeholder border is missing");
        int ink = 0;
        for (int y = 135; y < 165; ++y)
            for (int x = 10; x < 290; ++x)
                ink += qGray(pixels.GetPixel(x, y)) < 100;
        require(ink > 100, "Image placeholder did not render its centered label");
    }
    {
        kobalt::EngineDocument separate(kobalt::test::readerFonts()),
                combined(kobalt::test::readerFonts());
        require(separate.open(bytes, QSize(1072, 1328), legacySettings, &error)
                        && combined.open(bytes, QSize(1072, 1328), legacySettings, &error),
                qPrintable(error));
        require(separate.go(3) && combined.goTo(separate.position()),
                "Cannot position appearance comparison");
        const auto anchor = separate.position();
        const QJsonObject appearance = kobalt::qt::object({ { "font_face", "Libron" },
                                                            { "font_pixels", 42 },
                                                            { "line_spacing", 140 },
                                                            { "margin_pixels", 60 },
                                                            { "text_alignment", "justify" },
                                                            { "weight_steps", 3 },
                                                            { "hyphenation", "on" },
                                                            { "dark_mode", true } });
        QElapsedTimer timer;
        timer.start();
        require(separate.style(42, 140, "Libron", 60, "justify") && separate.setWeight(3)
                        && separate.setHyphenation("on"),
                "Cannot apply separate appearance changes");
        separate.setDarkMode(true);
        separate.resize(QSize(800, 1000));
        const auto separateMs = timer.elapsed();
        timer.restart();
        require(combined.applyAppearance(appearance, QSize(800, 1000)),
                "Cannot apply combined appearance changes");
        const auto combinedMs = timer.elapsed();
        require(separate.state() == combined.state() && combined.position() == anchor
                        && separate.pages() == combined.pages()
                        && separate.page() == combined.page(),
                "Combined appearance changed pagination, preferences or the text anchor");
        for (int page : { combined.page() - 1, combined.page(), combined.page() + 1 })
            require(hash(separate.image(page)) == hash(combined.image(page)),
                    "Combined appearance changed page pixels");
        const auto saved = combined.state();
        const int cached = combined.cachedPages();
        auto invalid = appearance;
        invalid["hyphenation"] = QStringLiteral("invalid");
        invalid["font_pixels"] = 48;
        require(!combined.applyAppearance(invalid, QSize(900, 1100)) && combined.state() == saved
                        && combined.cachedPages() == cached,
                "Invalid appearance partially changed the book");
        require(combined.applyAppearance(appearance, QSize(800, 1000)) && combined.state() == saved
                        && combined.cachedPages() == cached,
                "Unchanged appearance invalidated the page cache");
        std::printf("Appearance comparison: separate=%lld ms, combined=%lld ms\n",
                    static_cast<long long>(separateMs), static_cast<long long>(combinedMs));
    }
    {
        QFile colorFile(QString::fromLocal8Bit(argv[2]));
        require(colorFile.open(QIODevice::ReadOnly), "Cannot read color fixture");
        kobalt::EngineDocument color(kobalt::test::readerFonts());
        const auto colorBytes = colorFile.readAll();
        require(color.open(colorBytes, QSize(1072, 1328), legacySettings, &error),
                qPrintable(error));
        {
            kobalt::EngineDocument retainedPreview(kobalt::test::readerFonts());
            QImage original;
            {
                kobalt::EngineDocument closingSource(kobalt::test::readerFonts());
                require(closingSource.open(colorBytes, QSize(1072, 1328), legacySettings, &error),
                        qPrintable(error));
                original = closingSource.image(0);
                require(retainedPreview.openPreview(closingSource, QSize(1072, 1328), &error),
                        qPrintable(error));
            }
            // Force rendering and resource reads after the original model dies.
            require(retainedPreview.style(34, 130) && retainedPreview.style(32, 120),
                    "Retained preview could not reflow");
            require(hash(retainedPreview.image(0)) == hash(original),
                    "Preview lost shared images or CSS when its source closed");
        }
        const auto colorPage = color.image(0);
        require(colorPage.text("kobalt.animation_text_only") == "false",
                "Colour page eligible for animation");
        require(colorPage.format() == QImage::Format_RGB32,
                "Book colors were converted to grayscale");
        QRegion colorRepairs;
        const auto metadata =
                QJsonDocument::fromJson(colorPage.text("kobalt.colour_regions").toUtf8());
        require(metadata.isArray(), "Colour repair metadata is missing");
        for (const auto &entry : metadata.array()) {
            const auto rectangle = entry.toArray();
            require(rectangle.size() == 4, "Colour repair rectangle is invalid");
            colorRepairs += QRect(rectangle[0].toInt(), rectangle[1].toInt(), rectangle[2].toInt(),
                                  rectangle[3].toInt());
        }
        int red = 0, green = 0, blue = 0, yellow = 0, blended = 0, magenta = 0, teal = 0,
            orange = 0;
        int svgGreen = 0, svgBlue = 0;
        QRect highlight;
        for (int y = 0; y < colorPage.height(); ++y) {
            for (int x = 0; x < colorPage.width(); ++x) {
                const auto pixel = colorPage.pixel(x, y);
                require(qAlpha(pixel) == 255, "Page has invalid opaque alpha");
                if (qRed(pixel) != qGreen(pixel) || qGreen(pixel) != qBlue(pixel))
                    require(colorRepairs.contains(QPoint(x, y)),
                            "Colour repair omitted a faint edge pixel");
                red += pixel == qRgb(255, 0, 0);
                green += pixel == qRgb(0, 192, 0);
                blue += pixel == qRgb(0, 0, 255);
                // SVG premultiplied-alpha conversion rounds opaque channels down by one.
                svgGreen += pixel == qRgb(0, 191, 0);
                svgBlue += pixel == qRgb(0, 0, 254);
                magenta += pixel == qRgb(255, 0, 255);
                teal += pixel == qRgb(0, 128, 128);
                orange += pixel == qRgb(255, 128, 0);
                yellow += pixel == qRgb(255, 255, 0);
                if (pixel == qRgb(255, 255, 0))
                    highlight |= QRect(x, y, 1, 1);
                blended += qAbs(qRed(pixel) - 127) <= 2 && qAbs(qGreen(pixel) - 223) <= 2
                        && qAbs(qBlue(pixel) - 127) <= 2;
            }
        }
        require(red > 50 && yellow > 200, "Publisher text or background colors were lost");
        // CSS pixels scale at the device density; the wider SVG also fits to the page.
        require(green == 19404 && blue == 19404,
                "Direct PNG colors or intrinsic dimensions were lost");
        require(svgGreen == 13646 && svgBlue == 13728,
                "SVG-relative PNG colors or dimensions were lost");
        require(magenta > 3000 && teal > 50 && orange > 1000,
                "SVG shape, text or inline SVG colors were lost");
        require(blended > 1000, "PNG transparency was not composited onto the page");
        require(color.go(0), "Cannot select color section");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(color, QSize(1072, 1328), &error), qPrintable(error));
        require(hash(preview.image(0)) == hash(colorPage), "Preview changed publisher colors");
        const auto position = color.position();
        const auto pages = color.pages();
        color.setDarkMode(true);
        require(color.cachedPages() == 0, "Dark mode kept stale day pages");
        const auto night = color.image(0);
        int black = 0, nightRed = 0, nightGreen = 0, nightBlue = 0, nightBlend = 0,
            nightMagenta = 0, nightTeal = 0, nightOrange = 0;
        int nightSvgGreen = 0, nightSvgBlue = 0;
        for (int y = 0; y < night.height(); ++y)
            for (int x = 0; x < night.width(); ++x) {
                const auto pixel = night.pixel(x, y);
                require(qAlpha(pixel) == 255, "Dark page has invalid opaque alpha");
                black += pixel == qRgb(0, 0, 0);
                nightRed += pixel == qRgb(255, 0, 0);
                nightGreen += pixel == qRgb(0, 192, 0);
                nightBlue += pixel == qRgb(0, 0, 255);
                nightSvgGreen += pixel == qRgb(0, 191, 0);
                nightSvgBlue += pixel == qRgb(0, 0, 254);
                nightMagenta += pixel == qRgb(255, 0, 255);
                nightTeal += pixel == qRgb(0, 128, 128);
                nightOrange += pixel == qRgb(255, 128, 0);
                nightBlend +=
                        qRed(pixel) <= 2 && qAbs(qGreen(pixel) - 96) <= 2 && qBlue(pixel) <= 2;
            }
        require(black > night.width() * night.height() / 2,
                "Dark mode did not change the paper to black");
        require(nightRed == red && nightGreen == green && nightBlue == blue,
                "Dark mode changed publisher or image colors");
        require(nightSvgGreen == 13448 && nightSvgBlue == 13530,
                "Dark-mode SVG image colors or dimensions changed");
        require(nightMagenta == magenta && nightTeal == teal && nightOrange == orange,
                "Dark mode changed SVG colors");
        require(nightBlend > 1000, "Transparent image did not composite onto black paper");
        int maxHighlightDifference = 0;
        for (int y = highlight.top(); y <= highlight.bottom(); ++y)
            for (int x = highlight.left(); x <= highlight.right(); ++x) {
                const auto before = colorPage.pixel(x, y), after = night.pixel(x, y);
                const int difference = qMax(qAbs(qRed(before) - qRed(after)),
                                            qMax(qAbs(qGreen(before) - qGreen(after)),
                                                 qAbs(qBlue(before) - qBlue(after))));
                maxHighlightDifference = qMax(maxHighlightDifference, difference);
            }
        // The engine's integer glyph blend can round by two levels when inverted.
        require(maxHighlightDifference <= 2, "Dark mode reduced contrast on a publisher highlight");
        require(color.pages() == pages && color.position() == position,
                "Dark mode moved the text or changed pagination");
        kobalt::EngineDocument darkPreview(kobalt::test::readerFonts()),
                reopened(kobalt::test::readerFonts());
        require(darkPreview.openPreview(color, QSize(1072, 1328), &error), qPrintable(error));
        require(darkPreview.state().value("dark_mode").toBool()
                        && hash(darkPreview.image(0)) == hash(night),
                "Section preview did not inherit dark mode");
        require(reopened.open(colorBytes, QSize(1072, 1328), color.state(), &error),
                qPrintable(error));
        require(hash(reopened.image(0)) == hash(night), "Reopening lost dark mode");
        color.setDarkMode(false);
        require(hash(color.image(0)) == hash(colorPage), "Returning to day mode changed the page");
    }
    require(fontMan->GetHintingMode() == HINTING_MODE_DISABLED, "Book fonts still use hinting");
    kobalt::EngineDocument transported(kobalt::test::readerFonts());
    require(transported.open(assembled, QSize(1072, 1328), legacySettings, &error),
            qPrintable(error));
    require(transported.pages() == model.pages()
                    && hash(transported.image(0)) == hash(model.image(0)),
            "Native resource assembly changed the publication");
    require(model.pages() > 6 && model.chapters().size() == 3, "Missing pages or chapters");
    auto checkChapterProgress = [&] {
        const auto chapters = model.chapters();
        for (int i = 0; i < chapters.size(); ++i) {
            require(model.goTo(chapters[i].position), "Cannot open chapter for progress check");
            const int start = model.page();
            const int end = i + 1 < chapters.size() ? chapters[i + 1].page : model.pages();
            const auto anchor = model.position();
            require(chapters[i].page == start && end > start, "Invalid chapter page boundaries");
            require(model.chapterProgress().title == chapters[i].title
                            && model.chapterProgress().page == 1
                            && model.chapterProgress().pages == end - start,
                    "Wrong chapter start progress");
            require(model.position() == anchor, "Measuring chapter progress moved the reader");
            require(model.go(end - 1) && model.chapterProgress().title == chapters[i].title
                            && model.chapterProgress().page == end - start
                            && model.chapterProgress().pages == end - start,
                    "Wrong final chapter page number");
        }
        require(model.go(0), "Cannot return after chapter progress check");
    };
    checkChapterProgress();
    require(model.style(42, 140), "Chapter progress reflow failed");
    checkChapterProgress();
    require(model.style(32, 120), "Cannot restore initial typography");
    model.resize(QSize(1072, 900));
    checkChapterProgress();
    model.resize(QSize(1072, 1328));
    checkChapterProgress();
    QString chapter2;
    for (const auto &name : names)
        if (name.endsWith("c2.xhtml"))
            chapter2 = name;
    {
        // CREngine normalizes heading whitespace. Native text-offset mapping
        // rejects that change even at offset zero, but the EPUB TOC still works.
        for (const auto &heading : { QByteArray("Chapter One"), QByteArray("Chapter\n   One") }) {
            const auto book = storedEpub(
                    { { "mimetype", "application/epub+zip" },
                      { "META-INF/container.xml",
                        "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                      { "book.opf",
                        "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata/><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/><item id='toc' href='toc.ncx' media-type='application/x-dtbncx+xml'/></manifest><spine toc='toc'><itemref idref='c'/></spine></package>" },
                      { "toc.ncx",
                        "<ncx xmlns='http://www.daisy.org/z3986/2005/ncx/' version='2005-1'><head/><docTitle><text>Book</text></docTitle><navMap><navPoint id='n1' playOrder='1'><navLabel><text>EPUB chapter</text></navLabel><content src='c.xhtml#heading'/></navPoint></navMap></ncx>" },
                      { "c.xhtml",
                        "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Test</title></head><body><h1 id='heading'>"
                                + heading + "</h1><p>Body text.</p></body></html>" } });
            kobalt::EngineDocument fallback(kobalt::test::readerFonts());
            require(fallback.open(book, QSize(1072, 1328), {}, &error), qPrintable(error));
            const auto original = fallback.chapters();
            require(original.size() == 1 && !original[0].position.isEmpty(),
                    "Missing EPUB TOC destination");
            const auto anchor = fallback.position();
            for (const auto &location :
                 { QString("c.xhtml#heading"), QString("c.xhtml#heading:0") }) {
                auto resolved = kobalt::resolveChapterTargets(
                        fallback, positions, { { "Native chapter", location } }, {});
                const bool normalized = heading.contains('\n');
                require(resolved.contents.size() == 1
                                && resolved.contents[0].position.isEmpty() == normalized,
                        "Heading whitespace did not reproduce the native destination failure");
                const QVector<kobalt::ChapterLink> graph{ { "Native graph", original[0].position,
                                                            -1, 0, 123 } };
                fallback.setChapters(std::move(resolved.contents), graph);
                const auto chapters = fallback.chapters();
                require(chapters.size() == 1
                                && chapters[0].title
                                        == (normalized ? "EPUB chapter" : "Native chapter")
                                && !chapters[0].position.isEmpty() && chapters[0].page >= 0
                                && fallback.position() == anchor,
                        "Unresolved native chapters did not fall back without moving the reader");
                require(fallback.chapterGraph().size() == 1
                                && fallback.chapterGraph()[0].words == 123,
                        "TOC fallback replaced native word counts");
                require(fallback.goTo(chapters[0].position),
                        "Cannot navigate to the selected chapter destination");
                require(fallback.goTo(anchor),
                        "Cannot restore the position after chapter navigation");
            }
            if (heading.contains('\n')) {
                require(fallback.platformPosition(positions, &error) == "c.xhtml" && error.isEmpty()
                                && fallback.position() == anchor,
                        "Normalized heading prevented chapter-only native progress");
            }
        }
    }
    {
        // A plain EPUB without element IDs. Nickel stores its position as a DOM
        // point that counts every child from <html>, text nodes included, with
        // a UTF-8 byte offset, as firmware 4.46 and 6.0 do for this layout.
        const auto plain = storedEpub(
                { { "mimetype", "application/epub+zip" },
                  { "META-INF/container.xml",
                    "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                  { "book.opf",
                    "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata/><manifest><item id='c' href='text/c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
                  { "text/c.xhtml",
                    "<?xml version='1.0' encoding='utf-8'?>\n<html xmlns='http://www.w3.org/1999/xhtml'>\n<head>\n<title>Plain</title>\n</head>\n"
                    "<body>\n<h1>Kapitel</h1>\n<p>Erste Zeile.</p>\n<p>Zweite <em>Gr\xc3\xb6\xc3\x9f"
                    "e</em> Zeile.</p>\n</body>\n</html>" } });
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(plain, QSize(1072, 1328), {}, &error), qPrintable(error));
        // Paragraph start, inside the inline element after a two-byte letter,
        // and inside the text after it.
        for (const auto &point :
             { "point(/1/4/6/1:0)", "point(/1/4/6/2/1:4)", "point(/1/4/6/3:1)" }) {
            const auto location = QStringLiteral("text/c.xhtml#") + point;
            require(model.goToPlatform(positions, location, &error),
                    qPrintable(location + ": " + error));
            require(model.platformPosition(positions, &error) == location && error.isEmpty(),
                    qPrintable("Plain EPUB position did not round-trip: " + location + " -> "
                               + model.platformPosition(positions, &error)));
        }
        require(model.goToPlatform(positions, "text/c.xhtml#point(/1/4/4/1:6)", &error)
                        && model.position().endsWith(".6"),
                "Plain EPUB point mapped to the wrong text offset");
        require(!model.goToPlatform(positions, "text/c.xhtml#point(/1/4/6/2/1:3)", &error),
                "A byte offset inside a UTF-8 character was accepted");
        require(!model.goToPlatform(positions, "text/c.xhtml#point(/1/4/99/1:0)", &error),
                "An out-of-range plain EPUB point was accepted");
    }
    for (const bool styled : { false, true }) {
        // A plain EPUB whose chapter is one <section> with an ID and no IDs on
        // its paragraphs. CREngine drops the whitespace between the section's
        // children, so the section's text differs from the source. Nickel
        // still stores a DOM point into the paragraph. Chapter styles add
        // CREngine's own <stylesheet> element beside <body>.
        const QByteArray style = styled ? "\t\t<style>p { text-indent: 1em; }</style>\n" : "";
        const auto sectioned = storedEpub(
                { { "mimetype", "application/epub+zip" },
                  { "META-INF/container.xml",
                    "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                  { "book.opf",
                    "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata/><manifest><item id='c' href='text/c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
                  { "text/c.xhtml",
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"en-GB\" xml:lang=\"en-GB\">\n"
                    "\t<head>\n\t\t<title>I</title>\n"
                            + style
                            + "\t</head>\n"
                              "\t<body epub:type=\"bodymatter z3998:fiction\">\n"
                              "\t\t<section id=\"chapter-1\" role=\"doc-chapter\" epub:type=\"chapter\">\n"
                              "\t\t\t<h2 epub:type=\"z3998:ordinal z3998:roman\">I</h2>\n"
                              "\t\t\t<p>The first paragraph.</p>\n"
                              "\t\t\t<p>\xe2\x80\x9cYou must follow me carefully.\xe2\x80\x9d</p>\n"
                              "\t\t\t<p>Is not <i epub:type=\"se:name\">that</i> rather large?</p>\n"
                              "\t\t</section>\n\t</body>\n</html>\n" } });
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(sectioned, QSize(1072, 1328), {}, &error), qPrintable(error));
        // Heading and paragraph starts, after a three-byte quotation mark,
        // inside the inline element and in the text after it.
        for (const auto &point :
             { "point(/1/4/2/2/1:0)", "point(/1/4/2/4/1:0)", "point(/1/4/2/6/1:7)",
               "point(/1/4/2/8/2/1:2)", "point(/1/4/2/8/3:1)" }) {
            const auto location = QStringLiteral("text/c.xhtml#") + point;
            require(model.goToPlatform(positions, location, &error),
                    qPrintable(location + ": " + error));
            require(model.platformPosition(positions, &error) == location && error.isEmpty(),
                    qPrintable("Sectioned plain EPUB position did not round-trip: " + location
                               + " -> " + model.platformPosition(positions, &error)));
        }
        require(model.goToPlatform(positions, "text/c.xhtml#point(/1/4/2/6/1:7)", &error)
                        && model.position().endsWith(".5"),
                "Sectioned plain EPUB point mapped to the wrong text offset");
        require(!model.goToPlatform(positions, "text/c.xhtml#point(/1/4/2/6/1:1)", &error),
                "A byte offset inside a UTF-8 character was accepted");
    }
    {
        // A KEPUB-shaped chapter: book-columns and book-inner around the text,
        // a kobo span in one paragraph and none in the other. Nickel's KEPUB
        // reader is not known to open a point(), so a position outside the
        // spans keeps the chapter start, as before. Span positions are exact.
        const auto kepub = storedEpub(
                { { "mimetype", "application/epub+zip" },
                  { "META-INF/container.xml",
                    "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                  { "book.opf",
                    "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata/><manifest><item id='c' href='text/c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
                  { "text/c.xhtml",
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
                    "\t<head>\n\t\t<title>I</title>\n\t\t<style>p { text-indent: 1em; }</style>\n\t</head>\n"
                    "\t<body>\n"
                    "\t\t<div id=\"book-columns\">\n\t\t\t<div id=\"book-inner\">\n"
                    "\t\t\t\t<p>Plain text outside a span.</p>\n"
                    "\t\t\t\t<p><span class=\"koboSpan\" id=\"kobo.2.1\">Text inside a span.</span></p>\n"
                    "\t\t\t</div>\n\t\t</div>\n\t</body>\n</html>\n" } });
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(kepub, QSize(1072, 1328), {}, &error), qPrintable(error));
        require(model.goToPlatform(positions, "text/c.xhtml#point(/1/4/2/2/2/1:6)", &error),
                qPrintable(error));
        require(model.platformPosition(positions, &error) == "text/c.xhtml" && error.isEmpty(),
                qPrintable("A KEPUB position outside a kobo span wrote a point: "
                           + model.platformPosition(positions, &error)));
        require(model.goToPlatform(positions, "text/c.xhtml#kobo.2.1:5", &error),
                qPrintable(error));
        require(model.platformPosition(positions, &error) == "text/c.xhtml#kobo.2.1:5",
                "A KEPUB span position lost its exact form");
    }
    kobalt::EngineDocument native(kobalt::test::readerFonts());
    require(native.open(bytes, QSize(1072, 1328), legacySettings, &error), qPrintable(error));
    require(native.goToPlatform(positions, chapter2 + "#kobo.8.1:7", &error), qPrintable(error));
    require(native.position().contains("/DocFragment[2]/") && native.position().endsWith(".7"),
            "Native chapter, element or offset mapped to the wrong text");
    require(native.platformPosition(positions, &error) == chapter2 + "#kobo.8.1:7",
            "Native position export lost the source offset");
    {
        const auto before = native.position();
        require(!native.goToPlatform({}, before, &error) && !error.isEmpty()
                        && native.position() == before,
                "Missing codec treated a platform location as an engine anchor");
        require(native.goToPlatformNearest({}, chapter2, &error)
                                == kobalt::EngineDocument::PositionTarget::Unavailable
                        && native.position() == before,
                "Missing codec changed position through nearest fallback");
        require(native.platformPosition({}, &error).isEmpty() && !error.isEmpty(),
                "Missing codec exported a platform location");
    }

    for (const auto &point : { "point(/1/2/8/1/1:7)", "point(/1/2/8:7)" }) {
        require(native.goToPlatform(positions, chapter2 + '#' + point, &error), qPrintable(error));
        require(native.platformPosition(positions, &error) == chapter2 + "#kobo.8.1:7",
                "Native DOM point changed the source position");
    }
    require(!native.goToPlatform(positions, chapter2 + "#point(/1/2/999:0)", &error),
            "Out-of-range native DOM child was accepted");
    const auto comparisonAnchor = native.position();
    require(native.comparePlatformPosition(positions, chapter2 + "#kobo.8.1:6", &error) < 0
                    && error.isEmpty(),
            "Earlier source offset must compare before");
    require(native.comparePlatformPosition(positions, chapter2 + "#kobo.8.1:8", &error) > 0
                    && error.isEmpty(),
            "Later source offset must compare beyond");
    require(native.comparePlatformPosition(positions, chapter2 + "#kobo.8.1:7", &error) == 0
                    && error.isEmpty(),
            "Equal source location must compare equal");
    require(native.comparePlatformPosition(positions, chapter2, &error) < 0 && error.isEmpty(),
            "Chapter start must compare before its text");
    native.comparePlatformPosition(positions, chapter2 + "#missing:0", &error);
    require(!error.isEmpty() && native.position() == comparisonAnchor,
            "Comparison must reject unknown IDs without moving");
    {
        const QVector<kobalt::ChapterTarget> targets{
            { "Whole section", chapter2, 0, 300 },
            { "Chapter two", chapter2, 0, 100 },
            { "First part", chapter2 + "#kobo.8.1:6", 1, 120 },
            { "Second part", chapter2 + "#kobo.8.1:8", 1, 80 },
            { "Missing part", chapter2 + "#missing:0", 1, 50 }
        };
        const int beforePage = native.page(), beforePages = native.pages();
        auto mapped = kobalt::resolveChapterTargets(native, positions, targets, targets);
        native.setChapters(std::move(mapped.contents), std::move(mapped.graph));
        auto links = native.chapterGraph();
        require(native.chapters().size() == targets.size()
                        && native.chapters().last().position.isEmpty(),
                "A partially resolved native chapter list was replaced by the publication TOC");
        require(native.position() == comparisonAnchor && native.page() == beforePage
                        && native.pages() == beforePages,
                "Mapping chapters changed the reading position or layout");
        require(links.size() == 5 && links[0].position == links[1].position && links[0].words == 300
                        && links[2].depth == 1 && links[2].words == 120
                        && links[4].position.isEmpty() && links[4].page == -1,
                "Chapter mapping lost duplicates, counts, depth or an unresolved destination");
        require(links[2].page == links[3].page,
                "Adjacent source offsets should share a rendered page");
        auto selected = native.chapterSelection(links);
        require(selected.current == 2 && selected.next == 3 && selected.page == 1
                        && selected.pages == 1,
                "Chapter selection used page numbers instead of exact source positions");
        require(native.chapterProgress().title == "First part",
                "Header and contents selected different chapters");
        require(native.goToPlatform(positions, chapter2, &error), qPrintable(error));
        require(native.chapterSelection(native.chapterGraph()).current == 1,
                "Duplicate chapter destinations lost their order");
        require(native.goToPlatform(positions, chapter2 + "#kobo.8.1:8", &error),
                qPrintable(error));
        require(native.chapterSelection(native.chapterGraph()).current == 3,
                "Exact chapter boundary selected the previous chapter");
        const auto mappedAnchor = native.position();
        {
            // A copy with its own storage takes the full scan. The cached
            // search must choose the same entries at every position.
            const auto sameSelection = [&native](const QVector<kobalt::ChapterLink> &cached) {
                QVector<kobalt::ChapterLink> scanned;
                for (const auto &link : cached)
                    scanned.append(link);
                const auto fast = native.chapterSelection(cached),
                           full = native.chapterSelection(scanned);
                return fast.current == full.current && fast.next == full.next
                        && fast.page == full.page && fast.pages == full.pages;
            };
            for (const auto &location : { chapter2, chapter2 + "#kobo.8.1:6",
                                          chapter2 + "#kobo.8.1:7", chapter2 + "#kobo.8.1:8" }) {
                require(native.goToPlatform(positions, location, &error), qPrintable(error));
                require(sameSelection(native.chapterGraph()) && sameSelection(native.chapters()),
                        "Cached chapter search disagreed at a boundary");
            }
            for (int page = 0; page < native.pages(); ++page) {
                require(native.go(page), "Cannot turn to compare chapter searches");
                require(sameSelection(native.chapterGraph()) && sameSelection(native.chapters()),
                        "Cached chapter search disagreed with a full scan");
            }
            // An element position with a child offset compares before every
            // position inside that element, and those positions can compare
            // before it too. Such lists must not take the sorted search.
            const QString body = "/body[1]/DocFragment[2]/body[1]";
            QVector<kobalt::ChapterLink> mixed{
                { "Offset five", body + ".5" },
                { "Early text", body + "/p[1]/span[1]/text()[1].0" },
                { "Offset twenty-five", body + ".25" },
                { "Late text", body + "/p[20]/span[1]/text()[1].0" }
            };
            const auto mappedContents = native.chapters(), mappedGraph = native.chapterGraph();
            std::sort(mixed.begin(), mixed.end(),
                      [](const auto &a, const auto &b) { return a.title < b.title; });
            do {
                native.setChapters(mixed, mixed);
                for (const auto &link : native.chapterGraph())
                    require(link.page >= 0, "Offset chapter fixture did not resolve");
                for (int page = 0; page < native.pages(); ++page) {
                    require(native.go(page), "Cannot turn to compare offset chapter searches");
                    require(sameSelection(native.chapterGraph())
                                    && sameSelection(native.chapters()),
                            "Cached chapter search disagreed with offset positions");
                }
            } while (std::next_permutation(
                    mixed.begin(), mixed.end(),
                    [](const auto &a, const auto &b) { return a.title < b.title; }));
            native.setChapters(mappedContents, mappedGraph);
            require(native.goTo(mappedAnchor), "Cannot return after comparing chapter searches");
        }
        require(native.style(56, 160), "Cannot reflow chapter mappings");
        native.resize(QSize(1448, 920));
        links = native.chapterGraph();
        require(native.position() == mappedAnchor && native.chapterSelection(links).current == 3,
                "Reflow changed the selected source chapter");
        require(native.goTo(links[2].position) && native.page() == links[2].page,
                "Mapped chapter page was stale after reflow");
        native.setChapters({}, {});
        require(native.chapterGraph().isEmpty() && native.chapters().size() == 3,
                "Empty native contents did not restore the publication TOC");
        native.resize(QSize(1072, 1328));
        require(native.style(32, 120) && native.goTo(comparisonAnchor),
                "Cannot restore the native-position fixture");
    }

    const auto nativeAnchor = native.position();
    const int originalNativePage = native.page();
    require(native.go(originalNativePage + 1), "Cannot turn the imported page");
    const auto turnedLocation = native.platformPosition(positions, &error);
    require(!turnedLocation.isEmpty() && turnedLocation != chapter2 + "#kobo.8.1:7",
            "Export reused the imported bookmark after a page turn");
    const auto turnedAnchor = native.position();
    require(native.goToPlatform(positions, turnedLocation, &error)
                    && native.position() == turnedAnchor,
            "Turned page did not round-trip exactly");
    require(native.goToPlatform(positions, chapter2 + "#kobo.8.1:7", &error), qPrintable(error));
    require(!native.goToPlatform(positions, chapter2 + "#missing:0", &error)
                    && !native.goToPlatform(positions, chapter2 + "#kobo.8.1:999999", &error)
                    && !native.goToPlatform(positions, "missing.xhtml#kobo.8.1:0", &error),
            "Invalid native location accepted");
    require(native.position() == nativeAnchor, "Failed native import moved the passage");
    require(native.style(42, 140) && native.position() == nativeAnchor,
            "Native import was not stable through reflow");
    require(native.goToPlatform(positions, chapter2, &error), qPrintable(error));
    require(native.platformPosition(positions, &error) == chapter2,
            "Chapter boundary export failed");
    const int chapterStart = native.page();
    require(native.goTo(native.chapters()[1].position) && native.page() == chapterStart,
            "Native chapter-only location was misplaced");
    // Like Nickel, a position that cannot be placed opens its chapter, or the
    // book when the chapter is missing, and says why.
    using Target = kobalt::EngineDocument::PositionTarget;
    require(native.goToPlatformNearest(positions, chapter2 + "#kobo.8.1:7", &error) == Target::Exact
                    && native.position() == nativeAnchor,
            "An exact native position fell back");
    for (const auto &location : { chapter2 + "#missing:0", chapter2 + "#kobo.8.1:999999" }) {
        require(native.goToPlatformNearest(positions, location, &error) == Target::Chapter
                        && native.page() == chapterStart && !error.isEmpty(),
                "An unknown anchor did not open its chapter");
    }
    for (const auto &location : { QString("missing.xhtml#kobo.8.1:0"), QString("garbage") }) {
        require(native.goToPlatformNearest(positions, location, &error) == Target::Book
                        && native.page() == 0 && !error.isEmpty(),
                "An unknown chapter did not open the book");
    }
    require(!model.go(-1) && !model.go(model.pages()), "Out-of-bounds page accepted");
    require(!model.goTo("/not/a/position"), "Invalid position accepted");
    require(model.go(3) && model.page() == 3, "Initial page jump was reset");
    const auto first = model.image(3);
    require(!first.isNull(), "No page image");
    int dark = 0;
    for (int y = 0; y < first.height(); ++y)
        for (int x = 0; x < first.width(); ++x)
            dark += qGray(first.pixel(x, y)) < 128;
    require(dark > 1000, "Fixture page is blank");
    const auto original = hash(first);
    const auto anchor = model.position();
    const int pages = model.pages();
    {
        const auto unchanged = model.state();
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(model, QSize(1072, 1328), &error), qPrintable(error));
        require(preview.pages() > 0 && preview.pages() < model.pages(),
                "Preview did not isolate one section");
        const auto before = hash(preview.image(preview.page()));
        require(before == original,
                "Section preview changed the current page at original settings");
        require(preview.style(48, 150) && preview.setWeight(6), "Cannot style the page preview");
        require(hash(preview.image(preview.page())) != before,
                "Draft settings did not change the preview");
        require(model.state() == unchanged && model.pages() == pages
                        && hash(model.image(3)) == original,
                "Preview changed the book's settings, position or pixels");
        require(preview.style(32, 120) && preview.setWeight(0)
                        && hash(preview.image(preview.page())) == before,
                "Preview cannot restore the original settings");
    }
    {
        const int previousPpi = kobalt::readingPpi.load();
        const auto captureDirectory = QString::fromLocal8Bit(qgetenv("KOBALT_USERSTYLE_CAPTURE"));
        if (!captureDirectory.isEmpty()) {
            require(QDir().mkpath(captureDirectory), "Cannot create user CSS capture directory");
        }
        for (const int ppi : { 300, 212 }) {
            kobalt::readingPpi.store(ppi);
            for (const bool dark : { false, true }) {
                kobalt::EngineDocument baseline(model.fontInputs()), styled(model.fontInputs()),
                        preview(model.fontInputs());
                auto settings = model.state();
                settings["dark_mode"] = dark;
                const QByteArray userCss(
                        "* { color: #777777 !important; } p { text-indent: 0 !important; }");
                require(baseline.open(bytes, model.viewport(), settings, &error),
                        qPrintable(error));
                styled.setUserStyleSheet(userCss);
                require(styled.open(bytes, model.viewport(), settings, &error), qPrintable(error));
                require(hash(styled.image(styled.page())) != hash(baseline.image(baseline.page())),
                        "User CSS did not change book rendering");
                require(preview.openPreview(styled, styled.viewport(), &error), qPrintable(error));
                require(preview.userStyleSheet() == userCss
                                && hash(preview.image(preview.page()))
                                        == hash(styled.image(styled.page())),
                        "Section preview did not inherit user CSS");
                require(preview.style(48, 150) && preview.style(32, 120),
                        "Cannot change styled preview typography");
                require(hash(preview.image(preview.page())) == hash(styled.image(styled.page())),
                        "Changing typography lost user CSS");
                if (!captureDirectory.isEmpty()) {
                    const auto prefix = captureDirectory + '/' + QString::number(ppi)
                            + (dark ? "-dark" : "-light");
                    require(baseline.image(baseline.page()).save(prefix + "-before.png")
                                    && styled.image(styled.page()).save(prefix + "-after.png"),
                            "Cannot save user CSS captures");
                }
            }
        }
        kobalt::readingPpi.store(previousPpi);
    }
    {
        require(model.goTo(model.chapters()[1].position) && model.go(model.page() + 1),
                "Cannot open second section");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        const auto expected = hash(model.image(model.page()));
        require(preview.openPreview(model, QSize(1072, 1328), &error), qPrintable(error));
        require(hash(preview.image(preview.page())) == expected,
                "Section preview lost a later chapter's text anchor");
        require(model.go(3), "Cannot return after section preview check");
    }
    {
        kobalt::EngineDocument recycling(model.fontInputs()), fresh(model.fontInputs());
        require(recycling.open(bytes, QSize(1072, 1328), {}, &error), qPrintable(error));
        require(fresh.open(bytes, QSize(1072, 1328), {}, &error), qPrintable(error));
        require(recycling.pages() > 4, "Buffer reuse fixture needs several pages");
        const auto retained = recycling.image(0);
        const auto retainedHash = hash(retained);
        const auto retainedKey = retained.cacheKey();
        require(recycling.go(3), "Cannot retire a retained page");
        const auto ownedPixels = recycling.image(3).constBits();
        require(ownedPixels != retained.constBits(), "Recycled a client-owned page buffer");
        require(recycling.go(0), "Cannot retire a model-owned page");
        const auto reused = recycling.image(0);
        require(reused.constBits() == ownedPixels,
                "Uniquely owned matching page buffer was not reused");
        require(hash(reused) == retainedHash && retained.cacheKey() == retainedKey
                        && hash(retained) == retainedHash,
                "Buffer reuse changed a retained image or left stale page pixels");
        for (int i = 1; i < qMin(8, recycling.pages()); ++i) {
            require(recycling.go(i) && fresh.go(i), "Cannot turn through buffer reuse fixture");
            require(hash(recycling.image(i)) == hash(fresh.image(i).copy()),
                    "Recycled page differs from fresh rendering");
            require(hash(retained) == retainedHash && retained.cacheKey() == retainedKey,
                    "Later page rendering mutated a client-owned image");
            require(recycling.cachedPages() <= 3, "Buffer reuse enlarged the page cache");
            fresh.releaseImages();
        }
        recycling.releaseImages();
        require(recycling.cachedPages() == 0, "Image release retained page images");
        require(hash(recycling.image(recycling.page())) == hash(fresh.image(fresh.page())),
                "Image release changed the next rendered page");
        recycling.resize(QSize(480, 640));
        fresh.resize(QSize(480, 640));
        require(recycling.image(recycling.page()).size() == QSize(480, 640)
                        && hash(recycling.image(recycling.page()))
                                == hash(fresh.image(fresh.page())),
                "Resize reused an old-size buffer or retained old pixels");
        recycling.setDarkMode(true);
        fresh.setDarkMode(true);
        require(hash(recycling.image(recycling.page())) == hash(fresh.image(fresh.page()))
                        && hash(retained) == retainedHash,
                "Appearance change retained old buffer pixels or mutated a retained image");
    }
    for (int i = 0; i < model.pages(); ++i) {
        model.go(i);
        model.image(i);
        model.image(i - 1);
        model.image(i + 1);
        require(model.cachedPages() <= 3, "Page cache exceeded bound");
    }
    model.go(3);
    require(hash(model.image(3)) == original, "Revisited page changed pixels");
    const auto damageFrom = model.image(3), damageTo = model.image(model.turnTarget(3, 1));
    model.prepareDamage(3, model.turnTarget(3, 1));
    QRect forwardDamage, backwardDamage;
    require(model.preparedDamage(damageFrom.cacheKey(), damageTo.cacheKey(), &forwardDamage)
                    && model.preparedDamage(damageTo.cacheKey(), damageFrom.cacheKey(),
                                            &backwardDamage)
                    && forwardDamage == backwardDamage,
            "Prepared damage was not reusable in both turn directions");
    require(model.style(42, 140) && model.pages() > pages, "Typography did not reflow");
    require(!model.preparedDamage(damageFrom.cacheKey(), damageTo.cacheKey(), &forwardDamage)
                    && !model.preparedDamage(damageTo.cacheKey(), damageFrom.cacheKey(),
                                             &backwardDamage),
            "Reflow retained prepared damage from the old images");
    const int reflowPage = model.page();
    require(model.goTo(anchor) && model.page() == reflowPage, "Reflow lost text anchor");
    require(model.position() == anchor, "Reflow replaced the requested text anchor");
    model.resize(QSize(314, 1));
    require(model.position() == anchor && model.page() == reflowPage,
            "Temporary window size moved the passage");
    model.resize(QSize(800, 1000));
    model.resize(QSize(1072, 1328));
    require(model.position() == anchor && model.page() == reflowPage,
            "Resize changed the text anchor");
    const auto saved = model.state();
    const auto expected = hash(model.image(model.page()));
    kobalt::EngineDocument reopened(kobalt::test::readerFonts());
    require(reopened.open(bytes, QSize(1072, 1328), saved, &error), qPrintable(error));
    require(reopened.page() == model.page() && hash(reopened.image(reopened.page())) == expected,
            "Reopen lost page or typography");
    model.style(34, 120);
    model.style(34, 130);
    for (int at : { 0, 1, 3, model.pages() - 1 }) {
        model.go(at);
        const auto checkpoint = model.state();
        require(reopened.open(bytes, QSize(1072, 1328), checkpoint, &error), qPrintable(error));
        require(reopened.page() == at && hash(reopened.image(at)) == hash(model.image(at)),
                "Sequential typography resume changed the page");
    }
    require(model.goTo(model.chapters().last().position) && model.page() > reflowPage,
            "Chapter navigation failed");
    const auto styleAnchor = model.position();
    require(model.style(34, 130, "NV Bitter", 96), "Margin change failed");
    require(model.position() == styleAnchor && model.margin() == 96,
            "Margin change moved the text anchor");
    const auto faces = model.fontFaces();
    require(faces.size() > 1, "Reading font selection is unavailable");
    // This sequence tests static-font thickness. An external variable fixture
    // can sort first and would make setWeight() select its weight axis instead.
    const auto otherFace = QStringLiteral("Libron");
    require(faces.contains(otherFace), "Bundled alternate static font is unavailable");
    require(model.style(34, 130, otherFace, 96), "Font change failed");
    require(model.position() == styleAnchor && model.fontFace() == otherFace,
            "Font change moved the text anchor");
    require(reopened.open(bytes, QSize(1072, 1328), model.state(), &error), qPrintable(error));
    require(reopened.fontFace() == otherFace && reopened.margin() == 96
                    && hash(reopened.image(reopened.page())) == hash(model.image(model.page())),
            "Font or margins lost on reopen");
    require(!model.style(34, 130, "Missing font", 96) && !model.style(34, 130, otherFace, 121),
            "Invalid typography accepted");
    require(model.style(34, 130, otherFace, 96, "left"), "Left alignment failed");
    const auto leftPage = hash(model.image(model.page()));
    require(model.style(34, 130, otherFace, 96, "justify")
                    && hash(model.image(model.page())) != leftPage,
            "Justification did not change paragraph layout");
    require(model.position() == styleAnchor && model.style(36, 130)
                    && model.alignment() == "justify",
            "Typography change lost alignment or text anchor");
    require(reopened.open(bytes, QSize(1072, 1328), model.state(), &error), qPrintable(error));
    require(reopened.alignment() == "justify"
                    && hash(reopened.image(reopened.page())) == hash(model.image(model.page())),
            "Alignment lost on reopen");
    require(!model.style(36, 130, otherFace, 96, "unsupported") && model.alignment() == "justify",
            "Invalid alignment changed the model");

    {
        kobalt::EngineDocument hyphenated(kobalt::test::readerFonts());
        require(hyphenated.open(bytes, QSize(320, 720), legacySettings, &error), qPrintable(error));
        const auto anchor = hyphenated.position();
        require(hyphenated.setHyphenation("on"), "Cannot enable hyphenation");
        const auto enabled = leadingPages(hyphenated);
        require(hyphenated.setHyphenation("off") && hyphenated.position() == anchor,
                "Hyphenation changed the text anchor");
        require(leadingPages(hyphenated) != enabled,
                "Disabling hyphenation did not change wrapping");
        require(reopened.open(bytes, QSize(320, 720), hyphenated.state(), &error)
                        && reopened.state().value("hyphenation").toString() == "off",
                "Hyphenation lost on reopen");
        require(hyphenated.setHyphenation("on") && leadingPages(hyphenated) == enabled,
                "On did not restore original pixels");
        require(hyphenated.setHyphenation("on") && hyphenated.position() == anchor,
                "Forced hyphenation changed the text anchor");
        require(reopened.open(bytes, QSize(320, 720), hyphenated.state(), &error)
                        && reopened.state().value("hyphenation").toString() == "on",
                "Forced hyphenation lost on reopen");
        require(!hyphenated.setHyphenation("invalid")
                        && hyphenated.state().value("hyphenation").toString() == "on",
                "Invalid hyphenation mode changed the model");
    }
    for (const auto &publisherRule : { QByteArray(), QByteArray("html { hyphens:auto; }"),
                                       QByteArray("p { hyphens:none; }") }) {
        kobalt::Publication publisherHyphens;
        require(publisherHyphens.inspect(bytes, &error), qPrintable(error));
        for (const auto &name : publisherHyphens.names()) {
            auto resource =
                    source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
            QByteArray content(int(resource->GetSize()), '\0');
            lvsize_t read = 0;
            require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                            && read == lvsize_t(content.size()),
                    "Cannot read hyphenation fixture resource");
            if (name.endsWith(".xhtml"))
                content.replace("</head>", "<style>" + publisherRule + "</style></head>");
            require(publisherHyphens.append(name, content, &error), qPrintable(error));
        }
        kobalt::EngineDocument hyphens(kobalt::test::readerFonts());
        const auto fixture = publisherHyphens.finish(&error);
        require(hyphens.open(fixture, QSize(320, 720), legacySettings, &error), qPrintable(error));
        const auto publisherPixels = leadingPages(hyphens);
        require(hyphens.setHyphenation("off"), "Cannot disable hyphenation");
        const auto offPixels = leadingPages(hyphens);
        require(hyphens.setHyphenation("on"), "Cannot force hyphenation");
        const auto onPixels = leadingPages(hyphens);
        require(onPixels != offPixels, "Hyphenation overrides did not change wrapping");
        require(publisherPixels == (publisherRule.contains("auto") ? onPixels : offPixels),
                "Auto did not follow publisher CSS with hyphenation off by default");
        auto settings = legacySettings;
        settings.remove("hyphenation");
        kobalt::EngineDocument defaultHyphens(kobalt::test::readerFonts());
        require(defaultHyphens.open(fixture, QSize(320, 720), settings, &error)
                        && defaultHyphens.state().value("hyphenation").toString() == "off"
                        && leadingPages(defaultHyphens) == offPixels,
                "Missing hyphenation setting did not override publisher CSS with Off");
        settings["hyphenation"] = QStringLiteral("auto");
        require(defaultHyphens.open(fixture, QSize(320, 720), settings, &error)
                        && leadingPages(defaultHyphens) == publisherPixels,
                "Off default replaced an explicit Auto setting");
    }
    // Repeated identical glyphs let the image reveal space compression without
    // depending on a particular font's advance width or antialiasing details.
    kobalt::Publication spaced;
    require(spaced.inspect(bytes, &error), qPrintable(error));
    for (const auto &name : spaced.names()) {
        auto resource =
                source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
        QByteArray content(int(resource->GetSize()), '\0');
        lvsize_t read = 0;
        require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                        && read == lvsize_t(content.size()),
                "Cannot read spacing fixture resource");
        if (name.endsWith(".xhtml"))
            content = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Spacing</title>"
                      "<style>p { margin:0 0 20px; text-indent:0; }</style></head>"
                      "<body><p>H H H</p><p>"
                    + QByteArray("H ").repeated(80) + "H</p></body></html>";
        require(spaced.append(name, content, &error), qPrintable(error));
    }
    const auto spacedBytes = spaced.finish(&error);
    // Identical glyphs on forced lines expose the actual rendered line pitch.
    // Measure pixels so applying the percentage twice cannot pass this check.
    auto lineHeightFixture = [&](const QByteArray &body) {
        kobalt::Publication fixture;
        require(fixture.inspect(bytes, &error), qPrintable(error));
        for (const auto &name : fixture.names()) {
            auto resource =
                    source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
            QByteArray content(int(resource->GetSize()), '\0');
            lvsize_t read = 0;
            require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                            && read == lvsize_t(content.size()),
                    "Cannot read line-height fixture resource");
            if (name.endsWith(".xhtml")) {
                content = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Line "
                          "height</title>"
                          "<style>p { margin:0; text-indent:0; }</style></head><body>"
                        + body + "</body></html>";
            }
            require(fixture.append(name, content, &error), qPrintable(error));
        }
        return fixture.finish(&error);
    };
    const auto lineHeightBytes = lineHeightFixture("<p>H<br/>H<br/>H<br/>H</p>");
    auto linePitch = [](const QImage &image) {
        QVector<int> starts;
        bool previousInk = false;
        for (int y = 0; y < image.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < image.width(); ++x) {
                ink |= qGray(image.pixel(x, y)) < 128;
            }
            if (ink && !previousInk) {
                starts.append(y);
            }
            previousInk = ink;
        }
        require(starts.size() >= 4, "Line-height fixture did not render four lines");
        const int pitch = starts[1] - starts[0];
        require(starts[2] - starts[1] == pitch && starts[3] - starts[2] == pitch,
                "Line-height fixture has inconsistent pitch");
        return pitch;
    };
    {
        kobalt::EngineDocument height(kobalt::test::readerFonts());
        auto settings = kobalt::qt::object({ { "font_pixels", 40 },
                                             { "line_spacing", 150 },
                                             { "line_spacing_version", 1 },
                                             { "margin_pixels", 0 },
                                             { "top_margin_pixels", 0 },
                                             { "bottom_margin_pixels", 0 } });
        require(height.open(lineHeightBytes, QSize(320, 720), settings, &error), qPrintable(error));
        require(linePitch(height.image(0)) == 60, "150% did not render at 1.5 times the font size");
        for (const int spacing : { 100, 200, 300 }) {
            require(height.style(40, spacing), "Cannot change line height");
            require(linePitch(height.image(0)) == 40 * spacing / 100,
                    "Relayout multiplied line height twice");
        }
        require(!height.style(40, 301), "Spacing above 300% was accepted");
        settings.remove("line_spacing_version");
        settings["line_spacing"] = 122;
        require(height.open(lineHeightBytes, QSize(320, 720), settings, &error)
                        && height.spacing() == 150,
                "Opening did not migrate saved 122% to 150%");
        const auto migrated = height.state();
        require(height.open(lineHeightBytes, QSize(320, 720), migrated, &error)
                        && height.spacing() == 150 && linePitch(height.image(0)) == 60,
                "Reopening migrated spacing applied the conversion again");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(height, height.viewport(), &error)
                        && linePitch(preview.image(0)) == 60,
                "Preview multiplied or migrated line height again");
        // Table cells, like paragraphs, space their lines by their own text size.
        for (const QByteArray cell : { "td", "th" }) {
            const auto table =
                    lineHeightFixture("<table style=\"font-size:0.5em\"><tr><" + cell
                                      + ">H<br/>H<br/>H<br/>H</" + cell + "></tr></table>");
            kobalt::EngineDocument cells(kobalt::test::readerFonts());
            require(cells.open(table, QSize(320, 720),
                               kobalt::qt::object({ { "font_pixels", 40 },
                                                    { "line_spacing", 150 },
                                                    { "line_spacing_version", 1 },
                                                    { "margin_pixels", 0 },
                                                    { "top_margin_pixels", 0 },
                                                    { "bottom_margin_pixels", 0 } }),
                               &error)
                            && linePitch(cells.image(0)) == 30,
                    "Table cell line height ignored the cell's text size");
        }
    }
    auto checkSpaces = [](const QImage &image) {
        QVector<QVector<int>> lines;
        QVector<bool> columns(image.width(), false);
        bool inLine = false;
        for (int y = 0; y <= image.height(); ++y) {
            bool ink = false;
            for (int x = 0; y < image.height() && x < image.width(); ++x) {
                if (qGray(image.pixel(x, y)) < 128) {
                    columns[x] = true;
                    ink = true;
                }
            }
            if (!ink && inLine) {
                QVector<int> starts;
                for (int x = 0; x < columns.size(); ++x)
                    if (columns[x] && (x == 0 || !columns[x - 1]))
                        starts.append(x);
                lines.append(starts);
                columns.fill(false);
            }
            inLine = ink;
        }
        require(lines.size() > 2 && lines.first().size() == 3,
                "Spacing reference did not render three glyphs");
        const int natural = lines.first()[1] - lines.first()[0];
        int minimum = natural, maximum = natural;
        for (int line = 1; line < lines.size(); ++line)
            for (int word = 1; word < lines[line].size(); ++word) {
                const int actual = lines[line][word] - lines[line][word - 1];
                minimum = qMin(minimum, actual);
                maximum = qMax(maximum, actual);
            }
        if (minimum < natural)
            qFatal("Line fitting compressed normal glyph pitch from %d to %d pixels", natural,
                   minimum);
        return maximum > natural;
    };
    for (const auto &alignment : { QString("left"), QString("justify") }) {
        kobalt::EngineDocument spacing(kobalt::test::readerFonts());
        require(spacing.open(spacedBytes, QSize(320, 720),
                             kobalt::qt::object(
                                     { { "margin_pixels", 0 }, { "text_alignment", alignment } }),
                             &error),
                qPrintable(error));
        const bool expanded = checkSpaces(spacing.image(0));
        require(alignment != "justify" || expanded, "Justification stopped expanding spaces");
        spacing.resize(QSize(352, 720));
        checkSpaces(spacing.image(0));
        require(spacing.style(48, 120), "Cannot reflow spacing fixture");
        checkSpaces(spacing.image(0));
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(spacing, QSize(352, 720), &error), qPrintable(error));
        checkSpaces(preview.image(0));
    }
    kobalt::Publication styled;
    require(styled.inspect(bytes, &error), qPrintable(error));
    for (const auto &name : styled.names()) {
        auto resource =
                source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
        QByteArray content(int(resource->GetSize()), '\0');
        lvsize_t read = 0;
        require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                        && read == lvsize_t(content.size()),
                "Cannot read style fixture resource");
        if (name.endsWith(".xhtml"))
            content =
                    "<?source test?><!--outside--><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Hidden title</title>"
                    "<style>p { margin-left:64px; text-indent:0; font-family:sans-serif; line-height:300%; } .hidden { display:none; }</style>"
                    "</head><body><p>Publisher styles control this paragraph.</p><p><b>Bold words retain their original face.</b></p>"
                    "<p><span id=\"native\">A&#x1f600;<em>BC</em>D</span></p>"
                    "<p id=\"normalized\">A   B\n C</p>"
                    "<p id=\"point-nodes\">A<!--comment--><?instruction test?>B<![CDATA[C]]>D</p>"
                    "<p class=\"hidden\">Hidden text<span id=\"duplicate\"><span id=\"duplicate\">X</span></span></p></body></html>";
        require(styled.append(name, content, &error), qPrintable(error));
    }
    kobalt::EngineDocument css(kobalt::test::readerFonts());
    const auto styledBytes = styled.finish(&error);
    require(css.open(styledBytes, QSize(1072, 1328), legacySettings, &error), qPrintable(error));
    require(css.pages() == 3,
            "Short EPUB chapter files ran together instead of starting new pages");
    require(css.goToPlatform(positions, chapter2 + "#native:4", &error), qPrintable(error));
    require(css.position().contains("/em") && css.position().endsWith(".1"),
            "UTF-16 offset did not map across nested text and emoji");
    require(css.platformPosition(positions, &error) == chapter2 + "#native:4",
            "Nested Unicode export lost its UTF-16 offset");
    require(css.comparePlatformPosition(positions, chapter2 + "#native:3", &error) < 0
                    && error.isEmpty(),
            "Unicode comparison must use source UTF-16 offsets");
    css.comparePlatformPosition(positions, chapter2 + "#native:2", &error);
    require(!error.isEmpty(), "Comparison must reject a split Unicode character");

    require(css.goToPlatform(positions, chapter2 + "#point(/1/2/3/1:6)", &error),
            qPrintable(error));
    require(css.platformPosition(positions, &error) == chapter2 + "#native:4",
            "Native DOM point must convert UTF-8 bytes to UTF-16 units");
    require(!css.goToPlatform(positions, chapter2 + "#point(/1/2/3/1:2)", &error),
            "Native DOM point split a UTF-8 character");
    require(!css.goToPlatform(positions, chapter2 + "#point(/1/2/3/1:999)", &error),
            "Native DOM point exceeded its text");
    for (int child : { 3, 4, 5 }) {
        require(css.goToPlatform(positions,
                                 chapter2 + "#point(/1/2/5/" + QString::number(child) + ":1)",
                                 &error),
                qPrintable(error));
        require(css.platformPosition(positions, &error)
                        == chapter2 + "#point-nodes:" + QString::number(child - 1),
                "Native DOM comments, instructions or CDATA changed child numbering");
    }
    require(!css.goToPlatform(positions, chapter2 + "#point(/1/2/5/2:0)", &error),
            "Comment node must not become a text position");
    require(css.goToPlatform(positions, chapter2 + "#native:4", &error), qPrintable(error));
    const auto unicodeAnchor = css.position();
    require(!css.goToPlatform(positions, chapter2 + "#native:2", &error)
                    && css.position() == unicodeAnchor,
            "Native offset inside a surrogate pair was accepted");
    require(!css.goToPlatform(positions, chapter2 + "#duplicate:0", &error)
                    && css.position() == unicodeAnchor,
            "Ambiguous nested source IDs were accepted");
    for (int offset : { 0, 1, 3, 4, 5, 6 }) {
        const auto location = chapter2 + "#native:" + QString::number(offset);
        require(css.goToPlatform(positions, location, &error), qPrintable(error));
        require(css.platformPosition(positions, &error) == location,
                "Unicode export changed the offset");
    }
    require(css.goTo("/body/DocFragment[2]/body/p[1]/text().4"),
            "Cannot select a node without an ID");
    const auto unlabelledAnchor = css.position();
    // The chapter's <style> gives CREngine a <stylesheet> element beside
    // <body>; the paragraph without an ID still gets Nickel's DOM point.
    require(css.platformPosition(positions, &error) == chapter2 + "#point(/1/2/1/1:4)"
                    && error.isEmpty() && css.position() == unlabelledAnchor,
            "Missing source ID prevented a native DOM point or moved the local position");
    require(css.goToPlatform(positions, chapter2 + "#point(/1/2/1/1:4)", &error)
                    && css.position() == "/body[1]/DocFragment[2]/body[1]/p[1]/text()[1].4",
            "Native DOM point beside a stylesheet did not return to the same text");
    require(css.goTo("/body/DocFragment[2]/body/p[4]/text().1"),
            "Cannot select whitespace fixture");
    const auto normalizedAnchor = css.position();
    require(css.platformPosition(positions, &error) == chapter2 && error.isEmpty()
                    && css.position() == normalizedAnchor,
            "Normalized whitespace prevented chapter-only progress or changed the local position");
    require(css.goTo("/body/DocFragment[2]/body/p[6]/span[1]/span[1]/text().0"),
            "Cannot select the duplicate-ID fixture");
    const auto ambiguousAnchor = css.position();
    require(css.platformPosition(positions, &error).isEmpty() && !error.isEmpty()
                    && css.position() == ambiguousAnchor,
            "Ambiguous source IDs were saved as a native position");
    const auto styledPage = css.image(0);
    int firstInk = styledPage.width();
    for (int y = 0; y < styledPage.height(); ++y)
        for (int x = 0; x < firstInk; ++x)
            if (qGray(styledPage.pixel(x, y)) < 128)
                firstInk = x;
    const int publisherIndent = 36 + qRound(64.0 * kobalt::readingPpi.load() / 96);
    require(firstInk >= publisherIndent && firstInk < publisherIndent + 44,
            "Publisher CSS did not override the default paragraph margin");
    {
        require(css.go(0), "Cannot select styled section");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(css, QSize(1072, 1328), &error), qPrintable(error));
        require(hash(preview.image(preview.page())) == hash(styledPage),
                "Section preview changed publisher CSS");
    }
    int previousInk = firstInk;
    for (const auto &alignment : { QString("center"), QString("right") }) {
        require(css.style(32, 120, "NV Bitter", 36, alignment), "Paragraph alignment failed");
        const auto aligned = css.image(0);
        int ink = aligned.width();
        for (int y = 0; y < aligned.height(); ++y)
            for (int x = 0; x < ink; ++x)
                if (qGray(aligned.pixel(x, y)) < 128)
                    ink = x;
        require(ink > previousInk + 20, "Paragraph did not move to the selected alignment");
        previousInk = ink;
    }
    require(css.style(32, 120, "NV Bitter", 36, "publisher")
                    && hash(css.image(0)) == hash(styledPage),
            "Publisher alignment did not restore the original page");
    const auto variableFace = [] {
        for (const auto &font : kobalt::discoverReadingFonts(kobalt::test::readerFonts().sources))
            if (font.weightAxis.available())
                return font;
        return kobalt::ReadingFont{};
    }();
    require(variableFace.weightAxis.available(),
            "The pinned variable-font fixture has no weight axis");
    if (variableFace.weightAxis.available()) {
        const auto axis = variableFace.weightAxis;
        const int initialWeight =
                qBound(axis.minimum, kobalt::defaultVariableFontWeight, axis.maximum);
        const int normalWeight = axis.minimum;
        const int alternateWeight = axis.maximum;
        kobalt::EngineDocument variable(kobalt::test::readerFonts()),
                restored(kobalt::test::readerFonts());
        require(variable.open(styledBytes, QSize(1072, 1328),
                              kobalt::qt::object({ { "font_face", variableFace.family },
                                                   { "font_weight", normalWeight } }),
                              &error),
                qPrintable(error));
        require(restored.open(styledBytes, QSize(1072, 1328),
                              kobalt::qt::object({ { "font_face", variableFace.family } }), &error),
                qPrintable(error));
        require(restored.weight() == initialWeight,
                "Variable font did not open at weight 450 by default");
        const auto normal = hash(variable.image(0));
        require(variable.setWeight(alternateWeight), "Intermediate variable weight was rejected");
        const auto medium = hash(variable.image(0));
        require(normal != medium && variable.weight() == alternateWeight,
                "Variable weight did not change the page");
        require(restored.open(styledBytes, variable.viewport(), variable.state(), &error),
                qPrintable(error));
        require(restored.weight() == alternateWeight && hash(restored.image(0)) == medium,
                "Variable weight was lost on reopening");
        require(variable.setWeight(normalWeight) && hash(variable.image(0)) != medium,
                "Light variable weight did not render");
        require(hash(restored.image(0)) == medium && hash(css.image(0)) == hash(styledPage),
                "Variable preview changed another model's weight");
        require(variable.setWeight(normalWeight) && hash(variable.image(0)) == normal,
                "Variable weight did not restore the original page");
        const int variablePages = variable.pages();
        require(variable.setThickness(3) && variable.thickness() == 3
                        && variable.weight() == normalWeight,
                "Variable font thickness replaced its axis weight");
        const auto thickened = hash(variable.image(0));
        require(thickened != normal && variable.pages() == variablePages,
                "Variable thickness did not change pixels without changing page count");
        require(restored.open(styledBytes, variable.viewport(), variable.state(), &error),
                qPrintable(error));
        require(restored.thickness() == 3 && restored.weight() == normalWeight
                        && hash(restored.image(0)) == thickened,
                "Independent variable weight and thickness were lost on reopening");
        require(variable.setThickness(0) && hash(variable.image(0)) == normal,
                "Clearing variable thickness did not restore pixels");
        require(!variable.setThickness(-1) && !variable.setThickness(13),
                "Thickness accepted an unsupported value");
        auto actual = fontMan->GetFont(32, alternateWeight, false, css_ff_serif,
                                       lString8(variableFace.family.toUtf8().constData()));
        require(actual->getWeight() == alternateWeight && actual->getSynthWeight() == 0,
                "Variable font used synthetic weight instead of its axis");
        if (variableFace.opticalSizeAxis.available()) {
            const int points = qCeil(variableFace.opticalSizeAxis.minimum);
            require(variable.setOpticalSize(points) && variable.opticalSize() == points,
                    "Manual optical size was rejected");
            require(restored.open(styledBytes, variable.viewport(), variable.state(), &error),
                    qPrintable(error));
            require(restored.opticalSize() == points, "Manual optical size was lost on reopening");
            require(!variable.setOpticalSize(points + 0.5),
                    "Optical size accepted a fractional point");
        } else
            require(!variable.setOpticalSize(14), "Weight-only font exposed optical-size control");
        require(!variable.setWeight(axis.minimum - 1) && !variable.setWeight(axis.maximum + 1),
                "Variable weight accepted an unsupported axis value");
        require(variable.style(32, 120, "Libron", 36) && variable.setWeight(3)
                        && variable.weight() == 3,
                "Switching to a static font did not restore thickness control");
        require(variable.style(32, 120, variableFace.family, 36)
                        && variable.weight() == normalWeight,
                "Static thickness replaced the saved variable weight");
    }
    const int weightPages = model.pages();
    const auto weightAnchor = model.position();
    const auto boundaryPage = model.page();
    require(model.go(boundaryPage), "Cannot resolve page boundary before weight change");
    const auto pageBoundary = model.position();
    require(model.goTo(weightAnchor), "Cannot restore weight test anchor");
    auto regular = fontMan->GetFont(32, 400, false, css_ff_serif, lString8("NV Bitter"));
    auto bold = fontMan->GetFont(32, 700, false, css_ff_serif, lString8("NV Bitter"));
    const auto regularWidth = regular->getTextWidth(U"Regular words", 13);
    const auto boldWidth = bold->getTextWidth(U"Bold words", 10);
    int thicknessLayoutCallbacks = 0;
    require(css.applyAppearance(kobalt::qt::object({ { "weight_steps", 3 } }), {},
                                [&](kobalt::OpeningStage stage, int) {
                                    if (stage == kobalt::OpeningStage::Styles
                                        || stage == kobalt::OpeningStage::Pages) {
                                        ++thicknessLayoutCallbacks;
                                    }
                                }),
            "Weight adjustment failed");
    require(thicknessLayoutCallbacks == 0, "Outline thickness recalculated styles or pagination");
    const auto weightedPage = hash(css.image(0));
    require(weightedPage != hash(styledPage) && css.weight() == 3,
            "Weight adjustment did not change pixels");
    require(reopened.open(styledBytes, QSize(1072, 1328), css.state(), &error), qPrintable(error));
    require(reopened.weight() == 3 && hash(reopened.image(0)) == weightedPage,
            "Weight lost on reopen");
    require(css.setWeight(0) && hash(css.image(0)) == hash(styledPage),
            "Zero weight did not restore original glyphs");
    require(!css.setWeight(-1) && !css.setWeight(13) && css.weight() == 0,
            "Invalid weight changed the model");
    require(hash(reopened.image(0)) == weightedPage, "Another model changed the saved font weight");
    quint64 previousInkSum = 0;
    for (int step = 0; step <= 12; ++step) {
        require(css.setWeight(step), "Weight step failed");
        const auto image = css.image(0);
        quint64 ink = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                ink += 255 - qGray(image.pixel(x, y));
        require(ink > previousInkSum, "Increasing outline weight did not increase ink");
        previousInkSum = ink;
    }
    require(reopened.open(styledBytes, QSize(1072, 1328), css.state(), &error)
                    && reopened.thickness() == 12,
            "Maximum thickness was lost on reopen");
    require(reopened.setThickness(3), "Cannot restore the comparison model's thickness");
    require(css.setWeight(0) && hash(css.image(0)) == hash(styledPage),
            "Weight steps changed the original rendering");
    require(regular->getWeight() == 400 && bold->getWeight() == 700
                    && regular->getTextWidth(U"Regular words", 13) == regularWidth
                    && bold->getTextWidth(U"Bold words", 10) == boldWidth,
            "Extra thickness changed the font selection or advance widths");
    require(model.setWeight(8) && model.pages() == weightPages && model.go(boundaryPage)
                    && model.position() == pageBoundary,
            "Extra thickness changed pagination");
    require(model.goTo(weightAnchor) && model.position() == styleAnchor && model.style(38, 130)
                    && model.weight() == 8,
            "Weight or text anchor lost across reflow");
    // Prefix runs retain DOM offsets, and distinct font instances keep emphasized
    // glyphs out of normal caches even when models draw alternately.
    const lString32 marked(U"Cafe\u0301 reading שלום");
    const auto runs = lvBionicRuns(marked.c_str(), marked.length());
    require(runs.size() == 6 && runs[0].start == 0 && runs[0].end == 2 && runs[2].start == 6
                    && runs[2].end == 10 && runs[4].end == 16,
            "Bionic prefixes did not count letters or retain Unicode offsets");
    const lString32 combining(U"a\u0301bc");
    const auto accents = lvBionicRuns(combining.c_str(), combining.length());
    require(accents[0].end == 3, "Bionic prefix split a combining mark from its letter");
    const lString32 discretionary(U"read\u00ading");
    const auto hyphenatedRuns = lvBionicRuns(discretionary.c_str(), discretionary.length());
    require(hyphenatedRuns.size() == 2 && hyphenatedRuns[0].end == 4
                    && !hyphenatedRuns[1].emphasized,
            "A discretionary hyphen restarted emphasis inside a word");
    for (const auto *word : { U"abꞳcd", U"abᲀcd", U"ＡＢＣＤＥ", U"ab\U0001df00cd" }) {
        const lString32 extended(word);
        const auto prefixes = lvBionicRuns(extended.c_str(), extended.length());
        require(prefixes.size() == 2 && prefixes[0].start == 0 && prefixes[0].end == 3
                        && prefixes[0].emphasized && !prefixes[1].emphasized
                        && prefixes[1].end == 5,
                "An extended Latin or Cyrillic letter split the Bionic word prefix");
    }
    const lString32 unchanged(U"العربية 日本語 12345");
    const auto otherScripts = lvBionicRuns(unchanged.c_str(), unchanged.length());
    require(otherScripts.size() == 1 && !otherScripts[0].emphasized,
            "Bionic mode split a connected or unsegmented script");
    const lString32 interrupted(U"ding next");
    const auto continued = lvBionicRuns(interrupted.c_str(), interrupted.length(), true);
    require(!continued[0].emphasized && continued[0].end == 5,
            "Inline markup created a second prefix in the middle of a word");
    const auto normalBionicPage = hash(css.image(0));
    const auto bionicAnchor = css.position();
    require(css.setWeight(4), "Cannot prepare saved thickness for Bionic reading");
    const auto priorWeightPage = hash(css.image(0));
    for (const auto *mode : { "bold" }) {
        require(css.applyAppearance(kobalt::qt::object({ { "bold_reading", mode } })),
                "Cannot enable Bionic reading");
        const auto emphasized = hash(css.image(0));
        kobalt::EngineDocument freshlyEmphasized(css.fontInputs());
        require(freshlyEmphasized.open(styledBytes, css.viewport(), css.state(), &error),
                qPrintable(error));
        require(css.pages() == freshlyEmphasized.pages()
                        && hash(freshlyEmphasized.image(0)) == emphasized,
                "Changing Bionic mode reused old pagination instead of matching a fresh layout");
        require(emphasized != normalBionicPage && emphasized != priorWeightPage
                        && css.weight() == 4,
                "Bionic reading did not emphasize prefixes or lost the saved thickness");
        require(css.position() == bionicAnchor && css.goTo(bionicAnchor),
                "Bionic reading lost the source position");
        require(!css.searchPage("words", 0).hits.isEmpty(),
                "Bionic reading broke source-text search");
        require(css.go(0), "Cannot select the Bionic search page");
        const auto hits = css.searchPage("styles", 0).hits;
        require(!hits.isEmpty(), "Bionic prefix boundary broke source search");
        const auto rectangles = css.rangeRects(hits[0].start, hits[0].end);
        require(!rectangles.isEmpty(), "Bionic prefix boundary lost selection rectangles");
        const auto selected = css.wordAt(rectangles.first().center());
        require(selected.text == "styles",
                "Dictionary lookup split a word at the emphasized prefix");
        require(css.goTo(bionicAnchor), "Cannot restore Bionic position after lookup");
        css.releaseImages();
        require(hash(reopened.image(0)) == weightedPage && hash(css.image(0)) == emphasized,
                "Alternating models contaminated font glyph caches");
        require(css.applyAppearance(kobalt::qt::object({ { "bold_reading", "off" } }))
                        && hash(css.image(0)) == priorWeightPage,
                "Disabling Bionic reading did not restore thickness");
    }
    require(!css.applyAppearance(kobalt::qt::object({ { "bold_reading", "invalid" } })),
            "Invalid Bionic mode was accepted");
    require(!css.applyAppearance(kobalt::qt::object({ { "bold_reading", "synthetic" } })),
            "Retired Bionic mode was accepted");
    auto legacyBionic = css.state();
    legacyBionic["bold_reading"] = QStringLiteral("synthetic");
    kobalt::EngineDocument migratedBionic(css.fontInputs());
    require(migratedBionic.open(styledBytes, css.viewport(), legacyBionic, &error),
            qPrintable(error));
    require(migratedBionic.state().value("bold_reading").toString() == "bold",
            "Retired Bionic setting did not migrate to bold");

    require(css.setWeight(0) && hash(css.image(0)) == normalBionicPage,
            "Bionic reading changed normal rendering");
    kobalt::Publication codeBook;
    require(codeBook.inspect(bytes, &error), qPrintable(error));
    for (const auto &name : codeBook.names()) {
        auto resource =
                source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
        QByteArray content(int(resource->GetSize()), '\0');
        lvsize_t read = 0;
        require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                        && read == lvsize_t(content.size()),
                "Cannot read code fixture resource");
        if (name.endsWith(".xhtml"))
            content =
                    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
                    "<p><code style=\"font-family:serif\">Proportional code must retain its original typography.</code></p>"
                    "</body></html>";
        require(codeBook.append(name, content, &error), qPrintable(error));
    }
    kobalt::EngineDocument codeModel(css.fontInputs());
    require(codeModel.open(codeBook.finish(&error), QSize(1072, 1328), {}, &error),
            qPrintable(error));
    const auto codeImage = hash(codeModel.image(0));
    for (const auto *mode : { "bold" })
        require(codeModel.applyAppearance(kobalt::qt::object({ { "bold_reading", mode } }))
                        && hash(codeModel.image(0)) == codeImage,
                "Bionic reading altered proportional code");
    kobalt::Publication nowrapBook;
    require(nowrapBook.inspect(bytes, &error), qPrintable(error));
    for (const auto &name : nowrapBook.names()) {
        auto resource =
                source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
        QByteArray content(int(resource->GetSize()), '\0');
        lvsize_t read = 0;
        require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                        && read == lvsize_t(content.size()),
                "Cannot read nowrap fixture resource");
        if (name.endsWith(".xhtml"))
            content =
                    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
                    "<p style=\"white-space:nowrap\">ordinary reading text</p>"
                    "<p>Before <span style=\"white-space:nowrap\">another reading phrase</span> after.</p>"
                    "</body></html>";
        require(nowrapBook.append(name, content, &error), qPrintable(error));
    }
    kobalt::EngineDocument nowrapModel(css.fontInputs());
    require(nowrapModel.open(nowrapBook.finish(&error), QSize(1072, 1328),
                             kobalt::qt::object({ { "font_pixels", 44 },
                                                  { "font_face", "Libron" },
                                                  { "weight_steps", 5 } }),
                             &error),
            qPrintable(error));
    require(nowrapModel.pages() > 1 && nowrapModel.go(nowrapModel.pages() / 2),
            "Cannot select the middle nowrap section");
    const auto nowrapAnchor = nowrapModel.position();
    for (const auto *mode : { "bold", "off", "bold", "off" }) {
        int formattingStarts = 0;
        require(nowrapModel.applyAppearance(
                        kobalt::qt::object({ { "bold_reading", mode } }), {},
                        [&](kobalt::OpeningStage stage, int percent) {
                            require(stage != kobalt::OpeningStage::Text && percent >= 0
                                            && percent <= 100,
                                    "Relayout reported parsing or invalid progress");
                            if (stage == kobalt::OpeningStage::Pages && percent == 0)
                                ++formattingStarts;
                        }),
                "Cannot toggle Bionic reading on nowrap text");
        require(formattingStarts > 0, "Bionic relayout did not report engine formatting progress");
        require(nowrapModel.position() == nowrapAnchor, "Bionic reading moved the nowrap position");
        const auto hits = nowrapModel.searchPage("reading", nowrapModel.page()).hits;
        require(hits.size() == 2, "Bionic reading lost nowrap source text");
        for (const auto &hit : hits) {
            const auto rectangles = nowrapModel.rangeRects(hit.start, hit.end);
            require(!rectangles.isEmpty(), "Bionic reading lost nowrap selection rectangles");
            require(nowrapModel.wordAt(rectangles.first().center()).text == "reading",
                    "Bionic reading split nowrap word selection");
        }
        require(!nowrapModel.image(nowrapModel.page()).isNull(),
                "Bionic reading lost the nowrap page image");
    }
    kobalt::Publication inlineBook;
    require(inlineBook.inspect(bytes, &error), qPrintable(error));
    for (const auto &name : inlineBook.names()) {
        auto resource =
                source->OpenStream(Utf8ToUnicode(name.toUtf8().constData()).c_str(), LVOM_READ);
        QByteArray content(int(resource->GetSize()), '\0');
        lvsize_t read = 0;
        require(resource->Read(content.data(), content.size(), &read) == LVERR_OK
                        && read == lvsize_t(content.size()),
                "Cannot read inline-heading fixture resource");
        if (name.endsWith(".xhtml"))
            content =
                    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
                    "<h2 style=\"font-size:0.7em;font-weight:normal;letter-spacing:0.25em;text-transform:uppercase;text-align:center\">"
                    "Chapter <span style=\"display:inline-block;margin-left:0.35em\">One</span></h2>"
                    "<p>Ordinary reading text.</p></body></html>";
        require(inlineBook.append(name, content, &error), qPrintable(error));
    }
    kobalt::EngineDocument inlineHeading(css.fontInputs());
    require(inlineHeading.open(inlineBook.finish(&error), QSize(1072, 1328), {}, &error),
            qPrintable(error));
    for (const auto &face : { QStringLiteral("Libron"), kobalt::test::headingReadingFace() }) {
        require(inlineHeading.applyAppearance(
                        kobalt::qt::object({ { "font_face", face }, { "bold_reading", "off" } })),
                "Cannot select the inline-heading regression font");
        int normalHeight = 0;
        for (const auto *mode : { "off", "bold" }) {
            require(inlineHeading.applyAppearance(kobalt::qt::object({ { "bold_reading", mode } })),
                    "Cannot test emphasized inline heading");
            const auto headings = inlineHeading.searchPage("One", 0).hits;
            require(!headings.isEmpty(), "Cannot find inline chapter number");
            QRect bounds;
            for (const auto &rect : inlineHeading.rangeRects(headings[0].start, headings[0].end))
                bounds = bounds.united(rect);
            if (!normalHeight)
                normalHeight = bounds.height();
            require(!bounds.isEmpty() && bounds.height() <= normalHeight + 2,
                    "Bionic prefix split a shrink-to-fit chapter number across lines");
        }
    }
    require(css.style(32, 120, otherFace, 36) && hash(css.image(0)) != hash(styledPage),
            "Publisher font declaration prevented changing the reading font");
    const auto fontPage = hash(css.image(0));
    require(css.style(32, 180) && hash(css.image(0)) != fontPage,
            "Publisher line-height declaration prevented changing line spacing");
    require(css.style(32, 120, QString(), 36) && css.fontFace().isEmpty(),
            "Publisher fonts could not be restored");
    {
        QImage picture(64, 64, QImage::Format_RGB32);
        picture.fill(qRgb(240, 20, 40));
        QByteArray png;
        QBuffer encoded(&png);
        encoded.open(QIODevice::WriteOnly);
        require(picture.save(&encoded, "PNG"), "Cannot create deferred image fixture");
        QMap<QString, QByteArray> data{
            { "mimetype", "application/epub+zip" },
            { "META-INF/container.xml",
              "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='OPS/book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
            { "OPS/book.opf",
              "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata/><manifest><item id='c' href='chapter.xhtml' media-type='application/xhtml+xml'/><item id='bad' href='bad.png' media-type='image/png'/><item id='good' href='good.png' media-type='image/png'/></manifest><spine><itemref idref='c'/></spine></package>" },
            { "OPS/chapter.xhtml",
              "<html xmlns='http://www.w3.org/1999/xhtml'><body><p>Images</p><p><img src='bad.png'/><img src='good.png'/></p></body></html>" },
            { "OPS/bad.png", png },
            { "OPS/good.png", png }
        };
        QMap<QString, qint64> sizes;
        for (auto it = data.begin(); it != data.end(); ++it)
            sizes[it.key()] = it.value().size();
        int failedLoads = 0, goodLoads = 0;
        auto resources = std::make_shared<kobalt::ResourceCache>(
                data.keys(),
                kobalt::ResourceCache::Access{
                        [&](const QString &name, const kobalt::ResourceCache::Sink &sink,
                            QString *failure) {
                            if (name == "OPS/bad.png") {
                                ++failedLoads;
                                *failure = "Synthetic image timeout";
                                return false;
                            }
                            if (name == "OPS/good.png")
                                ++goodLoads;
                            const auto content = data.value(name);
                            return sink(content.constData(), content.size());
                        },
                        {},
                        {},
                        [](const QString &name) {
                            return name.endsWith(".png") ? QSize(64, 64) : QSize();
                        } });
        kobalt::EngineDocument deferred(kobalt::test::readerFonts());
        require(deferred.openResources(resources, sizes, QSize(600, 800), {}, &error),
                qPrintable(error));
        deferred.image(0);
        require(deferred.loadNextImage() && deferred.resourceError().isEmpty()
                        && deferred.resourceStats()["image_error"].toString()
                                == "Synthetic image timeout",
                "Deferred image failure stopped rendering or lost its warning");
        require(deferred.loadNextImage(), "Deferred image failure stopped subsequent work");
        const auto rendered = deferred.image(0);
        bool colored = false;
        for (int y = 0; y < rendered.height() && !colored; ++y)
            for (int x = 0; x < rendered.width() && !colored; ++x) {
                const auto pixel = rendered.pixel(x, y);
                colored = qRed(pixel) > 200 && qGreen(pixel) < 50 && qBlue(pixel) < 80;
            }
        require(colored && failedLoads == 1 && goodLoads == 1 && deferred.resourceError().isEmpty(),
                "Good image did not render after a failed image, or the failed image retried while painting");
    }
    auto invalid = saved;
    invalid["position"] = QStringLiteral("/missing");
    require(!reopened.open(bytes, QSize(1072, 1328), invalid, &error),
            "Invalid stored position silently accepted");
    std::puts(
            "PASS: JPEG and SVG decoding, publisher CSS, chapter boundaries, EPUB preflight, page bounds, cache, typography and persisted anchors");
}
