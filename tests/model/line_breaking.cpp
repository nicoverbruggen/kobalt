#include "reader_inputs.h"
#include "support/qt_compat.h"
#include "engine/blaze/adapter/engine_document.h"
#include <lvtinydom.h>
#include <textlang.h>
#include <QDir>
#include <QGuiApplication>
#include <cstdio>
#include "check.h"

struct Layout
{
    QImage image;
    QRect iterationMark;
};

static void checkParagraphBreaks()
{
    ldomDocument document;
    auto *node = document.getRootNode()->insertChildText(lString32(U"fixture"));
    auto checkBreak = [&](const lString32 &text, const lChar32 *tag, css_line_break_t lineBreak,
                          css_word_break_t wordBreak, int position, bool allowed) {
        auto *language = TextLangMan::getTextLangCfg(lString32(tag), true);
        for (int visit = 0; visit < 2; ++visit) {
            const auto *breaks = document.paragraphBreaks(node, text.c_str(), text.length(),
                                                          language, lineBreak, wordBreak);
            require(breaks && (breaks[position] == LINEBREAK_ALLOWBREAK) == allowed,
                    "Cached paragraph break does not match its text, language and CSS");
        }
    };
    // Every request uses the same slot. Change one input at a time.
    checkBreak(U"字々字", U"ja", css_lb_loose, css_wb_normal, 1, true);
    checkBreak(U"字々字", U"ja", css_lb_strict, css_wb_normal, 1, false);
    checkBreak(U"字字字", U"ja", css_lb_strict, css_wb_normal, 1, true);
    checkBreak(U"abc", U"en", css_lb_auto, css_wb_normal, 1, false);
    checkBreak(U"abc", U"en", css_lb_auto, css_wb_break_all, 1, true);
    checkBreak(U"x a t", U"en", css_lb_auto, css_wb_normal, 4, true);
    checkBreak(U"x a t", U"pl", css_lb_auto, css_wb_normal, 4, false);
    checkBreak(U"x  y", U"en", css_lb_auto, css_wb_normal, 3, true);
    checkBreak(U"x\u00a0y", U"en", css_lb_auto, css_wb_normal, 2, false);
    checkBreak(U"x\u00a0y", U"en", css_lb_cr_loose, css_wb_normal, 2, true);
    checkBreak(U"x \u2060y", U"en", css_lb_auto, css_wb_normal, 3, false);
}

static Layout render(const QByteArray &mode)
{
    const QByteArray content =
            QByteArray("<html xmlns='http://www.w3.org/1999/xhtml'><head><style>")
            + "html,body{margin:0;padding:0}p{margin:0;text-indent:0;font-family:'Noto Serif CJK JP';font-size:32px;line-height:1.25;line-break:"
            + mode + "}</style></head><body><p lang='ja'>日本語々日本語日本語</p></body></html>";
    const QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata><title>Line breaking</title><language>ja</language></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "c.xhtml", content }
    };
    QMap<QString, qint64> sizes;
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto resources = std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    QString error;
    require(model.openResources(
                    resources, sizes, QSize(122, 480),
                    kobalt::qt::object({ { "margin_pixels", 0 }, { "font_pixels", 32 } }), &error),
            qPrintable(error));
    if (mode == "strict")
        checkParagraphBreaks();
    QVector<kobalt::SearchHit> hits;
    for (int page = 0; page < model.pages(); ++page)
        hits += model.searchPage("々", page).hits;
    require(hits.size() == 1, "Iteration mark search did not find its single source character");
    require(model.goTo(hits.first().start), "Cannot navigate to the iteration mark");
    const auto rects = model.rangeRects(hits.first().start, hits.first().end);
    require(rects.size() == 1 && !rects.first().isEmpty(),
            "Iteration mark has no single line rectangle");
    return { model.image(model.page()), rects.first() };
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // Keep the narrow CSS-pixel viewport used to distinguish these two
    // line-break modes. Device-density CSS sizing has its own reader check.
    const int previousPpi = kobalt::readingPpi.exchange(96);
    const auto strict = render("strict");
    const auto loose = render("loose");
    kobalt::readingPpi.store(previousPpi);
    if (argc > 1) {
        QDir().mkpath(QString::fromLocal8Bit(argv[1]));
        require(strict.image.save(QString::fromLocal8Bit(argv[1]) + "/strict.png"),
                "Cannot save strict layout image");
        require(loose.image.save(QString::fromLocal8Bit(argv[1]) + "/loose.png"),
                "Cannot save loose layout image");
    }
    std::printf("strict iteration mark at (%d,%d); loose at (%d,%d)\n", strict.iterationMark.x(),
                strict.iterationMark.y(), loose.iterationMark.x(), loose.iterationMark.y());
    require(loose.iterationMark.y() > strict.iterationMark.y()
                    || (loose.iterationMark.y() == strict.iterationMark.y()
                        && loose.iterationMark.x() < strict.iterationMark.x()),
            "Japanese line-break: loose did not place the iteration mark before the strict layout position");
    std::puts("PASS: Japanese strict and loose line breaking place the iteration mark differently");
}
