#include "support/qt_compat.h"
#include "reader_alignment.h"
#include "stop_point.h"
#include <lvtinydom.h>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <vector>

namespace kobalt {
namespace {
QString text(const lString32 &value)
{
    return QString::fromUtf8(UnicodeToUtf8(value).c_str());
}

bool hasText(ldomNode *node)
{
    const auto bytes = node->getText8();
    for (int i = 0; i < bytes.length(); ++i) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        // Keep the existing Unicode and malformed-input handling.
        if (c >= 0x80)
            return !text(Utf8ToUnicode(bytes)).trimmed().isEmpty();
        if (c == 0)
            return false;
        if (c != ' ' && (c < '\t' || c > '\r'))
            return true;
    }
    return false;
}

bool preserve(ldomNode *node, const QString &name)
{
    static const QSet<QString> elements{ "h1",     "h2",     "h3",     "h4",         "h5",
                                         "h6",     "header", "figure", "figcaption", "caption",
                                         "table",  "img",    "image",  "svg",        "picture",
                                         "object", "video",  "audio",  "canvas",     "iframe",
                                         "pre",    "code",   "head",   "style",      "script" };
    // Computed publisher styles cover arbitrary class names and inherited centering.
    const auto style = node->getStyle();
    if (!style.isNull() && style->text_align == css_ta_center) {
        return true;
    }
    if (elements.contains(name))
        return true;
    const auto role = text(node->getAttributeValue("role")).split(' ', kobalt::qt::skipEmptyParts);
    if (role.contains("heading"))
        return true;
    const auto type = text(node->getAttributeValue(LXML_NS_ANY,
                                                   node->getDocument()->getAttrNameIndex("type")))
                              .split(' ', kobalt::qt::skipEmptyParts);
    for (const auto *value :
         { "title", "subtitle", "fulltitle", "covertitle", "halftitle", "titlepage" })
        if (type.contains(value))
            return true;
    // EPUBs also commonly encode headings as paragraphs with named classes.
    static const QRegularExpression headingClass(
            "(?:^|[\\s_-])(?:title|subtitle|heading)(?:$|[\\s_-])",
            QRegularExpression::CaseInsensitiveOption);
    const auto &classes = node->getAttributeValue("class");
    return !classes.empty() && headingClass.match(text(classes)).hasMatch();
}

struct Frame
{
    ldomNode *node;
    int child = 0;
    bool blocked = false, words = false, protectedChild = false, blockChild = false, block = false;
    bool paragraphBlocked = false;
};
} // namespace

void markBodyAlignment(ldomDocument *document)
{
    if (!document || !document->getRootNode())
        return;
    const auto attribute = document->getAttrNameIndex("data-kobalt-prose");
    const auto paragraphAttribute = document->getAttrNameIndex("data-kobalt-paragraph");
    static const QSet<QString> blocks{ "p",          "li",  "dt",      "dd",
                                       "blockquote", "div", "section", "article" };
    std::vector<Frame> stack{ { document->getRootNode() } };
    while (!stack.empty()) {
        stopPoint();
        auto &frame = stack.back();
        auto *node = frame.node;
        if (!frame.child && node->isElement()) {
            const auto name = text(node->getNodeName());
            frame.block = blocks.contains(name);
            frame.blocked = frame.blocked || preserve(node, name);
            // Paragraph gaps must not flatten verse, quotations, lists or notes.
            // Alignment has a broader scope, so keep this safeguard separate.
            static const QSet<QString> special{
                "blockquote", "ul", "ol", "dl", "aside", "address"
            };
            static const QRegularExpression specialClass(
                    "(?:^|[\\s_-])(?:verse|poem|poetry|stanza|caption|epigraph|dedication|footnote|endnote)(?:$|[\\s_-])",
                    QRegularExpression::CaseInsensitiveOption);
            const auto &type =
                    node->getAttributeValue(LXML_NS_ANY, document->getAttrNameIndex("type"));
            const auto matchesSpecial = [&](const lString32 &value) {
                return !value.empty() && specialClass.match(text(value)).hasMatch();
            };
            frame.paragraphBlocked = frame.paragraphBlocked || frame.blocked
                    || special.contains(name) || matchesSpecial(node->getAttributeValue("class"))
                    || matchesSpecial(type);
            const auto style = node->getStyle();
            // Publisher exports can leave a border style on a zero-width frame.
            // Only borders with width need to protect the text inside it.
            if (!style.isNull())
                frame.paragraphBlocked = frame.paragraphBlocked || style->white_space == css_ws_pre
                        || style->white_space == css_ws_pre_wrap || style->float_ != css_f_none
                        || (style->border_style_top != css_border_none
                            && style->border_width[0].value > 0)
                        || (style->border_style_right != css_border_none
                            && style->border_width[1].value > 0)
                        || (style->border_style_bottom != css_border_none
                            && style->border_width[2].value > 0)
                        || (style->border_style_left != css_border_none
                            && style->border_width[3].value > 0)
                        || (name == "p"
                            && lUInt32(style->background_color.value) != CSS_COLOR_TRANSPARENT);
            if (node->hasAttribute(attribute))
                node->setAttributeValue(0, attribute, U"");
            if (node->hasAttribute(paragraphAttribute))
                node->setAttributeValue(0, paragraphAttribute, U"");
        }
        if (frame.child < node->getChildCount()) {
            auto *child = node->getChildNode(frame.child++);
            Frame next{ child, 0, frame.blocked };
            next.paragraphBlocked = frame.paragraphBlocked;
            stack.push_back(next);
            continue;
        }
        if (node->isText())
            frame.words = hasText(node);
        const bool block = frame.block;
        if (block && frame.words && !frame.blocked && !frame.protectedChild && !frame.blockChild)
            node->setAttributeValue(0, attribute, U"1");
        if (node->isElement() && text(node->getNodeName()) == "p" && frame.words
            && !frame.paragraphBlocked && !frame.protectedChild && !frame.blockChild)
            node->setAttributeValue(0, paragraphAttribute, U"1");
        const auto finished = frame;
        stack.pop_back();
        if (!stack.empty()) {
            auto &parent = stack.back();
            parent.words |= finished.words;
            parent.protectedChild |= finished.blocked || finished.protectedChild;
            parent.blockChild |= block || finished.blockChild;
        }
    }
}
} // namespace kobalt
