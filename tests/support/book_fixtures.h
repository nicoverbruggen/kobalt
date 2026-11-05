#pragma once
#include <QMap>
#include <QByteArray>
#include <QString>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

namespace kobalt::test {
inline bool writeBook(const QString &directory, const QString &filename,
                      const QMap<QString, QByteArray> &files)
{
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const auto path = directory + '/' + it.key();
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            return false;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size()
            || !file.flush()
            || !file.setFileTime(QDateTime::fromSecsSinceEpoch(946684800),
                                 QFileDevice::FileModificationTime)) {
            return false;
        }
    }
    QProcess zip;
    zip.setWorkingDirectory(directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("TZ", "UTC");
    zip.setProcessEnvironment(environment);
    const auto archive = directory + '/' + filename;
    QStringList members{ "-X", "-q", archive };
    for (const auto &name : files.keys())
        if (name != "mimetype")
            members.append(name);
    // EPUB requires the first ZIP member to be the uncompressed mimetype.
    for (const QStringList &arguments :
         { QStringList{ "-X", "-q", "-0", archive, "mimetype" }, members }) {
        zip.start(QStringLiteral(KOBALT_TEST_ZIP), arguments);
        if (!zip.waitForFinished(10000) || zip.exitStatus() != QProcess::NormalExit
            || zip.exitCode() != 0) {
            qCritical() << "Cannot create EPUB fixture:" << zip.errorString()
                        << zip.readAllStandardError();
            return false;
        }
    }
    return true;
}

// Preserve the content used by the restored search, selection and link checks.
// General reading and screenshots use the checked-in Type Tester instead.
inline QMap<QString, QByteArray> lookupBook()
{
    return {
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0' unique-identifier='id'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>kobalt-dictionary-fixture</dc:identifier><dc:title>Dictionary selection fixture</dc:title><dc:language>en</dc:language></metadata><manifest><item id='one' href='one.xhtml' media-type='application/xhtml+xml'/><item id='two' href='two.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='one'/><itemref idref='two'/></spine></package>" },
        { "one.xhtml",
          QByteArray(
                  "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Lookup fixture</title></head><body>"
                  "<p id='plain'>Hello, world! “Reader’s” don't café café naïve.</p>"
                  "<p id='inline'>A rain<strong>bow</strong> and co<em>oper</em>ation.</p>"
                  "<p id='soft'>An extra\xc2\xadordinary word can wrap across lines.</p>"
                  "<p id='multilingual'>Astral A𐐀word. Bonjour. 日本語の言葉。中文词语。</p>"
                  "<p id='annotation-punctuation'><span>“</span>quoted<span>…”</span> [bracket], adjacent. Next <em>𐐀token</em>。</p>"
                  "<p id='annotation-sentences'>Al<span>pha</span> finishes clearly. Beta completes fully! “Quotation ending.” (Nested closing?) Dr. Smith waits. Value 3.14 remains. 東京。 Ends with ellipsis… “Earlier completed.” Later rests.” We're arriving.' <span>(“</span>Layered pairing<span>.</span><span>”)</span> “Greeting,” she murmured softly. “First one. Second two.”</p>")
                  + QByteArray("<p>More fixture text.</p>").repeated(80) + "</body></html>" },
        { "two.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Lookup fixture</title></head><body><p id='boundary'>Chapter boundary. Hello again.</p></body></html>" }
    };
}

inline QMap<QString, QByteArray> linkBook()
{
    QByteArray notes =
            "<aside id='short' epub:type='footnote'><p>A <strong>short</strong> note with café &amp; tea.</p>"
            "<p>Second paragraph of the same note.</p></aside><aside id='long' role='doc-footnote'>";
    for (int i = 0; i < 60; ++i)
        notes += "<p>Note paragraph " + QByteArray::number(i)
                + ". Explicit paging keeps this text readable without leaving the original passage.</p>";
    notes +=
            "<p>End of the long note.</p></aside><p class='footnote'><a id='legacy'></a>A legacy note inside one paragraph.</p>"
            "<aside id='oversize' epub:type='footnote' style='display:none'><p>";
    notes += QByteArray("Long note text. ").repeated(10000);
    notes +=
            "</p></aside><aside id='hidden' epub:type='footnote' style='display:none'><p>A hidden note remains available in its popup.</p></aside>";
    const QMap<QString, QByteArray> pages{
        { "first.xhtml",
          "<p>Read a <a id='short-ref' ops:type='noteref' href='Notes/foot%20notes.xhtml#short'>short note</a>.</p>"
          "<p>Read a <a role='doc-noteref' href='Notes/foot%20notes.xhtml#long'>long note</a>.</p>"
          "<p>Visit a <a href='second.xhtml#destination'>later passage</a>.</p>"
          "<p>Jump to a <a href='#same-page'>nearby passage</a>.</p>"
          "<p>Try a <a href='second.xhtml#missing'>missing passage</a>.</p>"
          "<p>See a <a href='https://example.org/book'>website</a>.</p>"
          "<p>Reject an <a href='javascript:alert(1)'>unsupported link</a>.</p>"
          "<p>Read a <a href='Notes/foot%20notes.xhtml#legacy'>legacy note</a>.</p>"
          "<p>Read an <a epub:type='noteref' href='Notes/foot%20notes.xhtml#oversize'>oversized note</a>.</p>"
          "<p>Read a <a epub:type='noteref' href='Notes/foot%20notes.xhtml#hidden'>hidden note</a>.</p>"
          "<p id='same-page'>The nearby passage stays in the same chapter.</p>" },
        { "second.xhtml",
          QByteArray(
                  "<p id='destination'>The destination passage. <a style='color:#cc3300' href='first.xhtml#short-ref'>Return through a link</a>.</p>")
                  + QByteArray("<p>More destination text.</p>").repeated(40) },
        { "Notes/foot notes.xhtml", notes }
    };
    QMap<QString, QByteArray> files{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles><rootfile full-path='OEBPS/book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "OEBPS/book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>kobalt-link-fixture</dc:identifier><dc:title>Links and footnotes</dc:title><dc:language>en</dc:language></metadata><manifest><item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/><item id='first' href='first.xhtml' media-type='application/xhtml+xml'/><item id='second' href='second.xhtml' media-type='application/xhtml+xml'/><item id='notes' href='Notes/foot%20notes.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='first'/><itemref idref='second'/><itemref idref='notes'/></spine></package>" },
        { "OEBPS/nav.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'><head><title>Contents</title></head><body><nav epub:type='toc'><ol><li><a href='first.xhtml'>References</a></li><li><a href='second.xhtml'>Passages</a></li><li><a href='Notes/foot%20notes.xhtml'>Notes</a></li></ol></nav></body></html>" }
    };
    for (auto it = pages.cbegin(); it != pages.cend(); ++it)
        files["OEBPS/" + it.key()] =
                "<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops' xmlns:ops='http://www.idpf.org/2007/ops'><head><title>Links</title></head><body>"
                + it.value() + "</body></html>";
    return files;
}
} // namespace kobalt::test
