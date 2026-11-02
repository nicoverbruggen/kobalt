#include "check.h"
#include "reader_inputs.h"
#include "book_fixtures.h"
#include "engine_fixtures.h"
#include "engine/crengine/reader_model.h"
#include <lvpagesplitter.h>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

using namespace kobalt;

static QRect inkBounds(const QImage &image) {
    QRect result;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 100)
                result |= QRect(x, y, 1, 1);
    return result;
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    // The density boundary uses occupied line-box height, excluding margins.
    LVRendPageInfo boundary(0, 900, 0);
    boundary.justificationLines.add(lvPoint(0, 450));
    boundary.justificationLines.add(lvPoint(450, 900));
    require(boundary.centeringOffset(1000) == 50, "90% occupancy did not center");
    require(boundary.centeringOffset(1001) == 0, "Content below 90% was centered");
    require(boundary.justificationOffset(700, 1000, 2) == 50 &&
            boundary.justificationSourceY(750, 1000, 2, false) == 700,
            "Centered coordinates did not round-trip");

    QTemporaryDir directory;
    require(directory.isValid(), "Create vertical spacing fixture directory");
    QByteArray body;
    const QByteArray text = "A reader keeps the passage ready. Careful typography preserves each word and every paragraph. ";
    for (int i = 0; i < 32; ++i)
        body += "<p>" + text.repeated(2 + i % 5) + "</p>";
    const auto files = test::publisherPackage("en", "Vertical spacing", {
        {"c.xhtml", "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}</style></head><body>"
            "<div style='border-width:0;border-style:solid'>" + body + "</div></body></html>"}},
        "<itemref idref='c.xhtml'/>");
    require(test::writeBook(directory.path(), "spacing.epub", files), "Write vertical spacing fixture");
    QFile input(directory.path() + "/spacing.epub");
    require(input.open(QIODevice::ReadOnly), "Read vertical spacing fixture");
    const auto bytes = input.readAll();
    const QJsonObject typography{{"font_pixels", 32}, {"line_spacing", 150}, {"line_spacing_version", 1}};
    QString error;
    {
        ReaderModel model(test::readerFonts());
        require(model.open(bytes, QSize(800, 973), typography, &error), qPrintable(error));
        const auto before = model.image(0);
        const auto beforeInk = inkBounds(before);
        const auto edges = model.annotationPageEdges();
        const auto first = model.rangeRects(edges.first.start, edges.first.end);
        const auto last = model.rangeRects(edges.last.start, edges.last.end);
        const auto position = model.position();
        const int pages = model.pages();
        require(!first.isEmpty() && !last.isEmpty(), "Missing baseline selection geometry");
        require(model.applyAppearance({{"vertical_justification", "center"}}), "Enable centering");
        const auto after = model.image(0);
        const auto centeredFirst = model.rangeRects(edges.first.start, edges.first.end);
        const auto centeredLast = model.rangeRects(edges.last.start, edges.last.end);
        const int offset = centeredFirst.first().top() - first.first().top();
        require(offset > 0 && centeredLast.last().top() - last.last().top() == offset,
                "Centering changed the distance between lines");
        const auto afterInk = inkBounds(after);
        require(afterInk == beforeInk.translated(0, offset) && before.copy(beforeInk) == after.copy(afterInk),
                "Centering changed text pixels or internal spacing");
        require(model.pages() == pages && model.position() == position, "Centering changed pagination or position");
        require(model.wordAt(centeredLast.last().center()).start == edges.last.start,
                "Centered text did not receive its word hit");
        require(model.annotationGeometry(edges.last.start, edges.last.end).rectangles == centeredLast,
                "Centered annotation geometry did not follow the text");
        require(model.applyAppearance({{"vertical_justification", "off"}}) && model.image(0) == before,
                "Off did not restore the original page");
    }

    for (const auto *side : {"top", "right", "bottom", "left"}) {
        const auto framed = test::publisherPackage("en", "Framed prose", {
            {"c.xhtml", "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}</style></head><body>"
                "<div style='border-style:solid;border-width:0;border-" + QByteArray(side) + "-width:1px'>" +
                body + "</div></body></html>"}}, "<itemref idref='c.xhtml'/>");
        require(test::writeBook(directory.path(), "framed.epub", framed), "Write visible border fixture");
        QFile framedInput(directory.path() + "/framed.epub");
        require(framedInput.open(QIODevice::ReadOnly), "Read visible border fixture");
        ReaderModel model(test::readerFonts());
        require(model.open(framedInput.readAll(), QSize(800, 973), typography, &error), qPrintable(error));
        const auto before = model.image(0);
        for (const auto *mode : {"expand", "center"}) {
            require(model.applyAppearance({{"vertical_justification", mode}}) && model.image(0) == before,
                    "Page spacing changed prose inside a visible border");
        }
    }

    const auto fingerprint = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    int height = 640;
    for (bool cached : {false, true}) {
        ReaderModel model(test::readerFonts());
        model.setDocumentCache(directory.path() + "/cache", fingerprint, "vertical-spacing", bytes.size());
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
                require(model.applyAppearance({{"vertical_justification", "off"}}), "Disable page adjustment");
                const auto before = model.image(page);
                require(model.applyAppearance({{"vertical_justification", "expand"}, {"vertical_spacing_scope", "protection"}}),
                        "Enable protection-only expansion");
                const auto protectedImage = model.image(page);
                require(model.applyAppearance({{"vertical_spacing_scope", "all"}}), "Enable general expansion");
                const auto generalImage = model.image(page);
                protectedPage |= protectedImage != before;
                ordinaryPage |= protectedImage == before && generalImage != before;
            }
            if (protectedPage && ordinaryPage) break;
        }
        require(protectedPage, "No gap caused by widow/orphan protection was adjusted");
        require(ordinaryPage, "Protection-only expansion also changed ordinary page gaps");
        require(model.applyAppearance({{"widows_orphans", "allow"}, {"vertical_justification", "off"}}),
                "Disable paragraph protection");
        for (int page = 0; page < qMin(8, model.pages()); ++page) {
            require(model.go(page), "Open unprotected page");
            require(model.applyAppearance({{"vertical_justification", "off"}}), "Disable unprotected adjustment");
            const auto before = model.image(page);
            for (const auto &mode : {QString("expand"), QString("center")}) {
                require(model.applyAppearance({{"vertical_justification", mode}, {"vertical_spacing_scope", "protection"}}),
                        "Select protection-only mode");
                require(model.image(page) == before, "Protection-only mode changed a page with protection disabled");
            }
        }
        require(model.applyAppearance({{"widows_orphans", "avoid"}, {"vertical_justification", "off"}}),
                "Restore protected layout for cache");
        require(model.saveDocumentCache(), "Save protection metadata");
    }
    return 0;
}
