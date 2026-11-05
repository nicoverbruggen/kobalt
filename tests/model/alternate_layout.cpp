#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdio>

using kobalt::EngineDocument;

namespace {
const QSize portrait(800, 1136), landscape(1136, 800);

QByteArray pixels(const QImage &image)
{
    require(!image.isNull(), "Missing page image");
    return QCryptographicHash::hash(
            QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()),
                                    image.sizeInBytes()),
            QCryptographicHash::Sha256);
}

void open(EngineDocument &model, const kobalt::test::BookFiles &files, QJsonObject settings = {},
          QSize size = portrait)
{
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto resources = std::make_shared<kobalt::ResourceCache>(
            files.keys(),
            [files](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    QString error;
    require(model.openResources(resources, sizes, size, settings, &error), qPrintable(error));
}

int reflow(EngineDocument &model, QSize size, QJsonObject settings = {})
{
    int formats = 0;
    require(model.applyAppearance(settings, size,
                                  [&](kobalt::OpeningStage stage, int percent) {
                                      if (stage == kobalt::OpeningStage::Pages && percent == 0)
                                          ++formats;
                                  }),
            "Cannot change the viewport");
    return formats;
}

void compareWithReflow(const kobalt::test::BookFiles &files, const QJsonObject &settings = {})
{
    EngineDocument reused(kobalt::test::readerFonts()), fresh(kobalt::test::readerFonts());
    open(reused, files, settings);
    open(fresh, files, settings);
    require(reused.go(reused.pages() / 2) && fresh.goTo(reused.position()),
            "Cannot place comparison readers");
    const auto anchor = reused.position();
    for (auto size : { landscape, portrait, landscape, portrait }) {
        fresh.releaseImages();
        reflow(fresh, size);
        reflow(reused, size);
        require(reused.position() == anchor && fresh.position() == anchor,
                "Reflow comparison moved the source anchor");
        require(reused.pages() == fresh.pages(), "Cached and recomputed page counts differ");
        for (int page : { 0, reused.page(), reused.pages() - 1 }) {
            require(reused.pageStart(page) == fresh.pageStart(page),
                    "Cached and recomputed page starts differ");
            require(pixels(reused.image(page)) == pixels(fresh.image(page)),
                    "Cached and recomputed page pixels differ");
        }
        const auto edges = reused.annotationPageEdges();
        require(reused.rangeRects(edges.first.start, edges.first.end)
                        == fresh.rangeRects(edges.first.start, edges.first.end),
                "Cached annotation geometry differs from recomputed geometry");
    }
}

// Opt-in engine micro-measurement, with loading, pagination and paint separated.
// End-to-end performance scenarios remain in platforms/eval/scenarios.json.
void measureLayouts(EngineDocument &model, double loadMs)
{
    require(model.go(model.pages() / 2), "Cannot position timing book");
    auto sample = [&](QSize size, const char *label, bool cold) {
        if (cold)
            model.releaseImages();
        QElapsedTimer timer;
        timer.start();
        const int formats = reflow(model, size);
        const double layoutMs = timer.nsecsElapsed() / 1e6;
        timer.restart();
        require(!model.image(model.page()).isNull(), "Cannot paint timing page");
        const double paintMs = timer.nsecsElapsed() / 1e6;
        // A reused sample reports its formats, so a missed reuse shows in the output.
        require(!cold || formats > 0, "Timing sample did not take its declared layout path");
        const QJsonObject result{ { "sample", label },
                                  { "layout_ms", layoutMs },
                                  { "paint_ms", paintMs },
                                  { "formats", formats },
                                  { "snapshot_bytes", double(model.alternateLayoutBytes()) },
                                  { "pages", model.pages() } };
        puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    };
    printf("{\"load_ms\":%.3f}\n", loadMs);
    for (int repeat = 0; repeat < 5; ++repeat) {
        sample(landscape, "landscape-cold", true);
        sample(portrait, "portrait-reused", false);
        sample(landscape, "landscape-reused", false);
        sample(portrait, "portrait-cold", true);
    }
}

void roundTrip(EngineDocument &model)
{
    require(model.go(model.pages() / 2), "Cannot move into the book");
    const auto anchor = model.position();
    const auto count = model.pages();
    const auto image = pixels(model.image(model.page()));
    require(reflow(model, landscape) > 0, "A missing landscape layout was not computed");
    require(model.position() == anchor, "First rotation lost the source position");
    require(model.alternateLayoutBytes() > 0
                    && model.alternateLayoutBytes() <= EngineDocument::maxAlternateLayoutBytes,
            "Alternate layout exceeded its budget or was not retained");
    for (int i = 0; i < 3; ++i) {
        require(reflow(model, portrait) == 0, "Returning to portrait computed the book again");
        require(model.position() == anchor && model.pages() == count,
                "Restored layout lost position or pages");
        require(pixels(model.image(model.page())) == image, "Restored layout changed page pixels");
        // Search, selection and annotations consume node geometry, not just pages.
        const auto hits = model.searchPage("reader", model.page());
        if (!hits.hits.isEmpty()) {
            const auto &hit = hits.hits.first();
            require(!model.rangeRects(hit.start, hit.end).isEmpty(),
                    "Restored text lost its geometry");
        }
        require(reflow(model, landscape) == 0, "Returning to landscape computed the book again");
        require(model.position() == anchor, "Restored landscape lost the source position");
    }
    require(reflow(model, QSize(1153, 800)) > 0, "Changed dimensions reused stale geometry");
    require(reflow(model, portrait, { { "font_pixels", 55 } }) > 0,
            "Changed typography reused stale geometry");
    require(model.alternateLayoutBytes() == 0, "Changed typography retained an alternate layout");
    require(reflow(model, landscape) > 0, "Old typography survived invalidation");
    model.releaseImages();
    require(model.alternateLayoutBytes() == 0, "An idle book retained an alternate layout");
}
} // namespace

// Checks that a retained portrait or landscape layout, also two columns with hanging punctuation, is reused only when it matches and renders like a full reflow.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("KOBALT_LAYOUT_TIMINGS")) {
        require(argc == 2, "Pass a publication for engine timings");
        QFile file(QString::fromLocal8Bit(argv[1]));
        require(file.open(QIODevice::ReadOnly), "Cannot read timing publication");
        const auto bytes = file.readAll();
        QTemporaryDir cacheRoot;
        require(cacheRoot.isValid(), "Cannot create timing cache root");
        // Saved settings for every opening, such as two landscape columns.
        QJsonParseError parse{};
        const auto settingsText = qgetenv("KOBALT_LAYOUT_SETTINGS");
        const auto settings = settingsText.isEmpty()
                ? QJsonObject{}
                : QJsonDocument::fromJson(settingsText, &parse).object();
        require(parse.error == QJsonParseError::NoError,
                "KOBALT_LAYOUT_SETTINGS is not a JSON object");
        const bool disk = qEnvironmentVariableIsSet("KOBALT_LAYOUT_DISK_CACHE");
        const auto useCache = [&](EngineDocument &target) {
            if (disk)
                target.setDocumentCache(
                        cacheRoot.path() + "/.cache",
                        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                        "layout-timings", bytes.size());
        };
        EngineDocument model(kobalt::test::readerFonts());
        useCache(model);
        QString error;
        QElapsedTimer timer;
        timer.start();
        require(model.open(bytes, portrait, settings, &error), qPrintable(error));
        const double loadMs = timer.nsecsElapsed() / 1e6;
        if (disk) {
            timer.restart();
            require(model.saveDocumentCache(), "Cannot prepare disk-backed timing book");
            printf("{\"cache_save_ms\":%.3f}\n", timer.nsecsElapsed() / 1e6);
        }
        measureLayouts(model, loadMs);
        if (disk) {
            // Reopen from a cache saved in the landscape viewport.
            reflow(model, landscape);
            require(model.saveDocumentCache(), "Cannot save the landscape timing cache");
            for (int repeat = 0; repeat < 5; ++repeat) {
                EngineDocument reopened(kobalt::test::readerFonts());
                useCache(reopened);
                int formats = 0;
                timer.restart();
                require(reopened.open(bytes, landscape, settings, &error,
                                      [&](kobalt::OpeningStage stage, int percent) {
                                          if (stage == kobalt::OpeningStage::Pages && percent == 0)
                                              ++formats;
                                      }),
                        qPrintable(error));
                const QJsonObject result{ { "sample", "landscape-cached-open" },
                                          { "open_ms", timer.nsecsElapsed() / 1e6 },
                                          { "formats", formats },
                                          { "from_cache", reopened.openedFromCache() } };
                puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
            }
        }
        return 0;
    }
    // Mixed blocks, floats, lists and tables require more than a page-list cache.
    QByteArray body;
    for (int i = 0; i < 120; ++i)
        body += "<section><h2>A reader and a layout</h2><p style='float:left;width:20%'>A floating reader.</p>"
                "<p>A reader keeps the same passage after rotation. Words and spaces fill these lines.</p>"
                "<ol><li>First reader</li><li>Second reader</li></ol>"
                "<table><tr><td>A reader in a table.</td><td>More words.</td></tr></table></section>";
    const auto files = kobalt::test::publisherPackage(
            "en", "Orientation layouts", { { "text.xhtml", kobalt::test::publisherHtml(body) } },
            "<itemref idref='text.xhtml'/>");
    // Hanging punctuation gives two columns a side allowance that one column does not have.
    const QJsonObject hangingColumns{ { "two_columns_landscape", true },
                                      { "hanging_punctuation", true } };
    for (const auto &settings :
         { QJsonObject{}, QJsonObject{ { "two_columns_landscape", true } }, hangingColumns,
           QJsonObject{ { "vertical_justification", "expand" } } }) {
        EngineDocument model(kobalt::test::readerFonts());
        open(model, files, settings);
        roundTrip(model);
        open(model, kobalt::test::engineBook(false, 1, 4));
        require(model.alternateLayoutBytes() == 0, "Another publication inherited layout geometry");
    }
    compareWithReflow(files);
    compareWithReflow(files, { { "two_columns_landscape", true } });
    compareWithReflow(files, hangingColumns);
    compareWithReflow(files, { { "weight_steps", 3 }, { "dark_mode", true } });
    for (const auto &variant :
         { QByteArray("writing-mode:vertical-rl"), QByteArray("writing-mode:vertical-lr"),
           QByteArray("direction:rtl"), QByteArray() }) {
        auto mixed = files;
        mixed["text.xhtml"].replace("<head>", "<head><style>body{" + variant + "}</style>");
        compareWithReflow(mixed);
    }
    {
        auto media = files;
        media["text.xhtml"].replace(
                "<head>", "<head><style>@media (min-width:900px){p{font-size:180%;}}</style>");
        EngineDocument rotated(kobalt::test::readerFonts()), fresh(kobalt::test::readerFonts());
        open(rotated, media);
        open(fresh, media, {}, landscape);
        require(reflow(rotated, landscape) > 0 && rotated.pages() == fresh.pages(),
                "Rotation evaluated media queries with the previous viewport");
        for (int page : { 0, rotated.pages() / 2, rotated.pages() - 1 })
            require(pixels(rotated.image(page)) == pixels(fresh.image(page)),
                    "Rotated media-query styles differ from a fresh layout");
        require(reflow(rotated, portrait) == 0 && reflow(rotated, landscape) == 0,
                "Unchanged media-query layouts were computed again");
    }
    auto rightToLeft = files;
    rightToLeft["book.opf"].replace("<spine>", "<spine page-progression-direction='rtl'>");
    compareWithReflow(rightToLeft, { { "two_columns_landscape", true } });
    compareWithReflow(kobalt::test::publisherBooks({}).value("publisher.epub"));
    {
        EngineDocument model(kobalt::test::readerFonts());
        open(model, files);
        reflow(model, landscape);
        kobalt::readingPpi.store(212);
        require(reflow(model, portrait) > 0 && model.alternateLayoutBytes() == 0,
                "A changed reading density retained stale geometry");
        kobalt::readingPpi.store(300);
        model.setUserStyleSheet("p { letter-spacing:0.1em; }");
        open(model, files);
        require(model.alternateLayoutBytes() == 0,
                "A reopened stylesheet inherited an alternate layout");
    }
    for (const auto &settings :
         { QJsonObject{ { "font_face", kobalt::monospaceFontFace } },
           QJsonObject{ { "line_spacing", 175 } }, QJsonObject{ { "hyphenation", "on" } },
           QJsonObject{ { "bold_reading", "bold" } }, QJsonObject{ { "paragraph_spacing", 80 } },
           QJsonObject{ { "margin_pixels", 80 } },
           QJsonObject{ { "vertical_justification", "center" } } }) {
        EngineDocument model(kobalt::test::readerFonts());
        open(model, files);
        reflow(model, landscape);
        require(reflow(model, portrait, settings) > 0
                        || settings.contains("vertical_justification"),
                "Changed layout inputs did not reflow");
        require(model.alternateLayoutBytes() == 0,
                "Changed layout inputs kept stale alternate geometry");
        require(reflow(model, landscape) > 0, "Changed layout inputs reused the old landscape");
    }
    {
        EngineDocument oversized(kobalt::test::readerFonts());
        open(oversized, kobalt::test::engineBook(false, 1, 20000));
        const auto anchor = oversized.position();
        require(reflow(oversized, landscape) > 0 && oversized.alternateLayoutBytes() == 0,
                "Oversized layout escaped the snapshot budget");
        require(reflow(oversized, portrait) > 0 && oversized.position() == anchor,
                "Oversized fallback did not reflow or lost the source position");
    }
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        require(file.open(QIODevice::ReadOnly), "Cannot open Type Tester");
        const auto bytes = file.readAll();
        EngineDocument model(kobalt::test::readerFonts());
        QString error;
        require(model.open(bytes, portrait, {}, &error), qPrintable(error));
        roundTrip(model);
        for (const auto &settings : { QJsonObject{}, hangingColumns }) {
            QTemporaryDir directory;
            require(directory.isValid(), "Cannot create cached-layout profile");
            const auto fingerprint =
                    QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
            QJsonObject saved;
            QByteArray before;
            {
                EngineDocument cached(kobalt::test::readerFonts());
                cached.setDocumentCache(directory.path() + "/.cache", fingerprint,
                                        "alternate-layout-test", bytes.size());
                require(cached.open(bytes, portrait, settings, &error), qPrintable(error));
                require(cached.go(cached.pages() / 2), "Cannot position disk-cache book");
                require(reflow(cached, landscape) > 0 && reflow(cached, portrait) == 0,
                        "Disk-backed layout was not reused");
                before = pixels(cached.image(cached.page()));
                saved = cached.state();
                require(cached.saveDocumentCache(), "Cannot persist a restored layout");
            }
            {
                EngineDocument cached(kobalt::test::readerFonts());
                cached.setDocumentCache(directory.path() + "/.cache", fingerprint,
                                        "alternate-layout-test", bytes.size());
                int formats = 0;
                require(cached.open(bytes, portrait, saved, &error,
                                    [&](kobalt::OpeningStage stage, int percent) {
                                        if (stage == kobalt::OpeningStage::Pages && percent == 0)
                                            ++formats;
                                    }),
                        qPrintable(error));
                require(cached.openedFromCache() && formats == 0
                                && pixels(cached.image(cached.page())) == before,
                        "Persisted restored layout was invalid or changed pixels");
                require(reflow(cached, landscape) > 0 && reflow(cached, portrait) == 0,
                        "A reopened disk cache could not retain another viewport");
                // Save the landscape layout, so the next opening uses its side allowance.
                require(reflow(cached, landscape) == 0,
                        "A reopened disk cache did not return to landscape");
                before = pixels(cached.image(cached.page()));
                saved = cached.state();
                require(cached.saveDocumentCache(), "Cannot persist a landscape layout");
            }
            {
                EngineDocument cached(kobalt::test::readerFonts()),
                        fresh(kobalt::test::readerFonts());
                cached.setDocumentCache(directory.path() + "/.cache", fingerprint,
                                        "alternate-layout-test", bytes.size());
                int formats = 0;
                require(cached.open(bytes, landscape, saved, &error,
                                    [&](kobalt::OpeningStage stage, int percent) {
                                        if (stage == kobalt::OpeningStage::Pages && percent == 0)
                                            ++formats;
                                    }),
                        qPrintable(error));
                require(cached.openedFromCache() && formats == 0
                                && pixels(cached.image(cached.page())) == before,
                        "A cached landscape layout was laid out again or changed pixels");
                require(fresh.open(bytes, landscape, saved, &error), qPrintable(error));
                require(pixels(fresh.image(fresh.page())) == before,
                        "A cached landscape layout differs from a fresh one");
            }
        }
    }
    puts("PASS: alternate viewport reuse, geometry, invalidation, position and idle release");
}
