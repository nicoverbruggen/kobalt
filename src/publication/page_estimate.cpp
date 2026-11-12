#include "page_estimate.h"
#include <QSet>
#include <QString>
#include <QXmlStreamReader>
#include <cmath>

namespace kobalt {
namespace {
// The advance of one character in units of the font size. These are typical
// values for a book face, not measurements of the reading font.
float advance(char32_t c)
{
    if (c == U' ')
        return 0.26f;
    if (c < 0x80) {
        switch (c) {
        case 'i':
        case 'j':
        case 'l':
        case 'I':
        case 'f':
        case 't':
        case 'r':
        case '.':
        case ',':
        case ';':
        case ':':
        case '\'':
        case '!':
        case '|':
        case '`':
        case '(':
        case ')':
        case '[':
        case ']':
            return 0.3f;
        case 'm':
        case 'w':
        case 'M':
        case 'W':
        case '@':
            return 0.85f;
        default:
            break;
        }
        if (c >= 'A' && c <= 'Z')
            return 0.68f;
        if (c >= '0' && c <= '9')
            return 0.52f;
        return 0.5f;
    }
    if (c == 0x2014)
        return 1.0f;
    if (c == 0x2018 || c == 0x2019)
        return 0.3f;
    // CJK and other full-width scripts.
    if (c >= 0x2E80 && c <= 0xD7FF)
        return 1.0f;
    if (c >= 0xF900 && c <= 0xFAFF)
        return 1.0f;
    if (c >= 0xFF00 && c <= 0xFFEF)
        return 1.0f;
    if (c >= 0x20000)
        return 1.0f;
    return 0.55f;
}

bool whitespace(QChar c)
{
    const auto unit = c.unicode();
    return unit == ' ' || unit == '\t' || unit == '\n' || unit == '\r' || unit == '\f';
}

float headingScale(const QString &name)
{
    if (name.size() == 2 && name[0] == QLatin1Char('h')) {
        switch (name[1].unicode()) {
        case '1':
            return 1.6f;
        case '2':
            return 1.4f;
        case '3':
            return 1.2f;
        default:
            return 1;
        }
    }
    return 1;
}

const QSet<QString> &blockNames()
{
    static const QSet<QString> names{
        "p",       "div",     "h1",     "h2",     "h3",      "h4",   "h5",     "h6",
        "li",      "ul",      "ol",     "dl",     "dt",      "dd",   "pre",    "blockquote",
        "section", "article", "header", "footer", "aside",   "nav",  "figure", "figcaption",
        "table",   "tr",      "td",     "th",     "caption", "body", "hgroup", "address"
    };
    return names;
}

bool indents(const QString &name)
{
    return name == QLatin1String("blockquote") || name == QLatin1String("ul")
            || name == QLatin1String("ol") || name == QLatin1String("dd");
}

bool skipped(const QString &name)
{
    return name == QLatin1String("head") || name == QLatin1String("script")
            || name == QLatin1String("style") || name == QLatin1String("title");
}
} // namespace

PageEstimateItem measurePageEstimate(const QByteArray &xhtml)
{
    PageEstimateItem item;
    QXmlStreamReader parser(xhtml);
    // The text of the block that is open, as its width so far.
    float width = 0;
    bool content = false, pendingSpace = false;
    QVector<float> scales{ 1 };
    int depth = 0, skip = 0;
    // Ends the open text. `kind` says what ended it. Text before a line break
    // gets no paragraph gap. A line break without text is an empty line.
    const auto flush = [&](PageEstimateBlock::Kind kind) {
        if (content || kind != PageEstimateBlock::Text) {
            PageEstimateBlock block;
            block.width = width;
            block.scale = scales.last();
            block.depth = quint8(qBound(0, depth, 255));
            block.kind = content && kind != PageEstimateBlock::LineBreak ? PageEstimateBlock::Text
                                                                         : kind;
            item.blocks.append(block);
        }
        width = 0;
        content = pendingSpace = false;
    };
    while (!parser.atEnd()) {
        parser.readNext();
        if (parser.isStartElement()) {
            const QString name = parser.name().toString();
            if (skip || skipped(name)) {
                ++skip;
                continue;
            }
            if (name == QLatin1String("br")) {
                flush(PageEstimateBlock::LineBreak);
            } else if (name == QLatin1String("hr")) {
                flush(PageEstimateBlock::Text);
                flush(PageEstimateBlock::Rule);
            } else if (name == QLatin1String("img") || name == QLatin1String("svg")
                       || name == QLatin1String("image")) {
                flush(PageEstimateBlock::Text);
                flush(PageEstimateBlock::Image);
            } else if (blockNames().contains(name)) {
                flush(PageEstimateBlock::Text);
                scales.append(scales.last() * headingScale(name));
                depth += indents(name);
            }
        } else if (parser.isEndElement()) {
            const QString name = parser.name().toString();
            if (skip) {
                --skip;
                continue;
            }
            if (blockNames().contains(name)) {
                flush(PageEstimateBlock::Text);
                if (scales.size() > 1)
                    scales.removeLast();
                depth -= indents(name);
            }
        } else if (parser.isCharacters() && !skip) {
            const QString text = parser.text().toString();
            const float scale = scales.last();
            for (int i = 0; i < text.size(); ++i) {
                const QChar unit = text[i];
                if (whitespace(unit)) {
                    pendingSpace = content;
                    continue;
                }
                char32_t c = unit.unicode();
                if (unit.isHighSurrogate() && i + 1 < text.size())
                    c = QChar::surrogateToUcs4(unit, text[++i]);
                if (pendingSpace)
                    width += advance(U' ') * scale;
                pendingSpace = false;
                width += advance(c) * scale;
                content = true;
                ++item.characters;
            }
        }
    }
    flush(PageEstimateBlock::Text);
    item.parsed = !parser.hasError();
    return item;
}

double estimatePages(const PageEstimateItem &item, const PageEstimateLayout &layout)
{
    if (layout.fontSize <= 0 || layout.lineHeight <= 0 || layout.width <= 0 || layout.height <= 0)
        return 0;
    double height = 0;
    for (const auto &block : item.blocks) {
        const double line = layout.lineHeight * block.scale;
        switch (block.kind) {
        case PageEstimateBlock::Image:
            // The size of an image is not known without its file. Half a
            // page is between an ornament and a full plate.
            height += layout.height / 2;
            continue;
        case PageEstimateBlock::Rule:
            height += line;
            continue;
        case PageEstimateBlock::LineBreak:
        case PageEstimateBlock::Text:
            break;
        }
        const double available =
                qMax(layout.fontSize * 4, layout.width - block.depth * 2 * layout.fontSize);
        const double text = block.width * layout.fontSize + layout.indent;
        height += qMax(1.0, std::ceil(text / available)) * line;
        if (block.kind == PageEstimateBlock::Text)
            height += layout.paragraphGap;
    }
    return qMax(0.05, height / layout.height);
}
} // namespace kobalt
