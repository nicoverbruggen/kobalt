#include "engine_document.h"
#include "reader_source.h"
#include "stop_point.h"
#include "publication/publication.h"
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QXmlStreamReader>
#include "xml_entities.h"

namespace kobalt {
namespace {
enum Boundary { Start = 1, After = 2, Prologue = 4, Epilogue = 8 };

int semantics(const QStringList &types)
{
    if (types.contains("bodymatter"))
        return Start;
    if (types.contains("backmatter") || types.contains("appendix") || types.contains("index")
        || types.contains("bibliography") || types.contains("glossary"))
        return After;
    return 0;
}

int narrativeType(const QStringList &types)
{
    if (types.contains("prologue"))
        return Prologue;
    if (types.contains("epilogue"))
        return Epilogue;
    return 0;
}

QString fragment(int item)
{
    return QString("/body/DocFragment[%1]").arg(item + 1);
}

const QString ops = QStringLiteral("http://www.idpf.org/2007/ops");

QByteArray resource(ReaderSource &source, const QString &path)
{
    // The resource cache finds members through its own index; opening an
    // engine stream for each spine file costs more than reading it.
    if (source.resources && source.resources->contains(path))
        return source.resources->readOptional(path, Publication::maxResource);
    const auto stream = source.container->OpenStream(
            Utf8ToUnicode(path.toUtf8().constData()).c_str(), LVOM_READ);
    if (stream.isNull() || stream->GetSize() > Publication::maxResource)
        return {};
    QByteArray bytes(int(stream->GetSize()), '\0');
    lvsize_t count = 0;
    if (stream->Read(bytes.data(), bytes.size(), &count) != LVERR_OK
        || count != lvsize_t(bytes.size()))
        return {};
    return bytes;
}

// Stop at a DTD whose entities could grow without limit on Qt 5.2.
void nextToken(QXmlStreamReader &xml)
{
    if (xml.readNext() == QXmlStreamReader::DTD && unsafeEntities(xml))
        xml.raiseError(QStringLiteral("Entity declarations"));
}

// Only an OPS-namespaced type attribute can mark a boundary, and an attribute
// in a namespace always has a prefix, as in epub:type.
bool mayHaveNamespacedType(const QByteArray &bytes)
{
    for (int at = bytes.indexOf(":type"); at >= 0; at = bytes.indexOf(":type", at + 5)) {
        int next = at + 5;
        while (next < bytes.size() && QChar::isSpace(uchar(bytes[next])))
            ++next;
        if (next < bytes.size() && bytes[next] == '=')
            return true;
    }
    return false;
}

struct Reference
{
    QString path, id;
    int boundary;
};

struct Element
{
    QString name;
    int ordinal;
    QHash<QString, int> children;
    qint64 begin;
    int textNodes;
    bool inText;
    bool narrativeSection;
    QString precedingEnd;
};

QString sourcePath(int item, const QVector<Element> &elements)
{
    auto position = fragment(item);
    for (const auto &element : elements)
        position += QString("/%1[%2]").arg(element.name).arg(element.ordinal);
    return position;
}

Reference reference(const QString &base, const QString &href, int boundary)
{
    const QUrl url(href);
    if (!url.isRelative() || !url.authority().isEmpty() || url.path().startsWith('/')
        || href.isEmpty())
        return { {}, {}, boundary };
    const QString path = url.path().isEmpty()
            ? base
            : QDir::cleanPath(QFileInfo(base).path() + "/" + url.path());
    return { path, url.fragment(QUrl::FullyDecoded), boundary };
}

void record(QPair<QString, QString> &result, int flags, const QString &path)
{
    if ((flags & Start) && result.first.isEmpty()) {
        result.first = path;
        // A guide index before the body is front matter.
        result.second.clear();
    }
    if ((flags & After) && result.second.isEmpty())
        result.second = path;
}

struct FragmentAnchors
{
    QHash<int, QString> positions;
    QHash<QString, int> order;

    void publish(ReaderSource &source) const
    {
        for (auto it = positions.constBegin(); it != positions.constEnd(); ++it)
            source.tocAnchors[it.key()].position = it.value();
        for (auto it = order.constBegin(); it != order.constEnd(); ++it)
            source.boundaryElementOrder.insert(it.key(), it.value());
    }
};

// Counts source content between anchors, independent of pagination. Equal offsets
// mean that only container tags and whitespace separate the two positions.
struct NarrativeRange
{
    int type;
    qint64 begin, end;
    QString position, after;
};

struct NarrativeScan
{
    qint64 offset = 0;
    QVector<NarrativeRange> ranges;
    QHash<QString, qint64> boundaries;
    QString epilogueBoundary;

