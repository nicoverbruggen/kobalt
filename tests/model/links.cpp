#include "reader_inputs.h"
#include "engine/blaze/adapter/column_view.h"
#include "book_fixtures.h"
#include <QTemporaryDir>
#include "support/qt_compat.h"
#include "engine/blaze/adapter/engine_document.h"
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QMap>
#include <cstdio>
#include "check.h"

namespace kobalt {
struct ReaderFootnoteProbe
{
    static ColumnView &view(EngineDocument &document) { return *document.engine_; }
};
} // namespace kobalt

namespace {
QImage decoratedLink(const QByteArray &markup, const QByteArray &css, bool dark)
{
    const QMap<QString, QByteArray> files{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Link decoration</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "c.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>p{margin:0;text-indent:0}" + css
                  + "</style></head><body><p>" + markup + "</p></body></html>" }
    };
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
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    QString error;
    require(model.openResources(
                    resources, sizes, QSize(800, 600),
                    kobalt::qt::object({ { "font_pixels", 40 }, { "dark_mode", dark } }), &error),
            qPrintable(error));
    require(model.pageLinks().size() == 1, "Decorated link lost its hit region");
    const auto image = model.image(0);
    kobalt::EngineDocument preview(kobalt::test::readerFonts());
    require(preview.openPreview(model, QSize(800, 600), &error), qPrintable(error));
    require(preview.image(0) == image, "Link decoration changed in the appearance preview");
    return image;
}

void inpageMarkup()
{
    using namespace kobalt;

    struct Case
    {
        QByteArray body;
        bool note;
    };

    const QList<Case> cases{
        { "<aside id='note' epub:type='footnote'><p>Explicit note.</p></aside>", true },
        { "<section id='note' epub:type='note'><p>Explicit note.</p></section>", true },
        { "<div id='note' epub:type='endnote'><p>Explicit note.</p></div>", true },
        { "<div id='note' epub:type='rearnote'><p>Explicit note.</p></div>", true },
        { "<aside id='note' epub:type='annotation footnote'><p>Tokenized note.</p></aside>", true },
        { "<aside id='note' role='doc-footnote'><p>Explicit note.</p></aside>", true },
        { "<aside id='note' role='doc-endnote'><p>Explicit note.</p></aside>", true },
        { "<aside id='note' role='doc-note'><p>Explicit note.</p></aside>", true },
        { "<aside id='note' role='doc-rearnote'><p>Explicit note.</p></aside>", true },
        { "<div class='_idFootnotes'><div class='_idFootnote'><p id='note'>InDesign note.</p></div><p>Group text.</p></div>",
          true },
        { "<div class='footnotes'><div class='footnote'><p><a class='label' id='note' href='#ref'>[1]</a> Labeled note.</p></div></div>",
          true },
        { "<p id='note' data-kobalt-page-note='start'>A source-supplied marker.</p>", false },
        { "<div class='footnotes'><div class='footnote'><p id='note'>An unlinked note.</p></div></div>",
          false },
        { "<p id='note'>An ordinary numeric destination.</p>", false },
        { "<section id='note' epub:type='footnotes'><p>A whole note group.</p></section>", false },
        { "<a id='note' epub:type='note' href='#ref'>Backlink.</a>", false },
        { "<div class='_idFootnotes'><a id='note' class='_idFootnote' href='#ref'>Backlink.</a></div>",
          false },
        { "<aside id='note' epub:type='footnote' style='display:none'><p>Hidden note.</p></aside>",
          false }
    };
    for (const auto &test : cases) {
        auto files = test::linkBook();
        files["OEBPS/first.xhtml"] =
                "<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'><body>"
                "<p id='ref'>Read the reference <a href='#note'>1</a>.</p>"
                + test.body + "</body></html>";
        QMap<QString, qint64> sizes;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            sizes[it.key()] = it.value().size();
        auto resources = std::make_shared<ResourceCache>(
                files.keys(),
                [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                    const auto bytes = files.value(name);
                    return sink(bytes.constData(), bytes.size());
                },
                std::make_shared<std::atomic_bool>(false));
        EngineDocument document(test::readerFonts());
        QString error;
        require(document.openResources(resources, sizes, QSize(800, 900),
                                       { { "inpage_footnotes", true } }, &error),
                qPrintable(error));
        require((ReaderFootnoteProbe::view(document)
                         .getPageList()
                         ->operator[](0)
                         ->footnotes.length()
                 > 0) == test.note,
                test.body.constData());
    }
}

