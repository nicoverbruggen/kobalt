// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace blaze {
// Walk descendants in document order, without recursing through publisher markup.
template <class Visit>
inline void visitNavChildren(ldomNode *root, Visit visit)
{
    ldomNode *node = root;
    int child = 0;
    while (true) {
        if (node->isElement() && child < node->getChildCount()) {
            node = node->getChildNode(child);
            child = 0;
            if (!visit(node))
                return;
        } else {
            if (node == root)
                return;
            child = node->getNodeIndex() + 1;
            node = node->getParentNode();
        }
    }
}

inline lString32 navLabel(ldomNode *link, ldomDocumentFragmentWriter &writer, lString32 &image)
{
    const auto img = link->getDocument()->getElementNameIndex(U"img");
    lString32 title;
    bool hasText = false;
    visitNavChildren(link, [&](ldomNode *node) {
        lString32 text;
        if (node->isText()) {
            text = node->getText();
            text.trimDoubleSpaces(false, false, false);
            hasText = hasText || !text.empty();
        } else if (node->getNodeId() == img) {
            text = node->getAttributeValue("alt");
            if (text.empty())
                text = node->getAttributeValue("title");
            lString32 src = node->getAttributeValue("src");
            if (image.empty() && !src.empty() && src.pos(U":") < 0)
                image = writer.convertHref(DecodeHTMLUrlString(src));
        }
        if (!text.empty()) {
            if (!title.empty())
                title << ' ';
            title << text;
        }
        return true;
    });
    if (hasText)
        image.clear();
    title.trimDoubleSpaces(false, false, false);
    if (title.empty())
        title = link->getAttributeValue("title");
    return title;
}

} // namespace blaze
