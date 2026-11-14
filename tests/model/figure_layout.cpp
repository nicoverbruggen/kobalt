#include "check.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include "engine/crengine/column_view.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

using namespace kobalt;

namespace kobalt {
struct ReaderFigureProbe
{
    static css_page_break_t breakInside(EngineDocument &model, const QString &anchor)
    {
        auto *doc = model.engine_->getDocument();
        const auto pointer = doc->createXPointer(Utf8ToUnicode(anchor.toUtf8().constData()));
        for (auto *node = pointer.getNode(); node; node = node->getParentNode()) {
            if (node->isElement() && node->isNodeName("figure"))
                return node->getStyle()->page_break_inside;
        }
        require(false, "Caption has no figure ancestor");
        return css_pb_auto;
    }
};
} // namespace kobalt

namespace {
// Search yields after its time slice. Collect later batches before checking layout.
QVector<SearchHit> search(EngineDocument &model, const QString &query, int maximum)
{
    QVector<SearchHit> hits;
    SearchCursor cursor;
    for (int batches = 0; hits.size() < maximum; ++batches) {
        require(batches < 10000, "Figure search did not finish");
        const auto batch = model.searchText(query, cursor, maximum - hits.size());
        hits += batch.hits;
        cursor = batch.cursor;
        if (batch.done)
            break;
    }
    return hits;
}

QVector<QRect> imageRegions(const QImage &image)
{
    QVector<QRect> regions;
    for (const auto &value :
         QJsonDocument::fromJson(image.text("kobalt.image_regions").toUtf8()).array()) {
        const auto r = value.toArray();
        regions.append(QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt()));
    }
    return regions;
}

struct Fixture
{
    const char *name;
    int leadingHeight;
    QByteArray figureStyle, captionStyle;
    int imageHeight = 200;
    int captionLines = 1;
    bool together = true;
    bool figure = true;
    bool nested = false;
    bool captionFirst = false;
    QByteArray ancestorStyle{};
    bool unchanged = false;
};

Fixture nonBlockFixture(const char *name, QByteArray figureStyle, QByteArray ancestorStyle = {})
{
    Fixture fixture{ name, 580, std::move(figureStyle), {} };
    fixture.ancestorStyle = std::move(ancestorStyle);
    fixture.unchanged = true;
    return fixture;
}

