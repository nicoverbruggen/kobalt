#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/column_view.h"
#include "engine/blaze/adapter/engine_document.h"
#include "engine/blaze/adapter/reader_locations.h"
#include "engine/blaze/adapter/reader_render_callback.h"
#include "engine/blaze/adapter/reader_source.h"
#include "publication/page_estimate.h"
#include "render/page_content.h"
#include "support/qt_compat.h"
#include "support/diagnostics.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>
#include <cstdio>
#include <lvdocview.h>

using namespace kobalt;

namespace kobalt {
// Reads the spine items of an open book, for the comparison with real pages.
struct ReaderLocationProbe
{
    // Exercises the nested layout used during reflow and saved-position resolution.
    static bool layoutSliceWithCancelledCaller(EngineDocument &model, bool duringExtension)
    {
        OpeningCallback caller(model.engine_, {},
                               [&] { return !duringExtension || model.sliceExtra_ > 0; });
        return model.layoutSlice(model.sliceFirst_, model.sliceForcedEnd_);
    }

    static bool callbacksDetached(const EngineDocument &model)
    {
        if (model.engine_ && model.engine_->getCallback())
            return false;
        if (model.ahead_ && !callbacksDetached(*model.ahead_))
            return false;
        if (model.full_ && !callbacksDetached(*model.full_))
            return false;
        for (const auto &kept : model.kept_)
            if (!callbacksDetached(*kept.document))
                return false;
        return true;
    }

    // How many positions have their place in the text of a spine item.
    static int textPlaces(const EngineDocument &model)
    {
        return int(model.source_->textFractions.size());
    }

    static QStringList items(const EngineDocument &model)
    {
        QStringList result;
        auto *root = model.engine_->getDocument()->nodeFromXPath(U"/body");
        for (int i = 0; root && i < root->getChildCount(); ++i) {
            auto *fragment = root->getChildNode(i);
            if (fragment->isElement() && fragment->isNodeName("DocFragment"))
                result.append(QString::fromUtf8(
                        UnicodeToUtf8(fragment->getAttributeValue("Source")).c_str()));
        }
        return result;
    }

    static QByteArray source(const EngineDocument &model, const QString &href)
    {
        auto stream = model.engine_->getDocument()->getContainer()->OpenStream(
                Utf8ToUnicode(href.toUtf8().constData()).c_str(), LVOM_READ);
        if (stream.isNull() || stream->GetSize() > 64 * 1024 * 1024)
            return {};
        QByteArray bytes(int(stream->GetSize()), '\0');
        lvsize_t read = 0;
        if (stream->Read(bytes.data(), bytes.size(), &read) != LVERR_OK
            || read != lvsize_t(bytes.size()))
            return {};
        return bytes;
    }

    // The page estimate of the book and the state of the layout in chapter
    // mode, for the checks of the observations.
    static const PageMap &map(const EngineDocument &model)
    {
        return model.source_->pageEstimate.map;
    }

    // For a change of the map that reading would make later.
    static PageMap &editMap(EngineDocument &model) { return model.source_->pageEstimate.map; }

    static QVector<qint64> spineBytes(const EngineDocument &model) { return model.spineBytes(); }

    static int windowFirst(const EngineDocument &model) { return model.windowFirst_; }

    static int windowLast(const EngineDocument &model) { return model.windowLast_; }

    static int sliceFirst(const EngineDocument &model) { return model.sliceFirst_; }

    static int sliceEnd(const EngineDocument &model) { return model.sliceEnd_; }

    static int sliceSize(const EngineDocument &model) { return model.sliceSize(); }

    static int visiblePages(const EngineDocument &model) { return model.visiblePages_; }

    static int spineItemText(const EngineDocument &model) { return model.spineItemText(); }

    static bool layoutSlice(EngineDocument &model, int first) { return model.layoutSlice(first); }

    static const EngineDocument *ahead(const EngineDocument &model) { return model.ahead_.get(); }

    static bool hasChapter(const EngineDocument &model, int item)
    {
        const auto contains = [item](const EngineDocument &document) {
            return document.windowFirst_ <= item && item <= document.windowLast_;
        };
        if (contains(model) || (model.ahead_ && contains(*model.ahead_)))
            return true;
        for (const auto &unit : model.kept_)
            if (contains(*unit.document))
                return true;
        return false;
    }

    static const LVDocView *view(const EngineDocument &model) { return model.engine_.get(); }

    // The document of the whole book that Hybrid rendering prepared.
    static const LVDocView *fullView(const EngineDocument &model)
    {
        return model.full_ ? model.full_->engine_.get() : nullptr;
    }

    static void omitPreparedChapter(EngineDocument &model)
    {
        auto *body = model.full_->engine_->getDocument()->nodeFromXPath(U"/body");
        require(body, "The prepared document must have a body");
        while (body->getChildCount())
            body->removeLastChild();
    }

    static void forgetRefusedFullDocument(EngineDocument &model) { model.fullFailed_.clear(); }

    static void abandonFullBuild(EngineDocument &model) { model.dropSideDocuments(); }

    static bool measurePlace(EngineDocument &model, const std::function<bool()> &current)
    {
        return model.measureChapterPlaces(current);
    }

    static int measuredPlaces(const EngineDocument &model)
    {
        return model.source_->pageEstimate.chapterPlaces;
    }
};
} // namespace kobalt

namespace {
using Probe = ReaderLocationProbe;
using F = PageEstimateFeatures;

// The page of the survey and of the checks: a seven-inch screen with a
// large font.
struct Page
{
    int font = 44, margin = 80, spacing = 150;
    QSize viewport{ 1264, 1680 };

    QJsonObject settings() const
    {
        return { { "font_pixels", font },         { "margin_pixels", margin },
                 { "top_margin_pixels", margin }, { "bottom_margin_pixels", margin },
                 { "line_spacing", spacing },     { "line_spacing_version", 1 } };
    }

    PageEstimateLayout layout() const
    {
        PageEstimateLayout layout;
        layout.fontSize = font;
        layout.lineHeight = font * spacing / 100.0;
        layout.width = viewport.width() - 2 * margin;
        layout.height = viewport.height() - 2 * margin;
        layout.indent = font;
        return layout;
    }
};

void blocks()
{
    const QByteArray xhtml =
            "<html><head><title>Skipped title</title><style>p{}</style></head><body>"
            "<h1>Head</h1><p>One  two\n three</p><p>Line<br/>next</p><blockquote><p>Quoted</p>"
            "</blockquote><hr/><p><img src=\"a.png\"/></p><table><tr><td>Cell</td></tr></table>"
            "<ul><li>Item</li></ul></body></html>";
    const auto item = measurePageEstimate(xhtml);
    require(item.parsed, "Well-formed XHTML must parse");
    require(item.characters == 4 + 13 + 8 + 6 + 4 + 4,
            "The text counts with one space between words, without the head");
    require(item.blocks.size() == 9, "Each paragraph, line, rule, image and cell is one block");
    require(item.blocks[0].kind == PageEstimateBlock::Heading && item.blocks[0].scale > 1,
            "A heading has a larger font");
    require(item.blocks[1].kind == PageEstimateBlock::Text, "A paragraph is prose");
    require(item.blocks[2].kind == PageEstimateBlock::Line
                    && item.blocks[3].kind == PageEstimateBlock::Line,
            "The text before and after a line break are lines of verse");
    require(item.blocks[4].depth == 1 && item.blocks[4].quotes == 1, "A quotation is indented");
    require(item.blocks[5].kind == PageEstimateBlock::Rule
                    && item.blocks[6].kind == PageEstimateBlock::Image,
            "Rules and images are blocks");
    require(item.blocks[7].kind == PageEstimateBlock::Cell && item.blocks[7].rows == 1,
            "Text in a table is a cell of a row");
    require(item.blocks[8].listItems == 1 && item.blocks[8].depth == 1,
            "A list item is counted and indented");
    require(item.blocks.last().end == item.characters, "The last block ends the text");
    require(!measurePageEstimate("<html><body><p>Open").parsed, "Malformed XHTML must be reported");

    // Parts of any size give the same result as the whole.
    for (const int size : { 1, 7, 64 }) {
        PageEstimateMeasurer measurer;
        for (int at = 0; at < xhtml.size(); at += size)
            measurer.feed(xhtml.mid(at, size), at + size >= xhtml.size());
        require(measurer.done(), "The measurer must finish with the last part");
        const auto &parts = measurer.item();
        require(parts.characters == item.characters && parts.blocks.size() == item.blocks.size(),
                "Reading in parts must give the same blocks");
        for (int i = 0; i < parts.blocks.size(); ++i)
            require(parts.blocks[i].width == item.blocks[i].width
                            && parts.blocks[i].kind == item.blocks[i].kind,
                    "Reading in parts must give the same widths");
    }

    const auto layout = Page().layout();
    const auto prose = measurePageEstimate("<body><p>" + QByteArray(4000, 'a') + "</p></body>");
    const auto verse =
            measurePageEstimate("<body><p>" + QByteArray("a<br/>").repeated(4000) + "</p></body>");
    const auto model = PageEstimateModel::reference();
    require(model.lines(pageEstimateFeatures(verse, layout))
                    > 10 * model.lines(pageEstimateFeatures(prose, layout)),
            "Short lines need more pages than the same characters as prose");
    auto wide = layout;
    wide.width *= 2;
    require(pageEstimateFeatures(prose, wide).value[F::ProseLines]
                    < pageEstimateFeatures(prose, layout).value[F::ProseLines],
            "A wider page needs fewer lines");
    require(pageEstimateFeatures(prose, PageEstimateLayout()).value[F::ProseLines] == 0,
            "No layout, no estimate");
}

// One book for the survey and the checks: the measured spine items and
// their real pages in a layout of the whole book.
struct Book
{
    QString name;
    int pages = 0;
    QVector<PageEstimateItem> items;
    QVector<qint64> bytes;
    QVector<int> starts;
    qint64 measureMs = 0, openMs = 0;
    // The largest time of one measured part, in microseconds.
    qint64 partUs = 0;

