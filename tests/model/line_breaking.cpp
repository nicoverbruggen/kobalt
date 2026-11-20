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
            const auto reuses = document.paragraphBreakReuses();
            const auto *breaks = document.paragraphBreaks(node, text.c_str(), text.length(),
                                                          language, lineBreak, wordBreak);
            require(breaks && (breaks[position] == LINEBREAK_ALLOWBREAK) == allowed,
                    "Cached paragraph break does not match its text, language and CSS");
            // Each case changes one input, so only the second visit reuses decisions.
            require(document.paragraphBreakReuses() == reuses + (visit == 1 ? 1u : 0u),
                    "A paragraph break lookup reused the wrong decisions");
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

static void open(kobalt::EngineDocument &model, const QByteArray &content, QSize size)
{
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
    QString error;
    require(model.openResources(
                    resources, sizes, size,
                    kobalt::qt::object({ { "margin_pixels", 0 }, { "font_pixels", 32 } }), &error),
            qPrintable(error));
}

// A reflow reuses the break decisions of the earlier layout. Its lines must
// match the layout that computed them.
static void checkReflowReusesBreaks()
{
    const QByteArray content =
            "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
            "html,body{margin:0;padding:0}p{margin:0;text-indent:0;font-size:20px;"
            "line-height:1.25}</style></head><body>"
            "<p lang='en'>Breaking lines again after a reflow must give the same result, "
            "with non\u2011breaking hyphens, an em\u2014dash and a long "
            "word like incomprehensibilities.</p>"
            "<p lang='ja'>日本語々日本語日本語、「かな」とカナ。日本語日本語。</p>"
            "</body></html>";
    // Pages two lines tall, so the page starts follow the line breaks. The
    // first layout computes the decisions; the return to its width reuses them.
    const auto starts = [](const kobalt::EngineDocument &model) {
        QStringList result;
        for (int page = 0; page < model.pages(); ++page)
            result << model.pageStart(page);
        return result;
    };
    for (const int width : { 180, 260 }) {
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        open(model, content, QSize(width, 50));
        const auto computed = starts(model);
        require(computed.size() > 4, "The paragraphs did not span several pages");
        // A wider margin narrows the text, then the original margin restores it.
        const auto margin = [&model](int pixels) {
            return model.applyAppearance(
                    kobalt::appearance::Patch(kobalt::qt::object({ { "margin_pixels", pixels } })));
        };
        require(margin(37) && starts(model) != computed && margin(0),
                "Cannot reflow the paragraphs");
        require(starts(model) == computed,
                qPrintable(QString("Reused break decisions changed the lines at width %1")
                                   .arg(width)));
    }
}

static Layout render(const QByteArray &mode)
{
    const QByteArray content =
            QByteArray("<html xmlns='http://www.w3.org/1999/xhtml'><head><style>")
            + "html,body{margin:0;padding:0}p{margin:0;text-indent:0;font-family:'Noto Serif CJK JP';font-size:32px;line-height:1.25;line-break:"
            + mode + "}</style></head><body><p lang='ja'>日本語々日本語日本語</p></body></html>";
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    open(model, content, QSize(122, 480));
    if (mode == "strict") {
        checkReflowReusesBreaks();
        checkParagraphBreaks();
    }
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
