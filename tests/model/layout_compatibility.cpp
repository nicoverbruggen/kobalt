#include "publication/layout_compatibility.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <cstdio>
using namespace kobalt;
using Files = QMap<QString, QByteArray>;

static Files publication(const QByteArray &body = "<p>Horizontal text.</p>",
                         const QByteArray &head = {}, const QByteArray &meta = {},
                         const QByteArray &spine = {})
{
    return {
        { "META-INF/container.xml",
          "<container><rootfiles><rootfile full-path='OPS/book.opf'/></rootfiles></container>" },
        { "OPS/book.opf",
          "<package><metadata>" + meta
                  + "</metadata><manifest><item id='c' href='text/chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine "
                  + spine + "><itemref idref='c'/></spine></package>" },
        { "OPS/text/chapter.xhtml",
          "<html xmlns='http://www.w3.org/1999/xhtml'><head>" + head + "</head><body>" + body
                  + "</body></html>" }
    };
}

static LayoutCompatibility inspect(Files files, QStringList *requested = nullptr,
                                   QMap<QString, qint64> sizes = {})
{
    for (auto i = files.cbegin(); i != files.cend(); ++i)
        if (!sizes.contains(i.key()))
            sizes[i.key()] = i->size();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    ResourceCache cache(
            files.keys(),
            [files, requested](const QString &name, const ResourceCache::Sink &sink, QString *) {
                if (requested)
                    requested->append(name);
                const auto bytes = files.value(name);
                for (int at = 0; at < bytes.size(); at += 4096)
                    if (!sink(bytes.constData() + at, qMin(4096, int(bytes.size()) - at)))
                        return false;
                return true;
            },
            cancelled);
    return inspectLayout(cache, sizes);
}

