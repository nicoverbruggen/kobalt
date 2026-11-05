#include "engine/crengine/engine_document.h"
#include "reader_inputs.h"
#include "book_fixtures.h"
#include "engine_fixtures.h"
#include "check.h"
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRawFont>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTextDocumentFragment>

using namespace kobalt;

namespace {
QByteArray read(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), qPrintable("Cannot read " + path));
    return file.readAll();
}

QByteArray escaped(const QJsonValue &value)
{
    return value.toString().toHtmlEscaped().toUtf8();
}

QVector<SearchHit> search(EngineDocument &model, const QString &query)
{
    QVector<SearchHit> hits;
    QSet<QString> starts;
    for (int page = 0; page < model.pages(); ++page)
        for (const auto &hit : model.searchPage(query, page).hits) {
            require(!starts.contains(hit.start), "Duplicate multilingual search result");
            require(!hit.start.isEmpty() && !hit.end.isEmpty() && hit.page == page,
                    "Multilingual search returned an invalid source range");
            starts.insert(hit.start);
            hits.append(hit);
        }
    return hits;
}

QString prose(const QJsonArray &paragraphs, const QString &title)
{
    QString best;
    for (const auto &value : paragraphs) {
        // Ruby annotations split logical prose. Select one original contiguous run.
        const auto runs = value.toString().split(QRegularExpression("<ruby>.*?</ruby>"));
        for (const auto &run : runs) {
            const auto plain = QTextDocumentFragment::fromHtml(run).toPlainText().trimmed();
            if (plain != title && plain.size() > best.size())
                best = plain;
        }
    }
    return best.left(24);
}

