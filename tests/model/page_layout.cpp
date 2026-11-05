#include "engine/crengine/engine_document.h"

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
          bool following = false)
{
    QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Page layout</title></metadata><manifest><item id='before' href='before.xhtml' media-type='application/xhtml+xml'/><item id='page' href='page.xhtml' media-type='application/xhtml+xml'/><item id='after' href='after.xhtml' media-type='application/xhtml+xml'/></manifest><spine>"
                  + QByteArray(preceding ? "<itemref idref='before'/>" : "")
                  + "<itemref idref='page'/>"
                  + QByteArray(following ? "<itemref idref='after'/>" : "")
                  + "</spine></package>" },
        { "after.xhtml", "<html><body><p>Next chapter.</p></body></html>" },
        { "before.xhtml", "<html><body><p>Previous chapter.</p></body></html>" },
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
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
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
        require(heading.applyAppearance({ { "reduce_chapter_spacing", false } }),
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
        require(heading.applyAppearance({ { "reduce_chapter_spacing", true } }),
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
        require(margins.applyAppearance({ { "normalize_page_margins", false } }),
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
        kobalt::EngineDocument inlineImage(kobalt::test::readerFonts());
        open(inlineImage, "<p>Before</p>" + svg + "<p>After</p>");
        require(inlineImage.pages() == 1, "Inline SVG became page-height");
        require(ink(inlineImage.image(0), true).top() < 200, "Inline SVG was vertically centred");
    }
}
