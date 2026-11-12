#pragma once
#include <QByteArray>
#include <QVector>

// A rough page count for one spine item, without a rendering engine. It is
// for values that may be estimates, such as the percent of the book. It is
// never a page number that the reader sees as exact.
//
// The work has two parts. measurePageEstimate() reads the XHTML once and
// keeps, for each block of text, how wide its text is in units of the font
// size. That result does not depend on the font size or the page, so it can
// be kept. estimatePages() then counts lines and pages for one page size
// with arithmetic only.
namespace kobalt {
struct PageEstimateBlock
{
    enum Kind : quint8 { Text, LineBreak, Image, Rule };

    // The width of the text on one endless line, in units of the body font
    // size. It includes the larger font of a heading.
    float width = 0;
    // The font size of the block relative to the body text.
    float scale = 1;
    // How many indented ancestors the block has: lists and quotations.
    quint8 depth = 0;
    Kind kind = Text;
};

struct PageEstimateItem
{
    QVector<PageEstimateBlock> blocks;
    // The characters of the text, without whitespace.
    qint64 characters = 0;
    // False when the XHTML is not well formed. The blocks then cover the
    // text before the error.
    bool parsed = false;
};

struct PageEstimateLayout
{
    // All in pixels: the body font size, the height of one line, the width
    // and height of the text area, the gap after a paragraph and the indent
    // of a first line.
    double fontSize = 0, lineHeight = 0, width = 0, height = 0, paragraphGap = 0, indent = 0;
};

PageEstimateItem measurePageEstimate(const QByteArray &xhtml);
// The pages of the item as a fraction, at least a small part of one page.
// The caller rounds up, because a spine item starts on a new page.
double estimatePages(const PageEstimateItem &item, const PageEstimateLayout &layout);
} // namespace kobalt
