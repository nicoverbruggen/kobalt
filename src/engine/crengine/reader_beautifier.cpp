#include "reader_beautifier.h"
#include <lvtinydom.h>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <algorithm>
#include <vector>

namespace kobalt {
namespace {
QString text(const lString32 &value)
{
    return QString::fromUtf8(UnicodeToUtf8(value).c_str());
}

bool special(ldomNode *node)
{
    static const QSet<QString> tags{ "blockquote", "aside", "address", "figure", "figcaption",
                                     "table",      "ul",    "ol",      "dl",     "pre",
                                     "code",       "math",  "ruby",    "nav",    "header",
                                     "footer",     "h1",    "h2",      "h3",     "h4",
                                     "h5",         "h6" };
    if (tags.contains(text(node->getNodeName())))
        return true;
    static const QRegularExpression names(
            "(?:^|[\\s_-])(?:quote|quotation|blockquote|letter|signature|verse|poem|poetry|stanza|caption|epigraph|dedication|footnote|endnote|noteref|title|subtitle|heading|scene|break|separator|copyright|toc)(?:$|[\\s_-])",
            QRegularExpression::CaseInsensitiveOption);
    const auto types = text(
            node->getAttributeValue(LXML_NS_ANY, node->getDocument()->getAttrNameIndex("type")));
    if (names.match(text(node->getAttributeValue("class"))).hasMatch()
        || names.match(types).hasMatch()
        || names.match(text(node->getAttributeValue("role"))).hasMatch())
        return true;
    const auto style = node->getStyle();
    return !style.isNull()
            && (css_wm_is_vertical(style->writing_mode) || style->text_align == css_ta_center
                || style->text_align == css_ta_right || style->white_space == css_ws_pre
                || style->white_space == css_ws_pre_wrap);
}

bool initial(ldomNode *node)
{
    if (text(node->getNodeName()) != "span")
        return false;
    static const QRegularExpression name("drop.?cap|initial",
                                         QRegularExpression::CaseInsensitiveOption);
    const auto style = node->getStyle();
    // Some publishers use generic names such as "big". A floated
    // single letter is also evidence of a text initial, regardless of its name.
    const bool floated = !style.isNull() && style->float_ == css_f_left;
    if (!floated && !name.match(text(node->getAttributeValue("class"))).hasMatch())
        return false;
    const auto value = text(node->getText()).trimmed();
    // Include an opening quotation mark, but never a whole decorated word.
    if (value.isEmpty() || value.size() > 3)
        return false;
    bool letter = false;
    for (const auto c : value) {
        if (c.isLetter()) {
            if (letter)
                return false;
            letter = true;
        } else if (!c.isPunct()) {
            return false;
        }
    }
    return letter;
}
} // namespace

void markBeautifierContent(ldomDocument *document)
{
    if (!document || !document->getRootNode())
        return;
    const auto role = document->getAttrNameIndex("data-kobalt-beautify");
    const auto initialAttribute = document->getAttrNameIndex("data-kobalt-initial");
    const auto firstLetterAttribute = document->getAttrNameIndex("data-kobalt-first-letter");
    const auto engineFirstLetter = document->getAttrNameIndex("HasFirstLetter");

    struct Frame
    {
        ldomNode *node;
        bool blocked;
    };

    std::vector<Frame> stack{ { document->getRootNode(), false } };
    while (!stack.empty()) {
        const auto frame = stack.back();
        stack.pop_back();
        auto *node = frame.node;
        if (!node->isElement())
            continue;
        // Never trust source-supplied markers.
        if (node->hasAttribute(role))
            node->setAttributeValue(0, role, U"");
        if (node->hasAttribute(initialAttribute))
            node->setAttributeValue(0, initialAttribute, U"");
        if (node->hasAttribute(firstLetterAttribute))
            node->setAttributeValue(0, firstLetterAttribute, U"");
        // Text initials are independent of prose spacing and indentation. A
        // subtitle, leading gap or quotation must not keep a drop cap enlarged.
        if (initial(node))
            node->setAttributeValue(0, initialAttribute, U"1");
        static const QRegularExpression opener("drop.?cap|stickupcap",
                                               QRegularExpression::CaseInsensitiveOption);
        const bool firstLetter = node->hasAttribute(engineFirstLetter)
                || (text(node->getNodeName()) == "p"
                    && opener.match(text(node->getAttributeValue("class"))).hasMatch());
        if (firstLetter)
            node->setAttributeValue(0, firstLetterAttribute, U"1");
        const bool blocked = frame.blocked || special(node);
        for (int i = node->getChildCount() - 1; i >= 0; --i)
            stack.push_back({ node->getChildNode(i), blocked });
        if (blocked || text(node->getNodeName()) != "p"
            || text(node->getAttributeValue("data-kobalt-paragraph")) != "1")
            continue;
        const auto style = node->getStyle();
        if (style.isNull() || style->text_indent.value < 0 || style->margin[0].value
            || style->margin[1].value || style->padding[0].value || style->padding[1].value)
            continue;
        // A leading gap may be the only scene separator. Keep its paragraph
        // spacing and indentation. Zero margins are common in ordinary prose.
        if (style->margin[2].value > 0 || style->padding[2].value > 0)
            continue;
        if (text(node->getText()).trimmed().size() < 80)
            continue;
        bool safe = true;
        std::vector<ldomNode *> children, initials;
        for (int i = 0; i < node->getChildCount(); ++i)
            children.push_back(node->getChildNode(i));
        bool precedingText = false;
        // Traverse in source order, including through kepubify wrappers.
        std::reverse(children.begin(), children.end());
        while (!children.empty()) {
            auto *child = children.back();
            children.pop_back();
            if (child->isText()) {
                precedingText |= !text(child->getText()).trimmed().isEmpty();
                continue;
            }
            if (!child->isElement())
                continue;
            const auto name = text(child->getNodeName());
            static const QSet<QString> inlineTags{ "span", "i",   "em",  "b",    "strong",
                                                   "a",    "sup", "sub", "small" };
            // CREngine boxes floats during parsing. Keep those internal nodes;
            // reflow updates their rendering when the source float is disabled.
            const bool leadingBox =
                    !precedingText && (name == "floatBox" || (firstLetter && name == "pseudoElem"));
            if ((!inlineTags.contains(name) && !leadingBox) || special(child)) {
                safe = false;
                break;
            }
            if (!precedingText && initial(child)) {
                initials.push_back(child);
            } else if (const auto s = child->getStyle();
                       !leadingBox && !s.isNull() && s->float_ != css_f_none) {
                safe = false;
                break;
            }
            for (int i = child->getChildCount() - 1; i >= 0; --i)
                children.push_back(child->getChildNode(i));
        }
        if (!safe)
            continue;
        node->setAttributeValue(0, role,
                                firstLetter                 ? U"first-letter"
                                        : !initials.empty() ? U"opening"
                                                            : U"body");
    }
}
} // namespace kobalt
