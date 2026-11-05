#include "engine/crengine/column_view.h"
#include "engine_positions.h"
#include "engine/crengine/engine_document.h"
#include "support/strings.h"
#include <lvdocview.h>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QUrl>

namespace kobalt::nickel {
namespace {
QString qstring(const lString32 &s)
{
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}

lString32 cre(const QString &s)
{
    return Utf8ToUnicode(s.toUtf8().constData());
}

// A chapter's source XML as Nickel's point() locations see it: every child
// counts, including text and comments. Text is in UTF-16 units.
struct Source
{
    struct Node
    {
        QString id, name;
        QVector<int> children;
        int begin = 0, end = 0, parent = -1;
        bool text = false, cdata = false, element = false;
    };

    QVector<Node> nodes;
    QString text;
    int root = -1;

    int body() const
    {
        for (const int child : nodes[root].children)
            if (nodes[child].element && nodes[child].name == QLatin1String("body"))
                return child;
        return -1;
    }

    QString textOf(int node) const
    {
        return text.mid(nodes[node].begin, nodes[node].end - nodes[node].begin);
    }
};

bool readSource(ldomDocument *doc, const QString &path, Source &source)
{
    auto stream = doc->getContainer()->OpenStream(cre(path).c_str(), LVOM_READ);
    if (stream.isNull() || stream->GetSize() > 16 * 1024 * 1024)
        return false;
    QByteArray xml(int(stream->GetSize()), '\0');
    lvsize_t read = 0;
    if (stream->Read(xml.data(), xml.size(), &read) != LVERR_OK || read != lvsize_t(xml.size()))
        return false;
    auto &nodes = source.nodes;
    auto &text = source.text;
    nodes = QVector<Source::Node>(1);
    text.clear();
    source.root = -1;
    QVector<int> stack{ 0 };
    bool adjacentText = false;
    QXmlStreamReader parser(xml);
    while (!parser.atEnd()) {
        parser.readNext();
        if (!parser.isCharacters())
            adjacentText = false;
        if (parser.isStartElement() || parser.isCharacters() || parser.isComment()) {
            const int parent = stack.last();
            // Adjacent character tokens belong to one DOM text node.
            if (parser.isCharacters() && adjacentText && !nodes[parent].children.isEmpty()
                && nodes[nodes[parent].children.last()].text && !parser.isCDATA()
                && !nodes[nodes[parent].children.last()].cdata) {
                text += parser.text();
                nodes[nodes[parent].children.last()].end = text.size();
                continue;
            }
            const int index = nodes.size();
            Source::Node node;
            node.begin = text.size();
            node.parent = parent;
            node.text = parser.isCharacters();
            node.cdata = parser.isCDATA();
            node.element = parser.isStartElement();
            if (parser.isStartElement()) {
                node.id = parser.attributes().value("id").toString();
                node.name = parser.name().toString();
            }
            if (parser.isCharacters())
                text += parser.text();
            adjacentText = parser.isCharacters();
            node.end = text.size();
            nodes.append(node);
            nodes[parent].children.append(index);
            if (parser.isStartElement()) {
                if (parent == 0)
                    source.root = index;
                stack.append(index);
            }
        } else if (parser.isEndElement()) {
            nodes[stack.takeLast()].end = text.size();
        }
    }
    return !parser.hasError() && source.root >= 0;
}

// CREngine wraps some content in its own elements while it lays out a
// chapter. They are not in the source, so a path through one cannot match.
bool engineWrapper(ldomNode *node)
{
    for (const char *name : { "autoBoxing", "tabularBox", "rubyBox", "mathBox", "floatBox",
                              "inlineBox", "pseudoElem" })
        if (node->isNodeName(name))
            return true;
    return false;
}

// Plain EPUB chapters often have no element IDs. An element is then found by
// its index among its parent's elements, from <body>, which CREngine keeps
// inside the chapter's DocFragment. Each step must have the same element name.
struct ElementPath
{
    QVector<int> indices;
    QStringList names;
};

ldomNode *engineBody(ldomNode *fragment)
{
    ldomNode *body = nullptr;
    for (int i = 0; i < fragment->getChildCount(); ++i) {
        auto *child = fragment->getChildNode(i);
        // CREngine keeps the chapter's styles in a <stylesheet> element
        // beside <body>. It is not part of the body's structure.
        if (!child->isElement() || child->isNodeName("stylesheet"))
            continue;
        if (body || !child->isNodeName("body"))
            return nullptr;
        body = child;
    }
    return body;
}

bool engineElementPath(ldomNode *fragment, ldomNode *element, ElementPath &path)
{
    auto *body = engineBody(fragment);
    if (!body)
        return false;
    for (auto *node = element; node != body; node = node->getParentNode()) {
        if (!node || !node->isElement() || engineWrapper(node))
            return false;
        auto *parent = node->getParentNode();
        if (!parent)
            return false;
        int index = 0;
        bool found = false;
        for (int i = 0; i < parent->getChildCount() && !found; ++i) {
            auto *child = parent->getChildNode(i);
            if (!child->isElement())
                continue;
            if (engineWrapper(child))
                return false;
            if (child == node)
                found = true;
            else
                ++index;
        }
        if (!found)
            return false;
        path.indices.prepend(index);
        path.names.prepend(qstring(node->getNodeName()));
    }
    return true;
}

ldomNode *engineElement(ldomNode *fragment, const ElementPath &path)
{
    auto *node = engineBody(fragment);
    for (int step = 0; node && step < path.indices.size(); ++step) {
        ldomNode *next = nullptr;
        int index = 0;
        for (int i = 0; i < node->getChildCount(); ++i) {
            auto *child = node->getChildNode(i);
            if (!child->isElement())
                continue;
            if (engineWrapper(child))
                return nullptr;
            if (index++ == path.indices[step]) {
                next = child;
                break;
            }
        }
        if (!next
            || qstring(next->getNodeName()).compare(path.names[step], Qt::CaseInsensitive) != 0)
            return nullptr;
        node = next;
    }
    return node;
}

int sourceElement(const Source &source, const ElementPath &path)
{
    int node = source.body();
    for (int step = 0; node >= 0 && step < path.indices.size(); ++step) {
        int next = -1, index = 0;
        for (const int child : source.nodes[node].children) {
            if (source.nodes[child].element && index++ == path.indices[step]) {
                next = child;
                break;
            }
        }
        if (next < 0 || source.nodes[next].name.compare(path.names[step], Qt::CaseInsensitive) != 0)
            return -1;
        node = next;
    }
    return node;
}

bool sourceElementPath(const Source &source, int element, ElementPath &path)
{
    const int body = source.body();
    for (int node = element; node != body; node = source.nodes[node].parent) {
        if (node <= 0)
            return false;
        const int parent = source.nodes[node].parent;
        int index = 0;
        for (const int child : source.nodes[parent].children) {
            if (child == node)
                break;
            if (source.nodes[child].element)
                ++index;
        }
        path.indices.prepend(index);
        path.names.prepend(source.nodes[node].name);
    }
    return true;
}

// A native point() resolves to an element with an ID and an offset in it, or,
// in a chapter without IDs, to an element path and an offset in that element.
// The path, text and offset of the containing element are also kept when an
// ancestor has an ID, so the decoder can use them if that ID's text changed.
struct NativePoint
{
    QString id;
    int offset = 0;
    ElementPath path;
    QString text;
    int elementOffset = 0;
    bool located = false;
};

// Nickel point() locations count DOM children from the document element and
// measure offsets in UTF-8 bytes. Resolve against the original XML before
// checking that CREngine retained the identified element's exact text.
bool nativePoint(ldomDocument *doc, const QString &path, const QString &point, NativePoint &result)
{
    static const QRegularExpression format(
            QStringLiteral("^point\\((/1(?:/[1-9][0-9]*)*)(?::([0-9]+))?\\)$"));
    const auto match = format.match(point);
    if (!match.hasMatch())
        return false;
    Source source;
    if (!readSource(doc, path, source))
        return false;
    const auto &nodes = source.nodes;
    int target = source.root;
    const auto sequence = match.captured(1).split('/');
    for (int i = 2; i < sequence.size(); ++i) {
        bool ok = false;
        const int child = sequence[i].toInt(&ok) - 1;
        if (!ok || child < 0 || child >= nodes[target].children.size())
            return false;
        target = nodes[target].children[child];
    }
    if (!nodes[target].text && !nodes[target].element)
        return false;
    bool ok = true;
    const int bytes = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt(&ok);
    const auto utf8 = source.textOf(target).toUtf8();
    if (!ok || bytes > utf8.size())
        return false;
    const auto prefix = QString::fromUtf8(utf8.constData(), bytes);
    if (prefix.toUtf8() != utf8.left(bytes))
        return false;
    const int position = nodes[target].begin + prefix.size();
    const int element = nodes[target].element ? target : nodes[target].parent;
    result.located = element > 0 && sourceElementPath(source, element, result.path);
    if (result.located) {
        result.text = source.textOf(element);
        result.elementOffset = position - nodes[element].begin;
    } else {
        result.path = {};
    }
    for (int node = target; node > 0; node = nodes[node].parent) {
        if (!nodes[node].id.isEmpty()) {
            result.id = nodes[node].id;
            result.offset = position - nodes[node].begin;
            return true;
        }
    }
    // A native element location can point into a descendant with its own ID.
    for (int node = target + 1; node < nodes.size(); ++node) {
        if (nodes[node].begin > position)
            break;
        if (!nodes[node].id.isEmpty() && nodes[node].begin <= position
            && nodes[node].end >= position) {
            int ancestor = node;
            while (ancestor > 0 && ancestor != target)
                ancestor = nodes[ancestor].parent;
            if (ancestor == target) {
                result.id = nodes[node].id;
                result.offset = position - nodes[node].begin;
                return true;
            }
        }
    }
    if (!result.located)
        return false;
    result.offset = result.elementOffset;
    return true;
}

// The point() Nickel stores for `units` UTF-16 units into `element`, or an
// empty string when the source does not have the same element and text.
QString sourcePoint(ldomDocument *doc, const QString &path, ldomNode *fragment, ldomNode *element,
                    int units)
{
    ElementPath elementPath;
    Source source;
    if (!engineElementPath(fragment, element, elementPath) || !readSource(doc, path, source))
        return {};
    const int match = sourceElement(source, elementPath);
    if (match < 0 || source.textOf(match) != qstring(element->getText()))
        return {};
    const auto &nodes = source.nodes;
    const int position = nodes[match].begin + units;
    // Nickel's own points end in a text node. Prefer the text node that starts
    // at the position; at the element's end, use the last one.
    int target = match;
    for (int node = match + 1; node < nodes.size() && nodes[node].begin <= nodes[match].end;
         ++node) {
        int ancestor = node;
        while (ancestor > 0 && ancestor != match)
            ancestor = nodes[ancestor].parent;
        if (ancestor != match || !nodes[node].text || nodes[node].begin > position
            || nodes[node].end < position)
            continue;
        target = node;
        if (position < nodes[node].end)
            break;
    }
    const int bytes =
            source.text.mid(nodes[target].begin, position - nodes[target].begin).toUtf8().size();
    QStringList steps;
    for (int node = target; node != source.root; node = nodes[node].parent)
        steps.prepend(QString::number(nodes[nodes[node].parent].children.indexOf(node) + 1));
    steps.prepend(QStringLiteral("1"));
    return QStringLiteral("point(/") + steps.join('/') + ':' + QString::number(bytes) + ')';
}

// Counts the UTF-16 units of `element`'s text before `target` at `offset`.
bool unitsBefore(ldomNode *element, ldomNode *target, int offset, int &units, QString *error)
{
    units = 0;
    QVector<ldomNode *> pending{ element };
    while (!pending.isEmpty()) {
        auto *node = pending.takeLast();
        if (node == target) {
            if (node->isText()) {
                const auto text = node->getText();
                if (offset < 0 || offset > text.length()) {
                    if (error)
                        *error = strings::get("error.invalid_source_text_offset");
                    return false;
                }
                for (int i = 0; i < offset; ++i)
                    units += text[i] > 0xffff ? 2 : 1;
            } else {
                // An element XPointer counts children, not characters.
                if (offset < 0 || offset > node->getChildCount()) {
                    if (error)
                        *error = strings::get("error.invalid_source_element_offset");
                    return false;
                }
                for (int i = 0; i < offset; ++i)
                    units += qstring(node->getChildNode(i)->getText()).size();
            }
            return true;
        }
        if (node->isText())
            units += qstring(node->getText()).size();
        else
            for (int i = node->getChildCount() - 1; i >= 0; --i)
                pending.append(node->getChildNode(i));
    }
    if (error)
        *error = strings::get("error.source_position_is_outside_its_anchor");
    return false;
}

bool nativeText(ldomDocument *doc, const QString &path, const QString &id, ldomNode *element,
                QString &sourceText, QString *error, bool *textChanged = nullptr)
{
    if (textChanged) {
        *textChanged = false;
    }
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto archive = doc->getContainer();
    auto source = archive->OpenStream(cre(path).c_str(), LVOM_READ);
    if (source.isNull() || source->GetSize() > 16 * 1024 * 1024)
        return fail(strings::get("error.native_chapter_source_is_unavailable"));
    QByteArray xml(int(source->GetSize()), '\0');
    lvsize_t read = 0;
    if (source->Read(xml.data(), xml.size(), &read) != LVERR_OK || read != lvsize_t(xml.size()))
        return fail(strings::get("error.native_chapter_source_is_incomplete"));
    QXmlStreamReader parser(xml);
    sourceText.clear();
    int matches = 0, depth = 0, targetDepth = 0;
    while (!parser.atEnd()) {
        parser.readNext();
        if (parser.isStartElement()) {
            ++depth;
            if (parser.attributes().value("id") == id && ++matches == 1)
                targetDepth = depth;
        } else if (parser.isCharacters() && targetDepth) {
            sourceText += parser.text();
        } else if (parser.isEndElement()) {
            if (depth == targetDepth)
                targetDepth = 0;
            --depth;
        }
    }
    if (parser.hasError() || matches != 1) {
        return fail(strings::get(
                "error.native_text_anchor_cannot_be_mapped_without_changing_its_text"));
    }
    if (sourceText != qstring(element->getText())) {
        if (textChanged) {
            *textChanged = true;
        }
        return fail(strings::get(
                "error.native_text_anchor_cannot_be_mapped_without_changing_its_text"));
    }
    return true;
}
} // namespace

class NickelPositionCodec
{
public:
    static QString decode(const EngineDocument &engine, const QString &location, QString *error);
    static QString encode(const EngineDocument &engine, QString *error);
};

QString NickelPositionCodec::decode(const EngineDocument &engine, const QString &location,
                                    QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!engine.engine_ || location.isEmpty() || location.size() > 8192)
        return fail(strings::get("error.no_supported_native_reading_position"));
    const int hash = location.indexOf('#');
    const auto path =
            QUrl::fromPercentEncoding(location.left(hash < 0 ? location.size() : hash).toUtf8());
    QString id = hash < 0 ? QString() : location.mid(hash + 1);
    int offset = 0;
    const int colon = id.lastIndexOf(':');
    NativePoint point;
    const bool structural = id.startsWith("point(");
    if (structural) {
        if (!nativePoint(engine.engine_->getDocument(), path, id, point))
            return fail(strings::get("error.unsupported_native_text_offset"));
        id = point.id;
        offset = point.offset;
    } else if (colon >= 0) {
        bool ok = false;
        offset = id.mid(colon + 1).toInt(&ok);
        if (!ok || offset < 0)
            return fail(strings::get("error.unsupported_native_text_offset"));
        id.truncate(colon);
    }
    id = QUrl::fromPercentEncoding(id.toUtf8());
    auto *doc = engine.engine_->getDocument();
    auto *body = doc->nodeFromXPath(U"/body");
    if (!body)
        return fail(strings::get("error.missing_publication_body"));
    ldomNode *fragment = nullptr;
    for (int i = 0; i < body->getChildCount(); ++i) {
        auto *node = body->getChildNode(i);
        if (node->isElement() && node->isNodeName("DocFragment")
            && qstring(node->getAttributeValue("Source")) == path) {
            if (fragment)
                return fail(strings::get("error.ambiguous_native_chapter_path"));
            fragment = node;
        }
    }
    if (!fragment)
        return fail(strings::get("error.native_chapter_is_not_in_the_publication_spine"));
    ldomNode *element = nullptr;
    QString sourceText;
    // Find the point's own element by its path from <body>.
    const auto byPath = [&] {
        element = engineElement(fragment, point.path);
        if (!element || qstring(element->getText()) != point.text)
            return false;
        sourceText = point.text;
        offset = point.elementOffset;
        return true;
    };
    if (structural && id.isEmpty()) {
        // A chapter without IDs.
        if (!byPath())
            return fail(strings::get(
                    "error.native_text_anchor_cannot_be_mapped_without_changing_its_text"));
    } else if (id.isEmpty()) {
        if (offset)
            return fail(strings::get("error.invalid_native_chapter_position"));
        return qstring(ldomXPointer(fragment, 0).toString());
    } else {
        const auto internalId = qstring(fragment->getAttributeValue("id")) + "_ " + id;
        element = doc->getNodeById(doc->getAttrValueIndex(cre(internalId).c_str()));
        if (!element)
            return fail(strings::get("error.native_text_anchor_is_not_in_the_publication"));
        // TENG offsets count UTF-16 units in the source DOM. CREngine uses
        // UTF-32 and can normalize whitespace. Reject a changed text stream
        // rather than turning the same number into a position in different text.
        bool textChanged = false;
        if (!nativeText(doc, path, id, element, sourceText, error, &textChanged)) {
            // CREngine drops the whitespace between block children, so an ID
            // on a section around paragraphs without IDs fails this check. A
            // native point still names its own element, which can match.
            if (!structural || !textChanged || !point.located || !byPath())
                return {};
            if (error)
                error->clear();
        }
    }
    if (offset > sourceText.size())
        return fail(strings::get("error.native_text_offset_exceeds_its_anchor"));
    QVector<ldomNode *> pending{ element };
    int remaining = offset;
    while (!pending.isEmpty()) {
        auto *node = pending.takeLast();
        if (node->isText()) {
            const auto text = node->getText();
            int units = 0;
            for (int i = 0; i <= text.length(); ++i) {
                if (units == remaining) {
                    return qstring(ldomXPointer(node, i).toString());
                }
                if (i < text.length())
                    units += text[i] > 0xffff ? 2 : 1;
                if (units > remaining)
                    return fail(strings::get("error.native_offset_splits_a_unicode_character"));
            }
            remaining -= units;
        } else {
            for (int i = node->getChildCount() - 1; i >= 0; --i)
                pending.append(node->getChildNode(i));
        }
    }
    if (sourceText.isEmpty() && !offset)
        return qstring(ldomXPointer(element, 0).toString());
    return fail(strings::get("error.native_text_position_was_not_found"));
}

