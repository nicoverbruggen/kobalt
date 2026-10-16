#include "reader_inputs.h"
#include "support/qt_compat.h"
#include "engine/crengine/reader_model.h"
#include <QDir>
#include <QGuiApplication>
#include <QMap>
#include <atomic>
#include "check.h"

namespace {
struct Probe {
    int page;
    QRect rect;
};

Probe marker(kobalt::ReaderModel &model, const QString &text) {
    QVector<kobalt::SearchHit> hits;
    for (int page = 0; page < model.pages(); ++page) {
        const auto batch = model.searchPage(text, page);
        hits += batch.hits;
    }
    require(hits.size() == 1, qPrintable("MathML marker is not unique: " + text));
    const auto rectangles = model.rangeRects(hits.first().start, hits.first().end);
    require(!rectangles.isEmpty(), qPrintable("MathML marker has no geometry: " + text));
    QRect rect;
    for (const auto &part : rectangles)
        rect = rect.isNull() ? part : rect.united(part);
    return {hits.first().page, rect};
}

void openFixture(kobalt::ReaderModel &model, const QJsonObject &settings = {}, const QByteArray &content = {},
                 const QByteArray &publisherCss = {}) {
    QMap<QString, QByteArray> data{
        {"mimetype", "application/epub+zip"},
        {"META-INF/container.xml",
         "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>"},
        {"book.opf",
         "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>MathML fraction test</title></metadata><manifest><item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='chapter'/></spine></package>"},
        {"chapter.xhtml",
         "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>body{margin:0}p{margin:0;text-indent:0}</style></head><body>"
         "<p>MATH-FRACTION-BEGIN</p>"
         "<math xmlns='http://www.w3.org/1998/Math/MathML' display='block' dir='ltr'><mfrac><mn>19</mn><mn>4</mn></mfrac></math>"
         "<p>MATH-FRACTION-END</p>"
         "<p>MATH-CONTROL-BEGIN</p><p dir='ltr'>19 / 4</p><p>MATH-CONTROL-END</p>"
         "</body></html>"}};
    if (!content.isEmpty()) {
        data["chapter.xhtml"] = "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
                                "body{margin:0}p{margin:0;text-indent:0}" +
                                publisherCss + "</style></head><body>" + content + "</body></html>";
    }
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
    require(model.openResources(resources, sizes, {640, 900}, settings, &error), qPrintable(error));
}

QImage checkFraction() {
    kobalt::ReaderModel model(kobalt::test::readerFonts());
    openFixture(model);
    const auto fractionBegin = marker(model, QStringLiteral("MATH-FRACTION-BEGIN"));
    const auto fractionEnd = marker(model, QStringLiteral("MATH-FRACTION-END"));
    const auto controlBegin = marker(model, QStringLiteral("MATH-CONTROL-BEGIN"));
    const auto controlEnd = marker(model, QStringLiteral("MATH-CONTROL-END"));
    require(fractionBegin.page == fractionEnd.page && fractionBegin.page == controlBegin.page &&
                fractionBegin.page == controlEnd.page,
            "MathML fraction and control did not fit on one page");
    require(fractionEnd.rect.center().y() - fractionBegin.rect.center().y() >
                controlEnd.rect.center().y() - controlBegin.rect.center().y() + 4,
            "MathML fraction did not add vertical layout beyond the plain-text control");

    require(model.go(fractionBegin.page), "Cannot open the MathML fraction page");
    const auto image = model.image(fractionBegin.page);
    int ink = 0;
    for (int y = qMax(0, fractionBegin.rect.bottom() + 1); y < qMin(image.height(), fractionEnd.rect.top()); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 230)
                ++ink;
    require(ink > 4, "MathML fraction rendered no ink between its framing markers");
    return image;
}

void checkMathFonts(const QString &captureDirectory) {
    const QByteArray formula("<math xmlns='http://www.w3.org/1998/Math/MathML' display='block'>"
                             "<mrow><munderover><mo>∑</mo><mi>n</mi><mn>9</mn></munderover>"
                             "<mfrac><msqrt><mrow><msup><mi>x</mi><mn>2</mn></msup><mo>+</mo><mn>1</mn></mrow></msqrt>"
                             "<mrow><mn>2</mn><mi>n</mi></mrow></mfrac><mo>=</mo>"
                             "<msubsup><mo>∫</mo><mn>0</mn><mn>1</mn></msubsup><mi>f</mi>"
                             "<mo stretchy='true'>(</mo><mfrac><mi>x</mi><mi>y</mi></mfrac><mo stretchy='true'>)</mo>"
                             "<mi>d</mi><mi>x</mi></mrow></math>"
                             "<math xmlns='http://www.w3.org/1998/Math/MathML' display='block'>"
                             "<mrow><mo>∀</mo><mi>x</mi><mo>∈</mo><mi>ℝ</mi><mo>:</mo>"
                             "<mo>∂</mo><mi>f</mi><mo>→</mo><mo>∇</mo><mi>f</mi></mrow></math>");
    const auto previousPpi = kobalt::readingPpi.load();
    for (const int ppi : {300, 212}) {
        kobalt::readingPpi.store(ppi);
        for (const bool dark : {false, true}) {
            const auto settings = kobalt::qt::object(
                {{"font_pixels", kobalt::defaultFontPixels()}, {"font_face", ""}, {"dark_mode", dark}});
            kobalt::ReaderModel reference(kobalt::test::readerFonts());
            reference.setUserStyleSheet("math, math * { font-family: 'Noto Sans Math' !important; }");
            openFixture(reference, settings, formula);
            const auto expected = reference.image(0);
            for (const auto &face : {QString(), QStringLiteral("Libron"), QStringLiteral("NV Bitter")}) {
                auto selected = settings;
                selected["font_face"] = face;
                kobalt::ReaderModel model(kobalt::test::readerFonts());
                openFixture(model, selected, formula);
                if (!captureDirectory.isEmpty()) {
                    const auto name = QString("%1/actual-%2-%3-%4.png")
                                          .arg(captureDirectory)
                                          .arg(ppi)
                                          .arg(dark ? "dark" : "light")
                                          .arg(face.isEmpty() ? "publisher" : face);
                    require(model.image(0).save(name), "Cannot save MathML capture");
                }
                require(model.image(0) == expected,
                        "MathML symbols did not use the bundled math font independently of the reading font");
                require(model.applyAppearance(kobalt::qt::object({{"font_face", "NV Bitter"}})) &&
                            model.image(0) == expected &&
                            model.applyAppearance(kobalt::qt::object({{"font_face", ""}})) &&
                            model.image(0) == expected,
                        "Changing the reading font lost MathML font selection");
                kobalt::ReaderModel preview(model.fontInputs());
                QString error;
                require(preview.openPreview(model, model.viewport(), &error), qPrintable(error));
                require(preview.image(0) == expected, "Appearance preview lost MathML font selection");
            }
            if (!captureDirectory.isEmpty()) {
                const auto name =
                    QString("%1/mathml-%2-%3.png").arg(captureDirectory).arg(ppi).arg(dark ? "dark" : "light");
                require(expected.save(name), "Cannot save MathML capture");
            }
        }
    }
    kobalt::readingPpi.store(previousPpi);

    const auto publisher = kobalt::qt::object({{"font_face", ""}});
    kobalt::ReaderModel generic(kobalt::test::readerFonts()), explicitMath(kobalt::test::readerFonts());
    const QByteArray symbols("<p style='font-family:math'>∀∈∂∇→∑∫</p>");
    explicitMath.setUserStyleSheet("p { font-family: 'Noto Sans Math' !important; }");
    openFixture(generic, publisher, symbols);
    openFixture(explicitMath, publisher, symbols);
    require(generic.image(0) == explicitMath.image(0), "CSS math family did not resolve to Noto Sans Math");

    kobalt::ReaderModel named(kobalt::test::readerFonts()), namedReference(kobalt::test::readerFonts()),
        defaultMath(kobalt::test::readerFonts());
    const QByteArray namedCss("math, math * { font-family: 'Noto Serif'; }");
    namedReference.setUserStyleSheet("math, math * { font-family: 'Noto Serif' !important; }");
    openFixture(named, publisher, formula, namedCss);
    openFixture(namedReference, publisher, formula);
    openFixture(defaultMath, publisher, formula);
    require(named.image(0) == namedReference.image(0) && named.image(0) != defaultMath.image(0),
            "Generic math fallback replaced a named publisher font");
}
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    const auto image = checkFraction();
    require(!image.isNull(), "MathML fraction page did not render");
    const QString captures = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!captures.isEmpty()) {
        require(QDir().mkpath(captures), "Cannot create MathML capture directory");
    }
    checkMathFonts(captures);
    puts("PASS: MathML layout, symbol fonts, reading-font changes, previews and publisher font selection");
}
