#pragma once
#include <QBuffer>
#include <QImage>
#include <QMap>
#include <QString>

namespace kobalt::test {
using BookFiles = QMap<QString, QByteArray>;

inline QByteArray publisherHtml(const QByteArray &body)
{
    return "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
           "<head><title>Publisher markup</title></head><body>"
            + body + "</body></html>";
}

inline BookFiles publisherPackage(const QByteArray &language, const QByteArray &title,
                                  BookFiles files, const QByteArray &spine)
{
    QByteArray manifest;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const auto name = it.key().toUtf8();
        const auto type = name.endsWith(".xhtml") ? "application/xhtml+xml"
                : name.endsWith(".svg")           ? "image/svg+xml"
                : name.endsWith(".css")           ? "text/css"
                                                  : "font/ttf";
        manifest += "<item id=\"" + name + "\" href=\"" + name + "\" media-type=\"" + type + "\"/>";
    }
    files["mimetype"] = "application/epub+zip";
    files["META-INF/container.xml"] =
            "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\"><rootfiles>"
            "<rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    files["book.opf"] =
            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">"
            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">kobalt-publisher-"
            + language + "</dc:identifier><dc:title>" + title + "</dc:title><dc:language>"
            + language + "</dc:language></metadata><manifest>" + manifest + "</manifest><spine>"
            + spine + "</spine></package>";
    return files;
}

