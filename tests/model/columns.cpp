#include "engine/crengine/engine_document.h"
#include "check.h"
#include "reader_inputs.h"
#include <QGuiApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cstdio>

namespace {
void openResources(kobalt::EngineDocument &model, const QMap<QString, QByteArray> &data,
                   QJsonObject settings, QSize size, kobalt::OpeningProgress progress = {})
{
    QMap<QString, qint64> sizes;
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto source = std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    settings["font_pixels"] = 40;
    settings["line_spacing"] = 100;
    if (!settings.contains("margin_pixels"))
        settings["margin_pixels"] = 0;
    if (!settings.contains("column_spacing"))
        settings["column_spacing"] = 20;
    if (!settings.contains("top_margin_pixels"))
        settings["top_margin_pixels"] = 0;
    if (!settings.contains("bottom_margin_pixels"))
        settings["bottom_margin_pixels"] = 0;
    QString error;
    require(model.openResources(source, sizes, size, settings, &error, std::move(progress)),
            qPrintable(error));
}

void open(kobalt::EngineDocument &model, const QByteArray &body, const QByteArray &css = {},
          QJsonObject settings = {}, QSize size = { 1072, 1448 }, bool rtl = false,
          kobalt::OpeningProgress progress = {})
{
    const QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          QByteArray(
                  "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata><title>Columns</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine")
                  + (rtl ? " page-progression-direction='rtl'" : "")
                  + "><itemref idref='c'/></spine></package>" },
        { "c.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}" + css
                  + "</style></head><body>" + body + "</body></html>" }
    };
    openResources(model, data, settings, size, std::move(progress));
}

QMap<QString, QByteArray> chapterBook(int firstColumns, bool rtl)
{
    const auto html = [](const char *name, int count) {
        QByteArray body;
        for (int i = 0; i < count; ++i)
            body += "<p style='page-break-before:always'>" + QByteArray(name)
                    + QByteArray::number(i) + "<br/>"
                    + QByteArray("Amber birch cedar.<br/>").repeated(i == count - 1 ? 8 : 16)
                    + "</p>";
        return "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0;line-height:1}</style></head><body>"
                + body + "</body></html>";
    };
    return {
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          QByteArray(
                  "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Chapter columns</title></metadata><manifest><item id='a' href='a.xhtml' media-type='application/xhtml+xml'/><item id='b' href='b.xhtml' media-type='application/xhtml+xml'/><item id='toc' href='toc.ncx' media-type='application/x-dtbncx+xml'/></manifest><spine toc='toc'")
                  + (rtl ? " page-progression-direction='rtl'" : "")
                  + "><itemref idref='a'/><itemref idref='b'/></spine></package>" },
        { "toc.ncx",
          "<ncx xmlns='http://www.daisy.org/z3986/2005/ncx/' version='2005-1'><head/><docTitle><text>Chapter columns</text></docTitle><navMap><navPoint id='a' playOrder='1'><navLabel><text>Short chapter</text></navLabel><content src='a.xhtml'/></navPoint><navPoint id='b' playOrder='2'><navLabel><text>Next chapter</text></navLabel><content src='b.xhtml'/></navPoint></navMap></ncx>" },
        { "a.xhtml", html("A", firstColumns) },
        { "b.xhtml", html("B", 13) }
    };
}

