#include "annotation_export.h"
#include "support/main_thread.h"
#include <annotation_export_html.h>
#include "render/highlight_paint.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <array>
#include <limits>
#include <utility>
#include <zlib.h>

namespace kobalt {
namespace {
QString lines(QString value)
{
    value.replace("\r\n", "\n");
    value.replace('\r', '\n');
    return value;
}

QString markdown(QString value)
{
    value = lines(value);
    value.replace('&', "&amp;");
    value.replace('<', "&lt;");
    value.replace('>', "&gt;");
    QString escaped;
    for (const auto ch : value) {
        if (QStringLiteral("\\`*_{}[]()#+.!|>~-").contains(ch))
            escaped += '\\';
        escaped += ch;
    }
    return escaped;
}

QString paragraphs(const QString &value, bool marked = false)
{
    QString html;
    for (const auto &paragraph : lines(value).split(QStringLiteral("\n\n")))
        html += marked
                ? "<p><span class=\"passage-mark\">" + paragraph.toHtmlEscaped() + "</span></p>"
                : "<p>" + paragraph.toHtmlEscaped() + "</p>";
    return html;
}

QString utcTimestamp(const QDateTime &value)
{
    return value.isValid() ? value.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")) + 'Z'
                           : QString();
}

QString stamp(const QDateTime &value)
{
    return value.isValid()
            ? QLocale(QLocale::English)
                      .toString(value.toUTC(), QStringLiteral("d MMMM yyyy 'at' HH:mm:ss 'UTC'"))
            : QStringLiteral("Unknown date");
}

struct Chapter
{
    QString label;
    QVector<int> rows;
};

QByteArray document(const QString &title, const QString &author, const QVector<Highlight> &rows,
                    const QDateTime &exportedAt, const QByteArray &font,
                    AnnotationExportFormat format)
{
    if (rows.isEmpty() || rows.size() > HighlightStore::maxHighlights || !exportedAt.isValid())
        return {};
    QVector<Chapter> chapters;
    QSet<QString> ids;
    int notes = 0;
    qint64 units = 0;
    for (int i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        if (!validHighlight(row) || ids.contains(row.id))
            return {};
        ids.insert(row.id);
        units += row.text.size() + row.note.size() + row.chapter.size();
        if (units > HighlightStore::maxFileBytes)
            return {};
        if (!row.note.isEmpty())
            ++notes;
        const auto label = row.chapter.simplified().isEmpty()
                ? QStringLiteral("Unspecified chapter")
                : row.chapter.simplified();
        // Equal labels can occur in different parts of a book. Group consecutive
        // records only, preserving the source order supplied by the session.
        if (chapters.isEmpty() || chapters.last().label != label)
            chapters.append({ label, {} });
        chapters.last().rows.append(i);
    }
    const auto bookTitle =
            title.simplified().isEmpty() ? QStringLiteral("Untitled book") : title.simplified();
    const auto bookAuthor = author.simplified();
    const auto entryName = QStringLiteral("Annotation ");
    const auto kind = QStringLiteral("annotations");
    const auto heading = QStringLiteral("Annotations");
    const auto counts = QString::number(rows.size())
            + (rows.size() == 1 ? " annotation" : " annotations")
            + (" · " + QString::number(notes) + (notes == 1 ? " note" : " notes"));
    const auto summary = counts + " · Exported " + stamp(exportedAt);
    if (format == AnnotationExportFormat::Markdown) {
        QString md = "# " + markdown(bookTitle) + "\n\n";
        if (!bookAuthor.isEmpty())
            md += markdown(bookAuthor) + "\n\n";
        md += summary + "\n\n";
        for (const auto &group : chapters) {
            md += "## " + markdown(group.label) + "\n\n";
            for (const auto index : group.rows) {
                const auto &row = rows[index];
                const auto number = QString::number(index + 1);
                const auto label = entryName + number.rightJustified(3, '0');
                md += "### " + label + "\n\n";
                const auto passage = markdown(row.text).split('\n');
                for (int line = 0; line < passage.size(); ++line) {
                    md += "> " + passage[line];
                    if (!passage[line].isEmpty() && line + 1 < passage.size()
                        && !passage[line + 1].isEmpty())
                        md += "  ";
                    md += '\n';
                }
                md += '\n';
                if (!row.note.isEmpty())
                    md += "**Note**\n\n" + markdown(row.note) + "\n\n";
                md += "*Added " + stamp(row.created) + "*\n\n---\n\n";
            }
        }
        md += "Exported from Kobalt.\n";
        return md.toUtf8();
    }
    const bool embeddedFont = !font.isEmpty();
    const auto fontStyle = embeddedFont
            ? QStringLiteral("@font-face{font-family:'Kobalt Libron';src:url(data:font/ttf;base64,")
                    + QString::fromLatin1(font.toBase64())
                    + ") format('truetype');font-weight:400;font-style:normal;font-display:swap;}"
            : QString();
    QString html =
            QStringLiteral(
                    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>")
            + bookTitle.toHtmlEscaped() + " · " + heading + "</title><style>" + fontStyle
            + QString::fromUtf8(annotation_export::style)
            + "</style></head><body><main id=\"top\" data-kind=\"annotations\"><header><p class=\"eyebrow\">"
            + heading + " · Kobalt</p><h1>" + bookTitle.toHtmlEscaped() + "</h1>";
    if (!bookAuthor.isEmpty())
        html += "<p class=\"author\">" + bookAuthor.toHtmlEscaped() + "</p>";
    html += "<p class=\"summary\">" + counts + " · Exported <time data-local-time datetime=\""
            + utcTimestamp(exportedAt) + "\">" + stamp(exportedAt) + "</time></p></header>";
    const auto markupOption = QStringLiteral(
            "<div class=\"markup-option\"><label for=\"show-markup\"><input type=\"checkbox\" id=\"show-markup\" checked autocomplete=\"off\" aria-controls=\"annotations chronological\">Show annotation styling</label></div>");
    html += QStringLiteral(
                    "<div class=\"search-toolbar\" id=\"search-toolbar\" hidden>"
                    "<div class=\"search-field\" role=\"search\"><label for=\"query\">%1</label>"
                    "<input type=\"search\" id=\"query\" placeholder=\"Search this export\" autocomplete=\"off\" spellcheck=\"false\" aria-controls=\"annotations chronological\"></div>"
                    "<div class=\"sort-field\"><label for=\"sort-order\">Sort by</label><select id=\"sort-order\" autocomplete=\"off\">"
                    "<option value=\"position\">Book order</option><option value=\"newest\">Newest first</option>"
                    "<option value=\"oldest\">Oldest first</option></select></div>"
                    "<button id=\"clear-search\" type=\"button\">Clear</button>"
                    "<span id=\"search-status\" role=\"status\" aria-live=\"polite\" aria-atomic=\"true\"></span>"
                    "%2</div>"
                    "<noscript><p>Search and sorting need JavaScript. All %3 are listed below in book order.</p></noscript>"
                    "<nav id=\"chapter-navigation\" aria-label=\"Chapters\"><details><summary id=\"chapter-summary\">Browse ")
                    .arg(QStringLiteral("Search highlights and notes"), markupOption, kind)
            + QString::number(chapters.size()) + " chapters</summary><ol>";
    for (int i = 0; i < chapters.size(); ++i)
        html += "<li><a href=\"#chapter-" + QString::number(i + 1) + "\">"
                + chapters[i].label.toHtmlEscaped() + "</a></li>";
    html += "</ol></details></nav><p id=\"no-results\" role=\"status\" hidden>No matching " + kind
            + ". Try fewer words or clear the search.</p><div id=\"annotations\">";
    for (int chapter = 0; chapter < chapters.size(); ++chapter) {
        const auto &group = chapters[chapter];
        const auto key = QString::number(chapter + 1);
        html += "<section class=\"chapter\" id=\"chapter-" + key + "\" aria-labelledby=\"heading-"
                + key + "\"><h2 id=\"heading-" + key + "\">" + group.label.toHtmlEscaped()
                + " <span class=\"chapter-count\">" + QString::number(group.rows.size())
                + "</span></h2>";
        for (const auto index : group.rows) {
            const auto &row = rows[index];
            const auto number = QString::number(index + 1);
            html += "<article id=\"annotation-" + number + "\" data-created=\""
                    + utcTimestamp(row.created) + "\" aria-label=\"" + entryName + number
                    + "\"><a class=\"number\" href=\"#annotation-" + number + "\" aria-label=\""
                    + entryName + number + "\">#" + number
                    + "</a><div class=\"entry\"><p class=\"entry-chapter\">"
                    + group.label.toHtmlEscaped() + "</p>";
            const auto color = highlightDisplayColor(row.color);
            const auto tint = render::highlightTint(color).name();
            html += "<blockquote data-style=\"" + highlightDisplayStyle(row.style)
                    + "\" data-color=\"" + color + "\" style=\"--mark:" + tint
                    + ";--mark-ink:" + (color == "gray" ? QStringLiteral("#000000") : tint) + "\">"
                    + paragraphs(row.text, true) + "</blockquote>";
            if (!row.note.isEmpty())
                html += "<aside class=\"note\" aria-label=\"Your note\"><h3>Note</h3>"
                        + paragraphs(row.note) + "</aside>";
            html += "<p class=\"annotation-meta\">Added <time data-local-time datetime=\""
                    + utcTimestamp(row.created) + "\">" + stamp(row.created)
                    + "</time></p></div></article>";
        }
        html += "</section>";
    }
    html += "</div><div id=\"chronological\" class=\"chronological\" hidden></div><footer><span>Exported from Kobalt. Print includes all "
            + kind + " in the selected order.</span><a href=\"#top\">Back to top ↑</a></footer>";
    if (embeddedFont)
        html += "<!-- Libron is licensed under the SIL Open Font License (OFL) 1.1. https://openfontlicense.org/ -->";
    html += "</main><script>" + QString::fromUtf8(annotation_export::search)
            + "</script></body></html>";
    return html.toUtf8();
}

QByteArray deflate(const QByteArray &bytes)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY)
        != Z_OK)
        return {};
    const auto bound = deflateBound(&stream, uLong(bytes.size()));
    if (bound > uLong(std::numeric_limits<int>::max())) {
        deflateEnd(&stream);
        return {};
    }
    QByteArray packed(int(bound), '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.constData()));
    stream.avail_in = uInt(bytes.size());
    stream.next_out = reinterpret_cast<Bytef *>(packed.data());
    stream.avail_out = uInt(packed.size());
    const auto result = ::deflate(&stream, Z_FINISH);
    packed.resize(int(stream.total_out));
    deflateEnd(&stream);
    return result == Z_STREAM_END ? packed : QByteArray();
}
} // namespace

