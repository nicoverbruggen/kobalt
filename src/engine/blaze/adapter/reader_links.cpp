#include "column_view.h"
#include "stop_point.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "engine_document.h"
#include "reader_source.h"
#include "engine/engine_activity.h"
#include <lvdocview.h>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>

namespace kobalt {
void EngineDocument::markKoboLinkDecorations()
{
    auto *document = engine_->getDocument();
    const auto attribute = document->getAttrNameIndex("data-kobalt-kobo-link");
    QVector<ldomNode *> pending{ document->getRootNode() };
    while (!pending.isEmpty()) {
        stopPoint();
        auto *node = pending.takeLast();
        if (!node || !node->isElement())
            continue;
        if (node->hasAttribute(attribute))
            node->setAttributeValue(0, attribute, U"");
        if (node->isNodeName("a") && !node->getAttributeValue("href").empty()) {
            bool wrapped = false, otherContent = false;
            for (int i = 0; i < node->getChildCount(); ++i) {
                auto *child = node->getChildNode(i);
                if (child->isText()) {
                    otherContent |= !QString::fromUtf8(UnicodeToUtf8(child->getText()).c_str())
                                             .trimmed()
                                             .isEmpty();
                } else {
                    const auto classes =
                            QString::fromUtf8(
                                    UnicodeToUtf8(child->getAttributeValue("class")).c_str())
                                    .split(QRegularExpression("\\s+"), qt::skipEmptyParts);
                    const bool wrapper =
                            child->isNodeName("se-span") && classes.contains("kobo-link");
                    wrapped |= wrapper;
                    otherContent |= !wrapper;
                }
            }
            // Decorations propagate from the anchor through inline children.
            // Mark the anchor only when wrappers cover all its visible content.
            // This changes the in-memory DOM, never the publication bytes.
            if (wrapped && !otherContent)
                node->setAttributeValue(0, attribute, U"1");
        }
        for (int i = node->getChildCount() - 1; i >= 0; --i)
            pending.append(node->getChildNode(i));
    }
}

namespace {
constexpr int noteNodeLimit = 8192;
constexpr int noteDepthLimit = 64;
constexpr int noteHtmlLimit = 128 * 1024;
constexpr int inferredNoteTextLimit = 10000;

QString text(const lString32 &value)
{
    return QString::fromUtf8(UnicodeToUtf8(value).c_str());
}

lString32 domText(const QString &value)
{
    return Utf8ToUnicode(value.toUtf8().constData());
}

// Returns -1 when the source cannot establish whether the target exists.
// Read only the linked chapter, and stop as soon as its id is found. Chunked
// reads keep cancellation checks between storage reads, also for a long note.
int linkTargetExists(ReaderSource &source, const QString &path, const QString &id,
                     const QString &target, const std::function<bool()> &interrupted)
{
    const auto known = source.linkTargets.constFind(target);
    if (known != source.linkTargets.cend())
        return known.value() ? 1 : 0;
    if (source.container.isNull() || path.isEmpty())
        return -1;
    auto stream = source.container->OpenStream(domText(path).c_str(), LVOM_READ);
    if (stream.isNull())
        return -1;
    QXmlStreamReader xml;
    const auto remember = [&](bool exists) {
        if (source.linkTargets.size() >= 4096)
            source.linkTargets.clear();
        source.linkTargets.insert(target, exists);
        return exists ? 1 : 0;
    };
    const lvsize_t total = stream->GetSize();
    lvsize_t received = 0;
    while (received < total) {
        engineActivity().checkpoint();
        stopPoint();
        if (interrupted && interrupted())
            return -1;
        QByteArray chunk;
        chunk.resize(int(qMin<lvsize_t>(16384, total - received)));
        lvsize_t count = 0;
        if (stream->Read(chunk.data(), chunk.size(), &count) != LVERR_OK || !count)
            return -1;
        received += count;
        chunk.resize(int(count));
        xml.addData(chunk);
        while (!xml.atEnd()) {
            stopPoint();
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            const auto attributes = xml.attributes();
            for (const auto &attribute : attributes)
                if ((attribute.name() == QLatin1String("id")
                     || (xml.name() == QLatin1String("a")
                         && attribute.name() == QLatin1String("name")))
                    && attribute.value() == id)
                    return remember(true);
        }
        if (xml.hasError() && xml.error() != QXmlStreamReader::PrematureEndOfDocumentError)
            return -1;
    }
    return xml.hasError() ? -1 : remember(false);
}

bool token(ldomNode *node, const char *name, const char *value, bool anyNamespace = false)
{
    const auto attribute = anyNamespace
            ? node->getAttributeValue(LXML_NS_ANY,
                                      node->getDocument()->getAttrNameIndex(
                                              domText(QString::fromLatin1(name)).c_str()))
            : node->getAttributeValue(name);
    return text(attribute)
            .split(QRegularExpression("\\s+"), kobalt::qt::skipEmptyParts)
            .contains(value, Qt::CaseInsensitive);
}

bool noteElement(ldomNode *node)
{
    return token(node, "type", "footnote", true) || token(node, "type", "endnote", true)
            || token(node, "type", "note", true) || token(node, "type", "rearnote", true)
            || token(node, "role", "doc-footnote") || token(node, "role", "doc-endnote")
            || token(node, "role", "doc-note") || token(node, "role", "doc-rearnote")
            || token(node, "class", "footnote") || token(node, "class", "endnote");
}

enum class NoteMatch { Unmarked, Explicit, Passage };

NoteMatch noteReference(ldomNode *node)
{
    const auto style = node->getStyle();
    if (!style.isNull()) {
        if (STYLE_HAS_CR_HINT(style, NOTEREF)) {
            return NoteMatch::Explicit;
        }
        if (STYLE_HAS_CR_HINT(style, NOTEREF_IGNORE)) {
            return NoteMatch::Passage;
        }
    }
    if (token(node, "type", "noteref", true) || token(node, "role", "doc-noteref")
        || token(node, "rel", "footnote") || token(node, "type", "note")) {
        return NoteMatch::Explicit;
    }
    if (token(node, "type", "link", true) || token(node, "role", "doc-link")) {
        return NoteMatch::Passage;
    }
    return NoteMatch::Unmarked;
}

bool heading(ldomNode *node)
{
    const auto name = node->getNodeName();
    return name.length() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

// The same reference shapes as KOReader's normal popup detector. This is an
// independent implementation; the upstream rules and revision are in development.md.
bool noteMarker(const lString32 &label)
{
    int digits = 0, letters = 0, romans = 0;
    for (int i = 0; i < label.length(); ++i) {
        const auto c = label[i];
        if (lStr_isWordSeparator(c)) {
            continue;
        }
        const auto properties = lGetCharProps(c);
        if (properties & CH_PROP_DIGIT) {
            ++digits;
        } else if (properties & CH_PROP_ALPHA) {
            ++letters;
            if (c == 'i' || c == 'v' || c == 'x' || c == 'l' || c == 'c' || c == 'd' || c == 'm'
                || c == 'I' || c == 'V' || c == 'X' || c == 'L' || c == 'C' || c == 'D'
                || c == 'M') {
                ++romans;
            }
        } else {
            return false;
        }
    }
    return (digits >= 1 && digits <= 3 && letters == 0)
            || (letters >= 1 && letters <= 2 && digits <= 2)
            || (romans >= 1 && romans <= 10 && romans == letters && digits == 0);
}

// Read only enough text to compare the link with its containing paragraph.
// A traversal that reaches the node or depth limit cannot establish a match.
bool plainText(ldomNode *node, lString32 &value, int limit, int &visited, int depth = 0,
               bool replaceImages = false)
{
    engineActivity().checkpoint();
    if (++visited > noteNodeLimit || depth > noteDepthLimit) {
        visited = noteNodeLimit + 1;
        return false;
    }
    if (node->isText()) {
        const auto part = node->getText();
        if (part.length() > limit - value.length()) {
            return false;
        }
        value += part;
        return true;
    }
    if (replaceImages && (node->isNodeName("img") || node->isNodeName("svg"))) {
        if (value.length() == limit) {
            return false;
        }
        value += U"Z";
        return true;
    }
    for (int i = 0; i < node->getChildCount(); ++i) {
        if (!plainText(node->getChildNode(i), value, limit, visited, depth + 1, replaceImages)) {
            return false;
        }
    }
    return true;
}

bool shiftedReference(ldomNode *source, ldomNode *block)
{
    auto *parent = source->getParentNode();
    for (int i = 0; parent && i < 2; ++i, parent = parent->getParentNode()) {
        if (parent->isNodeName("sup") || parent->isNodeName("sub")) {
            return true;
        }
    }
    bool onlySupSub = false;
    for (int i = 0; i < source->getChildCount(); ++i) {
        auto *child = source->getChildNode(i);
        if (child->isText() && child->getText().trim().empty()) {
            continue;
        }
        if (!child->isNodeName("sup") && !child->isNodeName("sub")) {
            onlySupSub = false;
            break;
        }
        onlySupSub = true;
    }
    if (onlySupSub) {
        return true;
    }
    ldomXPointerEx current(source, 0), end(source, 0);
    end.lastInnerTextNode(true);
    int visited = 0;
    bool seen = false;
    while (current.nextText() && current.compare(end) <= 0) {
        engineActivity().checkpoint();
        if (++visited > noteNodeLimit) {
            return false;
        }
        if (current.getNode()->getText().trim().empty()) {
            continue;
        }
        seen = true;
        bool shifted = false;
        int ancestors = 0;
        for (auto *node = current.getNode()->getParentNode(); node; node = node->getParentNode()) {
            engineActivity().checkpoint();
            if (++ancestors > noteDepthLimit) {
                return false;
            }
            const auto style = node->getStyle();
            if (!style.isNull()) {
                const auto alignment = style->vertical_align;
                shifted = alignment.type == css_val_unspecified
                        ? alignment.value != css_va_inherit && alignment.value != css_va_baseline
                        : alignment.value != 0;
                if (shifted) {
                    break;
                }
            }
            if (node == block) {
                break;
            }
        }
        if (!shifted) {
            return false;
        }
    }
    return seen;
}

bool chapterTarget(LVTocItem *root, ldomNode *target)
{
    auto *item = root;
    int visited = 0;
    while (item) {
        engineActivity().checkpoint();
        if (++visited > noteNodeLimit || item->getXPointer().getNode() == target) {
            return true;
        }
        if (item->getChildCount()) {
            item = item->getChild(0);
            continue;
        }
        while (item && item != root && item->getIndex() + 1 == item->getParent()->getChildCount()) {
            item = item->getParent();
        }
        if (!item || item == root) {
            break;
        }
        item = item->getParent()->getChild(item->getIndex() + 1);
    }
    return false;
}

bool inferredNote(ldomNode *source, ldomNode *target, LVTocItem *toc)
{
    const ldomXPointerEx reference(source, 0), destination(target, 0);
    if (target->isNodeName("DocFragment") || target->isNodeName("body")
        || target->isNodeName("html") || reference.compare(destination) > 0) {
        return false;
    }
    lString32 label, paragraph;
    int visited = 0;
    if (!plainText(source, label, inferredNoteTextLimit, visited)) {
        return false;
    }
    const bool hasText = !text(label).trimmed().isEmpty();
    if (!hasText) {
        // Some books use an image for a reference. It supplies content for
        // the paragraph check, but needs superscript/subscript evidence.
        label.clear();
        visited = 0;
        if (!plainText(source, label, inferredNoteTextLimit, visited, 0, true)
            || text(label).trimmed().isEmpty()) {
            return false;
        }
    }
    auto *block = reference.getFinalNode();
    if (block) {
        visited = 0;
        const bool complete = plainText(block, paragraph, label.length(), visited);
        if (visited > noteNodeLimit || (complete && paragraph == label)) {
            return false;
        }
    }
    // Most internal links are words or chapter titles. Check the reference
    // first so those links need no TOC or target-subtree scan.
    if ((!(hasText && noteMarker(label)) && !shiftedReference(source, block))
        || chapterTarget(toc, target)) {
        return false;
    }
    int ancestors = 0;
    for (auto *parent = target; parent; parent = parent->getParentNode()) {
        engineActivity().checkpoint();
        if (++ancestors > noteDepthLimit) {
            return false;
        }
        if (heading(parent)) {
            return false;
        }
    }
    ldomXPointerEx current(target, 0), end(target, 0);
    end.lastInnerNode();
    visited = 0;
    do {
        engineActivity().checkpoint();
        if (++visited > noteNodeLimit || heading(current.getNode())) {
            return false;
        }
    } while (current.nextElement() && current.compare(end) <= 0);
    return true;
}

bool pageBreak(css_page_break_t value)
{
    return value == css_pb_always || value == css_pb_left || value == css_pb_right;
}

// A block that starts another note or a new section.
bool endsNote(ldomNode *node, int &visited, int depth = 0, bool descendants = true)
{
    engineActivity().checkpoint();
    if (++visited > noteNodeLimit || depth > noteDepthLimit) {
        visited = noteNodeLimit + 1;
        return true;
    }
    const auto tag = text(node->getNodeName()).toLower();
    if (tag == "section" || tag == "aside" || tag == "hr"
        || (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6')
        || !node->getAttributeValue("id").empty()
        || (node->isNodeName("a") && !node->getAttributeValue("name").empty())
        || noteElement(node)) {
        return true;
    }
    const auto style = node->getStyle();
    if (!style.isNull() && pageBreak(style->page_break_before)) {
        return true;
    }
    for (int i = 0; descendants && i < node->getChildCount(); ++i) {
        auto *child = node->getChildNode(i);
        if (child->isElement() && endsNote(child, visited, depth + 1)) {
            return true;
        }
    }
    return false;
}

QVector<ldomNode *> noteParts(ldomNode *target, bool contained)
{
    auto *note = target;
    if (!contained) {
        auto *block = ldomXPointerEx(target, 0).getFinalNode();
        if (block) {
            note = block;
        }
    }
    QVector<ldomNode *> parts;
    if (!note->isNodeName("body") && !note->isNodeName("DocFragment")
        && !note->isNodeName("html")) {
        parts.append(note);
    }
    if (contained || (note->getChildCount() && !ldomXPointerEx(note, 0).isFinalNode())) {
        return parts;
    }
    ldomXPointerEx current(note, 0);
    const auto style = note->getStyle();
    if ((!style.isNull() && pageBreak(style->page_break_after)) || !current.nextOuterElement()) {
        return parts;
    }
    int visited = 0;
    ldomXPointerEx stop;
    do {
        engineActivity().checkpoint();
        auto *node = current.getNode();
        if (++visited > noteNodeLimit) {
            return {};
        }
        if ((!stop.isNull() && current.compare(stop) >= 0) || node->isNodeName("DocFragment")
            || node->isNodeName("body")) {
            break;
        }
        // Inspect a block before adding it, so an ID inside it starts the next
        // note. Plain wrappers can contain further paragraphs of this note.
        if (endsNote(node, visited, 0, current.isFinalNode())) {
            if (visited > noteNodeLimit) {
                return {};
            }
            break;
        }
        if (current.isFinalNode()) {
            parts.append(node);
            const auto nextStyle = node->getStyle();
            if ((!nextStyle.isNull() && pageBreak(nextStyle->page_break_after))
                || !current.nextOuterElement()) {
                break;
            }
        } else {
            const auto wrapperStyle = node->getStyle();
            if (!wrapperStyle.isNull() && pageBreak(wrapperStyle->page_break_after)) {
                auto after = current;
                if (after.nextOuterElement() && (stop.isNull() || after.compare(stop) < 0)) {
                    stop = after;
                }
            }
            if (!current.nextElement()) {
                break;
            }
        }
    } while (true);
    return parts;
}

struct NoteBudget
{
    int nodes = 0, characters = 0;
    int textLimit = noteHtmlLimit;
    bool hasText = false;
};

// Copy note text and basic structure only. Publisher styles, scripts and
// resources must not reach the popup's QTextDocument or start external I/O.
bool noteHtml(ldomNode *node, QString &html, NoteBudget &budget, int depth = 0)
{
    engineActivity().checkpoint();
    if (!node || ++budget.nodes > noteNodeLimit || depth > noteDepthLimit
        || html.size() > noteHtmlLimit) {
        return false;
    }
    if (node->isText()) {
        const auto value = node->getText();
        if (value.length() > budget.textLimit - budget.characters) {
            return false;
        }
        budget.characters += value.length();
        budget.hasText |= !text(value).trimmed().isEmpty();
        html += text(value).toHtmlEscaped();
        return html.size() <= noteHtmlLimit;
    }
    const auto tag = text(node->getNodeName()).toLower();
    if (tag == "script" || tag == "style" || tag == "head")
        return true;
    if (tag == "img" || tag == "svg") {
        const auto alt = text(node->getAttributeValue("alt")).left(1024);
        html += "["
                + (alt.isEmpty() ? strings::get("common.image").toHtmlEscaped()
                                 : alt.toHtmlEscaped())
                + "]";
        return html.size() <= noteHtmlLimit;
    }
    static const QSet<QString> allowed{ "p",     "div",   "span", "b",   "strong",     "i",
                                        "em",    "u",     "s",    "sub", "sup",        "br",
                                        "hr",    "ul",    "ol",   "li",  "blockquote", "pre",
                                        "code",  "table", "tr",   "td",  "th",         "thead",
                                        "tbody", "tfoot", "dl",   "dt",  "dd" };
    const auto output = allowed.contains(tag) ? tag
            : tag == "aside" || tag == "section"
                    || (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6')
            ? QStringLiteral("div")
            : QString();
    if (!output.isEmpty())
        html += "<" + output + ">";
    for (int i = 0; i < node->getChildCount(); ++i)
        if (!noteHtml(node->getChildNode(i), html, budget, depth + 1))
            return false;
    if (!output.isEmpty() && output != "br" && output != "hr")
        html += "</" + output + ">";
    return html.size() <= noteHtmlLimit;
}
} // namespace

void EngineDocument::markPageNotes()
{
    auto *document = engine_->getDocument();
    if (document->getProps()->getIntDef("kobalt.page-notes.version", 0) == 1)
        return;
    const auto marker = document->getAttrNameIndex("data-kobalt-page-note");
    bool changed = false;
    QVector<ldomNode *> groups;
    QVector<ldomNode *> pending{ document->getRootNode() };
    while (!pending.isEmpty()) {
        stopPoint();
        engineActivity().checkpoint();
        auto *node = pending.takeLast();
        if (!node || !node->isElement())
            continue;
        // Source-supplied markers must not turn ordinary text into page notes.
        if (node->hasAttribute(marker)) {
            node->setAttributeValue(0, marker, U"");
            changed = true;
        }
        auto *parent = node->getParentNode();
        if (node->isNodeName("div") && token(node, "class", "footnote") && parent
            && token(parent, "class", "footnotes"))
            groups.append(node);
        for (int i = node->getChildCount() - 1; i >= 0; --i)
            pending.append(node->getChildNode(i));
    }
    // Gutenberg sometimes puts several notes in one .footnote container.
    // A labeled backlink starts each note; unlabeled paragraphs continue it.
    for (auto *group : groups) {
        bool started = false;
        for (int i = 0; i < group->getChildCount(); ++i) {
            auto *paragraph = group->getChildNode(i);
            if (!paragraph->isNodeName("p"))
                continue;
            bool label = false;
            QVector<ldomNode *> children{ paragraph };
            while (!children.isEmpty()) {
                engineActivity().checkpoint();
                auto *child = children.takeLast();
                const auto href = child->getAttributeValue("href");
                if (child->isNodeName("a") && token(child, "class", "label")
                    && !child->getAttributeValue("id").empty() && !href.empty() && href[0] == '#')
                    label = true;
                for (int j = 0; j < child->getChildCount(); ++j)
                    children.append(child->getChildNode(j));
            }
            if (label || started) {
                paragraph->setAttributeValue(0, marker, label ? U"start" : U"extend");
                changed = true;
                started = true;
                group->setAttributeValue(0, marker, U"box");
                group->getParentNode()->setAttributeValue(0, marker, U"box");
            }
        }
    }
    document->getProps()->setInt("kobalt.page-notes.version", 1);
    document->setCacheFileStale(true);
    if (changed) {
        document->forceReinitStyles();
        engine_->requestRender();
    }
}

QVector<PageLink> EngineDocument::pageLinks()
{
    QVector<PageLink> result;
    if (!engine_)
        return result;
    ldomXRangeList links;
    engine_->getCurrentPageLinks(links);
    // CREngine's page range contains the main flow only. Include links from
    // the note fragments that it draws below that flow, including backlinks.
    if (inpageFootnotes_) {
        class NoteLinks : public ldomNodeCallback
        {
        public:
            explicit NoteLinks(ldomXRangeList &links) : links(links) { }

            bool onElement(ldomXPointerEx *pointer) override
            {
                add(pointer->getNode());
                return true;
            }

            void onText(ldomXRange *range) override { add(range->getStart().getNode()); }

        private:
            void add(ldomNode *node)
            {
                while (node && !node->isNodeName("a"))
                    node = node->getParentNode();
                if (!node || !node->hasChildren())
                    return;
                for (int i = 0; i < links.length(); ++i)
                    if (links[i]->getStart().getNode() == node)
                        return;
                links.add(new ldomXRange(node, true));
            }

            ldomXRangeList &links;
        } collect(links);

        const int first = engine_->getCurPage() * columns_;
        for (int column = 0; column < columns_ && first + column < engine_->getPageList()->length();
             ++column) {
            const auto &page = *engine_->getPageList()->operator[](first + column);
            for (int i = 0; i < page.footnotes.length(); ++i) {
                const auto &note = page.footnotes[i];
                auto *doc = engine_->getDocument();
                ldomXPointerEx start(doc->createXPointer(lvPoint(0, note.start),
                                                         PT_DIR_SCAN_FORWARD_LOGICAL_FIRST));
                ldomXPointerEx end(doc->createXPointer(
                        lvPoint(engine_->columnWidth(), note.start + note.height - 1),
                        PT_DIR_SCAN_BACKWARD_LOGICAL_LAST));
                if (start.isNull() || end.isNull())
                    continue;
                if (end.isText())
                    end.setOffset(end.getNode()->getText().length());
                ldomXRange range(start, end);
                range.forEach(&collect);
            }
        }
    }
    for (int i = 0; i < links.length(); ++i) {
        engineActivity().checkpoint();
        auto *range = links[i];
        ldomXPointer source;
        if (range->getHRef(source).empty() || source.isNull())
            continue;
        const auto rectangles = rangeRects(text(range->getStart().toString()),
                                           text(range->getEnd().toString()), true);
        if (!rectangles.isEmpty())
            result.append({ text(source.toString()), rectangles,
                            linkDetails(text(source.toString()), false).kind });
    }
    return result;
}

LinkDetails EngineDocument::linkDetails(const QString &source, bool preview)
{
    LinkDetails result;
    if (!engine_)
        return result;
    auto *doc = engine_->getDocument();
    auto pointer = doc->createXPointer(domText(source));
    ldomXPointer link;
    const auto href = pointer.getHRef(link);
    if (href.empty() || link.isNull())
        return result;
    auto *node = link.getNode();
    if (preview)
        result.label = text(node->getText(' ', 256)).trimmed();
    result.kind = LinkKind::Unavailable;
    result.error = strings::get("error.this_passage_is_unavailable");
    if (href[0] != '#') {
        const QUrl url(text(href));
        if (url.isValid() && !url.host().isEmpty()
            && (url.scheme() == "https" || url.scheme() == "http")) {
            result.kind = LinkKind::Website;
            result.target = url.toString();
            result.error.clear();
        }
        return result;
    }
    auto *target = doc->getNodeById(doc->getAttrValueIndex(href.substr(1).c_str()));
    auto match = noteReference(node);
    // In chapter mode the target can be in a spine item that is not loaded.
    // The engine names a link into spine item n "#_doc_fragment_n_ id".
    EngineDocument *targetDocument = this;
    std::unique_ptr<EngineDocument> sideWindow;
    if (!target && windowFirst_ >= 0) {
        static const QRegularExpression fragment(QStringLiteral("^#_doc_fragment_(\\d+)_ "));
        const auto found = fragment.match(text(href));
        const int item = found.hasMatch() ? found.captured(1).toInt() : -1;
        if (item < 0 || (item >= windowFirst_ && item <= windowLast_))
            return result;
        // The kind of a link on the page needs no layout. A missing target
        // stays unavailable; a note without markup remains a guess.
        if (!preview) {
            if (source_
                && linkTargetExists(*source_, spineItems().value(item),
                                    text(href).mid(found.capturedLength()), text(href), interrupt_)
                        == 0)
                return result;
            result.kind = match == NoteMatch::Explicit ? LinkKind::Footnote : LinkKind::Passage;
            result.error.clear();
            return result;
        }
        if (slicePages_ > 0) {
            targetDocument = openAhead(item);
        } else {
            sideWindow = openSide(windowSettings(item, item), {}, nullptr);
            targetDocument = sideWindow.get();
        }
        if (!targetDocument || !targetDocument->engine_)
            return result;
        auto *other = targetDocument->engine_->getDocument();
        target = other->getNodeById(other->getAttrValueIndex(href.substr(1).c_str()));
    }
    if (!target)
        return result;
    auto *note = target;
    const auto targetStyle = target->getStyle();
    if (match == NoteMatch::Unmarked && !targetStyle.isNull()) {
        if (STYLE_HAS_CR_HINT(targetStyle, FOOTNOTE)) {
            match = NoteMatch::Explicit;
        } else if (STYLE_HAS_CR_HINT(targetStyle, FOOTNOTE_IGNORE)) {
            match = NoteMatch::Passage;
        }
    }
    bool contained = false;
    int ancestors = 0;
    for (auto *parent = target;
         match != NoteMatch::Passage && parent && !parent->isNodeName("DocFragment");
         parent = parent->getParentNode()) {
        engineActivity().checkpoint();
        if (++ancestors > noteDepthLimit) {
            break;
        }
        const auto style = parent->getStyle();
        if (noteElement(parent) || (!style.isNull() && STYLE_HAS_CR_HINT(style, FOOTNOTE))) {
            note = parent;
            match = NoteMatch::Explicit;
            contained = true;
            break;
        }
    }
    ldomXPointer destination(target, 0);
    // The destination may be in another slice of this document or of a chapter
    // opened for the link. Such a block has no coordinates until the jump.
    const auto position = text(destination.toString());
    bool navigable = destination.toPoint().y >= 0;
    if (!navigable && targetDocument->slices()) {
        const int offset = targetDocument->textBefore(position);
        navigable = offset >= 0
                && (offset < targetDocument->sliceFirst_ || offset >= targetDocument->sliceEnd_);
        for (auto *parent = target; navigable && parent; parent = parent->getParentNode()) {
            const auto style = parent->getStyle();
            if (!style.isNull() && style->display == css_d_none)
                navigable = false;
        }
    }
    if (navigable) {
        result.target = position;
        if (preview)
            result.chapter = chapterTitle(result.target);
    }
    if (!result.target.isEmpty()) {
        result.kind = LinkKind::Passage;
        result.error.clear();
    }
    const bool inferred =
            match == NoteMatch::Unmarked && inferredNote(node, target, engine_->getToc());
    if (match != NoteMatch::Explicit && !inferred) {
        return result;
    }
    // Explicit notes need no preview serialization when a frame only needs their kind.
    // Inferred notes still use the same bounded-content check as the popup.
    if (!preview && match == NoteMatch::Explicit) {
        result.kind = LinkKind::Footnote;
        result.error.clear();
        return result;
    }
    const auto parts = noteParts(note, contained);
    NoteBudget budget;
    budget.textLimit = inferred ? inferredNoteTextLimit : noteHtmlLimit;
    bool complete = true;
    QString html;
    for (auto *part : parts) {
        complete = complete && noteHtml(part, html, budget);
    }
    if (inferred && (!complete || parts.isEmpty() || !budget.hasText)) {
        return result;
    }
    result.kind = LinkKind::Footnote;
    result.error.clear();
    if (parts.isEmpty()) {
        result.error = strings::get("error.this_note_has_no_separate_preview");
    } else if (!complete) {
        result.error = strings::get("error.this_note_is_too_long_to_preview");
    } else if (html.trimmed().isEmpty()) {
        result.error = strings::get("error.this_note_has_no_text_to_preview");
    } else {
        result.html = html;
    }
    return result;
}
} // namespace kobalt
