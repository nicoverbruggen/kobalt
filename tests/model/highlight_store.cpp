#include "storage/highlight_store.h"
#include "storage/bookmark_store.h"
#include "storage/annotation_export.h"
#include "publication/zip_resource.h"
#include "check.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <utime.h>

using namespace kobalt;

namespace {
QByteArray bytes(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read fixture");
    return file.readAll();
}

QByteArray member(QByteArray archive, const QString &name)
{
    QBuffer source(&archive);
    require(source.open(QIODevice::ReadOnly), "Cannot open ZIP buffer");
    ZipResource resource;
    require(resource.open(source, name), "Cannot read ZIP member");
    QByteArray content;
    while (!resource.finished() && resource.valid())
        content += resource.next();
    require(resource.valid() && resource.finished(), "ZIP member failed CRC or bounds checks");
    return content;
}

void replace(const QString &path, const QByteArray &data)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    && file.write(data) == data.size(),
            "Cannot write fixture");
}
} // namespace

// Checks the highlight store and ZIP export: Markdown and HTML entries, notes, the embedded font and destinations.
int main()
{
    // These cases obstruct legacy JSON files; SQLite behavior has its own database regressions.
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    QTemporaryDir root;
    require(root.isValid(), "No temporary folder");
    const QByteArray fingerprint(64, 'a');
    HighlightStore store(root.path(), "file:///book.epub", fingerprint, "Book");
    require(store.load(), "Cannot load empty highlights");
    const Highlight h{ "first",        "/body/p[1].0",
                       "/body/p[9].8", "Complete passage across pages",
                       "Chapter",      QDateTime::currentDateTimeUtc() };
    auto overlapping = h;
    overlapping.id = "second";
    store.setHighlights({ h, overlapping });
    require(store.save(), "Cannot save overlapping passages");
    const auto path = store.folder() + "/highlights.json";
    const auto saved = bytes(path);
    HighlightStore reopened(root.path(), "file:///book.epub", fingerprint, "Book");
    require(reopened.load() && reopened.highlights().size() == 2
                    && reopened.highlights()[0].text == h.text
                    && reopened.highlights()[0].start == h.start
                    && reopened.highlights()[1].id == "second",
            "Passages did not round-trip independently");
    auto noted = h;
    noted.note = QString::fromUtf8("First thought\nSecond thought: café 日本語");
    auto exported = noted;
    exported.text =
            QString::fromUtf8("<script>alert('x')</script> & **quoted** café 日本語\nNext line");
    const auto exportRoot = root.path() + "/exports";
    const auto exportedPath = exportAnnotations(exportRoot, "one", "../Book: title", "Author",
                                                { exported }, AnnotationExportFormat::Markdown);
    require(!exportedPath.isEmpty() && QFileInfo(exportedPath).absolutePath() == exportRoot
                    && exportedPath.endsWith(".zip"),
            "Unsafe export filename");
    const auto archive = bytes(exportedPath);
    const auto markdown = member(archive, "annotations.md");
    const auto htmlArchive = annotationExportArchive("../Book: title", "Author", { exported },
                                                     AnnotationExportFormat::Html);
    const auto html = member(htmlArchive, "annotations.html");
    require(archive.mid(archive.size() - 14, 4) == QByteArray::fromHex("01000100")
                    && htmlArchive.mid(htmlArchive.size() - 14, 4)
                            == QByteArray::fromHex("01000100"),
            "A selected-format ZIP must contain exactly one file");
    require(archive.startsWith("PK") && html.contains("&lt;script&gt;")
                    && !html.contains("<script>alert") && html.contains(">#1</a>")
                    && html.contains("sort-order") && html.contains("data-local-time"),
            "HTML export lost labels, controls or escaping");
    require(html.contains("id=\"show-markup\" checked") && html.contains("Show annotation styling"),
            "HTML has no checked styling toggle");
    const auto chosenPath = root.path() + "/chosen name.zip";
    require(writeAnnotationExport(QUrl::fromLocalFile(chosenPath), archive)
                    && bytes(chosenPath) == archive,
            "Export to a chosen location changed the ZIP");
    require(!writeAnnotationExport(QUrl::fromLocalFile(chosenPath), {})
                    && bytes(chosenPath) == archive,
            "An empty export replaced the chosen file");
    require(!writeAnnotationExport(QUrl("https://example.com/export.zip"), archive)
                    && !writeAnnotationExport(
                            QUrl::fromLocalFile(root.path() + "/missing/fail.zip"), archive),
            "Unsupported or unavailable export destination succeeded");
    require(markdown.contains("&lt;script&gt;") && !markdown.contains("<script>")
                    && markdown.contains("\\*\\*quoted\\*\\*")
                    && markdown.contains(noted.note.toUtf8()),
            "Markdown export lost text, notes or escaping");
    require(exportAnnotations(exportRoot, "two", "../Book: title", "Author", { exported },
                              AnnotationExportFormat::Markdown)
                    != exportedPath,
            "Different books share an export filename");
    exported.detailsLoaded = false;
    require(exportAnnotations(exportRoot, "one", "../Book: title", "Author", { exported },
                              AnnotationExportFormat::Markdown)
                            .isEmpty()
                    && bytes(exportedPath) == archive,
            "Incomplete payload replaced an export");
    exported.detailsLoaded = true;
    exported.note = "Updated note";
    require(exportAnnotations(exportRoot, "one", "../Book: title", "Author", { exported },
                              AnnotationExportFormat::Markdown)
                            == exportedPath
                    && member(bytes(exportedPath), "annotations.md").contains("Updated note"),
            "Repeat export did not replace the previous export");
    const auto instant = QDateTime::fromString("2026-10-25T00:30:07.123+02:00", Qt::ISODateWithMs);
    QVector<Highlight> scale;
    for (int i = 0; i < 500; ++i) {
        auto row = noted;
        row.id = QString::number(i + 1);
        row.chapter = i < 250 ? "Repeated chapter title"
                : i < 400     ? "Other chapter"
                              : "Repeated chapter title";
        row.text = i % 2
                ? QString::fromUtf8("Natásha considers the journey.\n\nA second paragraph.")
                : "Pierre speaks with the prince.";
        row.note = i % 4 == 0 ? "Note-only search marker.\nAnother thought." : QString();
        row.color = QStringList{ "gray", "yellow", "green", "blue", "pink" }[i % 5];
        row.style = QStringList{ "shaded", "underline", "dotted" }[(i / 5) % 3];
        // Out-of-order creation times, with ties, exercise global date sorting.
        row.created = instant.addSecs((i % 17) * 71);
        scale.append(row);
    }
    QDir project(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath());
    require(project.cd("../.."), "Cannot locate checked-in export font");
    const auto font = bytes(project.filePath("assets/fonts/ebook-core/Libron-Regular.ttf"));
    const auto large = annotationExportArchive("<Book & title>", "Author", scale,
                                               AnnotationExportFormat::Html, instant, font);
    const auto largeHtml = member(large, "annotations.html");
    const auto largeMarkdown = annotationExportArchive("<Book & title>", "Author", scale,
                                                       AnnotationExportFormat::Markdown, instant);
    const auto largeMd = member(largeMarkdown, "annotations.md");
    require(largeHtml.count("<article ") == 500 && largeMd.count("### Annotation ") == 500
                    && largeHtml.count("<section class=\"chapter\"") == 3
                    && largeHtml.count("<aside class=\"note\"") == 125
                    && largeHtml.contains("&lt;Book &amp; title&gt;")
                    && largeHtml.contains("2026-10-24T22:30:07.123Z")
                    && largeMd.contains("24 October 2026 at 22:30:07 UTC")
                    && !largeHtml.contains("Page "),
            "Large export lost records, chapters, timestamps or escaped metadata");
    require(largeHtml.contains("data:font/ttf;base64," + font.toBase64())
                    && largeHtml.contains(
                            "<!-- Libron is licensed under the SIL Open Font License (OFL) 1.1.")
                    && !largeHtml.contains("font-license")
                    && !largeHtml.contains("PERMISSION &amp; CONDITIONS")
                    && !largeHtml.contains(">Annotation 001</a>"),
            "HTML lost its embedded font, license or compact number labels");
    for (const auto &style : { "shaded", "underline", "dotted" })
        for (const auto &color : { "gray", "yellow", "green", "blue", "pink" })
            require(largeHtml.contains(QByteArray("data-style=\"") + style + "\" data-color=\""
                                       + color + '"'),
                    "HTML lost a saved color or mark style");
    require(largeHtml.contains("--mark:#e8df9b;--mark-ink:#e8df9b")
                    && largeHtml.contains("--mark:#d7d7d7;--mark-ink:#000000"),
            "HTML colors differ from the reader palette");
    auto unknown = noted;
    unknown.color = "futurecolor";
    unknown.style = "futurestyle";
    const auto fallback =
            member(annotationExportArchive("Book", "", { unknown }, AnnotationExportFormat::Html),
                   "annotations.html");
    require(fallback.contains("data-style=\"shaded\" data-color=\"gray\"")
                    && !fallback.contains("data:font/ttf;base64,"),
            "Unknown styles or absent fonts have no safe fallback");
    require(member(annotationExportArchive("<Book & title>", "Author", scale,
                                           AnnotationExportFormat::Markdown, instant, font),
                   "annotations.md")
                    == largeMd,
            "HTML font embedding changed the Markdown document");
    require(annotationExportArchive("Book", "", {}, AnnotationExportFormat::Html).isEmpty()
                    && annotationExportArchive("Book", "", { noted, noted },
                                               AnnotationExportFormat::Html)
                               .isEmpty(),
            "Empty or duplicate annotation payload was exported");
    require(annotationExportFileName("one", "Book", AnnotationExportFormat::Html)
                    != annotationExportFileName("one", "Book", AnnotationExportFormat::Markdown),
            "HTML and Markdown exports share a destination");
    const auto example = qEnvironmentVariable("KOBALT_TEST_EXPORT_ARCHIVE");
    if (!example.isEmpty())
        replace(example, large);
    const auto blockedExport = root.path() + "/blocked-export";
    replace(blockedExport, "block");
    require(exportAnnotations(blockedExport, "one", "Book", "Author", { exported },
                              AnnotationExportFormat::Markdown)
                    .isEmpty(),
            "Failed export reported success");
    store.setHighlights({ noted });
    require(store.save() && reopened.load() && reopened.highlights().first().note == noted.note,
            "Annotation note did not round-trip");
    noted.note = QString(HighlightStore::maxNoteUnits, 'x');
    store.setHighlights({ noted });
    require(store.save() && reopened.load() && reopened.highlights().first().note == noted.note,
            "A note at the storage limit was rejected");
    const auto notedBytes = bytes(path);
    noted.note = QString(HighlightStore::maxNoteUnits + 1, 'x');
    store.setHighlights({ noted });
    require(!store.save() && bytes(path) == notedBytes,
            "Oversized note replaced saved annotations");
    for (const auto &badNote :
         { QJsonValue(123), QJsonValue(QJsonObject()), QJsonValue(QJsonValue::Null),
           QJsonValue(QString(HighlightStore::maxNoteUnits + 1, 'x')) }) {
        auto object = QJsonDocument::fromJson(notedBytes).object();
        auto list = object.value("highlights").toArray();
        auto entry = list.first().toObject();
        entry["note"] = badNote;
        list[0] = entry;
        object["highlights"] = list;
        const auto damaged = QJsonDocument(object).toJson();
        replace(path, damaged);
        HighlightStore invalid(root.path(), "file:///book.epub", fingerprint, "Book");
        require(!invalid.load() && !invalid.save() && bytes(path) == damaged,
                "Malformed note was accepted or overwritten");
    }
    replace(path, saved);
    require(reopened.load() && reopened.highlights().first().note.isEmpty(),
            "Legacy annotation without a note was not empty");
    // Older version-one files have no color field.
    auto legacy = QJsonDocument::fromJson(saved).object();
    auto entries = legacy.value("highlights").toArray();
    for (int i = 0; i < entries.size(); ++i) {
        auto entry = entries[i].toObject();
        entry.remove("color");
        entry.remove("style");
        entries[i] = entry;
    }
    legacy["highlights"] = entries;
    replace(path, QJsonDocument(legacy).toJson());
    require(reopened.load() && reopened.highlights().first().color == "gray",
            "Legacy passage is not gray");
    require(reopened.highlights().first().style == "shaded", "Legacy passage is not shaded");
    for (const auto &color : { "gray", "yellow", "green", "blue", "pink", "orange" }) {
        auto colored = h;
        colored.color = QString::fromLatin1(color);
        store.setHighlights({ colored });
        require(store.save() && reopened.load()
                        && reopened.highlights().first().color == colored.color,
                "Stored color did not round-trip");
        require(highlightDisplayColor(colored.color)
                        == (colored.color == "orange" ? "gray" : colored.color),
                "Unknown future color did not display gray");
    }
    for (const auto &style : { "shaded", "underline", "dotted", "future" }) {
        auto styled = h;
        styled.style = style;
        store.setHighlights({ styled });
        require(store.save() && reopened.load() && reopened.highlights().first().style == style,
                "Stored style did not round-trip");
        require(highlightDisplayStyle(style)
                        == (QString(style) == "future" ? "shaded" : QString(style)),
                "Future style did not fall back to shaded");
    }
    auto invalidStyle = h;
    invalidStyle.style = "INVALID!";
    store.setHighlights({ invalidStyle });
    const auto beforeInvalidStyle = bytes(path);
    require(!store.save() && bytes(path) == beforeInvalidStyle,
            "Invalid style replaced saved data");
    auto invalidColor = h;
    invalidColor.color = "INVALID!";
    store.setHighlights({ invalidColor });
    const auto beforeInvalidColor = bytes(path);
    require(!store.save() && bytes(path) == beforeInvalidColor,
            "Invalid color replaced saved data");
    auto invalidObject = QJsonDocument::fromJson(beforeInvalidColor).object();
    auto invalidEntries = invalidObject.value("highlights").toArray();
    auto invalidEntry = invalidEntries.first().toObject();
    invalidEntry["color"] = 123;
    invalidEntries[0] = invalidEntry;
    invalidObject["highlights"] = invalidEntries;
    const auto invalidBytes = QJsonDocument(invalidObject).toJson();
    replace(path, invalidBytes);
    HighlightStore invalidStored(root.path(), "file:///book.epub", fingerprint, "Book");
    require(!invalidStored.load() && !invalidStored.save() && bytes(path) == invalidBytes,
            "Malformed color was overwritten");
    replace(path, saved);
    auto automatic = h;
    automatic.chosenEnd = "/body/p[9].7";
    automatic.sentenceEndPolicy = true;
    automatic.chosenStart = "/body/p[1].1";
    automatic.sentenceStartPolicy = true;
    store.setHighlights({ automatic });
    require(store.save() && reopened.load() && reopened.highlights().first().sentenceEndPolicy
                    && reopened.highlights().first().chosenEnd == automatic.chosenEnd
                    && reopened.highlights().first().sentenceStartPolicy
                    && reopened.highlights().first().chosenStart == automatic.chosenStart,
            "Automatic endpoint provenance did not survive save and reopen");
    require(reopened.highlights().first().end == h.end,
            "Provenance changed effective saved endpoint");
    store.setHighlights({ h, overlapping });
    require(store.save() && reopened.load() && !reopened.highlights().first().sentenceEndPolicy
                    && reopened.highlights().first().chosenEnd.isEmpty()
                    && !reopened.highlights().first().sentenceStartPolicy
                    && reopened.highlights().first().chosenStart.isEmpty(),
            "Legacy/manual exact endpoints acquired automatic policy");
    BookmarkStore bookmarks(root.path(), "file:///book.epub", fingerprint, "Book");
    require(bookmarks.load(), "Cannot load bookmarks beside highlights");
    bookmarks.setBookmarks({ { h.start, "Chapter", "Excerpt", h.created } });
    require(bookmarks.save() && bytes(path) == saved, "Bookmark save changed highlights");
    HighlightStore other(root.path(), "different-server-book", {}, "Other");
    require(other.load() && other.highlights().isEmpty(), "Highlights leaked across books");
    HighlightStore edited(root.path(), "file:///book.epub", QByteArray(64, 'b'), "Book");
    require(edited.load() && edited.highlights().size() == 2 && edited.save(),
            "Edited sideload lost passages");
    auto bad = h;
    bad.text = QString(HighlightStore::maxTextUnits + 1, 'x');
    store.setHighlights({ bad });
    require(!store.save() && bytes(path) == saved, "Oversized passage replaced saved data");
    store.setHighlights({ h, h });
    require(!store.save() && bytes(path) == saved, "Duplicate IDs replaced saved data");
    for (const auto &damaged : { QByteArray("{"), QByteArray("{\"version\":2,\"highlights\":[]}"),
                                 QByteArray("{\"version\":1,\"highlights\":[{}]}"),
                                 QByteArray("{\"version\":1,\"highlights\":{}}") }) {
        replace(path, damaged);
        HighlightStore corrupt(root.path(), "file:///book.epub", fingerprint, "Book");
        require(!corrupt.load(), "Damaged highlights accepted");
        corrupt.setHighlights({ h });
        require(!corrupt.save() && bytes(path) == damaged,
                "Damaged bytes overwritten after failed load");
    }
    replace(path, saved);
    // Books without highlight files must not parse the rest of the library's metadata.
    QTemporaryDir library;
    require(library.isValid(), "Cannot create adoption library");
    for (int i = 0; i < 300; ++i) {
        const auto folder = library.path() + "/file-"
                + QString::fromLatin1(QByteArray::number(i, 16).rightJustified(64, '0'));
        require(QDir().mkpath(folder), "Cannot create unhighlighted folder");
        QJsonObject metadata;
        metadata["identity"] = QString("file:///unrelated-%1.epub").arg(i);
        replace(folder + "/book.json", QJsonDocument(metadata).toJson());
    }
    const QString adoptionIdentity = "file:///changed.epub";
    HighlightStore unmarked(library.path(), adoptionIdentity, QByteArray(64, 'a'), "Book");
    require(unmarked.load() && unmarked.highlights().isEmpty()
                    && unmarked.adoptionMetadataReads() == 0,
            "Opening an unhighlighted book parsed unrelated metadata");
    require(unmarked.load() && unmarked.adoptionMetadataReads() == 0,
            "Reopening an unhighlighted book parsed unrelated metadata");
    unmarked.setHighlights({ h });
    require(unmarked.save(), "Cannot save older highlight revision");
    HighlightStore recent(library.path(), adoptionIdentity, QByteArray(64, 'b'), "Book");
    require(recent.load() && recent.highlights().size() == 1 && recent.adoptionMetadataReads() == 1,
            "Changed book did not adopt the older highlight revision");
    auto newer = h;
    newer.id = "newer";
    recent.setHighlights({ newer });
    require(recent.save(), "Cannot save newer highlight revision");
    const struct utimbuf oldTime{ 1000000000, 1000000000 }, newTime{ 1000000100, 1000000100 };
    require(utime(QFile::encodeName(unmarked.folder() + "/highlights.json").constData(), &oldTime)
                            == 0
                    && utime(QFile::encodeName(recent.folder() + "/highlights.json").constData(),
                             &newTime)
                            == 0,
            "Cannot order highlight revisions");
    HighlightStore adopted(library.path(), adoptionIdentity, QByteArray(64, 'c'), "Book");
    require(adopted.load() && adopted.highlights().size() == 1
                    && adopted.highlights().first().id == "newer"
                    && adopted.adoptionMetadataReads() == 2,
            "Changed book did not adopt its newest matching highlight revision");
    const QByteArray corruptHighlight("{broken");
    replace(recent.folder() + "/highlights.json", corruptHighlight);
    require(!adopted.load() && !adopted.save() && adopted.adoptionMetadataReads() == 2
                    && bytes(recent.folder() + "/highlights.json") == corruptHighlight,
            "A corrupt newest highlight revision was ignored or overwritten");
    // An authoritative empty list must not adopt older highlights.
    store.setHighlights({});
    require(store.save(), "Cannot delete all highlights");
    require(reopened.load() && reopened.highlights().isEmpty(),
            "Deleted highlights returned after reopening");
    return 0;
}