// One source chapter with explicit line boxes makes the page boundary repeatable.
void check(const Fixture &fixture, int columns, bool dark, const QByteArray &png,
           bool keepMargins = false, bool stylesheetLayout = false)
{
    QByteArray caption = "<a href='#after'>Caption marker</a>";
    for (int i = 1; i < fixture.captionLines; ++i)
        caption += "<br/>Caption continuation " + QByteArray::number(i);
    if (fixture.captionLines)
        caption += "<br/>Caption end";
    const QByteArray image = "<img src='art.png' style='width:200px;height:"
            + QByteArray::number(fixture.imageHeight) + "px'/>";
    const QByteArray captionTag = fixture.figure ? "figcaption" : "p";
    const QByteArray captionMarkup = "<" + captionTag + " class='caption' style='"
            + fixture.captionStyle + "'>" + caption + "</" + captionTag + ">";
    QByteArray contents = fixture.captionFirst ? captionMarkup + image : image + captionMarkup;
    if (fixture.nested)
        contents = "<div>" + contents + "</div>";
    const auto figureStyle = stylesheetLayout ? QByteArray() : fixture.figureStyle;
    const auto ancestorStyle = stylesheetLayout ? QByteArray() : fixture.ancestorStyle;
    const auto layoutCss = stylesheetLayout
            ? "#figure{" + fixture.figureStyle + "}#container{" + fixture.ancestorStyle + "}"
            : QByteArray();
    QByteArray group = fixture.figure
            ? "<figure id='figure' style='" + figureStyle + "'>" + contents + "</figure>"
            : contents;
    if (!fixture.ancestorStyle.isEmpty())
        group = "<div id='container' style='" + ancestorStyle + "'>" + group + "</div>";
    const QByteArray body = "<p style='height:" + QByteArray::number(fixture.leadingHeight)
            + "px'>Before figure.</p>" + group + "<p id='after'>After figure.</p>";
    QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata/><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/><item id='i' href='art.png' media-type='image/png'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "art.png", png },
        { "c.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p,figure,figcaption{margin:0;padding:0;text-indent:0}figcaption,.caption{font-size:24px}"
                  + layoutCss + "</style></head><body>" + body + "</body></html>" }
    };
    QMap<QString, qint64> lengths;
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        lengths[it.key()] = it.value().size();
    auto resources = std::make_shared<ResourceCache>(
            data.keys(),
            [data](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    QTemporaryDir cache;
    require(cache.isValid(), "Create figure cache");
    const auto fingerprint =
            QCryptographicHash::hash(data["c.xhtml"], QCryptographicHash::Sha256).toHex();
    EngineDocument model(test::readerFonts());
    model.setDocumentCache(cache.path(), fingerprint, "figure-layout", 1024 * 1024);
    const double scale = readingPpi.load() / 96.0;
    const QSize viewport(qRound(columns * 600 * scale), qRound(800 * scale));
    const QJsonObject settings{ { "font_pixels", qRound(24 * scale) },
                                { "margin_pixels", 0 },
                                { "top_margin_pixels", 0 },
                                { "bottom_margin_pixels", 0 },
                                { "columns", columns },
                                { "column_spacing", 0 },
                                { "dark_mode", dark },
                                { "reduce_chapter_spacing", !keepMargins } };
    QString error;
    require(model.openResources(resources, lengths, viewport, settings, &error), qPrintable(error));
    const auto hits = search(model, "Caption marker", 10);
    require(hits.size() == 1, "Caption text missing or duplicated");
    const auto anchor = hits.first().start;
    const auto end = search(model, fixture.captionLines ? "Caption end" : "Caption marker", 10);
    const auto after = search(model, "After figure", 10);
    require(end.size() == 1 && after.size() == 1, "End of caption or following prose lost");
    QVector<int> sourceOrder;
    const auto pages =
            model.pagesOf({ anchor, end.first().start, after.first().start }, false, &sourceOrder);
    require(sourceOrder == QVector<int>({ 0, 1, 2 }), "Figure changed source order");
    if (!fixture.unchanged)
        require(pages[0] <= pages[1] && pages[1] <= pages[2], "Figure reordered visible prose");
    require(model.pages() > 0 && model.pages() < 12, "Figure created excessive pages");
    int imagePage = -1;
    QRect imageRect;
    QVector<QImage> rendered;
    const auto capture = qEnvironmentVariable("KOBALT_TEST_FIGURE_CAPTURE_DIR");
    for (int page = 0; page < model.pages(); ++page) {
        require(model.go(page), "Go to figure page");
        const auto image = model.image(page);
        const auto regions = imageRegions(image);
        if (!regions.isEmpty()) {
            require(imagePage < 0 && regions.size() == 1, "Figure image duplicated");
            imagePage = page;
            imageRect = regions.first();
        }
        rendered.append(image);
        if (!capture.isEmpty() && readingPpi.load() == 96 && !keepMargins) {
            require(QDir().mkpath(capture), "Create figure captures");
            const auto name = QString("%1/%2-%3-%4-page-%5.png")
                                      .arg(capture, fixture.name)
                                      .arg(columns)
                                      .arg(dark)
                                      .arg(page);
            require(image.save(name), "Save figure capture");
        }
    }
    require(imagePage >= 0 && !imageRect.isEmpty(), "Missing figure image");
    require(qAbs(imageRect.width() - qRound(200 * scale)) <= 1
                    && qAbs(imageRect.height() - qRound(fixture.imageHeight * scale)) <= 1,
            "Figure changed authored image dimensions");
    require(model.go(pages[0]), "Go to caption");
    const auto rects = model.rangeRects(hits.first().start, hits.first().end);
    require(!rects.isEmpty(), "Caption selection lost");
    require(model.go(pages[1]), "Go to caption end");
    const auto endRects = model.rangeRects(end.first().start, end.first().end);
    require(!endRects.isEmpty(), "Caption end selection lost");
    const int endColumn = pages[1] * columns
            + (columns == 2 && endRects.first().center().x() >= viewport.width() / 2);
    require(model.go(pages[0]), "Return to caption");
    const int imageColumn =
            imagePage * columns + (columns == 2 && imageRect.center().x() >= viewport.width() / 2);
    const int captionColumn = pages[0] * columns
            + (columns == 2 && rects.first().center().x() >= viewport.width() / 2);
    std::fprintf(stderr, "%s columns=%d dark=%d: image column=%d caption column=%d pages=%d\n",
                 fixture.name, columns, dark, imageColumn, captionColumn, model.pages());
    if (fixture.unchanged) {
        require(ReaderFigureProbe::breakInside(model, anchor) == css_pb_auto,
                "Default keep-together leaked into a non-block layout");
        EngineDocument reference(test::readerFonts());
        reference.setUserStyleSheet("figure { page-break-inside:auto !important; }");
        require(reference.openResources(resources, lengths, viewport, settings, &error),
                qPrintable(error));
        require(reference.pages() == model.pages(), "Non-block pagination changed");
        for (int page = 0; page < model.pages(); ++page)
            require(reference.image(page) == rendered[page], "Non-block figure pixels changed");
    } else if (fixture.together)
        require(imageColumn == captionColumn && captionColumn == endColumn,
                "Fitting figure separated from its caption");
    else
        require(imageColumn < captionColumn || captionColumn < endColumn,
                "Expected figure split did not occur");
    if (QByteArray(fixture.name) == "forced-figure") {
        require(imageColumn == 1, "Publisher figure break-before lost");
        require(model.go(pages[2]), "Go to following prose");
        const auto afterRects = model.rangeRects(after.first().start, after.first().end);
        require(!afterRects.isEmpty(), "Following prose selection lost");
        const int afterColumn = pages[2] * columns
                + (columns == 2 && afterRects.first().center().x() >= viewport.width() / 2);
        require(afterColumn == 2, "Publisher figure break-after lost");
        require(model.go(pages[0]), "Return to caption link");
    }
    for (int i = 1; i < fixture.captionLines; ++i) {
        const auto continuation = search(model, "Caption continuation " + QString::number(i), 1);
        require(!continuation.isEmpty(), "Caption continuation lost");
        require(model.go(continuation.first().page), "Go to caption continuation");
        const auto continuationRects =
                model.rangeRects(continuation.first().start, continuation.first().end);
        require(!continuationRects.isEmpty(), "Caption continuation not drawn");
        for (const auto &rect : continuationRects)
            require(QRect(QPoint(), viewport).contains(rect),
                    "Caption clipped outside the viewport");
    }
    require(model.go(pages[0]), "Return to caption hit test");
    require(model.wordAt(rects.first().center()).start == hits.first().start,
            "Caption hit test lost its anchor");
    require(model.annotationGeometry(hits.first().start, hits.first().end).rectangles == rects,
            "Caption highlight geometry differs");
    const auto links = model.pageLinks();
    require(!links.isEmpty() && !model.linkDetails(links.first().source, false).target.isEmpty(),
            "Caption link lost");
    require(model.goTo(anchor), "Cannot restore caption anchor");
    EngineDocument preview(test::readerFonts());
    require(preview.openPreview(model, viewport, &error)
                    && preview.image(pages[0]) == rendered[pages[0]],
            "Figure preview differs");
    require(model.saveDocumentCache(), "Save figure cache");
    EngineDocument reopened(test::readerFonts());
    reopened.setDocumentCache(cache.path(), fingerprint, "figure-layout", 1024 * 1024);
    require(reopened.openResources(resources, lengths, viewport, settings, &error)
                    && reopened.openedFromCache(),
            "Figure cache miss");
    for (int page = 0; page < model.pages(); ++page)
        require(reopened.image(page) == rendered[page], "Cached figure page differs");
    require(model.applyAppearance(appearance::Patch(QJsonObject{}),
                                  QSize(viewport.width(), qRound(1000 * scale)))
                    && model.goTo(anchor) && model.pagesOf({ anchor }).first() >= 0,
            "Reflow lost caption anchor");
    require(model.applyAppearance(appearance::Patch(QJsonObject{}), viewport),
            "Restore figure viewport");
    for (int page = 0; page < model.pages(); ++page)
        require(model.image(page) == rendered[page], "Reflow did not restore figure layout");
}

// Exercise the saved-id capacity and the rectangle fallback with ordinary floats.
void floatFootprints()
{
    for (const auto *side : { "left", "right" })
        for (int count : { 0, 1, 5, 6 }) {
            QByteArray body;
            for (int i = 0; i < count; ++i)
                body += "<div class='float'></div>";
            body += "<p>";
            for (int i = 0; i < 24; ++i)
                body += "Boundary ";
            body += "</p>";
            QMap<QString, QByteArray> data{
                { "mimetype", "application/epub+zip" },
                { "META-INF/container.xml",
                  "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
                { "book.opf",
                  "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata/><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
                { "c.xhtml",
                  "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}.float{float:"
                          + QByteArray(side)
                          + ";width:60px;height:160px;background:red}</style></head><body>" + body
                          + "</body></html>" }
            };
            QMap<QString, qint64> lengths;
            for (auto it = data.cbegin(); it != data.cend(); ++it)
                lengths[it.key()] = it.value().size();
            auto resources = std::make_shared<ResourceCache>(
                    data.keys(),
                    [data](const QString &name, const ResourceCache::Sink &sink, QString *) {
                        const auto bytes = data.value(name);
                        return sink(bytes.constData(), bytes.size());
                    },
                    std::make_shared<std::atomic_bool>(false));
            QTemporaryDir cache;
            require(cache.isValid(), "Create float footprint cache");
            EngineDocument model(test::readerFonts());
            model.setDocumentCache(cache.path(), "float-footprints", "float-layout", 1024 * 1024);
            const QJsonObject settings{ { "font_pixels", 24 },
                                        { "margin_pixels", 0 },
                                        { "top_margin_pixels", 0 },
                                        { "bottom_margin_pixels", 0 } };
            QString error;
            require(model.openResources(resources, lengths, { 600, 800 }, settings, &error),
                    qPrintable(error));
            require(model.pages() == 1, "Float footprint fixture unexpectedly paginated");
            const auto image = model.image(0);
            const auto hits = search(model, "Boundary", 100);
            require(hits.size() == 24, "Float footprint lost or duplicated text");
            const bool right = QByteArray(side) == "right";
            const QRect occupied(right ? 600 - count * 60 : 0, 0, count * 60, 160);
            for (const auto &hit : hits)
                for (const auto &rect : model.rangeRects(hit.start, hit.end))
                    require(!rect.intersects(occupied),
                            "Saved float footprint lost during painting");
            require(model.saveDocumentCache(), "Save float footprint cache");
            EngineDocument reopened(test::readerFonts());
            reopened.setDocumentCache(cache.path(), "float-footprints", "float-layout",
                                      1024 * 1024);
            require(reopened.openResources(resources, lengths, { 600, 800 }, settings, &error)
                            && reopened.openedFromCache() && reopened.image(0) == image,
                    "Saved float footprint changed on cache reopening");
        }
}

// MobileRead post 4622373: keep the author's float, inline-block and viewport limits.
void reporterFigure(int columns, bool dark, const QByteArray &css = {}, bool fullWidth = true,
                    const QString &capture = {})
{
    QFile file(QFileInfo(QStringLiteral(KOBALT_TEST_FIGURE_BOOK))
                       .dir()
                       .filePath("reporter-figure-float.epub"));
    require(file.open(QIODevice::ReadOnly), "Read reporter figure fixture");
    const auto bytes = file.readAll();
    require(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                    == "79faf982bf7cd77721b978da1701b70329ed6bfe624771cf8ef0738540e2f36e",
            "Reporter figure fixture changed");
    const double scale = readingPpi.load() / 96.0;
    const QSize viewport(qRound(600 * columns * scale), qRound(800 * scale));
    const QJsonObject settings{ { "font_pixels", qRound(22 * scale) },
                                { "margin_pixels", qRound(40 * scale) },
                                { "top_margin_pixels", qRound(40 * scale) },
                                { "bottom_margin_pixels", qRound(40 * scale) },
                                { "columns", columns },
                                { "column_spacing", 0 },
                                { "dark_mode", dark },
                                { "line_spacing", 150 },
                                { "line_spacing_version", 1 } };
    QTemporaryDir cache;
    require(cache.isValid(), "Create reporter figure cache");
    EngineDocument model(test::readerFonts());
    model.setUserStyleSheet(css);
    model.setDocumentCache(cache.path(), "reporter-figure", "float-layout", 1024 * 1024);
    QString error;
    require(model.open(bytes, viewport, settings, &error), qPrintable(error));
    const auto caption = search(model, "Mr. Pickwick Addresses the Club", 1);
    const auto following = search(model, "Mr. Pickwick observed", 1);
    const auto prose = search(model, "The members gathered around the table", 100);
    require(caption.size() == 1 && following.size() == 1 && prose.size() == 8,
            "Reporter figure lost caption or following prose");
    QVector<QImage> rendered;
    int imagePage = -1;
    bool wrappedProse = false;
    QRect imageRect;
    for (int page = 0; page < model.pages(); ++page) {
        require(model.go(page), "Go to reporter figure page");
        const auto image = model.image(page);
        const auto regions = imageRegions(image);
        if (!regions.isEmpty()) {
            require(imagePage < 0 && regions.size() == 1, "Reporter image split or duplicated");
            imagePage = page;
            imageRect = regions.first();
            require(qAbs(imageRect.height() - imageRect.width() * 1.5) <= 2,
                    "Reporter image lost its aspect ratio");
            require(caption.first().page == page, "Reporter caption detached from fitting image");
            for (const auto &hit : prose) {
                if (hit.page != page)
                    continue;
                for (const auto &rect : model.rangeRects(hit.start, hit.end)) {
                    require(!rect.intersects(imageRect),
                            "Reporter float paints over following prose");
                    const bool sameColumn = columns == 1
                            || (rect.center().x() >= viewport.width() / 2)
                                    == (imageRect.center().x() >= viewport.width() / 2);
                    if (sameColumn && rect.top() < imageRect.bottom()
                        && rect.bottom() > imageRect.top())
                        wrappedProse = true;
                }
            }
            const auto captionRects = model.rangeRects(caption.first().start, caption.first().end);
            require(!captionRects.isEmpty(), "Reporter caption selection lost");
            for (const auto &rect : captionRects)
                require(!rect.intersects(imageRect), "Reporter caption overlaps its image");
            if (fullWidth && following.first().page == page) {
                const auto rects = model.rangeRects(following.first().start, following.first().end);
                require(!rects.isEmpty(), "Reporter prose selection lost");
                const auto cap = captionRects.last();
                const auto text = rects.first();
                const int capColumn = columns == 2 && cap.center().x() >= viewport.width() / 2;
                const int textColumn = columns == 2 && text.center().x() >= viewport.width() / 2;
                require(textColumn > capColumn
                                || (textColumn == capColumn && text.top() >= cap.bottom()),
                        "Following prose moved ahead of full-width figure");
            }
        }
        rendered.append(image);
        if (!capture.isEmpty()) {
            require(QDir().mkpath(capture), "Create reporter captures");
            require(image.save(capture + "/page-" + QString::number(page + 1) + ".png"),
                    "Save reporter capture");
        }
    }
    require(imagePage >= 0, "Reporter image missing");
    require(fullWidth || wrappedProse, "Narrow reporter float no longer wraps prose");
    require(model.saveDocumentCache(), "Save reporter cache");
    EngineDocument reopened(test::readerFonts());
    reopened.setUserStyleSheet(css);
    reopened.setDocumentCache(cache.path(), "reporter-figure", "float-layout", 1024 * 1024);
    require(reopened.open(bytes, viewport, settings, &error) && reopened.openedFromCache(),
            "Reporter cache miss");
    for (int page = 0; page < model.pages(); ++page)
        require(reopened.image(page) == rendered[page], "Cached reporter page differs");
    require(model.applyAppearance(appearance::Patch(QJsonObject{}),
                                  QSize(viewport.width(), qRound(1000 * scale)))
                    && model.goTo(caption.first().start),
            "Reporter reflow lost caption anchor");
    require(model.applyAppearance(appearance::Patch(QJsonObject{}), viewport),
            "Restore reporter viewport");
    for (int page = 0; page < model.pages(); ++page)
        require(model.image(page) == rendered[page], "Reporter reflow changed restored pages");
}

void reporterFigures(int columns, bool dark)
{
    reporterFigure(columns, dark);
    reporterFigure(columns, dark, "figure{float:right !important}");
    reporterFigure(columns, dark, "figure{width:50% !important}", false);
    reporterFigure(columns, dark, "p:first-child{height:350px}");
}

// Uses the accepted review book in the regular suite; a directory also keeps captures.
void reviewDocument(const QString &directory = {}, const QString &existingBook = {},
                    bool split = false)
{
    const QSize viewport(600, 800);
    const QJsonObject settings{ { "font_pixels", 22 },       { "margin_pixels", 40 },
                                { "top_margin_pixels", 40 }, { "bottom_margin_pixels", 40 },
                                { "line_spacing", 150 },     { "line_spacing_version", 1 } };
    QFile input(existingBook.isEmpty() ? QStringLiteral(KOBALT_TEST_FIGURE_BOOK) : existingBook);
    require(input.open(QIODevice::ReadOnly), "Read figure review fixture");
    const auto bytes = input.readAll();
    require(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                    == "e7238b5e9a1782465b4fd90fa431a8d3057351923a6744d84deba6632a17f452",
            "Figure review fixture changed");
    QString error;
    // Confirm that this exact book exercises separation without the default.
    EngineDocument reference(test::readerFonts());
    reference.setUserStyleSheet("figure { page-break-inside:auto !important; }");
    require(reference.open(bytes, viewport, settings, &error), qPrintable(error));
    const auto detachedCaption = search(reference, "Figure 1.", 1);
    require(reference.pages() == 2 && detachedCaption.size() == 1
                    && detachedCaption.first().page == 1
                    && !imageRegions(reference.image(0)).isEmpty()
                    && imageRegions(reference.image(1)).isEmpty(),
            "Figure review fixture no longer exercises image/caption separation");
    EngineDocument model(test::readerFonts());
    require(model.open(bytes, viewport, settings, &error), qPrintable(error));
    const auto caption = search(model, "Figure 1.", 1);
    require(caption.size() == 1 && caption.first().page == 1, "Review caption must be on page two");
    require(model.pages() == 2, "Review document must have exactly two pages");
    if (!directory.isEmpty()) {
        require(QDir().mkpath(directory), "Create figure document output");
        QFile book(directory + "/figure-caption.epub");
        require(book.open(QIODevice::WriteOnly) && book.write(bytes) == bytes.size(),
                "Save review EPUB");
    }
    for (int page = 0; page < model.pages(); ++page) {
        const auto image = model.image(page);
        require(imageRegions(image).isEmpty() == (page != (split ? 0 : 1)),
                "Review image is on the wrong page");
        if (!directory.isEmpty())
            require(image.save(directory + "/page-" + QString::number(page + 1) + ".png"),
                    "Save review document page");
    }
    std::fprintf(stderr, "Review document: image page=%d caption page=2 EPUB SHA256=%s\n",
                 split ? 1 : 2,
                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().constData());
}

} // namespace

// Checks default keep-together, publisher overrides, forced breaks, oversize content, source anchors and reflow.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    readingPpi.store(96);
    if (argc >= 3 && QByteArray(argv[1]) == "--reporter-document") {
        reporterFigure(1, false, {}, true, QString::fromLocal8Bit(argv[2]));
        return 0;
    }
    if (argc >= 2 && QByteArray(argv[1]) == "--reporter-only") {
        floatFootprints();
        for (int ppi : { 96, 300 }) {
            readingPpi.store(ppi);
            for (int columns : { 1, 2 })
                for (bool dark : { false, true }) {
                    reporterFigures(columns, dark);
                }
        }
        return 0;
    }
    if (argc >= 3 && QByteArray(argv[1]) == "--review-document") {
        reviewDocument(QString::fromLocal8Bit(argv[2]),
                       argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString(),
                       qEnvironmentVariableIsSet("KOBALT_TEST_FIGURE_EXPECT_SPLIT"));
        return 0;
    }
    floatFootprints();
    reviewDocument();
    QImage art(200, 200, QImage::Format_RGB32);
    art.fill(Qt::red);
    QByteArray png;
    QBuffer output(&png);
    require(output.open(QIODevice::WriteOnly) && art.save(&output, "PNG"), "Encode figure");
    const Fixture fixtures[]{
        { "fits", 0, {}, {} },
        { "boundary", 580, {}, {}, 200, 0 },
        { "near-end", 580, {}, {} },
        { "nested", 580, {}, {}, 200, 1, true, true, true },
        { "caption-first", 580, {}, {}, 200, 1, true, true, false, true },
        { "publisher-auto", 580, "page-break-inside:auto", {}, 200, 1, false },
        { "forced-caption", 100, {}, "page-break-before:always", 200, 1, false },
        { "forced-figure", 100, "page-break-before:always;page-break-after:always", {} },
        { "tall-figure", 100, {}, {}, 780, 2, false },
        { "long-caption", 100, {}, {}, 200, 30, false },
        { "plain-image-caption", 580, {}, {}, 200, 1, false, false },
        nonBlockFixture("inline-figure", "display:inline"),
        nonBlockFixture("inline-block-figure", "display:inline-block"),
        nonBlockFixture("floating-figure", "float:right;width:300px"),
        nonBlockFixture("inside-float", {}, "float:right;width:300px"),
        nonBlockFixture("inside-inline-block", {}, "display:inline-block"),
        nonBlockFixture("inside-table-cell", {}, "display:table-cell")
    };
    for (int ppi : { 96, 300 }) {
        readingPpi.store(ppi);
        for (int columns : { 1, 2 })
            for (bool dark : { false, true })
                reporterFigures(columns, dark);
        for (const auto &fixture : fixtures)
            for (int columns : { 1, 2 })
                for (bool dark : { false, true }) {
                    check(fixture, columns, dark, png);
                    if (QByteArray(fixture.name).startsWith("forced"))
                        check(fixture, columns, dark, png, true);
                    if (fixture.unchanged || QByteArray(fixture.name) == "publisher-auto")
                        check(fixture, columns, dark, png, false, true);
                }
    }
}
