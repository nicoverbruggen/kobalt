#include "support/qt_compat.h"
#include "support/strings.h"
#include "layout_compatibility.h"
#include <QElapsedTimer>
#include <QSet>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>
#include <QTextStream>

namespace kobalt {
const char *LayoutCompatibility::reasonKey() const
{
    switch (verdict) {
    case Supported:
        return "";
    case VerticalWriting:
        return strings::key("error.publication_declares_vertical_writing");
    case RightToLeftProgression:
        return strings::key("error.publication_requires_right_to_left_page_navigation");
    case Incomplete:
        return strings::key("error.publication_layout_inspection_is_incomplete");
    }
    return strings::key("error.publication_layout_inspection_is_incomplete");
}

QString LayoutCompatibility::reason() const
{
    return verdict == Supported ? QString() : strings::get(reasonKey());
}

namespace {
constexpr qint64 resourceLimit = 2 * 1024 * 1024, totalLimit = 8 * 1024 * 1024;
constexpr int resourceCount = 512, timeLimit = 15000;

QString xmlSource(QByteArray bytes)
{
    // Match XML's UTF-16 detection even when the document has no BOM.
    if (bytes.startsWith(QByteArray::fromHex("003c"))) {
        bytes.prepend(QByteArray::fromHex("feff"));
    } else if (bytes.startsWith(QByteArray::fromHex("3c00"))) {
        bytes.prepend(QByteArray::fromHex("fffe"));
    }
    QTextStream stream(&bytes, QIODevice::ReadOnly);
    static const QRegularExpression declaration(
            QStringLiteral("^<\\?xml\\s[^>]*encoding\\s*=\\s*['\"]([^'\"]+)['\"]"));
    const auto encoding = declaration.match(QString::fromLatin1(bytes.left(512))).captured(1);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec(encoding.isEmpty() ? "UTF-8" : encoding.toLatin1().constData());
#else
    if (const auto codec = QStringConverter::encodingForName(encoding.toLatin1().constData())) {
        stream.setEncoding(*codec);
    }
#endif
    return stream.readAll();
}

QString boundedDiagnostic(QString text, int limit)
{
    text.truncate(limit);
    for (int i = 0; i < text.size(); ++i) {
        const auto character = text.at(i);
        if (character.category() == QChar::Other_Control && character != '\n'
            && character != '\t') {
            text[i] = QChar(0xfffd);
        }
    }
    return text;
}

struct Token
{
    QString value;
    int offset = 0;
    bool identifier = false, string = false;
};

// Preserve strings and comments as tokens so examples in generated content do
// not look like declarations. CSS escapes also apply to property names/URLs.
bool tokenize(const QString &css, QVector<Token> &tokens, int &i, QString &error)
{
    i = 0;
    auto escape = [&](QString &out) {
        if (++i >= css.size()) {
            error = QStringLiteral("CSS ends inside an escape");
            return false;
        }
        uint code = 0;
        int digits = 0;
        while (i < css.size() && digits < 6) {
            const auto c = css[i].toLower().unicode();
            const int hex = c >= '0' && c <= '9' ? c - '0'
                    : c >= 'a' && c <= 'f'       ? c - 'a' + 10
                                                 : -1;
            if (hex < 0)
                break;
            code = code * 16 + uint(hex);
            ++i;
            ++digits;
        }
        if (digits) {
            if (i < css.size() && css[i].isSpace())
                ++i;
            if (!code || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                code = 0xfffd;
            out += kobalt::qt::character(char32_t(code));
        } else {
            out += css[i++];
        }
        return true;
    };
    while (i < css.size()) {
        if (tokens.size() >= 200000) {
            error = QStringLiteral("CSS exceeds the 200000 token inspection limit");
            return false;
        }
        if (css[i].isSpace()) {
            ++i;
            continue;
        }
        if (css.mid(i, 2) == "/*") {
            const int end = css.indexOf("*/", i + 2);
            if (end < 0) {
                error = QStringLiteral("Unterminated CSS comment");
                return false;
            }
            i = end + 2;
            continue;
        }
        Token token;
        token.offset = i;
        if (css[i] == '\'' || css[i] == '"') {
            const auto quote = css[i++];
            token.string = true;
            while (i < css.size() && css[i] != quote) {
                if (css[i] == '\\') {
                    if (!escape(token.value))
                        return false;
                } else
                    token.value += css[i++];
            }
            if (i == css.size()) {
                error = QStringLiteral("Unterminated CSS string");
                return false;
            }
            ++i;
        } else if (css[i].isLetterOrNumber() || css[i] == '-' || css[i] == '_' || css[i] == '\\') {
            token.identifier = true;
            while (i < css.size()
                   && (css[i].isLetterOrNumber() || css[i] == '-' || css[i] == '_'
                       || css[i] == '\\')) {
                if (css[i] == '\\') {
                    if (!escape(token.value))
                        return false;
                } else
                    token.value += css[i++];
            }
        } else
            token.value = css[i++];
        tokens.append(token);
    }
    return true;
}

class Inspection
{
public:
    ResourceCache &cache;
    const QMap<QString, qint64> &sizes;
    LayoutCompatibility result;
    QElapsedTimer clock;
    QSet<QString> seen;
    QVector<QPair<QString, bool>> pending;
    QString currentResource, currentSource;
    qint64 currentOffset = 0, hintOffset = 0;
    QString hintResource, hintSource;
    bool vertical = false, verticalHint = false, verticalStyle = false, complete = true;

    Inspection(ResourceCache &cache, const QMap<QString, qint64> &sizes)
        : cache(cache), sizes(sizes)
    {
        clock.start();
    }

    void issue(const QString &message)
    {
        if (!result.issue.isEmpty()) {
            return;
        }
        result.issue = boundedDiagnostic(message, 256);
        result.resource = boundedDiagnostic(currentResource, 256);
        const int offset = int(qBound(qint64(0), currentOffset, qint64(currentSource.size())));
        const int start = qMax(0, offset - 120);
        result.excerpt = boundedDiagnostic(currentSource.mid(start, 240), 240);
        if (!currentSource.isEmpty()) {
            const auto preceding = currentSource.left(offset);
            result.line = preceding.count('\n') + 1;
            result.column = offset - preceding.lastIndexOf('\n');
        }
    }

    void fail(const QString &message)
    {
        issue(message);
        complete = false;
    }

    void xmlError(const QXmlStreamReader &xml)
    {
        if (!xml.hasError()) {
            return;
        }
        currentOffset = xml.characterOffset();
        fail(xml.errorString());
        // Qt reports the parser's exact location, including XML encoding rules.
        if (result.resource == currentResource && result.issue == xml.errorString()) {
            result.line = xml.lineNumber();
            result.column = xml.columnNumber();
        }
    }

    QString resolve(const QString &base, const QString &reference)
    {
        const QUrl relative(reference);
        if (reference.isEmpty() || !relative.isValid() || !relative.scheme().isEmpty()
            || !relative.authority().isEmpty() || relative.hasQuery() || reference.contains('\\')) {
            fail(QStringLiteral("Invalid resource reference: %1").arg(reference));
            return {};
        }
        QUrl origin;
        origin.setScheme("https");
        origin.setHost("publication.invalid");
        origin.setPath('/' + base);
        const auto url = origin.resolved(relative).adjusted(QUrl::NormalizePathSegments);
        const auto name = url.path(QUrl::FullyDecoded).mid(1);
        if (!sizes.contains(name)) {
            fail(QStringLiteral("Referenced resource is missing: %1").arg(name));
            return {};
        }
        return name;
    }

    QByteArray read(const QString &name)
    {
        QString limit;
        if (name.isEmpty() || sizes.value(name, -1) < 0) {
            limit = QStringLiteral("Resource is absent from the publication directory");
        } else if (clock.elapsed() > timeLimit) {
            limit = QStringLiteral("Inspection exceeded its 15000 ms deadline");
        } else if (result.resources >= resourceCount) {
            limit = QStringLiteral("Inspection exceeded its 512 resource limit");
        } else if (sizes.value(name) > resourceLimit) {
            limit = QStringLiteral("Resource exceeds the 2 MiB inspection limit: %1 bytes")
                            .arg(sizes.value(name));
        } else if (sizes.value(name) > totalLimit - result.bytes || result.bytes >= totalLimit) {
            limit = QStringLiteral(
                            "Resource exceeds the remaining 8 MiB inspection budget: %1 bytes remaining")
                            .arg(totalLimit - result.bytes);
        }
        if (!limit.isEmpty()) {
            currentResource = name;
            currentSource.clear();
            fail(limit);
            return {};
        }
        ++result.resources;
        const auto data = cache.readBounded(name, qMin(resourceLimit, totalLimit - result.bytes));
        if (data.isNull()) {
            currentResource = name;
            currentSource.clear();
            switch (cache.failure()) {
            case ResourceCache::Failure::Limit:
                fail(QStringLiteral("Transferred resource exceeds the inspection byte limit"));
                break;
            case ResourceCache::Failure::Cancelled:
                fail(QStringLiteral("Resource read was cancelled"));
                break;
            case ResourceCache::Failure::Changed:
                fail(QStringLiteral("Resource bytes changed during inspection"));
                break;
            case ResourceCache::Failure::Transfer:
                fail(QStringLiteral("Resource transfer failed"));
                break;
            case ResourceCache::Failure::None:
                fail(QStringLiteral("Resource read failed"));
                break;
            }
        }
        result.bytes += data.size();
        return data;
    }

    void queue(const QString &base, const QString &href, bool css)
    {
        const auto name = resolve(base, href);
        if (name.isEmpty() || seen.contains(name))
            return;
        if (seen.size() >= resourceCount) {
            fail(QStringLiteral("Inspection resource count limit exceeded"));
            return;
        }
        seen.insert(name);
        pending.append({ name, css });
    }

    void mode(QString value, bool metadata = false, bool supportedProperty = true)
    {
        value = value.trimmed().toLower();
        if (value == "vertical-rl" && (metadata || supportedProperty)) {
            if (metadata) {
                verticalHint = true;
                hintResource = currentResource;
                hintSource = currentSource;
                hintOffset = currentOffset;
            } else {
                verticalStyle = true;
            }
            return;
        }
        if (QStringList{ "vertical-rl", "vertical-lr", "sideways-rl", "sideways-lr", "tb", "tb-rl",
                         "tb-lr" }
                    .contains(value)) {
            vertical = true;
            issue(QStringLiteral("Unsupported writing mode: %1").arg(value));
        } else if (metadata && value == "horizontal-rl")
            fail(QStringLiteral("Unsupported metadata writing mode: %1").arg(value));
        else if (!QStringList{ "horizontal-tb", "horizontal-lr", "lr", "lr-tb", "rl", "rl-tb",
                               "initial", "inherit", "unset", "revert", "revert-layer" }
                          .contains(value))
            fail(QStringLiteral("Unknown writing mode: %1").arg(value));
    }

    void css(const QString &base, const QString &source)
    {
        const auto savedResource = currentResource;
        const auto savedSource = currentSource;
        const auto savedOffset = currentOffset;
        currentResource = base;
        currentSource = source;
        int offset = 0;
        QVector<Token> tokens;
        QString error;
        if (!tokenize(source, tokens, offset, error)) {
            currentOffset = offset;
            fail(error);
            return;
        }
        for (int i = 0; i < tokens.size(); ++i) {
            const auto &token = tokens[i];
            currentOffset = token.offset;
            const QString name = token.value.toLower();
            if (token.identifier
                && (name == "writing-mode" || name == "-epub-writing-mode"
                    || name == "-webkit-writing-mode" || name == "-ms-writing-mode"
                    || name == "-moz-writing-mode" || name == "-o-writing-mode")
                && i + 2 < tokens.size() && tokens[i + 1].value == ":") {
                const auto &value = tokens[i + 2];
                if (!value.identifier || (i + 3 < tokens.size() && tokens[i + 3].value == "("))
                    fail(QStringLiteral("Writing mode cannot be resolved statically"));
                else
                    mode(value.value, false,
                         name == "writing-mode" || name == "-epub-writing-mode"
                                 || name == "-webkit-writing-mode");
            }
            // text-orientation (including prefixed forms) only changes glyphs
            // in vertical writing. It does not make horizontal text vertical.
            if (token.value != "@" || i + 1 >= tokens.size() || !tokens[i + 1].identifier)
                continue;
            const auto rule = tokens[i + 1].value.toLower();
            if (rule == "charset") {
                if (i + 2 >= tokens.size()
                    || tokens[i + 2].value.compare("utf-8", Qt::CaseInsensitive))
                    fail(QStringLiteral("Stylesheet charset is not UTF-8"));
            } else if (rule == "import") {
                int at = i + 2;
                if (at < tokens.size() && tokens[at].string)
                    queue(base, tokens[at].value, true);
                else if (at + 2 < tokens.size()
                         && tokens[at].value.compare("url", Qt::CaseInsensitive) == 0
                         && tokens[at + 1].value == "(") {
                    at += 2;
                    QString url;
                    while (at < tokens.size() && tokens[at].value != ")")
                        url += tokens[at++].value;
                    if (at == tokens.size())
                        fail(QStringLiteral("Unterminated CSS import URL"));
                    else
                        queue(base, url, true);
                } else
                    fail(QStringLiteral("Malformed CSS import"));
            }
        }
        currentResource = savedResource;
        currentSource = savedSource;
        currentOffset = savedOffset;
    }

    void document(const QString &path)
    {
        currentResource = path;
        const auto bytes = read(path);
        currentSource = xmlSource(bytes);
        currentOffset = 0;
        QXmlStreamReader xml(bytes);
        while (!xml.atEnd() && complete && !vertical) {
            xml.readNext();
            currentOffset = xml.characterOffset();
            if (xml.isProcessingInstruction()
                && xml.processingInstructionTarget().toString() == "xml-stylesheet")
                fail(QStringLiteral("XML stylesheet processing instruction is not supported"));
            if (!xml.isStartElement())
                continue;
            const auto name = xml.name().toString().toLower();
            const auto attrs = xml.attributes();
            // CREngine does not execute scripts. Their presence cannot establish
            // a vertical layout; inspect the publication's actual CSS instead.
            if (name == "script") {
                xml.skipCurrentElement();
                continue;
            }
            if (attrs.hasAttribute("http://www.w3.org/XML/1998/namespace", "base")
                || name == "base")
                fail(QStringLiteral("XML base changes resource resolution"));
            if (attrs.hasAttribute("style"))
                css(path, attrs.value("style").toString());
            for (const auto &a : attrs) {
                if (a.name().toString().compare("writing-mode", Qt::CaseInsensitive) == 0)
                    mode(a.value().toString(), false, false);
            }
            if (name == "style")
                css(path, xml.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement));
            // CREngine loads CSS links only. Adobe page templates and other
            // explicitly declared types cannot affect its layout.
            const auto stylesheetType = attrs.value("type").toString().toLower();
            if (name == "link" && (stylesheetType.isEmpty() || stylesheetType == "text/css")
                && attrs.value("rel")
                           .toString()
                           .toLower()
                           .split(QRegularExpression("\\s+"), kobalt::qt::skipEmptyParts)
                           .contains("stylesheet")) {
                queue(path, attrs.value("href").toString(), true);
            }
        }
        xmlError(xml);
    }

    LayoutCompatibility run()
    {
        currentResource = QStringLiteral("META-INF/container.xml");
        const auto containerBytes = read(currentResource);
        currentSource = xmlSource(containerBytes);
        QXmlStreamReader container(containerBytes);
        QString package;
        while (!container.atEnd()) {
            container.readNext();
            currentOffset = container.characterOffset();
            if (container.isStartElement() && container.name().toString() == "rootfile") {
                if (!package.isEmpty()) {
                    fail(QStringLiteral("Multiple package rootfiles"));
                    break;
                }
                package = resolve({}, container.attributes().value("full-path").toString());
            }
        }
        xmlError(container);
        if (package.isEmpty())
            fail(QStringLiteral("Invalid container or missing package rootfile"));
        QMap<QString, QString> manifest;
        QMap<QString, QMap<QString, QString>> manifestAttributes;
        QStringList spine;
        currentResource = package;
        const auto packageBytes = complete ? read(package) : QByteArray();
        currentSource = xmlSource(packageBytes);
        currentOffset = 0;
        QXmlStreamReader opf(packageBytes);
        while (!opf.atEnd() && complete) {
            opf.readNext();
            currentOffset = opf.characterOffset();
            if (!opf.isStartElement())
                continue;
            const auto name = opf.name().toString();
            const auto a = opf.attributes();
            if (a.hasAttribute("http://www.w3.org/XML/1998/namespace", "base"))
                fail(QStringLiteral("XML base changes package resource resolution"));
            if (name == "meta") {
                const auto key = a.value("name").isEmpty() ? a.value("property").toString()
                                                           : a.value("name").toString();
                if (key.contains("writing-mode", Qt::CaseInsensitive))
                    mode(a.hasAttribute("content") ? a.value("content").toString()
                                                   : opf.readElementText(),
                         true);
            } else if (name == "item") {
                const auto id = a.value("id").toString();
                const auto media = a.value("media-type").toString();
                QMap<QString, QString> attributes;
                for (const auto &attribute : a) {
                    attributes[attribute.namespaceUri().toString() + QChar(0)
                               + attribute.name().toString()] = attribute.value().toString();
                }
                if (id.isEmpty()) {
                    fail(QStringLiteral("Manifest item has no ID"));
                    break;
                }
                if (manifest.contains(id)) {
                    // Repeated identical items do not change resource lookup or
                    // layout. Conflicting declarations remain ambiguous.
                    if (manifestAttributes.value(id) != attributes) {
                        fail(QStringLiteral("Conflicting manifest item ID: %1").arg(id));
                        break;
                    }
                    continue;
                }
                manifestAttributes.insert(id, attributes);
                manifest[id] = a.value("href").toString();
                // Include non-linear and linked reading sections, not just the
                // first chapter. Fonts, images and audio are never requested.
                if (media == "application/xhtml+xml" || media == "text/html")
                    queue(package, manifest[id], false);
            } else if (name == "spine") {
                const auto direction = a.value("page-progression-direction").toString();
                if (!direction.isEmpty() && direction != "rtl" && direction != "ltr"
                    && direction != "default")
                    fail(QStringLiteral("Unknown page progression: %1").arg(direction));
            } else if (name == "itemref")
                spine.append(a.value("idref").toString());
        }
        if (complete) {
            xmlError(opf);
        }
        if (spine.isEmpty() || pending.isEmpty())
            fail(QStringLiteral("Invalid package or missing reading sections"));
        for (const auto &id : spine) {
            if (!manifest.contains(id))
                fail(QStringLiteral("Spine refers to an unknown manifest ID: %1").arg(id));
            else {
                const auto path = resolve(package, manifest[id]);
                if (!seen.contains(path))
                    fail(QStringLiteral("Spine item is not a reading section: %1").arg(id));
            }
        }
        for (int i = 0; i < pending.size() && complete && !vertical; ++i) {
            const auto entry = pending[i];
            if (entry.second) {
                currentResource = entry.first;
                currentOffset = 0;
                const auto bytes = read(entry.first);
                const auto utf8 = bytes.startsWith("\xef\xbb\xbf") ? bytes.mid(3) : bytes;
                const auto text = QString::fromUtf8(utf8);
                currentSource = text;
                if (text.toUtf8() != utf8)
                    fail(QStringLiteral("Stylesheet is not valid UTF-8"));
                else
                    css(entry.first, text);
            } else
                document(entry.first);
        }
        result.transientFailure = clock.elapsed() > timeLimit
                || (!cache.error().isEmpty() && cache.failure() != ResourceCache::Failure::Limit);
        if (result.transientFailure)
            fail(QStringLiteral("Inspection interrupted or resource transport failed"));
        result.verdict = (vertical || (verticalHint && !verticalStyle))
                ? LayoutCompatibility::VerticalWriting
                : complete ? LayoutCompatibility::Supported
                           : LayoutCompatibility::Incomplete;
        if (result.verdict == LayoutCompatibility::VerticalWriting && result.issue.isEmpty()) {
            currentResource = hintResource;
            currentSource = hintSource;
            currentOffset = hintOffset;
            issue(QStringLiteral("Vertical metadata has no supported vertical stylesheet"));
        }
        if (result.verdict == LayoutCompatibility::Supported) {
            result.issue.clear();
            result.resource.clear();
            result.excerpt.clear();
            result.line = result.column = 0;
        }
        return result;
    }
};
} // namespace

LayoutCompatibility inspectLayout(ResourceCache &resources, const QMap<QString, qint64> &sizes)
{
    return Inspection(resources, sizes).run();
}
} // namespace kobalt
