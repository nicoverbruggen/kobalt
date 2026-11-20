#include "engine/blaze/adapter/engine_document.h"

#include "check.h"
#include "reader_inputs.h"

#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdio>

namespace {
QRect ink(const QImage &image, bool red = false)
{
    QRect result;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto c = image.pixelColor(x, y);
            if (red ? c.red() > 180 && c.green() < 80 && c.blue() < 80
                    : c.red() < 80 && c.green() < 80 && c.blue() < 80) {
                result = result.united(QRect(x, y, 1, 1));
            }
        }
    }
    return result;
}

void open(kobalt::EngineDocument &model, const QByteArray &body, const QByteArray &css = {},
          QJsonObject saved = {}, QSize size = QSize(800, 1000), bool preceding = false,
          bool following = false, const QByteArray &before = "<p>Previous chapter.</p>",
          bool beforeLinear = true)
{
    QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Page layout</title></metadata><manifest><item id='before' href='before.xhtml' media-type='application/xhtml+xml'/><item id='page' href='page.xhtml' media-type='application/xhtml+xml'/><item id='after' href='after.xhtml' media-type='application/xhtml+xml'/></manifest><spine>"
                  + QByteArray(!preceding             ? ""
                                       : beforeLinear ? "<itemref idref='before'/>"
                                                      : "<itemref idref='before' linear='no'/>")
                  + "<itemref idref='page'/>"
                  + QByteArray(following ? "<itemref idref='after'/>" : "")
                  + "</spine></package>" },
        { "after.xhtml", "<html><body><p>Next chapter.</p></body></html>" },
        { "before.xhtml", "<html><body>" + before + "</body></html>" },
        { "page.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}" + css
                  + "</style></head><body>" + body + "</body></html>" }
    };
    QMap<QString, qint64> sizes;
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        sizes[it.key()] = it.value().size();
    }
    auto resources = std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    saved["font_pixels"] = 32;
    saved["margin_pixels"] = 0;
    saved["top_margin_pixels"] = 0;
    saved["bottom_margin_pixels"] = 0;
    QString error;
    require(model.openResources(resources, sizes, size, saved, &error), qPrintable(error));
}

QRect wordRect(kobalt::EngineDocument &doc, const QString &word)
{
    auto hits = doc.searchText(word, {}, 2).hits;
    require(hits.size() == 1, "Flex item missing or duplicated");
    const auto page = doc.pagesOf({ hits.first().start });
    require(page.size() == 1 && doc.go(page.first()), "Cannot navigate to flex item");
    const auto rects = doc.rangeRects(hits.first().start, hits.first().end);
    require(!rects.isEmpty(), "Flex item has no selection bounds");
    return rects.first();
}