    bool finishRange(const QString &publisherEnd)
    {
        auto &range = ranges.last();
        if (offset == range.begin) {
            ranges.removeLast();
            return false;
        }
        range.end = offset;
        if (range.type != Epilogue)
            return false;
        if (boundaries.value(publisherEnd, -1) == range.begin)
            epilogueBoundary = publisherEnd;
        return true;
    }

    void extend(QPair<QString, QString> &result) const
    {
        auto start = boundaries.value(result.first, -1);
        for (int i = ranges.size() - 1; i >= 0; --i) {
            const auto &range = ranges[i];
            if (range.type == Prologue && range.end == start) {
                result.first = range.position;
                start = range.begin;
            }
        }
        auto end = boundaries.value(result.second, -1);
        for (const auto &range : ranges) {
            if (range.type == Epilogue && range.begin == end && !range.after.isNull()) {
                result.second = range.after;
                end = range.end;
            }
        }
    }
};

void scanSpineItem(FragmentAnchors &anchors, int item, const QByteArray &bytes,
                   const QHash<QString, int> &targets, const QMultiHash<QString, int> &ids,
                   bool epub3, QPair<QString, QString> &result, NarrativeScan *narrative = nullptr,
                   int fileFlags = 0, const QString &nextFile = {})
{
    QXmlStreamReader xml(bytes);
    QVector<Element> elements;
    const auto before = result;
    int elementOrder = 0;
    bool chapterBody = false;
    int narrativeDepth = 0, nextAnchor = -1;
    bool headingScope = false;
    const auto priorEpilogue = narrative ? narrative->epilogueBoundary : QString();
    const int firstRange = narrative ? narrative->ranges.size() : 0;
    const auto fileOffset = narrative ? narrative->offset : 0;
    while (!xml.atEnd()) {
        stopPoint();
        nextToken(xml);
        if (xml.isStartElement()) {
            if (elements.isEmpty() && xml.name() != QLatin1String("body"))
                continue;
            ++elementOrder;
            const auto name = xml.name().toString();
            const int ordinal = elements.isEmpty() ? 1 : ++elements.last().children[name];
            if (!elements.isEmpty())
                elements.last().inText = false;
            elements.append(Element{ name,
                                     ordinal,
                                     {},
                                     narrative ? narrative->offset : 0,
                                     0,
                                     false,
                                     name == "section" || name == "article",
                                     {} });
            const auto attrs = xml.attributes();
            const auto id =
                    targets.isEmpty() && ids.isEmpty() ? QString() : attrs.value("id").toString();
            int flags = id.isEmpty() ? 0 : targets.value(id);
            const bool headingLandmark = (flags & (Prologue | Epilogue)) && name.size() == 2
                    && name[0] == QLatin1Char('h') && name[1] >= QLatin1Char('1')
                    && name[1] <= QLatin1Char('6');
            if (elements.size() == 1)
                flags |= fileFlags & (Prologue | Epilogue);
            const auto type = attrs.value(ops, QStringLiteral("type"));
            if (epub3 && !type.isEmpty()) {
                const auto types = type.toString().simplified().split(' ');
                flags |= narrativeType(types);
                if (semantics(types) || types.contains("frontmatter"))
                    elements.last().narrativeSection = false;
                if (name == "body" || name == "section" || name == "div" || name == "article") {
                    if (elements.size() == 1)
                        chapterBody = types.contains("chapter");
                    // A glossary inside a chapter is local to that chapter. Only a
                    // partition of the publication or a landmark can end the main text.
                    if (types.contains("backmatter") || types.contains("bodymatter")
                        || elements.size() == 1 || (elements.size() == 2 && !chapterBody))
                        flags |= semantics(types);
                    const bool inEpilogue = narrative
                            && ((flags & Epilogue)
                                || (narrativeDepth && narrative->ranges.last().type == Epilogue));
                    if ((types.contains("chapter") || types.contains("part")) && !inEpilogue
                        && !result.second.isEmpty()
                        && (!narrative || result.second != narrative->epilogueBoundary)) {
                        // The first heading may still identify this chapter as the epilogue.
                        if (narrative
                            && narrative->boundaries.value(result.second, -1) == narrative->offset)
                            elements.last().precedingEnd = result.second;
                        result.second.clear();
                    }
                }
            }
            const auto rows = id.isEmpty() ? QList<int>() : ids.values(id);
            if (flags || !rows.isEmpty() || nextAnchor >= 0) {
                // Only anchors need paths, but their ordinals include every sibling.
                const auto position = sourcePath(item, elements);
                for (const int row : rows)
                    anchors.positions.insert(row, position);
                anchors.order.insert(position, elementOrder);
                if (nextAnchor >= 0) {
                    narrative->ranges[nextAnchor].after = position;
                    nextAnchor = -1;
                }
                if (narrative && (flags & (Start | After)))
                    narrative->boundaries.insert(position, narrative->offset);
                if (headingScope && (flags & Prologue)
                    && narrative->ranges.last().type == Epilogue) {
                    // An opening heading names the enclosing section only up to
                    // a more specific narrative boundary inside that section.
                    if (narrative->finishRange(result.second))
                        narrative->ranges.last().after = position;
                    narrativeDepth = 0;
                    headingScope = false;
                }
                if (narrative && !narrativeDepth && (flags & (Prologue | Epilogue))) {
                    narrativeDepth = elements.size();
                    // A landmark on the opening heading names the enclosing section.
                    // A partition can contain several works, so keep its heading local.
                    if (headingLandmark && narrativeDepth > 1) {
                        const auto &parent = elements.at(narrativeDepth - 2);
                        if (parent.narrativeSection && parent.begin == narrative->offset) {
                            --narrativeDepth;
                            headingScope = true;
                            if ((flags & Epilogue) && result.second.isEmpty())
                                result.second = parent.precedingEnd;
                        }
                    }
                    narrative->ranges.append(NarrativeRange{
                            flags & (Prologue | Epilogue), narrative->offset, 0, position, {} });
                }
                if (flags)
                    record(result, flags, position);
            }
            if (narrative
                && (name == "img" || name == "svg" || name == "math" || name == "hr" || name == "br"
                    || name == "object" || name == "audio" || name == "video" || name == "canvas"))
                ++narrative->offset;
        } else if (xml.isCharacters() && !elements.isEmpty() && !xml.isWhitespace()) {
            auto &parent = elements.last();
            if (!parent.inText) {
                ++elementOrder;
                ++parent.textNodes;
                parent.inText = true;
            }
            if (nextAnchor >= 0) {
                const auto position =
                        sourcePath(item, elements) + QString("/text()[%1]").arg(parent.textNodes);
                narrative->ranges[nextAnchor].after = position + ".0";
                anchors.order.insert(position, elementOrder);
                nextAnchor = -1;
            }
            if (narrative)
                ++narrative->offset;
        } else if (xml.isEndElement() && !elements.isEmpty()) {
            if (narrativeDepth && narrativeDepth == elements.size()) {
                if (narrative->finishRange(result.second))
                    nextAnchor = narrative->ranges.size() - 1;
                narrativeDepth = 0;
                headingScope = false;
            }
            elements.removeLast();
        }
    }
    if (xml.hasError()) {
        result = before;
        if (narrative) {
            narrative->ranges.resize(firstRange);
            narrative->offset = fileOffset + 1;
            narrative->epilogueBoundary = priorEpilogue;
        }
    } else if (nextAnchor >= 0)
        narrative->ranges[nextAnchor].after = nextFile;
}

QPair<QString, QString> detect(ReaderSource &source, const QStringList &spine,
                               const QVector<bool> &nonLinear)
{
    QString package, nav, ncxId;
    QHash<QString, QString> ncx;
    QXmlStreamReader container(resource(source, "META-INF/container.xml"));
    while (!container.atEnd()) {
        nextToken(container);
        if (container.isStartElement() && container.name() == QStringLiteral("rootfile")) {
            package = container.attributes().value("full-path").toString();
            break;
        }
    }
    if (package.isEmpty())
        return {};
    QXmlStreamReader opf(resource(source, package));
    QVector<Reference> refs;
    bool epub3 = false, guide = false;
    while (!opf.atEnd()) {
        nextToken(opf);
        const auto name = opf.name().toString();
        if (name == "guide")
            guide = opf.isStartElement();
        if (!opf.isStartElement())
            continue;
        const auto attrs = opf.attributes();
        const auto href = attrs.value("href").toString();
        if (name == "package")
            epub3 = attrs.value("version").toString().startsWith("3.");
        if (epub3 && name == "item"
            && attrs.value("properties").toString().simplified().split(' ').contains("nav"))
            nav = reference(package, href, 0).path;
        if (name == "item"
            && attrs.value("media-type") == QStringLiteral("application/x-dtbncx+xml"))
            ncx.insert(attrs.value("id").toString(), reference(package, href, 0).path);
        if (name == "spine")
            ncxId = attrs.value("toc").toString();
        if (!epub3 && guide && name == "reference") {
            const auto type = attrs.value("type").toString();
            if (type == "text" || type == "bibliography" || type == "glossary" || type == "index")
                refs.append(reference(package, href, type == "text" ? Start : After));
        }
    }
    if (opf.hasError())
        return {};
    if (!nav.isEmpty()) {
        QXmlStreamReader xml(resource(source, nav));
        bool landmarks = false, toc = false;
        QVector<int> parents;
        while (!xml.atEnd()) {
            stopPoint();
            nextToken(xml);
            const auto types = xml.attributes()
                                       .value(ops, QStringLiteral("type"))
                                       .toString()
                                       .simplified()
                                       .split(' ');
            if (xml.name() == QStringLiteral("nav")) {
                landmarks = xml.isStartElement() && types.contains("landmarks");
                toc = xml.isStartElement() && types.contains("toc");
            }
            if (toc && xml.name() == QStringLiteral("li")) {
                if (xml.isStartElement()) {
                    if (source.tocAnchors.size() < maximumChapterEntries) {
                        parents.append(source.tocAnchors.size());
                        source.tocAnchors.append(ReaderSource::TocAnchor{});
                    } else
                        parents.append(-1);
                } else if (xml.isEndElement() && !parents.isEmpty())
                    parents.removeLast();
            }
            if (toc && !parents.isEmpty() && parents.last() >= 0 && xml.isStartElement()
                && xml.name() == QStringLiteral("a")) {
                const auto ref = reference(nav, xml.attributes().value("href").toString(), 0);
                auto &anchor = source.tocAnchors[parents.last()];
                anchor.path = ref.path;
                anchor.id = ref.id;
            }
            if (landmarks && xml.isStartElement() && xml.name() == QStringLiteral("a")
                && (semantics(types) || narrativeType(types)))
                refs.append(reference(nav, xml.attributes().value("href").toString(),
                                      semantics(types) | narrativeType(types)));
        }
        if (xml.hasError()) {
            refs.clear();
            source.tocAnchors.clear();
        }
    }

    if (source.tocAnchors.isEmpty() && ncx.contains(ncxId)) {
        const auto path = ncx.value(ncxId);
        QXmlStreamReader xml(resource(source, path));
        QVector<int> parents;
        while (!xml.atEnd()) {
            stopPoint();
            nextToken(xml);
            const auto name = xml.name().toString();
            if (xml.isStartElement() && name == "navPoint") {
                if (source.tocAnchors.size() >= maximumChapterEntries)
                    break;
                parents.append(source.tocAnchors.size());
                source.tocAnchors.append(ReaderSource::TocAnchor{});
            } else if (xml.isEndElement() && name == "navPoint" && !parents.isEmpty())
                parents.removeLast();
            else if (!parents.isEmpty() && xml.isStartElement()) {
                auto &anchor = source.tocAnchors[parents.last()];
                if (name == "content") {
                    const auto ref = reference(path, xml.attributes().value("src").toString(), 0);
                    anchor.path = ref.path;
                    anchor.id = ref.id;
                }
            }
        }
        if (xml.hasError())
            source.tocAnchors.clear();
    }
    QHash<QString, QMultiHash<QString, int>> anchors;
    for (int row = 0; row < source.tocAnchors.size(); ++row) {
        const auto &anchor = source.tocAnchors[row];
        if (!anchor.id.isEmpty())
            anchors[anchor.path].insert(anchor.id, row);
    }
    QHash<QString, QHash<QString, int>> references;
    for (const auto &ref : refs)
        references[ref.path][ref.id] |= ref.boundary;
    QPair<QString, QString> result;
    NarrativeScan narrative;
    for (int i = 0; i < spine.size(); ++i) {
        stopPoint();
        const auto &path = spine[i];
        auto targets = references.value(path);
        const int flags = targets.take(QString());
        const bool linear = epub3 && !nonLinear.value(i);
        const auto before = narrative.offset;
        if (flags) {
            const auto position = fragment(i);
            record(result, flags, position);
            if (linear && (flags & (Start | After)))
                narrative.boundaries.insert(position, before);
        }
        const auto ids = anchors.value(path);
        if (epub3 || !targets.isEmpty()) {
            const auto bytes = resource(source, path);
            // A namespace URI can contain character references. Include any
            // candidate prefixed type when TOC links used to force this parse.
            const bool mayHaveSemantics = bytes.contains(char(0))
                    || ((!ids.isEmpty() || bytes.contains("http://www.idpf.org/2007/ops"))
                        && mayHaveNamespacedType(bytes))
                    || (!ids.isEmpty() && bytes.contains("<!ENTITY"));
            if (!targets.isEmpty() || mayHaveSemantics || (flags & (Prologue | Epilogue))) {
                FragmentAnchors found;
                scanSpineItem(found, i, bytes, targets, ids, epub3, result,
                              linear ? &narrative : nullptr, flags,
                              i + 1 < spine.size() ? fragment(i + 1) : QStringLiteral(""));
                if (narrative.offset == before)
                    ++narrative.offset;
                found.publish(source);
                continue;
            }
        }
        ++narrative.offset;
        if (!ids.isEmpty()) {
            auto &pending = source.pendingTocFragments[path];
            pending.ids = ids;
            pending.items.append(i);
        }
    }
    narrative.extend(result);
    return result;
}
} // namespace

bool EngineDocument::resolveTocFragments(int item) const
{
    if (!source_ || source_->pendingTocFragments.isEmpty())
        return true;
    const auto path = spineItems().value(item);
    const auto pending = source_->pendingTocFragments.value(path);
    if (pending.items.isEmpty())
        return true;
    const auto scan = [&] {
        const auto bytes = resource(*source_, path);
        if (bytes.isEmpty() || engineCancellationRequested())
            return false;
        FragmentAnchors found;
        QPair<QString, QString> unused;
        for (const int index : pending.items)
            scanSpineItem(found, index, bytes, {}, pending.ids, false, unused);
        if (engineCancellationRequested())
            return false;
        found.publish(*source_);
        source_->pendingTocFragments.remove(path);
        return true;
    };
#ifdef KOBLAZE_CANCEL_CHECK
    try {
        return scan();
    } catch (const KoblazeRenderCancelled &) {
        return false;
    }
#else
    return scan();
#endif
}

QString EngineDocument::chapterBoundaryPosition(const QVector<ChapterLink> &links, int index,
                                                bool *deferred) const
{
    const auto &link = links[index];
    if (!source_ || source_->tocAnchors.isEmpty() || externalChapters_)
        return link.position;
    const int item = spineItemIndex(link.position);
    const bool unloaded = windowFirst_ >= 0 && (item < windowFirst_ || item > windowLast_);
    if (source_->tocAnchors.size() == links.size()) {
        // A group without its own link inherits the destination of its first child.
        int target = index;
        while (source_->tocAnchors[target].path.isEmpty() && target + 1 < links.size()
               && links[target + 1].depth > link.depth)
            ++target;
        const auto &anchor = source_->tocAnchors[target];
        if (!anchor.path.isEmpty() && spineItems().value(item) == anchor.path) {
            if (anchor.id.isEmpty())
                return link.position;
            if (!resolveTocFragments(item)) {
                if (deferred)
                    *deferred = true;
                return {};
            }
            if (!anchor.position.isEmpty())
                return anchor.position;
        }
    }
    // A window placeholder cannot establish which fragment the publisher targeted.
    return unloaded ? QString() : link.position;
}

QPair<QString, QString> EngineDocument::mainTextHints() const
{
    if (!source_ || source_->parent || source_->container.isNull())
        return {};
    if (!source_->mainTextChecked) {
        source_->mainTextHints = detect(*source_, spineItems(), nonLinearSpineItems());
        source_->mainTextChecked = true;
    }
    return source_->mainTextHints;
}
} // namespace kobalt
