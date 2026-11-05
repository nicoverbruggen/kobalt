#include "reader_inputs.h"
#include "support/qt_compat.h"
#include "engine/crengine/engine_document.h"
#include <QDir>
#include <QGuiApplication>
#include <cstdio>
#include "check.h"

struct Layout
{
    QImage image;
    QRect iterationMark;
};

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
