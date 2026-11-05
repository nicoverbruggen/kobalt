#include "reader_inputs.h"
#include "support/qt_compat.h"
#include <QColor>
#include "engine/crengine/engine_document.h"
#include <QGuiApplication>
#include <cstdio>
#include "check.h"

static QRect bounds(const QImage &image, int channel)
{
    QRect result;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto color = QColor::fromRgba(image.pixel(x, y));
            const bool match = channel == 0
                    ? color.red() > 180 && color.green() < 80 && color.blue() < 80
                    : channel == 1 ? color.blue() > 180 && color.red() < 80 && color.green() < 80
                                   : color.red() < 80 && color.green() < 80 && color.blue() < 80;
            if (match)
                result = result.united(QRect(x, y, 1, 1));
        }
    return result;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QByteArray content =
            "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
            "body{margin:0}h2{font-size:32px;text-align:center}p{margin:12px 0;text-indent:0}"
            ".prose{text-align:right!important;color:blue}.CDL{text-align:center}"
            "</style></head><body><h2>Centered heading</h2>"
            "<p style='text-align:center'><span><img src='art.svg' width='120' height='40'/></span></p>"
            "<p class='CDL'><span>SIX YEARS AGO</span></p>"
            "<div style='text-align:center'><p>Inherited centered text</p></div>"
            "<p style='text-align:center!important'>Inline centered text</p>"
            "<p class='chapter-title' style='text-align:center'>Paragraph title</p>"
            "<blockquote style='text-align:center'><h3>Nested heading</h3></blockquote>"
            "<p role='heading' style='text-align:center'>Accessible heading</p>"
            "<figure style='text-align:center'><figcaption>Figure caption</figcaption></figure>"
            "<p class='prose'><span style='display:block;text-align:right!important'>Ordinary text.</span></p></body></html>";
    const QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Alignment</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/><item id='i' href='art.svg' media-type='image/svg+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "c.xhtml", content },
        { "art.svg",
          "<svg xmlns='http://www.w3.org/2000/svg' width='120' height='40'><rect width='120' height='40' fill='red'/></svg>" }
    };
    QMap<QString, qint64> sizes;
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto resources = std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    QString error;
    // Alignment needs all decorative blocks on one page at device density.
    require(model.openResources(resources, sizes, QSize(800, 2200),
                                kobalt::qt::object({ { "text_alignment", "publisher" } }), &error),
            qPrintable(error));
    const auto original = model.image(0);
    const auto imageBounds = bounds(original, 0), proseBounds = bounds(original, 1),
               headingBounds = bounds(original, 2);
    require(!imageBounds.isEmpty() && !proseBounds.isEmpty() && !headingBounds.isEmpty(),
            "Alignment fixture is missing artwork or text");
    const auto anchor = model.position();
    for (const auto *mode : { "left", "right", "justify" }) {
        require(model.applyAppearance(kobalt::qt::object({ { "text_alignment", mode } })),
                "Cannot apply alignment");
        const auto aligned = model.image(0);
        std::printf("%s: image x=%d, prose x=%d, heading x=%d\n", mode, bounds(aligned, 0).x(),
                    bounds(aligned, 1).x(), bounds(aligned, 2).x());
        std::fflush(stdout);
        require(bounds(aligned, 0) == imageBounds, "Text alignment moved a centered image");
        require(bounds(aligned, 2) == headingBounds,
                "Text alignment moved publisher-centered text or a heading");
        for (int y = 0; y < aligned.height(); ++y)
            for (int x = 0; x < aligned.width(); ++x) {
                const auto before = QColor::fromRgba(original.pixel(x, y)),
                           after = QColor::fromRgba(aligned.pixel(x, y));
                const bool b = before.red() < 80 && before.green() < 80 && before.blue() < 80;
                const bool a = after.red() < 80 && after.green() < 80 && after.blue() < 80;
                require(a == b,
                        "Alignment changed publisher-centered text, decorative text or a caption");
            }
        require(QString(mode) == "right" ? bounds(aligned, 1).x() == proseBounds.x()
                                         : bounds(aligned, 1).x() < proseBounds.x(),
                "Body text did not override publisher alignment");
        require(model.position() == anchor, "Alignment moved the reading anchor");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(model, QSize(800, 2200), &error), qPrintable(error));
        const auto previewImage = preview.image(0);
        std::printf("preview: image x=%d, prose x=%d, heading x=%d\n", bounds(previewImage, 0).x(),
                    bounds(previewImage, 1).x(), bounds(previewImage, 2).x());
        std::fflush(stdout);
        require(previewImage == aligned, "Alignment differs in the section preview");
    }
    require(model.applyAppearance(kobalt::qt::object({ { "text_alignment", "publisher" } }))
                    && model.image(0) == original,
            "Publisher alignment did not restore the original layout");
    auto marginImage = [&](const QByteArray &body, int margin, int ppi = 300,
                           QJsonObject settings = {}) {
        const int callerPpi = kobalt::readingPpi.exchange(ppi);
        auto pageData = data;
        pageData["c.xhtml"] =
                "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
                "@page{margin:100px}html{margin:90px!important;padding:20px!important}"
                "body{margin:80px!important;padding:30px!important}"
                "p{margin:0;text-indent:0}p.picture{margin-top:80px!important}"
                "</style></head><body>"
                + body + "</body></html>";
        QMap<QString, qint64> pageSizes;
        for (auto it = pageData.cbegin(); it != pageData.cend(); ++it)
            pageSizes[it.key()] = it.value().size();
        auto cache = std::make_shared<kobalt::ResourceCache>(
                pageData.keys(),
                [pageData](const QString &name, const kobalt::ResourceCache::Sink &sink,
                           QString *) {
                    const auto bytes = pageData.value(name);
                    return sink(bytes.constData(), bytes.size());
                },
                std::make_shared<std::atomic_bool>(false));
        kobalt::EngineDocument page(kobalt::test::readerFonts());
        // Leave room for the device-scaled image and authored spacing.
        settings["margin_pixels"] = margin;
        require(page.openResources(cache, pageSizes, QSize(1600, 1800), settings, &error),
                qPrintable(error));
        const auto image = page.image(0);
        kobalt::readingPpi.store(callerPpi);
        return image;
    };
    const QByteArray picture =
            "<p class='picture'><span><img src='art.svg' width='120' height='40' style='display:block;margin-top:40px!important'/></span></p>";
    const auto edge = marginImage(picture, 0);
    std::printf("Zero margins: first image x=%d y=%d\n", bounds(edge, 0).x(), bounds(edge, 0).y());
    const auto barePicture = QByteArray(picture)
                                     .replace("class='picture'", "class='plain'")
                                     .replace("margin-top:40px!important", "margin-top:0");
    const auto bare = marginImage(barePicture, 0);
    const auto parentMargin = marginImage(
            QByteArray(picture).replace("margin-top:40px!important", "margin-top:0"), 0);
    const auto imageMargin =
            marginImage(QByteArray(picture).replace("class='picture'", "class='plain'"), 0);
    std::fprintf(
            stderr,
            "Leading image bounds: both=%d,%d,%d,%d bare=%d,%d,%d,%d parent=%d,%d,%d,%d image=%d,%d,%d,%d\n",
            bounds(edge, 0).x(), bounds(edge, 0).y(), bounds(edge, 0).width(),
            bounds(edge, 0).height(), bounds(bare, 0).x(), bounds(bare, 0).y(),
            bounds(bare, 0).width(), bounds(bare, 0).height(), bounds(parentMargin, 0).x(),
            bounds(parentMargin, 0).y(), bounds(parentMargin, 0).width(),
            bounds(parentMargin, 0).height(), bounds(imageMargin, 0).x(),
            bounds(imageMargin, 0).y(), bounds(imageMargin, 0).width(),
            bounds(imageMargin, 0).height());
    require(bounds(bare, 0).y() == 0 && bounds(edge, 0).x() == 0 && bounds(edge, 0).y() == 251
                    && bounds(parentMargin, 0).y() == 251 && bounds(imageMargin, 0).y() == 126,
            "The leading image or its parent lost publisher spacing");
    for (bool margins : { false, true }) {
        for (bool chapters : { false, true }) {
            const QJsonObject rules{ { "normalize_page_margins", margins },
                                     { "reduce_chapter_spacing", chapters } };
            const auto plain = marginImage(barePicture, 0, 300, rules);
            const auto spaced = marginImage(picture, 0, 300, rules);
            require(bounds(spaced, 0).top() >= bounds(plain, 0).top() + 240,
                    "Independent spacing rules removed a chapter-opening illustration's margin");
        }
    }
    const auto bare212 = marginImage(barePicture, 0, 212);
    const auto parent212 = marginImage(
            QByteArray(picture).replace("margin-top:40px!important", "margin-top:0"), 0, 212);
    require(bounds(bare212, 0).y() == 0 && bounds(parent212, 0).y() == (80 * 212 + 48) / 96 + 1,
            "The leading image boundary added or lost a pixel at 212 ppi");
    const auto inset = marginImage(picture, 48);
    require(bounds(inset, 0).x() == 48 && bounds(inset, 0).y() == bounds(edge, 0).y() + 24,
            "Publisher spacing did not add to the app's page inset");
    require(model.applyAppearance(kobalt::qt::object(
                    { { "top_margin_pixels", 96 }, { "bottom_margin_pixels", 60 } })),
            "Cannot set independent vertical margins");
    require(model.state().value("top_margin_pixels").toInt() == 96
                    && model.state().value("bottom_margin_pixels").toInt() == 60,
            "Vertical margins were not retained");
    require(bounds(model.image(0), 2).top() >= 96, "Top margin did not inset the text");
    require(!model.applyAppearance(kobalt::qt::object({ { "top_margin_pixels", -1 } })),
            "Accepted a negative top margin");
    require(!model.applyAppearance(kobalt::qt::object(
                    { { "bottom_margin_pixels", kobalt::maximumMarginPixels() + 1 } })),
            "Accepted an excessive bottom margin");
    require(model.style(model.fontSize(), model.spacing(), model.fontFace(), 12),
            "Cannot change side margins");
    require(model.state().value("top_margin_pixels").toInt() == 96
                    && model.state().value("bottom_margin_pixels").toInt() == 60,
            "Side margins changed independent vertical margins");
    const auto later =
            marginImage("<p style='color:blue'>Text before the illustration.</p>" + picture, 0);
    require(bounds(later, 0).y() >= bounds(later, 1).bottom() + 70,
            "A later illustration lost its publisher spacing");
    std::puts("PASS: body alignment preserves headings and illustrations");
}
