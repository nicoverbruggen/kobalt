#include "column_view.h"
#include "engine_document.h"
#include "reader_locations.h"
#include <lvdocview.h>

namespace kobalt {
namespace {
QString qstring(const lString32 &s)
{
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}

lString32 cre(const QString &s)
{
    return Utf8ToUnicode(s.toUtf8().constData());
}

int countedBefore(const lString32 &text, int end)
{
    int count = 0;
    for (int i = 0; i < end; ++i)
        count += !locationUncounted(text[i]);
    return count;
}

template <typename Visit>
bool eachSourceElement(ldomNode *parent, const Visit &visit)
{
    for (int i = 0; i < parent->getChildCount(); ++i) {
        auto *child = parent->getChildNode(i);
        if (!child->isElement() || enginelocation::generated(child))
            continue;
        if (enginelocation::wrapper(child)) {
            if (!eachSourceElement(child, visit))
                return false;
        } else if (!visit(child)) {
            return false;
        }
    }
    return true;
}

template <typename Visit>
bool eachSourceText(ldomNode *element, const Visit &visit)
{
    for (int i = 0; i < element->getChildCount(); ++i) {
        auto *child = element->getChildNode(i);
        if (child->isText()) {
            if (!visit(child))
                return false;
        } else if (!enginelocation::generated(child) && !eachSourceText(child, visit)) {
            return false;
        }
    }
    return true;
}

ldomNode *fragmentOf(ldomNode *node)
{
    while (node && !node->isNodeName("DocFragment"))
        node = node->getParentNode();
    return node;
}

ldomNode *fragmentFor(ldomDocument *doc, const QString &href)
{
    auto *root = doc->nodeFromXPath(U"/body");
    if (!root)
        return nullptr;
    for (int i = 0; i < root->getChildCount(); ++i) {
        auto *node = root->getChildNode(i);
        if (node->isElement() && node->isNodeName("DocFragment")
            && qstring(node->getAttributeValue("Source")) == href)
            return node;
    }
    return nullptr;
}
} // namespace

namespace enginelocation {
bool wrapper(ldomNode *node)
{
    for (const char *name :
         { "autoBoxing", "tabularBox", "rubyBox", "mathBox", "floatBox", "inlineBox" })
        if (node->isNodeName(name))
            return true;
    return false;
}

bool generated(ldomNode *node)
{
    return node->isNodeName("pseudoElem");
}

ldomNode *body(ldomNode *fragment)
{
    ldomNode *found = nullptr;
    for (int i = 0; i < fragment->getChildCount(); ++i) {
        auto *child = fragment->getChildNode(i);
        // CREngine keeps the styles of the spine item in a <stylesheet>
        // element beside <body>.
        if (!child->isElement() || child->isNodeName("stylesheet"))
            continue;
        if (found || !child->isNodeName("body"))
            return nullptr;
        found = child;
    }
    return found;
}

ldomNode *sourceParent(ldomNode *node)
{
    auto *parent = node->getParentNode();
    while (parent && wrapper(parent))
        parent = parent->getParentNode();
    return parent;
}

bool elementPath(ldomNode *body, ldomNode *element, QVector<int> &path)
{
    path.clear();
    for (auto *node = element; node != body;) {
        if (!node || !node->isElement() || wrapper(node) || generated(node))
            return false;
        auto *parent = sourceParent(node);
        if (!parent)
            return false;
        int index = 0;
        bool found = false;
        eachSourceElement(parent, [&](ldomNode *child) {
            found = child == node;
            index += !found;
            return !found;
        });
        if (!found)
            return false;
        path.prepend(index);
        node = parent;
    }
    return true;
}

ldomNode *elementAt(ldomNode *body, const QVector<int> &path)
{
    auto *node = body;
    for (const int step : path) {
        ldomNode *next = nullptr;
        int index = 0;
        eachSourceElement(node, [&](ldomNode *child) {
            if (index++ != step)
                return true;
            next = child;
            return false;
        });
        if (!next)
            return nullptr;
        node = next;
    }
    return node;
}

QVector<ldomNode *> textNodes(ldomNode *element)
{
    QVector<ldomNode *> nodes;
    eachSourceText(element, [&](ldomNode *text) {
        nodes.append(text);
        return true;
    });
    return nodes;
}
} // namespace enginelocation

QStringList EngineDocument::spineItems() const
{
    QStringList items;
    auto *root = engine_ ? engine_->getDocument()->nodeFromXPath(U"/body") : nullptr;
    for (int i = 0; root && i < root->getChildCount(); ++i) {
        auto *node = root->getChildNode(i);
        if (node->isElement() && node->isNodeName("DocFragment"))
            items.append(qstring(node->getAttributeValue("Source")));
    }
    return items;
}

int EngineDocument::spineItemOf(const QString &position) const
{
    Location location;
    return locationOf(position, &location) ? int(spineItems().indexOf(location.href)) : -1;
}

bool EngineDocument::locationOf(const QString &position, Location *location, bool *exact) const
{
    using namespace enginelocation;
    if (exact)
        *exact = false;
    if (!engine_ || position.isEmpty())
        return false;
    const auto pointer = engine_->getDocument()->createXPointer(cre(position));
    auto *node = pointer.getNode();
    auto *fragment = fragmentOf(node);
    if (!fragment)
        return false;
    Location result;
    result.href = qstring(fragment->getAttributeValue("Source"));
    if (result.href.isEmpty())
        return false;
    auto *root = body(fragment);
    if (!root)
        return false;
    if (node == fragment || node == root) {
        if (exact)
            *exact = node == fragment && pointer.getOffset() == 0;
        *location = result;
        return true;
    }
    auto *element = node->isText() ? node->getParentNode() : node;
    while (element && wrapper(element))
        element = element->getParentNode();
    if (!element || !elementPath(root, element, result.path))
        return false;
    if (!node->isText()) {
        // An element position can name one of its children. The element is
        // the nearest position that the source also has.
        if (exact)
            *exact = pointer.getOffset() == 0 && element == node;
        *location = result;
        return true;
    }
    const auto text = node->getText();
    const int offset = pointer.getOffset();
    if (offset < 0 || offset > text.length())
        return false;
    int counted = 0;
    bool found = false;
    eachSourceText(element, [&](ldomNode *candidate) {
        found = candidate == node;
        const auto content = found ? text : candidate->getText();
        counted += countedBefore(content, found ? offset : content.length());
        return !found;
    });
    // Text below generated content is not in the source.
    if (!found)
        return false;
    result.offset = counted;
    const bool before = offset < text.length() && !locationUncounted(text[offset]);
    const bool behind = offset > 0 && !locationUncounted(text[offset - 1]);
    result.after = before || !behind;
    if (exact)
        *exact = before || behind;
    *location = result;
    return true;
}

QString EngineDocument::positionOf(const Location &location, Restoration *restoration) const
{
    using namespace enginelocation;
    auto finish = [restoration](Restoration value, ldomXPointer pointer) {
        if (restoration)
            *restoration = value;
        return value == Restoration::Unresolved ? QString() : qstring(pointer.toString());
    };
    if (!engine_ || !location.valid())
        return finish(Restoration::Unresolved, {});
    auto *fragment = fragmentFor(engine_->getDocument(), location.href);
    auto *root = fragment ? body(fragment) : nullptr;
    if (!root)
        return finish(Restoration::Unresolved, {});
    if (location.path.isEmpty() && location.offset < 0)
        return finish(Restoration::Exact, ldomXPointer(fragment, 0));
    auto *element = elementAt(root, location.path);
    if (!element)
        return finish(Restoration::Unresolved, {});
    if (location.offset < 0)
        return finish(Restoration::Exact, ldomXPointer(element, 0));
    const auto nodes = textNodes(element);
    if (nodes.isEmpty())
        return finish(Restoration::Approximate, ldomXPointer(element, 0));
    // The counted character that the position touches: the one after it, or
    // the one before it.
    const int target = location.after ? location.offset : location.offset - 1;
    if (target < 0)
        return finish(Restoration::Exact, ldomXPointer(nodes.first(), 0));
    int counted = 0;
    for (auto *node : nodes) {
        const auto text = node->getText();
        for (int i = 0; i < text.length(); ++i) {
            if (locationUncounted(text[i]))
                continue;
            if (counted++ == target)
                return finish(Restoration::Exact, ldomXPointer(node, location.after ? i : i + 1));
        }
    }
    const auto end = ldomXPointer(nodes.last(), nodes.last()->getText().length());
    return finish(location.offset == counted ? Restoration::Exact : Restoration::Approximate, end);
}
} // namespace kobalt
