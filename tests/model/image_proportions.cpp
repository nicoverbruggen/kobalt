#include "check.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include "support/qt_compat.h"
#include <QBuffer>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <cstdio>

static QRect redBounds(const QImage &image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixel(x, y);
            if (qRed(pixel) > 200 && qGreen(pixel) < 30 && qBlue(pixel) < 30)
                bounds = bounds.united(QRect(x, y, 1, 1));
        }
    return bounds;
}

// Checks image sizes for authored proportions, caps, CSS and SVG in previews and cached reopening, and dark-mode image inversion.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // Square artwork deliberately differs from the authored dimensions: caps
    // must preserve the authored ratio, not restore the file's intrinsic ratio.
    QImage art(40, 40, QImage::Format_RGB32);
    art.fill(Qt::red);
    QByteArray png;
    QBuffer output(&png);
    require(output.open(QIODevice::WriteOnly) && art.save(&output, "PNG"),
            "Cannot encode proportion fixture");
    const QMap<QString, QByteArray> base{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata/><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/><item id='i' href='art.png' media-type='image/png'/><item id='s' href='art.svg' media-type='image/svg+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "art.png", png },
        { "art.svg",
          "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='40' viewBox='0 0 40 40'><rect width='40' height='40' fill='red'/></svg>" }
    };

    const struct
    {
        const char *name;
        const char *markup;
        QSize size;
    } cases[]{
        { "wide report",
          "<img src='art.png' width='1251' height='350' style='max-width:95%'/>",
          { 760, 213 } },
        { "tall report",
          "<img src='art.png' width='1236' height='1012' style='max-width:95%'/>",
          { 760, 622 } },
        { "CSS width cap",
          "<img src='art.png' style='width:300px;height:100px;max-width:150px'/>",
          { 150, 50 } },
        { "CSS height cap",
          "<img src='art.png' style='width:300px;height:100px;max-height:50px'/>",
          { 150, 50 } },
        { "both, height limits",
          "<img src='art.png' width='300' height='100' style='max-width:150px;max-height:20px'/>",
          { 60, 20 } },
        { "both, width limits",
          "<img src='art.png' width='300' height='100' style='max-width:60px;max-height:50px'/>",
          { 60, 20 } },
        { "single rounding",
          "<img src='art.png' width='7' height='5' style='max-width:4px;max-height:2px'/>",
          { 3, 2 } },
        { "thin width cap",
          "<img src='art.png' width='1251' height='1' style='max-width:1px'/>",
          { 1, 1 } },
        { "thin height cap",
          "<img src='art.png' width='1' height='1251' style='max-height:1px'/>",
          { 1, 1 } },
        { "fitting attributes",
          "<img src='art.png' width='120' height='30' style='max-width:95%;max-height:100px'/>",
          { 120, 30 } },
        { "fitting CSS",
          "<img src='art.png' style='width:120px;height:30px;max-width:95%'/>",
          { 120, 30 } },
        { "auto height",
          "<img src='art.png' width='300' height='100' style='height:auto;max-width:100px'/>",
          { 100, 100 } },
        { "auto width",
          "<img src='art.png' style='height:100px;width:auto;max-height:50px'/>",
          { 50, 50 } },
        { "minimum wins",
          "<img src='art.png' width='300' height='100' style='max-width:150px;min-height:80px'/>",
          { 150, 80 } },
        { "zero width", "<img src='art.png' width='0' height='100' style='max-height:50px'/>", {} },
        { "zero maximum", "<img src='art.png' width='300' height='100' style='max-width:0'/>", {} },
        { "external SVG img",
          "<img src='art.svg' width='300' height='100' style='max-width:150px'/>",
          { 50, 50 } },
        { "inline SVG none",
          "<svg xmlns='http://www.w3.org/2000/svg' width='300' height='100' viewBox='0 0 300 100' preserveAspectRatio='none' style='max-width:150px'><rect width='300' height='100' fill='red'/></svg>",
          { 150, 50 } },
        { "inline SVG CSS",
          "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 300 100' preserveAspectRatio='none' style='width:300px;height:100px;max-height:50px'><rect width='300' height='100' fill='red'/></svg>",
          { 300, 50 } },
    };

    const auto settings = kobalt::qt::object({ { "font_pixels", 40 },
                                               { "margin_pixels", 0 },
                                               { "top_margin_pixels", 0 },
                                               { "bottom_margin_pixels", 0 } });
    const auto captureDirectory = qEnvironmentVariable("KOBALT_TEST_IMAGE_PROPORTIONS_CAPTURE_DIR");
    if (!captureDirectory.isEmpty())
        require(QDir().mkpath(captureDirectory),
                "Cannot create image proportion capture directory");
    const int originalPpi = kobalt::readingPpi.load();
    int failures = 0;
    for (const int ppi : { 96, 300 }) {
        kobalt::readingPpi.store(ppi);
        QTemporaryDir cache;
        require(cache.isValid(), "Cannot create image cache fixture");
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        for (const auto &fixture : cases) {
            // The full matrix uses CSS pixels directly. Repeat the reported
            // figures and the zero-rounding cases at Kobo's reading density.
            const auto name = QByteArray(fixture.name);
            if (ppi == 300 && !name.contains("report") && !name.startsWith("thin"))
                continue;
            auto data = base;
            data["c.xhtml"] = QByteArray("<html xmlns='http://www.w3.org/1999/xhtml'><body><div>")
                    + fixture.markup + "</div><h6>Caption</h6></body></html>";
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
            QString error;
            const bool cacheCase = name == "wide report";
            model.setDocumentCache(cacheCase ? cache.path() : QString(), "image-proportions",
                                   "image-test", 1024 * 1024);
            require(model.openResources(resources, lengths, QSize(800, 1000), settings, &error),
                    qPrintable(error));
            const auto rendered = model.image(0);
            const auto bounds = redBounds(rendered);
            if (!captureDirectory.isEmpty())
                require(rendered.save(captureDirectory + "/"
                                      + QString::fromLatin1(name).replace(' ', '-') + "-"
                                      + QString::number(ppi) + ".png"),
                        "Cannot save image proportion capture");
            const auto expected = ppi == 300 && name.startsWith("thin")
                    ? (name.contains("width") ? QSize(3, 1) : QSize(1, 3))
                    : fixture.size;
            std::fprintf(stderr, "%s at %d PPI: %d x %d\n", fixture.name, ppi, bounds.width(),
                         bounds.height());
            if (expected.isEmpty()) {
                require(bounds.isEmpty(), "Explicit zero image size became visible");
            } else {
                const int tolerance = name.contains("report") ? 1 : 0;
                if (bounds.isEmpty() || qAbs(bounds.width() - expected.width()) > tolerance
                    || qAbs(bounds.height() - expected.height()) > tolerance) {
                    std::fprintf(stderr, "FAIL: expected %d x %d (tolerance %d)\n",
                                 expected.width(), expected.height(), tolerance);
                    ++failures;
                    continue;
                }
                kobalt::EngineDocument preview(kobalt::test::readerFonts());
                require(preview.openPreview(model, QSize(800, 1000), &error), qPrintable(error));
                require(preview.image(0) == rendered, "Image proportions changed in a preview");
                if (cacheCase) {
                    require(!model.openedFromCache() && model.saveDocumentCache(),
                            "Cannot cache image proportions");
                    kobalt::EngineDocument reopened(kobalt::test::readerFonts());
                    reopened.setDocumentCache(cache.path(), "image-proportions", "image-test",
                                              1024 * 1024);
                    require(reopened.openResources(resources, lengths, QSize(800, 1000), settings,
                                                   &error),
                            qPrintable(error));
                    require(reopened.openedFromCache() && reopened.image(0) == rendered,
                            "Cached reopening changed image proportions");
                }
            }
        }
    }
    // Smart inversion changes image pixels only, including transparent ink and SVGs.
    kobalt::readingPpi.store(96);
    for (const auto *kind :
         { "gray", "color", "accent", "dark", "transparent", "svg", "color-svg" }) {
        const QByteArray type(kind);
        auto data = base;
        QImage illustration(100, 100, QImage::Format_ARGB32);
        illustration.fill(type == "transparent"    ? qRgba(255, 0, 255, 0)
                                  : type == "dark" ? qRgb(0, 0, 0)
                                                   : qRgb(255, 255, 255));
        for (int y = 30; y < 70; ++y)
            for (int x = 30; x < 70; ++x)
                illustration.setPixel(x, y, type == "color" ? qRgb(255, 0, 0) : qRgb(60, 60, 60));
        if (type == "accent")
            illustration.setPixel(50, 50, qRgb(255, 0, 0));
        QByteArray bytes;
        QBuffer encoded(&bytes);
        require(encoded.open(QIODevice::WriteOnly) && illustration.save(&encoded, "PNG"),
                "Cannot encode invert fixture");
        data["art.png"] = bytes;
        data["art.svg"] =
                R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="320" viewBox="0 0 640 320">
<rect width="640" height="320" fill="white"/>
<g stroke="#3c3c3c" stroke-width="4" fill="none">
<rect x="36" y="94" width="148" height="112" rx="8"/>
<rect x="246" y="94" width="148" height="112" rx="8"/>
<rect x="456" y="94" width="148" height="112" rx="8"/>
<path d="M184 150H234M221 138L234 150L221 162M394 150H444M431 138L444 150L431 162"/>
</g>
<g fill="#3c3c3c" font-family="sans-serif" text-anchor="middle">
<text x="320" y="48" font-size="28">How a book reaches you</text>
<text x="110" y="160" font-size="24">Author</text>
<text x="320" y="160" font-size="24">Publisher</text>
<text x="530" y="160" font-size="24">Reader</text>
<text x="320" y="270" font-size="19">An illustration printed on white paper</text>
</g></svg>)svg";
        if (type == "color-svg")
            data["art.svg"].replace("#3c3c3c", "#ff0000");
        data["c.xhtml"] =
                QByteArray("<html xmlns='http://www.w3.org/1999/xhtml'><body><img src='art.")
                + (type.endsWith("svg") ? "svg" : "png") + "'/><p>Caption</p></body></html>";
        QMap<QString, qint64> lengths;
        for (auto it = data.cbegin(); it != data.cend(); ++it)
            lengths[it.key()] = it.value().size();
        auto resources = std::make_shared<kobalt::ResourceCache>(
                data.keys(),
                [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                    const auto bytes = data.value(name);
                    return sink(bytes.constData(), bytes.size());
                },
                std::make_shared<std::atomic_bool>(false));
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        QString error;
        require(model.openResources(resources, lengths, { 800, 1000 }, {}, &error),
                qPrintable(error));
        const auto day = model.image(0);
        const auto position = model.position();
        const int pages = model.pages();
        const auto regions =
                QJsonDocument::fromJson(day.text("kobalt.image_regions").toUtf8()).array();
        require(!regions.isEmpty(), "Missing illustration region");
        const auto first = regions.first().toArray();
        const QRect region(first[0].toInt(), first[1].toInt(), first[2].toInt(), first[3].toInt());
        const auto original = day.copy(region);
        model.setDarkMode(true);
        require(model.cachedPages() == 0, "Dark mode kept stale page pixels");
        const auto night = model.image(0);
        const auto inverted = night.copy(region);
        const bool eligible = type == "gray" || type == "transparent" || type == "svg";
        require((inverted != original) == eligible, kind);
        int ink = 0, invertedInk = 0;
        for (int y = 0; y < inverted.height(); ++y)
            for (int x = 0; x < inverted.width(); ++x) {
                const auto before = original.pixel(x, y), after = inverted.pixel(x, y);
                if (qRed(before) == 60 && qGreen(before) == 60 && qBlue(before) == 60) {
                    ++ink;
                    invertedInk += after == qRgb(195, 195, 195);
                }
            }
        require(ink > 100 || type.endsWith("svg") || type == "color", "Missing illustration ink");
        require(!eligible || type == "svg" || invertedInk > 100,
                "Illustration ink was not inverted");
        if (!captureDirectory.isEmpty()) {
            require(original.save(captureDirectory + "/smart-" + kind + "-light.png")
                            && inverted.save(captureDirectory + "/smart-" + kind + "-dark.png"),
                    "Cannot capture smart inversion");
        }
        require(model.image(0) == night, "Repeated rendering changed inversion");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(model, { 800, 1000 }, &error), qPrintable(error));
        require(preview.image(0) == night, "Preview lost automatic smart inversion");
        require(model.pages() == pages && model.position() == position,
                "Smart inversion changed reading position");
        model.setDarkMode(false);
        require(model.image(0) == day, "Returning to light mode changed the source illustration");
        model.setDarkMode(true);
        require(model.image(0) == night, "Returning to dark mode lost automatic inversion");
    }
    kobalt::readingPpi.store(originalPpi);
    require(failures == 0, "Constrained images lost their proportions or compatibility sizes");
    std::puts("PASS: image maximum constraints, positive rounding, CSS and SVG compatibility");
}