void checkBook(const QJsonObject &entry, const QString &directory)
{
    const auto language = entry["language"].toString();
    const auto bytes = read(QStringLiteral(KOBALT_TEST_CORPUS) + '/' + entry["file"].toString());
    require(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                    == entry["sha256"].toString().toLatin1(),
            "Literary source checksum differs from the original fixture");
    const auto book = QJsonDocument::fromJson(bytes).object();
    require(book["language"].toString() == language && !book["license"].toString().isEmpty(),
            "Literary source identity or license is missing");
    const QString fontName = language == "ar" ? "NotoNaskhArabic-Regular.ttf"
            : language == "he"                ? "NotoSerifHebrew-Regular.ttf"
            : language == "hi"                ? "NotoSerifDevanagari-Regular.ttf"
                                              : "NotoSerifCJK-Regular.ttc";
    const QRawFont scriptFont(test::readerAssets().fontData(fontName), 36, QFont::PreferNoHinting);
    const char32_t letter = language == "ar" ? U'م'
            : language == "he"               ? U'א'
            : language == "hi"               ? U'क'
                                             : U'日';
    require(scriptFont.isValid() && scriptFont.supportsCharacter(letter),
            "Bundled script font has no probe glyph");
    const bool rtl = language == "ar" || language == "he";
    const QByteArray direction = rtl ? "rtl" : "ltr";
    test::BookFiles files;
    QByteArray spine, contents, sources;
    QStringList probes, titles;
    const auto chapters = book["chapters"].toArray();
    require(chapters.size() >= 2, "Literary fixture must include two original chapters");
    for (int index = 0; index < 2; ++index) {
        const auto chapter = chapters[index].toObject();
        const auto paragraphs = chapter["paragraphs"].toArray();
        QByteArray original;
        for (const auto &paragraph : paragraphs)
            original += paragraph.toString().toUtf8() + '\n';
        require(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex()
                        == chapter["sha256"].toString().toLatin1(),
                "Original literary chapter checksum differs");
        // Eight unmodified paragraphs per chapter bound routine corpus work.
        QJsonArray excerpt;
        QByteArray body;
        for (int p = 0; p < qMin(8, int(paragraphs.size())); ++p) {
            excerpt.append(paragraphs[p]);
            body += paragraphs[p].toString().toUtf8();
        }
        const auto title = chapter["title"].toString();
        titles.append(title);
        probes.append(prose(excerpt, title));
        require(!probes.last().isEmpty(), "Literary excerpt has no searchable prose");
        const auto name = "c" + QByteArray::number(index) + ".xhtml";
        files[QString::fromLatin1(name)] =
                "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + language.toUtf8()
                + "\" lang=\"" + language.toUtf8() + "\"><head><title>" + escaped(chapter["title"])
                + "</title></head><body dir=\"" + direction + "\"><h1>" + escaped(chapter["title"])
                + "</h1>" + body + "</body></html>";
        spine += "<itemref idref=\"" + name + "\"/>";
        contents += "<li><a href=\"" + name + "\">" + escaped(chapter["title"]) + "</a></li>";
        sources += "<dc:source>" + escaped(chapter["source"]) + "</dc:source>";
    }
    files["nav.xhtml"] =
            "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
            "<head><title>Contents</title></head><body><nav epub:type=\"toc\"><ol>"
            + contents + "</ol></nav></body></html>";
    files = test::publisherPackage(language.toUtf8(), escaped(book["title"]), files, spine);
    auto &opf = files["book.opf"];
    opf.replace("<spine>", "<spine page-progression-direction=\"" + direction + "\">");
    opf.replace("<item id=\"nav.xhtml\"", "<item properties=\"nav\" id=\"nav.xhtml\"");
    opf.replace("</metadata>",
                "<dc:creator>" + escaped(book["author"])
                        + "</dc:creator>"
                          "<dc:contributor>Wikisource contributors</dc:contributor><dc:rights>"
                        + escaped(book["rights"]) + "; " + escaped(book["license"]) + "</dc:rights>"
                        + sources + "</metadata>");
    opf.replace(
            "</manifest>",
            "<item id=\"credits\" href=\"sources.json\" media-type=\"application/json\"/></manifest>");
    files["sources.json"] = bytes;
    const auto name = language + ".epub";
    require(test::writeBook(directory, name, files), "Cannot generate literary EPUB");
    EngineDocument model(test::readerFonts());
    QString error;
    require(model.open(read(directory + '/' + name), { 1264, 1680 }, {}, &error),
            qPrintable(error));
    require(model.pages() > 1, "Literary chapters did not paginate");
    require(model.state()["page_progression_rtl"].toBool() == rtl,
            "Literary book has the wrong page direction");
    const auto links = model.chapters();
    for (int chapter = 0; chapter < 2; ++chapter) {
        bool visited = false;
        for (const auto &link : links)
            if (link.title == titles[chapter]) {
                require(model.goTo(link.position) && !model.image(model.page()).isNull(),
                        "Literary chapter navigation failed");
                visited = true;
                break;
            }
        require(visited, "Literary TOC lost an original chapter title");
        const auto hits = search(model, probes[chapter]);
        require(!hits.isEmpty(),
                qPrintable("Original literary prose is not searchable: " + probes[chapter]));
        const auto anchor = hits.first().start;
        require(model.goTo(anchor), "Literary search anchor did not resolve");
        const auto rectangles = model.rangeRects(hits.first().start, hits.first().end);
        require(!rectangles.isEmpty(), "Literary prose has no selection geometry");
        int ink = 0;
        const auto page = model.image(model.page());
        for (const auto &rect : rectangles)
            for (int y = qMax(0, rect.top()); y <= qMin(page.height() - 1, rect.bottom()); ++y)
                for (int x = qMax(0, rect.left()); x <= qMin(page.width() - 1, rect.right()); ++x)
                    ink += qGray(page.pixel(x, y)) < 230;
        require(ink > 2, "Literary prose has no rendered ink");
        // A multiword range center can be whitespace, especially in RTL prose.
        // Find a selectable word inside the searched text, as the word-selection suite does.
        WordSelection selected;
        const auto probe = probes[chapter].normalized(QString::NormalizationForm_C);
        for (const auto &rectangle : rectangles) {
            const auto area = rectangle.intersected(page.rect());
            for (int x = area.left(); x <= area.right() && selected.text.isEmpty(); x += 2) {
                const auto candidate = model.wordAt({ x, area.center().y() });
                if (!candidate.text.isEmpty()
                    && probe.contains(candidate.text.normalized(QString::NormalizationForm_C)))
                    selected = candidate;
            }
        }
        require(!selected.text.isEmpty() && !selected.start.isEmpty() && !selected.end.isEmpty(),
                qPrintable("Native-script prose has no selectable word: " + language));
        require(model.style(48, 130), "Literary book could not reflow");
        const auto reflowed = search(model, probes[chapter]);
        require(!reflowed.isEmpty() && reflowed.first().start == anchor && model.goTo(anchor),
                "Literary source anchor changed after reflow");
    }
    const auto checkpoint = model.state();
    EngineDocument reopened(test::readerFonts());
    require(reopened.open(read(directory + '/' + name), { 1264, 1680 }, checkpoint, &error),
            qPrintable(error));
    require(reopened.position() == model.position(), "Literary reopen lost its source position");
    std::printf("PASS: %s literary excerpts, native TOC, search, selection, reflow and reopen\n",
                qPrintable(language));
}
} // namespace

// Checks Arabic, Hebrew, Hindi and Japanese excerpts: chapter titles, script glyphs, direction, search, selection and reflow.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const auto entries =
            QJsonDocument::fromJson(read(QStringLiteral(KOBALT_TEST_CORPUS) + "/catalog.json"))
                    .array();
    require(entries.size() == 4, "Representative literary inventory must contain four languages");
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create literary fixture directory");
    QSet<QString> found;
    for (const auto &entry : entries) {
        const auto language = entry.toObject()["language"].toString();
        require(!found.contains(language), "Duplicate literary language");
        found.insert(language);
        checkBook(entry.toObject(), directory.path());
    }
    require(found == QSet<QString>({ "ar", "he", "hi", "ja" }),
            "Representative literary selection changed");
}