QString annotationExportFileName(const QString &identity, const QString &title,
                                 AnnotationExportFormat format)
{
    if (identity.isEmpty())
        return {};
    QString name = title.normalized(QString::NormalizationForm_C);
    name.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f<>:\"/\\\\|?*]")), "_");
    name = name.trimmed().left(60);
    while (name.startsWith('.'))
        name.remove(0, 1);
    while (name.endsWith('.') || name.endsWith(' ')
           || (!name.isEmpty() && name.at(name.size() - 1).isHighSurrogate()))
        name.chop(1);
    if (name.isEmpty())
        name = QStringLiteral("Book");
    const auto key = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
                             .toHex()
                             .left(16);
    return name + "-" + QString::fromLatin1(key)
            + (format == AnnotationExportFormat::Markdown ? "-markdown.zip" : "-html.zip");
}

namespace {
QByteArray archiveDocument(QByteArray payload, const QDateTime &exportedAt,
                           AnnotationExportFormat format)
{
    if (payload.isEmpty())
        return {};

    struct Entry
    {
        QByteArray name, bytes;
        quint32 crc = 0, size = 0, offset = 0;
    };

    std::array<Entry, 1> entries{
        { { format == AnnotationExportFormat::Markdown ? "annotations.md" : "annotations.html",
            std::move(payload) } }
    };
    auto date = exportedAt.toUTC().date();
    if (date.year() < 1980 || date.year() > 2107)
        date = QDate(1980, 1, 1);
    const auto time = exportedAt.toUTC().time();
    const quint16 dosDate = quint16(((date.year() - 1980) << 9) | (date.month() << 5) | date.day());
    const quint16 dosTime =
            quint16((time.hour() << 11) | (time.minute() << 5) | (time.second() / 2));
    QByteArray archive;
    QDataStream out(&archive, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    // One fixed filename, no extra fields or descriptors. Each raw DEFLATE
    // payload has its size and CRC in both the local and central headers.
    for (auto &entry : entries) {
        if (entry.bytes.size() > 64 * 1024 * 1024)
            return {};
        entry.size = quint32(entry.bytes.size());
        entry.crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(entry.bytes.constData()),
                                  uInt(entry.bytes.size())));
        entry.bytes = deflate(entry.bytes);
        if (entry.bytes.isEmpty())
            return {};
        entry.offset = quint32(archive.size());
        out << quint32(0x04034b50) << quint16(20) << quint16(0x800) << quint16(8) << dosTime
            << dosDate << entry.crc << quint32(entry.bytes.size()) << entry.size
            << quint16(entry.name.size()) << quint16(0);
        out.writeRawData(entry.name.constData(), int(entry.name.size()));
        out.writeRawData(entry.bytes.constData(), int(entry.bytes.size()));
    }
    const auto directory = quint32(archive.size());
    for (const auto &entry : entries) {
        out << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0x800) << quint16(8)
            << dosTime << dosDate << entry.crc << quint32(entry.bytes.size()) << entry.size
            << quint16(entry.name.size()) << quint16(0) << quint16(0) << quint16(0) << quint16(0)
            << quint32(0) << entry.offset;
        out.writeRawData(entry.name.constData(), int(entry.name.size()));
    }
    const auto directorySize = quint32(archive.size()) - directory;
    out << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(1) << quint16(1)
        << directorySize << directory << quint16(0);
    return out.status() == QDataStream::Ok ? archive : QByteArray();
}
} // namespace

