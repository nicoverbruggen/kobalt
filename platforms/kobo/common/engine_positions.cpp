#include "engine/blaze/adapter/column_view.h"
#include "engine_positions.h"
#include "engine/blaze/adapter/engine_document.h"
#include "engine/blaze/adapter/reader_source.h"
#include "support/strings.h"
#include <lvdocview.h>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QUrl>
#include <algorithm>

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

// The parsed source of the chapter file that was read last. The reader asks
// for the place of every page, mostly in the same chapter, and the file does
// not change while its book is open. `identity` is the ReaderSource identity
// of the book, which no other book gets.
struct SourceCache
{
    quint64 identity = 0;
    QString path;
    bool valid = false;
    Source source;
};

const Source *chapterSource(SourceCache &cached, quint64 identity, ldomDocument *doc,
                            const QString &path)
{
    if (!identity || cached.identity != identity || cached.path != path) {
        cached = SourceCache();
        cached.valid = readSource(doc, path, cached.source);
        // A file that could not be read is read again at the next call.
        cached.identity = cached.valid ? identity : 0;
        cached.path = path;
    }
    return cached.valid ? &cached.source : nullptr;
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
// ancestor has an ID, so the decoder can use them if that ID is duplicated
// or its text changed.
struct NativePoint
{
    QString id;
    int offset = 0;
    ElementPath path;
    QString text;
    int elementOffset = 0;
    bool located = false;
    bool textStart = false;
};

// Nickel point() locations count DOM children from the document element and
// measure offsets in UTF-8 bytes. Resolve against the original XML before
// checking that CREngine retained the identified element's exact text.
bool nativePoint(SourceCache &cache, quint64 identity, ldomDocument *doc, const QString &path,
                 const QString &point, NativePoint &result)
{
    static const QRegularExpression format(
            QStringLiteral("^point\\((/1(?:/[1-9][0-9]*)*)(?::([0-9]+))?\\)$"));
    const auto match = format.match(point);
    if (!match.hasMatch())
        return false;
    const auto *read = chapterSource(cache, identity, doc, path);
    if (!read)
        return false;
    const Source &source = *read;
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
    result.textStart = nodes[target].text && bytes == 0;
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
QString sourcePoint(SourceCache &cache, quint64 identity, ldomDocument *doc, const QString &path,
                    ldomNode *fragment, ldomNode *element, int units, bool textEnd)
{
    ElementPath elementPath;
    if (!engineElementPath(fragment, element, elementPath))
        return {};
    const auto *read = chapterSource(cache, identity, doc, path);
    if (!read)
        return {};
    const Source &source = *read;
    const int match = sourceElement(source, elementPath);
    if (match < 0 || source.textOf(match) != qstring(element->getText()))
        return {};
    const auto &nodes = source.nodes;
    const int position = nodes[match].begin + units;
    // Nickel's own points end in a text node. Preserve an explicit text-node
    // end; otherwise prefer the node that starts here, or the last node at
    // the element's end.
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
        if (position < nodes[node].end || textEnd)
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

// The text of each element with an id in a chapter file, from one parse.
// `matches` counts the elements with that id; the text is the first one's.
struct IdText
{
    // The text is `ChapterIds::text` from `begin` to `end`, and its text
    // tokens end at `ChapterIds::textEnds` from `firstToken` to `endToken`.
    // Nested elements share the text of their chapter.
    int begin = 0, end = 0, firstToken = 0, endToken = 0;
    int matches = 0;
    // How the text maps onto `engineText`, the text of the element in the
    // engine: not yet, the same, the same after collapsing XML whitespace
    // (with `engineUnits`), or not at all.
    enum class Mapping { Unknown, Same, Collapsed, Different } mapping = Mapping::Unknown;
    QString engineText;
    QVector<int> engineUnits;
};

struct ChapterIds
{
    // The key of a message when the file cannot be read.
    const char *failure = nullptr;
    bool parseError = false;
    QHash<QString, int> index;
    QVector<IdText> texts;
    int mappedId = -1;
    // The text inside elements with an id, once. Whitespace on opposite sides
    // of inline markup can survive separately, so `textEnds` keeps where each
    // text token ends.
    QString text;
    QVector<int> textEnds;

    QString textOf(const IdText &entry) const
    {
        return text.mid(entry.begin, entry.end - entry.begin);
    }
};

// The ids of the chapter file that was read last. The reader asks for the
// place of every page, mostly in the same chapter: one parse serves them
// all. `identity` is as for chapterSource().
struct IdsCache
{
    quint64 identity = 0;
    QString path;
    ChapterIds ids;
};

ChapterIds *chapterIds(IdsCache &cached, quint64 identity, ldomDocument *doc, const QString &path)
{
    if (identity && cached.identity == identity && cached.path == path)
        return &cached.ids;
    cached = IdsCache();
    cached.identity = identity;
    cached.path = path;
    auto &ids = cached.ids;
    auto archive = doc->getContainer();
    auto source = archive->OpenStream(cre(path).c_str(), LVOM_READ);
    // A file that could not be read is read again at the next call.
    if (source.isNull() || source->GetSize() > 16 * 1024 * 1024) {
        ids.failure = "error.native_chapter_source_is_unavailable";
        cached.identity = 0;
        return &ids;
    }
    QByteArray xml(int(source->GetSize()), '\0');
    lvsize_t read = 0;
    if (source->Read(xml.data(), xml.size(), &read) != LVERR_OK || read != lvsize_t(xml.size())) {
        ids.failure = "error.native_chapter_source_is_incomplete";
        cached.identity = 0;
        return &ids;
    }
    QXmlStreamReader parser(xml);
    // The elements with an id that are open, by depth, as indexes in `texts`.
    QVector<QPair<int, int>> open;
    const auto close = [&ids, &open] {
        auto &entry = ids.texts[open.takeLast().second];
        entry.end = ids.text.size();
        entry.endToken = ids.textEnds.size();
    };
    int depth = 0;
    while (!parser.atEnd()) {
        parser.readNext();
        if (parser.isStartElement()) {
            ++depth;
            const auto id = parser.attributes().value("id");
            if (id.isEmpty())
                continue;
            const auto key = id.toString();
            auto found = ids.index.find(key);
            if (found == ids.index.end()) {
                found = ids.index.insert(key, ids.texts.size());
                ids.texts.append(IdText());
            }
            auto &entry = ids.texts[found.value()];
            if (++entry.matches == 1) {
                entry.begin = ids.text.size();
                entry.firstToken = ids.textEnds.size();
                open.append({ depth, found.value() });
            }
        } else if (parser.isCharacters() && !open.isEmpty()) {
            ids.text += parser.text();
            ids.textEnds.append(ids.text.size());
        } else if (parser.isEndElement()) {
            while (!open.isEmpty() && open.last().first == depth)
                close();
            --depth;
        }
    }
    // A damaged file can leave elements open; parseError rejects it.
    while (!open.isEmpty())
        close();
    ids.parseError = parser.hasError();
    return &ids;
}

bool nativeText(IdsCache &cache, quint64 identity, ldomDocument *doc, const QString &path,
                const QString &id, ldomNode *element, QString &sourceText,
                QVector<int> &engineUnits, QString *error, bool *unusableAnchor = nullptr)
{
    engineUnits.clear();
    if (unusableAnchor) {
        *unusableAnchor = false;
    }
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *ids = chapterIds(cache, identity, doc, path);
    if (ids->failure)
        return fail(strings::get(ids->failure));
    const auto found = ids->index.constFind(id);
    IdText *entry = found == ids->index.cend() ? nullptr : &ids->texts[found.value()];
    const int matches = entry ? entry->matches : 0;
    if (!ids->parseError && matches > 1 && unusableAnchor)
        *unusableAnchor = true;
    if (ids->parseError || matches != 1) {
        return fail(strings::get(
                "error.native_text_anchor_cannot_be_mapped_without_changing_its_text"));
    }
    if (ids->mappedId >= 0 && ids->mappedId != found.value()) {
        auto &previous = ids->texts[ids->mappedId];
        previous.engineText.clear();
        previous.engineUnits = QVector<int>();
        previous.mapping = IdText::Mapping::Unknown;
    }
    ids->mappedId = found.value();
    sourceText = ids->textOf(*entry);
    const auto engineText = qstring(element->getText());
    if (entry->mapping == IdText::Mapping::Unknown || entry->engineText != engineText) {
        entry->engineText = engineText;
        entry->engineUnits.clear();
        entry->mapping = IdText::Mapping::Same;
        if (sourceText != engineText) {
            // Only accept XML whitespace normalization when the complete
            // result matches the engine. Keep a boundary for every source
            // UTF-16 unit; using the original offset in collapsed text would
            // move the passage.
            const QString &source = ids->text;
            QString collapsed;
            QVector<int> offsets;
            offsets.reserve(sourceText.size() + 1);
            int begin = entry->begin;
            // Whitespace on opposite sides of inline markup can survive
            // separately.
            for (int token = entry->firstToken; token < entry->endToken; ++token) {
                const int end = ids->textEnds[token];
                bool space = false;
                for (int i = begin; i < end; ++i) {
                    offsets.append(collapsed.size());
                    const auto ch = source.at(i);
                    const bool white = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
                    if (!white || !space)
                        collapsed += white ? QChar(' ') : ch;
                    space = white;
                }
                begin = end;
            }
            if (collapsed == engineText) {
                offsets.append(collapsed.size());
                entry->engineUnits = std::move(offsets);
                entry->mapping = IdText::Mapping::Collapsed;
            } else {
                entry->mapping = IdText::Mapping::Different;
            }
        }
    }
    if (entry->mapping == IdText::Mapping::Different) {
        if (unusableAnchor) {
            *unusableAnchor = true;
        }
        return fail(strings::get(
                "error.native_text_anchor_cannot_be_mapped_without_changing_its_text"));
    }
    if (entry->mapping == IdText::Mapping::Collapsed)
        engineUnits = entry->engineUnits;
    return true;
}
} // namespace

// The parsed chapter files of one EngineDocument. The document owns them, so
// they go when it closes. Its calls stay on one thread.
struct NativePositionCache
{
    SourceCache source;
    IdsCache ids;
};

class NickelPositionCodec
{
public:
    static QString decode(const Engine &engine, const QString &location, QString *error);
    static QString encode(const Engine &engine, QString *error);

    // The book of `engine`, for chapterSource(); 0 keeps nothing.
    static quint64 sourceIdentity(const EngineDocument &engine)
    {
        return engine.source_ ? engine.source_->identity : 0;
    }

    static NativePositionCache &cache(const EngineDocument &engine)
    {
        if (!engine.nativePositions_)
            engine.nativePositions_ = std::make_shared<NativePositionCache>();
        return *engine.nativePositions_;
    }

    static int keptText(const Engine &engine);
};

// Nickel locations are matched against CREngine's DOM. Another engine has no
// such position until this codec works on shared locations.
const EngineDocument *crengine(const Engine &engine)
{
    return qstrcmp(engine.name(), "crengine") == 0 ? static_cast<const EngineDocument *>(&engine)
                                                   : nullptr;
}

QString NickelPositionCodec::decode(const Engine &other, const QString &location, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!crengine(other))
        return fail(strings::get("error.no_supported_native_reading_position"));
    const auto &engine = *crengine(other);
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
        if (!nativePoint(cache(engine).source, sourceIdentity(engine),
                         engine.engine_->getDocument(), path, id, point))
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
    // A structural point already supplied the literal ID from the chapter XML.
    if (!structural)
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
        // TENG offsets count UTF-16 units in the source DOM. The engine uses
        // UTF-32 and can collapse XML whitespace, so translate those offsets
        // only after checking the complete text stream.
        bool unusableAnchor = false;
        QVector<int> engineUnits;
        if (!nativeText(cache(engine).ids, sourceIdentity(engine), doc, path, id, element,
                        sourceText, engineUnits, error, &unusableAnchor)) {
            // CREngine drops the whitespace between block children, so an ID
            // on a section around paragraphs without IDs fails this check. A
            // native point still names its own element, which can match even
            // when the ancestor ID is duplicated.
            if (!structural || !unusableAnchor || !point.located || !byPath())
                return {};
            if (error)
                error->clear();
        } else if (!engineUnits.isEmpty()) {
            if (offset > sourceText.size())
                return fail(strings::get("error.native_text_offset_exceeds_its_anchor"));
            offset = engineUnits[offset];
            sourceText = qstring(element->getText());
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
                // A point at the start of a text node belongs after intervening
                // markup (for example <br/>), not at the previous node's end.
                if (units == remaining && !(point.textStart && i == text.length())) {
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

QString NickelPositionCodec::encode(const Engine &other, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!crengine(other))
        return fail(strings::get("error.no_reading_position"));
    const auto &engine = *crengine(other);
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
        const auto chapter = chapterFallback();
        if (chapter.isEmpty())
            return {};
        auto *nearest = target->isText() ? target->getParentNode() : target;
        int units = 0;
        if (!nearest || nearest == fragment
            || !unitsBefore(nearest, target, pointer.getOffset(), units, nullptr))
            return chapter;
        const bool textEnd = target->isText() && pointer.getOffset() > 0
                && pointer.getOffset() == target->getText().length();
        const auto point = sourcePoint(cache(engine).source, sourceIdentity(engine), doc, path,
                                       fragment, nearest, units, textEnd);
        return point.isEmpty() ? chapter : chapter + '#' + point;
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
    bool unusableAnchor = false;
    QVector<int> engineUnits;
    if (!nativeText(cache(engine).ids, sourceIdentity(engine), doc, path, id, element, sourceText,
                    engineUnits, error, &unusableAnchor)) {
        if (!unusableAnchor)
            return {};
        if (error)
            error->clear();
        // A KEPUB keeps its text in kobo.x.y spans inside book-columns and
        // book-inner. Nickel's KEPUB reader is not known to open a point(),
        // so it keeps the chapter start there, as before.
        if (id.startsWith("kobo.") || id == "book-inner" || id == "book-columns")
            return chapterFallback();
        // A duplicated ID or changed text cannot identify this position. Nickel's
        // DOM point for the nearest element can still match; otherwise this
        // falls back to the chapter.
        return structuralPoint();
    }
    // A node-only XPointer has offset -1, not a child boundary. Keep the
    // local anchor and give Nickel the checked chapter start, including when
    // the node is an empty anchor or an image-only span.
    if (target->isElement() && pointer.getOffset() == -1)
        return chapterFallback();
    int units = 0;
    if (!unitsBefore(element, target, pointer.getOffset(), units, error))
        return {};
    // Several source boundaries can collapse onto one engine boundary. Prefer
    // the last one, just before the next retained character (or the text end).
    if (!engineUnits.isEmpty())
        units = int(std::upper_bound(engineUnits.begin(), engineUnits.end(), units)
                    - engineUnits.begin())
                - 1;
    return encodedPath + '#' + QString::fromLatin1(QUrl::toPercentEncoding(id)) + ':'
            + QString::number(units);
}

int NickelPositionCodec::keptText(const Engine &other)
{
    const auto *engine = crengine(other);
    if (!engine || !engine->nativePositions_)
        return 0;
    const auto &kept = *engine->nativePositions_;
    return kept.source.source.text.size() + kept.ids.ids.text.size();
}

int keptNativeText(const Engine &engine)
{
    return NickelPositionCodec::keptText(engine);
}

PositionCodec positionCodec()
{
    return { NickelPositionCodec::decode, NickelPositionCodec::encode, [](const QString &location) {
                return location.left(location.indexOf('#') < 0 ? location.size()
                                                               : location.indexOf('#'));
            } };
}
} // namespace kobalt::nickel