void chapterProgress()
{
    for (bool rtl : { false, true })
        for (int firstColumns : { 2, 3, 4 }) {
            kobalt::EngineDocument model(kobalt::test::readerFonts());
            openResources(model, chapterBook(firstColumns, rtl), { { "columns", 2 } },
                          { 1000, 800 });
            const auto chapters = model.chapters();
            require(chapters.size() == 2 && chapters[1].page == firstColumns / 2,
                    "Chapter fixture lost its column boundary");
            const int firstScreens = firstColumns == 2 ? 1 : 2;
            const int nextScreen = chapters[1].page;
            for (bool supplied : { false, true }) {
                if (supplied)
                    model.setChapters(chapters, chapters);
                require(model.go(0), "Cannot open the short chapter");
                const auto anchor = model.position();
                const auto progress = model.chapterProgress();
                require(progress.title == "Short chapter" && progress.page == 1
                                && progress.pages == firstScreens,
                        "Chapter total omitted its final shared screen or included the next full screen");
                require(model.chapterSelection(model.chapters()).pages == firstScreens
                                && model.position() == anchor,
                        "Chapter selection disagreed with the total or moved the reader");
                require(model.go(nextScreen), "Cannot open the chapter boundary");
                if (firstColumns == 3) {
                    require(!model.searchPage("A2", nextScreen).hits.isEmpty()
                                    && !model.searchPage("B0", nextScreen).hits.isEmpty(),
                            "Shared screen lost one of its chapters");
                    const auto mixed = model.chapterProgress();
                    require(supplied ? mixed.title == "Short chapter" && mixed.page == 2
                                            && mixed.pages == 2
                                     : mixed.title == "Next chapter" && mixed.page == 1
                                            && mixed.pages == 7,
                            "Shared-screen chapter label or page count changed");
                }
                require(model.goTo(chapters[1].position), "Cannot open the next chapter");
                const auto next = model.chapterProgress();
                require(next.title == "Next chapter" && next.page == 1 && next.pages == 7,
                        "Next chapter did not retain its first shared screen");
                require(model.go(model.pages() - 1) && model.chapterProgress().page == 7
                                && model.chapterProgress().pages == 7,
                        "Last chapter lost its partial final screen");
            }
            if (firstColumns == 3) {
                // Keep the engine's text width unchanged while changing the screen
                // grouping, so cached chapter pages must include the column count.
                require(model.applyAppearance({ { "columns", 1 } }, { 490, 800 }) && model.go(0),
                        "Cannot reflow to one column");
                require(model.chapters()[1].page == 3 && model.chapterProgress().pages == 3,
                        "Chapter pages stayed grouped after switching to one column");
                require(model.applyAppearance({ { "columns", 2 } }, { 1000, 800 }) && model.go(0)
                                && model.chapterProgress().pages == 2,
                        "Chapter total lost its shared screen after reflow");
            }
        }
}

// A cached two-column layout keeps the hanging-punctuation allowance it was
// laid out with. Both settings give 420-pixel columns, so only the allowance
// (40 or 2 pixels) tells the two layouts apart.
void cachedColumns()
{
    const QByteArray paragraph = "<p style='text-align:justify'>"
            + QByteArray("\xe2\x80\x9c"
                         "Amber,"
                         "\xe2\x80\x9d"
                         " said birch. Cedar, dogwood, elm and fir; "
                         "\xe2\x80\x9c"
                         "quite so."
                         "\xe2\x80\x9d"
                         " ")
                      .repeated(200)
            + "</p>";
    const QSize size(1000, 800);

    struct Layout
    {
        int margin, spacing;
    };

    const Layout wide{ 40, 80 }, narrow{ 78, 4 };
    for (bool hanging : { false, true }) {
        QTemporaryDir root;
        require(root.isValid(), "Cannot create a column cache directory");
        const auto settings = [&](Layout layout) {
            return QJsonObject{ { "columns", 2 },
                                { "hanging_punctuation", hanging },
                                { "margin_pixels", layout.margin },
                                { "column_spacing", layout.spacing } };
        };
        // Returns the number of full layouts during the opening.
        const auto cached = [&](kobalt::EngineDocument &model, Layout layout) {
            model.setDocumentCache(root.path() + "/.cache", QByteArray(64, 'c'), "columns-test",
                                   1024 * 1024);
            int formats = 0;
            open(model, paragraph, {}, settings(layout), size, false,
                 [&](kobalt::OpeningStage stage, int percent) {
                     if (stage == kobalt::OpeningStage::Pages && percent == 0)
                         ++formats;
                 });
            return formats;
        };
        kobalt::EngineDocument freshWide(kobalt::test::readerFonts()),
                freshNarrow(kobalt::test::readerFonts());
        open(freshWide, paragraph, {}, settings(wide), size);
        open(freshNarrow, paragraph, {}, settings(narrow), size);
        const auto column = [](kobalt::EngineDocument &model, Layout layout) {
            return model.image(0).copy(layout.margin, 0, 420, 800);
        };
        require((column(freshWide, wide) == column(freshNarrow, narrow)) == !hanging,
                "The column fixture does not show the hanging-punctuation allowance");
        {
            kobalt::EngineDocument parsed(kobalt::test::readerFonts());
            require(cached(parsed, wide) > 0 && !parsed.openedFromCache()
                            && parsed.saveDocumentCache(),
                    "Cannot save two columns to the document cache");
        }
        kobalt::EngineDocument restored(kobalt::test::readerFonts());
        require(cached(restored, wide) == 0 && restored.openedFromCache(),
                "A cached two-column book with an unchanged allowance was laid out again");
        require(restored.pages() == freshWide.pages(), "Cached columns changed the page count");
        for (int page : { 0, restored.pages() / 2, restored.pages() - 1 })
            require(restored.image(page) == freshWide.image(page),
                    "Cached columns differ from a fresh layout");
        kobalt::EngineDocument changed(kobalt::test::readerFonts());
        const int formats = cached(changed, narrow);
        require(changed.openedFromCache() && (formats > 0) == hanging,
                "A changed allowance reused cached columns, or an unchanged one laid them out again");
        require(changed.pages() == freshNarrow.pages(), "Cached columns kept the old page count");
        for (int page : { 0, changed.pages() / 2, changed.pages() - 1 })
            require(changed.image(page) == freshNarrow.image(page),
                    "Cached columns kept stale geometry");
    }
}
} // namespace

