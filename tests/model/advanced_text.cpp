#include "engine/crengine/engine_document.h"
#include "check.h"
#include "reader_inputs.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <array>

namespace {
void open(kobalt::EngineDocument &model, const QByteArray &body, const QByteArray &css = {},
          QJsonObject settings = {}, QSize size = { 1072, 1448 })
{
    const QMap<QString, QByteArray> data{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Advanced text</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "c.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}" + css
                  + "</style></head><body>" + body + "</body></html>" }
    };
    QMap<QString, qint64> sizes;
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto source = std::make_shared<kobalt::ResourceCache>(
            data.keys(),
            [data](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = data.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    settings["font_pixels"] = 40;
    settings["line_spacing"] = 100;
    if (!settings.contains("margin_pixels"))
        settings["margin_pixels"] = 0;
    settings["top_margin_pixels"] = 0;
    settings["bottom_margin_pixels"] = 0;
    QString error;
    require(model.openResources(source, sizes, size, settings, &error), qPrintable(error));
}

std::array<int, 4> paragraphPages(kobalt::EngineDocument &model)
{
    std::array<int, 4> pages;
    int index = 0;
    for (const auto *marker : { "Firstmarker", "Secondmarker", "Thirdmarker", "Fourthmarker" }) {
        bool found = false;
        for (int page = 0; page < model.pages(); ++page) {
            const auto hits = model.searchPage(marker, page).hits;
            if (hits.isEmpty())
                continue;
            require(model.goTo(hits.first().start), "Cannot navigate to paragraph line");
            pages[index++] = model.page();
            found = true;
            break;
        }
        require(found, "Pagination lost a paragraph line");
    }
    return pages;
}
} // namespace

// Checks justification tracking, word spacing, widow and orphan control and hanging punctuation, with previews and cached reopening.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    using kobalt::EngineDocument;
    const QByteArray prose = "<p>"
            + QByteArray("Office affinity café amber birch cedar dogwood hazel oak. ").repeated(20)
            + "</p>";
    EngineDocument model(kobalt::test::readerFonts());
    open(model, prose, {}, { { "text_alignment", "justify" } });
    const auto original = model.image(0);
    const auto anchor = model.position();
    const int pages = model.pages();
    require(model.state().value("justification_tracking").toInt(-1) == 0
                    && model.state().value("widows_orphans").toString() == "publisher",
            "Advanced text defaults changed");
    for (int percent : { 5, 15 }) {
        require(model.applyAppearance({ { "justification_tracking", percent } }),
                "Cannot apply tracking");
        require(model.image(0) != original, "Tracking did not change loose justified lines");
        require(model.pages() == pages && model.position() == anchor,
                "Tracking changed line fitting or position");
        EngineDocument preview(kobalt::test::readerFonts());
        QString error;
        require(preview.openPreview(model, { 1072, 1448 }, &error), qPrintable(error));
        require(preview.state().value("justification_tracking").toInt() == percent
                        && preview.image(0) == model.image(0),
                "Preview lost tracking");
        EngineDocument reopened(kobalt::test::readerFonts());
        open(reopened, prose, {}, model.state());
        require(reopened.image(0) == model.image(0), "Reopening lost tracking");
    }
    require(model.applyAppearance({ { "justification_tracking", 0 } })
                    && model.image(0) == original,
            "Turning tracking off did not restore pixels");
    require(!model.applyAppearance({ { "justification_tracking", -1 } })
                    && !model.applyAppearance({ { "justification_tracking", 21 } })
                    && !model.applyAppearance({ { "widows_orphans", "invalid" } }),
            "Invalid advanced text was accepted");
    for (const auto *alignment : { "left", "right", "center" }) {
        EngineDocument aligned(kobalt::test::readerFonts());
        open(aligned, prose, {}, { { "text_alignment", alignment } });
        const auto before = aligned.image(0);
        require(aligned.applyAppearance({ { "justification_tracking", 15 } })
                        && aligned.image(0) == before,
                "Tracking changed non-justified text");
    }
    for (const QByteArray &text : { QByteArray("日本語の文章を読む楽しみ。漢字と仮名。"),
                                    QByteArray("القراءة تمنح الإنسان معرفة وحكمة ") }) {
        EngineDocument shaped(kobalt::test::readerFonts());
        open(shaped, "<p>" + text.repeated(15) + "</p>", {}, { { "text_alignment", "justify" } });
        const auto before = shaped.image(0);
        require(shaped.applyAppearance({ { "justification_tracking", 15 } })
                        && shaped.image(0) == before,
                "Tracking changed CJK glyphs or cursive joining");
    }
    const QByteArray spacingProse = "<p>"
            + QByteArray(
                      "A small boat lay by the shore while we walked slowly over the sand and watched birds rise above the water. ")
                      .repeated(25)
            + "</p>";
    EngineDocument spaced(kobalt::test::readerFonts());
    open(spaced, spacingProse, {}, { { "text_alignment", "left" } }, { 530, 320 });
    const auto normalSpaces = spaced.image(0);
    require(spaced.state().value("word_spacing").toInt() == 100, "Word spacing default changed");
    QStringList narrowStarts;
    for (int percent : { 80, 120 }) {
        require(spaced.applyAppearance({ { "word_spacing", percent } }),
                "Cannot apply word spacing");
        require(spaced.image(0) != normalSpaces, "Word spacing did not change rendered text");
        QStringList starts;
        for (int page = 0; page < spaced.pages(); ++page)
            starts.append(spaced.pageStart(page));
        if (percent == 80)
            narrowStarts = starts;
        else
            require(starts != narrowStarts, "Word spacing did not reflow the book");
        EngineDocument preview(kobalt::test::readerFonts());
        QString error;
        require(preview.openPreview(spaced, { 530, 320 }, &error), qPrintable(error));
        require(preview.state().value("word_spacing").toInt() == percent
                        && preview.image(0) == spaced.image(0),
                "Preview lost word spacing");
        EngineDocument reopened(kobalt::test::readerFonts());
        open(reopened, spacingProse, {}, spaced.state(), { 530, 320 });
        require(reopened.image(0) == spaced.image(0), "Reopening lost word spacing");
    }
    require(spaced.applyAppearance({ { "word_spacing", 100 } }) && spaced.image(0) == normalSpaces,
            "Normal word spacing did not restore pixels");
    require(!spaced.applyAppearance({ { "word_spacing", 0 } })
                    && !spaced.applyAppearance({ { "word_spacing", 101 } }),
            "Invalid word spacing was accepted");
    EngineDocument invalidSpacing(kobalt::test::readerFonts());
    open(invalidSpacing, spacingProse, {},
         { { "word_spacing", 500 }, { "text_alignment", "left" } }, { 530, 320 });
    require(invalidSpacing.state().value("word_spacing").toInt() == 100
                    && invalidSpacing.image(0) == normalSpaces,
            "Invalid saved word spacing did not fall back to Normal");
    bool orphan = false, widow = false;
    const QByteArray paragraph =
            "<p id='target' style='orphans:1!important;widows:1!important'>"
            "Firstmarker<br/>Secondmarker<br/>Thirdmarker<br/>Fourthmarker</p>";
    for (int count = 3; count < 10 && (!orphan || !widow); ++count) {
        const auto body =
                "<p>" + QByteArray("Filler<br/>").repeated(count - 1) + "Filler</p>" + paragraph;
        EngineDocument pagination(kobalt::test::readerFonts());
        open(pagination, body, {}, {}, { 1072, 320 });
        const auto allowed = paragraphPages(pagination);
        const bool hasOrphan = allowed[0] != allowed[1];
        const bool hasWidow = allowed[2] != allowed[3] && allowed[1] == allowed[2];
        if (!hasOrphan && !hasWidow)
            continue;
        orphan |= hasOrphan;
        widow |= hasWidow;
        require(pagination.applyAppearance({ { "widows_orphans", "avoid" } }),
                "Cannot avoid widows and orphans");
        const auto avoided = paragraphPages(pagination);
        require(avoided[0] == avoided[1] && avoided[2] == avoided[3],
                "Avoid both failed to override inline publisher important rules");
        EngineDocument preview(kobalt::test::readerFonts());
        QString error;
        require(preview.openPreview(pagination, { 1072, 320 }, &error), qPrintable(error));
        require(paragraphPages(preview) == avoided, "Preview lost widow and orphan avoidance");
        EngineDocument reopened(kobalt::test::readerFonts());
        open(reopened, body, {}, pagination.state(), { 1072, 320 });
        require(paragraphPages(reopened) == avoided, "Reopening lost widow and orphan avoidance");
        require(pagination.applyAppearance({ { "widows_orphans", "publisher" } })
                        && paragraphPages(pagination) == allowed,
                "Publisher did not restore authored pagination");
        EngineDocument publisher(kobalt::test::readerFonts());
        auto protectedBody = body;
        protectedBody.replace("orphans:1!important;widows:1!important",
                              "orphans:2!important;widows:2!important");
        open(publisher, protectedBody, {}, {}, { 1072, 320 });
        require(paragraphPages(publisher) == avoided, "Publisher mode ignored authored avoidance");
        require(publisher.applyAppearance({ { "widows_orphans", "allow" } })
                        && paragraphPages(publisher) == allowed,
                "Allow both failed to override publisher avoidance");
        EngineDocument inherited(kobalt::test::readerFonts());
        auto inheritedBody = body;
        inheritedBody.replace(" style='orphans:1!important;widows:1!important'", "");
        open(inherited, inheritedBody, "body{orphans:2;widows:2}", {}, { 1072, 320 });
        require(paragraphPages(inherited) == avoided, "Publisher mode lost inherited avoidance");
    }
    require(orphan && widow, "Fixture did not exercise both page-boundary cases");
    // Hanging punctuation needs real paper beside both one- and two-column text.
    // Exercise the public appearance path, including reflow, previews and cached reopening.
    for (int columns : { 1, 2 })
        for (const auto *alignment : { "justify", "right", "left", "center" })
            for (const auto *kind : { "soft-hyphens", "automatic-hyphens", "punctuation" }) {
                const QByteArray text = QString(kind) == "soft-hyphens"
                        ? "extra&#173;or&#173;di&#173;na&#173;ry re&#173;mark&#173;a&#173;ble ty&#173;pog&#173;ra&#173;phy "
                        : QString(kind) == "automatic-hyphens"
                        ? "extraordinary remarkable typography "
                        : "&quot;Reading,&quot; she said, &quot;is a pleasure!&quot; (Really?) Yes; indeed. ";
                const QByteArray body = "<p lang='en'>" + text.repeated(35) + "</p>";
                QJsonObject settings{ { "columns", columns },
                                      { "column_spacing", 80 },
                                      { "margin_pixels", 40 },
                                      { "text_alignment", alignment },
                                      { "hyphenation", "on" } };
                EngineDocument hanging(kobalt::test::readerFonts());
                open(hanging, body, {}, settings, { 800, 600 });
                const auto before = hanging.image(0);
                const auto anchor = hanging.position();
                const auto rightInk = [](const QImage &image, int start, int end) {
                    int ink = 0;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = start; x < end; ++x)
                            ink += qGray(image.pixel(x, y)) < 128;
                    return ink;
                };
                require(!hanging.state().value("hanging_punctuation").toBool(),
                        "Hanging punctuation default changed");
                require(hanging.applyAppearance({ { "hanging_punctuation", true } }),
                        "Cannot enable hanging punctuation");
                const auto enabled = hanging.image(0);
                if (QString(alignment) == "justify" || QString(alignment) == "right") {
                    require(enabled != before
                                    && rightInk(enabled, 760, 800) > rightInk(before, 760, 800),
                            "Hyphens did not extend into the outer margin");
                    if (columns == 2)
                        require(rightInk(enabled, 360, 400) > rightInk(before, 360, 400),
                                "Hyphens did not extend into the column gutter");
                } else if (QString(kind) != "punctuation" || QString(alignment) == "center") {
                    require(enabled == before,
                            "Hanging hyphens changed left-aligned or centered text");
                }
                require(rightInk(enabled, 799, 800) == 0,
                        "Hanging punctuation reached the screen edge");
                EngineDocument preview(kobalt::test::readerFonts());
                QString error;
                require(preview.openPreview(hanging, { 800, 600 }, &error), qPrintable(error));
                require(preview.image(0) == enabled,
                        "Preview lost hanging punctuation or column geometry");
                EngineDocument reopened(kobalt::test::readerFonts());
                open(reopened, body, {}, hanging.state(), { 800, 600 });
                require(reopened.image(0) == enabled, "Reopening lost hanging punctuation");
                require(hanging.applyAppearance({ { "hanging_punctuation", false } })
                                && hanging.image(0) == before && hanging.position() == anchor,
                        "Disabling hanging punctuation did not restore pixels and position");
                if (QString(alignment) == "justify") {
                    QTemporaryDir directory;
                    require(directory.isValid(), "Cannot create hanging punctuation cache");
                    {
                        EngineDocument cached(kobalt::test::readerFonts());
                        cached.setDocumentCache(directory.path(), "hanging", "hanging-test", 4096);
                        open(cached, body, {}, settings, { 800, 600 });
                        require(cached.saveDocumentCache(),
                                "Cannot save hanging punctuation cache");
                    }
                    EngineDocument restored(kobalt::test::readerFonts());
                    restored.setDocumentCache(directory.path(), "hanging", "hanging-test", 4096);
                    settings["hanging_punctuation"] = true;
                    open(restored, body, {}, settings, { 800, 600 });
                    require(restored.openedFromCache() && restored.image(0) == enabled,
                            "Cached reopening retained disabled hanging punctuation");
                    require(restored.saveDocumentCache(),
                            "Cannot cache enabled hanging punctuation");
                    EngineDocument disabled(kobalt::test::readerFonts());
                    disabled.setDocumentCache(directory.path(), "hanging", "hanging-test", 4096);
                    settings["hanging_punctuation"] = false;
                    open(disabled, body, {}, settings, { 800, 600 });
                    require(disabled.openedFromCache() && disabled.image(0) == before,
                            "Cached reopening retained enabled hanging punctuation");
                }
            }
    QTemporaryDir cache;
    require(cache.isValid(), "Cannot create advanced text cache");
    {
        EngineDocument cached(kobalt::test::readerFonts());
        cached.setDocumentCache(cache.path(), "advanced-text", "advanced-text-test", 4096);
        open(cached, prose, {}, { { "text_alignment", "justify" } });
        require(cached.saveDocumentCache(), "Cannot save advanced text cache");
    }
    EngineDocument restored(kobalt::test::readerFonts());
    restored.setDocumentCache(cache.path(), "advanced-text", "advanced-text-test", 4096);
    open(restored, prose, {},
         { { "text_alignment", "justify" }, { "justification_tracking", 15 } });
    require(restored.openedFromCache() && restored.image(0) != original,
            "Document cache retained disabled tracking");
    EngineDocument cachedSpaces(kobalt::test::readerFonts());
    cachedSpaces.setDocumentCache(cache.path(), "advanced-text", "advanced-text-test", 4096);
    open(cachedSpaces, prose, {}, { { "text_alignment", "justify" }, { "word_spacing", 120 } });
    EngineDocument freshSpaces(kobalt::test::readerFonts());
    open(freshSpaces, prose, {}, { { "text_alignment", "justify" }, { "word_spacing", 120 } });
    require(cachedSpaces.openedFromCache() && cachedSpaces.image(0) == freshSpaces.image(0)
                    && cachedSpaces.image(0) != original,
            "Document cache retained Normal word spacing");
    std::puts(
            "PASS: adaptive tracking, publisher overrides, pagination, previews and cached reopening");
}
