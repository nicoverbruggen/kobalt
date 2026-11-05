#include "check.h"
#include "reader_inputs.h"
#include "book_fixtures.h"
#include "engine_fixtures.h"
#include "engine/crengine/engine_document.h"
#include <lvpagesplitter.h>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QBuffer>
#include <QDir>
#include <algorithm>
#include <QTemporaryDir>

using namespace kobalt;

static QRect inkBounds(const QImage &image)
{
    QRect result;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 100)
                result |= QRect(x, y, 1, 1);
    return result;
}

namespace {
QVector<QRect> imageRegions(const QImage &image)
{
    QVector<QRect> result;
    for (const auto &value :
         QJsonDocument::fromJson(image.text("kobalt.image_regions").toUtf8()).array()) {
        const auto r = value.toArray();
        result.append(QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt()));
    }
    return result;
}

void spacingGeometry()
{
    LVRendPageInfo mediaOnly(0, 950, 0);
    mediaOnly.justificationLines.add(lvPoint(0, 950));
    require(mediaOnly.centeringOffset(1000) == 0 && mediaOnly.justificationExtra(1000) == 0,
            "A page containing only fixed media was adjusted");
    LVRendPageInfo page(0, 950, 0);
    for (const auto &box : { lvPoint(0, 100), lvPoint(100, 200), lvPoint(200, 300),
                             lvPoint(300, 500), lvPoint(500, 600), lvPoint(600, 700),
                             lvPoint(700, 800), lvPoint(800, 900), lvPoint(900, 950) })
        page.justificationLines.add(box);
    for (int edge : { 100, 200, 600, 700, 800, 900 })
        page.justificationGaps.add(edge);
    page.justificationProseLines = 8;
    page.justificationProseHeight = 750;
    require(page.justificationExtra(1000) == 50, "Suitable fixed group did not expand");
    for (int y : { 299, 300, 350, 499, 500 })
        require(page.justificationOffset(y, 1000, 1) == page.justificationOffset(300, 1000, 1),
                "Expansion changed spacing inside or immediately beside a protected group");
    for (int mode : { 1, 2 })
        for (int y = 0; y < 950; ++y) {
            const int displayed = y + page.justificationOffset(y, 1000, mode);
            require(page.justificationSourceY(displayed, 1000, mode, false) == y,
                    "Fixed-group map did not invert");
        }
    require(page.justificationSourceY(101, 1000, 1, false) == -1
                    && page.justificationSourceY(101, 1000, 1, true) == 100,
            "Added leading accepted an ordinary text hit");
    page.flags |= RN_PAGE_KEEP_VERTICAL_SPACING;
    require(page.justificationExtra(1000) == 0 && page.centeringOffset(1000) == 25,
            "Forced-ending expansion or dense-ending centering policy changed");
    page.flags &= ~RN_PAGE_KEEP_VERTICAL_SPACING;
    page.justificationProseLines = 5;
    require(page.justificationExtra(1000) == 0,
            "Decorations counted toward the six-prose-line minimum");
    page.footnotes.add(LVPageFootNoteInfo(0, 1));
    require(page.centeringOffset(1000) == 0, "Footnote page was centered");

    LVRendPageInfo opening(0, 950, 0);
    opening.justificationLines.add(lvPoint(0, 500));
    opening.justificationPrefix = 500;
    for (int y = 500; y < 950; y += 45) {
        opening.justificationLines.add(lvPoint(y, y + 45));
        if (y > 500)
            opening.justificationGaps.add(y);
    }
    opening.justificationProseLines = 10;
    opening.justificationProseHeight = 450;
    require(opening.centeringOffset(1000) == 25, "Opening suffix did not meet the 90% boundary");
    require(opening.centeringOffset(1001) == 0, "Sparse opening suffix was centered");
    for (int mode : { 1, 2 }) {
        require(opening.justificationOffset(200, 1000, mode) == 0, "Opening prefix moved");
        require(opening.justificationOffset(700, 1000, mode) > 0,
                "Opening prefix disabled subsequent prose");
        for (int y = 0; y < 950; ++y) {
            const auto displayed = y + opening.justificationOffset(y, 1000, mode);
            require(opening.justificationSourceY(displayed, 1000, mode, false) == y,
                    "Opening map did not invert");
        }
    }
}

