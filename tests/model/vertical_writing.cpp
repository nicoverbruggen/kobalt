#include "reader_inputs.h"
#include "check.h"
#include "engine/crengine/engine_document.h"
#include <QGuiApplication>
#include <QColor>
#include <cstdio>

using namespace kobalt;

static QRect ink(const QImage &image)
{
    QRect box;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto c = image.pixel(x, y);
            if (qRed(c) < 100 && qGreen(c) < 100 && qBlue(c) < 100)
                box = box.united(QRect(x, y, 1, 1));
        }
    return box;
}

// Checks vertical Japanese text: column layout, page progression, selection, reflow, mixed writing modes and combined underline and overline.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const auto chapter = [](const QByteArray &mode) {
        return QByteArray(
                       "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>body{margin:0;writing-mode:")
                + mode
                + ";font-family: Noto Serif CJK JP}p{margin:0;text-indent:0}</style></head><body><p>日本語縦書文章</p></body></html>";
    };
    QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata><title>Writing modes</title><language>ja</language></metadata><manifest><item id='v' href='v.xhtml' media-type='application/xhtml+xml'/><item id='h' href='h.xhtml' media-type='application/xhtml+xml'/></manifest><spine page-progression-direction='rtl'><itemref idref='v'/><itemref idref='h'/></spine></package>" },
        { "v.xhtml", chapter("vertical-rl") },
        { "h.xhtml", chapter("horizontal-tb") }
    };
    const auto open = [&](EngineDocument &reader, QString *error) {
        QMap<QString, qint64> sizes;
        for (auto it = data.cbegin(); it != data.cend(); ++it)
            sizes[it.key()] = it->size();
        auto resources = std::make_shared<ResourceCache>(
                data.keys(),
                [data](const QString &name, const ResourceCache::Sink &sink, QString *) {
                    const auto bytes = data.value(name);
                    return sink(bytes.constData(), bytes.size());
                },
                std::make_shared<std::atomic_bool>(false));
        return reader.openResources(resources, sizes, QSize(600, 800), {}, error);
    };
    EngineDocument model(test::readerFonts());
    QString error;
    require(open(model, &error), qPrintable(error));
    require(model.pages() == 2, "Separate writing modes must occupy separate pages");
    const auto v = model.image(0), h = model.image(1);
    const auto vb = ink(v), hb = ink(h);
    std::printf("vertical %d,%d %dx%d; horizontal %d,%d %dx%d\n", vb.x(), vb.y(), vb.width(),
                vb.height(), hb.x(), hb.y(), hb.width(), hb.height());
    if (argc > 1) {
        v.save(QString::fromLocal8Bit(argv[1]) + "/vertical.png");
        h.save(QString::fromLocal8Bit(argv[1]) + "/horizontal.png");
    }
    require(vb.height() > vb.width() * 3, "Japanese text did not form a vertical column");
    require(hb.width() > hb.height() * 3, "Horizontal chapter inherited vertical layout");
    require(vb.center().x() > 300, "Vertical column must start on the right");
    require(model.state().value("page_progression_rtl").toBool(), "EPUB page progression was lost");
    require(model.go(0), "Cannot return to the vertical chapter");
    bool selected = false;
    for (int y = vb.top(); y <= vb.bottom() && !selected; y += 4)
        for (int x = vb.left(); x <= vb.right() && !selected; x += 4) {
            const auto word = model.wordAt({ x, y });
            if (word.text.isEmpty())
                continue;
            require(!model.rangeRects(word.start, word.end).isEmpty(),
                    "Vertical selection lost its highlight");
            const auto geometry = model.annotationGeometry(word.start, word.end);
            require(geometry.vertical && geometry.start.visible && geometry.end.visible,
                    "Vertical source endpoints or explicit writing axis are missing");
            require(geometry.start.outward == QPoint(0, -1) && geometry.end.outward == QPoint(0, 1),
                    "Vertical endpoint normals did not rotate with the source glyph");
            require(geometry.start.first.y() == geometry.start.last.y()
                            && geometry.end.first.y() == geometry.end.last.y()
                            && geometry.start.first.y() < geometry.end.first.y(),
                    "Vertical endpoint spans did not rotate across the column");
            require(model.goTo(word.start), "Vertical selection position cannot be restored");
            selected = true;
        }
    require(selected, "Vertical glyphs cannot be selected");
    const auto anchor = model.position();
    require(model.style(56, model.spacing()), "Cannot reflow vertical text");
    require(model.goTo(anchor), "Vertical bookmark did not survive reflow");
    require(model.pagesOf({ anchor }).first() == model.page(),
            "Vertical bookmark resolved to the wrong page");
    // A single spine item must restore the parent direction after a mixed block.
    data["book.opf"].replace("<itemref idref='h'/>", "");
    for (const bool vertical : { true, false }) {
        const QByteArray outer = vertical ? "vertical-rl" : "horizontal-tb";
        const QByteArray inner = vertical ? "horizontal-tb" : "vertical-rl";
        auto content = chapter(outer);
        content.replace("</body>",
                        "<div style='writing-mode:" + inner
                                + "'><p>日本語横書文章</p></div><p>日本語続文章</p></body>");
        data["v.xhtml"] = content;
        EngineDocument mixed(test::readerFonts());
        require(open(mixed, &error), qPrintable(error));
        require(mixed.pages() == 3, "Mixed blocks must occupy three separate pages");
        for (int page = 0; page < 3; ++page) {
            const auto bounds = ink(mixed.image(page));
            const bool expectedVertical = page == 1 ? !vertical : vertical;
            require(expectedVertical ? bounds.height() > bounds.width() * 3
                                     : bounds.width() > bounds.height() * 3,
                    "A mixed block or following paragraph used the wrong writing direction");
        }
    }
    // Inline mixtures open without a book-wide rejection; their layout can still be incorrect.
    for (const auto *display : { "inline", "inline-block" }) {
        auto content = chapter("vertical-rl");
        content.replace("日本語縦書文章",
                        QByteArray("日本語<span style='writing-mode:horizontal-tb;display:")
                                + display + "'>Horizontal</span>続文章");
        data["v.xhtml"] = content;
        EngineDocument mixed(test::readerFonts());
        require(open(mixed, &error), qPrintable(error));
        require(!ink(mixed.image(0)).isEmpty(), "Inline mixed content rendered an empty page");
    }
    // Combined decorations must equal the union of each separate stroke,
    // including an overline that continues through a nested underline span.
    for (const auto *mode : { "vertical-rl", "vertical-lr" }) {
        for (const bool nested : { false, true }) {
            QImage pages[3];
            for (int variant = 0; variant < 3; ++variant) {
                const bool underline = variant != 0, overline = variant != 1;
                const QByteArray rules = underline && overline ? "underline overline"
                        : underline                            ? "underline"
                                                               : "overline";
                const QByteArray text = nested
                        ? "<span style='text-decoration:"
                                + QByteArray(overline ? "overline" : "none")
                                + "'>日本<span style='font-size:150%;text-decoration:"
                                + QByteArray(underline ? "underline" : "none")
                                + "'>語縦</span>書文章</span>"
                        : "<span style='text-decoration:" + rules + "'>日本語縦書文章</span>";
                auto content = chapter(mode);
                content.replace("日本語縦書文章", text);
                data["v.xhtml"] = content;
                EngineDocument decorated(test::readerFonts());
                require(open(decorated, &error), qPrintable(error));
                pages[variant] = decorated.image(0);
            }
            for (int y = 0; y < pages[2].height(); ++y)
                for (int x = 0; x < pages[2].width(); ++x)
                    require(qGray(pages[2].pixel(x, y))
                                    == qMin(qGray(pages[0].pixel(x, y)),
                                            qGray(pages[1].pixel(x, y))),
                            "Combined vertical decorations lost a stroke or changed its edge");
        }
    }
    std::puts(
            "PASS: mixed chapters and blocks, inline acceptance, page progression, selection and reflow");
}