// Checks each column against a single-column render, screen counts, gutters, selection, search, links, reading order and cache reuse with hanging punctuation.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    chapterProgress();
    cachedColumns();
    for (bool rtl : { false, true })
        for (bool dark : { false, true })
            for (int count : { 1, 2 }) {
                kobalt::EngineDocument model(kobalt::test::readerFonts());
                const auto paragraph = QByteArray("<p>")
                        + QByteArray(
                                  "<a href='https://example.test/'>Amber</a> birch cedar dogwood elm fir. ")
                                  .repeated(100)
                        + "</p>";
                open(model, paragraph, {}, { { "columns", count }, { "dark_mode", dark } },
                     { 1000, 800 }, rtl);
                require(model.state().value("columns").toInt() == count,
                        "Column count did not reach the model");
                require(model.pages() > 1, "Column fixture needs multiple screens");
                const auto image = model.image(0);
                require(image.size() == QSize(1000, 800), "Column rendering resized the screen");
                if (count > 1) {
                    const int width = (1000 - 20 * (count - 1)) / count;
                    kobalt::EngineDocument narrow(kobalt::test::readerFonts());
                    open(narrow, paragraph, {}, { { "columns", 1 }, { "dark_mode", dark } },
                         { width, 800 }, rtl);
                    require(model.pages() == (narrow.pages() + count - 1) / count,
                            "Column screens lost logical pages");
                    for (int i = 0; i < count; ++i)
                        require(image.copy((rtl ? count - 1 - i : i) * (width + 20), 0, width, 800)
                                        == narrow.image(i),
                                "Column text differs from its native-size engine rendering");
                }
                QVector<kobalt::WordSelection> words;
                for (int column = 0; column < count; ++column) {
                    const int width = (1000 - 20 * (count - 1)) / count;
                    const int left =
                            (rtl ? count - 1 - column : column) * (width + (count > 1 ? 20 : 0));
                    kobalt::WordSelection selected;
                    for (int y = 5; y < 80 && selected.text.isEmpty(); y += 3)
                        for (int x = left + 5; x < left + width && selected.text.isEmpty(); x += 4)
                            selected = model.wordAt({ x, y });
                    require(!selected.text.isEmpty(), "A column cannot select its text");
                    require(!selected.rectangles.isEmpty(), "A column selection lost its geometry");
                    require(selected.rectangles.first().left() >= left
                                    && selected.rectangles.first().right() < left + width,
                            "Selection geometry is in the wrong column");
                    require(model.page() == 0, "Hit testing changed the reading page");
                    words.append(selected);
                }
                if (count > 1) {
                    require(words.first().start != words.last().start,
                            "Columns repeat the same text");
                    const auto geometry =
                            model.annotationGeometry(words.first().start, words.last().end);
                    require(geometry.start.visible && geometry.end.visible,
                            "Cross-column annotation lost a handle");
                    require(rtl ? geometry.start.first.x() > geometry.end.first.x()
                                : geometry.start.first.x() < geometry.end.first.x(),
                            "Cross-column handles are in the wrong order");
                    require(qGray(image.pixel((1000 - 20 * (count - 1)) / count + 10, 20))
                                    == (dark ? 0 : 255),
                            "Column gutter has the wrong paper color");
                    require(model.wordAt({ (1000 - 20 * (count - 1)) / count + 10, 20 })
                                    .text.isEmpty(),
                            "Column gutter selected text");
                }
                QVector<bool> linked(count, false);
                for (const auto &link : model.pageLinks())
                    for (const auto &rectangle : link.rectangles) {
                        const int column = rectangle.center().x() / (1000 / count);
                        if (column >= 0 && column < count)
                            linked[column] = true;
                    }
                for (bool found : linked)
                    require(found, "A column lost its links");
                QVector<bool> searched(count, false);
                const auto matches = model.searchPage("Amber", 0);
                for (const auto &hit : matches.hits) {
                    require(hit.page == 0, "Search returned a logical column as the screen number");
                    for (const auto &rectangle : model.rangeRects(hit.start, hit.end)) {
                        const int column = rectangle.center().x() / (1000 / count);
                        if (column >= 0 && column < count)
                            searched[column] = true;
                    }
                }
                for (bool found : searched)
                    require(found, "Search missed a column");
                require(model.page() == 0, "Search changed the reading page");
                const auto anchor = model.position();
                require(model.go(1) && model.page() == 1,
                        "Turning did not advance a complete screen");
                require(model.goTo(anchor) && model.page() == 0,
                        "Column position restoration changed the screen");
                require(model.go(model.pages() - 1), "Cannot open last column screen");
                require(!model.image(model.page()).isNull(),
                        "Last partial screen failed to render");
                require(!model.applyAppearance({ { "columns", 0 } })
                                && !model.applyAppearance({ { "columns", 3 } }),
                        "Invalid column count was accepted");
                require(model.applyAppearance({ { "columns", count == 2 ? 1 : 2 } }),
                        "Cannot change column count");
                require(model.viewport() == QSize(1000, 800), "Column change lost screen size");
            }
    for (bool rtl : { false, true })
        for (bool dark : { false, true })
            for (bool hanging : { false, true })
                for (int count : { 2 }) {
                    const QByteArray paragraph = "<p>"
                            + QByteArray("Amber birch cedar dogwood elm fir. ").repeated(100)
                            + "</p>";
                    kobalt::EngineDocument model(kobalt::test::readerFonts());
                    open(model, paragraph, {},
                         { { "columns", count },
                           { "hanging_punctuation", hanging },
                           { "margin_pixels", 40 },
                           { "top_margin_pixels", 25 },
                           { "bottom_margin_pixels", 35 },
                           { "dark_mode", dark } },
                         { 1000, 800 }, rtl);
                    for (int gap : { 0, 20, 90 }) {
                        require(model.applyAppearance({ { "column_spacing", gap } }),
                                "Cannot change column spacing");
                        require(model.state().value("column_spacing").toInt() == gap,
                                "Column spacing was not retained");
                        const int width = (1000 - 80 - gap * (count - 1)) / count;
                        kobalt::EngineDocument narrow(kobalt::test::readerFonts());
                        open(narrow, paragraph, {},
                             { { "hanging_punctuation", hanging },
                               { "top_margin_pixels", 25 },
                               { "bottom_margin_pixels", 35 },
                               { "dark_mode", dark } },
                             { width, 800 }, rtl);
                        const auto image = model.image(0);
                        for (int i = 0; i < count; ++i) {
                            const int left = 40 + (rtl ? count - 1 - i : i) * (width + gap);
                            require(image.copy(left, 0, width, 800) == narrow.image(i),
                                    "Screen margins or spacing changed the column text width");
                            kobalt::WordSelection word;
                            for (int y = 28; y < 75 && word.text.isEmpty(); y += 3)
                                for (int x = left + 2; x < left + width && word.text.isEmpty();
                                     x += 4)
                                    word = model.wordAt({ x, y });
                            // Hanging can include the glyph's side bearing as well as punctuation.
                            const int allowance = hanging ? qMax(1, qMin(40, gap / 2)) : 0;
                            require(!word.text.isEmpty()
                                            && word.rectangles.first().left() >= left - allowance
                                            && word.rectangles.first().right()
                                                    < left + width + allowance,
                                    "Outside margins displaced column selection geometry");
                        }
                        for (int x : { 10, 990 }) {
                            require(qGray(image.pixel(x, 60)) == (dark ? 0 : 255),
                                    "Outside margin has the wrong paper color");
                            require(model.wordAt({ x, 60 }).text.isEmpty(),
                                    "Outside margin selected a word");
                        }
                        if (gap > 0)
                            require(model.wordAt({ 40 + width + gap / 2, 60 }).text.isEmpty(),
                                    "Custom column gap selected text");
                    }
                    model.resize({ 800, 1000 });
                    require(model.state().value("columns").toInt() == 2
                                    && model.state().value("column_spacing").toInt() == 90,
                            "Portrait rotation changed the column choice or spacing");
                    model.resize({ 1000, 800 });
                    require(model.state().value("columns").toInt() == 2
                                    && model.state().value("column_spacing").toInt() == 90,
                            "Landscape rotation changed the column choice or spacing");
                    require(model.applyAppearance({ { "margin_pixels", 60 } }),
                            "Cannot change outer margins");
                    require(model.applyAppearance({ { "columns", 1 } }),
                            "Cannot restore one column with outer margins");
                    kobalt::EngineDocument single(kobalt::test::readerFonts());
                    open(single, paragraph, {},
                         { { "hanging_punctuation", hanging },
                           { "margin_pixels", 60 },
                           { "top_margin_pixels", 25 },
                           { "bottom_margin_pixels", 35 },
                           { "dark_mode", dark } },
                         { 1000, 800 }, rtl);
                    require(model.image(0) == single.image(0),
                            "Column settings changed single-column margins");
                    require(!model.applyAppearance({ { "column_spacing", -1 } })
                                    && !model.applyAppearance(
                                            { { "column_spacing",
                                                kobalt::maximumMarginPixels() + 1 } }),
                            "Invalid column spacing was accepted");
                }
    kobalt::EngineDocument cramped(kobalt::test::readerFonts());
    open(cramped, "<p>" + QByteArray("Amber birch cedar dogwood elm fir. ").repeated(100) + "</p>",
         {}, { { "columns", 2 }, { "margin_pixels", 120 }, { "column_spacing", 120 } },
         { 320, 800 });
    const auto crampedMatches = cramped.searchPage("Amber", 0);
    require(!crampedMatches.hits.isEmpty(), "Tight viewport lost its text");
    for (const auto &hit : crampedMatches.hits)
        for (const auto &rect : cramped.rangeRects(hit.start, hit.end))
            require(rect.left() >= 80 && rect.right() < 240,
                    "Tight viewport let column text escape outer margins");
    const QByteArray sample = "<h2>Two columns</h2><p>"
            + QByteArray(
                      "A reader keeps the chapter ready. Changing the size of the words should preserve the passage. We can turn the page, visit another chapter, and return to the same place. ")
                      .repeated(30)
            + "</p>";
    const auto captures = qEnvironmentVariable("KOBALT_TEST_COLUMN_SPACING_CAPTURE_DIR");
    for (int margin : { 0, 48, 80 }) {
        kobalt::EngineDocument relative(kobalt::test::readerFonts());
        open(relative, sample, {},
             { { "two_columns_landscape", true },
               { "column_spacing_percent", 150 },
               { "margin_pixels", margin } },
             { 1000, 800 });
        require(relative.state().value("columns").toInt() == 2
                        && relative.state().value("column_spacing").toInt() == 60,
                "Font-relative column spacing changed with outside margins");
        if (!captures.isEmpty()) {
            require(QDir().mkpath(captures), "Cannot create spacing capture directory");
            require(relative.image(0).save(captures
                                           + QString("/font-size-margin-%1.png").arg(margin)),
                    "Cannot save text-size sample");
            kobalt::EngineDocument marginRelative(kobalt::test::readerFonts());
            open(marginRelative, sample, {},
                 { { "columns", 2 },
                   { "column_spacing", qRound(margin * 1.5) },
                   { "margin_pixels", margin } },
                 { 1000, 800 });
            require(marginRelative.image(0).save(captures
                                                 + QString("/page-margin-%1.png").arg(margin)),
                    "Cannot save margin-relative sample");
        }
        require(relative.applyAppearance({ { "font_pixels", 60 } })
                        && relative.state().value("column_spacing").toInt() == 90,
                "Changing text size did not update the column separator");
        const auto position = relative.position();
        relative.resize({ 800, 1000 });
        require(relative.state().value("columns").toInt() == 1
                        && relative.state().value("two_columns_landscape").toBool(),
                "Portrait did not temporarily use one column");
        relative.resize({ 1000, 800 });
        require(relative.state().value("columns").toInt() == 2
                        && relative.state().value("column_spacing").toInt() == 90,
                "Landscape did not restore two columns and the font-relative gap");
        require(relative.goTo(position), "Rotation lost the saved passage");
        require(relative.applyAppearance({ { "two_columns_landscape", false } })
                        && relative.state().value("columns").toInt() == 1,
                "Disabling landscape columns retained the current two-column rendering");
        require(!relative.applyAppearance({ { "column_spacing_percent", 99 } })
                        && !relative.applyAppearance({ { "column_spacing_percent", 301 } }),
                "Invalid relative column spacing was accepted");
    }
    std::puts(
            "PASS: one and two columns preserve text, geometry, chapter progress, cached layouts and screen navigation");
}
