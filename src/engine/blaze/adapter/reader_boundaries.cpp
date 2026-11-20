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
enum Boundary { Start = 1, After = 2 };

int semantics(const QStringList &types)
{
    if (types.contains("bodymatter"))
        return Start;
    if (types.contains("backmatter") || types.contains("appendix") || types.contains("index")
        || types.contains("bibliography") || types.contains("glossary"))
        return After;
    return 0;
}

const QString ops = QStringLiteral("http://www.idpf.org/2007/ops");

QByteArray resource(ReaderSource &source, const QString &path)
{
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

struct Reference
{
    QString path, id;
    int boundary;
};

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

QPair<QString, QString> detect(ReaderSource &source, const QStringList &spine)
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
                && semantics(types))
                refs.append(reference(nav, xml.attributes().value("href").toString(),
                                      semantics(types)));
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
    auto record = [&](int flags, const QString &path) {
        if ((flags & Start) && result.first.isEmpty()) {
            result.first = path;
            // A guide index before the body is front matter.
            result.second.clear();
        }
        if ((flags & After) && result.second.isEmpty())
            result.second = path;
    };
    for (int i = 0; i < spine.size(); ++i) {
        stopPoint();
        const auto root = QString("/body/DocFragment[%1]").arg(i + 1);
        auto targets = references.value(spine[i]);
        record(targets.take(QString()), root);
        const auto ids = anchors.value(spine[i]);
        if (!epub3 && targets.isEmpty() && ids.isEmpty())
            continue;
        const auto bytes = resource(source, spine[i]);
        if (targets.isEmpty() && ids.isEmpty() && !bytes.contains("type")
            && !bytes.contains(char(0)))
            continue;
        QXmlStreamReader xml(bytes);
        QVector<QHash<QString, int>> children;
        QStringList steps;
        const auto before = result;
        int elementOrder = 0;
        while (!xml.atEnd()) {
            stopPoint();
            nextToken(xml);
            const auto name = xml.name().toString();
            if (xml.isStartElement()) {
                if (steps.isEmpty() && name != "body")
                    continue;
                ++elementOrder;
                const int ordinal = children.isEmpty() ? 1 : ++children.last()[name];
                steps.append(QString("/%1[%2]").arg(name).arg(ordinal));
                children.append(QHash<QString, int>());
                const auto attrs = xml.attributes();
                const auto types =
                        attrs.value(ops, QStringLiteral("type")).toString().simplified().split(' ');
                const auto id = attrs.value("id").toString();
                int flags = targets.value(id);
                if (!id.isEmpty() && ids.contains(id))
                    for (const int row : ids.values(id))
                        source.tocAnchors[row].position = root + steps.join(QString());
                if (epub3
                    && (name == "body" || name == "section" || name == "div"
                        || name == "article")) {
                    // A glossary inside a chapter is local to that chapter. Only a
                    // partition of the publication or a landmark can end the main text.
                    if (types.contains("backmatter") || types.contains("bodymatter")
                        || steps.size() <= 2)
                        flags |= semantics(types);
                    if (types.contains("chapter") || types.contains("part"))
                        result.second.clear();
                }
                if (flags || ids.contains(id)) {
                    const auto position = root + steps.join(QString());
                    source.boundaryElementOrder.insert(position, elementOrder);
                    if (flags)
                        record(flags, position);
                }
            } else if (xml.isEndElement() && !steps.isEmpty()) {
                steps.removeLast();
                children.removeLast();
            }
        }
        if (xml.hasError())
            result = before;
    }
    return result;
}
} // namespace

QString EngineDocument::chapterBoundaryPosition(const QVector<ChapterLink> &links, int index) const
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
            if (!anchor.position.isEmpty())
                return anchor.position;
            if (anchor.id.isEmpty())
                return link.position;
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
        source_->mainTextHints = detect(*source_, spineItems());
        source_->mainTextChecked = true;
    }
    return source_->mainTextHints;
}
} // namespace kobalt