void decorativePages()
{
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create decorative spacing fixtures");
    QImage ornament(120, 12, QImage::Format_RGB32);
    ornament.fill(Qt::white);
    for (int x = 8; x < 112; ++x)
        for (int y = 3; y < 9; ++y)
            ornament.setPixelColor(x, y, Qt::blue);
    QByteArray png;
    QBuffer buffer(&png);
    require(buffer.open(QIODevice::WriteOnly) && ornament.save(&buffer, "PNG"),
            "Cannot encode ornament");
    const QByteArray image = "<img src='ornament.png' alt='' style='height:.5em;width:5em'/>";
    const QByteArray separator =
            "<div aria-hidden='true' style='text-align:center;margin:1em 0'>" + image + "</div>";
    const QByteArray words =
            "A careful reader keeps the <a href='#target'>passage</a> ready. Every word stays in its source paragraph, "
            "with stable typography and useful selection coordinates. ";
    for (const auto &kind :
         { QByteArray("first-letter"), QByteArray("top"), QByteArray("middle"),
           QByteArray("bottom"), QByteArray("multiple"), QByteArray("opening"),
           QByteArray("caption"), QByteArray("unknown"), QByteArray("unknown-middle"),
           QByteArray("frame"), QByteArray("split-caption"), QByteArray("float") }) {
        QByteArray body;
        for (int i = 0; i < 40; ++i) {
            if (((kind == "top" || kind == "first-letter") && i == 0)
                || (kind == "middle" && i == 2) || (kind == "bottom" && i == 4)
                || (kind == "multiple" && (i == 1 || i == 3)))
                body += separator;
            if (i == 0 && kind == "opening")
                body += "<h2>Chapter opening</h2>" + separator;
            if (i == 0 && kind == "caption")
                body += "<figure>" + image
                        + "<figcaption>Caption stays with its illustration.</figcaption></figure>";
            if (i == 0 && kind == "split-caption")
                body += "<figure>" + image + "<figcaption>" + ("Captionline " + words).repeated(30)
                        + "</figcaption></figure>";
            if (i == 0 && kind == "float")
                body += "<div style='float:right'>" + image + "</div>";
            if ((i == 0 && kind == "unknown") || (i == 2 && kind == "unknown-middle"))
                body += "<div>" + image + "</div>";
            body += "<p id='p" + QByteArray::number(i) + "'"
                    + QByteArray(i == 0 && kind == "opening" ? " class='initial'" : "") + ">"
                    + words + "</p>";
        }
        if (kind == "frame")
            body = "<div style='border:1px solid'>" + separator + body + "</div>";
        const auto files = test::publisherPackage(
                "en", "Decorative spacing",
                { { "ornament.png", png },
                  { "c.xhtml",
                    "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
                    "p{margin:0;text-indent:1em}figure{margin:1em 0;text-align:center}figcaption{margin:0}"
                    "h2{margin:0;text-align:center}.initial::first-letter{float:left;font-size:3em;line-height:.8}"
                            + QByteArray(kind == "first-letter" ? "p::first-letter{color:#333}"
                                                                : "")
                            + "</style></head><body><span id='target'></span>" + body
                            + "</body></html>" } },
                "<itemref idref='c.xhtml'/>");
        require(test::writeBook(directory.path(), "decorations.epub", files),
                "Cannot write decorative book");
        QFile input(directory.path() + "/decorations.epub");
        require(input.open(QIODevice::ReadOnly), "Cannot open decorative book");
        const auto bytes = input.readAll();
        const auto fingerprint =
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        for (bool dark : { false, true })
            for (int columns : { 1, 2 }) {
                EngineDocument model(test::readerFonts());
                const QSize viewport = columns == 1 ? QSize(800, 1143) : QSize(1143, 807);
                const QJsonObject settings{
                    { "font_pixels", 32 },         { "line_spacing", 150 },
                    { "line_spacing_version", 1 }, { "widows_orphans", "avoid" },
                    { "columns", columns },        { "dark_mode", dark }
                };
                QString error;
                const auto cache = directory.path() + "/cache-" + kind + QString::number(columns)
                        + QString::number(dark);
                // CREngine does not retain publications whose declared source size is under 30 kB.
                model.setDocumentCache(cache, fingerprint, "decorative-spacing", 1024 * 1024);
                require(model.open(bytes, viewport, settings, &error), qPrintable(error));
                const int pageCount = model.pages();
                bool foundImage = false, expanded = false, centered = false;
                for (int page = 0; page < qMin(8, pageCount); ++page) {
                    require(model.go(page), "Cannot visit decoration page");
                    const auto anchor = model.position();
                    const auto before = model.image(page);
                    const auto regions = imageRegions(before);
                    const auto splitCaptionHits = model.searchPage("Captionline", page).hits;
                    if (regions.isEmpty() && splitCaptionHits.isEmpty())
                        continue;
                    foundImage |= !regions.isEmpty();
                    QVector<QRect> protectedRegions = regions;
                    for (const auto &hit : splitCaptionHits)
                        protectedRegions += model.rangeRects(hit.start, hit.end);
                    const auto hits = model.searchPage("passage", page).hits;
                    QVector<QVector<QRect>> hitRects;
                    for (const auto &hit : hits)
                        hitRects.append(model.rangeRects(hit.start, hit.end));
                    const auto captions = model.searchPage("Caption stays", page).hits;
                    QVector<QVector<QRect>> captionRects;
                    for (const auto &hit : captions)
                        captionRects.append(model.rangeRects(hit.start, hit.end));
                    require(!hits.isEmpty(), "Graphic page contains no prose to adjust");
                    for (const auto *mode : { "expand", "center" }) {
                        require(model.applyAppearance({ { "vertical_justification", mode },
                                                        { "vertical_spacing_scope", "all" } }),
                                "Cannot adjust decorative page");
                        const auto after = model.image(page);
                        if (kind == "top" && columns == 1 && page == 0)
                            require(after != before, "Decorative image disabled page spacing");
                        require(model.pages() == pageCount && model.position() == anchor,
                                "Decorative spacing changed pagination or anchor");
                        const auto adjustedRegions = imageRegions(after);
                        require(regions.size() == adjustedRegions.size(),
                                "Page spacing lost an image");
                        for (int i = 0; i < regions.size(); ++i)
                            require(regions[i].x() == adjustedRegions[i].x()
                                            && regions[i].size() == adjustedRegions[i].size()
                                            && before.copy(regions[i])
                                                    == after.copy(adjustedRegions[i]),
                                    "Page spacing changed image pixels or dimensions");
                        for (int i = 0; i < hits.size(); ++i) {
                            const auto &hit = hits[i];
                            const auto rects = model.rangeRects(hit.start, hit.end);
                            require(rects.size() == hitRects[i].size(),
                                    "Page spacing rewrapped text");
                            for (int j = 0; j < rects.size(); ++j)
                                require(rects[j].x() == hitRects[i][j].x()
                                                && rects[j].size() == hitRects[i][j].size(),
                                        "Page spacing changed horizontal alignment or glyph geometry");
                            if (kind == "opening" && page == 0 && i == 0)
                                require(rects == hitRects[i],
                                        "Opening initial and its paragraph moved apart");
                            require(!rects.isEmpty()
                                            && model.wordAt(rects.first().center()).start
                                                    == hit.start,
                                    "Decorative spacing broke text hit coordinates");
                            require(model.annotationGeometry(hit.start, hit.end).rectangles
                                            == rects,
                                    "Decorative spacing broke highlight coordinates");
                        }
                        for (int i = 0; i < captions.size(); ++i) {
                            auto expected = captionRects[i];
                            for (auto &rect : expected)
                                rect.translate(0,
                                               adjustedRegions.first().y() - regions.first().y());
                            require(model.rangeRects(captions[i].start, captions[i].end)
                                            == expected,
                                    "Caption moved independently of its image");
                        }
                        const auto capture =
                                qEnvironmentVariable("KOBALT_TEST_SPACING_CAPTURE_DIR");
                        if (!capture.isEmpty()) {
                            require(QDir().mkpath(capture), "Cannot create spacing captures");
                            const auto name = capture + '/' + kind + '-' + QString::number(columns)
                                    + '-' + QString::number(dark) + '-' + QString::number(page)
                                    + '-' + mode;
                            require(before.save(name + "-normal.png") && after.save(name + ".png"),
                                    "Cannot save spacing capture");
                        }
                        const auto links = model.pageLinks();
                        require(!links.isEmpty(), "Decorative page lost text links");
                        for (const auto &hit : hits) {
                            const auto point =
                                    model.rangeRects(hit.start, hit.end).first().center();
                            require(std::any_of(links.cbegin(), links.cend(),
                                                [&](const PageLink &link) {
                                                    return std::any_of(link.rectangles.cbegin(),
                                                                       link.rectangles.cend(),
                                                                       [&](const QRect &rect) {
                                                                           return rect.contains(
                                                                                   point);
                                                                       });
                                                }),
                                    "Visible linked text escaped its link hit rectangle");
                        }
                        for (const auto &link : links) {
                            const auto detail = model.linkDetails(link.source, false);
                            require(!detail.target.isEmpty() && !link.rectangles.isEmpty(),
                                    "Decorative spacing lost a link target or rectangle");
                        }
                        if (kind == "frame")
                            require(after == before, "Framed content was adjusted");
                        else if (kind == "unknown" || kind == "unknown-middle"
                                 || kind == "split-caption" || kind == "float") {
                            for (const auto &region : protectedRegions) {
                                const int left =
                                        columns == 2 && region.center().x() >= viewport.width() / 2
                                        ? viewport.width() / 2
                                        : 0;
                                const int right = columns == 2 && !left ? viewport.width() / 2
                                                                        : viewport.width();
                                const QRect column(left, 0, right - left, viewport.height());
                                require(after.copy(column) == before.copy(column),
                                        qPrintable("Protected column adjusted: " + kind + " mode="
                                                   + mode + " columns=" + QString::number(columns)
                                                   + " page=" + QString::number(page)));
                            }
                        } else if (QByteArray(mode) == "expand")
                            expanded |= after != before;
                        else
                            centered |= after != before;
                        EngineDocument preview(test::readerFonts());
                        require(preview.openPreview(model, viewport, &error)
                                        && preview.image(page) == after,
                                "Decorative spacing preview differs");
                        require(model.saveDocumentCache(), "Cannot save decorative layout");
                        EngineDocument reopened(test::readerFonts());
                        reopened.setDocumentCache(cache, fingerprint, "decorative-spacing",
                                                  1024 * 1024);
                        require(reopened.open(bytes, viewport, model.state(), &error)
                                        && reopened.openedFromCache()
                                        && reopened.image(page) == after,
                                "Cached decorative spacing differs");
                        for (const auto &hit : hits)
                            require(reopened.rangeRects(hit.start, hit.end)
                                            == model.rangeRects(hit.start, hit.end),
                                    "Cached decorative hit geometry differs");
                        require(model.applyAppearance({ { "vertical_justification", "off" } })
                                        && model.image(page) == before,
                                "Normal did not restore decorative pixels");
                    }
                    const auto original = model.image(page);
                    int formats = 0;
                    require(model.applyAppearance({}, QSize(viewport.height(), viewport.width()))
                                    && model.applyAppearance({}, viewport,
                                                             [&](OpeningStage stage, int percent) {
                                                                 if (stage == OpeningStage::Pages
                                                                     && percent == 0)
                                                                     ++formats;
                                                             })
                                    && formats == 0 && model.image(page) == original,
                            "Retained rotation changed decorative layout");
                }
                require(foundImage, "Fixture did not exercise a graphic page");
                if (kind != "unknown" && kind != "unknown-middle" && kind != "frame"
                    && kind != "split-caption" && kind != "float") {
                    require(expanded, qPrintable("Expand excluded decorative fixture: " + kind));
                    require(centered, qPrintable("Center excluded decorative fixture: " + kind));
                }
                require(model.applyAppearance({ { "widows_orphans", "allow" },
                                                { "vertical_justification", "off" } }),
                        "Cannot disable paragraph protection");
                const auto before = model.image(0);
                for (const auto *mode : { "expand", "center" })
                    require(model.applyAppearance({ { "vertical_justification", mode },
                                                    { "vertical_spacing_scope", "protection" } })
                                    && model.image(0) == before,
                            "Protection-only adjusted an unprotected decorative page");
            }
    }
}
} // namespace

