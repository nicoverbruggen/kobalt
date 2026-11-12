#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QMap>
#include <QVector>
#include <memory>

class QXmlStreamReader;

// Estimated page counts for a book whose spine items do not all have a
// layout. They are for values that may be estimates: the rough page number,
// the page total and the percent of the book. They are never shown as exact
// page numbers.
//
// The work has three parts.
//
// 1. PageEstimateMeasurer reads the XHTML of each spine item once, in small
//    steps, and keeps for each block of text how wide its text is in units
//    of the font size, and what kind of block it is. That does not depend on
//    the font size or the page.
// 2. pageEstimateFeatures() counts, for one page size, the lines of each kind
//    of block and the blocks that add space: paragraphs, quotations, list
//    items, table rows and images. This is arithmetic only.
// 3. PageMap weighs those counts with a linear model. Its starting weights
//    come from a fit on a set of books. Each spine item or slice that gets a
//    real layout while the user reads gives its real page count, and the
//    weights move towards the book that is open.
namespace kobalt {
struct PageEstimateBlock
{
    enum Kind : quint8 {
        // Prose: a paragraph or another block of running text.
        Text,
        // One line of verse: text that a line break ends, and the text after
        // the last line break of the same block.
        Line,
        // Text in a table cell.
        Cell,
        // A heading.
        Heading,
        Image,
        Rule
    };

    // The width of the text on one endless line, in units of the body font
    // size. It includes the larger font of a heading.
    float width = 0;
    // The characters of the item before the end of this block, with one
    // space between words. They place a slice of the engine on the blocks.
    qint32 end = 0;
    // The font size of the block relative to the body text.
    float scale = 1;
    // How many indented ancestors the block has: lists and quotations.
    quint8 depth = 0;
    Kind kind = Text;
    // The quotations, list items and table rows that start after the block
    // before this one and before the end of this one.
    quint8 quotes = 0, listItems = 0, rows = 0;
};

struct PageEstimateItem
{
    QVector<PageEstimateBlock> blocks;
    // The characters of the text, with one space between words.
    qint32 characters = 0;
    // False when the XHTML is not well formed. The blocks then cover the
    // text before the error.
    bool parsed = false;
};

// The size of one part of the XHTML that the reader measures in one step,
// in bytes. On the desktop one part takes well under a millisecond.
constexpr int pageEstimatePart = 8 * 1024;

// Reads one spine item in parts, so that the reader can stop between them.
class PageEstimateMeasurer
{
public:
    PageEstimateMeasurer();
    ~PageEstimateMeasurer();
    PageEstimateMeasurer(const PageEstimateMeasurer &) = delete;
    PageEstimateMeasurer &operator=(const PageEstimateMeasurer &) = delete;

    // Reads the next part of the XHTML. `last` says that no more follows.
    void feed(const QByteArray &chunk, bool last);

    bool done() const { return done_; }

    // The result, complete when done() is true.
    const PageEstimateItem &item() const { return item_; }

private:
    void flush(PageEstimateBlock::Kind kind);
    void start(const QString &name);
    void end(const QString &name);
    void text(const QString &text);

    std::unique_ptr<QXmlStreamReader> parser_;
    PageEstimateItem item_;
    QVector<float> scales_{ 1 };
    float width_ = 0;
    qint32 characters_ = 0;
    int depth_ = 0, skip_ = 0, cells_ = 0, headings_ = 0;
    quint8 quotes_ = 0, listItems_ = 0, rows_ = 0;
    bool content_ = false, pendingSpace_ = false, broken_ = false, done_ = false;
};

// Reads one spine item at once.
PageEstimateItem measurePageEstimate(const QByteArray &xhtml);

struct PageEstimateLayout
{
    // All in pixels: the body font size, the height of one line, the width
    // and height of the text area and the indent of a first line.
    double fontSize = 0, lineHeight = 0, width = 0, height = 0, indent = 0;

    bool valid() const { return fontSize > 0 && lineHeight > 0 && width > 0 && height > 0; }

    // Whole lines of body text on one page, at least one.
    double linesPerPage() const;
    bool operator==(const PageEstimateLayout &other) const;

    bool operator!=(const PageEstimateLayout &other) const { return !(*this == other); }
};

// The counts that the model weighs, for some blocks of one item. Line counts
// are in lines of body text. The image and the start counts are multiplied
// by the lines of one page, so that their weights are in pages.
struct PageEstimateFeatures
{
    enum Index {
        ProseLines,
        VerseLines,
        CellLines,
        HeadingLines,
        Paragraphs,
        Quotes,
        ListItems,
        Rows,
        Images,
        // One for each part that starts on a new page: a spine item, or a
        // slice of one. Its weight is the empty part of the last page.
        Starts,
        Count
    };

    double value[Count] = {};

    PageEstimateFeatures &operator+=(const PageEstimateFeatures &other);
};

// The counts of the blocks from `first` up to before `last`, without the
// count of starts. A negative `last` is the end of the item.
PageEstimateFeatures pageEstimateFeatures(const PageEstimateItem &item,
                                          const PageEstimateLayout &layout, int first = 0,
                                          int last = -1);

// Weights for PageEstimateFeatures. The result of a model is in lines.
struct PageEstimateModel
{
    double weight[PageEstimateFeatures::Count] = {};

