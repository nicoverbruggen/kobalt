#include "engine/crengine/engine_document.h"
#include "reading/appearance_settings.h"
#include "publication/local_resources.h"
#include "publication/publication.h"
#include "reader_inputs.h"
#include "check.h"
#include <QCryptographicHash>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace kobalt;

namespace {
std::shared_ptr<ResourceCache> fixture(const QByteArray &body, QMap<QString, qint64> &sizes,
                                       const QByteArray &css = {})
{
    const QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Beautifier</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "c.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0 0 .8em;text-indent:0}.dropcap{float:left;font-size:3em;margin:0 .1em 0 0}"
                  + css + "</style></head><body>" + body + "</body></html>" }
    };
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        sizes[it.key()] = it.value().size();
    return std::make_shared<ResourceCache>(
            data.keys(),
            [data](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
}

QByteArray read(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read comparison book");
    return file.readAll();
}

SearchHit find(EngineDocument &model, const QString &query)
{
    for (int page = 0; page < model.pages(); ++page) {
        const auto hits = model.searchPage(query, page).hits;
        if (!hits.isEmpty())
            return hits.first();
    }
    require(false, "Comparison passage was not found");
    return {};
}

void compare(const QString &path, const QString &query, const QString &output)
{
    const auto bytes = read(path);
    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    QString error;
    EngineDocument model(test::readerFonts());
    QFile input(path);
    require(input.open(QIODevice::ReadOnly), "Cannot open comparison source");
    Publication publication;
    require(publication.inspect(input, &error), qPrintable(error));
    input.close();
    const auto factory = localResourceFactory(path, std::make_shared<SourceLease>());
    const auto resources =
            factory(publication.sizes(), {}, std::make_shared<std::atomic_bool>(false));
    require(model.openResources(resources, publication.sizes(), { 1072, 1448 },
                                { { "font_pixels", 44 },
                                  { "line_spacing_version", 1 },
                                  { "text_alignment", "publisher" } },
                                &error),
            qPrintable(error));
    const auto hit = find(model, query);
    require(model.goTo(hit.start), "Cannot open comparison passage");
    const auto before = model.image(model.page());
    require(QDir().mkpath(output) && before.save(output + "/before.png"),
            "Cannot save before render");
    require(model.applyAppearance(kobalt::appearance::Patch({ { "beautifier_initials", true },
                                                              { "beautifier_spacing", true },
                                                              { "beautifier_indent", true } })),
            "Cannot enable beautifier");
    require(model.goTo(hit.start), "Beautifier lost comparison anchor");
    const auto after = model.image(model.page());
    require(after.save(output + "/after.png"), "Cannot save after render");
    require(model.textRange(hit.start, hit.end).text == query, "Beautifier changed source text");
    const int afterPages = model.pages();
    require(model.applyAppearance(kobalt::appearance::Patch({ { "beautifier_initials", false },
                                                              { "beautifier_spacing", false },
                                                              { "beautifier_indent", false } }))
                    && model.goTo(hit.start) && model.image(model.page()) == before,
            "Turning beautifier off did not restore the original pixels");
    require(QCryptographicHash::hash(read(path), QCryptographicHash::Sha256).toHex() == hash,
            "Source EPUB changed");
    QFile record(output + "/comparison.json");
    require(record.open(QIODevice::WriteOnly), "Cannot save comparison record");
    record.write(QJsonDocument(QJsonObject{ { "source_sha256", QString::fromLatin1(hash) },
                                            { "changed", before != after },
                                            { "before_pages", model.pages() },
                                            { "after_pages", afterPages },
                                            { "anchor", hit.start },
                                            { "off_roundtrip_equal", true },
                                            { "source_unchanged", true } })
                         .toJson());
}

void compareLayout(const QString &path, const QString &query, const QString &output)
{
    const auto bytes = read(path);
    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    QString error;
    EngineDocument model(test::readerFonts());
    require(model.open(bytes, { 1072, 1448 },
                       { { "font_pixels", 44 },
                         { "line_spacing_version", 1 },
                         { "text_alignment", "publisher" } },
                       &error),
            qPrintable(error));
    const auto hit = find(model, query);
    require(model.goTo(hit.start), "Cannot open layout comparison passage");
    require(QDir().mkpath(output), "Cannot create layout comparison directory");
    const auto settings = model.state();
    const auto original = model.image(model.page());
    QJsonObject result;
    for (const auto *key : { "paragraph_spacing", "paragraph_indent", "line_spacing" }) {
        require(model.applyAppearance(kobalt::appearance::Patch(
                        { { key, key == QString("line_spacing") ? 100 : 0 } }))
                        && model.goTo(hit.start),
                "Cannot set first layout comparison value");
        const auto before = model.image(model.page());
        require(model.applyAppearance(kobalt::appearance::Patch(
                        { { key, key == QString("paragraph_spacing") ? 100 : 200 } }))
                        && model.goTo(hit.start),
                "Cannot set second layout comparison value");
        const auto after = model.image(model.page());
        result[key] = before != after;
        require(before.save(output + "/" + key + "-before.png")
                        && after.save(output + "/" + key + "-after.png"),
                "Cannot save layout comparison images");
        require(model.applyAppearance(kobalt::appearance::Patch(settings)) && model.goTo(hit.start)
                        && model.image(model.page()) == original,
                "Layout comparison did not restore the original page");
    }
    const int firstPage = model.page();
    const int pageCount = model.pages();
    const int endPage = qMin(model.pages(), firstPage + 32);
    QVector<QImage> pages;
    for (int page = firstPage; page < endPage; ++page)
        pages.append(model.image(page));
    for (const auto *mode : { "expand", "center" }) {
        require(model.applyAppearance(
                        kobalt::appearance::Patch({ { "vertical_justification", mode },
                                                    { "vertical_spacing_scope", "all" } })),
                "Cannot select comparison page spacing");
        require(model.pages() == pageCount, "Page spacing changed pagination");
        int changed = 0;
        for (int page = firstPage; page < endPage; ++page) {
            const auto after = model.image(page);
            if (after == pages[page - firstPage])
                continue;
            if (!changed) {
                require(pages[page - firstPage].save(output + "/" + mode + "-before.png")
                                && after.save(output + "/" + mode + "-after.png"),
                        "Cannot save page spacing comparison");
            }
            ++changed;
        }
        result[QString(mode) + "_changed_pages"] = changed;
        require(model.applyAppearance(kobalt::appearance::Patch(settings)) && model.goTo(hit.start)
                        && model.image(model.page()) == original,
                "Page spacing comparison did not restore the original page");
    }
    require(QCryptographicHash::hash(read(path), QCryptographicHash::Sha256).toHex() == hash,
            "Source EPUB changed");
    result["source_sha256"] = QString::fromLatin1(hash);
    result["source_unchanged"] = true;
    result["roundtrip_equal"] = true;
    QFile record(output + "/layout-comparison.json");
    require(record.open(QIODevice::WriteOnly), "Cannot save layout comparison record");
    record.write(QJsonDocument(result).toJson());
}
} // namespace

