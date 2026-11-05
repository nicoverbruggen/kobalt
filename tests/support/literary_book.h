#pragma once
#include "book_fixtures.h"
#include "engine_fixtures.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

namespace kobalt::test {
// The gallery uses complete, unchanged chapters. The everyday corpus test
// deliberately keeps its smaller excerpts and independent assertion inventory.
inline BookFiles literaryBook(const QString &root, const QJsonObject &entry, bool vertical,
                              QJsonObject &metadata)
{
    QFile source(root + '/' + entry["file"].toString());
    if (!source.open(QIODevice::ReadOnly))
        return {};
    const auto bytes = source.readAll();
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
        != entry["sha256"].toString().toLatin1())
        return {};
    metadata = QJsonDocument::fromJson(bytes).object();
    const auto language = metadata["language"].toString();
    metadata["language_name"] = QLocale::languageToString(QLocale(language).language());
    if (language != entry["language"].toString() || metadata["license"].toString().isEmpty())
        return {};
    const bool rtl = language == "ar" || language == "he";
    const QByteArray direction = rtl ? "rtl" : "ltr";
    const auto escape = [](const QJsonValue &value) {
        return value.toString().toHtmlEscaped().toUtf8();
    };
    BookFiles files;
    QByteArray spine, contents, sources;
    const auto chapters = metadata["chapters"].toArray();
    if (chapters.isEmpty())
        return {};
    for (int i = 0; i < chapters.size(); ++i) {
        const auto chapter = chapters[i].toObject();
        QByteArray original, body;
        for (const auto &p : chapter["paragraphs"].toArray()) {
            original += p.toString().toUtf8() + '\n';
            body += p.toString().toUtf8();
        }
        if (QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex()
            != chapter["sha256"].toString().toLatin1())
            return {};
        const auto name = "c" + QByteArray::number(i) + ".xhtml";
        const QByteArray mode = vertical && i % 2 == 0 ? "vertical-rl" : "horizontal-tb";
        files[QString::fromLatin1(name)] =
                "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + language.toUtf8()
                + "\" lang=\"" + language.toUtf8() + "\"><head><title>" + escape(chapter["title"])
                + "</title><style>body{writing-mode:" + mode + ";direction:" + direction
                + "}h1{font-size:1.2em;text-align:center;margin:1em 0}p{margin:0 0 .5em;text-indent:1em}</style></head>"
                  "<body dir=\""
                + direction + "\"><h1>" + escape(chapter["title"]) + "</h1>" + body
                + "</body></html>";
        spine += "<itemref idref=\"" + name + "\"/>";
        contents += "<li><a href=\"" + name + "\">" + escape(chapter["title"]) + "</a></li>";
        sources += "<dc:source>" + escape(chapter["source"]) + "</dc:source>";
    }
    files["nav.xhtml"] =
            "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
            "<head><title>Contents</title></head><body dir=\""
            + direction + "\"><nav epub:type=\"toc\"><ol>" + contents + "</ol></nav></body></html>";
    files = publisherPackage(language.toUtf8(), escape(metadata["title"]), files, spine);
    auto &opf = files["book.opf"];
    opf.replace("<spine>",
                "<spine page-progression-direction=\"" + QByteArray(rtl || vertical ? "rtl" : "ltr")
                        + "\">");
    opf.replace("<item id=\"nav.xhtml\"", "<item properties=\"nav\" id=\"nav.xhtml\"");
    opf.replace("</metadata>",
                "<dc:creator>" + escape(metadata["author"])
                        + "</dc:creator>"
                          "<dc:contributor>Wikisource contributors</dc:contributor><dc:rights>"
                        + escape(metadata["rights"]) + "; " + escape(metadata["license"])
                        + "</dc:rights>" + sources + "</metadata>");
    opf.replace(
            "</manifest>",
            "<item id=\"credits\" href=\"sources.json\" media-type=\"application/json\"/></manifest>");
    files["sources.json"] = bytes;
    metadata["source_sha256"] = entry["sha256"];
    metadata["writing_mode"] =
            vertical ? "alternating vertical-rl and horizontal-tb" : "horizontal-tb";
    return files;
}
} // namespace kobalt::test