// Compare actual pixels so publisher boxes cannot leak through the footer clip.
void inpageStyling()
{
    using namespace kobalt;
    for (bool dark : { false, true }) {
        const auto render = [dark](const QByteArray &css, bool enabled, bool markup) {
            auto files = test::linkBook();
            const QByteArray content = markup
                    ? "A <em>formatted</em> note with <strong>emphasis</strong> and x<sup>2</sup>."
                    : "A formatted note with emphasis and x2.";
            files["OEBPS/first.xhtml"] = "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
                    + css
                    + "</style></head><body><p id='ref'>Text <a href='#note'>1</a> and <a href='#second'>2</a>.</p>"
                      "<div class='footnotes'><h3>FOOTNOTES:</h3><div class='footnote'>"
                      "<p><a class='label' id='note' href='#ref'>[1]</a> "
                    + content
                    + "</p><p>Continuation paragraph.</p>"
                      "<p><a class='label' id='second' href='#ref'>[2]</a> Another note.</p>"
                      "</div></div></body></html>";
            QMap<QString, qint64> sizes;
            for (auto it = files.cbegin(); it != files.cend(); ++it)
                sizes[it.key()] = it.value().size();
            auto resources = std::make_shared<ResourceCache>(
                    files.keys(),
                    [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                        const auto bytes = files.value(name);
                        return sink(bytes.constData(), bytes.size());
                    },
                    std::make_shared<std::atomic_bool>(false));
            EngineDocument document(test::readerFonts());
            QString error;
            require(document.openResources(
                            resources, sizes, QSize(800, 900),
                            qt::object({ { "inpage_footnotes", enabled }, { "dark_mode", dark } }),
                            &error),
                    qPrintable(error));
            if (enabled)
                require(ReaderFootnoteProbe::view(document)
                                        .getPageList()
                                        ->operator[](0)
                                        ->footnotes.length()
                                == 2,
                        "Labeled Gutenberg notes were merged or their continuation was split");
            return document.image(0);
        };
        const QByteArray publisher =
                ".footnotes,.footnote,.footnote p,.footnote a,.footnote em{"
                "margin:27px!important;padding:13px!important;border:3px dashed!important;"
                "background-color:#888!important;text-indent:3em!important}";
        const auto clean = render({}, true, true);
        require(render(publisher, true, true) == clean,
                "Publisher boxes or spacing override Kobalt page-note styling");
        require(render(publisher, false, true) != render({}, false, true),
                "Disabled page notes discarded publisher styling");
        require(render({}, true, false) != clean,
                "Page-note styling discarded inline content markup");
    }
}

