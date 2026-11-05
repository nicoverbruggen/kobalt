#include "engine/crengine/engine_document.h"

#include "check.h"
#include "reader_inputs.h"
#include "support/qt_compat.h"

#include <QDir>
#include <QGuiApplication>
#include <QMap>
#include <QTemporaryDir>

namespace {
std::shared_ptr<kobalt::ResourceCache> resources(const QByteArray &indent, bool inherited,
                                                 QMap<QString, qint64> &sizes)
{
    const QByteArray text = "Startmarker "
            + QByteArray("Amber birch cedar dogwood elm fir hazel oak ").repeated(10) + "Endmarker";
    const QByteArray chapter =
            "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>body{text-indent:"
            + (inherited ? indent : QByteArray("0")) + "}p{margin:0 0 0 2em;padding:0;text-indent:"
            + (inherited ? QByteArray("inherit") : indent) + "}</style></head><body><div><div><p>"
            + text + "</p></div></div></body></html>";
    const QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Indentation</title></metadata><manifest><item id='c' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "chapter.xhtml", chapter }
    };
    sizes.clear();
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        sizes[it.key()] = it.value().size();
    }
    return std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
}

int offset(kobalt::EngineDocument &model)
{
    const auto first = model.searchPage("Startmarker", 0).hits;
    const auto last = model.searchPage("Endmarker", 0).hits;
    require(first.size() == 1 && last.size() == 1, "Indentation fixture must fit on one page");
    require(model.goTo(first.first().start), "Cannot locate the first line");
    QMap<int, QRect> lines;
    for (const auto &rect : model.rangeRects(first.first().start, last.first().end)) {
        lines[rect.top()] = lines.contains(rect.top()) ? lines[rect.top()].united(rect) : rect;
    }
    require(lines.size() >= 3, "Indentation fixture needs continuation lines");
    const auto rectangles = lines.values();
    require(rectangles[1].left() == rectangles[2].left(),
            "Continuation lines have different indentation");
    return rectangles[0].left() - rectangles[1].left();
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QSize size(1072, 1800);
    const int originalPpi = kobalt::readingPpi.load();
    QString error;
    for (int ppi : { 212, 300 }) {
        kobalt::readingPpi.store(ppi);
        for (bool dark : { false, true }) {
            // 1.5em resolves to 78 (even) or 81 (odd) physical pixels.
            for (int pixels : { 54, 52 }) {
                for (const auto *unit : { "1.5em", "3ex", "3ch" }) {
                    for (bool negative : { false, true }) {
                        for (bool hanging : { false, true }) {
                            QByteArray indent = negative ? "-" : "";
                            indent += unit;
                            if (hanging) {
                                indent += " hanging";
                            }
                            const auto settings = kobalt::qt::object({ { "font_pixels", pixels },
                                                                       { "font_face", "Libron" },
                                                                       { "text_alignment", "left" },
                                                                       { "margin_pixels", 48 },
                                                                       { "dark_mode", dark },
                                                                       { "hyphenation", "off" } });
                            const int expected = pixels * 3 / 2 * (negative == hanging ? 1 : -1);
                            const auto context = QString("%1 ppi, %2 px, %3, dark=%4")
                                                         .arg(ppi)
                                                         .arg(pixels)
                                                         .arg(QString::fromLatin1(indent))
                                                         .arg(dark);
                            QMap<QString, qint64> sizes;
                            kobalt::EngineDocument direct(kobalt::test::readerFonts());
                            auto directSource = resources(indent, false, sizes);
                            require(direct.openResources(directSource, sizes, size, settings,
                                                         &error),
                                    qPrintable(error));
                            require(offset(direct) == expected,
                                    qPrintable("Authored indent changed: " + context));
                            kobalt::EngineDocument inherited(kobalt::test::readerFonts());
                            auto source = resources(indent, true, sizes);
                            require(inherited.openResources(source, sizes, size, settings, &error),
                                    qPrintable(error));
                            require(offset(inherited) == expected,
                                    qPrintable("Inherited indent changed: " + context));
                            const auto image = inherited.image(0);
                            kobalt::EngineDocument preview(kobalt::test::readerFonts());
                            require(preview.openPreview(inherited, size, &error),
                                    qPrintable(error));
                            require(offset(preview) == expected && preview.image(0) == image,
                                    qPrintable("Preview changed indentation: " + context));
                            const auto saved = inherited.state();
                            require(inherited.openResources(source, sizes, size, saved, &error),
                                    qPrintable(error));
                            require(offset(inherited) == expected && inherited.image(0) == image,
                                    qPrintable("Reopening changed indentation: " + context));
                            if (ppi == 300 && !dark && QByteArray(unit) == "1.5em") {
                                QTemporaryDir cache(QDir::currentPath()
                                                    + "/indentation-cache-XXXXXX");
                                require(cache.isValid(), "Cannot create the indentation cache");
                                kobalt::EngineDocument cached(kobalt::test::readerFonts());
                                // CREngine does not cache books reported as smaller than 30 kB.
                                cached.setDocumentCache(cache.path(), QByteArray(64, 'a'),
                                                        "indentation-v1", 1024 * 1024);
                                require(cached.openResources(source, sizes, size, settings, &error),
                                        qPrintable(error));
                                require(cached.saveDocumentCache(),
                                        "Cannot save the indentation cache");
                                kobalt::EngineDocument restored(kobalt::test::readerFonts());
                                restored.setDocumentCache(cache.path(), QByteArray(64, 'a'),
                                                          "indentation-v1", 1024 * 1024);
                                require(restored.openResources(source, sizes, size, settings,
                                                               &error),
                                        qPrintable(error));
                                require(restored.openedFromCache() && offset(restored) == expected
                                                && restored.image(0) == image,
                                        qPrintable("Cached styles changed indentation: "
                                                   + context));
                            }
                            const int otherPixels = pixels == 52 ? 54 : 52;
                            require(inherited.applyAppearance(
                                            kobalt::qt::object({ { "font_pixels", otherPixels } })),
                                    "Cannot reflow the indentation fixture");
                            require(offset(inherited)
                                            == otherPixels * 3 / 2 * (negative == hanging ? 1 : -1),
                                    qPrintable("Reflow changed indentation: " + context));
                        }
                    }
                }
            }
        }
    }
    kobalt::readingPpi.store(originalPpi);
}
