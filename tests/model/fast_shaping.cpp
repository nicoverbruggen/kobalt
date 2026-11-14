#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <koblaze.h>

using kobalt::EngineDocument;

namespace {
const QSize viewport(800, 1136);

void open(EngineDocument &model, int characters, const QString &face)
{
    const QByteArray prose("The reader crossed the quiet room and opened the book. ");
    QByteArray body;
    int remaining = characters;
    while (remaining > 0) {
        const auto text = prose.left(qMin(remaining, prose.size()));
        body += "<p>" + text + "</p>";
        remaining -= text.size();
    }
    auto files = kobalt::test::engineBook(false, 1, 1);
    files["OEBPS/c1.xhtml"] = kobalt::test::publisherHtml(body);
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto resources = std::make_shared<kobalt::ResourceCache>(
            files.keys(),
            [files](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    QString error;
    require(model.openResources(resources, sizes, viewport, { { "font_face", face } }, &error),
            qPrintable(error));
}

void checkChoice(EngineDocument &model, bool expected)
{
    require(model.resourceStats().value("fast_shaping").toBool() == expected,
            "Document selected the wrong shaping path");
    model.releaseImages();
    require(!model.image(0).isNull(), "Cannot draw the shaping fixture");
    require(koblazeWordWidths() == expected && koblazePairKerning() == expected
                    && koblazeWordPairs() == expected && koblazePlainJoiner() == expected,
            "Another document changed the active shaping path");
}

void compare(EngineDocument &automatic, EngineDocument &reference)
{
    require(automatic.pages() == reference.pages(), "Conditional shaping changed page count");
    for (int page : { 0, automatic.pages() / 2, automatic.pages() - 1 }) {
        require(automatic.pageStart(page) == reference.pageStart(page),
                "Conditional shaping changed page boundaries");
        require(automatic.image(page) == reference.image(page),
                "Conditional shaping changed page pixels");
    }
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    for (const auto &face : { QStringLiteral("Libron"), kobalt::test::alternateReadingFace(),
                              kobalt::test::headingReadingFace() }) {
        for (int characters : { 8000, 249999, 250000 }) {
            const bool fast = characters >= 250000;
            qunsetenv("KOBALT_FAST_SHAPING");
            EngineDocument automatic(kobalt::test::readerFonts());
            open(automatic, characters, face);
            checkChoice(automatic, fast);
            qputenv("KOBALT_FAST_SHAPING", fast ? "1" : "0");
            EngineDocument reference(kobalt::test::readerFonts());
            open(reference, characters, face);
            qunsetenv("KOBALT_FAST_SHAPING");
            compare(automatic, reference);
            if (fast) {
                qputenv("KOBALT_FAST_SHAPING", "0");
                EngineDocument normal(kobalt::test::readerFonts());
                open(normal, characters, face);
                qunsetenv("KOBALT_FAST_SHAPING");
                compare(automatic, normal);
            }
            for (auto *model : { &automatic, &reference })
                require(model->applyAppearance(
                                kobalt::appearance::Patch(QJsonObject{ { "font_pixels", 52 } }),
                                viewport),
                        "Cannot reflow shaping fixture");
            checkChoice(automatic, fast);
            compare(automatic, reference);
        }
    }
    qunsetenv("KOBALT_FAST_SHAPING");
    EngineDocument large(kobalt::test::readerFonts()), small(kobalt::test::readerFonts());
    open(large, 300000, "Libron");
    open(small, 8000, "Libron");
    checkChoice(large, true);
    checkChoice(small, false);
    checkChoice(large, true);
    large.setChapterSlices(2);
    QString error;
    require(large.showSpineItems(0, 0, &error), qPrintable(error));
    checkChoice(large, false);
    QTemporaryDir cache;
    require(cache.isValid(), "Cannot prepare shaping cache");
    const auto prepare = [&](EngineDocument &model) {
        model.setDocumentCache(cache.path(), QByteArray(64, 'a'), "fast-shaping", 1024 * 1024);
        open(model, 300000, "Libron");
    };
    {
        EngineDocument cached(kobalt::test::readerFonts());
        prepare(cached);
        require(cached.saveDocumentCache(), "Cannot save shaping cache");
    }
    {
        EngineDocument restored(kobalt::test::readerFonts());
        prepare(restored);
        require(restored.openedFromCache(), "Shaping count discarded the parsed cache");
        require(restored.resourceStats().value("shaping_text_characters").toInt() == 250000,
                "Cached shaping count was lost");
        checkChoice(restored, true);
    }
    return 0;
}