static void expect(const Files &files, LayoutCompatibility::Verdict verdict, const char *message)
{
    if (inspect(files).verdict != verdict)
        qFatal("%s", message);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2) {
        Files files;
        QDir root(QString::fromLocal8Bit(argv[1]));
        std::function<void(QDir)> visit = [&](QDir dir) {
            for (const auto &info :
                 dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (info.isDir())
                    visit(QDir(info.filePath()));
                else {
                    QFile file(info.filePath());
                    if (!file.open(QIODevice::ReadOnly))
                        qFatal("fixture read");
                    files[root.relativeFilePath(info.filePath())] = file.readAll();
                }
            }
        };
        visit(root);
        const auto result = inspect(files);
        std::printf("%d %d %lld\n", int(result.verdict), result.resources,
                    static_cast<long long>(result.bytes));
        return 0;
    }
    using L = LayoutCompatibility;
    for (const auto *lang : { "ja", "zh", "ko", "en", "" }) {
        const auto meta = QByteArray("<language>") + lang + "</language>";
        expect(publication("<p>日本語 中文 한국어 ordinary text</p>", {}, meta), L::Supported,
               "Language must not decide compatibility");
    }
    for (const auto *property : { "writing-mode", "-epub-writing-mode", "-webkit-writing-mode",
                                  "-ms-writing-mode", "-moz-writing-mode", "-o-writing-mode" }) {
        for (const auto *value :
             { "vertical-rl", "vertical-lr", "sideways-rl", "tb-rl", "tb-lr" }) {
            const auto declaration = QByteArray(property) + ":" + value;
            const auto verdict = QByteArray(value) == "vertical-rl"
                            && (QByteArray(property) == "writing-mode"
                                || QByteArray(property) == "-epub-writing-mode"
                                || QByteArray(property) == "-webkit-writing-mode")
                    ? L::Supported
                    : L::VerticalWriting;
            expect(publication("<p style='" + declaration + "'>Text</p>"), verdict,
                   "Inline writing-mode policy mismatch");
            auto files = publication({}, "<link rel='stylesheet' href='../css/main.css'/>");
            files["OPS/css/main.css"] = "body {" + declaration + "}";
            expect(files, verdict, "External writing-mode policy mismatch");
        }
    }
    expect(publication({}, "<style>body{WRITING-MODE:VeRtIcAl-Rl!important}</style>"), L::Supported,
           "Case folding failed");
    expect(publication({}, "<style>body{writ\\69 ng-mode:vert\\69 cal-rl}</style>"), L::Supported,
           "CSS escapes missed");
    expect(publication(
                   {},
                   "<style>/* writing-mode:vertical-rl */p:before{content:'writing-mode:vertical-rl'}</style>"),
           L::Supported, "Comments or strings treated as layout");
    expect(publication(
                   {},
                   "<style>p{text-orientation:upright;-webkit-text-orientation:sideways;writing-mode:horizontal-tb}</style>"),
           L::Supported, "Orientation alone is not vertical writing");
    expect(publication({}, {}, {}, "page-progression-direction='rtl'"), L::Supported,
           "RTL spine is supported");
    expect(publication({}, {}, {}, "page-progression-direction='ltr'"), L::Supported,
           "LTR rejected");
    expect(publication({}, {}, "<meta name='primary-writing-mode' content='vertical-rl'/>"),
           L::VerticalWriting, "Legacy package metadata missed");
    expect(publication({}, "<style>body{writing-mode:vertical-rl}</style>",
                       "<meta name='primary-writing-mode' content='vertical-rl'/>"),
           L::Supported, "Vertical metadata must not reject a book with supported CSS");
    expect(publication({}, {}, "<meta property='rendition:writing-mode'>vertical-lr</meta>"),
           L::VerticalWriting, "Property metadata missed");
    expect(publication(
                   {},
                   "<style>body{writing-mode:vertical-rl}body{writing-mode:horizontal-tb}</style>"),
           L::Supported, "First-release policy must not guess the cascade");
    expect(publication({}, "<style>@media print{.unused{writing-mode:vertical-rl}}</style>"),
           L::Supported, "Conditional styles must be inspected conservatively");
    expect(publication({}, "<style>body{writing-mode:var(--mode)}</style>"), L::Incomplete,
           "Variable mode must not pass as horizontal");
    auto mixed = publication();
    mixed["OPS/book.opf"].replace(
            "</manifest>",
            "<item id='v' href='text/vertical.xhtml' media-type='application/xhtml+xml'/></manifest>");
    mixed["OPS/book.opf"].replace("</spine>", "<itemref idref='v' linear='no'/></spine>");
    mixed["OPS/text/vertical.xhtml"] =
            "<html><body style='writing-mode:vertical-rl'>Text</body></html>";
    expect(mixed, L::Supported, "Mixed or non-linear sections missed");
    auto imports = publication({}, "<link rel='stylesheet' href='../css/one.css'/>");
    imports["OPS/css/one.css"] = "@import url('two.css') screen;";
    imports["OPS/css/two.css"] = "@import 'one.css';p{writing-mode:vertical-lr}";
    expect(imports, L::VerticalWriting, "Imported CSS or cycle handling failed");
    imports["OPS/css/two.css"] = "@import 'one.css';p{writing-mode:horizontal-tb}";
    expect(imports, L::Supported, "Horizontal CSS cycle should terminate");
    imports.remove("OPS/css/two.css");
    expect(imports, L::Incomplete, "Missing imported CSS passed");
    for (const auto *type :
         { "application/adobe-page-template+xml", "application/vnd.adobe-page-template+xml",
           "APPLICATION/ADOBE-PAGE-TEMPLATE+XML" }) {
        auto templates = publication({},
                                     QByteArray("<link rel='stylesheet' type='") + type
                                             + "' href='../styles/page-template.xpgt'/>");
        expect(templates, L::Supported, "Missing Adobe page template caused a formatting warning");
        templates["OPS/styles/page-template.xpgt"] = "not CSS: writing-mode:vertical-lr";
        QStringList requests;
        if (inspect(templates, &requests).verdict != L::Supported
            || requests.contains("OPS/styles/page-template.xpgt")) {
            qFatal("Adobe page template was inspected as CSS");
        }
        templates["OPS/text/chapter.xhtml"].replace(
                "</head>",
                "<link rel='stylesheet' type='text/css' href='../css/missing.css'/></head>");
        expect(templates, L::Incomplete, "Adobe page template concealed missing CSS");
    }
    expect(publication(
                   {},
                   "<link rel='stylesheet' type='text/css' href='../styles/page-template.xpgt'/>"),
           L::Incomplete, "Filename extension concealed a missing CSS stylesheet");
    expect(publication({}, "<link rel='stylesheet' href='https://example.invalid/style.css'/>"),
           L::Incomplete, "Remote CSS must not be fetched");
    const QByteArray script =
            "<script type='text/javascript'><![CDATA[document.body.style.writingMode='vertical-rl';]]></script>";
    expect(publication({}, script), L::Supported, "Non-executed script rejected horizontal layout");
    expect(publication("<p style='writing-mode:vertical-rl'>Text</p>", script), L::Supported,
           "Script concealed vertical body layout");
    auto scripted = publication(
            {}, "<script src='../reader.js'/><link rel='stylesheet' href='../css/main.css'/>");
    scripted["OPS/css/main.css"] = "body{writing-mode:horizontal-tb}";
    QStringList scriptRequests;
    if (inspect(scripted, &scriptRequests).verdict != L::Supported
        || scriptRequests.contains("OPS/reader.js"))
        qFatal("External script fetched or rejected");
    scripted["OPS/css/main.css"] = "body{writing-mode:vertical-rl}";
    expect(scripted, L::Supported, "Script concealed linked vertical CSS");
    scripted.remove("OPS/css/main.css");
    expect(scripted, L::Incomplete, "Script concealed missing stylesheet");
    auto brokenScript = publication({}, "<script>");
    expect(brokenScript, L::Incomplete, "Unterminated script concealed malformed XML");
    auto assets = publication();
    assets["OPS/huge.png"] = "not an image";
    assets["OPS/book.opf"].replace(
            "</manifest>",
            "<item id='img' href='huge.png' media-type='image/png'/><item id='font' href='missing.ttf' media-type='font/ttf'/></manifest>");
    QStringList requested;
    if (inspect(assets, &requested).verdict != L::Supported || requested.contains("OPS/huge.png"))
        qFatal("Assets fetched during layout inspection");
    {
        auto duplicate = publication();
        const QByteArray item =
                "<item id='c' href='text/chapter.xhtml' media-type='application/xhtml+xml'/>";
        duplicate["OPS/book.opf"].replace("</manifest>", item + "</manifest>");
        QStringList requests;
        if (inspect(duplicate, &requests).verdict != L::Supported
            || requests.count("OPS/text/chapter.xhtml") != 1)
            qFatal("Identical duplicate manifest item rejected or inspected twice");
        for (const auto &conflict : {
                     QByteArray(
                             "<item id='c' href='text/other.xhtml' media-type='application/xhtml+xml'/>"),
                     QByteArray("<item id='c' href='text/chapter.xhtml' media-type='image/png'/>"),
                     QByteArray(
                             "<item id='c' href='text/chapter.xhtml' media-type='application/xhtml+xml' properties='nav'/>"),
                     QByteArray(
                             "<item id='c' href='text/chapter.xhtml' media-type='application/xhtml+xml' fallback='other'/>"),
             }) {
            auto ambiguous = publication();
            ambiguous["OPS/book.opf"].replace("</manifest>", conflict + "</manifest>");
            expect(ambiguous, L::Incomplete, "Conflicting duplicate manifest item passed");
        }
        duplicate["OPS/text/chapter.xhtml"] =
                "<html><body style='writing-mode:vertical-lr'>Text</body></html>";
        expect(duplicate, L::VerticalWriting,
               "Duplicate manifest item concealed unsupported writing mode");
        auto image = publication();
        const QByteArray imageItem = "<item id='image' href='image.jpg' media-type='image/jpeg'/>";
        image["OPS/book.opf"].replace("</manifest>", imageItem + imageItem + "</manifest>");
        expect(image, L::Supported, "Identical duplicate image manifest item rejected");
    }
    {
        const auto broken = publication("<p>" + QByteArray(1000, 'x') + "\n<p>&nbsp;</p>");
        const auto failure = inspect(broken);
        if (failure.verdict != L::Incomplete || failure.resource != "OPS/text/chapter.xhtml"
            || !failure.issue.contains("nbsp") || failure.line != 2 || failure.column <= 0
            || !failure.excerpt.contains("&nbsp;") || failure.excerpt.size() > 240) {
            qFatal("XML failure lost its exact parser location or bounded excerpt");
        }
        for (const bool bom : { false, true }) {
            auto utf16 = publication();
            const QString text = QStringLiteral("<html><body><p>Text &nbsp;</p></body></html>");
            QByteArray encoded = bom ? QByteArray::fromHex("fffe") : QByteArray();
            for (const auto c : text) {
                encoded.append(char(c.unicode() & 0xff));
                encoded.append(char(c.unicode() >> 8));
            }
            utf16["OPS/text/chapter.xhtml"] = encoded;
            const auto failure = inspect(utf16);
            if (!failure.issue.contains("nbsp") || !failure.excerpt.contains("&nbsp;")
                || failure.excerpt.contains(QChar(0))) {
                qFatal("UTF-16 XML diagnostics lost the decoded failure excerpt");
            }
        }
        const auto missing =
                inspect(publication({}, "<link rel='stylesheet' href='../css/missing.css'/>"));
        if (missing.resource != "OPS/text/chapter.xhtml"
            || missing.issue != "Referenced resource is missing: OPS/css/missing.css"
            || !missing.excerpt.contains("missing.css")) {
            qFatal("Missing stylesheet lost its reference and source context");
        }
        auto css = publication({}, "<link rel='stylesheet' href='../css/main.css'/>");
        css["OPS/css/main.css"] = QByteArray(1000, ' ') + "body{content:'unterminated";
        const auto malformedCss = inspect(css);
        if (malformedCss.resource != "OPS/css/main.css"
            || malformedCss.issue != "Unterminated CSS string"
            || !malformedCss.excerpt.contains("unterminated")
            || malformedCss.excerpt.size() > 240) {
            qFatal("Malformed CSS lost its resource and bounded excerpt");
        }
        const auto supported = inspect(publication());
        if (!supported.issue.isEmpty() || !supported.excerpt.isEmpty()) {
            qFatal("Supported book unexpectedly has issue diagnostics");
        }
    }
    auto large = publication();
    large["OPS/text/chapter.xhtml"] = QByteArray(3 * 1024 * 1024, 'x');
    expect(large, L::Incomplete, "Resource limit not enforced");
    if (inspect(large, nullptr, { { "OPS/text/chapter.xhtml", 100 } }).verdict != L::Incomplete)
        qFatal("Untrusted declared size bypassed transfer limit");
    if (inspect(large, nullptr, { { "OPS/text/chapter.xhtml", 100 } }).transientFailure)
        qFatal("Actual inspection-limit overrun was treated as retryable I/O");
    auto malformed = publication();
    malformed["OPS/text/chapter.xhtml"] = "<html><body>";
    expect(malformed, L::Incomplete, "Incomplete XML passed");
    if (inspect(malformed).transientFailure)
        qFatal("Malformed content was mistaken for a transient request failure");
    {
        auto css = publication({}, "<link rel='stylesheet' href='../css/main.css'/>");
        css["OPS/css/main.css"] = "body{color:black}";
        expect(css, L::Supported, "Valid UTF-8 CSS rejected");
        css["OPS/css/main.css"].prepend(QByteArray::fromHex("efbbbf"));
        expect(css, L::Supported, "UTF-8 BOM rejected valid CSS");
        css["OPS/css/main.css"].append(char(0xff));
        expect(css, L::Incomplete, "BOM concealed invalid UTF-8 CSS");
    }
    {
        QByteArray links;
        for (int i = 0; i < 5; ++i)
            links += "<link rel='stylesheet' href='../css/" + QByteArray::number(i) + ".css'/>";
        auto files = publication({}, links);
        for (int i = 0; i < 4; ++i)
            files["OPS/css/" + QString::number(i) + ".css"] = QByteArray(1800000, ' ');
        qint64 bytes = 0;
        for (const auto &data : files)
            bytes += data.size();
        const int remaining = 8 * 1024 * 1024 - bytes;
        files["OPS/css/4.css"] = QByteArray(remaining, ' ');
        const auto exact = inspect(files);
        if (exact.verdict != L::Supported || exact.bytes != 8 * 1024 * 1024)
            qFatal("Exact inspection budget rejected complete content");
        files["OPS/css/4.css"].append(' ');
        const auto over = inspect(files);
        if (over.verdict != L::Incomplete || over.transientFailure)
            qFatal("Declared inspection-budget overrun was treated as transient I/O");
    }
    {
        const auto files = publication();
        QMap<QString, qint64> sizes;
        for (auto i = files.cbegin(); i != files.cend(); ++i)
            sizes[i.key()] = i->size();
        ResourceCache unavailable(
                files.keys(),
                [](const QString &, const ResourceCache::Sink &, QString *error) {
                    *error = "Synthetic resource transport failure";
                    return false;
                },
                std::make_shared<std::atomic_bool>(false));
        const auto failed = inspectLayout(unavailable, sizes);
        if (failed.verdict != L::Incomplete || !failed.transientFailure)
            qFatal("Resource failure was mistaken for a stable book rejection");
    }
    std::puts(
            "PASS: layout compatibility, languages, prefixes, imports, mixed sections, conservative overrides and resource bounds");
}