QString NickelPositionCodec::encode(const EngineDocument &engine, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!engine.engine_)
        return fail(strings::get("error.no_reading_position"));
    auto *doc = engine.engine_->getDocument();
    const auto pointer = doc->createXPointer(cre(engine.anchor_));
    auto *target = pointer.getNode();
    if (!target)
        return fail(strings::get("error.invalid_source_position"));
    auto *fragment = target;
    while (fragment && !fragment->isNodeName("DocFragment"))
        fragment = fragment->getParentNode();
    if (!fragment)
        return fail(strings::get("error.position_has_no_source_chapter"));
    const auto path = qstring(fragment->getAttributeValue("Source"));
    if (path.isEmpty())
        return fail(strings::get("error.position_has_no_source_path"));
    const auto encodedPath = QString::fromLatin1(QUrl::toPercentEncoding(path, "/"));
    const auto chapterFallback = [&] {
        auto *body = doc->nodeFromXPath(U"/body");
        if (!body) {
            return fail(strings::get("error.missing_publication_body"));
        }
        for (int i = 0; i < body->getChildCount(); ++i) {
            auto *node = body->getChildNode(i);
            if (node != fragment && node->isElement() && node->isNodeName("DocFragment")
                && qstring(node->getAttributeValue("Source")) == path) {
                return fail(strings::get("error.ambiguous_native_chapter_path"));
            }
        }
        // Keep the exact local anchor. Nickel gets a valid chapter start when
        // neither an ID nor the source structure supplies an exact offset.
        // This still lets it update the active book, cover and percentage.
        if (error) {
            error->clear();
        }
        return encodedPath;
    };
    if (target == fragment && pointer.getOffset() == 0)
        return encodedPath;
    const auto prefix = qstring(fragment->getAttributeValue("id")) + "_ ";
    // Without an ID, store Nickel's own DOM point for the nearest element.
    const auto structuralPoint = [&]() -> QString {
        auto *nearest = target->isText() ? target->getParentNode() : target;
        int units = 0;
        if (!nearest || nearest == fragment
            || !unitsBefore(nearest, target, pointer.getOffset(), units, nullptr))
            return chapterFallback();
        const auto point = sourcePoint(doc, path, fragment, nearest, units);
        return point.isEmpty() ? chapterFallback() : encodedPath + '#' + point;
    };
    auto *element = target->isText() ? target->getParentNode() : target;
    while (element && element != fragment && element->getAttributeValue("id").empty())
        element = element->getParentNode();
    if (!element || element == fragment) {
        return structuralPoint();
    }
    const auto internalId = qstring(element->getAttributeValue("id"));
    if (!internalId.startsWith(prefix)) {
        return structuralPoint();
    }
    const auto id = internalId.mid(prefix.size());
    QString sourceText;
    bool textChanged = false;
    if (!nativeText(doc, path, id, element, sourceText, error, &textChanged)) {
        if (!textChanged)
            return {};
        if (error)
            error->clear();
        // A KEPUB keeps its text in kobo.x.y spans inside book-columns and
        // book-inner. Nickel's KEPUB reader is not known to open a point(),
        // so it keeps the chapter start there, as before.
        if (id.startsWith("kobo.") || id == "book-inner" || id == "book-columns")
            return chapterFallback();
        // CREngine drops the whitespace between block children, so an ID on
        // a section around paragraphs without IDs fails this check. Nickel's
        // DOM point for the nearest element can still match; otherwise this
        // falls back to the chapter.
        return structuralPoint();
    }
    int units = 0;
    if (!unitsBefore(element, target, pointer.getOffset(), units, error))
        return {};
    return encodedPath + '#' + QString::fromLatin1(QUrl::toPercentEncoding(id)) + ':'
            + QString::number(units);
}

PositionCodec positionCodec()
{
    return { NickelPositionCodec::decode, NickelPositionCodec::encode, [](const QString &location) {
                return location.left(location.indexOf('#') < 0 ? location.size()
                                                               : location.indexOf('#'));
            } };
}
} // namespace kobalt::nickel