    double lines(const PageEstimateFeatures &features) const;
    // The weights fitted on the reference books. See doc/engines/migration.md.
    static PageEstimateModel reference();
    // How many lines of evidence the reference weights count as, for each
    // weight, when the open book corrects them.
    static const double *strength();
};

// Sums for a weighted least-squares fit of a model, drawn towards a prior
// model. Each observation weighs as much as its inverse size, so that a long
// spine item does not outweigh many short ones.
class PageEstimateFit
{
public:
    void add(const PageEstimateFeatures &features, double lines);
    // The model that fits the observations best, drawn towards `prior` by
    // `strength`, one value for each weight. Without observations, the prior.
    PageEstimateModel solve(const PageEstimateModel &prior, const double *strength) const;

    int observations() const { return count_; }

private:
    double a_[PageEstimateFeatures::Count][PageEstimateFeatures::Count] = {};
    double b_[PageEstimateFeatures::Count] = {};
    int count_ = 0;
};

// The saved part of the page estimate of one book: the corrected weights,
// the page numbers shown at the saved position and the real pages of the
// spine items that were read. It is the optional book-state field
// `page_estimate`; see doc/storage.md.
struct PageEstimateRecord
{
    // At most this many spine items with real pages, so that the record
    // stays small.
    static constexpr int maxRead = 500;

    PageEstimateModel model;
    // A digest of everything that decides the pages. The page numbers and
    // the read spine items are valid only for the same digest.
    QString layout;
    int spineItems = 0;
    // The shown page: its spine item, the first character of its slice, its
    // page in that part and the pages of the part, and the book page and the
    // page in the spine item that were shown there. A negative item means
    // none.
    int item = -1, first = 0, page = 0, partPages = 0, shown = 0, itemShown = 0;
    // The real pages of whole spine items, by item.
    QMap<int, int> read;

    QJsonObject toJson() const;
    // False for a missing record or one that is not valid. Invalid parts of
    // the place or of the read spine items are left out.
    static bool fromJson(const QJsonValue &value, PageEstimateRecord *record);
};

// The estimated pages of a whole book, one spine item after the other. A
// spine item is one part, or several slices of `sliceCharacters`
// characters, each starting on a new page.
class PageMap
{
public:
    // Forgets everything. `bytes` is the size of each spine item, in spine
    // order. It gives an estimate for the items that are not measured yet.
    void reset(const QVector<qint64> &bytes);

    int items() const { return items_.size(); }

    // The first item without a measurement, or -1. `first` goes first.
    int nextUnmeasured(int first = -1) const;
    bool measured(int item) const;

    bool complete() const { return measuredCount_ == items_.size(); }

    void setMeasured(int item, const PageEstimateItem &measurement);

    // Sets the page. A new page or slice size forgets the observations,
    // because they were for the old one, and returns true. Slices of zero
    // characters lay out a spine item as a whole.
    bool setLayout(const PageEstimateLayout &layout, int sliceCharacters);

    const PageEstimateLayout &layout() const { return layout_; }

    void setPrior(const PageEstimateModel &prior);

    // A layout of the characters from `first` to `last` of `item` gave
    // `pages` pages. The characters are those of the engine; `characters` is
    // the engine's count for the whole item. A new observation replaces the
    // ones of the same item that it overlaps.
    void observe(int item, int first, int last, int characters, int pages);
    // The real pages of the spine items whose every part has a layout.
    QMap<int, int> completeItems() const;
    // Gives the real pages of whole spine items, from an earlier reading. They
    // count as observations but do not change the weights, which already
    // contain them.
    void restore(const QMap<int, int> &pages);

    int observations() const { return observed_.size(); }

    const PageEstimateModel &model() const { return model_; }

    // The estimated pages of an item, of the items before `item`, and of
    // the part of `item` before its engine character `before`, of
    // `characters` in all. Zero when nothing is known yet.
    double itemPages(int item) const;
    double pagesBefore(int item) const;
    double pagesBefore(int item, int before, int characters) const;
    double total() const;

private:
    struct Observed
    {
        int first = 0, last = 0, characters = 0, pages = 0;
        // The counts of the part, once its item is measured.
        PageEstimateFeatures features;
        bool ready = false;
        // False for restored pages, which the weights already contain.
        bool fit = true;
    };

    struct Item
    {
        PageEstimateItem measurement;
        bool measured = false;
        // The counts of the whole item for the layout.
        PageEstimateFeatures features;
        qint64 bytes = 0;
        double pages = 0;
    };

    // The first block that ends after `fraction` of the text of the item.
    int blockAt(const Item &item, double fraction) const;
    // The parts of an item that start on a new page, before `fraction`.
    int startsBefore(const Item &item, double fraction) const;
    double estimate(int index, double fraction) const;
    // The same for an item without a measurement, from its bytes.
    double byteEstimate(int index, double fraction) const;
    void refit();
    void update();

    QVector<Item> items_;
    QVector<double> starts_;
    // By item and first character, in order.
    QMap<qint64, Observed> observed_;
    PageEstimateLayout layout_;
    PageEstimateModel prior_ = PageEstimateModel::reference(), model_ = prior_;
    int sliceCharacters_ = 0, measuredCount_ = 0;
    // Pages for one byte of the measured items, for the others.
    double pagesPerByte_ = 0;
};

// Keeps a shown page number from moving backward while the user reads
// forward. A forward turn adds one. At the start of a spine item or a slice
// the number may also move up to a larger estimate. A jump shows the estimate.
class PageAnchor
{
public:
    enum Step { Same, Forward, Backward, Jump };

    int place(int estimate, Step step, bool partStart);

    void reset() { shown_ = 0; }

    // Continues from a number shown earlier.
    void restore(int shown) { shown_ = qMax(0, shown); }

    int shown() const { return shown_; }

private:
    int shown_ = 0;
};
} // namespace kobalt