// These fixtures retain the text, span IDs and resource paths from develop.
// Position-codec and pixel assertions depend on those details.
inline BookFiles engineBook(bool colors = false, int chapterCount = 3, int paragraphs = 30)
{
    BookFiles files;
    const QByteArray prefix = "OEBPS/";
    const QByteArray paragraph =
            "A reader keeps the chapter ready. Changing the size of the words should preserve the passage. "
            "We can turn the page, visit another chapter, and return to the same place. ";
    QByteArray manifest, spine, navigation;
    if (colors) {
        QImage image(128, 64, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixel(x, y,
                               qRgba(0, x < 64 ? 192 : 0, x < 64 ? 0 : 255, y < 32 ? 255 : 128));
        QBuffer buffer(&files["OEBPS/colors.png"]);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {};
        files["OEBPS/artwork/colors.svg"] =
                "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"384\" height=\"64\" viewBox=\"0 0 384 64\">"
                "<rect width=\"64\" height=\"64\" fill=\"#ff00ff\"/><text x=\"72\" y=\"44\" font-family=\"Bitter\" font-size=\"32\" fill=\"#008080\">SVG</text>"
                "<image x=\"200\" width=\"128\" height=\"64\" xlink:href=\"../colors.png\"/></svg>";
        manifest += "<item id=\"colors\" href=\"colors.png\" media-type=\"image/png\"/>"
                    "<item id=\"svg\" href=\"artwork/colors.svg\" media-type=\"image/svg+xml\"/>";
    }
    for (int chapter = 1; chapter <= chapterCount; ++chapter) {
        const auto number = QByteArray::number(chapter);
        const auto name = "c" + number + ".xhtml";
        manifest += "<item id=\"c" + number + "\" href=\"" + name
                + "\" media-type=\"application/xhtml+xml\"/>";
        spine += "<itemref idref=\"c" + number + "\"/>";
        navigation += "<navPoint id=\"n" + number + "\" playOrder=\"" + number
                + "\"><navLabel><text>Chapter " + number + "</text></navLabel><content src=\""
                + name + "\"/></navPoint>";
        QByteArray body;
        if (colors)
            body = "<p style=\"color:#ff0000\">Publisher red text.</p><p><span style=\"background-color:#ffff00\">Yellow highlight.</span></p>"
                   "<p><img src=\"colors.png\" width=\"128\" height=\"64\" alt=\"Color test\"/></p>"
                   "<p><img src=\"artwork/colors.svg\" width=\"384\" height=\"64\" alt=\"SVG test\"/></p>"
                   "<p><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"24\" viewBox=\"0 0 64 24\">"
                   "<rect width=\"64\" height=\"24\" fill=\"#ff8000\"/></svg></p>";
        for (int index = 2; index < paragraphs + 2; ++index)
            body += "<p><span class=\"koboSpan\" id=\"kobo." + QByteArray::number(index) + ".1\">"
                    + paragraph.repeated(3) + "</span></p>";
        files[QString::fromUtf8(prefix + name)] =
                "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chapter " + number
                + "</title></head>"
                  "<body><h1><span class=\"koboSpan\" id=\"kobo.1.1\">Chapter "
                + number + "</span></h1>" + body + "</body></html>";
    }
    files["mimetype"] = "application/epub+zip";
    files["META-INF/container.xml"] =
            "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
            "<rootfile full-path=\"OEBPS/book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    files["OEBPS/book.opf"] =
            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"uid\">kobalt-fixture</dc:identifier>"
            "<dc:title>Native reader fixture</dc:title><dc:language>en</dc:language><dc:creator>Kobalt</dc:creator></metadata><manifest>"
            + manifest
            + "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/></manifest><spine toc=\"ncx\" page-progression-direction=\"ltr\">"
            + spine + "</spine></package>";
    files["OEBPS/toc.ncx"] =
            "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head><meta name=\"dtb:uid\" content=\"kobalt-fixture\"/></head>"
            "<docTitle><text>Native reader fixture</text></docTitle><navMap>"
            + navigation + "</navMap></ncx>";
    return files;
}

inline BookFiles wordCountBook(int repeats = 1)
{
    const QByteArray paragraph =
            "<p>rain<span>bow</span> don't café café co&#173;operation A𐐀word 123</p>";
    BookFiles files{
        { "c1.xhtml",
          "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Ignored title</title></head><body>"
          "<p>Front matter.</p><h1 id=\"first\">First chapter</h1>"
                  + paragraph.repeated(repeats)
                  + "<p>日本語中文 عربي كلمتان עברית שלום 한국어 문장 ภาษาไทย</p><p style=\"display:none\">These hidden words must not count.</p>"
                    "<ruby>漢字<rt>ruby annotation</rt><rp>ignored fallback</rp></ruby><h2 id=\"part\">Second part</h2>"
                    "<p>After inline anchors.</p><p><b>un<span id=\"inside\">break</span>able</b></p></body></html>" },
        { "c2.xhtml",
          "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Ignored title</title></head><body>"
          "<h1 id=\"last\">Last chapter</h1><p>one two three</p></body></html>" },
        { "notes.xhtml",
          "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head/><body><h1>Note</h1><p>Extra note text</p></body></html>" }
    };
    files["mimetype"] = "application/epub+zip";
    files["META-INF/container.xml"] =
            "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\"><rootfiles>"
            "<rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    files["book.opf"] =
            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"uid\">logical-words</dc:identifier><dc:title>Logical word fixture</dc:title><dc:language>en</dc:language></metadata>"
            "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
            "<item id=\"notes\" href=\"notes.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
            "</manifest><spine toc=\"ncx\"><itemref idref=\"c1\"/><itemref idref=\"c2\"/><itemref idref=\"notes\" linear=\"no\"/></spine></package>";
    QByteArray navigation;
    const QList<QByteArray> ids{ "first", "part", "alias", "inside", "last" };
    const QList<QByteArray> titles{ "First", "Part", "Alias", "Inside", "Last" };
    const QList<QByteArray> hrefs{ "c1.xhtml#first", "c1.xhtml#part", "c1.xhtml#part",
                                   "c1.xhtml#inside", "c2.xhtml#last" };
    for (int index = 0; index < ids.size(); ++index)
        navigation += "<navPoint id=\"" + ids[index] + "\" playOrder=\""
                + QByteArray::number(index + 1) + "\"><navLabel><text>" + titles[index]
                + "</text></navLabel><content src=\"" + hrefs[index] + "\"/></navPoint>";
    files["toc.ncx"] =
            "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head/><docTitle><text>Words</text></docTitle><navMap>"
            + navigation + "</navMap></ncx>";
    return files;
}

inline QMap<QString, BookFiles> publisherBooks(const QByteArray &font)
{
    const QByteArray continuation =
            QByteArray(
                    "<p>Linear text continues after the aside with enough words to fill part of a page.</p>")
                    .repeated(12);
    BookFiles publisher{
        { "start.xhtml",
          publisherHtml(
                  "<p id=\"lead\">Read the first note<a epub:type=\"noteref\" href=\"notes.xhtml#n1\">1</a> and the second<a epub:type=\"noteref\" href=\"notes.xhtml#n2\">2</a>.</p>"
                  "<pre id=\"code\">iiiiiiiiii\nMMMMMMMMMM</pre><p id=\"scripts\">Greek αβγΩ, Cyrillic Жж, phonetic ɛʃ and mathematics ∑∫→.</p>"
                  "<p id=\"search\">The ﬁnal e‑mail about hyphen​ation.</p><p lang=\"fr\">Une phrase française dans un livre anglais.</p>"
                  "<aside epub:type=\"footnote\" id=\"aside\"><p>Asidetext belongs to a footnote.</p></aside>"
                  + continuation) },
        { "extra.xhtml",
          publisherHtml("<p>Nonlinearspine content outside the reading order.</p>") },
        { "plate.svg",
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"><rect width=\"100\" height=\"100\" fill=\"#888\"/></svg>" },
        { "after.xhtml",
          publisherHtml(
                  "<p>First paragraph of the second chapter.</p><p>Domtarget marks the saved position.</p>"
                  + continuation) },
        { "notes.xhtml",
          publisherHtml(
                  "<h2>Notes</h2><p id=\"n1\">Note one opens here.</p><p>Note one continues in a second paragraph.</p>"
                  "<p id=\"n2\">Note two stands alone.</p><h2>Afterword</h2><p>Closingword ends the book.</p>") }
    };
    QMap<QString, BookFiles> books;
    books["publisher.epub"] = publisherPackage(
            "en", "Publisher markup", publisher,
            "<itemref idref=\"start.xhtml\"/><itemref idref=\"extra.xhtml\" linear=\"no\"/><itemref idref=\"plate.svg\"/>"
            "<itemref idref=\"after.xhtml\"/><itemref idref=\"notes.xhtml\"/>");
    auto oneChapter = [&](const QString &name, const QByteArray &language, const QByteArray &title,
                          const QByteArray &body) {
        books[name] = publisherPackage(language, title, { { "text.xhtml", publisherHtml(body) } },
                                       "<itemref idref=\"text.xhtml\"/>");
    };
    const QByteArray german =
            "Die Donaudampfschifffahrtsgesellschaftskapitäne besprachen Rechtsschutzversicherungsgesellschaften "
            "und Straßenbahnhaltestellen, während die Arbeitsunfähigkeitsbescheinigungen auf dem Tisch lagen. ";
    oneChapter("publisher-de.epub", "de", "Silbentrennung",
               ("<p>" + german.repeated(4) + "</p>").repeated(6));
    oneChapter(
            "publisher-ru.epub", "ru", "Перенос",
            "<p>" + QByteArray("Достопримечательности государственного университета. ").repeated(40)
                    + "</p>");
    const QMap<QString, QByteArray> hyphenation{
        { "et-EE", "rahvusvaheline" }, { "fi-FI", "kirjallisuus" },
        { "ka-GE", "საქართველო" },     { "hu-HU", "Magyarország" },
        { "lt-LT", "universitetas" },  { "lv-LV", "universitāte" },
        { "pl-PL", "przedstawienie" }, { "ru-RU", "программирование" },
        { "sk-SK", "medzinárodného" }, { "sl-SI", "slovenščina" },
        { "uk-UA", "література" }
    };
    for (auto it = hyphenation.cbegin(); it != hyphenation.cend(); ++it)
        oneChapter("hyphenation-" + it.key() + ".epub", it.key().toUtf8(), "Hyphenation",
                   "<p>" + (it.value() + ' ').repeated(80) + "</p>");
    for (const QByteArray language : { "ja", "ko", "zh-Hans", "zh-Hant", "zh-HK", "inline" }) {
        const bool inlined = language == "inline";
        oneChapter("cjk-" + QString::fromUtf8(language) + ".epub",
                   inlined ? QByteArray("ja") : language, "CJK fallback",
                   inlined ? "<p style=\"text-indent:0\" lang=\"zh-Hans\">骨直令曜</p>"
                           : "<p style=\"text-indent:0\">骨直令曜</p>");
    }
    books["equations.epub"] = publisherPackage(
            "en", "Equation rendering",
            { { "text.xhtml",
                publisherHtml(
                        "<p>Inline <img src=\"size.svg\" style=\"width:4ex;height:2ex\"/> equation.</p>"
                        "<p><img data-tex=\"x\" src=\"math.svg\" style=\"width:8ex;height:4ex\"/></p>") },
              { "size.svg",
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"200\" viewBox=\"0 0 400 200\"><rect width=\"400\" height=\"200\" fill=\"#d02030\"/></svg>" },
              { "math.svg",
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"200\" viewBox=\"0 0 400 200\"><rect width=\"100\" height=\"200\"/>"
                "<rect x=\"100\" width=\"100\" height=\"200\" fill=\"#333333\"/><rect x=\"200\" width=\"100\" height=\"200\" fill=\"#00cc66\"/></svg>" } },
            "<itemref idref=\"text.xhtml\"/>");
    const QByteArray link = "<link rel=\"stylesheet\" href=\"fonts.css\"/></head>";
    auto embedded = publisherHtml(
            "<p class=\"embedded\">Embedded family text.</p><p>Fallback family text.</p>");
    embedded.replace("</head>", link);
    books["embedded-fonts.epub"] = publisherPackage(
            "en", "Embedded fonts",
            { { "text.xhtml", embedded },
              { "font.ttf", font },
              { "fonts.css",
                "@font-face{font-family:Embedded;src:url(font.ttf)}@font-face{font-family:Embedded;font-style:italic;src:url(font.ttf)}"
                "@font-face{font-family:Missing;src:url(missing.ttf)}@font-face{font-family:Alias;src:local(\"Libron\")}p.embedded{font-family:Embedded}" } },
            "<itemref idref=\"text.xhtml\"/>");
    const QByteArray css =
            "p,pre{page-break-before:always;text-indent:0}.serif{font-family:serif}.sans{font-family:sans-serif}"
            ".mono{font-family:monospace}.named{font-family:\"IBM Plex Sans\",serif}.embedded{font-family:Embedded,serif}";
    auto families = publisherHtml(
            "<p class=\"serif\">Serifmarker family text.</p><p class=\"sans\">Sansmarker family text.</p>"
            "<pre class=\"mono\">Monomarker family text.</pre><p class=\"named\">Namedmarker family text.</p>");
    families.replace("</head>", link);
    BookFiles familyFiles{ { "text.xhtml", families }, { "fonts.css", css } };
    books["font-families.epub"] = publisherPackage("en", "Font family defaults", familyFiles,
                                                   "<itemref idref=\"text.xhtml\"/>");
    auto embeddedFamily = publisherHtml("<p class=\"embedded\">Embeddedmarker family text.</p>");
    embeddedFamily.replace("</head>", "<link rel=\"stylesheet\" href=\"embedded.css\"/></head>");
    familyFiles["embedded.xhtml"] = embeddedFamily;
    familyFiles["embedded.css"] = "@font-face{font-family:Embedded;src:url(font.ttf)}" + css;
    familyFiles["font.ttf"] = font;
    books["font-families-embedded.epub"] =
            publisherPackage("en", "Font family defaults", familyFiles,
                             "<itemref idref=\"text.xhtml\"/><itemref idref=\"embedded.xhtml\"/>");
    return books;
}
} // namespace kobalt::test