// Checks Expand and Center page spacing: unchanged breaks, geometry, hit tests, decorative content and cached reopening.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    spacingGeometry();
    decorativePages();
    // The density boundary uses occupied line-box height, excluding margins.
    LVRendPageInfo boundary(0, 900, 0);
    boundary.justificationLines.add(lvPoint(0, 450));
    boundary.justificationLines.add(lvPoint(450, 900));
    boundary.justificationProseLines = 2;
    boundary.justificationProseHeight = 900;
    require(boundary.centeringOffset(1000) == 50, "90% occupancy did not center");
    require(boundary.centeringOffset(1001) == 0, "Content below 90% was centered");
    require(boundary.justificationOffset(700, 1000, 2) == 50
                    && boundary.justificationSourceY(750, 1000, 2, false) == 700,
            "Centered coordinates did not round-trip");

    QTemporaryDir directory;
    require(directory.isValid(), "Create vertical spacing fixture directory");
    QByteArray body;
    const QByteArray text =
            "A reader keeps the passage ready. Careful typography preserves each word and every paragraph. ";
    for (int i = 0; i < 32; ++i)
        body += "<p>" + text.repeated(2 + i % 5) + "</p>";
    const auto files = test::publisherPackage(
            "en", "Vertical spacing",
            { { "c.xhtml",
                "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}</style></head><body>"
                "<div style='border-width:0;border-style:solid'>"
                        + body + "</div></body></html>" } },
            "<itemref idref='c.xhtml'/>");
    require(test::writeBook(directory.path(), "spacing.epub", files),
            "Write vertical spacing fixture");
    QFile input(directory.path() + "/spacing.epub");
    require(input.open(QIODevice::ReadOnly), "Read vertical spacing fixture");
    const auto bytes = input.readAll();
    const QJsonObject typography{ { "font_pixels", 32 },
                                  { "line_spacing", 150 },
                                  { "line_spacing_version", 1 } };
    QString error;
    {
        EngineDocument model(test::readerFonts());
        require(model.open(bytes, QSize(800, 973), typography, &error), qPrintable(error));
        const auto before = model.image(0);
        const auto beforeInk = inkBounds(before);
        const auto edges = model.annotationPageEdges();
        const auto first = model.rangeRects(edges.first.start, edges.first.end);
        const auto last = model.rangeRects(edges.last.start, edges.last.end);
        const auto position = model.position();
        const int pages = model.pages();
        require(!first.isEmpty() && !last.isEmpty(), "Missing baseline selection geometry");
        require(model.applyAppearance({ { "vertical_justification", "center" } }),
                "Enable centering");
        const auto after = model.image(0);
        const auto centeredFirst = model.rangeRects(edges.first.start, edges.first.end);
        const auto centeredLast = model.rangeRects(edges.last.start, edges.last.end);
        const int offset = centeredFirst.first().top() - first.first().top();
        require(offset > 0 && centeredLast.last().top() - last.last().top() == offset,
                "Centering changed the distance between lines");
        const auto afterInk = inkBounds(after);
        require(afterInk == beforeInk.translated(0, offset)
                        && before.copy(beforeInk) == after.copy(afterInk),
                "Centering changed text pixels or internal spacing");
        require(model.pages() == pages && model.position() == position,
                "Centering changed pagination or position");
        require(model.wordAt(centeredLast.last().center()).start == edges.last.start,
                "Centered text did not receive its word hit");
        require(model.annotationGeometry(edges.last.start, edges.last.end).rectangles
                        == centeredLast,
                "Centered annotation geometry did not follow the text");
        require(model.applyAppearance({ { "vertical_justification", "off" } })
                        && model.image(0) == before,
                "Off did not restore the original page");
    }

    for (const auto *side : { "top", "right", "bottom", "left" }) {
        const auto framed = test::publisherPackage(
                "en", "Framed prose",
                { { "c.xhtml",
                    "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}</style></head><body>"
                    "<div style='border-style:solid;border-width:0;border-"
                            + QByteArray(side) + "-width:1px'>" + body + "</div></body></html>" } },
                "<itemref idref='c.xhtml'/>");
        require(test::writeBook(directory.path(), "framed.epub", framed),
                "Write visible border fixture");
        QFile framedInput(directory.path() + "/framed.epub");
        require(framedInput.open(QIODevice::ReadOnly), "Read visible border fixture");
        EngineDocument model(test::readerFonts());
        require(model.open(framedInput.readAll(), QSize(800, 973), typography, &error),
                qPrintable(error));
        const auto before = model.image(0);
        for (const auto *mode : { "expand", "center" }) {
            require(model.applyAppearance({ { "vertical_justification", mode } })
                            && model.image(0) == before,
                    "Page spacing changed prose inside a visible border");
        }
    }

    const auto fingerprint = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    int height = 640;
    for (bool cached : { false, true }) {
        EngineDocument model(test::readerFonts());
        model.setDocumentCache(directory.path() + "/cache", fingerprint, "vertical-spacing",
                               bytes.size());
        auto settings = typography;
        settings["widows_orphans"] = "avoid";
        require(model.open(bytes, QSize(800, height), settings, &error), qPrintable(error));
        require(model.openedFromCache() == cached, "Protection metadata did not reopen from cache");
        bool protectedPage = false, ordinaryPage = false;
        for (int attempt = 0; attempt < (cached ? 1 : 6); ++attempt) {
            if (attempt) {
                height += 7;
                require(model.applyAppearance({}, QSize(800, height)), "Resize protection fixture");
            }
            for (int page = 0; page < qMin(12, model.pages()); ++page) {
                require(model.go(page), "Open protection fixture page");
                require(model.applyAppearance({ { "vertical_justification", "off" } }),
                        "Disable page adjustment");
                const auto before = model.image(page);
                require(model.applyAppearance({ { "vertical_justification", "expand" },
                                                { "vertical_spacing_scope", "protection" } }),
                        "Enable protection-only expansion");
                const auto protectedImage = model.image(page);
                require(model.applyAppearance({ { "vertical_spacing_scope", "all" } }),
                        "Enable general expansion");
                const auto generalImage = model.image(page);
                protectedPage |= protectedImage != before;
                ordinaryPage |= protectedImage == before && generalImage != before;
            }
            if (protectedPage && ordinaryPage)
                break;
        }
        require(protectedPage, "No gap caused by widow/orphan protection was adjusted");
        require(ordinaryPage, "Protection-only expansion also changed ordinary page gaps");
        require(model.applyAppearance(
                        { { "widows_orphans", "allow" }, { "vertical_justification", "off" } }),
                "Disable paragraph protection");
        for (int page = 0; page < qMin(8, model.pages()); ++page) {
            require(model.go(page), "Open unprotected page");
            require(model.applyAppearance({ { "vertical_justification", "off" } }),
                    "Disable unprotected adjustment");
            const auto before = model.image(page);
            for (const auto &mode : { QString("expand"), QString("center") }) {
                require(model.applyAppearance({ { "vertical_justification", mode },
                                                { "vertical_spacing_scope", "protection" } }),
                        "Select protection-only mode");
                require(model.image(page) == before,
                        "Protection-only mode changed a page with protection disabled");
            }
        }
        require(model.applyAppearance(
                        { { "widows_orphans", "avoid" }, { "vertical_justification", "off" } }),
                "Restore protected layout for cache");
        require(model.saveDocumentCache(), "Save protection metadata");
    }
    return 0;
}