// Checks book normalization: reversible styling, protected paragraphs, text initials, explicit overrides, caches and previews.
static void checkTextColors()
{
    const QByteArray body =
            "<h2 id='title'>Colored heading</h2>"
            "<p id='colored' style='color:#12120d !important;background-color:#224466 !important'>"
            "Inherited ink <span>and nested text</span> "
            "<span style='color:#cc2200 !important;background-color:#ffff00 !important'>inline ink</span></p>"
            "<p><a href='#title'>Linked text</a></p>";
    const QByteArray css =
            "body {color:#334455;background-color:#ffddaa}"
            "#title {color:#e8f0f0 !important;background-color:#222 !important}"
            "a {color:#0000ff !important;text-decoration:underline} p::before {content:'Before ';color:red !important}";
    QMap<QString, qint64> sizes, referenceSizes;
    const auto source = fixture(body, sizes, css);
    // Same markup and geometry, but with explicit theme-neutral author colors.
    auto referenceBody = body;
    referenceBody.replace("#12120d", "#000")
            .replace("#224466", "transparent")
            .replace("#cc2200", "#000")
            .replace("#ffff00", "transparent");
    auto referenceCss = css;
    referenceCss.replace("#334455", "#000")
            .replace("#ffddaa", "transparent")
            .replace("#e8f0f0", "#000")
            .replace("#222", "transparent")
            .replace("#0000ff", "#000")
            .replace("color:red", "color:#000");
    const auto reference = fixture(referenceBody, referenceSizes, referenceCss);
    QTemporaryDir cache(QDir::currentPath() + "/text-colors-cache-XXXXXX");
    require(cache.isValid(), "Cannot create text color cache");
    QString error;
    EngineDocument model(test::readerFonts()), expected(test::readerFonts());
    model.setDocumentCache(cache.path(), QByteArray(64, 'c'), "text-colors", 1024 * 1024);
    require(model.openResources(source, sizes, { 800, 1000 }, {}, &error), qPrintable(error));
    require(expected.openResources(reference, referenceSizes, { 800, 1000 }, {}, &error),
            qPrintable(error));
    const auto original = model.image(0);
    const auto anchor = model.position();
    require(model.saveDocumentCache(), "Cannot save publisher color cache");
    for (bool dark : { false, true, false }) {
        model.setDarkMode(dark);
        expected.setDarkMode(dark);
        const auto publisher = model.image(0);
        require(model.applyAppearance(appearance::Patch({ { "normalize_text_colors", true } })),
                "Cannot enable text color normalization");
        require(model.image(0) == expected.image(0),
                "Normalized author, inline, inherited or pseudo-element colors differ from neutral text");
        require(model.position() == anchor, "Text color normalization moved the anchor");
        model.resize({ 900, 1000 });
        expected.resize({ 900, 1000 });
        require(model.image(0) == expected.image(0), "Reflow lost normalized text colors");
        model.resize({ 800, 1000 });
        expected.resize({ 800, 1000 });
        require(model.applyAppearance(appearance::Patch({ { "normalize_text_colors", false } })),
                "Cannot disable text color normalization");
        require(model.image(0) == publisher,
                "Disabling normalization did not restore publisher colors");
    }
    require(model.image(0) == original, "Color toggles changed the original light page");
    require(model.applyAppearance(appearance::Patch({ { "normalize_text_colors", true } })),
            "Cannot save normalized appearance");
    const auto saved = model.state();
    const auto normalized = model.image(0);
    require(normalized != original, "Color fixture did not exercise normalization");
    require(model.openResources(source, sizes, { 800, 1000 }, saved, &error), qPrintable(error));
    require(model.openedFromCache() && model.image(0) == normalized && model.position() == anchor,
            "Cached reopen lost normalized text colors or reading anchor");
    model.setDarkMode(true);
    expected.setDarkMode(true);
    require(model.image(0) == expected.image(0), "Dark-mode toggle lost active normalization");
    model.setDarkMode(false);
    require(model.image(0) == normalized, "Light-mode toggle lost active normalization");
    require(model.saveDocumentCache(), "Cannot save normalized color cache");
    require(model.openResources(source, sizes, { 800, 1000 }, {}, &error), qPrintable(error));
    require(model.openedFromCache() && model.image(0) == original,
            "Reopening normalized cache with rule off lost publisher colors");

    QImage raster(60, 90, QImage::Format_RGB32);
    raster.fill(QColor("#cc2200"));
    QByteArray png;
    QBuffer buffer(&png);
    require(buffer.open(QIODevice::WriteOnly) && raster.save(&buffer, "PNG"),
            "Cannot encode image fixture");
    const auto svg = QByteArray("<svg xmlns='http://www.w3.org/2000/svg' width='60' height='90'>"
                                "<rect width='60' height='90' fill='#224466'/></svg>")
                             .toBase64();
    const auto images = fixture(
            "<p><img src='data:image/png;base64," + png.toBase64()
                    + "'/><img src='data:image/svg+xml;base64," + svg
                    + "'/></p><svg xmlns='http://www.w3.org/2000/svg' width='60' height='90' style='color:#cc2200'><rect width='60' height='90' fill='currentColor'/></svg><div style='color:#224466'><svg xmlns='http://www.w3.org/2000/svg' width='60' height='90'><rect width='60' height='90' fill='currentColor'/></svg></div>",
            sizes);
    EngineDocument vertical(test::readerFonts()), verticalReference(test::readerFonts());
    QMap<QString, qint64> verticalSizes, verticalReferenceSizes;
    const auto verticalSource =
            fixture(body, verticalSizes, css + "body {writing-mode:vertical-rl}");
    const auto verticalExpected = fixture(referenceBody, verticalReferenceSizes,
                                          referenceCss + "body {writing-mode:vertical-rl}");
    for (bool dark : { false, true }) {
        require(vertical.openResources(verticalSource, verticalSizes, { 800, 1000 },
                                       { { "dark_mode", dark }, { "normalize_text_colors", true } },
                                       &error),
                qPrintable(error));
        require(verticalReference.openResources(verticalExpected, verticalReferenceSizes,
                                                { 800, 1000 }, { { "dark_mode", dark } }, &error),
                qPrintable(error));
        require(vertical.image(0) == verticalReference.image(0),
                "Vertical text or decorations did not use normalized ink");
    }
    EngineDocument imageModel(test::readerFonts());
    for (bool dark : { false, true }) {
        require(imageModel.openResources(images, sizes, { 800, 1000 }, { { "dark_mode", dark } },
                                         &error),
                qPrintable(error));
        const auto before = imageModel.image(0);
        require(imageModel.applyAppearance(appearance::Patch({ { "normalize_text_colors", true } }))
                        && imageModel.image(0) == before,
                "Text color normalization changed raster or vector image pixels");
    }
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc == 5 && QByteArray(argv[1]) == "--compare") {
        compare(QString::fromLocal8Bit(argv[2]), QString::fromUtf8(argv[3]),
                QString::fromLocal8Bit(argv[4]));
        return 0;
    }
    if (argc == 5 && QByteArray(argv[1]) == "--compare-layout") {
        compareLayout(QString::fromLocal8Bit(argv[2]), QString::fromUtf8(argv[3]),
                      QString::fromLocal8Bit(argv[4]));
        return 0;
    }
    checkTextColors();
    const QByteArray prose =
            " ordinary words continue through the paragraph, with enough text to distinguish prose from a heading or separator. The words keep their original order and their source anchors.";
    QString error;
    for (bool dark : { false, true })
        for (bool wrapped : { false, true }) {
            const auto wrap = [wrapped](const QByteArray &value, int id) {
                return wrapped ? "<span class='koboSpan' id='kobo." + QByteArray::number(id)
                                + ".1'>" + value + "</span>"
                               : value;
            };
            QMap<QString, qint64> sizes;
            const auto body = "<h2>Chapter</h2><p><span class='dropcap'>" + wrap("Z", 1) + "</span>"
                    + wrap("etamarker <a href='#destination'>Reference</a>" + prose, 2)
                    + "</p><p id='destination'>" + wrap("Betamarker" + prose, 3) + "</p>";
            const auto source = fixture(body, sizes);
            QTemporaryDir cache(QDir::currentPath() + "/beautifier-cache-XXXXXX");
            require(cache.isValid(), "Cannot create cache");
            EngineDocument model(test::readerFonts());
            model.setDocumentCache(cache.path(), QByteArray(64, 'b'), "beautifier", 1024 * 1024);
            require(model.openResources(source, sizes, { 1072, 1448 }, { { "dark_mode", dark } },
                                        &error),
                    qPrintable(error));
            const auto before = model.image(0);
            const auto hit = find(model, "Betamarker");
            const auto links = model.pageLinks();
            require(links.size() == 1, "Fixture link has no hit region");
            const auto target = model.linkDetails(links.first().source, false).target;
            require(!target.isEmpty(), "Fixture link target did not resolve");
            require(!model.state()["beautifier_initials"].toBool(), "Beautifier is not opt-in");
            require(model.applyAppearance(
                            kobalt::appearance::Patch({ { "beautifier_initials", true },
                                                        { "beautifier_spacing", true },
                                                        { "beautifier_indent", true } })),
                    "Cannot enable beautifier");
            require(model.image(0) != before, "Beautifier did not change prose or initial");
            EngineDocument normalInitial(test::readerFonts());
            require(normalInitial.openResources(
                            fixture(body, sizes, ".dropcap{font-size:1em;float:none;margin:0}"),
                            sizes, { 1072, 1448 },
                            { { "dark_mode", dark },
                              { "beautifier_initials", true },
                              { "beautifier_spacing", true },
                              { "beautifier_indent", true } },
                            &error),
                    qPrintable(error));
            require(model.image(0) == normalInitial.image(0),
                    "Text initial differs from ordinary inline text");
            EngineDocument unnamedInitial(test::readerFonts());
            auto unnamedBody = body;
            unnamedBody.replace("dropcap", "big");
            require(unnamedInitial.openResources(
                            fixture(unnamedBody, sizes,
                                    ".big{float:left;font-size:200%;margin-top:-.5em;margin-bottom:-.2em}"),
                            sizes, { 1072, 1448 }, { { "dark_mode", dark } }, &error),
                    qPrintable(error));
            const auto unnamedBefore = unnamedInitial.image(0);
            require(unnamedInitial.applyAppearance(
                            kobalt::appearance::Patch({ { "beautifier_initials", true },
                                                        { "beautifier_spacing", true },
                                                        { "beautifier_indent", true } }))
                            && unnamedInitial.image(0) == normalInitial.image(0),
                    "Generic floated initial was not normalized");
            require(unnamedInitial.applyAppearance(
                            kobalt::appearance::Patch({ { "beautifier_initials", false },
                                                        { "beautifier_spacing", false },
                                                        { "beautifier_indent", false } }))
                            && unnamedInitial.image(0) == unnamedBefore,
                    "Generic initial did not restore its original styling");
            require(model.textRange(hit.start, hit.end).text == "Betamarker",
                    "Source range changed");
            const auto styledLinks = model.pageLinks();
            require(styledLinks.size() == 1 && styledLinks.first().source == links.first().source
                            && model.linkDetails(styledLinks.first().source, false).target
                                    == target,
                    "Beautifier changed the link source or target ID");
            require(model.saveDocumentCache(), "Cannot save beautifier cache");
            EngineDocument restored(test::readerFonts());
            restored.setDocumentCache(cache.path(), QByteArray(64, 'b'), "beautifier", 1024 * 1024);
            require(restored.openResources(source, sizes, { 1072, 1448 }, model.state(), &error),
                    qPrintable(error));
            require(restored.openedFromCache() && restored.image(0) == model.image(0),
                    "Warm cache changed beautifier rendering");
            EngineDocument preview(test::readerFonts());
            require(preview.openPreview(model, { 1072, 1448 }, &error)
                            && preview.image(0) == model.image(0),
                    "Preview differs");
            require(restored.applyAppearance(
                            kobalt::appearance::Patch({ { "beautifier_initials", false },
                                                        { "beautifier_spacing", false },
                                                        { "beautifier_indent", false } }))
                            && restored.image(0) == before,
                    "Off did not restore pixels");
            require(!restored.applyAppearance(
                            kobalt::appearance::Patch({ { "beautifier_initials", "on" } })),
                    "Invalid setting accepted");
            for (const auto &special :
                 { QByteArray("<p class='quote'>"), QByteArray("<p class='letter'>"),
                   QByteArray("<p style='margin-top:2em'>"),
                   QByteArray("<p style='text-indent:-1em;margin-left:2em'>"),
                   QByteArray("<p style='text-align:right'>") }) {
                EngineDocument protectedBook(test::readerFonts());
                require(protectedBook.openResources(
                                fixture(special + "Protectedmarker" + prose + "</p>", sizes), sizes,
                                { 1072, 1448 }, { { "dark_mode", dark } }, &error),
                        qPrintable(error));
                const auto original = protectedBook.image(0);
                require(protectedBook.applyAppearance(
                                kobalt::appearance::Patch({ { "beautifier_initials", true },
                                                            { "beautifier_spacing", true },
                                                            { "beautifier_indent", true } }))
                                && protectedBook.image(0) == original,
                        "Beautifier changed a protected passage");
            }
        }
    // Initials do not depend on prose classification, paragraph length or position.
    for (const auto &body :
         { QByteArray(
                   "<p class='subtitle'>A date before the opening</p><p style='margin-top:1.2em'><span class='big'>Z</span>etamarker")
                   + prose + "</p>",
           QByteArray(
                   "<blockquote><p style='margin-top:1.2em'><span class='big'>Z</span>etamarker</p></blockquote>"),
           QByteArray("<p>Earlier words. <span class='big'>Z</span>etamarker") + prose + "</p>" }) {
        QMap<QString, qint64> sizes;
        EngineDocument decorated(test::readerFonts()), reference(test::readerFonts());
        require(decorated.openResources(
                        fixture(body, sizes,
                                ".big{float:left;font-size:200%;margin-top:-.5em;margin-bottom:-.2em}"),
                        sizes, { 1072, 1448 }, {}, &error),
                qPrintable(error));
        const auto before = decorated.image(0);
        require(reference.openResources(fixture(body, sizes), sizes, { 1072, 1448 }, {}, &error),
                qPrintable(error));
        require(before != reference.image(0), "Initial fixture has no visible decoration");
        require(decorated.applyAppearance(
                        kobalt::appearance::Patch({ { "beautifier_initials", true } }))
                        && decorated.image(0) == reference.image(0),
                "Initial normalization depends on paragraph position or classification");
        require(decorated.applyAppearance(
                        kobalt::appearance::Patch({ { "beautifier_initials", false } }))
                        && decorated.image(0) == before,
                "Initial normalization did not restore publisher styling");
    }
    // Explicit paragraph controls take precedence over beautifier defaults.
    QMap<QString, qint64> sizes;
    const auto ordinary =
            fixture("<p>Firstmarker" + prose + "</p><p>Secondmarker" + prose + "</p>", sizes);
    EngineDocument explicitLayout(test::readerFonts());
    require(explicitLayout.openResources(
                    ordinary, sizes, { 1072, 1448 },
                    { { "paragraph_indent", 200 }, { "paragraph_spacing", 50 } }, &error),
            qPrintable(error));
    const auto explicitImage = explicitLayout.image(0);
    require(explicitLayout.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_initials", true },
                                                { "beautifier_spacing", true },
                                                { "beautifier_indent", true } }))
                    && explicitLayout.image(0) == explicitImage,
            "Beautifier overrode explicit paragraph controls");

    // Each global rule changes only its own aspect of ordinary prose.
    EngineDocument independent(test::readerFonts());
    require(independent.openResources(ordinary, sizes, { 1072, 1448 }, {}, &error),
            qPrintable(error));
    const auto unchanged = independent.image(0);
    require(independent.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_initials", false },
                                                { "beautifier_spacing", false },
                                                { "beautifier_indent", false } }))
                    && independent.image(0) == unchanged,
            "Disabled rules still changed the book");
    require(independent.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_initials", true } }))
                    && independent.image(0) == unchanged,
            "Initial-only normalization changed ordinary prose");
    require(independent.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_spacing", true } }))
                    && independent.image(0) != unchanged,
            "Paragraph spacing rule did not apply independently");
    const auto spacingOnly = independent.image(0);
    require(independent.applyAppearance(kobalt::appearance::Patch(
                    { { "beautifier_spacing", false }, { "beautifier_indent", true } }))
                    && independent.image(0) != unchanged && independent.image(0) != spacingOnly,
            "Indentation rule did not apply independently");

    // A CSS first letter uses an engine pseudo-element, not a source span.
    const auto firstLetterBody = "<p class='stickupcaps'>Firstmarker" + prose + "</p>";
    EngineDocument largeLetter(test::readerFonts()), normalLetter(test::readerFonts());
    require(largeLetter.openResources(
                    fixture(firstLetterBody, sizes, "p::first-letter{font-size:3em;float:left}"),
                    sizes, { 1072, 1448 }, {}, &error),
            qPrintable(error));
    const auto largeImage = largeLetter.image(0);
    require(normalLetter.openResources(fixture(firstLetterBody, sizes), sizes, { 1072, 1448 },
                                       { { "beautifier_initials", true },
                                         { "beautifier_spacing", true },
                                         { "beautifier_indent", true } },
                                       &error),
            qPrintable(error));
    require(largeLetter.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_initials", true },
                                                { "beautifier_spacing", true },
                                                { "beautifier_indent", true } }))
                    && largeLetter.image(0) != largeImage
                    && largeLetter.image(0) == normalLetter.image(0),
            "CSS first letter was not normalized");
    const QByteArray spacedLetterBody =
            "<p class='subtitle'>A date before the opening</p><blockquote><p style='margin-top:1.2em'>Firstmarker</p></blockquote>";
    QTemporaryDir initialCache(QDir::currentPath() + "/initial-cache-XXXXXX");
    require(initialCache.isValid(), "Cannot create initial cache");
    EngineDocument spacedLetter(test::readerFonts()), plainLetter(test::readerFonts());
    spacedLetter.setDocumentCache(initialCache.path(), QByteArray(64, 'c'), "initial", 1024 * 1024);
    require(spacedLetter.openResources(
                    fixture(spacedLetterBody, sizes,
                            "blockquote p::first-letter{font-size:3em;float:left}"),
                    sizes, { 1072, 1448 }, {}, &error),
            qPrintable(error));
    require(plainLetter.openResources(
                    fixture(spacedLetterBody, sizes, "blockquote p::first-letter{font-size:1em}"),
                    sizes, { 1072, 1448 }, {}, &error),
            qPrintable(error));
    const auto spacedBefore = spacedLetter.image(0);
    require(spacedBefore != plainLetter.image(0), "CSS initial fixture has no visible decoration");
    require(spacedLetter.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_initials", true } }))
                    && spacedLetter.image(0) == plainLetter.image(0),
            "CSS initial normalization depends on paragraph classification or class name");
    require(spacedLetter.saveDocumentCache(), "Cannot save initial cache");
    EngineDocument cachedLetter(test::readerFonts());
    cachedLetter.setDocumentCache(initialCache.path(), QByteArray(64, 'c'), "initial", 1024 * 1024);
    require(cachedLetter.openResources(
                    fixture(spacedLetterBody, sizes,
                            "blockquote p::first-letter{font-size:3em;float:left}"),
                    sizes, { 1072, 1448 }, spacedLetter.state(), &error),
            qPrintable(error));
    require(cachedLetter.openedFromCache() && cachedLetter.image(0) == spacedLetter.image(0),
            "Warm cache lost independent CSS initial normalization");
    require(spacedLetter.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_initials", false } }))
                    && spacedLetter.image(0) == spacedBefore,
            "CSS initial normalization did not restore publisher styling");
    const auto image =
            QByteArray(
                    "<svg xmlns='http://www.w3.org/2000/svg' width='60' height='90'><rect width='60' height='90'/></svg>")
                    .toBase64();
    EngineDocument imageInitial(test::readerFonts());
    require(imageInitial.openResources(
                    fixture("<p><span class='dropcap'><img src='data:image/svg+xml;base64," + image
                                    + "'/></span>Image marker" + prose + "</p>",
                            sizes),
                    sizes, { 1072, 1448 }, {}, &error),
            qPrintable(error));
    const auto imageBefore = imageInitial.image(0);
    require(imageInitial.applyAppearance(
                    kobalt::appearance::Patch({ { "beautifier_initials", true } }))
                    && imageInitial.image(0) == imageBefore,
            "Text-initial normalization changed an image initial");
    QJsonObject prefs;
    appearance::rememberTextLayout(prefs,
                                   { { "font_pixels", 44 },
                                     { "beautifier_initials", true },
                                     { "beautifier_spacing", true },
                                     { "beautifier_indent", true } });
    require(!appearance::bookDefaults(prefs).value("beautifier_initials").toBool(),
            "Beautifier leaked through inheritance");
    require(appearance::globalPreferences({ { "beautifier_initials", true },
                                            { "beautifier_spacing", true },
                                            { "beautifier_indent", true } })
                    .value("beautifier_initials")
                    .toBool(),
            "Global beautifier was discarded");
}
