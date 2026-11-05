#include "engine/crengine/engine_document.h"
#include "check.h"
#include "reader_inputs.h"
#include <QDir>
#include <QGuiApplication>
#include <QTemporaryDir>

namespace {
std::shared_ptr<kobalt::ResourceCache> resources(const QByteArray &body,
                                                 QMap<QString, qint64> &sizes)
{
    const QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Paragraph gaps</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "c.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>body{background:white}p{margin:2em 0!important;padding:.25em 0;text-indent:1em}</style></head><body>"
                  + body + "</body></html>" }
    };
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        sizes[it.key()] = it.value().size();
    return std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
}

QRect word(kobalt::EngineDocument &model, const QString &text)
{
    const auto hits = model.searchPage(text, 0).hits;
    require(hits.size() == 1, "Paragraph fixture word is missing or duplicated");
    QRect result;
    for (const auto &rect : model.rangeRects(hits.first().start, hits.first().end))
        result = result.united(rect);
    require(!result.isEmpty(), "Paragraph word has no rendered bounds");
    return result;
}
} // namespace

// Checks paragraph gap and first-line indent overrides, Publisher restoration, special blocks and cached reopening.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QSize size(1072, 1800);
    QString error;
    const int originalPpi = kobalt::readingPpi.load();
    for (int ppi : { 212, 300 })
        for (bool dark : { false, true }) {
            kobalt::readingPpi.store(ppi);
            const QJsonObject settings{ { "font_pixels", 40 },
                                        { "line_spacing", 150 },
                                        { "line_spacing_version", 1 },
                                        { "text_alignment", "publisher" },
                                        { "dark_mode", dark } };
            QMap<QString, qint64> sizes;
            // Publisher exports can leave a solid border style on a zero-width frame.
            // Its paragraphs must still support the same adjustments and warm caches.
            const auto source = resources(
                    "<h2>Chapter heading</h2><div style='border-width:0;border-style:solid'>"
                    "<p>Firstmarker ordinary prose.</p><p>Secondmarker ordinary prose.</p></div>",
                    sizes);
            QTemporaryDir cache(QDir::currentPath() + "/paragraph-cache-XXXXXX");
            require(cache.isValid(), "Cannot create paragraph cache");
            kobalt::EngineDocument model(kobalt::test::readerFonts());
            model.setDocumentCache(cache.path(), QByteArray(64, 'a'), "paragraph-gap", 1024 * 1024);
            require(model.openResources(source, sizes, size, settings, &error), qPrintable(error));
            const auto publisher = model.image(0);
            require(model.applyAppearance({ { "line_spacing", 200 } })
                            && model.image(0) != publisher,
                    "Line spacing did not change prose inside a zero-width frame");
            require(model.applyAppearance({ { "line_spacing", 150 } })
                            && model.image(0) == publisher,
                    "Restoring line spacing changed the original frame");
            require(model.state().value("paragraph_spacing").toInt() == -1,
                    "Older book did not use Publisher spacing");
            const auto anchor = model.position();
            require(model.applyAppearance({ { "paragraph_spacing", 0 } }),
                    "Cannot remove paragraph gaps");
            const auto compact = model.image(0);
            const auto first = word(model, "Firstmarker"), second = word(model, "Secondmarker");
            require(compact != publisher, "Removing publisher gaps did not change layout");
            for (int percent : { 25, 50, 100, 200 }) {
                require(model.applyAppearance({ { "paragraph_spacing", percent } }),
                        "Cannot set a custom paragraph gap");
                require(word(model, "Secondmarker").top() - word(model, "Firstmarker").top()
                                == second.top() - first.top() + 40 * percent / 100,
                        "Paragraph gap does not follow the text size");
                require(word(model, "Firstmarker").left() == first.left(),
                        "Paragraph gap changed indentation");
                require(model.position() == anchor, "Paragraph gap lost the reading anchor");
                kobalt::EngineDocument preview(kobalt::test::readerFonts());
                require(preview.openPreview(model, size, &error), qPrintable(error));
                require(preview.image(0) == model.image(0),
                        "Preview paragraph gap differs from the book");
            }
            require(model.saveDocumentCache(), "Cannot cache paragraph markers");
            kobalt::EngineDocument restored(kobalt::test::readerFonts());
            restored.setDocumentCache(cache.path(), QByteArray(64, 'a'), "paragraph-gap",
                                      1024 * 1024);
            require(restored.openResources(source, sizes, size, model.state(), &error),
                    qPrintable(error));
            require(restored.openedFromCache() && restored.image(0) == model.image(0),
                    "Cached paragraph layout changed");
            require(restored.applyAppearance({ { "paragraph_spacing", 0 } })
                            && restored.image(0) == compact,
                    "Cached book lost its paragraph markers");
            require(model.applyAppearance({ { "paragraph_spacing", -1 } })
                            && model.image(0) == publisher,
                    "Publisher did not restore authored spacing");
            for (const auto &invalid : { QJsonValue(-2), QJsonValue(201), QJsonValue(true),
                                         QJsonValue("50"), QJsonValue(1.5) })
                require(!model.applyAppearance({ { "paragraph_spacing", invalid } })
                                && model.image(0) == publisher,
                        "Invalid paragraph spacing changed the book");
            require(model.applyAppearance({ { "paragraph_indent", 0 } }),
                    "Cannot remove paragraph indentation");
            const int unindented = word(model, "Firstmarker").left();
            for (int percent : { 50, 100, 200, 300 }) {
                require(model.applyAppearance({ { "paragraph_indent", percent } }),
                        "Cannot set paragraph indentation");
                require(word(model, "Firstmarker").left() - unindented == 40 * percent / 100,
                        "Paragraph indentation does not follow text size");
                require(model.state().value("paragraph_spacing").toInt() == -1,
                        "Indentation changed the independent spacing choice");
                require(model.position() == anchor, "Indentation lost the reading anchor");
                kobalt::EngineDocument preview(kobalt::test::readerFonts());
                require(preview.openPreview(model, size, &error), qPrintable(error));
                require(preview.image(0) == model.image(0),
                        "Preview changed paragraph indentation");
            }
            require(model.saveDocumentCache(), "Cannot cache paragraph indentation");
            kobalt::EngineDocument indented(kobalt::test::readerFonts());
            indented.setDocumentCache(cache.path(), QByteArray(64, 'a'), "paragraph-gap",
                                      1024 * 1024);
            require(indented.openResources(source, sizes, size, model.state(), &error),
                    qPrintable(error));
            require(indented.openedFromCache() && indented.image(0) == model.image(0),
                    "Cached reopening lost paragraph indentation");
            require(model.applyAppearance({ { "paragraph_indent", -1 } })
                            && model.image(0) == publisher,
                    "Publisher did not restore original indentation");
            for (const auto &invalid : { QJsonValue(-2), QJsonValue(301), QJsonValue(true),
                                         QJsonValue("100"), QJsonValue(1.5) })
                require(!model.applyAppearance({ { "paragraph_indent", invalid } })
                                && model.image(0) == publisher,
                        "Invalid indentation changed the book");

            // Each example contains only protected content, so every pixel must stay.
            for (const auto &body :
                 { QByteArray("<h2>Heading</h2><p class='chapter-title'>Paragraph heading</p>"),
                   QByteArray(
                           "<p role='heading'>Accessible heading</p><p style='text-align:center'>Centered text</p>"),
                   QByteArray(
                           "<blockquote><p>Quoted text</p></blockquote><ul><li><p>List text</p></li></ul>"),
                   QByteArray("<div class='poem'><p>Verse line</p><p>Another line</p></div>"),
                   QByteArray(
                           "<p style='white-space:pre-wrap'>Preformatted text</p><p style='border:1px solid black'>Boxed text</p>"),
                   QByteArray(
                           "<figure><p>Figure caption</p></figure><table><tr><td><p>Table text</p></td></tr></table>") }) {
                const auto special = resources(body, sizes);
                kobalt::EngineDocument protectedBook(kobalt::test::readerFonts());
                require(protectedBook.openResources(special, sizes, size, settings, &error),
                        qPrintable(error));
                const auto before = protectedBook.image(0);
                require(protectedBook.applyAppearance(
                                { { "paragraph_spacing", 0 }, { "paragraph_indent", 200 } })
                                && protectedBook.image(0) == before,
                        "Paragraph adjustments changed a heading or special block");
            }
            for (const auto *side : { "top", "right", "bottom", "left" }) {
                const auto special = resources(
                        "<div style='border-width:0;border-style:solid;border-" + QByteArray(side)
                                + "-width:1px'><p>Firstmarker ordinary prose.</p><p>Secondmarker ordinary prose.</p></div>",
                        sizes);
                kobalt::EngineDocument framed(kobalt::test::readerFonts());
                require(framed.openResources(special, sizes, size, settings, &error),
                        qPrintable(error));
                const auto before = framed.image(0);
                require(framed.applyAppearance(
                                { { "paragraph_spacing", 0 }, { "paragraph_indent", 200 } })
                                && framed.image(0) == before,
                        "Paragraph adjustments changed text inside a visible border");
            }
        }
    kobalt::readingPpi.store(originalPpi);
}