// Exercise the engine's actual note fragments, not just a changed stylesheet.
void inpageFootnotes()
{
    using namespace kobalt;
    auto files = test::linkBook();
    files["OEBPS/first.xhtml"].replace(
            "</body>",
            "<p id='legacy-ref'>Another reference <a href='#legacy-note'>7</a>.</p>"
            "<div class='footnotes'><div class='footnote'><p>"
            "<a id='legacy-note' class='label' href='#legacy-ref'>[7]</a> Cached labeled note."
            "</p><p>Its continuation.</p></div></div></body>");
    // Keep this book small, but long enough to write a real parsed-book cache.
    files["OEBPS/Notes/foot notes.xhtml"].replace(QByteArray("Long note text. ").repeated(10000),
                                                  QByteArray("Hidden text."));
    files["OEBPS/Notes/foot notes.xhtml"].replace(
            "Second paragraph of the same note.",
            "Second paragraph of the same note. <a href='../first.xhtml#short-ref'>Return</a>");
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto resources = std::make_shared<ResourceCache>(
            files.keys(),
            [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    const QSize viewport(800, 900);
    QString error;
    QTemporaryDir cache;
    require(cache.isValid(), "Cannot create note cache directory");
    const auto open = [&](EngineDocument &document, const QJsonObject &settings, bool cached) {
        if (cached)
            document.setDocumentCache(cache.path() + "/.cache", QByteArray(64, 'f'),
                                      "inpage-notes-test", 1024 * 1024);
        require(document.openResources(resources, sizes, viewport, settings, &error),
                qPrintable(error));
    };
    bool cacheWritten = false;
    for (int columns : { 1, 2 }) {
        for (bool dark : { false, true }) {
            auto settings = qt::object(
                    { { "font_pixels", 32 }, { "dark_mode", dark }, { "columns", columns } });
            EngineDocument document(test::readerFonts());
            open(document, settings, false);
            const auto original = document.image(0);
            const int originalPages = document.pages();
            auto &view = ReaderFootnoteProbe::view(document);
            require(view.getPageList()->operator[](0)->footnotes.length() == 0,
                    "Default layout enabled page notes");
            const auto origin = document.position();
            require(document.applyAppearance(appearance::Patch({ { "inpage_footnotes", true } })),
                    "Cannot enable page notes");
            require(document.position() == origin, "Page notes lost the source reading position");
            require(document.image(0) != original, "Page notes did not change the rendered page");
            const auto &first = *view.getPageList()->operator[](0);
            require(first.footnotes.length() >= 1, "Semantic EPUB notes were not paginated");
            int fragments = 0, notePages = 0;
            for (int page = 0; page < view.getPageList()->length(); ++page) {
                const auto &info = *view.getPageList()->operator[](page);
                int noteHeight = 0;
                for (int i = 0; i < info.footnotes.length(); ++i) {
                    require(info.footnotes[i].height > 0, "Empty note fragment");
                    noteHeight += info.footnotes[i].height;
                    ++fragments;
                }
                if (noteHeight) {
                    ++notePages;
                    require(info.height + noteHeight < viewport.height(),
                            "Overflowing note overlaps the main text or page boundary");
                }
                require(!document.image(page / columns).isNull(), "Cannot draw note continuation");
            }
            require(fragments > 2 && notePages > 1, "Long note did not continue across pages");
            require(document.go(0), "Cannot return to note reference");
            int noteTop =
                    viewport.height() - document.state().value("bottom_margin_pixels").toInt();
            for (int i = 0; i < first.footnotes.length(); ++i)
                noteTop -= first.footnotes[i].height;
            bool popup = false, backlink = false;
            QString noteTarget;
            for (const auto &link : document.pageLinks()) {
                const auto detail = document.linkDetails(link.source);
                if (detail.kind == LinkKind::Footnote && detail.html.contains("short")) {
                    popup = true;
                    noteTarget = detail.target;
                }
                if (detail.label == "Return" && detail.kind == LinkKind::Passage) {
                    backlink = link.rectangles.first().top() >= noteTop;
                    const auto word = document.wordAt(link.rectangles.first().center());
                    require(word.text == "Return",
                            "Displayed note text cannot be selected for Follow link");
                    require(document.rangeVisible(word.start, word.end),
                            "Displayed note selection is not considered visible");
                    require(!document.rangeRects(word.start, word.end).isEmpty(),
                            "Displayed note selection has no rectangles");
                    const auto geometry = document.annotationGeometry(word.start, word.end);
                    require(!geometry.rectangles.isEmpty() && geometry.start.visible
                                    && geometry.end.visible,
                            "Displayed note selection lost its rectangles or handles");
                    const QVector<QPair<QString, QString>> ranges{ { word.start, word.end } };
                    require(document.visibleRanges(ranges) == QVector<int>{ 0 },
                            "Displayed note highlight is filtered out");
                    const auto highlights = document.pageRangeRects(0, ranges);
                    require(highlights.size() == 1 && highlights.first() == geometry.rectangles,
                            "Saved note highlight cannot be redrawn");
                }
            }
            require(popup, "In-page notes disabled the existing popup");
            require(backlink, "Displayed note backlink has no usable hit region");
            require(document.goTo(noteTarget), "Popup Show in book cannot open the note");
            auto targetPoint =
                    view.getDocument()
                            ->createXPointer(Utf8ToUnicode(noteTarget.toUtf8().constData()))
                            .toPoint();
            require(view.docToWindowPoint(targetPoint),
                    "Popup Show in book opens an invisible note");
            require(document.goTo(origin), "Cannot return from the note to its reference");
            // Reflow and viewport layout reuse must use the new note policy.
            document.resize(QSize(1100, 700));
            document.resize(viewport);
            EngineDocument fresh(test::readerFonts());
            settings["inpage_footnotes"] = true;
            open(fresh, settings, false);
            require(document.pages() == fresh.pages() && document.image(0) == fresh.image(0),
                    "Reflow retained stale page-note layout");
            require(document.applyAppearance(appearance::Patch({ { "inpage_footnotes", false } })),
                    "Cannot disable page notes");
            require(document.pages() == originalPages && document.image(0) == original,
                    "Disabling page notes did not restore ordinary pagination and pixels");
            // A parsed cache saved with either choice must reopen with the other
            // choice without reparsing, and match a fresh layout at every page.
            for (bool enabled : { false, true, false }) {
                settings["inpage_footnotes"] = enabled;
                EngineDocument cached(test::readerFonts()), uncached(test::readerFonts());
                open(cached, settings, true);
                require(cached.openedFromCache() == cacheWritten,
                        "Page-note setting changed the parsed-cache identity");
                open(uncached, settings, false);
                require(cached.pages() == uncached.pages(), "Cached note pagination is stale");
                for (int page = 0; page < cached.pages(); ++page)
                    require(cached.image(page) == uncached.image(page),
                            "Cached note pixels differ from a fresh parse");
                if (!cacheWritten) {
                    // Simulate a parsed cache saved before optional note markers.
                    auto *dom = ReaderFootnoteProbe::view(cached).getDocument();
                    dom->getProps()->setInt("kobalt.page-notes.version", 0);
                    const auto marker = dom->getAttrNameIndex("data-kobalt-page-note");
                    QVector<ldomNode *> pending{ dom->getRootNode() };
                    while (!pending.isEmpty()) {
                        auto *node = pending.takeLast();
                        if (!node->isElement())
                            continue;
                        if (node->hasAttribute(marker))
                            node->setAttributeValue(0, marker, U"");
                        for (int i = 0; i < node->getChildCount(); ++i)
                            pending.append(node->getChildNode(i));
                    }
                    dom->setCacheFileStale(true);
                }
                require(cached.saveDocumentCache(), "Cannot save page-note cache");
                cacheWritten = true;
            }
        }
    }
}

void linkDecoration()
{
    for (const bool dark : { false, true }) {
        const auto render = [dark](const QByteArray &markup, const QByteArray &css) {
            return decoratedLink(markup, css, dark);
        };
        const QByteArray wrapped =
                "<a href='https://example.org'><se-span class='kobo-link'><span class='koboSpan'>Sample link</span></se-span></a>";
        const QByteArray border =
                "se-span.kobo-link{border-bottom:1px dotted currentColor!important}";
        const auto expected = render(wrapped, border + "a{ text-decoration:none }");
        const auto actual = render(wrapped, border);
        require(actual == expected,
                "Default underline duplicates the publisher's Kobo link border");
        const QByteArray ordinary = "<a href='https://example.org'>Sample link</a>";
        require(render(ordinary, {}) != render(ordinary, "a{text-decoration:none}"),
                "Ordinary links lost their default underline");
        require(render(wrapped, {})
                        != render(wrapped, "a{text-decoration:none}se-span.kobo-link{border:none}"),
                "Unstyled Kobo link wrapper lacks a visible decoration");
        const QByteArray custom =
                "se-span.kobo-link{border-bottom:3px dashed black!important;text-decoration:overline!important}";
        require(render(wrapped, custom) == render(wrapped, custom + "a{text-decoration:none}"),
                "Kobo compatibility styling changed publisher decoration");
        require(render(wrapped, border + "a{text-decoration:overline}") != expected,
                "Kobo compatibility styling overrode a publisher anchor decoration");
        const QByteArray mixed =
                "<a href='https://example.org'>Prefix <se-span class='kobo-link'>Sample link</se-span></a>";
        require(render(mixed, border) != render(mixed, border + "a{text-decoration:none}"),
                "Partially wrapped link lost the underline on its ordinary text");
    }
}

struct NoteCase
{
    const char *name;
    QByteArray reference, target;
    kobalt::LinkKind kind = kobalt::LinkKind::Footnote;
    QByteArray css = {}, toc = {}, before = {};
    QString included = {}, excluded = {};
};

kobalt::LinkDetails inspectNote(const NoteCase &test, bool dark)
{
    static const QByteArray pixel = [] {
        QImage image(12, 12, QImage::Format_RGB32);
        image.fill(Qt::black);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
                "Cannot encode reference image");
        return bytes;
    }();
    const QMap<QString, QByteArray> files{
        { "mimetype", "application/epub+zip" },
        { "pixel.png", pixel },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata><title>Note matching</title></metadata><manifest><item id='c' href='c.xhtml' media-type='application/xhtml+xml'/><item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/><item id='pixel' href='pixel.png' media-type='image/png'/></manifest><spine><itemref idref='c'/></spine></package>" },
        { "nav.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'><body><nav epub:type='toc'><ol><li><a href='c.xhtml'>Chapter</a></li>"
                  + test.toc + "</ol></nav></body></html>" },
        { "c.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops' xmlns:ops='http://www.idpf.org/2007/ops'><head><style>p{margin:0;text-indent:0}"
                  + test.css + "</style></head><body>" + test.before + test.reference + test.target
                  + "</body></html>" }
    };
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        sizes[it.key()] = it.value().size();
    }
    auto resources = std::make_shared<kobalt::ResourceCache>(
            files.keys(),
            [files](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    QString error;
    require(model.openResources(
                    resources, sizes, QSize(1072, 1300),
                    kobalt::qt::object({ { "font_pixels", 24 }, { "dark_mode", dark } }), &error),
            qPrintable(QString(test.name) + ": " + error));
    const auto origin = model.position();
    const auto links = model.pageLinks();
    require(links.size() == 1, qPrintable(QString(test.name) + ": expected one reference"));
    const auto details = model.linkDetails(links.first().source);
    require(links.first().kind == details.kind,
            "Frame link classification differs from popup routing");
    require(!links.first().tappable("off") && !links.first().tappable("invalid"),
            "Disabled links became tappable");
    require(links.first().tappable("all"), "All-links mode omitted a visible link");
    require(links.first().tappable("footnotes") == (test.kind == kobalt::LinkKind::Footnote),
            "Footnote-only mode disagrees with the existing semantic detection");
    require(model.position() == origin, "Note matching changed the reading position");
    require(details.kind == test.kind,
            qPrintable(QString("%1: expected kind %2, got %3")
                               .arg(test.name)
                               .arg(int(test.kind))
                               .arg(int(details.kind))));
    if (test.kind == kobalt::LinkKind::Footnote) {
        require(details.error.isEmpty() && !details.html.isEmpty(),
                qPrintable(QString(test.name) + ": no preview"));
    }
    if (!test.included.isEmpty()) {
        require(details.html.contains(test.included),
                qPrintable(QString(test.name) + ": missing note continuation"));
    }
    if (!test.excluded.isEmpty()) {
        require(!details.html.contains(test.excluded),
                qPrintable(QString(test.name) + ": preview crossed its boundary"));
    }
    return details;
}

void noteMatching()
{
    using kobalt::LinkKind;
    const auto reference = [](const QByteArray &label, const QByteArray &attributes = {}) {
        return "<p>A passage with a reference <a " + attributes + " href='#note'>" + label
                + "</a>.</p>";
    };
    const QByteArray note = "<p id='note'>The note text.</p>";
    const QVector<NoteCase> cases{
        { "numeric reference", reference("[1]"), note },
        { "three-digit reference", reference("999"), note },
        { "letter reference", reference("a"), note },
        { "letters and digits", reference("Ab12"), note },
        { "roman reference", reference("MCMXIV"), note },
        { "unicode numeric reference", reference("&#x661;"), note },
        { "four-digit reference", reference("2026"), note, LinkKind::Passage },
        { "long word", reference("notes"), note, LinkKind::Passage },
        { "punctuation only", reference("*"), note, LinkKind::Passage },
        { "superscript parent", "<p>A passage <sup><a href='#note'>annotation</a></sup>.</p>",
          note },
        { "subscript grandparent",
          "<p>A passage <sub><span><a href='#note'>annotation</a></span></sub>.</p>", note },
        { "superscript child", reference("<sup>annotation</sup>"), note },
        { "superscript image",
          reference("<sup><img src='pixel.png' width='12' height='12'/></sup>"), note },
        { "ordinary image", reference("<img src='pixel.png' width='12' height='12'/>"), note,
          LinkKind::Passage },
        { "vertical alignment", reference("annotation", "style='vertical-align:super'"), note },
        { "mixed vertical alignment", reference("<sup>annotation</sup> text"), note,
          LinkKind::Passage },
        { "paragraph link", "<p><a href='#note'>1</a></p>", note, LinkKind::Passage },
        { "heading target", reference("1"), "<h2 id='note'>Chapter two</h2>", LinkKind::Passage },
        { "target inside heading", reference("1"), "<h2><span id='note'>Chapter two</span></h2>",
          LinkKind::Passage },
        { "target contains heading", reference("1"),
          "<div id='note'><h2>Chapter two</h2><p>Text.</p></div>", LinkKind::Passage },
        { "toc target",
          reference("1"),
          note,
          LinkKind::Passage,
          {},
          "<li><a href='c.xhtml#note'>A nested chapter</a></li>" },
        { "nested toc target",
          reference("1"),
          note,
          LinkKind::Passage,
          {},
          "<li><a href='c.xhtml'>Part</a><ol><li><a href='c.xhtml#note'>Chapter</a></li></ol></li>" },
        { "file link", "<p>A passage <a href='c.xhtml'>1</a>.</p>", note, LinkKind::Passage },
        { "backlink", reference("1"), {}, LinkKind::Passage, {}, {}, note },
        { "explicit backward note",
          reference("annotation", "epub:type='noteref'"),
          {},
          LinkKind::Footnote,
          {},
          {},
          note },
        { "generic note", reference("annotation"),
          "<aside id='note' epub:type='note'><p>The note text.</p></aside>" },
        { "rear note", reference("annotation"),
          "<aside id='note' epub:type='rearnote'><p>The note text.</p></aside>" },
        { "endnote", reference("annotation"),
          "<aside id='note' epub:type='endnote'><p>The note text.</p></aside>" },
        { "generic note role", reference("annotation"),
          "<aside id='note' role='doc-note'><p>The note text.</p></aside>" },
        { "rear note role", reference("annotation"),
          "<aside id='note' role='doc-rearnote'><p>The note text.</p></aside>" },
        { "endnote role", reference("annotation"),
          "<aside id='note' role='doc-endnote'><p>The note text.</p></aside>" },
        { "alternate namespace", reference("annotation"),
          "<aside id='note' ops:type='sidebar ENDNOTE'><p>The note text.</p></aside>" },
        { "ancestor container",
          reference("annotation"),
          "<aside epub:type='endnote'><p id='note'>The note text.</p><p>Continuation.</p></aside>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "Continuation." },
        { "explicit passage", reference("1", "epub:type='link'"),
          "<aside id='note' epub:type='footnote'><p>Text.</p></aside>", LinkKind::Passage },
        { "explicit passage role", reference("1", "role='doc-link'"), note, LinkKind::Passage },
        { "source css hint", reference("annotation", "style='-cr-hint:noteref'"), note },
        { "source css exclusion",
          reference("1", "epub:type='noteref' style='-cr-hint:noteref-ignore'"), note,
          LinkKind::Passage },
        { "target css hint", reference("annotation"),
          "<div id='note' style='-cr-hint:footnote'><p>Text.</p></div>" },
        { "target css exclusion", reference("1"),
          "<p id='note' epub:type='footnote' style='-cr-hint:footnote-ignore'>Text.</p>",
          LinkKind::Passage },
        { "source hint precedence", reference("annotation", "style='-cr-hint:noteref'"),
          "<p id='note' style='-cr-hint:footnote-ignore'>Text.</p>" },
        { "long inferred note", reference("1"), "<p id='note'>" + QByteArray(10001, 'x') + "</p>",
          LinkKind::Passage },
        { "inferred text limit", reference("1"),
          "<p id='note'>" + QByteArray(10000, 'x') + "</p>" },
        { "whitespace target", reference("1"),
          "<p id='note'>&#160;</p><p id='next'>Another note.</p>", LinkKind::Passage },
        { "inferred node limit", reference("1"),
          "<p id='note'>" + QByteArray("<span>x</span>").repeated(4096) + "</p>",
          LinkKind::Passage },
        { "long explicit note", reference("annotation", "epub:type='noteref'"),
          "<p id='note'>" + QByteArray(10001, 'x') + "</p>" },
        { "inferred continuation",
          reference("1"),
          note + "<div><p>Continuation.</p></div><p id='next'>Another note.</p>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "Continuation.",
          "Another note." },
        { "wrapped note boundary",
          reference("1"),
          note + "<div><p>Continuation.</p><p id='next'>Another note.</p></div>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "Continuation.",
          "Another note." },
        { "legacy name boundary",
          reference("1"),
          note + "<p>Continuation.</p><p><a name='next'></a>Another note.</p>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "Continuation.",
          "Another note." },
        { "page break before",
          reference("1"),
          note + "<p style='page-break-before:always'>Another note.</p>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "The note text.",
          "Another note." },
        { "page break after",
          reference("1"),
          "<p id='note' style='page-break-after:always'>The note text.</p><p>Another note.</p>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "The note text.",
          "Another note." },
        { "wrapper page break after",
          reference("1"),
          note + "<div style='page-break-after:always'><p>Continuation.</p></div><p>Another note.</p>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "Continuation.",
          "Another note." },
        { "empty anchor before note",
          reference("1"),
          "<a id='note'></a><p>The note text.</p><p id='next'>Another note.</p>",
          LinkKind::Footnote,
          {},
          {},
          {},
          "The note text.",
          "Another note." },
    };
    for (const bool dark : { false, true }) {
        for (const auto &test : cases) {
            inspectNote(test, dark);
        }
    }
}
} // namespace

// Checks link classification and note detection: explicit and inferred notes, passage overrides and missing targets.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 2, "Pass the synthetic links EPUB");
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "Cannot open link fixture");
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    QString error;
    require(model.open(file.readAll(), QSize(1072, 1300),
                       kobalt::qt::object({ { "font_pixels", 32 } }), &error),
            qPrintable(error));
    linkDecoration();
    inpageMarkup();
    inpageStyling();
    inpageFootnotes();
    noteMatching();
    const auto origin = model.position();
    for (const bool dark : { false, true }) {
        model.setDarkMode(dark);
        require(model.image(0).text("kobalt.animation_text_only") == "true",
                "Default link colors do not follow the monochrome page palette");
    }
    model.setDarkMode(false);
    QMap<QString, kobalt::LinkDetails> details;
    QMap<QString, QString> sources;
    const auto links = model.pageLinks();
    for (const auto &link : links) {
        require(!link.source.isEmpty() && !link.rectangles.isEmpty(),
                "Visible link lacks a source or hit region");
        for (const auto &rect : link.rectangles)
            require(QRect(0, 0, 1072, 1300).contains(rect), "Link hit region escaped the viewport");
        const auto value = model.linkDetails(link.source);
        details[value.label] = value;
        sources[value.label] = link.source;
    }
    require(details.size() == 10,
            qPrintable(QString("Expected 10 visible links, found: %1")
                               .arg(QStringList(details.keys()).join(", "))));
    using kobalt::LinkKind;
    require(details["short note"].kind == LinkKind::Footnote
                    && !details["short note"].target.isEmpty(),
            "EPUB note link did not resolve");
    const auto html = details["short note"].html;
    require(html.contains("<strong>short</strong>") && html.contains("café &amp; tea")
                    && html.contains("Second paragraph"),
            "Note lost structure, Unicode or escaped text");
    require(!html.contains("Note paragraph") && !html.contains("href="),
            "Note preview included unrelated content or active links");
    require(details["long note"].kind == LinkKind::Footnote
                    && details["long note"].html.contains("End of the long note."),
            "Long note was silently truncated");
    require(details["later passage"].kind == LinkKind::Passage
                    && details["nearby passage"].kind == LinkKind::Passage,
            "Internal passage links were misclassified");
    require(details["short note"].chapter == "Notes"
                    && details["later passage"].chapter == "Passages"
                    && details["nearby passage"].chapter == "References",
            "Link targets do not name their chapter");
    require(details["missing passage"].kind == LinkKind::Unavailable
                    && details["missing passage"].target.isEmpty(),
            "Missing target appeared navigable");
    require(details["website"].kind == LinkKind::Website
                    && details["website"].target == "https://example.org/book",
            "Website URL was changed or followed");
    require(details["unsupported link"].kind == LinkKind::Unavailable,
            "Unsupported link scheme became navigable");
    require(details["legacy note"].html.contains("A legacy note")
                    && !details["legacy note"].html.contains("hidden note"),
            "Legacy note escaped its paragraph");
    require(!details["oversized note"].error.isEmpty() && details["oversized note"].html.isEmpty(),
            "Oversized note bypassed the preview bound");
    require(details["hidden note"].kind == LinkKind::Footnote
                    && details["hidden note"].html.contains("A hidden note"),
            "Hidden note could not be previewed");
    require(model.position() == origin && model.page() == 0, "Inspecting links moved the reader");
    require(model.style(48, 140), "Cannot reflow the links fixture");
    require(model.linkDetails(sources["short note"]).target == details["short note"].target,
            "Reflow changed the note's source target");
    require(model.goTo(details["later passage"].target), "Cannot navigate to a resolved passage");
    require(model.image(model.page()).text("kobalt.animation_text_only") == "false",
            "Default link styling discarded the publisher link color");
    require(model.goTo(origin) && model.position() == origin,
            "Cannot return to the exact original source anchor");
    require(model.linkDetails("/missing/node").kind == LinkKind::None,
            "Invalid source produced a link");
    puts("PASS: page link regions, internal targets, note boundaries, preview limits, hidden notes and exact return after reflow");
}