    int real(int item) const
    {
        return (item + 1 < starts.size() ? starts[item + 1] : pages) - starts[item];
    }
};

Book load(const QString &name, const QByteArray &bytes, const Page &page)
{
    EngineDocument model(test::readerFonts());
    QString error;
    QElapsedTimer timer;
    timer.start();
    require(model.open(bytes, page.viewport, page.settings(), &error), qPrintable(error));
    Book book;
    book.name = name;
    book.openMs = timer.elapsed();
    book.pages = model.pages();
    const auto items = Probe::items(model);
    QStringList starts;
    for (const auto &href : items) {
        Location location;
        location.href = href;
        starts.append(model.positionOf(location));
    }
    const auto pages = model.pagesOf(starts);
    // A spine item without a page, such as an empty one, starts where the
    // next one starts.
    for (int i = pages.size() - 1; i >= 0; --i)
        book.starts.prepend(pages[i] >= 0 ? pages[i]
                                          : (book.starts.isEmpty() ? book.pages : book.starts[0]));
    QVector<QByteArray> sources;
    for (const auto &href : items) {
        sources.append(Probe::source(model, href));
        book.bytes.append(sources.last().size());
    }
    timer.restart();
    QElapsedTimer part;
    for (const auto &source : sources) {
        PageEstimateMeasurer measurer;
        constexpr int size = pageEstimatePart;
        for (int at = 0; !measurer.done(); at += size) {
            part.start();
            measurer.feed(source.mid(at, size), at + size >= source.size());
            book.partUs = qMax(book.partUs, part.nsecsElapsed() / 1000);
        }
        book.items.append(measurer.item());
    }
    book.measureMs = timer.elapsed();
    return book;
}

PageMap mapOf(const Book &book, const PageEstimateLayout &layout, const PageEstimateModel &prior)
{
    PageMap map;
    map.reset(book.bytes);
    map.setPrior(prior);
    map.setLayout(layout, 0);
    for (int i = 0; i < book.items.size(); ++i)
        map.setMeasured(i, book.items[i]);
    return map;
}

// The model fitted on whole spine items of `books`, without `skip`.
PageEstimateModel fitted(const QVector<Book> &books, const PageEstimateLayout &layout, int skip)
{
    PageEstimateFit fit;
    const double page = layout.linesPerPage();
    for (int b = 0; b < books.size(); ++b) {
        if (b == skip)
            continue;
        for (int i = 0; i < books[b].items.size(); ++i) {
            auto features = pageEstimateFeatures(books[b].items[i], layout);
            features.value[F::Starts] = page;
            fit.add(features, books[b].real(i) * page);
        }
    }
    double none[F::Count];
    for (auto &value : none)
        value = 1e-9;
    return fit.solve(PageEstimateModel(), none);
}

// The page estimate of the first version: typical character widths and
// fixed line and image heights, without a fit, rounded up for each item.
double firstVersion(const PageEstimateItem &item, const PageEstimateLayout &layout)
{
    double height = 0;
    for (const auto &block : item.blocks) {
        const double line = layout.lineHeight * block.scale;
        if (block.kind == PageEstimateBlock::Image) {
            height += layout.height / 2;
            continue;
        }
        if (block.kind == PageEstimateBlock::Rule) {
            height += line;
            continue;
        }
        const double available =
                qMax(layout.fontSize * 4, layout.width - block.depth * 2 * layout.fontSize);
        const double text = block.width * layout.fontSize + layout.indent;
        height += qMax(1.0, std::ceil(text / available)) * line;
    }
    return std::ceil(qMax(0.05, height / layout.height));
}

struct Error
{
    // At the start of each spine item after the ones that have a layout:
    // the largest and the mean distance in pages and in percent of the book,
    // and the estimated total in percent of the real one.
    double worstPages = 0, meanPages = 0, worstPercent = 0, meanPercent = 0, total = 0;
};

// `start(i)` is the estimated start page of item i, `total` the estimated
// total. Items up to `known` have a layout.
template <typename Start>
Error error(const Book &book, int known, double total, Start start)
{
    Error result;
    result.total = 100 * total / book.pages;
    int count = 0;
    for (int i = known + 1; i < book.items.size(); ++i) {
        const double pages = std::fabs(start(i) - book.starts[i]);
        const double percent =
                std::fabs(100 * start(i) / total - 100.0 * book.starts[i] / book.pages);
        result.worstPages = qMax(result.worstPages, pages);
        result.worstPercent = qMax(result.worstPercent, percent);
        result.meanPages += pages;
        result.meanPercent += percent;
        ++count;
    }
    if (count) {
        result.meanPages /= count;
        result.meanPercent /= count;
    }
    return result;
}

// The spine item that holds the page at `fraction` of the book.
int itemAt(const Book &book, double fraction)
{
    int item = 0;
    while (item + 1 < book.starts.size() && book.starts[item + 1] <= fraction * book.pages)
        ++item;
    return item;
}

// The page estimate after reading up to `fraction` of the book: the items
// before the reading position have a real layout. The item at the position
// counts as unknown, which is less than chapter mode knows.
Error calibrated(const Book &book, const PageEstimateLayout &layout, const PageEstimateModel &prior,
                 double fraction, qint64 *updateUs = nullptr)
{
    auto map = mapOf(book, layout, prior);
    if (fraction < 0)
        return error(book, -1, map.total(), [&](int i) { return map.pagesBefore(i); });
    const int known = itemAt(book, fraction);
    QElapsedTimer timer;
    for (int i = 0; i < known; ++i) {
        timer.start();
        map.observe(i, 0, 1, 1, book.real(i));
        if (updateUs)
            *updateUs = qMax(*updateUs, timer.nsecsElapsed() / 1000);
    }
    return error(book, known, map.total(), [&](int i) { return map.pagesBefore(i); });
}

// The rough pages of the first chapter mode: the bytes of each spine item
// times the pages per byte of the spine item at the reading position.
Error bytes(const Book &book, double fraction)
{
    const int known = itemAt(book, fraction);
    const double perByte = double(book.real(known)) / qMax<qint64>(1, book.bytes[known]);
    QVector<double> starts{ 0 };
    for (const auto size : book.bytes)
        starts.append(starts.last() + size * perByte);
    return error(book, known, starts.last(), [&](int i) { return starts[i]; });
}

void print(const QString &book, const char *read, const char *method, const Error &e)
{
    std::printf("R|%s|%s|%s|%.0f|%.1f|%.2f|%.2f|%.1f\n", qPrintable(book), read, method,
                e.worstPages, e.meanPages, e.worstPercent, e.meanPercent, e.total);
}

// With a folder argument, reports each EPUB in it and requires nothing.
//
// Lines starting with B: name, spine items, real pages, ms to open the whole
// book, ms to measure all spine items, the largest µs of one part,
// the largest µs of one observation.
//
// Lines starting with R: name, read part of the book, method, then the
// error at the start of each spine item after the reading position: worst
// and mean in pages, worst and mean in percent of the book, and the
// estimated total in percent of the real one. Methods: `first` is the
// first estimator, `bytes` the rough pages of the first chapter mode,
// `model` this estimator with weights fitted on the other books of the
// folder, or on the reference books with --reference.
//
// --fit prints the weights fitted on all books of the folder.
// --small uses a six-inch page with a smaller font.
void survey(const QString &path, const QStringList &options)
{
    Page page;
    if (options.contains("--small")) {
        page.font = 32;
        page.margin = 48;
        page.viewport = { 1072, 1448 };
    }
    const auto layout = page.layout();
    const QDir folder(path);
    QVector<Book> books;
    for (const auto &name : folder.entryList({ "*.epub" }, QDir::Files, QDir::Name)) {
        QFile file(folder.filePath(name));
        if (file.open(QIODevice::ReadOnly))
            books.append(load(name.left(name.size() - 5), file.readAll(), page));
    }
    if (options.contains("--fit")) {
        const auto model = fitted(books, layout, -1);
        std::printf("W");
        for (const double weight : model.weight)
            std::printf("|%.3f", weight);
        std::printf("\n");
        return;
    }
    const bool reference = options.contains("--reference");

    const struct
    {
        const char *name;
        double fraction;
    } reads[] = { { "none", -1 }, { "1", 0.01 }, { "5", 0.05 }, { "20", 0.2 }, { "50", 0.5 } };

    for (int b = 0; b < books.size(); ++b) {
        const auto &book = books[b];
        const auto prior = reference ? PageEstimateModel::reference() : fitted(books, layout, b);
        qint64 updateUs = 0;
        for (const auto &read : reads) {
            if (read.fraction < 0) {
                QVector<double> starts{ 0 };
                for (const auto &item : book.items)
                    starts.append(starts.last() + firstVersion(item, layout));
                print(book.name, read.name, "first",
                      error(book, -1, starts.last(), [&](int i) { return starts[i]; }));
            } else {
                print(book.name, read.name, "bytes", bytes(book, read.fraction));
            }
            print(book.name, read.name, "model",
                  calibrated(book, layout, prior, read.fraction, &updateUs));
        }
        std::printf("B|%s|%d|%d|%lld|%lld|%lld|%lld\n", qPrintable(book.name),
                    int(book.items.size()), book.pages, (long long)book.openMs,
                    (long long)book.measureMs, (long long)book.partUs, (long long)updateUs);
    }
}

// The page map on the checked-in book: stable, exact when every spine item
// has a layout, better or as good with observations, and within a bound
// before any.
void map(const Book &book, const PageEstimateLayout &layout)
{
    const auto model = PageEstimateModel::reference();
    const auto first = mapOf(book, layout, model), second = mapOf(book, layout, model);
    for (int i = 0; i <= book.items.size(); ++i)
        require(first.pagesBefore(i) == second.pagesBefore(i), "The estimate must be stable");

    const auto before = calibrated(book, layout, model, -1);
    std::printf("fixture: %d pages, estimate %.1f%%, worst %.0f pages (%.2f%%), mean %.2f%%\n",
                book.pages, before.total, before.worstPages, before.worstPercent,
                before.meanPercent);
    // The bound of this book: 5 percent of the book for each spine item start
    // and 15 percent for the total.
    require(before.worstPercent <= 5, "A spine item start is too far from its real page");
    require(std::fabs(before.total - 100) <= 15, "The total is too far from the real one");

    // Observations in reading order. The estimate of the pages that are not
    // read yet improves or holds, within half a page for each observation.
    auto map = mapOf(book, layout, model);
    double previous = std::fabs(map.total() - book.pages);
    for (int i = 0; i < book.items.size(); ++i) {
        map.observe(i, 0, 1, 1, book.real(i));
        const double now = std::fabs(map.total() - book.pages);
        require(now <= previous + 0.5 * (i + 1) + 1e-9 || now <= 1,
                "An observation made the total much worse");
        previous = qMin(previous, now);
    }
    require(std::fabs(map.total() - book.pages) < 1e-6,
            "With every spine item laid out, the total is the real one");
    for (int i = 0; i < book.items.size(); ++i)
        require(std::fabs(map.pagesBefore(i) - book.starts[i]) < 1e-6,
                "With every spine item laid out, each start is the real one");

    // The weights stay near the reference for a part that matches it.
    PageMap single;
    single.reset(book.bytes);
    single.setLayout(layout, 0);
    single.setMeasured(0, book.items[0]);
    const double start = single.total();
    single.observe(0, 0, 1, 1, qRound(single.itemPages(0)));
    require(std::fabs(single.total() - start) <= 1, "A matching observation must not move much");

    PageAnchor anchor;
    require(anchor.place(10, PageAnchor::Jump, false) == 10, "A jump shows the estimate");
    require(anchor.place(5, PageAnchor::Forward, false) == 11, "Forward adds one");
    require(anchor.place(5, PageAnchor::Forward, true) == 12,
            "A smaller estimate never moves the page back");
    require(anchor.place(20, PageAnchor::Forward, true) == 20,
            "A larger estimate moves the page up at a spine item start");
    require(anchor.place(30, PageAnchor::Forward, false) == 21,
            "Inside a spine item the page only adds one");
    require(anchor.place(30, PageAnchor::Same, false) == 21, "The same page keeps its number");
    require(anchor.place(3, PageAnchor::Backward, false) == 20, "Backward takes one away");
}

// Counted pages win over the guess from the bytes, before the background
// measurer reaches the spine item: a complete item counts with its real
// pages, an empty one with none, and a partly observed one with at least the
// pages observed in it.
void unmeasured(const PageEstimateLayout &layout)
{
    const QVector<qint64> bytes{ 1000, 4000, 200, 3000 };
    PageMap map;
    map.reset(bytes);
    map.setLayout(layout, 0);
    const auto equal = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    const auto consistent = [&] {
        double sum = 0;
        for (int i = 0; i < bytes.size(); ++i) {
            require(equal(map.pagesBefore(i), sum), "The start of an item is the sum before it");
            require(equal(map.pagesBefore(i, 10, 10), map.itemPages(i)),
                    "The end of an item is its pages");
            sum += map.itemPages(i);
        }
        require(equal(map.total(), sum), "The total is the sum of the items");
    };
    const int counted = 7;
    map.observe(3, 0, 30, 30, 6);
    map.observe(1, 0, 40, 40, counted);
    map.observe(2, 0, 2, 2, 0);
    require(map.nextUnmeasured() == 0, "No item may be measured");
    require(equal(map.itemPages(1), counted), "A complete item counts with its real pages");
    require(equal(map.itemPages(2), 0), "An empty item counts with no pages");
    require(equal(map.pagesBefore(1, 20, 40), counted / 2.0),
            "Inside a complete item, its real pages count");
    // The guess from the bytes uses the pages for one byte of the observed
    // parts. The total differs from it only by the observed items.
    const double perByte = (6.0 + counted) / (3000 + 4000 + 200);
    double guess = 0;
    for (const qint64 size : bytes)
        guess += qMax(1.0, size * perByte);
    const double difference = (6 - qMax(1.0, 3000 * perByte))
            + (counted - qMax(1.0, 4000 * perByte)) + (0 - qMax(1.0, 200 * perByte));
    std::printf("unmeasured: total %.2f, byte guess %.2f\n", map.total(), guess);
    require(equal(map.total() - guess, difference),
            "The total changes by the difference of the counted pages to the guess");
    consistent();

    // A partly observed item: the first half has more pages than the bytes
    // give for the whole item.
    map.observe(0, 0, 50, 100, 5);
    require(map.itemPages(0) >= 5, "A partly observed item keeps its observed pages");
    require(equal(map.pagesBefore(0, 50, 100), 5), "The observed part counts with its pages");
    consistent();
}

// The saved record: a round trip keeps it, and records that are not valid
// are refused or lose only their invalid parts.
void record()
{
    PageEstimateRecord record;
    record.model = PageEstimateModel::reference();
    record.layout = "0123456789abcdef";
    record.spineItems = 40;
    record.item = 12;
    record.first = 6000;
    record.page = 3;
    record.partPages = 7;
    record.shown = 412;
    record.itemShown = 25;
    for (const int item : { 0, 1, 2, 5, 6, 30 })
        record.read.insert(item, item + 3);
    const auto json = record.toJson();
    require(json.value("read").toArray().size() == 3, "Read spine items are saved as runs");
    PageEstimateRecord loaded;
    require(PageEstimateRecord::fromJson(json, &loaded), "A saved record must load");
    require(loaded.layout == record.layout && loaded.spineItems == 40 && loaded.item == 12
                    && loaded.first == 6000 && loaded.page == 3 && loaded.partPages == 7
                    && loaded.shown == 412 && loaded.itemShown == 25 && loaded.read == record.read,
            "A round trip keeps the record");
    for (int i = 0; i < F::Count; ++i)
        require(std::fabs(loaded.model.weight[i] - record.model.weight[i]) < 1e-4,
                "A round trip keeps the weights");
    const auto size = QJsonDocument(json).toJson(QJsonDocument::Compact).size();
    require(size < 400, "A small record must stay small");

    const auto changed = [&json](const char *key, const QJsonValue &value) {
        auto copy = json;
        if (value.isUndefined())
            copy.remove(key);
        else
            copy[key] = value;
        return copy;
    };
    require(!PageEstimateRecord::fromJson(QJsonValue(), &loaded), "No record, nothing loads");
    require(!PageEstimateRecord::fromJson(changed("version", 2), &loaded),
            "An unknown version is refused");
    auto weights = json.value("weights").toArray();
    weights.removeLast();
    require(!PageEstimateRecord::fromJson(changed("weights", weights), &loaded),
            "Missing weights are refused");
    weights = json.value("weights").toArray();
    weights[0] = QString("1");
    require(!PageEstimateRecord::fromJson(changed("weights", weights), &loaded),
            "A weight that is not a number is refused");
    weights[0] = 5000;
    require(!PageEstimateRecord::fromJson(changed("weights", weights), &loaded),
            "A weight out of range is refused");

    // Invalid parts are left out; the weights still load.
    QJsonArray place;
    for (const int value : { 12, 6000, 7, 7 })
        place.append(value);
    require(PageEstimateRecord::fromJson(changed("place", place), &loaded) && loaded.item < 0
                    && loaded.read == record.read,
            "A page outside its part drops only the place");
    require(PageEstimateRecord::fromJson(changed("page", 0), &loaded) && loaded.item < 0,
            "A page below one drops the place");
    QJsonArray runs, run;
    run.append(4);
    run.append(9);
    run.append(-1);
    run.append(9);
    runs.append(run);
    runs.append(QString("run"));
    require(PageEstimateRecord::fromJson(changed("read", runs), &loaded) && loaded.read.size() == 1
                    && loaded.read.value(4) == 9,
            "A run stops at its first invalid page");

    // Too many read spine items: only the limit loads.
    QJsonArray long_run;
    long_run.append(0);
    for (int i = 0; i < PageEstimateRecord::maxRead + 100; ++i)
        long_run.append(10);
    QJsonArray long_runs;
    long_runs.append(long_run);
    require(PageEstimateRecord::fromJson(changed("read", long_runs), &loaded)
                    && loaded.read.size() == PageEstimateRecord::maxRead,
            "At most the limit of read spine items loads");

    // The map: parts without a gap make a complete spine item. Restored
    // pages count, but do not change the weights.
    PageMap map;
    map.reset({ 1000, 1000, 1000 });
    map.setLayout(Page().layout(), 100);
    map.observe(0, 0, 100, 250, 4);
    map.observe(0, 100, 200, 250, 4);
    map.observe(0, 200, 250, 250, 2);
    map.observe(1, 0, 100, 250, 4);
    map.observe(1, 200, 250, 250, 2);
    const auto complete = map.completeItems();
    require(complete.size() == 1 && complete.value(0) == 10,
            "Only a spine item without a gap is complete");
    PageMap restored;
    restored.reset({ 1000, 1000, 1000 });
    restored.setLayout(Page().layout(), 100);
    const auto before = restored.model();
    QMap<int, int> pages;
    pages.insert(1, 30);
    pages.insert(2, 0);
    pages.insert(7, 30);
    PageEstimateRecord empty = record;
    empty.read = pages;
    require(PageEstimateRecord::fromJson(empty.toJson(), &loaded) && loaded.read == pages,
            "A spine item without pages is saved and loads");
    restored.restore(pages);
    require(restored.observations() == 2, "A restored spine item outside the book is left out");
    require(restored.completeItems().value(1) == 30, "A restored spine item is complete");
    require(restored.completeItems().value(2, -1) == 0,
            "A restored spine item without pages is complete with none");
    for (int i = 0; i < F::Count; ++i)
        require(restored.model().weight[i] == before.weight[i],
                "Restored pages do not change the weights");
}

#if __has_include(<koblaze.h>)
// Reads the checked-in book forward in chapter mode, in small slices.
// Returns the rough page of each page.
QVector<Engine::RoughPages> readForward(const QByteArray &bytes, const Page &page, bool measure,
                                        int *slicePages)
{
    EngineDocument model(test::readerFonts());
    QString error;
    require(model.open(bytes, page.viewport, page.settings(), &error), qPrintable(error));
    model.setChapterSlices(6);
    require(model.showSpineItems(0, 0, &error), qPrintable(error));
    require(model.go(0), "Cannot go to the first page");
    int steps = 0;
    while (measure && model.refinePageEstimate())
        ++steps;
    require(!measure || steps > 0, "Measuring must take steps");
    QVector<Engine::RoughPages> pages{ model.roughPages() };
    *slicePages = model.pages();
    while (model.turnInWindow(1)) {
        pages.append(model.roughPages());
        // The rough values do not change when asked again for the same page.
        const auto again = model.roughPages();
        require(again.page == pages.last().page && again.pages == pages.last().pages,
                "The same page must keep its rough values");
        *slicePages = qMax(*slicePages, model.pages());
    }
    require(!measure || !model.refinePageEstimate(), "Every spine item is measured at the end");
    return pages;
}

// Opens the checked-in book in chapter mode with `saved` state.
void openChapter(EngineDocument &model, const QByteArray &bytes, const QJsonObject &saved, int item)
{
    auto settings = saved;
    settings["koblaze_first_spine_item"] = item;
    settings["koblaze_last_spine_item"] = item;
    QString error;
    model.setChapterSlices(6);
    require(model.open(bytes, Page().viewport, settings, &error), qPrintable(error));
}

// A book that opens again shows the page number of the saved position, and
// keeps the corrected weights and the read spine items.
void reopen(const QByteArray &bytes)
{
    const Page page;
    EngineDocument first(test::readerFonts());
    QString error;
    require(first.open(bytes, page.viewport, page.settings(), &error), qPrintable(error));
    first.setChapterSlices(6);
    require(first.showSpineItems(0, 0, &error), qPrintable(error));
    require(first.go(0), "Cannot go to the first page");
    while (first.refinePageEstimate()) { }
    first.roughPages();
    for (int turn = 0; turn < 30; ++turn) {
        require(first.turnInWindow(1), "The fixture is too short to read on");
        first.roughPages();
    }
    const auto shown = first.roughPages();
    const auto saved = first.state();
    const auto record = saved.value("page_estimate").toObject();
    require(!record.isEmpty(), "Chapter mode saves a page estimate");
    require(record.value("page").toInt() == shown.page
                    && record.value("item_page").toInt() == shown.itemPage,
            "The record holds the page on the screen");
    require(!record.value("read").toArray().isEmpty(), "Read spine items are saved");
    const int item = first.spineItemOf(saved.value("position").toString());

    // The same page opens again with the same numbers once the book is
    // measured. Before that the reader shows percentages.
    EngineDocument second(test::readerFonts());
    openChapter(second, bytes, saved, item);
    require(second.roughPages().page == 0, "No page number before measuring");
    // A save before the measuring ends keeps the saved number and weights.
    PageEstimateRecord loaded;
    require(PageEstimateRecord::fromJson(second.state().value("page_estimate"), &loaded),
            "The book saves its record again");
    require(loaded.shown == shown.page, "The saved page number is kept while measuring");
    for (int i = 0; i < F::Count; ++i)
        require(std::fabs(loaded.model.weight[i] - record.value("weights").toArray()[i].toDouble())
                        < 1e-3,
                "The corrected weights carry over");
    while (second.refinePageEstimate()) { }
    const auto again = second.roughPages();
    std::printf("reopened: page %d of %d, saved page %d of %d\n", again.page, again.pages,
                shown.page, shown.pages);
    require(again.page == shown.page && again.itemPage == shown.itemPage,
            "The page number after opening again is the saved one");
    require(second.turnInWindow(1) && second.roughPages().page == shown.page + 1,
            "The next turn adds one");

    // Another font size: the numbers start from the estimate, with the
    // weights of the record.
    auto larger = saved;
    larger["font_pixels"] = page.font + 8;
    EngineDocument third(test::readerFonts());
    openChapter(third, bytes, larger, item);
    while (third.refinePageEstimate()) { }
    require(third.roughPages().page >= 1, "Another layout still has a page number");
    require(!PageEstimateRecord::fromJson(third.state().value("page_estimate"), &loaded)
                    || loaded.layout != record.value("layout").toString(),
            "Another layout has another digest");

    // A damaged record is ignored.
    auto damaged = saved;
    damaged["page_estimate"] = QString("damaged");
    EngineDocument fourth(test::readerFonts());
    openChapter(fourth, bytes, damaged, item);
    while (fourth.refinePageEstimate()) { }
    require(fourth.roughPages().page >= 1, "A damaged record leaves the estimate");
}

// A book for the observations: dense text whose slices must be extended,
// four short spine items that make one unit, and a long last spine item.
test::BookFiles observedBook()
{
    const QByteArray sentence =
            "The reader turns the page and the text goes on without a break in it. ";
    const auto chapter = [](const QByteArray &body) {
        return "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Part</title></head>"
               "<body>"
                + body + "</body></html>";
    };
    test::BookFiles files;
    QByteArray dense;
    for (int i = 0; i < 8; ++i)
        dense += "<p>" + sentence.repeated(70) + "</p>";
    files["dense.xhtml"] = chapter(dense);
    // Short spine items of different lengths: each has its own pages.
    const int lengths[] = { 1, 12, 4, 20 };
    for (int i = 0; i < 4; ++i) {
        QByteArray body = "<h1>Short " + QByteArray::number(i + 1) + "</h1>";
        for (int p = 0; p < lengths[i]; ++p)
            body += "<p>" + sentence.repeated(2) + "</p>";
        files[QString("short%1.xhtml").arg(i + 1)] = chapter(body);
    }
    QByteArray last;
    for (int i = 0; i < 120; ++i)
        last += "<p>" + sentence.repeated(3) + "</p>";
    files["last.xhtml"] = chapter(last);
    return test::publisherPackage(
            "en", "Observations", files,
            "<itemref idref=\"dense.xhtml\"/><itemref idref=\"short1.xhtml\"/>"
            "<itemref idref=\"short2.xhtml\"/><itemref idref=\"short3.xhtml\"/>"
            "<itemref idref=\"short4.xhtml\"/><itemref idref=\"last.xhtml\"/>");
}

// Opens the files of a book with `settings`, which can hold a saved state.
void openResources(EngineDocument &model, const test::BookFiles &files, const QJsonObject &settings)
{
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto source = std::make_shared<ResourceCache>(
            files.keys(),
            [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    QString error;
    require(model.openResources(source, sizes, { 600, 800 }, settings, &error), qPrintable(error));
}

void openFiles(EngineDocument &model, const test::BookFiles &files)
{
    openResources(model, files, { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
    model.setChapterSlices(6);
    QString error;
    require(model.showSpineItems(0, 0, &error), qPrintable(error));
    require(model.go(0), "Cannot go to the first page");
}

// A spine item without text between two chapters: the forward turn past it
// adds one page, also when the number shown is ahead of the estimate. The
// second chapter is short, so that it shares a unit with the empty item, or
// long, so that the empty item is alone.
void emptyItem()
{
    const QByteArray sentence =
            "The reader turns the page and the text goes on without a break in it. ";
    const auto chapter = [](const QByteArray &body) {
        return "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Part</title></head>"
               "<body>"
                + body + "</body></html>";
    };
    const auto paragraphs = [&](int count) {
        QByteArray body = "<h1>Chapter</h1>";
        for (int p = 0; p < count; ++p)
            body += "<p>" + sentence.repeated(3) + "</p>";
        return body;
    };
    for (const int second : { 6, 80 }) {
        test::BookFiles files;
        files["one.xhtml"] = chapter(paragraphs(40));
        files["empty.xhtml"] = chapter("");
        files["two.xhtml"] = chapter(paragraphs(second));
        files = test::publisherPackage(
                "en", "Empty", files,
                "<itemref idref=\"one.xhtml\"/><itemref idref=\"empty.xhtml\"/>"
                "<itemref idref=\"two.xhtml\"/>");
        EngineDocument first(test::readerFonts());
        openFiles(first, files);
        while (first.refinePageEstimate()) { }
        first.roughPages();
        auto saved = first.state();
        auto record = saved.value("page_estimate").toObject();
        require(!record.isEmpty(), "Chapter mode saves a page estimate");
        saved["koblaze_first_spine_item"] = 0;
        saved["koblaze_last_spine_item"] = 0;

        EngineDocument model(test::readerFonts());
        model.setChapterSlices(6);
        openResources(model, files, saved);
        // The saved number is held while the book is measured.
        require(model.roughPages().page == 0, "No page number before measuring");
        while (model.refinePageEstimate()) { }
        auto rough = model.roughPages();
        require(rough.page == record.value("page").toInt(), "The saved page number comes back");
        QVector<int> items{ model.spineItemOf(model.position()) };
        while (model.turnInWindow(1)) {
            const auto next = model.roughPages();
            items.append(model.spineItemOf(model.position()));
            // The estimate never goes back. The numbers move once, to the
            // real pages.
            require(next.page >= rough.page || (rough.pagesEstimated && !next.pagesEstimated),
                    qPrintable(QString("Page %1 followed page %2 in spine item %3")
                                       .arg(next.page)
                                       .arg(rough.page)
                                       .arg(items.last())));
            rough = next;
        }
        require(rough.page == items.size() && rough.pages == items.size(),
                "A book read as a whole ends on its real last page");
        std::printf("empty spine item, second chapter of %d paragraphs: %lld pages, last %d\n",
                    second, qlonglong(items.size()), rough.page);
        require(items.first() == 0 && items.last() == 2, "The book was not read to its end");
        require(!items.contains(1), "The empty spine item has no page");
        // Every spine item had a layout, the empty one too.
        require(Probe::map(model).completeItems().value(1, -1) == 0,
                "The empty spine item is complete with no pages");
        require(!rough.pagesEstimated, "The total of a book read as a whole is not estimated");
    }
}

// A long spine item has a layout for one part at a time. A search of the
// book must find the matches in every part, as in the whole book.
void searchLongItem()
{
    const QByteArray sentence =
            "The reader turns the page and the text goes on without a break in it. ";
    QByteArray body;
    for (int i = 0; i < 400; ++i)
        body += "<p>" + sentence.repeated(3) + (i % 37 == 5 ? "A quagmire here. " : "") + "</p>";
    test::BookFiles files;
    files["long.xhtml"] = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Part</title>"
                          "</head><body>"
            + body + "</body></html>";
    files["after.xhtml"] = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>After"
                           "</title></head><body><p>One more quagmire.</p></body></html>";
    files = test::publisherPackage(
            "en", "Long", files, "<itemref idref=\"long.xhtml\"/><itemref idref=\"after.xhtml\"/>");
    const auto hits = [](EngineDocument &model) {
        QStringList starts;
        SearchCursor cursor;
        for (int round = 0; round < 100000; ++round) {
            const auto batch = model.searchText("quagmire", cursor, 100);
            for (const auto &hit : batch.hits)
                starts.append(hit.start);
            cursor = batch.cursor;
            if (batch.done)
                return starts;
        }
        require(false, "The search must end");
        return starts;
    };
    EngineDocument whole(test::readerFonts());
    openResources(whole, files, { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
    const auto expected = hits(whole);
    require(expected.size() == 12, "The fixture has twelve matches");
    EngineDocument model(test::readerFonts());
    openFiles(model, files);
    const auto shown = model.pageStart(model.page());
    const auto found = hits(model);
    require(found == expected, "Chapter mode must find the matches of the whole book");
    require(model.pageStart(model.page()) == shown, "The search must not move the page on screen");
    // The list asks for the text around each hit later. A hit in a part
    // that has no layout at that moment gets the same text as in the whole book.
    const auto details = [&](EngineDocument &document) {
        QVector<SearchHit> wanted;
        for (const auto &start : expected)
            wanted.append({ start, start, {}, {}, {} });
        QStringList texts;
        for (const auto &hit : document.searchDetails(wanted, SearchScope::Book, {}))
            texts.append(hit.start + " | " + hit.before + " | " + hit.after);
        return texts;
    };
    const auto wholeDetails = details(whole), chapterDetails = details(model);
    require(wholeDetails.size() == expected.size() && !wholeDetails.first().contains("|  |"),
            "The whole book must give the text around each hit");
    require(chapterDetails == wholeDetails,
            "Chapter mode must give the text around a hit as the whole book does");
    require(model.pageStart(model.page()) == shown,
            "The text around the hits must not move the page on screen");
}

// In a long spine item, a forward step to the first page of a slice that is
// not the next one shows the estimate there, not one more than the last
// page. Steps to the next slice never go down.
void laterSlice()
{
    const QByteArray sentence =
            "The reader turns the page and the text goes on without a break in it. ";
    QByteArray body;
    for (int i = 0; i < 400; ++i)
        body += "<p>" + sentence.repeated(3) + "</p>";
    test::BookFiles files;
    files["long.xhtml"] = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Part</title>"
                          "</head><body>"
            + body + "</body></html>";
    files = test::publisherPackage("en", "Long", files, "<itemref idref=\"long.xhtml\"/>");
    EngineDocument model(test::readerFonts());
    openFiles(model, files);
    while (model.refinePageEstimate()) { }
    auto rough = model.roughPages();
    const auto forward = [&] {
        require(model.turnInWindow(1), "The fixture is too short to read on");
        const auto next = model.roughPages();
        require(next.page > rough.page && next.itemPage > rough.itemPage,
                "The page number went back on a forward turn");
        rough = next;
    };
    // Into the second slice, and to its last page.
    while (Probe::sliceFirst(model) == 0)
        forward();
    while (model.page() + 1 < Probe::visiblePages(model))
        forward();
    const int target = Probe::sliceFirst(model) + 10 * Probe::sliceSize(model);
    require(target < Probe::spineItemText(model), "The spine item is too short");
    require(Probe::layoutSlice(model, target) && model.go(0), "Cannot lay out a later slice");
    const auto far = model.roughPages();
    const auto &map = Probe::map(model);
    const double inItem =
            map.pagesBefore(0, Probe::sliceFirst(model), qMax(1, Probe::spineItemText(model)));
    const int estimate = qRound(map.pagesBefore(0) + inItem) + 1;
    std::printf("later slice: page %d after page %d, estimate %d\n", far.page, rough.page,
                estimate);
    require(estimate > rough.page + 10, "The later slice is too close");
    require(far.page == estimate && far.itemPage == qRound(inItem) + 1,
            "A step to a later slice shows the estimate");
    rough = far;
    while (model.turnInWindow(1)) {
        const auto next = model.roughPages();
        // The estimate never goes back. The numbers move once, to the real
        // pages.
        require(next.page >= rough.page || (rough.pagesEstimated && !next.pagesEstimated),
                "The page number went back on a forward turn");
        rough = next;
    }
}

// A change of settings that does not move the pages keeps the observed
// pages and the shown number. One that moves them forgets the observations.
void keptObservations()
{
    EngineDocument model(test::readerFonts());
    openFiles(model, observedBook());
    // Into the unit of short spine items, which gives one observation each.
    while (Probe::map(model).observations() < 3) {
        require(model.turnInWindow(1), "The fixture is too short to read on");
        model.roughPages();
    }
    const int observed = Probe::map(model).observations();
    const int shown = model.roughPages().page;
    model.setDarkMode(true);
    require(Probe::map(model).observations() == observed && model.roughPages().page == shown,
            "Dark mode forgot the observed pages");
    require(model.applyAppearance(
                    appearance::Patch(qt::object({ { "normalize_text_colors", true } }))),
            "Cannot normalize the text colors");
    require(Probe::map(model).observations() == observed && model.roughPages().page == shown,
            "The text colors forgot the observed pages");
    require(model.applyAppearance(appearance::Patch(qt::object({ { "font_pixels", 32 } }))),
            "Cannot change the font size");
    require(Probe::map(model).observations() < observed,
            "Another font size kept the observed pages");
}

// Each layout gives the page estimate its real pages: a slice with the
// characters it really holds, also when it was extended, and each spine
// item of a unit with its own pages. A stopped layout gives none. The page
// number never goes back while the reader turns forward, also into a spine
// item that was opened ahead.
void observations()
{
    const auto files = observedBook();
    EngineDocument model(test::readerFonts());
    openFiles(model, files);
    while (model.refinePageEstimate()) { }
    const int slice = Probe::sliceSize(model);
    QMap<int, int> turned;
    int extended = 0, units = 0, crossings = 0, adopted = 0, stops = 0;
    auto rough = model.roughPages();
    require(rough.page == 1, "The book starts on page one");
    for (int turn = 0;; ++turn) {
        const int item = model.spineItemOf(model.position());
        ++turned[item];
        if (Probe::windowFirst(model) == 0
            && Probe::sliceEnd(model) - Probe::sliceFirst(model) > slice)
            ++extended;
        if (Probe::windowLast(model) > Probe::windowFirst(model))
            ++units;
        // Work ahead that stops before it ends adds no observation.
        const int observed = Probe::map(model).observations();
        const int first = Probe::sliceFirst(model), window = Probe::windowFirst(model);
        const auto *ahead = Probe::ahead(model);
        int checks = 0;
        bool asked = false;
        const int limit = (turn * 7) % 45;
        model.prepareAhead([&] {
            asked = asked || ++checks > limit;
            return !asked;
        });
        if (asked && Probe::sliceFirst(model) == first && Probe::windowFirst(model) == window
            && Probe::ahead(model) == ahead) {
            ++stops;
            require(Probe::map(model).observations() == observed,
                    "A stopped layout gave an observation");
        }
        // The reader prepares the work ahead completely in most idle times.
        if (turn % 2)
            model.prepareAhead();
        const auto *prepared = Probe::ahead(model);
        const auto *preparedView = prepared ? Probe::view(*prepared) : nullptr;
        const int before = Probe::windowFirst(model);
        if (!model.turnInWindow(1))
            break;
        if (prepared && Probe::windowFirst(model) != before) {
            ++crossings;
            adopted += Probe::view(model) == preparedView;
        }
        const auto next = model.roughPages();
        // An estimate can repeat a number at the end of a spine item. The
        // numbers move once, when the book has its real pages.
        require(next.page >= rough.page || (rough.pagesEstimated && !next.pagesEstimated),
                "The page number went back on a forward turn");
        rough = next;
    }
    std::printf("observations: %d pages in extended slices, %d pages in a unit, %d of %d "
                "spine items shown from ahead, %d stops\n",
                extended, units, adopted, crossings, stops);
    require(extended > 0, "The dense spine item needs extended slices");
    require(units > 0, "The short spine items must make one unit");
    require(adopted > 0 && adopted == crossings, "A spine item opened ahead must be shown");
    require(stops > 5, "The work ahead was not stopped");
    // Every spine item was read as a whole: its real pages are the pages
    // that were turned in it, each item of the unit with its own.
    const auto complete = Probe::map(model).completeItems();
    for (auto it = turned.cbegin(); it != turned.cend(); ++it)
        require(complete.value(it.key(), -1) == it.value(),
                qPrintable(QString("Spine item %1 observed %2 pages, %3 were turned")
                                   .arg(it.key())
                                   .arg(complete.value(it.key(), -1))
                                   .arg(it.value())));
    require(turned.size() == 6, "Every spine item has pages");
    require(turned.value(1) < turned.value(4), "The short spine items have their own pages");

    // The idle work measures after it prepares the next unit. The work ahead
    // in the next idle time keeps the document it opened.
    EngineDocument fresh(test::readerFonts());
    openFiles(fresh, files);
    QString error;
    require(fresh.showSpineItems(1, 4, &error) && fresh.go(0), qPrintable(error));
    fresh.roughPages();
    fresh.prepareAhead();
    const auto *opened = Probe::ahead(fresh);
    require(opened, "The last unit before a spine item opens it ahead");
    require(fresh.refinePageEstimate(), "The book is not measured yet");
    while (fresh.refinePageEstimate()) { }
    fresh.roughPages();
    fresh.prepareAhead();
    require(Probe::ahead(fresh) == opened,
            "Measuring made the work ahead open the next unit again");
}

// A book for the rough page numbers: a cover without text, a long chapter in
// several slices, a picture without text between chapters, another long
// chapter and two short ones that make one unit.
test::BookFiles pagesBook()
{
    const QByteArray sentence =
            "The reader turns the page and the text goes on without a break in it. ";
    const auto chapter = [](const QByteArray &body) {
        return "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Part</title></head>"
               "<body>"
                + body + "</body></html>";
    };
    const auto paragraphs = [&](int count) {
        QByteArray body = "<h1>Chapter</h1>";
        for (int p = 0; p < count; ++p)
            body += "<p>" + sentence.repeated(3) + "</p>";
        return body;
    };
    const QByteArray picture = "<div><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"300\" "
                               "height=\"400\" viewBox=\"0 0 300 400\"><rect width=\"300\" "
                               "height=\"400\" fill=\"#888888\"/></svg></div>";
    test::BookFiles files;
    files["cover.xhtml"] = chapter(picture);
    files["long.xhtml"] = chapter(paragraphs(150));
    files["plate.xhtml"] = chapter(picture);
    files["middle.xhtml"] = chapter(paragraphs(120));
    files["short1.xhtml"] = chapter(paragraphs(4));
    files["short2.xhtml"] = chapter(paragraphs(6));
    return test::publisherPackage(
            "en", "Pages", files,
            "<itemref idref=\"cover.xhtml\"/><itemref idref=\"long.xhtml\"/>"
            "<itemref idref=\"plate.xhtml\"/><itemref idref=\"middle.xhtml\"/>"
            "<itemref idref=\"short1.xhtml\"/><itemref idref=\"short2.xhtml\"/>");
}

// Resolving a saved place can replace the initial chapter while its opening
// callback is still active. Retained chapters must not keep that stack pointer.
void savedPlaceOutsideOpeningWindow()
{
    const auto files = pagesBook();
    const QJsonObject settings{ { "font_pixels", 28 }, { "line_spacing_version", 1 } };
    EngineDocument whole(test::readerFonts());
    openResources(whole, files, settings);
    const auto chapter = whole.chapters().last();
    require(whole.goTo(chapter.position), "Cannot select a later chapter");
    const auto place = whole.position();
    auto saved = settings;
    saved["position"] = place;
    saved["koblaze_first_spine_item"] = 0;
    saved["koblaze_last_spine_item"] = 0;
    EngineDocument model(test::readerFonts());
    model.setChapterSlices(1);
    openResources(model, files, saved);
    require(model.position() == place, "Opening must resolve a place outside its initial window");
    require(ReaderLocationProbe::callbacksDetached(model),
            "Opening left a dead callback on a retained chapter");
    // A further jump moves the initial view from the ahead slot into the
    // retained cache. Building the whole book clears that cache.
    require(model.goTo(whole.chapters().at(2).position), "Cannot jump to another chapter");
    const auto shown = model.position();
    require(model.prepareFullDocument({}) == Engine::FullDocument::Ready,
            "Cannot build the whole book after retaining a chapter");
    require(ReaderLocationProbe::callbacksDetached(model),
            "Background pagination retained an opening callback");
    require(model.setChapterMode(false, appearance::Patch(settings), { 600, 800 }),
            "Cannot replace retained chapters with the whole book");
    require(model.position() == shown, "The whole-book handoff changed the reading position");
    // Hybrid appearance edits return to chapter mode before changing text.
    // Repeat the edit and whole-book handoff away from the cover.
    for (const int pixels : { 36, 20, 28 }) {
        const appearance::Patch next(qt::object({ { "font_pixels", pixels } }));
        require(model.setChapterMode(true, next, { 600, 800 }),
                "Cannot change text size in chapter mode");
        require(model.position() == shown, "A Hybrid font change returned to another position");
        require(model.prepareFullDocument({}) == Engine::FullDocument::Ready,
                "Cannot rebuild the whole book after a font change");
        require(model.setChapterMode(false, next, { 600, 800 }) && model.position() == shown,
                "The handoff after a font change moved the reading position");
        require(Probe::callbacksDetached(model), "A font change retained a callback");
    }
}

// Reflow and saved-position resolution open nested slice layouts. A caller
// that was interrupted must not wait for those layouts to finish.
void nestedSliceHonorsCancellation()
{
    // The opening paragraph needs an extension. Forced breaks later in the chapter
    // give the layout whole pages on which to end that slice.
    QByteArray body = "<p>" + QByteArray("The reader turns the page. ").repeated(600) + "</p>";
    for (int i = 0; i < 40; ++i)
        body += "<p style='page-break-before:always'>Page " + QByteArray::number(i) + ". "
                + QByteArray("The chapter continues. ").repeated(30) + "</p>";
    test::BookFiles files;
    files["text.xhtml"] = test::publisherHtml(body);
    files = test::publisherPackage("en", "Reflow", files, "<itemref idref=\"text.xhtml\"/>");
    EngineDocument model(test::readerFonts()), reference(test::readerFonts());
    openFiles(model, files);
    openFiles(reference, files);
    const auto compare = [&] {
        require(Probe::sliceFirst(model) == Probe::sliceFirst(reference)
                        && Probe::sliceEnd(model) == Probe::sliceEnd(reference)
                        && Probe::visiblePages(model) == Probe::visiblePages(reference)
                        && model.pages() == reference.pages() && model.page() == reference.page(),
                "Resumed layout differs from the uninterrupted slice or page");
        const auto start = reference.pageStart(reference.page());
        require(!start.isEmpty() && model.pageStart(model.page()) == start
                        && model.position() == reference.position(),
                "Resumed layout changed the page start or reading position");
        const auto expected = reference.image(reference.page());
        require(!expected.isNull() && model.image(model.page()) == expected,
                "Resumed layout painted a different page");
    };
    const auto turn = [&](int direction) {
        require(reference.turnInWindow(direction) && model.turnInWindow(direction),
                "Cannot turn to the comparison page");
        compare();
    };
    const auto interrupt = [&](bool duringExtension) {
        require(!Probe::layoutSliceWithCancelledCaller(model, duringExtension)
                        && model.layoutInterrupted(),
                duringExtension ? "A nested slice failed to stop during extension"
                                : "A nested slice ignored its caller's cancellation");
        require(model.resumeLayout() && !model.layoutInterrupted(),
                "A cancelled nested slice failed to resume");
        require(Probe::callbacksDetached(model), "The interrupted slice retained a callback");
        compare();
    };
    interrupt(false);
    turn(1);
    interrupt(true);
    turn(-1);

    const int boundary = Probe::sliceEnd(reference);
    require(boundary > 0 && boundary < Probe::spineItemText(reference),
            "The fixture must have a second slice");
    const int lookAhead = Probe::visiblePages(reference);
    require(lookAhead > 0 && reference.go(lookAhead) && model.go(lookAhead),
            "The fixture must have a page laid out beyond the cut");
    const int observations = Probe::map(model).observations();
    bool stopped = false;
    model.prepareAhead([&] {
        stopped = stopped || Probe::sliceFirst(model) == boundary;
        return !stopped;
    });
    require(stopped && !model.layoutInterrupted() && Probe::callbacksDetached(model),
            "Preparing the next slice must stop and restore a usable layout");
    require(Probe::map(model).observations() == observations,
            "A cancelled predictive layout recorded an observation");
    compare();
    turn(-1);
    require(reference.go(lookAhead) && model.go(lookAhead), "Cannot return to the look-ahead page");
    reference.prepareAhead();
    model.prepareAhead();
    require(Probe::sliceFirst(reference) == boundary && reference.page() == 0,
            "Preparing the next slice must enter its first page");
    compare();
    interrupt(false);
    turn(-1);
    require(Probe::sliceFirst(reference) == 0, "The reverse turn must cross the slice boundary");
    turn(1);
}

void parkWholeBook(EngineDocument &model)
{
    int checks = 0;
    require(model.prepareFullDocument([&] { return ++checks <= 5; })
                            == Engine::FullDocument::Stopped
                    && model.fullDocumentStarted() && !model.fullDocumentReady(),
            "The whole-book build must have started and parked");
}

void returnToEvictedBookmark()
{
    const auto files = test::engineBook(false, 10, 36);
    const QJsonObject settings{ { "font_pixels", 28 }, { "line_spacing_version", 1 } };
    EngineDocument whole(test::readerFonts());
    openResources(whole, files, settings);
    const auto chapters = whole.chapters();
    require(chapters.size() == 10 && whole.goTo(chapters[1].position) && whole.go(whole.page() + 2),
            "Cannot choose a bookmark inside the second chapter");
    const auto bookmark = whole.position();
    auto saved = settings;
    saved["koblaze_first_spine_item"] = 0;
    saved["koblaze_last_spine_item"] = 0;
    saved["position"] = bookmark;
    EngineDocument model(test::readerFonts());
    model.setChapterSlices(2);
    openResources(model, files, saved);
    require(model.position() == bookmark, "Opening did not restore the bookmark");
    parkWholeBook(model);
    for (int chapter = 2; chapter < chapters.size(); ++chapter)
        require(model.goTo(chapters[chapter].position), "Cannot jump while pagination is parked");
    require(!Probe::hasChapter(model, 1), "The bookmarked chapter must have been evicted");
    require(model.goTo(bookmark) && model.position() == bookmark,
            "Returning to an evicted bookmark changed its position");
    for (int direction : { 0, -1, 1, 1 }) {
        if (direction)
            require(model.turnInWindow(direction) && whole.go(whole.page() + direction),
                    "Cannot turn beside the restored bookmark");
        require(model.pageStart(model.page()) == whole.pageStart(whole.page())
                        && model.image(model.page()) == whole.image(whole.page()),
                "The restored bookmark or its neighbouring page differs from the whole book");
    }
    const auto shown = model.position();
    require(model.prepareFullDocument({}) == Engine::FullDocument::Ready
                    && model.setChapterMode(false, {}, {}) && model.position() == shown
                    && model.image(model.page()) == whole.image(whole.page()),
            "Finishing the parked build moved the reader after the bookmark return");
}

void reopenAfterWholeBookPreparation()
{
    const auto first = test::engineBook(false, 3, 15);
    auto second = test::engineBook(false, 3, 30);
    second["OEBPS/c1.xhtml"].replace("Chapter 1", "Another book");
    EngineDocument reference(test::readerFonts());
    openResources(reference, second, { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
    for (const bool parked : { false, true }) {
        EngineDocument model(test::readerFonts());
        openFiles(model, first);
        require(model.image(0) != reference.image(0), "The books must have distinct first pages");
        if (parked)
            parkWholeBook(model);
        else
            require(model.prepareFullDocument({}) == Engine::FullDocument::Ready,
                    "Cannot prepare the first book");
        openFiles(model, second);
        require(!model.fullDocumentStarted(),
                "Opening another book retained its predecessor's pagination");
        require(model.prepareFullDocument({}) == Engine::FullDocument::Ready
                        && model.setChapterMode(false, {}, { 600, 800 }),
                "Cannot switch to the second whole book");
        require(model.pages() == reference.pages() && model.page() == 0,
                "The handoff restored a page count or position from the first book");
        for (const int page : { 0, 1, reference.pages() - 1 })
            require(model.pageStart(page) == reference.pageStart(page)
                            && model.image(page) == reference.image(page),
                    "The replacement book differs from an independent opening");
    }
}

// Only matched, visible unsupported writing modes on this page cause a warning.
void pageFormattingWarnings()
{
    const auto book = [](const QByteArray &css, const QByteArray &body) {
        test::BookFiles files;
        files["chapter.xhtml"] =
                "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
                "<head><title>Page warnings</title><style>"
                + css + "</style></head><body>" + body + "</body></html>";
        return test::publisherPackage("en", "Warnings", files,
                                      "<itemref idref=\"chapter.xhtml\"/>");
    };

    struct Case
    {
        const char *css;
        const char *body;
        bool warning;
    };

    const Case cases[] = {
        { "", "<p>Supported text.</p>", false },
        { ".unused{writing-mode:vertical-lr}", "<p>Supported text.</p>", false },
        { "p{writing-mode:sideways-rl}", "<p>Unsupported text.</p>", true },
        { "p{writing-mode:sideways-lr}", "<p>Unsupported text.</p>", true },
        { "p{writing-mode:tb-rl}", "<p>Unsupported text.</p>", true },
        { "p{writing-mode:vertical-lr}", "<p>Unsupported text.</p>", true },
        { "p{writing-mode:vertical-rl}", "<p>Supported text.</p>", false },
        { "p{writing-mode:vertical-lr}p{writing-mode:horizontal-tb}", "<p>Override.</p>", false },
        { "body{writing-mode:sideways-rl}p{writing-mode:horizontal-tb}", "<p>Override.</p>",
          false },
        { "body{writing-mode:sideways-rl}", "<p>Inherited warning.</p>", true },
        { "p{writing-mode:sideways-rl!important}p{writing-mode:horizontal-tb}", "<p>Important.</p>",
          true },
        { "p{writing-mode:sideways-rl}p{writing-mode:horizontal-tb!important}", "<p>Important.</p>",
          false },
        { "p{writing-mode:sideways-rl}p{writing-mode:initial}", "<p>Initial.</p>", false },
        { "body{writing-mode:sideways-rl}p{writing-mode:inherit}", "<p>Inherited.</p>", true },
        { "body{writing-mode:sideways-rl}p{writing-mode:unset}", "<p>Inherited.</p>", true },
        { ".bad{writing-mode:sideways-rl;display:none}",
          "<p class='bad'>Hidden.</p><p>Visible.</p>", false },
        { ".bad{writing-mode:sideways-rl;visibility:hidden}",
          "<p class='bad'>Hidden.</p><p>Visible.</p>", false },
        { ".bad{writing-mode:sideways-rl;visibility:hidden}.visible{visibility:visible}",
          "<p class='bad'><span class='visible'>Visible.</span></p>", true },
        { "p{-ms-writing-mode:tb-rl}", "<p>Legacy.</p>", true },
        { "p{-moz-writing-mode:vertical-rl}", "<p>Legacy.</p>", true },
        { "p{-o-writing-mode:vertical-lr}", "<p>Legacy.</p>", true },
        { "p{-ms-writing-mode:tb-rl;writing-mode:horizontal-tb}", "<p>Override.</p>", false },
        { "@supports(writing-mode:sideways-rl){p{writing-mode:vertical-lr}}",
          "<p>Unsupported feature query.</p>", false },
        { "@supports(-ms-writing-mode:tb-rl){p{writing-mode:vertical-lr}}",
          "<p>Unsupported feature query.</p>", false },
        { "p::before{content:'Generated';writing-mode:sideways-rl}", "<p>Supported text.</p>",
          true },
    };
    for (const auto &entry : cases) {
        EngineDocument model(test::readerFonts());
        openResources(model, book(entry.css, entry.body),
                      { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
        require(!model.image(model.page()).isNull(), "The warning fixture must draw");
        if (pageFormattingWarning(model.image(model.page())) != entry.warning) {
            std::fprintf(stderr, "Wrong page warning for CSS: %s; body: %s\n", entry.css,
                         entry.body);
            require(false, "Page warning did not follow visible computed CSS");
        }
    }
    const auto pages = book("p{page-break-before:always}.bad{writing-mode:sideways-rl}",
                            "<p>First supported page.</p><p class='bad'>Unsupported page.</p>"
                            "<p>Last supported page.</p>");
    for (bool chapter : { false, true }) {
        EngineDocument model(test::readerFonts());
        openResources(model, pages, { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
        if (chapter) {
            model.setChapterSlices(6);
            QString error;
            require(model.showSpineItems(0, 0, &error), qPrintable(error));
        }
        require(model.pages() == 3, "The warning fixture must have three pages");
        for (int page = 0; page < 3; ++page) {
            require(model.go(page) && !model.image(page).isNull(), "Cannot draw warning page");
            require(pageFormattingWarning(model.image(model.page())) == (page == 1),
                    "Warning must appear and clear inside the same chapter");
        }
    }
    {
        EngineDocument model(test::readerFonts());
        openResources(model, pages, { { "font_pixels", 28 }, { "columns", 2 } });
        require(pageFormattingWarning(model.image(0)),
                "Both displayed columns contribute warnings");
        require(model.go(model.pages() - 1) && !pageFormattingWarning(model.image(model.page())),
                "A supported spread must clear the warning");
    }
    {
        const QByteArray picture =
                "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200' "
                "style='writing-mode:sideways-rl'><rect width='200' height='200' fill='red'/></svg>";
        EngineDocument model(test::readerFonts());
        openResources(model, book("", picture), {});
        require(pageFormattingWarning(model.image(0)),
                "Image-only pages retain visible style warnings");
    }
    {
        const auto notes =
                book("aside{writing-mode:sideways-rl}",
                     "<p>Normal text <a href='#note' epub:type='noteref'>1</a>.</p>"
                     "<p style='page-break-before:always'>A later supported page.</p>"
                     "<aside id='note' epub:type='footnote'><p>Unsupported note.</p></aside>");
        EngineDocument model(test::readerFonts());
        openResources(model, notes, { { "inpage_footnotes", true } });
        require(pageFormattingWarning(model.image(0)), "Displayed footnotes contribute warnings");
        require(model.go(1) && !pageFormattingWarning(model.image(1)),
                "A note outside the displayed page must not contribute a warning");
    }
    {
        const QByteArray paragraph =
                "<p>Supported text fills a cacheable book without changing its formatting.</p>";
        const auto cached =
                book(".bad{writing-mode:sideways-rl}",
                     "<p class='bad'>Unsupported first paragraph.</p>" + paragraph.repeated(700));
        QTemporaryDir directory;
        qint64 bytes = 0;
        for (const auto &data : cached)
            bytes += data.size();
        for (int pass = 0; pass < 2; ++pass) {
            EngineDocument model(test::readerFonts());
            model.setDocumentCache(directory.path(), QByteArray(64, 'w'), "page-warning-test",
                                   bytes);
            openResources(model, cached, {});
            require(model.openedFromCache() == (pass == 1),
                    "Warning fixture must reopen from the parsed cache");
            require(pageFormattingWarning(model.image(0)),
                    "Cached styles must retain unsupported intent");
            require(model.go(1) && !pageFormattingWarning(model.image(1)),
                    "Cached supported page must not warn");
            if (pass == 0)
                require(model.saveDocumentCache(), "Cannot save warning fixture cache");
        }
    }
    // A decoded image can have a box on this page without painting any pixels.
    for (const auto &fill :
         { QByteArray("fill='none'"), QByteArray("fill='red' fill-opacity='0'") }) {
        const auto transparent =
                book("",
                     "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100' "
                     "style='writing-mode:sideways-rl'><rect width='100' height='100' "
                             + fill + "/></svg>");
        EngineDocument model(test::readerFonts());
        openResources(model, transparent, {});
        require(!pageFormattingWarning(model.image(0)), "Empty and transparent SVGs must not warn");
    }
    const auto imageBook = [&](const QByteArray &css, const QImage &image) {
        auto files = book(css, "<p><img src='alpha.png' width='100' height='100'/></p>");
        QBuffer bytes(&files["alpha.png"]);
        require(bytes.open(QIODevice::WriteOnly) && image.save(&bytes, "PNG"),
                "Cannot encode alpha-mask PNG");
        files["book.opf"].replace(
                "</manifest>",
                "<item id='alpha' href='alpha.png' media-type='image/png'/></manifest>");
        return files;
    };
    {
        QImage clear(100, 100, QImage::Format_ARGB32);
        clear.fill(Qt::transparent);
        EngineDocument model(test::readerFonts());
        openResources(model, imageBook("img{writing-mode:sideways-rl}", clear), {});
        require(!pageFormattingWarning(model.image(0)), "Transparent PNGs must not warn");
    }
    for (bool visibleOpaqueHalf : { false, true }) {
        QImage mask(100, 100, QImage::Format_ARGB32);
        mask.fill(Qt::transparent);
        for (int y = 0; y < 100; ++y)
            for (int x = visibleOpaqueHalf ? 50 : 0; x < (visibleOpaqueHalf ? 100 : 50); ++x)
                mask.setPixel(x, y, qRgb(255, 0, 0));
        EngineDocument model(test::readerFonts());
        openResources(model, imageBook("p{margin-left:-80px}img{writing-mode:sideways-rl}", mask),
                      { { "font_pixels", 28 }, { "normalize_text_colors", false } });
        const auto image = model.image(0);
        bool ink = false;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                ink |= qRed(image.pixel(x, y)) != 255 || qGreen(image.pixel(x, y)) != 255
                        || qBlue(image.pixel(x, y)) != 255;
        require(ink == visibleOpaqueHalf && pageFormattingWarning(image) == visibleOpaqueHalf,
                "Only the visible alpha-bearing part of a clipped image may warn");
    }
    // Unsupported declarations retain the old horizontal fallback.
    EngineDocument plain(test::readerFonts()), unsupported(test::readerFonts());
    openResources(plain, book("", "<p>Same fallback geometry.</p>"), {});
    openResources(unsupported,
                  book("p{writing-mode:sideways-rl}", "<p>Same fallback geometry.</p>"), {});
    require(plain.image(0) == unsupported.image(0), "Warning metadata changed fallback pixels");
}

test::BookFiles publisherFontBook(QByteArray font)
{
    auto files = pagesBook();
    files["publisher.ttf"] = std::move(font);
    require(!files["publisher.ttf"].isEmpty(), "Missing publisher font fixture");
    files["book.opf"].replace(
            "</manifest>",
            "<item id='publisher-font' href='publisher.ttf' media-type='font/ttf'/></manifest>");
    for (auto it = files.begin(); it != files.end(); ++it)
        if (it.key().endsWith(".xhtml"))
            it.value().replace(
                    "</head>",
                    "<style>@font-face {font-family: Publisher; src: url('publisher.ttf');} body {font-family: Publisher;}</style></head>");
    return files;
}

// Building the whole book reads fresh chapter resources, but reuses the
// publisher font bytes already read for the same publication.
void fullDocumentKeepsPublisherFonts()
{
    const auto files =
            publisherFontBook(test::readerAssets().fontData("InstrumentSerif-Regular.ttf"));
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto reads = std::make_shared<std::atomic_int>(0);
    auto resources = std::make_shared<ResourceCache>(
            files.keys(),
            [files, reads](const QString &name, const ResourceCache::Sink &sink, QString *) {
                if (name == "publisher.ttf")
                    ++*reads;
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    EngineDocument model(test::readerFonts());
    QString error;
    setChapterModeOverride(1);
    const bool opened = model.openResources(resources, sizes, { 600, 800 },
                                            { { "font_face", "" }, { "font_pixels", 28 } }, &error);
    setChapterModeOverride(-1);
    require(opened && reads->load() == 1, "The chapter must read its publisher font");
    require(model.prepareFullDocument({}) == Engine::FullDocument::Ready,
            "The publisher-font book must build beside its chapter");
    require(reads->load() == 1, "The whole build read the same publisher font again");
}

// A page request interrupts chapter-place reads without keeping a partial
// result or skipping that place when the idle work resumes.
void interruptedChapterPlace()
{
    EngineDocument model(test::readerFonts());
    openResources(model, pagesBook(), { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
    const auto target = model.pageStart(10);
    require(model.spineItemOf(target) == 1, "The target must be in the long chapter");
    model.setChapterSlices(6);
    QString error;
    require(model.showSpineItems(0, 0, &error), qPrintable(error));
    model.setChapters({ { "Later", target } }, {});
    int checks = 0;
    require(Probe::measurePlace(model, [&] { return ++checks <= 4; }) && checks > 4,
            "Chapter-place measurement did not stop during its read or parse");
    require(Probe::measuredPlaces(model) == 0 && Probe::textPlaces(model) == 0,
            "An interrupted chapter place was kept or skipped");
    require(!Probe::measurePlace(model, {}) && Probe::measuredPlaces(model) == 1
                    && Probe::textPlaces(model) == 1,
            "The interrupted chapter place was not retried");
}

// The layout at the open gives its pages, also of a spine item without text
// such as a cover.
void openObserved()
{
    EngineDocument model(test::readerFonts());
    openFiles(model, pagesBook());
    require(Probe::windowFirst(model) == 0 && Probe::spineItemText(model) == 0,
            "The book must open on a cover without text");
    require(Probe::map(model).completeItems().value(0, -1) == model.pages(),
            "The cover at the open counts with its pages");
}

// Read forward from the start, the page number is the page that was turned,
// through slices, spine items without text and a unit of spine items. At
// the end the page and the total are the real total, which is not estimated.
// Without measuring, no number is shown until every spine item is counted.
void forwardPages()
{
    for (const bool measure : { true, false }) {
        EngineDocument model(test::readerFonts());
        openFiles(model, pagesBook());
        if (measure) {
            while (model.refinePageEstimate()) { }
        }
        int turned = 1, units = 0;
        auto rough = model.roughPages();
        QSet<int> items, slices;
        for (;;) {
            require(measure ? rough.page == turned : rough.page == 0 || rough.page == turned,
                    qPrintable(QString("Page %1 shown on turned page %2")
                                       .arg(rough.page)
                                       .arg(turned)));
            require(rough.pages == 0 || rough.pages >= rough.page, "The total is before the page");
            items.insert(model.spineItemOf(model.position()));
            if (Probe::sliceFirst(model) > 0)
                slices.insert(Probe::sliceFirst(model));
            units += Probe::windowLast(model) > Probe::windowFirst(model);
            model.prepareAhead();
            if (!model.turnInWindow(1))
                break;
            ++turned;
            rough = model.roughPages();
        }
        std::printf("forward pages (%s): %d pages, last %d of %d, %d slice starts, %d pages in "
                    "units, %lld spine items\n",
                    measure ? "measured" : "not measured", turned, rough.page, rough.pages,
                    int(slices.size()), units, qlonglong(items.size()));
        require(slices.size() > 2 && units > 0 && items.size() == 6,
                "The book must have slices, a unit and pages in every spine item");
        require(rough.page == turned && rough.pages == turned && !rough.pagesEstimated,
                "The last page is the real total, which is not estimated");
    }
}

// Idle estimate work also measures chapter positions.
void chaptersMeasuredInIdle()
{
    EngineDocument model(test::readerFonts());
    openResources(model, pagesBook(), { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
    model.setChapterSlices(6);
    QString error;
    require(model.showSpineItems(3, 3, &error) && model.go(0), qPrintable(error));
    while (model.refinePageEstimate()) { }
    const auto estimated = model.roughPages();
    require(estimated.pages > 0 && estimated.pagesEstimated, "The estimate must give a total");
    // The idle steps also measured where the chapters start. The first page
    // after them asks for the book page of every chapter and must not read
    // one file for each.
    const int places = Probe::textPlaces(model);
    QStringList chapterStarts;
    for (const auto &chapter : model.chapters())
        chapterStarts.append(chapter.position);
    require(chapterStarts.size() > 2 && !model.bookPages(chapterStarts).isEmpty(),
            "The fixture needs chapters");
    require(places > 0 && Probe::textPlaces(model) == places,
            "The book pages of the chapters must be measured in idle time");
}

// Hybrid rendering builds the document of the whole book beside the chapter
// that is shown, and then shows it: the same page, the pages of the whole
// book, and no new load. A try that is stopped leaves nothing behind.
void fullDocument()
{
    EngineDocument model(test::readerFonts());
    openResources(model, pagesBook(), { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
    QStringList whole;
    for (int page = 0; page < model.pages(); ++page)
        whole.append(model.pageStart(page));
    model.setChapterSlices(6);
    QString error;
    require(model.showSpineItems(3, 3, &error) && model.go(0), qPrintable(error));
    for (int turn = 0; turn < 9; ++turn)
        require(model.turnInWindow(1), "The fixture is too short to read on");
    const auto start = model.pageStart(model.page());
    const int wholePage = whole.indexOf(start);
    require(wholePage > 0, "The page of chapter mode must be a page of the whole book");

    require(model.prepareFullDocument([] { return false; }) == Engine::FullDocument::Stopped
                    && !Probe::fullView(model),
            "A stopped try must leave no document");
    require(model.prepareFullDocument({}) == Engine::FullDocument::Ready,
            "The whole book must build beside the chapter");
    const auto *full = Probe::fullView(model);
    require(full && model.prepareFullDocument({}) == Engine::FullDocument::Ready
                    && Probe::fullView(model) == full,
            "A prepared document must stay until it is shown");
    // Simulate a prepared document without the chapter of the shown place.
    // Resolving that place must fail without replacing the usable chapter.
    const auto beforeFailedSwitch = model.position();
    const auto beforeFailedImage = model.image(model.page());
    const auto *chapter = Probe::view(model);
    Probe::omitPreparedChapter(model);
    require(!model.setChapterMode(false, appearance::Patch(QJsonObject{}), {}, {}, {}),
            "A prepared document without the reading place must not be shown");
    require(Probe::view(model) == chapter && model.position() == beforeFailedSwitch
                    && model.image(model.page()) == beforeFailedImage,
            "A failed switch changed the reading page");
    // The same build would fail the same way: it is not offered or built
    // again for this appearance.
    require(!model.fullDocumentReady()
                    && model.prepareFullDocument({}) == Engine::FullDocument::None,
            "A refused whole-book document must not be offered again");
    // The rest of the test goes on with a new build for the same pages.
    Probe::forgetRefusedFullDocument(model);
    require(model.prepareFullDocument({}) == Engine::FullDocument::Ready,
            "The whole book must build again once the refusal is forgotten");
    full = Probe::fullView(model);
    // The reader went on in the chapter meanwhile.
    require(model.turnInWindow(1), "The fixture is too short to read on");
    const auto shown = model.pageStart(model.page());
    require(model.bookFraction() >= 0, "The chapter is still shown");
    require(model.setChapterMode(false, appearance::Patch(QJsonObject{}), {}, {}, {}),
            "The prepared document must be shown");
    std::printf("full document: page %d of %d after the switch, whole book page %d of %d\n",
                model.page() + 1, model.pages(), wholePage + 2, int(whole.size()));
    require(Probe::view(model) == full, "The switch must show the prepared document, not load one");
    require(model.bookFraction() < 0 && model.pages() == whole.size()
                    && model.page() == wholePage + 1 && model.pageStart(model.page()) == shown,
            "After the switch the reader is on the same page of the whole book");

    // Another font size drops a prepared document.
    EngineDocument other(test::readerFonts());
    openFiles(other, pagesBook());
    require(other.prepareFullDocument({}) == Engine::FullDocument::Ready, "The whole book builds");
    require(other.applyAppearance(appearance::Patch(qt::object({ { "font_pixels", 32 } }))),
            "Cannot change the font size");
    require(other.prepareFullDocument([] { return false; }) == Engine::FullDocument::Stopped
                    && !Probe::fullView(other),
            "A document for another font size must not stay");

    // A build that is parked is given up without a wait when the font size
    // changes, and when its book closes.
    int seen = 0;
    require(other.prepareFullDocument([&] { return ++seen <= 5; })
                    == Engine::FullDocument::Stopped,
            "The build must park");
    require(other.applyAppearance(appearance::Patch(qt::object({ { "font_pixels", 28 } }))),
            "Cannot change the font size with a parked build");
    seen = 0;
    require(other.prepareFullDocument([&] { return ++seen <= 5; })
                    == Engine::FullDocument::Stopped,
            "A new build must start for the new font size");
    require(other.prepareFullDocument({}) == Engine::FullDocument::Ready,
            "The new build must finish");
    EngineDocument closing(test::readerFonts());
    openFiles(closing, pagesBook());
    seen = 0;
    require(closing.prepareFullDocument([&] { return ++seen <= 5; })
                    == Engine::FullDocument::Stopped,
            "The build must park before its book closes");
}

// The build of the whole book parks at a stop check when other work needs
// the engine, and goes on from there. Parked after every few checks, with
// chapter work on the engine in between, it must still give the document of
// a load that was never stopped: the same pages, with the same pixels.
void parkedFullDocument(const std::function<void(EngineDocument &)> &openWhole, int item,
                        const char *name, int checks = 7,
                        const std::function<void(EngineDocument &)> &openChapter = {},
                        const std::function<void()> &between = {})
{
    const auto record = [](EngineDocument &model, int page) {
        const QImage image = model.image(page);
        return model.pageStart(page) + ' '
                + QString::fromLatin1(
                        QCryptographicHash::hash(
                                QByteArray::fromRawData(
                                        reinterpret_cast<const char *>(image.constBits()),
                                        int(image.sizeInBytes())),
                                QCryptographicHash::Md5)
                                .toHex());
    };
    QStringList reference;
    {
        EngineDocument whole(test::readerFonts());
        openWhole(whole);
        for (int page = 0; page < whole.pages(); ++page)
            reference.append(record(whole, page));
    }
    EngineDocument model(test::readerFonts());
    QString error;
    if (openChapter) {
        // A book that opens as a chapter, with its own setup.
        openChapter(model);
    } else {
        openWhole(model);
        model.setChapterSlices(6);
        require(model.showSpineItems(item, item, &error) && model.go(0), qPrintable(error));
    }
    int parks = 0;
    for (;;) {
        // A different number of checks each time, so that the build parks
        // in every kind of place.
        const int budget = 1 + parks % checks;
        int seen = 0;
        const auto result = model.prepareFullDocument([&] { return ++seen <= budget; });
        if (result == Engine::FullDocument::Ready)
            break;
        require(result == Engine::FullDocument::Stopped && parks < 2000000,
                "The build must park and go on until it is ready");
        ++parks;
        // Other work has the engine now: page turns, a jump to another
        // chapter and a drawn page.
        if (parks % 5 == 0)
            require(model.showSpineItems(item + parks % 3, item + parks % 3, &error),
                    qPrintable(error));
        else if (!model.turnInWindow(parks % 2 ? 1 : -1))
            model.turnInWindow(parks % 2 ? -1 : 1);
        if (parks % 3 == 0)
            model.image(model.page());
        if (between)
            between();
    }
    require(model.setChapterMode(false, appearance::Patch(QJsonObject{}), {}, {}, {}),
            "The prepared document must be shown");
    require(model.pages() == reference.size(), "A parked build changed the page count");
    int different = 0;
    for (int page = 0; page < model.pages(); ++page)
        different += record(model, page) != reference[page];
    std::printf("parked build (%s): %d parks, %d pages, %d differ\n", name, parks,
                int(reference.size()), different);
    require(parks > 50, "The fixture must park the build many times");
    require(different == 0, "A parked build changed a page");
}

// The switch to the whole book resolves the place of the page on screen in
// the whole-book document. Walks a book in chapter mode and asks the whole
// book for the place of every page.
void handoffWalk(const QByteArray &text)
{
    const QJsonObject settings{ { "font_pixels", 28 }, { "line_spacing_version", 1 } };
    const auto pixels = [](const QImage &image) {
        return QCryptographicHash::hash(
                QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()),
                                        int(image.sizeInBytes())),
                QCryptographicHash::Md5);
    };
    QString error;
    EngineDocument whole(test::readerFonts());
    require(whole.open(text, QSize(600, 800), settings, &error), qPrintable(error));
    EngineDocument model(test::readerFonts());
    setChapterModeOverride(1);
    const bool opened = model.open(text, QSize(600, 800), settings, &error);
    setChapterModeOverride(-1);
    require(opened && model.bookFraction() >= 0, qPrintable(error));
    int pages = 0, unresolved = 0, otherStart = 0, otherPixels = 0, compared = 0;
    for (;;) {
        const auto place = model.position();
        ++pages;
        if (place.isEmpty() || !whole.goTo(place)) {
            if (++unresolved <= 5)
                std::printf("unresolved place on chapter page %d: %s\n", pages, qPrintable(place));
        }
        // A blank page can share its source anchor with the following page.
        // Resolve the anchor above to test adoption, but compare pixels at
        // the same sequential page index so that such pages are not skipped.
        if (pages <= whole.pages() && whole.go(pages - 1)) {
            if (whole.pageStart(whole.page()) != model.pageStart(model.page()) && ++otherStart <= 5)
                std::printf("other page start at chapter page %d: %s / %s\n", pages,
                            qPrintable(model.pageStart(model.page())),
                            qPrintable(whole.pageStart(whole.page())));
            if (qEnvironmentVariableIsSet("KOBALT_HANDOFF_ALL_PAGES") || pages % 5 == 0) {
                ++compared;
                if (pixels(whole.image(whole.page())) != pixels(model.image(model.page()))
                    && ++otherPixels <= 5) {
                    std::printf("other pixels at chapter page %d: %s\n", pages, qPrintable(place));
                    const auto folder = qEnvironmentVariable("KOBALT_HANDOFF_CAPTURE_DIR");
                    if (!folder.isEmpty()) {
                        require(QDir().mkpath(folder), "Cannot create handoff capture directory");
                        require(whole.image(whole.page())
                                                .save(folder + QString("/whole-%1.png").arg(pages))
                                        && model.image(model.page())
                                                   .save(folder
                                                         + QString("/chapter-%1.png").arg(pages)),
                                "Cannot save handoff mismatch images");
                    }
                }
            }
        }
        require(pages <= whole.pages() * 2 + 50, "The chapter walk did not reach the end");
        if (!model.turnInWindow(1))
            break;
    }
    std::printf("handoff walk: %d chapter pages, %d whole-book pages, %d unresolved, "
                "%d other page starts, %d of %d compared pages with other pixels\n",
                pages, whole.pages(), unresolved, otherStart, otherPixels, compared);
    require(unresolved == 0, "A chapter page has no place in the whole book");
    require(pages == whole.pages() && otherStart == 0 && otherPixels == 0,
            "Chapter pages differ from the whole book");
}

// A parked build must not reuse another book's font bytes, even when the
// publisher gave both fonts the same file name and the same byte length.
void parkedPublisherFonts()
{
    auto first = test::readerAssets().fontData("InstrumentSerif-Regular.ttf");
    auto second = test::readerAssets().fontData("IBMPlexSans-Regular.ttf");
    require(!first.isEmpty() && !second.isEmpty(), "Missing publisher font fixtures");
    const int bytes = qMax(first.size(), second.size());
    first.append(QByteArray(bytes - first.size(), '\0'));
    second.append(QByteArray(bytes - second.size(), '\0'));
    const auto book = publisherFontBook(first), other = publisherFontBook(second);
    const QJsonObject settings{ { "font_face", "" }, { "font_pixels", 28 } };
    parkedFullDocument([&](EngineDocument &model) { openResources(model, book, settings); }, 1,
                       "publisher fonts with another book", 13, {},
                       [&] {
                           EngineDocument intruder(test::readerFonts());
                           setChapterModeOverride(1);
                           openResources(intruder, other, settings);
                           setChapterModeOverride(-1);
                           intruder.image(0);
                       });
}

// A book that is in the parsed-book cache gives the whole book without
// parsing: the try right after a book opens takes it. A book that is not
// there waits until the reader asks for the whole build.
void fullDocumentFromCache(const QByteArray &bytes)
{
    const Page page;
    QTemporaryDir cacheRoot;
    require(cacheRoot.isValid(), "Cannot create a document cache directory");
    const auto directory = cacheRoot.path() + "/.cache";
    const QByteArray fingerprint(64, 'b'), other(64, 'c'), policy("hybrid-test");
    QString error;
    {
        EngineDocument whole(test::readerFonts());
        whole.setDocumentCache(directory, fingerprint, policy, 1024 * 1024);
        require(whole.open(bytes, page.viewport, page.settings(), &error), qPrintable(error));
        require(!whole.openedFromCache() && whole.saveDocumentCache(),
                "Cannot save the parsed book");
    }
    const auto chapter = [&](EngineDocument &model, const QByteArray &key) {
        model.setDocumentCache(directory, key, policy, 1024 * 1024);
        require(model.open(bytes, page.viewport, page.settings(), &error), qPrintable(error));
        model.setChapterSlices(6);
        require(model.showSpineItems(1, 1, &error) && model.go(0), qPrintable(error));
    };
    EngineDocument cached(test::readerFonts());
    chapter(cached, fingerprint);
    require(cached.prepareFullDocument({}, true) == Engine::FullDocument::Ready,
            "A book in the cache must give the whole book without parsing");
    // Opened as a chapter from the start, so that nothing is in the cache.
    EngineDocument fresh(test::readerFonts());
    setChapterModeOverride(1);
    fresh.setDocumentCache(directory, other, policy, 1024 * 1024);
    const bool chapters = fresh.open(bytes, page.viewport, page.settings(), &error);
    setChapterModeOverride(-1);
    require(chapters && fresh.bookFraction() >= 0, qPrintable(error));
    require(fresh.prepareFullDocument({}, true) == Engine::FullDocument::None
                    && !Probe::fullView(fresh),
            "A book that is not in the cache must not be parsed before the user reads");
    require(fresh.prepareFullDocument({}, true) == Engine::FullDocument::None,
            "The cache is not asked again for the same page");
    require(fresh.prepareFullDocument({}) == Engine::FullDocument::Ready,
            "The whole build must still work");
}

// Abandonment can happen after a cached document opened, at its word count.
// Its destructor must leave that unfinished cache dirty, not save the reflow.
void abandonedOpenedDocument(const QByteArray &bytes)
{
    QTemporaryDir root;
    require(root.isValid(), "Cannot create the abandoned-build fixture");
    const auto directory = root.path() + "/.cache";
    const QByteArray fingerprint(64, 'f'), policy("abandoned-open-test");
    const QJsonObject settings{ { "font_pixels", 28 }, { "line_spacing_version", 1 } };
    QString error;
    {
        EngineDocument writer(test::readerFonts());
        writer.setDocumentCache(directory, fingerprint, policy, 1024 * 1024);
        require(writer.open(bytes, { 600, 800 }, settings, &error) && writer.saveDocumentCache(),
                "Cannot seed the abandoned-build cache");
    }
    EngineDocument model(test::readerFonts());
    model.setDocumentCache(directory, fingerprint, policy, 1024 * 1024);
    auto changed = settings;
    changed["font_pixels"] = 32;
    setChapterModeOverride(1);
    const bool opened = model.open(bytes, { 600, 800 }, changed, &error);
    setChapterModeOverride(-1);
    require(opened, qPrintable(error));
    initializeDiagnostics(root.path() + "/logs");
    pauseDiagnostics(false);
    auto ended = std::make_shared<std::atomic_bool>(false);
    setDiagnosticObserver([ended](const QJsonObject &event) {
        if (event.value("event") == "operation-end" && event.value("operation") == "engine-open")
            ended->store(true);
    });
    const auto reset = qScopeGuard([] {
        setDiagnosticObserver({});
        pauseDiagnostics(true);
    });
    require(model.prepareFullDocument([ended] { return !ended->load(); })
                            == Engine::FullDocument::Stopped
                    && ended->load(),
            "The cached build must park after opening and before counting words");
    Probe::abandonFullBuild(model);
    setDiagnosticObserver({});
    pauseDiagnostics(true);
    EngineDocument restored(test::readerFonts());
    restored.setDocumentCache(directory, fingerprint, policy, 1024 * 1024);
    require(restored.open(bytes, { 600, 800 }, settings, &error), qPrintable(error));
    require(!restored.openedFromCache(), "An abandoned opened build saved a reusable cache");
}

// Dark mode, the outline weight and the text colors move no text to another
// page, so they are not in the key of the page estimate. They change every
// pixel: a document of the whole book made before such a change must not be
// shown after it.
void fullDocumentPixels()
{
    for (const auto &change :
         { QJsonObject{ { "dark_mode", true } }, QJsonObject{ { "weight_steps", 3 } },
           QJsonObject{ { "normalize_text_colors", true } } }) {
        EngineDocument model(test::readerFonts());
        openResources(model, pagesBook(), { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
        model.setChapterSlices(6);
        QString error;
        require(model.showSpineItems(3, 3, &error) && model.go(0), qPrintable(error));
        require(model.prepareFullDocument({}) == Engine::FullDocument::Ready
                        && model.fullDocumentReady() && model.fullDocumentStarted(),
                "The whole book must build beside the chapter");
        require(model.applyAppearance(appearance::Patch(change), {}),
                "The chapter must take the change");
        require(!model.fullDocumentReady() && !model.fullDocumentStarted(),
                "A document for other pixels is neither ready nor started");
        require(model.prepareFullDocument({}) == Engine::FullDocument::Ready
                        && model.fullDocumentReady(),
                "The whole book must build again for the change");
        require(model.setChapterMode(false, appearance::Patch(change), {}, {}, {})
                        && model.bookFraction() < 0,
                "The prepared document must be shown");
        for (auto it = change.begin(); it != change.end(); ++it)
            require(model.state().value(it.key()) == it.value(),
                    "The whole book must have the changed setting");
    }
}

// Repeated chapter jumps must not exhaust the engine document slots.
void manyItemJumps()
{
    const QByteArray sentence =
            "The reader turns the page and the text goes on without a break in it. ";
    test::BookFiles files;
    QByteArray spine;
    for (int item = 0; item < 60; ++item) {
        QByteArray body = "<h1>Chapter</h1>";
        // Long enough that no spine item shares a unit with the next.
        for (int p = 0; p < 40 + item % 7; ++p)
            body += "<p>" + sentence.repeated(3) + "</p>";
        const auto name = QString("c%1.xhtml").arg(item, 2, 10, QChar('0'));
        files[name] = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Part</title>"
                      "</head><body>"
                + body + "</body></html>";
        spine += "<itemref idref=\"" + name.toUtf8() + "\"/>";
    }
    files = test::publisherPackage("en", "Many", files, spine);
    EngineDocument model(test::readerFonts());
    openResources(model, files, { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
    model.setChapterSlices(6);
    QString error;
    require(model.showSpineItems(30, 30, &error) && model.go(0), qPrintable(error));
    while (model.refinePageEstimate()) { }
    for (int jump = 0; jump < 120; ++jump) {
        const int item = (jump * 17) % 60;
        require(model.showSpineItems(item, item, &error) && model.go(0),
                qPrintable("A chapter jump failed: " + error));
        require(!model.image(model.page()).isNull(), "The chapter must draw after a jump");
    }
    std::printf("many spine items: repeated jumps passed without exhausting document slots\n");
}

// Reading every part of a book saves exact observations. Reopening at a
// middle position keeps those page numbers while reading on.
void reopenCounted()
{
    const auto files = pagesBook();
    EngineDocument first(test::readerFonts());
    openFiles(first, files);
    QString error;
    require(first.showSpineItems(3, 3, &error) && first.go(0), qPrintable(error));
    // Into a later slice of the long chapter.
    for (int turn = 0; turn < 10; ++turn)
        require(first.turnInWindow(1), "The fixture is too short to read on");
    require(Probe::sliceFirst(first) > 0, "The fixture needs a later slice");
    while (first.refinePageEstimate()) { }
    const auto middle = first.pageStart(first.page());
    require(first.showSpineItems(0, 0, &error) && first.go(0), qPrintable(error));
    int turns = 0;
    while (first.turnInWindow(1) && ++turns < 10000) { }
    require(turns < 10000 && first.goTo(middle), "Reading must finish and return to the middle");
    const auto shown = first.roughPages();
    require(!shown.pagesEstimated, "The book must be counted");
    auto saved = first.state();
    PageEstimateRecord record;
    require(PageEstimateRecord::fromJson(saved.value("page_estimate"), &record)
                    && !record.parts.isEmpty() && record.kept.isEmpty(),
            "A counted book saves the parts of its long spine items and no estimate");
    saved["koblaze_first_spine_item"] = 3;
    saved["koblaze_last_spine_item"] = 3;

    EngineDocument model(test::readerFonts());
    model.setChapterSlices(6);
    openResources(model, files, saved);
    auto rough = model.roughPages();
    std::printf("reopened counted: page %d of %d, saved page %d of %d\n", rough.page, rough.pages,
                shown.page, shown.pages);
    require(!rough.pagesEstimated && rough.page == shown.page && rough.pages == shown.pages,
            "A counted book must open with its real page numbers");
    for (int turn = 0; turn < 12; ++turn) {
        require(model.turnInWindow(1), "The fixture is too short to read on");
        const auto next = model.roughPages();
        require(!next.pagesEstimated && next.page == rough.page + 1 && next.pages == shown.pages,
                "Reading on in a counted book must keep its real page numbers");
        rough = next;
    }
    // A damaged part drops only the parts of its spine item.
    auto damaged = saved.value("page_estimate").toObject();
    auto runs = damaged.value("parts").toArray();
    auto run = runs[0].toArray();
    run[2] = -5;
    runs[0] = run;
    damaged["parts"] = runs;
    PageEstimateRecord partial;
    require(PageEstimateRecord::fromJson(damaged, &partial)
                    && partial.parts.size() < record.parts.size() && partial.read == record.read,
            "A damaged part must not drop the rest of the record");
}

// The first complete estimate stays while the reader reads. A spine item
// that gets its real pages later moves neither the page number nor the
// total. Only a book with the real pages of every spine item replaces it.
void laterCounted()
{
    EngineDocument model(test::readerFonts());
    openFiles(model, pagesBook());
    while (model.refinePageEstimate()) { }
    QString error;
    // Into the middle chapter without reading the chapters before it.
    require(model.showSpineItems(3, 3, &error) && model.go(0), qPrintable(error));
    const auto first = model.roughPages();
    auto &map = Probe::editMap(model);
    require(first.pages > 0 && first.pagesEstimated, "The estimate must give a total");
    const int estimate = qRound(map.itemPages(1));
    require(estimate > 12, "The long chapter is too short");

    // More real pages for an earlier chapter, then fewer.
    require(model.turnInWindow(1), "The fixture is too short to read on");
    auto rough = model.roughPages();
    require(rough.page == first.page + 1 && rough.pages == first.pages,
            "A forward turn adds one page and keeps the total");
    map.observe(1, 0, 1, 1, estimate + 8);
    require(model.roughPages().page == rough.page && model.roughPages().pages == first.pages,
            "A larger count of an earlier chapter moved the numbers");
    map.observe(1, 0, 1, 1, estimate - 8);
    require(model.roughPages().page == rough.page && model.roughPages().pages == first.pages,
            "A smaller count of an earlier chapter moved the numbers");

    // To the end of the book: the total stays, and the page never goes
    // back nor passes it.
    int turns = 0;
    while (model.turnInWindow(1)) {
        const auto next = model.roughPages();
        require(next.pages == first.pages, "The total moved while reading");
        require(next.page >= rough.page && next.page <= next.pages,
                "The page went back or passed the total");
        rough = next;
        ++turns;
    }
    require(turns > 20 && rough.pagesEstimated, "The fixture must end before the book is counted");
    require(model.showSpineItems(3, 3, &error) && model.go(0), qPrintable(error));
    const auto jumped = model.roughPages();
    std::printf("kept estimate: page %d of %d at the chapter, %d of %d at the end\n", jumped.page,
                jumped.pages, rough.page, rough.pages);
    require(jumped.page == first.page && jumped.pages == first.pages,
            "The same place must show the same numbers again");
}

// Until every spine item is measured or counted, there is no page number
// and no total: the reader shows percentages. The percent comes from the
// bytes of the whole book. The values of the spine item stay.
void hiddenUntilComplete()
{
    EngineDocument model(test::readerFonts());
    openFiles(model, pagesBook());
    QString error;
    require(model.showSpineItems(3, 3, &error) && model.go(0), qPrintable(error));
    const auto rough = model.roughPages();
    require(!Probe::map(model).complete(), "The map must not be complete yet");
    require(rough.page == 0 && rough.pages == 0, "A page number before the map is complete");
    require(rough.itemPage == 1 && rough.itemPages > 0, "The spine item keeps its pages");
    const auto bytes = Probe::spineBytes(model);
    qint64 before = 0, total = 0;
    for (int i = 0; i < bytes.size(); ++i) {
        total += bytes[i];
        before += i < 3 ? bytes[i] : 0;
    }
    require(std::fabs(model.bookFraction() - double(before) / total) < 1e-9,
            "The percent at a spine item start is the bytes before it");
    int steps = 0;
    while (model.refinePageEstimate()) {
        ++steps;
        const auto now = model.roughPages();
        require(now.pages == 0 || Probe::map(model).complete(),
                "A total was shown before the map was complete");
    }
    const auto measured = model.roughPages();
    require(steps > 0 && measured.page > 0 && measured.pages >= measured.page,
            "The page number comes once every spine item is measured");
}

// The page map of a spine item read in slices that are longer than the
// nominal slice size: before a slice start that the observed slices reach,
// the pages are their real pages, without the empty ends of slices that the
// observed ones do not have.
void slicedItem(const PageEstimateLayout &layout)
{
    QByteArray body;
    const QByteArray sentence =
            "The reader turns the page and the text goes on without a break in it. ";
    for (int i = 0; i < 400; ++i)
        body += "<p>" + sentence.repeated(3) + "</p>";
    const auto item = measurePageEstimate("<html><body>" + body + "</body></html>");
    const int characters = item.characters, slice = characters / 12;
    PageMap map;
    map.reset({ 1000, 200000 });
    map.setLayout(layout, slice);
    map.setMeasured(0, measurePageEstimate("<body><p>Cover</p></body>"));
    map.setMeasured(1, item);
    map.observe(0, 0, 1, 1, 1);
    // Slices a third longer than the nominal size, as cutSlice() makes them.
    int first = 0, pages = 0;
    for (int part = 0; part < 6; ++part) {
        const int last = first + slice * 4 / 3;
        const int partPages = 7 + part % 2;
        map.observe(1, first, last, characters, partPages);
        pages += partPages;
        first = last;
        require(std::fabs(map.pagesBefore(1, first, characters) - pages) < 1e-9,
                "Before a slice start the observed slices count with their real pages");
        require(std::fabs(map.pagesBefore(1) - 1) < 1e-9, "The cover counts with its page");
    }
}

void chapterMode(const QByteArray &bytes)
{
    const Page page;
    for (const bool measure : { true, false }) {
        int slicePages = 0;
        const auto pages = readForward(bytes, page, measure, &slicePages);
        require(slicePages <= 12, "A slice has too many pages");
        const int turned = pages.size();
        for (int i = 0; i < turned; ++i) {
            // While the total is an estimate, it stays as it is and the page
            // never goes back. A counted book shows the real page. Without
            // measuring there is no number until the last spine item is
            // counted.
            if (pages[i].page > 0 && !pages[i].pagesEstimated)
                require(pages[i].page == i + 1, "A counted book shows the page that was turned");
            else if (pages[i].page > 0)
                require(pages[i].pages == pages.first().pages
                                && (i == 0 || pages[i].page >= pages[i - 1].page),
                        "The estimate moved while reading");
            else
                require(!measure && pages[i].pages == 0, "A page number has no total");
            require(pages[i].page <= pages[i].pages, "The page is never after the total");
            require(pages[i].itemPage >= 1 && pages[i].itemPage <= pages[i].itemPages,
                    "The page in the spine item is inside it");
        }
        if (measure)
            require(pages.first().page == 1, "Measured first, the first page has its number");
        std::printf("chapter mode (%s): %d pages turned, last page %d of %d, first total %d\n",
                    measure ? "measured" : "not measured", turned, pages.last().page,
                    pages.last().pages, pages.first().pages);
        require(pages.last().page == turned && pages.last().pages == turned
                        && !pages.last().pagesEstimated,
                "At the end the page and the total are the pages turned, and not estimated");
        if (measure)
            require(std::abs(pages.first().pages - turned) <= turned / 5,
                    "The first total is too far from the pages turned");
        const auto again = readForward(bytes, page, measure, &slicePages);
        require(again.size() == pages.size(), "Reading again turns the same pages");
        for (int i = 0; i < pages.size(); ++i)
            require(again[i].page == pages[i].page && again[i].pages == pages[i].pages,
                    "Reading again gives the same rough pages");
    }
}

// Sets how the books that follow open, and gives the choice back to the
// environment at the end of the scope.
class ChapterModeScope
{
public:
    explicit ChapterModeScope(bool chapters) { setChapterModeOverride(chapters ? 1 : 0); }

    ~ChapterModeScope() { setChapterModeOverride(-1); }

    ChapterModeScope(const ChapterModeScope &) = delete;
    ChapterModeScope &operator=(const ChapterModeScope &) = delete;
};

// The chapter mode survey of one book: what a reader sees in chapter mode,
// compared with a layout of the whole book on a six-inch page. It is the
// acceptance check for the rough page numbers; doc/engines/migration.md
// describes how to run it on a folder of books.
//
// The book is read forward page by page from the start, as the reader does,
// with the work ahead after each turn. In the mode `measured` the background
// measuring finishes right after the open, in `unmeasured` it never runs.
// Then the book opens at the page that the whole-book layout has at 5, 20
// and 50 percent (`jump-measured` and `jump-unmeasured`).
//
// Each checkpoint prints one CSV row that starts with "C," and has these
// fields: book, spine items, real total, mode, checkpoint in percent, pages
// turned, spine item, shown page, real page, page error in pages and in
// percent of the book, shown total, total error in percent, shown and real
// page in the spine item, shown and real pages of the spine item, whether
// the total and the spine item pages are estimated, forward turns so far
// that kept or lowered the shown page, page reports so far before the map
// was complete, the shown book fraction in percent and its error in percent
// of the book, whether the map is complete, measuring steps, ms to measure,
// ms to open in chapter mode, ms to open as a whole, ms of reading, counted
// spine items, the counted spine items whose pages differ from the whole
// layout, and the first spine items that are not counted, as item:pages.
// Page values are 0 where none is shown. A row that starts with "D," names
// a forward turn that did not add to the shown page, and one that starts
// with "F," a book that cannot be opened.
void chapterSurvey(const QString &path)
{
    Page page;
    page.font = 32;
    page.margin = 48;
    page.viewport = { 1072, 1448 };
    const QString name = QFileInfo(path).completeBaseName().replace(',', ' ');
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::printf("F,%s,read\n", qPrintable(name));
        return;
    }
    const auto bytes = file.readAll();
    QString error;
    QElapsedTimer timer;

    // The truth: the total and the first page of each spine item.
    EngineDocument whole(test::readerFonts());
    qint64 wholeMs = 0;
    {
        const ChapterModeScope scope(false);
        timer.start();
        const bool opened = whole.open(bytes, page.viewport, page.settings(), &error);
        wholeMs = timer.elapsed();
        if (!opened || whole.pages() <= 0) {
            std::printf("F,%s,whole open: %s\n", qPrintable(name),
                        qPrintable(error.simplified().replace(',', ' ')));
            return;
        }
    }
    const int total = whole.pages();
    const auto items = Probe::items(whole);
    QStringList startPositions;
    for (const auto &href : items) {
        Location location;
        location.href = href;
        startPositions.append(whole.positionOf(location));
    }
    const auto startPages = whole.pagesOf(startPositions);
    // A spine item without a page starts where the next one starts.
    QVector<int> starts;
    for (int i = startPages.size() - 1; i >= 0; --i)
        starts.prepend(startPages[i] >= 0 ? startPages[i] : (starts.isEmpty() ? total : starts[0]));
    const auto realItemPages = [&](int item) {
        if (item < 0 || item >= starts.size())
            return -1;
        return (item + 1 < starts.size() ? starts[item + 1] : total) - starts[item];
    };

    struct Counts
    {
        int backward = 0, early = 0, steps = 0;
        qint64 measureMs = -1, openMs = 0, readMs = 0;
    };

    const auto row = [&](EngineDocument &model, const Engine::RoughPages &rough, const char *mode,
                         const char *checkpoint, int turned, const Counts &counts) {
        const auto position = model.position();
        const int realPage = whole.pagesOf({ position }).value(0, -1) + 1;
        const int item = model.spineItemOf(position);
        const int realItemPage =
                realPage > 0 && item >= 0 && item < starts.size() ? realPage - starts[item] : 0;
        const auto &map = Probe::map(model);
        const auto counted = map.completeItems();
        int differ = 0;
        for (auto it = counted.cbegin(); it != counted.cend(); ++it)
            differ += it.value() != realItemPages(it.key());
        QStringList missing;
        for (int i = 0; i < items.size() && missing.size() < 8; ++i) {
            if (!counted.contains(i))
                missing.append(QString("%1:%2").arg(i).arg(realItemPages(i)));
        }
        const bool shown = rough.page > 0 && realPage > 0;
        const double fraction = 100 * model.bookFraction();
        std::printf("C,%s,%d,%d,%s,%s,%d,%d,%d,%d,%d,%.3f,%d,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,"
                    "%.3f,%d,%d,%lld,%lld,%lld,%lld,%d,%d,%s\n",
                    qPrintable(name), int(items.size()), total, mode, checkpoint, turned, item,
                    rough.page, realPage, shown ? rough.page - realPage : 0,
                    shown ? 100.0 * (rough.page - realPage) / total : 0.0, rough.pages,
                    rough.pages > 0 ? 100.0 * (rough.pages - total) / total : 0.0, rough.itemPage,
                    realItemPage, rough.itemPages, realItemPages(item), int(rough.pagesEstimated),
                    int(rough.itemPagesEstimated), counts.backward, counts.early, fraction,
                    realPage > 0 ? fraction - 100.0 * realPage / total : 0.0, int(map.complete()),
                    counts.steps, (long long)counts.measureMs, (long long)counts.openMs,
                    (long long)wholeMs, (long long)counts.readMs, int(counted.size()), differ,
                    qPrintable(missing.join(';')));
        std::fflush(stdout);
    };
    const auto open = [&](EngineDocument &model, const QJsonObject &settings, Counts *counts) {
        const ChapterModeScope scope(true);
        timer.restart();
        const bool opened = model.open(bytes, page.viewport, settings, &error);
        counts->openMs = timer.elapsed();
        if (!opened || model.pages() <= 0) {
            std::printf("F,%s,chapter open: %s\n", qPrintable(name),
                        qPrintable(error.simplified().replace(',', ' ')));
            return false;
        }
        return true;
    };
    const auto measure = [&](EngineDocument &model, Counts *counts) {
        timer.restart();
        while (model.refinePageEstimate())
            ++counts->steps;
        counts->measureMs = timer.elapsed();
    };

    for (const bool measured : { false, true }) {
        EngineDocument model(test::readerFonts());
        Counts counts;
        if (!open(model, page.settings(), &counts))
            return;
        if (measured)
            measure(model, &counts);
        const char *mode = measured ? "measured" : "unmeasured";

        const struct
        {
            double fraction;
            const char *name;
        } checkpoints[] = { { 0.05, "5" }, { 0.2, "20" }, { 0.5, "50" } };

        int next = 0, turned = 1;
        auto rough = model.roughPages();
        QElapsedTimer read;
        read.start();
        row(model, rough, mode, "0", turned, counts);
        for (;;) {
            model.prepareAhead();
            if (!model.turnInWindow(1))
                break;
            ++turned;
            const auto now = model.roughPages();
            if (now.pages > 0 && !Probe::map(model).complete())
                ++counts.early;
            if (now.page > 0 && rough.page > 0 && now.page <= rough.page) {
                ++counts.backward;
                std::printf("D,%s,%s,%d,%d,%d\n", qPrintable(name), mode, turned, rough.page,
                            now.page);
            }
            rough = now;
            if (next < 3 && turned >= checkpoints[next].fraction * total) {
                counts.readMs = read.elapsed();
                row(model, rough, mode, checkpoints[next].name, turned, counts);
                ++next;
            }
        }
        counts.readMs = read.elapsed();
        row(model, rough, mode, "100", turned, counts);
    }

    for (const double fraction : { 0.05, 0.2, 0.5 }) {
        if (!whole.go(qBound(0, int(fraction * total), total - 1)))
            continue;
        auto settings = page.settings();
        settings["position"] = whole.position();
        const auto checkpoint = QByteArray::number(int(fraction * 100));
        for (const bool measured : { false, true }) {
            EngineDocument model(test::readerFonts());
            Counts counts;
            if (!open(model, settings, &counts))
                return;
            if (measured)
                measure(model, &counts);
            const auto rough = model.roughPages();
            counts.early = rough.pages > 0 && !Probe::map(model).complete();
            row(model, rough, measured ? "jump-measured" : "jump-unmeasured",
                checkpoint.constData(), 1, counts);
        }
    }
}
#endif
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
#if __has_include(<koblaze.h>)
    if (argc > 2 && QByteArray(argv[1]) == "--chapter") {
        chapterSurvey(QString::fromLocal8Bit(argv[2]));
        return 0;
    }
    // KOBALT_HANDOFF_BOOK names an EPUB for the handoff walk alone.
    if (const auto other = qEnvironmentVariable("KOBALT_HANDOFF_BOOK"); !other.isEmpty()) {
        QFile book(other);
        require(book.open(QIODevice::ReadOnly), "Cannot read KOBALT_HANDOFF_BOOK");
        handoffWalk(book.readAll());
        return 0;
    }
#endif
    if (argc > 1) {
        QStringList options;
        for (int i = 2; i < argc; ++i)
            options.append(QString::fromLocal8Bit(argv[i]));
        survey(QString::fromLocal8Bit(argv[1]), options);
        return 0;
    }
    savedPlaceOutsideOpeningWindow();
    returnToEvictedBookmark();
    reopenAfterWholeBookPreparation();
    nestedSliceHonorsCancellation();
    blocks();
    QFile file(QStringLiteral(KOBALT_TEST_FIXTURES) + "/trevelyans-type-tester.kepub.epub");
    require(file.open(QIODevice::ReadOnly), "Cannot read the book fixture");
    const auto bytes = file.readAll();
    const Page page;
    const auto book = load("trevelyans-type-tester", bytes, page);
    require(book.items.size() > 1 && book.pages > 0, "The fixture must have several spine items");
    for (const auto &item : book.items)
        require(item.parsed, "Every spine item of the fixture must parse");
    map(book, page.layout());
    unmeasured(page.layout());
    record();
#if __has_include(<koblaze.h>)
    pageFormattingWarnings();
    slicedItem(page.layout());
    openObserved();
    interruptedChapterPlace();
    forwardPages();
    laterCounted();
    chaptersMeasuredInIdle();
    reopenCounted();
    manyItemJumps();
    fullDocument();
    fullDocumentKeepsPublisherFonts();
    parkedPublisherFonts();
    fullDocumentPixels();
    fullDocumentFromCache(bytes);
    abandonedOpenedDocument(bytes);
    // KOBALT_PARKED_BOOK names another EPUB for the same check, for a book
    // that is not a fixture.
    if (const auto other = qEnvironmentVariable("KOBALT_PARKED_BOOK"); !other.isEmpty()) {
        QFile book(other);
        require(book.open(QIODevice::ReadOnly), "Cannot read KOBALT_PARKED_BOOK");
        const auto text = book.readAll();
        parkedFullDocument(
                [&](EngineDocument &model) {
                    QString error;
                    require(model.open(text, QSize(600, 800),
                                       { { "font_pixels", 28 }, { "line_spacing_version", 1 } },
                                       &error),
                            qPrintable(error));
                },
                1, "KOBALT_PARKED_BOOK", 199);
        // The same with a cache folder: a large book then moves parts of
        // itself to its cache file during the build, and the build can
        // park between two of those parts.
        QTemporaryDir cacheRoot;
        require(cacheRoot.isValid(), "Cannot create a document cache directory");
        const auto directory = cacheRoot.path() + "/.cache";
        parkedFullDocument(
                [&](EngineDocument &model) {
                    QString error;
                    require(model.open(text, QSize(600, 800),
                                       { { "font_pixels", 28 }, { "line_spacing_version", 1 } },
                                       &error),
                            qPrintable(error));
                },
                1, "KOBALT_PARKED_BOOK with a cache folder", 199,
                [&](EngineDocument &model) {
                    QString error;
                    setChapterModeOverride(1);
                    model.setDocumentCache(directory, QByteArray(64, 'e'), "parked-test",
                                           text.size());
                    const bool opened = model.open(
                            text, QSize(600, 800),
                            { { "font_pixels", 28 }, { "line_spacing_version", 1 } }, &error);
                    setChapterModeOverride(-1);
                    require(opened && model.bookFraction() >= 0, qPrintable(error));
                });
        const auto files = QDir(directory).entryList({ "*.cr3" }, QDir::Files);
        std::printf("cache files after the build: %d\n", int(files.size()));
    }
    parkedFullDocument(
            [](EngineDocument &model) {
                openResources(model, pagesBook(),
                              { { "font_pixels", 28 }, { "line_spacing_version", 1 } });
            },
            3, "pages book");
    parkedFullDocument(
            [&](EngineDocument &model) {
                QString error;
                require(model.open(bytes, Page().viewport, Page().settings(), &error),
                        qPrintable(error));
            },
            1, "type tester");
    hiddenUntilComplete();
    chapterMode(bytes);
    reopen(bytes);
    observations();
    emptyItem();
    laterSlice();
    searchLongItem();
    keptObservations();
#endif
    std::printf("PASS: page estimate measurement, map, saved record and chapter mode pages\n");
    return 0;
}