void flexRows()
{
    kobalt::readingPpi.store(96);
    const QByteArray body = "<div class='row'><span>Alpha</span><span>Bravo</span>"
                            "<span>Charlie</span></div>";
    const QByteArray css = ".row{display:flex;flex-wrap:wrap;justify-content:center;gap:6px 10px}"
                           ".row span{width:110px}";
    for (int width : { 410, 260, 200 }) {
        kobalt::EngineDocument doc(kobalt::test::readerFonts());
        open(doc, body, css, {}, QSize(width, 600));
        const QRect a = wordRect(doc, "Alpha"), b = wordRect(doc, "Bravo"),
                    c = wordRect(doc, "Charlie");
        std::printf("Flex rows at %d: (%d,%d) (%d,%d) (%d,%d)\n", width, a.x(), a.y(), b.x(), b.y(),
                    c.x(), c.y());
        require(width >= 260 ? (a.y() == b.y() && b.x() - a.x() == 120) : a.y() < b.y(),
                "Flex items did not fit or wrap at the expected width");
        require(width == 410 ? c.y() == a.y() : c.y() > b.y(), "Final flex item on wrong line");
        const int count = width == 410 ? 3 : width == 260 ? 2 : 1;
        require(qAbs(a.x() - (width - count * 110 - (count - 1) * 10) / 2) <= 2,
                "Row is not centered");
    }
    kobalt::EngineDocument grow(kobalt::test::readerFonts());
    open(grow, body,
         ".row{display:flex}.row span{flex:1 1 0%;min-width:0}"
         ".row span:first-child{flex-grow:2}",
         {}, QSize(400, 600));
    const auto ga = wordRect(grow, "Alpha"), gb = wordRect(grow, "Bravo"),
               gc = wordRect(grow, "Charlie");
    require(ga.y() == gc.y() && gb.x() - ga.x() == 200 && gc.x() - gb.x() == 100,
            "Flex factors did not distribute row space in a 2:1:1 ratio");
    kobalt::EngineDocument shrink(kobalt::test::readerFonts());
    open(shrink, body,
         ".row{display:flex}.row span{flex:0 1 150px;min-width:0}"
         ".row span:first-child{flex-shrink:2}",
         {}, QSize(330, 600));
    const auto sa = wordRect(shrink, "Alpha"), sb = wordRect(shrink, "Bravo"),
               sc = wordRect(shrink, "Charlie");
    require(sa.y() == sc.y() && sb.x() - sa.x() == 90 && sc.x() - sb.x() == 120,
            "Negative free space was not shared by the scaled shrink factors");
    kobalt::EngineDocument tall(kobalt::test::readerFonts());
    open(tall,
         "<div class='row'><div>First" + QByteArray("<br/>continuation").repeated(40)
                 + "<br/>Last</div><div>Second</div></div>",
         ".row{display:flex;flex-wrap:wrap}.row>div{width:150px}", {}, QSize(400, 480));
    const auto firstPage = tall.pagesOf({ tall.searchText("First", {}, 2).hits.value(0).start });
    const auto lastPage = tall.pagesOf({ tall.searchText("Last", {}, 2).hits.value(0).start });
    require(firstPage.size() == 1 && lastPage.size() == 1 && firstPage.first() < lastPage.first(),
            "A tall flex item lost its page breaks");
    wordRect(tall, "First");
    wordRect(tall, "Last");
    wordRect(tall, "Second");
    // The fill gives the border box independently of glyph bearings.
    const auto redItem = [&](QByteArray display, QByteArray container, QByteArray first,
                             QByteArray second, bool inlineBox = false) {
        kobalt::EngineDocument page(kobalt::test::readerFonts());
        const QByteArray group = "<span class='group'><span class='first'>A</span>"
                                 "<span class='second'>B</span></span>";
        const auto body = inlineBox ? "<p>X " + group + " Z</p>" : group;
        open(page, body,
             ".group{display:" + display + ";" + container
                     + "}.first,.second{height:32px;font-size:16px;line-height:24px}"
                       ".first{"
                     + first + "}.second{background:red;" + second + "}",
             {}, QSize(400, 240));
        return ink(page.image(0), true);
    };
    require(redItem("flex", "align-items:center;gap:12px", "width:64px;height:64px", "width:160px")
                    == QRect(76, 16, 160, 32),
            "Center a label beside a taller item");
    require(redItem("flex",
                    "flex-direction:column;align-items:center;gap:12px;height:140px;"
                    "justify-content:center",
                    "width:180px", "width:120px")
                    == QRect(140, 76, 120, 32),
            "Center a stack within a definite height");
    require(redItem("flex", "", "width:25%", "flex:1;min-width:0") == QRect(100, 0, 300, 32),
            "Resolve percentage flex item width against the row");
    require(redItem("flex", "", "flex:0 1 600px;min-width:0", "flex:0 1 200px;min-width:0")
                    == QRect(300, 0, 100, 32),
            "Shrink in proportion to the original bases");
    require(redItem("flex", "", "width:240px", "width:240px") == QRect(200, 0, 200, 32),
            "Short labels can shrink below a specified width");
    require(redItem("flex", "", "width:25%;min-width:160px", "flex:1;min-width:0")
                    == QRect(160, 0, 240, 32),
            "A percentage width respects an explicit minimum");
    require(redItem("flex",
                    "flex-direction:column;height:140px;align-items:center;justify-content:center",
                    "width:80px", "width:50%;padding:0 10px;box-sizing:content-box")
                    == QRect(90, 70, 220, 32),
            "Center the padded percentage border box");
    const QRect padded =
            redItem("flex", "", "width:400px;padding:0 122px;box-sizing:border-box", "width:400px");
    require(padded.x() > 244 && padded.right() == 399 && padded.y() == 0,
            "Keep the word and its padding inside the minimum border-box width");
    const auto fixed = redItem("inline-flex", "width:152px;gap:8px;vertical-align:top",
                               "width:64px", "width:80px", true);
    const auto natural =
            redItem("inline-flex", "gap:8px;vertical-align:top", "width:64px", "width:80px", true);
    require(fixed == natural && fixed.width() == 80 && fixed.x() > 72,
            "The natural inline flex width must include both items and their gap");
    kobalt::EngineDocument direct(kobalt::test::readerFonts()),
            wrapped(kobalt::test::readerFonts());
    const QByteArray headingCss = "h2{display:flex;justify-content:center;text-align:left}";
    open(direct, "<h2>Chapter</h2>", headingCss, {}, QSize(400, 240));
    open(wrapped, "<h2><span>Chapter</span></h2>", headingCss, {}, QSize(400, 240));
    require(wordRect(direct, "Chapter") == wordRect(wrapped, "Chapter")
                    && qAbs(wordRect(direct, "Chapter").center().x() - 200) < 4,
            "Center a text-only flex heading as an anonymous item");
    const QByteArray poem = "<div class='poem'><div><p>Opening</p><p>Verse</p></div></div>";
    kobalt::EngineDocument doc(kobalt::test::readerFonts());
    open(doc, poem, ".poem{display:flex;justify-content:center}", {}, QSize(410, 600));
    const auto opening = wordRect(doc, "Opening"), verse = wordRect(doc, "Verse");
    require(qAbs(opening.center().x() - 205) <= 3 && opening.x() == verse.x(),
            "Center the poem while keeping its verse left aligned");
    kobalt::EngineDocument plain(kobalt::test::readerFonts()), unused(kobalt::test::readerFonts());
    open(plain, poem);
    open(unused, poem,
         ".absent{display:flex;flex-wrap:wrap}.poem{display:flex;display:block;"
         "justify-content:center}@supports(display:flex){.poem{display:none}}");
    require(plain.image(0) == unused.image(0), "An unmatched flex rule changed ordinary blocks");
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    flexRows();
    const QByteArray svg =
            "<svg xmlns='http://www.w3.org/2000/svg' width='100%' height='100%' viewBox='0 0 200 100' preserveAspectRatio='xMidYMid meet'><rect width='200' height='100' fill='red'/></svg>";
    for (int ppi : { 300, 212 }) {
        kobalt::readingPpi.store(ppi);
        kobalt::EngineDocument heading(kobalt::test::readerFonts());
        open(heading, "<h2>Chapter heading</h2>", "h2{margin:2em 0;font-size:1em}", {},
             QSize(800, 1000), true);
        require(heading.pages() == 2, "Heading lost its chapter break");
        const auto original = heading.image(1);
        const auto trimmed = ink(original);
        require(heading.state().value("reduce_chapter_spacing").toBool(),
                "Space saving is not on by default");
        require(heading.applyAppearance(
                        kobalt::appearance::Patch({ { "reduce_chapter_spacing", false } })),
                "Cannot preserve publisher spacing");
        require(heading.state().value("normalize_page_margins").toBool(),
                "Chapter spacing changed the margin rule");
        const auto preserved = ink(heading.image(1));
        std::printf("%d ppi heading top: trimmed=%d preserved=%d\n", ppi, trimmed.top(),
                    preserved.top());
        require(preserved.top() >= trimmed.top() + 60,
                "Forced break discarded publisher top margin");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        QString error;
        require(preview.openPreview(heading, QSize(800, 1000), &error), qPrintable(error));
        require(!preview.state().value("reduce_chapter_spacing").toBool(),
                "Preview lost publisher spacing preference");
        require(heading.applyAppearance(
                        kobalt::appearance::Patch({ { "reduce_chapter_spacing", true } })),
                "Cannot restore space saving");
        require(heading.image(1) == original, "Spacing toggle did not restore original layout");

        QTemporaryDir cache;
        require(cache.isValid(), "Cannot create page layout cache");
        {
            kobalt::EngineDocument cached(kobalt::test::readerFonts());
            cached.setDocumentCache(cache.path(), "spacing-fixture", "page-layout-test", 4096);
            open(cached, "<h2>Chapter heading</h2>", "h2{margin:2em 0;font-size:1em}", {},
                 QSize(800, 1000), true);
            require(cached.saveDocumentCache(), "Cannot save trimmed layout cache");
        }
        {
            kobalt::EngineDocument cached(kobalt::test::readerFonts());
            cached.setDocumentCache(cache.path(), "spacing-fixture", "page-layout-test", 4096);
            open(cached, "<h2>Chapter heading</h2>", "h2{margin:2em 0;font-size:1em}",
                 { { "reduce_chapter_spacing", false } }, QSize(800, 1000), true);
            require(cached.openedFromCache(),
                    "Spacing fixture did not reopen from its document cache");
            require(ink(cached.image(1)).top() == preserved.top(),
                    "Document cache retained trimmed publisher margins");
        }

        kobalt::EngineDocument margins(kobalt::test::readerFonts());
        open(margins, "<p>Body margins</p>", "body{margin:20px;padding:10px}");
        const auto zero = ink(margins.image(0));
        require(margins.applyAppearance(
                        kobalt::appearance::Patch({ { "normalize_page_margins", false } })),
                "Cannot restore body margins");
        require(margins.state().value("reduce_chapter_spacing").toBool(),
                "Margin rule changed chapter spacing");
        const auto authored = ink(margins.image(0));
        require(authored.left() > zero.left() + 30 && authored.top() > zero.top() + 10,
                "Publisher body spacing was still overridden");

        for (const auto size : { QSize(800, 1000), QSize(1000, 800) }) {
            kobalt::EngineDocument image(kobalt::test::readerFonts());
            open(image, "<div>" + svg + "</div>", {}, {}, size, true);
            require(image.pages() == 2, "Full-page SVG changed chapter boundaries");
            const auto rect = ink(image.image(1), true);
            std::printf("SVG %dx%d: %d,%d %dx%d\n", size.width(), size.height(), rect.x(), rect.y(),
                        rect.width(), rect.height());
            require(!rect.isEmpty() && qAbs(rect.center().y() - size.height() / 2) < 20,
                    "Full-page SVG is not vertically centred");
            require(qAbs(rect.width() - 2 * rect.height()) < 5, "SVG aspect ratio changed");
            image.resize(QSize(size.height(), size.width()));
            const auto rotated = ink(image.image(1), true);
            require(qAbs(rotated.center().y() - size.width() / 2) < 20,
                    "SVG viewport did not update after rotation");

            for (bool avoid : { true, false }) {
                kobalt::EngineDocument verse(kobalt::test::readerFonts());
                open(verse,
                     "<div class='outer'><div class='inner'><p>First verse line.</p><p>Second verse line.</p></div></div>",
                     ".outer{display:table;width:100%;height:100%;position:fixed}.inner{display:table-cell;vertical-align:middle}",
                     { { "reduce_chapter_spacing", avoid } }, size, true, true);
                require(verse.pages() == 3, "Table did not start its own chapter page");
                require(!verse.pageExcerpt(0).contains("verse"),
                        "Table text leaked into previous chapter");
                require(verse.pageExcerpt(2).startsWith("Next chapter"),
                        "Table left a delayed break in the following chapter");
                const auto rect = ink(verse.image(1));
                std::printf("Verse %dx%d avoid=%d: y=%d height=%d\n", size.width(), size.height(),
                            avoid, rect.y(), rect.height());
                require(!rect.isEmpty() && qAbs(rect.center().y() - size.height() / 2) < 45,
                        "Full-page verse is not vertically centred");
            }
        }
        kobalt::EngineDocument stretched(kobalt::test::readerFonts());
        kobalt::EngineDocument styledImage(kobalt::test::readerFonts());
        open(styledImage,
             QByteArray(svg).replace("width='100%' height='100%'",
                                     "style='width:100%;height:100%'"));
        require(qAbs(ink(styledImage.image(0), true).center().y() - 500) < 20,
                "CSS percentage dimensions prevented full-page SVG alignment");
        open(stretched, QByteArray(svg).replace("xMidYMid meet", "none"));
        const auto stretch = ink(stretched.image(0), true);
        require(stretch.width() > 780 && stretch.height() > 950,
                "SVG preserveAspectRatio=none did not fill the page");
        kobalt::EngineDocument narrow(kobalt::test::readerFonts());
        open(narrow, QByteArray(svg).replace("width='100%'", "width='50%'"));
        require(ink(narrow.image(0), true).width() < 500,
                "Partial-width SVG became a full-page image");
        kobalt::EngineDocument ordinaryTable(kobalt::test::readerFonts());
        open(ordinaryTable, "<table><tr><td>Ordinary table.</td></tr></table>", {}, {},
             QSize(800, 1000), true, true);
        require(ordinaryTable.pages() == 3, "Ordinary table lost its chapter boundaries");
        require(ink(ordinaryTable.image(1)).top() < 100,
                "Ordinary table became vertically centred");
        // A spine item that is not part of the linear reading order, such
        // as a contents page, has pages of its own. When it ended inside a
        // block that must stay whole, the engine broke the page twice before
        // the next chapter, with nothing between the breaks: the reader saw
        // an empty page there.
        kobalt::EngineDocument keptEnd(kobalt::test::readerFonts());
        open(keptEnd, "<section><h2>Prologue</h2><p>First paragraph.</p></section>", {}, {},
             QSize(800, 1000), true, false,
             "<div style='page-break-inside:avoid'><p>Contents.</p></div>", false);
        require(keptEnd.pages() == 2, "A kept block at the end of a chapter made an empty page");
        require(keptEnd.pageStart(0) != keptEnd.pageStart(1), "Two pages start at the same place");
        kobalt::EngineDocument inlineImage(kobalt::test::readerFonts());
        open(inlineImage, "<p>Before</p>" + svg + "<p>After</p>");
        require(inlineImage.pages() == 1, "Inline SVG became page-height");
        require(ink(inlineImage.image(0), true).top() < 200, "Inline SVG was vertically centred");
    }
}