QByteArray annotationExportArchive(const QString &title, const QString &author,
                                   const QVector<Highlight> &rows, AnnotationExportFormat format,
                                   const QDateTime &exportedAt, const QByteArray &font)
{
    return archiveDocument(document(title, author, rows, exportedAt, font, format), exportedAt,
                           format);
}

bool writeAnnotationExport(const QUrl &destination, const QByteArray &archive)
{
    if (archive.isEmpty() || !destination.isValid())
        return false;
    // Known exception: the user waits for the exported file.
    mainthread::AllowBlocking allow("annotation export");
    mainthread::BlockingOperation blockingOperation("annotation-export-write");
    if (destination.isLocalFile()) {
        QSaveFile file(destination.toLocalFile());
        return file.open(QIODevice::WriteOnly) && file.write(archive) == archive.size()
                && file.commit();
    }
    if (destination.scheme() != "content")
        return false;
    QFile file(destination.toString(QUrl::FullyEncoded));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool written = file.write(archive) == archive.size() && file.flush();
    file.close();
    return written && file.error() == QFileDevice::NoError;
}

QString exportAnnotations(const QString &directory, const QString &identity, const QString &title,
                          const QString &author, const QVector<Highlight> &rows,
                          AnnotationExportFormat format, const QByteArray &font)
{
    const auto name = annotationExportFileName(identity, title, format);
    const auto bytes = annotationExportArchive(title, author, rows, format,
                                               QDateTime::currentDateTimeUtc(), font);
    if (directory.isEmpty() || name.isEmpty() || bytes.isEmpty() || !QDir().mkpath(directory))
        return {};
    const auto path = QDir(directory).filePath(name);
    return writeAnnotationExport(QUrl::fromLocalFile(path), bytes) ? path : QString();
}
} // namespace kobalt
