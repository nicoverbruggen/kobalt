#include "column_view.h"
#include "stop_point.h"
#include "engine_document.h"
#include "reader_locations.h"
#include "reader_render_callback.h"
#include "reader_source.h"
#include "publication/page_estimate.h"
#include <algorithm>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRegularExpression>
#include <climits>
#include <cmath>
#include <lvdocview.h>
#if __has_include(<koblaze.h>)
#  include <koblaze.h>
#endif

// Layout of one spine item in slices, with the koblaze fork. See
// EngineDocument::setChapterSlices().
namespace kobalt {
namespace {
#ifndef KOBLAZE_TEXT_LENGTHS
// The text nodes below `node` in document order, until `target`.
bool countText(ldomNode *node, ldomNode *target, int &count)
{
    for (int i = 0; i < node->getChildCount(); ++i) {
        auto *child = node->getChildNode(i);
        if (child == target)
            return true;
        if (child->isText())
            count += child->getText().length();
        else if (countText(child, target, count))
            return true;
    }
    return false;
}
#endif
} // namespace

int EngineDocument::sliceSize() const
{
    const double font = qMax(8, pixels_);
    const double width = qMax(100, size_.width() - 2 * margin_);
    const double height = qMax(100, size_.height() - topMargin_ - bottomMargin_);
    // Typical values: a character is half the font size wide, and a line is
    // a fifth higher than the line spacing says.
    const double page = (width / (font / 2)) * (height / (font * spacing_ / 100.0 * 1.2));
    return qMax(2000, int(slicePages_ * page));
}

const QVector<qint64> &EngineDocument::spineBytes() const
{
    auto &bytes = source_->spineBytes;
    if (bytes.size() != spineItemCount_ || bytes.isEmpty()) {
        bytes.clear();
        for (const auto &href : spineItems()) {
            const lString32 name = Utf8ToUnicode(href.toUtf8().constData());
            // The directory of the archive has the size. Opening the item
            // would read and clean all of its text.
            if (const auto *info = source_->container->GetObjectInfo(name)) {
                bytes.append(qint64(info->GetSize()));
                continue;
            }
            auto stream = source_->container->OpenStream(name.c_str(), LVOM_READ);
            bytes.append(stream.isNull() ? 0 : qint64(stream->GetSize()));
        }
    }
    return bytes;
}

qint64 EngineDocument::unitBudget() const
{
    // The text of a slice, with the markup around it.
    return qint64(sliceSize()) * 2;
}

QPair<int, int> EngineDocument::unitFrom(int item) const
{
    const auto &bytes = spineBytes();
    const qint64 budget = unitBudget();
    int last = item;
    qint64 sum = bytes.value(item);
    while (sum < budget && last + 1 < bytes.size() && last - item < 15
           && sum + bytes[last + 1] <= budget) {
        sum += bytes[++last];
    }
    return { item, last };
}

QPair<int, int> EngineDocument::unitUntil(int item) const
{
    const auto &bytes = spineBytes();
    const qint64 budget = unitBudget();
    int first = item;
    qint64 sum = bytes.value(item);
    while (sum < budget && first > 0 && item - first < 15 && sum + bytes[first - 1] <= budget)
        sum += bytes[--first];
    return { first, item };
}

PageEstimateLayout EngineDocument::estimateLayout() const
{
    PageEstimateLayout layout;
    const int columns = qMax(1, columns_);
    const double width = size_.width() - 2 * margin_;
    layout.fontSize = pixels_;
    layout.lineHeight = pixels_ * spacing_ / 100.0;
    layout.width = qMax(1.0, (width - (columns - 1) * columnSpacing_) / columns);
    layout.height = qMax(1, size_.height() - topMargin_ - bottomMargin_) * columns;
    layout.indent = paragraphIndent_ >= 0 ? pixels_ * paragraphIndent_ / 100.0 : pixels_;
    return layout;
}

PageMap &EngineDocument::pageMap() const
{
    auto &estimate = source_->pageEstimate;
    if (estimate.map.items() != spineItemCount_) {
        // The same sizes as the units, read once.
        estimate.map.keepFirstEstimate(true);
        estimate.map.reset(spineBytes());
        estimate.measurer.reset();
        estimate.stream = LVStreamRef();
        estimate.measuring = -1;
    }
    // A new page gives other page numbers. They start again from the
    // estimate. The size of a slice here is the nominal one: a slice that
    // was laid out observes its real end.
    if (estimate.map.setLayout(estimateLayout(), slicePages_ > 0 ? sliceSize() : 0)) {
        estimate.book.reset();
        estimate.item.reset();
        estimate.lastItem = -1;
    }
    return estimate.map;
}

void EngineDocument::forgetObservedPages()
{
    if (windowFirst_ < 0 || spineItemCount_ <= 0 || !source_)
        return;
    auto &estimate = source_->pageEstimate;
    if (estimate.map.items() != spineItemCount_)
        return;
    // Some settings change the pages but not the page of estimateLayout(),
    // such as the font. Others, such as the colors, do not change the pages,
    // and the observations stay. An empty page forgets the observations, and
    // the next pageMap() sets the real page again.
    const auto key = estimateKey(settingsState());
    if (key == estimate.key)
        return;
    estimate.key = key;
    estimate.map.setLayout(PageEstimateLayout(), 0);
}

void EngineDocument::observeLayout()
{
    // A document that is still opening has no spine item count yet. Its
    // pages are observed when the opening ends.
    if (windowFirst_ < 0 || spineItemCount_ <= 0 || !source_ || !engine_ || pages() <= 0)
        return;
    auto &map = pageMap();
    source_->pageEstimate.key = estimateKey(settingsState());
    if (slices()) {
        // The slice ends where cutSlice() ended it, which can be far after
        // its nominal size. The extra pages after the end belong to the
        // next slice. A spine item without text, such as a cover image, is
        // one slice that holds the whole item.
        const int text = spineItemText();
        if (sliceEnd_ <= sliceFirst_ && text > 0)
            return;
        map.observe(windowFirst_, sliceFirst_, sliceEnd_, text,
                    visiblePages_ > 0 ? visiblePages_ : pages());
        return;
    }
    // Each spine item of the window as a whole.
    const auto items = spineItems();
    QStringList starts;
    for (int i = windowFirst_; i <= windowLast_ && i < items.size(); ++i) {
        Location start;
        start.href = items[i];
        starts.append(positionOf(start));
    }
    auto first = pagesOf(starts);
    if (first.size() != starts.size())
        return;
    // A spine item without a page, such as an empty one, starts where the
    // next one starts. It is complete with no pages.
    for (int i = first.size() - 1; i >= 0; --i) {
        if (first[i] < 0 && !starts[i].isEmpty())
            first[i] = i + 1 < first.size() ? first[i + 1] : pages();
    }
    for (int i = 0; i < first.size(); ++i) {
        const int end = i + 1 < first.size() ? first[i + 1] : pages();
        if (first[i] >= 0 && end >= first[i])
            map.observe(windowFirst_ + i, 0, 1, 1, end - first[i]);
    }
}

QString EngineDocument::estimateKey(const QJsonObject &settings) const
{
    auto key = settings;
    // These do not change the pages. The outline thickness and the text
    // colors change pixels only.
    for (const auto *name : { "position", "language", "dark_mode", "imported_native_location",
                              "weight_steps", "normalize_text_colors" })
        key.remove(name);
    key["viewport"] = QString("%1x%2").arg(size_.width()).arg(size_.height());
    key["slice_pages"] = slicePages_;
    key["policy"] = QString::fromUtf8(documentCachePolicy());
    return QString::fromLatin1(
            QCryptographicHash::hash(QJsonDocument(key).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha1)
                    .toHex()
                    .left(16));
}

QString EngineDocument::fullDocumentKey(const QJsonObject &settings) const
{
    QJsonObject pixels;
    for (const auto *name : { "dark_mode", "weight_steps", "normalize_text_colors" })
        pixels[QLatin1String(name)] = settings.value(QLatin1String(name));
    return estimateKey(settings) + QLatin1Char('|')
            + QString::fromUtf8(QJsonDocument(pixels).toJson(QJsonDocument::Compact));
}

QJsonObject EngineDocument::pageEstimateRecord(const QJsonObject &settings) const
{
    if (windowFirst_ < 0 || spineItemCount_ <= 0 || !source_ || source_->parent || !engine_)
        return {};
    // Places the page first, so that the record holds the page on screen.
    const auto rough = roughPages();
    const auto &map = pageMap();
    if (map.total() <= 0)
        return {};
    const auto &estimate = source_->pageEstimate;
    PageEstimateRecord record;
    record.model = map.model();
    record.layout = estimateKey(settings);
    record.spineItems = spineItemCount_;
    record.item = estimate.lastItem;
    record.first = estimate.lastFirst;
    record.page = estimate.lastPage;
    record.partPages = estimate.lastPages;
    // While the reader shows percentages, the number held for the page, such
    // as one restored at the open. None after a turn.
    record.shown = estimate.book.shown();
    record.itemShown = rough.itemPage;
    record.read = map.completeItems();
    if (map.keeping())
        record.kept = map.keptPages();
    // The parts of the spine items that stay in the record, the nearest to
    // the reading position first.
    auto parts = map.completeParts();
    std::stable_sort(parts.begin(), parts.end(),
                     [&](const PageEstimateRecord::Part &a, const PageEstimateRecord::Part &b) {
                         return qAbs(a.item - estimate.lastItem) < qAbs(b.item - estimate.lastItem);
                     });
    for (const auto &part : parts) {
        if (record.parts.size() >= PageEstimateRecord::maxParts)
            break;
        if (record.read.contains(part.item))
            record.parts.append(part);
    }
    std::stable_sort(record.parts.begin(), record.parts.end(),
                     [](const PageEstimateRecord::Part &a, const PageEstimateRecord::Part &b) {
                         return a.item != b.item ? a.item < b.item : a.first < b.first;
                     });
    // Keep the spine items nearest to the reading position.
    while (record.read.size() > PageEstimateRecord::maxRead) {
        const int firstItem = record.read.firstKey(), lastItem = record.read.lastKey();
        if (estimate.lastItem - firstItem > lastItem - estimate.lastItem)
            record.read.remove(firstItem);
        else
            record.read.remove(lastItem);
    }
    return record.toJson();
}

void EngineDocument::restorePageEstimate(const QJsonValue &saved)
{
    PageEstimateRecord record;
    if (windowFirst_ < 0 || !source_ || !engine_ || !PageEstimateRecord::fromJson(saved, &record))
        return;
    auto &map = pageMap();
    // The corrected weights are the starting point of this reading.
    map.setPrior(record.model);
    if (record.spineItems != spineItemCount_ || record.layout != estimateKey(settingsState()))
        return;
    map.restore(record.read);
    map.restoreParts(record.parts);
    map.restoreKept(record.kept);
    auto &estimate = source_->pageEstimate;
    estimate.key = record.layout;
    if (record.item < 0)
        return;
    estimate.book.restore(record.shown);
    estimate.item.restore(record.itemShown);
    estimate.lastItem = record.item;
    estimate.lastFirst = record.first;
    // The record does not hold the end of the slice. A turn from the saved
    // page counts as a turn inside the slice.
    estimate.lastLast = INT_MAX;
    estimate.lastPage = record.page;
    estimate.lastPages = record.partPages;
}

bool EngineDocument::refinePageEstimate(const std::function<bool()> &current)
{
    if (current && !current())
        return true;
    if (windowFirst_ < 0 || spineItemCount_ <= 0 || !source_ || !engine_)
        return false;
    auto &map = pageMap();
    auto &estimate = source_->pageEstimate;
    if (!estimate.measurer) {
        // The spine item on the screen goes first: its slices place the
        // observations.
        const int item = map.nextUnmeasured(windowFirst_);
        if (item < 0)
            return measureChapterPlaces(current);
        const auto href = spineItems().value(item);
        estimate.stream = source_->container->OpenStream(
                Utf8ToUnicode(href.toUtf8().constData()).c_str(), LVOM_READ);
        estimate.measurer.reset(new PageEstimateMeasurer);
        estimate.measuring = item;
    }
    const int part = pageEstimatePart;
    QByteArray chunk;
    bool last = true;
    if (!estimate.stream.isNull()) {
        chunk.resize(part);
        lvsize_t read = 0;
        if (estimate.stream->Read(chunk.data(), part, &read) != LVERR_OK)
            read = 0;
        chunk.resize(int(read));
        last = read == 0 || estimate.stream->GetPos() >= estimate.stream->GetSize();
    }
    estimate.measurer->feed(chunk, last);
    if (estimate.measurer->done()) {
        map.setMeasured(estimate.measuring, estimate.measurer->item());
        estimate.measurer.reset();
        estimate.stream = LVStreamRef();
        estimate.measuring = -1;
    }
    return !map.allMeasured() || measureChapterPlaces(current);
}

bool EngineDocument::measureChapterPlaces(const std::function<bool()> &current) const
{
    auto &done = source_->pageEstimate.chapterPlaces;
    const auto list = chapters();
    const auto graph = chapterGraph();
    const int total = list.size() + graph.size();
    // One file in one step: a page turn waits for the step that runs.
    int files = 1;
    while (done < total && files > 0) {
        if (current && !current())
            return true;
        const auto &position =
                done < list.size() ? list[done].position : graph[done - list.size()].position;
        const int item = spineItemIndex(position);
        // The text on the screen has a layout to count from.
        if (item < 0 || item >= spineItemCount_ || (item >= windowFirst_ && item <= windowLast_)
            || source_->textFractions.contains(position)) {
            ++done;
            continue;
        }
        textFraction(item, position, false, current);
        // Retry this place after an interruption. textFraction keeps no
        // partial result, and its reads and parser check between chunks.
        if (current && !current())
            return true;
        ++done;
        --files;
    }
    return done < total;
}

EngineDocument::RoughPlace EngineDocument::roughPlace() const
{
    RoughPlace place;
    place.item = windowFirst_;
    place.page = page();
    place.pages = pages();
    if (slices()) {
        const int text = spineItemText();
        place.characters = qMax(1, text);
        place.first = sliceFirst_;
        place.last = text > 0 ? qMax(sliceFirst_, sliceEnd_) : place.characters;
        // The pages after the end of the slice show the start of the next
        // slice. Place them there, so that the same page has the same place
        // before and after the next slice gets its layout.
        if (visiblePages_ > 0) {
            if (place.page >= visiblePages_) {
                place.first = sliceEnd_;
                place.last =
                        qBound(sliceEnd_,
                               sliceForcedEnd_ > 0 ? sliceForcedEnd_ : sliceFirst_ + sliceReach(),
                               place.characters);
                place.page -= visiblePages_;
                place.pages -= visiblePages_;
            } else {
                place.pages = visiblePages_;
            }
        }
        return place;
    }
    const auto items = spineItems();
    place.item = qBound(windowFirst_, spineItemOf(position()), windowLast_);
    QStringList starts;
    for (const int index : { place.item, place.item + 1 }) {
        Location start;
        start.href = items.value(index);
        starts.append(index <= windowLast_ ? positionOf(start) : QString());
    }
    const auto bounds = pagesOf(starts);
    const int start = qMax(0, bounds.value(0, 0));
    const int end = bounds.value(1, -1) > start ? bounds[1] : pages();
    place.page -= start;
    place.pages = end - start;
    return place;
}

Engine::RoughPages EngineDocument::roughPages() const
{
    RoughPages rough;
    if (windowFirst_ < 0 || spineItemCount_ <= 0 || !source_ || !engine_ || pages() <= 0)
        return rough;
    auto &map = pageMap();
    if (map.total() <= 0)
        return rough;
    const auto place = roughPlace();
    // The page comes from the map: the pages before the spine item, the
    // pages before the part in the item and the page in the part. Where
    // everything before the page has a layout, this is the real page.
    const double inItem = map.pagesBefore(place.item, place.first, place.characters);
    const double before = map.pagesBefore(place.item) + inItem;
    auto &estimate = source_->pageEstimate;
    // How the page follows the page that was placed last.
    using Step = PageAnchor::Step;
    Step step = Step::Jump;
    const auto order = [](int item, int first) { return (qint64(item) << 32) | quint32(first); };
    const qint64 here = order(place.item, place.first),
                 there = order(estimate.lastItem, estimate.lastFirst);
    if (estimate.lastItem < 0) {
        step = Step::Jump;
    } else if (here == there) {
        step = place.page == estimate.lastPage        ? Step::Same
                : place.page == estimate.lastPage + 1 ? Step::Forward
                : place.page == estimate.lastPage - 1 ? Step::Backward
                                                      : Step::Jump;
    } else if (here > there && place.page == 0 && estimate.lastPage == estimate.lastPages - 1) {
        // Also past spine items without pages, and to a later slice that is
        // not the next one.
        step = Step::Forward;
    } else if (here < there && place.item >= estimate.lastItem - 1 && estimate.lastPage == 0
               && place.page == place.pages - 1) {
        step = Step::Backward;
    }
    // A forward turn adds one. The map can give more than that at the start
    // of a spine item, after an earlier spine item got its real pages, and
    // after a turn that passed text without a layout, such as slices
    // without a page.
    if (estimate.lastKept && !map.keeping()) {
        // The book got its real pages: the numbers start again from them.
        estimate.item.reset();
        estimate.book.reset();
    }
    estimate.lastKept = map.keeping();
    const bool newItem = place.item != estimate.lastItem;
    const bool passed = !newItem && place.first > estimate.lastLast;
    rough.itemPage = estimate.item.place(qRound(inItem) + place.page + 1,
                                         newItem ? Step::Jump : step, passed);
    rough.itemPages = qMax(1, qRound(map.itemPages(place.item)));
    rough.itemPagesEstimated = !map.counted();
    if (map.keeping()) {
        // The totals are an estimate that stays as it is. A spine item with
        // more pages than its estimate repeats its last number.
        rough.itemPage = qMin(rough.itemPage, rough.itemPages);
        estimate.item.restore(rough.itemPage);
    } else {
        rough.itemPages = qMax(rough.itemPage, rough.itemPages);
    }
    if (map.complete()) {
        rough.page = estimate.book.place(qRound(before) + place.page + 1, step, newItem || passed);
        rough.pagesEstimated = !map.counted();
        if (map.keeping()) {
            // The page stays in the pages of its spine item, so that it
            // never passes the total.
            const int first = qRound(map.pagesBefore(place.item)) + 1;
            const int last =
                    qMax(first, qRound(map.pagesBefore(place.item) + map.itemPages(place.item)));
            rough.page = qBound(first, rough.page, last);
            estimate.book.restore(rough.page);
            rough.pages = qMax(last, qRound(map.total()));
        } else {
            rough.pages = qMax(rough.page, qRound(map.total()));
        }
    } else if (step != Step::Same) {
        // Until every spine item is measured or counted, the total is a
        // guess from the bytes, and the reader shows percentages. The first
        // page number after that comes from the map.
        estimate.book.reset();
    }
    estimate.lastItem = place.item;
    estimate.lastFirst = place.first;
    estimate.lastLast = place.last;
    estimate.lastPage = place.page;
    estimate.lastPages = place.pages;
    return rough;
}

double EngineDocument::bookFraction() const
{
    if (windowFirst_ < 0 || spineItemCount_ <= 0)
        return -1;
    const auto rough = roughPages();
    if (rough.pages > 0)
        return qBound(0.0, double(rough.page) / rough.pages, 1.0);
    // Without a total: the bytes of the whole book before the page. In its
    // spine item, the characters before the page.
    const auto &bytes = spineBytes();
    if (!source_ || !engine_ || pages() <= 0 || bytes.size() != spineItemCount_)
        return double(windowFirst_) / spineItemCount_;
    const auto place = roughPlace();
    qint64 total = 0, before = 0;
    for (int i = 0; i < bytes.size(); ++i) {
        total += bytes[i];
        if (i < place.item)
            before += bytes[i];
    }
    if (total <= 0)
        return double(windowFirst_) / spineItemCount_;
    const double inPart = place.pages > 0 ? double(place.page) / place.pages : 0;
    const double inItem = (place.first + (place.last - place.first) * inPart) / place.characters;
    return qBound(0.0, (before + bytes.value(place.item) * qBound(0.0, inItem, 1.0)) / total, 1.0);
}

int EngineDocument::spineItemText() const
{
#if __has_include(<koblaze.h>)
    return engine_ ? engine_->getDocument()->getKoblazeSpineItemChars() : 0;
#else
    return 0;
#endif
}

int EngineDocument::textBefore(const QString &position) const
{
    if (!engine_)
        return -1;
    const auto pointer =
            engine_->getDocument()->createXPointer(Utf8ToUnicode(position.toUtf8().constData()));
    auto *target = pointer.getNode();
    auto *fragment = target;
    while (fragment && !fragment->isNodeName("DocFragment"))
        fragment = fragment->getParentNode();
    // The engine counts the text of the body. The styles of the spine item
    // are text beside it.
    auto *body = fragment ? enginelocation::body(fragment) : nullptr;
    if (!body)
        return -1;
    int count = 0;
#ifdef KOBLAZE_TEXT_LENGTHS
    // The engine remembers the text of each node. Counting it again for
    // every position takes longer than the layout of a slice.
    auto *document = engine_->getDocument();
    if (target != fragment && target != body) {
        count = document->koblazeTextBefore(body, target);
        if (count < 0)
            return -1;
    }
    if (target->isText())
        count += qBound(0, pointer.getOffset(), document->koblazeTextLength(target));
#else
    if (target != fragment && target != body && !countText(body, target, count))
        return -1;
    if (target->isText())
        count += qBound(0, pointer.getOffset(), int(target->getText().length()));
#endif
    return count;
}

int EngineDocument::estimatedBookPage(const QString &position) const
{
    if (windowFirst_ < 0 || !source_ || !engine_)
        return -1;
    auto &map = pageMap();
    if (!map.complete())
        return -1;
    const auto pointer =
            engine_->getDocument()->createXPointer(Utf8ToUnicode(position.toUtf8().constData()));
    auto *fragment = pointer.getNode();
    while (fragment && !fragment->isNodeName("DocFragment"))
        fragment = fragment->getParentNode();
    auto *body = fragment ? enginelocation::body(fragment) : nullptr;
    const int before = textBefore(position);
    if (!body || before < 0)
        return -1;
    const int item = int(spineItems().indexOf(
            QString::fromUtf8(UnicodeToUtf8(fragment->getAttributeValue("Source")).c_str())));
    // The characters of the item end with its last text.
    auto *last = body;
    while (last && last->isElement() && last->getChildCount() > 0)
        last = last->getChildNode(last->getChildCount() - 1);
    int characters = before;
    if (last && last->isText()) {
        ldomXPointer end(last, int(last->getText().length()));
        characters =
                qMax(before, textBefore(QString::fromUtf8(UnicodeToUtf8(end.toString()).c_str())));
    }
    if (item < 0)
        return -1;
    return qMax(0, int(map.pagesBefore(item) + map.pagesBefore(item, before, qMax(1, characters))));
}

void EngineDocument::applySlice()
{
#if __has_include(<koblaze.h>)
    if (!engine_)
        return;
    visiblePages_ = 0;
    if (!slices()) {
        engine_->getDocument()->setKoblazeLayoutSlice(0, 0);
        return;
    }
    engine_->getDocument()->setKoblazeLayoutSlice(
            sliceFirst_, sliceForcedEnd_ > 0 ? sliceForcedEnd_ : sliceFirst_ + sliceReach());
#endif
}

void EngineDocument::relayout()
{
    // The blocks outside the last slice have no layout method. The engine
    // gives those methods back and keeps the styles. When it cannot, the
    // new slice needs the styles again.
#ifdef KOBLAZE_RELAYOUT
    if (engine_->getDocument()->koblazeRelayout())
        return;
#endif
    engine_->getDocument()->forceReinitStyles();
}

int EngineDocument::sliceReach() const
{
    // A third more than the slice, to find a page that starts a block, and
    // twice as much more for each search that found none.
    const int size = sliceSize();
    return size + (size / 3) * (1 << qMin(sliceExtra_, 12));
}

bool EngineDocument::pageStartsBlock(int page, int *before) const
{
    const auto position = pageStart(page);
    const auto pointer =
            engine_->getDocument()->createXPointer(Utf8ToUnicode(position.toUtf8().constData()));
    auto *block = pointer.getNode();
    while (block && (block->isText() || block->getRendMethod() != erm_final))
        block = block->getParentNode();
    if (!block || block->isNodeName("DocFragment"))
        return false;
#ifdef KOBLAZE_TEXT_LENGTHS
    // A block without text, such as an image, belongs to the slice that
    // ends at it. A slice starts with a block that has text.
    if (engine_->getDocument()->koblazeTextLength(block) <= 0)
        return false;
#endif
    // A slice holds whole blocks of the text flow. A table, a float or
    // another such block is laid out as one, so a slice cannot start in it.
    for (auto *parent = block->getParentNode(); parent && !parent->isNodeName("DocFragment");
         parent = parent->getParentNode()) {
        if (parent->getRendMethod() != erm_block)
            return false;
    }
    const int start =
            textBefore(QString::fromUtf8(UnicodeToUtf8(ldomXPointer(block, 0).toString()).c_str()));
    if (start < 0 || textBefore(position) != start)
        return false;
    if (earlierContentOnPage(block, page))
        return false;
    *before = start;
    return true;
}

bool EngineDocument::earlierContentOnPage(ldomNode *block, int page) const
{
    // Pages of vertical text advance along x.
    if (engine_->isVerticalText())
        return false;
    const auto *list = engine_->getPageList();
    const int index = page * qMax(1, columns_);
    if (!list || index < 0 || index >= list->length())
        return false;
    const int top = list->get(index)->start;
    // A container can start this page with a border or padding before its
    // first text block. Starting the slice at that text would omit the space.
    lvRect first;
    block->getAbsRect(first);
    for (auto *parent = block->getParentNode(); parent && !parent->isNodeName("DocFragment");
         parent = parent->getParentNode()) {
        lvRect box;
        parent->getAbsRect(box);
        if (box.top >= top && box.top < first.top)
            return true;
    }
    // The page can start a little below the top of the last line before the
    // block. That line is then on the page, and the page start names the
    // block.
    ldomXPointerEx previous(block, 0);
    if (previous.prevVisibleText()) {
        previous.setOffset(qMax(0, int(previous.getNode()->getText().length()) - 1));
        lvRect rect;
        if (previous.getRect(rect) && rect.bottom > top)
            return true;
    }
#ifdef KOBLAZE_TEXT_LENGTHS
    // A block without text just before `block`, such as a rule drawn by a
    // ::before pseudo element, has the same text count. The engine puts it
    // in the slice before, so the page must not show it.
    auto *document = engine_->getDocument();
    for (auto *node = block; node && !node->isNodeName("DocFragment");
         node = node->getParentNode()) {
        for (int i = node->getNodeIndex() - 1; i >= 0; --i) {
            auto *sibling = node->getParentNode()->getChildNode(i);
            if (document->koblazeTextLength(sibling) > 0)
                return false;
            if (!sibling->isElement() || sibling->getRendMethod() == erm_invisible)
                continue;
            lvRect rect;
            sibling->getAbsRect(rect);
            if (rect.height() > 0 && rect.bottom > top)
                return true;
        }
    }
#endif
    return false;
}

void EngineDocument::cutSlice()
{
    visiblePages_ = 0;
    sliceEnd_ = 0;
    sliceCut_ = true;
    if (!engine_ || !slices())
        return;
    const int text = spineItemText();
    const int laidOutEnd = sliceForcedEnd_ > 0 ? sliceForcedEnd_ : sliceFirst_ + sliceReach();
    sliceEnd_ = qMin(laidOutEnd, text);
    // A forced end and the end of the spine item need no search.
    if (sliceForcedEnd_ > 0 || text <= laidOutEnd)
        return;
    const int laidOut = engine_->getPageCount();
    // The last page that starts a block, in the later half of the pages.
    // Without one the slice ends where the layout ends, on a short page.
    for (int page = laidOut - 1; page >= qMax(1, laidOut / 2); --page) {
        int before = 0;
        if (pageStartsBlock(page, &before) && before > sliceFirst_) {
            visiblePages_ = page;
            sliceEnd_ = before;
            return;
        }
    }
    sliceCut_ = false;
}

bool EngineDocument::extendSlice()
{
    // Dense text with long paragraphs can have no page that starts a block.
    // A slice that ends anywhere else leaves a short page, which the whole
    // spine item does not have. Lay out more and look again, until such a
    // page is found or the slice reaches the end of the spine item.
    while (slices() && !sliceCut_ && sliceExtra_ < 12) {
        ++sliceExtra_;
        cache_.clear();
        relayout();
        engine_->requestRender();
        if (!renderLayout())
            return false;
    }
    return true;
}

bool EngineDocument::layoutSlice(int first, int forcedEnd)
{
    EngineSlowdownScope slowdown(true, sliceInterrupt_);
    if (!engine_ || !slices())
        return false;
    QElapsedTimer timer;
    timer.start();
    sliceFirst_ = qMax(0, first);
    sliceForcedEnd_ = forcedEnd > sliceFirst_ ? forcedEnd : 0;
    sliceExtra_ = 0;
    searchText_.reset();
    sourceIndex_.reset();
    cache_.clear();
    damage_.clear();
    OpeningCallback callback(engine_, {}, sliceInterrupt_);
    relayout();
    engine_->requestRender();
    if (!renderLayout())
        return false;
    if (!extendSlice())
        return false;
    observeLayout();
    engineSlowdownPause();
    windowLoadMs_ = timer.elapsed();
    return true;
}

bool EngineDocument::enterNextSlice(const std::function<bool()> &current)
{
    if (!pastSliceEnd())
        return false;
    // The next slice starts with the page at visiblePages_, so the pages
    // after it keep their order.
    const int shown = page();
    const int ahead = shown - visiblePages_;
#if defined(KOBLAZE_SAVED_LAYOUT) && defined(KOBLAZE_CANCEL_CHECK)
    // The reader still has pages of this slice to turn to. When other work
    // waits, the layout of the next slice stops and this slice comes back.
    const int first = sliceFirst_, end = sliceEnd_, visible = visiblePages_,
              forced = sliceForcedEnd_, extra = sliceExtra_;
    const bool cut = sliceCut_;
    const auto history = sliceHistory_;
    const qint64 loadMs = windowLoadMs_;
    if (current && engine_->koblazeSaveLayout()) {
        sliceHistory_.append({ sliceFirst_, sliceForcedEnd_ });
        bool done = false;
        {
            const StopCheck check(current);
            sliceInterrupt_ = [&current] {
                return stopRequested(const_cast<void *>(static_cast<const void *>(&current)));
            };
            done = layoutSlice(sliceEnd_);
            sliceInterrupt_ = nullptr;
        }
        if (done) {
            engine_->getDocument()->koblazeDropSavedLayout();
            return go(qBound(0, ahead, qMax(0, pages() - 1)));
        }
        sliceFirst_ = first;
        sliceEnd_ = end;
        sliceForcedEnd_ = forced;
        sliceExtra_ = extra;
        sliceCut_ = cut;
        sliceHistory_ = history;
        windowLoadMs_ = loadMs;
        if (engine_->koblazeRestoreLayout()) {
            visiblePages_ = visible;
            layoutInterrupted_ = false;
            return go(shown) && false;
        }
        // The layout could not come back: lay out the slice again. This
        // does not stop.
        if (!layoutSlice(first, forced))
            return false;
        go(qBound(0, shown, qMax(0, pages() - 1)));
        return false;
    }
#else
    Q_UNUSED(current);
#endif
    sliceHistory_.append({ sliceFirst_, sliceForcedEnd_ });
    if (!layoutSlice(sliceEnd_))
        return false;
    return go(qBound(0, ahead, qMax(0, pages() - 1)));
}

void EngineDocument::prepareAhead(const std::function<bool()> &current)
{
    enterNextSlice(current);
    if (!current || current()) {
        if (page() == 0 && windowFirst_ > 0 && (!slices() || sliceFirst_ == 0))
            preparePreviousItem(current);
        else
            prepareNextItem(current);
    }
}

bool EngineDocument::showSliceOf(const QString &position)
{
    if (!slices())
        return true;
    const int before = textBefore(position);
    if (before < 0)
        return true;
    const auto laidOut = [&] {
        auto *document = engine_->getDocument();
        const int page = pagesOf({ position }).value(0, -1);
        if (pages() <= 0 || page < 0 || page >= pages())
            return false;
        ldomXPointerEx start(
                document->createXPointer(Utf8ToUnicode(pageStart(page).toUtf8().constData())));
        ldomXPointerEx target(
                document->createXPointer(Utf8ToUnicode(position.toUtf8().constData())));
        return start.isNull() || target.isNull() || start.compare(target) <= 0;
    };
    if (before >= sliceFirst_ && before < sliceEnd_ && laidOut())
        return true;
    if (!showSliceAt(before))
        return false;
    // A block belongs to the slice in which it starts. A position late in
    // a block that started earlier is in the slice before.
    if (!laidOut() && !sliceHistory_.isEmpty()) {
        const auto previous = sliceHistory_.takeLast();
        if (!layoutSlice(previous.first, previous.second))
            return false;
    }
    return true;
}

bool EngineDocument::showSliceAt(int before)
{
    // Only slices that start where reading forward from the start of the
    // spine item starts them give the pages of the whole book. Start from
    // the nearest such slice before the position: a recorded one, the one
    // on screen or the first, and lay out the slices after it in order.
    // On the extra pages after the end of the slice, the position belongs
    // to the next slice, so that its page is a whole page.
    auto starts = sliceHistory_;
    starts.append({ sliceFirst_, sliceForcedEnd_ });
    while (!starts.isEmpty() && starts.last().first > before)
        starts.removeLast();
    const auto start = starts.isEmpty() ? qMakePair(0, 0) : starts.takeLast();
    sliceHistory_ = starts;
    if ((start.first != sliceFirst_ || start.second != sliceForcedEnd_)
        && !layoutSlice(start.first, start.second))
        return false;
    while (before >= sliceEnd_ && sliceEnd_ < spineItemText()) {
        sliceHistory_.append({ sliceFirst_, sliceForcedEnd_ });
        if (!layoutSlice(sliceEnd_))
            return false;
    }
    return true;
}

double EngineDocument::textFraction(int item, const QString &position, bool previous,
                                    const std::function<bool()> &current) const
{
    const QString key = previous ? QStringLiteral("previous:") + position : position;
    const auto known = source_->textFractions.constFind(key);
    if (known != source_->textFractions.cend())
        return known.value();
    if (source_->textFractions.size() > 4096) {
        source_->textFractions.clear();
        source_->textWordPrefixes.clear();
    }
    const double fraction = readTextFraction(item, position, previous, current);
    if (current && !current())
        return -1;
    source_->textFractions.insert(key, fraction);
    return fraction;
}

double EngineDocument::readTextFraction(int item, const QString &position, bool previous,
                                        const std::function<bool()> &current) const
{
    const auto href = spineItems().value(item);
    if (href.isEmpty())
        return -1;
    auto stream = source_->container->OpenStream(Utf8ToUnicode(href.toUtf8().constData()).c_str(),
                                                 LVOM_READ);
    if (stream.isNull())
        return -1;
    QByteArray xhtml(int(stream->GetSize()), Qt::Uninitialized);
    int offset = 0;
    while (offset < xhtml.size()) {
        if (current && !current())
            return -1;
        lvsize_t read = 0;
        if (stream->Read(xhtml.data() + offset, qMin(pageEstimatePart, int(xhtml.size()) - offset),
                         &read)
            != LVERR_OK)
            return -1;
        if (!read)
            break;
        offset += int(read);
    }
    xhtml.resize(offset);
    // The position goes on after the DocFragment of its spine item.
    static const QRegularExpression fragment(QStringLiteral("^/body(?:\\[1\\])?/DocFragment[^/]*"));
    auto path = position;
    path.remove(fragment);
    qint64 words = -1;
    const double fraction = pageEstimateTextFraction(xhtml, path, previous, &words, current);
    if (!previous && words >= 0)
        source_->textWordPrefixes.insert(position, words);
    return fraction;
}

QVector<int> EngineDocument::bookPages(const QStringList &positions,
                                       const std::function<bool()> &current,
                                       bool rangeEndpoints) const
{
    QVector<int> result(positions.size(), -1);
    if (!engine_)
        return result;
    if (windowFirst_ < 0) {
        const auto pages = pagesOf(positions, rangeEndpoints, nullptr, current);
        if (pages.size() != positions.size())
            return {};
        for (int i = 0; i < pages.size(); ++i)
            result[i] = pages[i] >= 0 ? pages[i] + 1 : -1;
        return result;
    }
    if (spineItemCount_ <= 0 || !source_ || pages() <= 0)
        return result;
    auto &map = pageMap();
    const bool known = map.complete() && map.total() > 0;
    const int last = qMax(1, qRound(map.total()));
    const auto items = spineItems();
    // The pages of the spine items of the window, for a unit of several.
    QVector<int> itemStarts;
    if (!slices()) {
        QStringList starts;
        for (int item = windowFirst_; item <= windowLast_ + 1; ++item) {
            Location start;
            start.href = items.value(item);
            starts.append(item <= windowLast_ ? positionOf(start) : QString());
        }
        itemStarts = pagesOf(starts);
        if (itemStarts.size() == starts.size())
            itemStarts.last() = pages();
    }
    for (int i = 0; i < positions.size(); ++i) {
        if (current && !current())
            return {};
        QString position = positions[i];
        int item = spineItemIndex(position);
        if (item < 0 || item >= spineItemCount_)
            continue;
        if (!known) {
            result[i] = 0;
            continue;
        }
        bool exclusive = rangeEndpoints && i % 2 == 1;
        if (exclusive && item >= windowFirst_ && item <= windowLast_) {
            ldomXPointerEx pointer(engine_->getDocument()->createXPointer(
                    Utf8ToUnicode(position.toUtf8().constData())));
            if (!pointer.isNull() && pointer.prevVisibleChar(false)) {
                position = QString::fromUtf8(UnicodeToUtf8(pointer.toString()).c_str());
                item = spineItemIndex(position);
                exclusive = false;
            }
        }
        // Text with a layout counts as roughPages() does: the pages before
        // its part, then the pages of the layout.
        double pages = -1;
        if (item >= windowFirst_ && item <= windowLast_) {
            const int page = pagesOf({ position }).value(0, -1);
            if (slices()) {
                const int before = textBefore(position);
                if (page >= 0 && before >= sliceFirst_ && before < sliceEnd_)
                    pages = qRound(map.pagesBefore(item)
                                   + map.pagesBefore(item, sliceFirst_, qMax(1, spineItemText())))
                            + page;
            } else if (page >= 0 && itemStarts.size() > item - windowFirst_ + 1) {
                const int start = qMax(0, itemStarts[item - windowFirst_]);
                pages = qRound(map.pagesBefore(item)) + qMax(0, page - start);
            }
        }
        // Other text counts from its XHTML.
        if (pages < 0) {
            double fraction = qMax(0.0, textFraction(item, position, false, current));
            if (exclusive && fraction == 0 && item > 0) {
                // The first character of a chapter excludes that whole chapter.
                pages = qRound(map.pagesBefore(item)) - 1;
            } else {
                if (exclusive)
                    fraction = qMax(0.0, textFraction(item, position, true, current));
                exclusive = false;
                pages = std::floor(map.pagesBefore(item)
                                   + map.pagesBefore(item, qRound(fraction * 1000000), 1000000));
            }
        } else {
            exclusive = false;
        }
        if (map.keeping() && !exclusive) {
            // As in roughPages(): the page stays in the pages of its spine
            // item.
            const int first = qRound(map.pagesBefore(item));
            pages = qBound(
                    double(first), pages,
                    double(qMax(first, qRound(map.pagesBefore(item) + map.itemPages(item)) - 1)));
        }
        result[i] = qBound(1, int(pages) + 1, last);
    }
    return result;
}

bool EngineDocument::goToFraction(double fraction)
{
    fraction = qBound(0.0, fraction, 1.0);
    if (!engine_)
        return false;
    if (windowFirst_ < 0)
        return pages() > 0 && go(qBound(0, int(fraction * pages()), pages() - 1));
    if (spineItemCount_ <= 0 || !source_)
        return false;
    // The spine item at the fraction, and the part of it, by the estimated
    // pages, or by the bytes before the book is measured.
    auto &map = pageMap();
    const auto &bytes = spineBytes();
    const bool measured = map.total() > 0;
    const auto size = [&](int item) {
        return measured ? map.itemPages(item) : double(bytes.value(item));
    };
    double total = 0;
    for (int item = 0; item < spineItemCount_; ++item)
        total += size(item);
    double target = fraction * total;
    int item = 0;
    while (item + 1 < spineItemCount_ && target >= size(item)) {
        target -= size(item);
        ++item;
    }
    double inItem = size(item) > 0 ? qBound(0.0, target / size(item), 1.0) : 0;
    if (measured) {
        // The pages of a spine item are not spread evenly over its text.
        double low = 0, high = 1;
        for (int step = 0; step < 20; ++step) {
            const double middle = (low + high) / 2;
            (map.pagesBefore(item, qRound(middle * 1000000), 1000000) < target ? low : high) =
                    middle;
        }
        inItem = low;
    }
    if (item < windowFirst_ || item > windowLast_) {
        const auto unit = slicePages_ > 0 ? unitFrom(item)
                                          : qMakePair(qMax(0, item - 1),
                                                      qMin(spineItemCount_ - 1, item + 1));
        if (!showSpineItems(unit.first, unit.second))
            return false;
    }
    if (slices()) {
        const int character = int(inItem * spineItemText());
        if (!showSliceAt(character))
            return false;
        // The last page of the slice that starts at or before the character.
        const int shown = visiblePages_ > 0 ? visiblePages_ : pages();
        int page = 0;
        for (int candidate = 1; candidate < shown; ++candidate) {
            const int before = textBefore(pageStart(candidate));
            if (before < 0 || before > character)
                break;
            page = candidate;
        }
        return go(page);
    }
    Location start, next;
    start.href = spineItems().value(item);
    next.href = spineItems().value(item + 1);
    const auto bounds = pagesOf({ positionOf(start), item < windowLast_ ? positionOf(next) : QString() });
    const int first = qMax(0, bounds.value(0, 0));
    const int end = bounds.value(1, -1) > first ? bounds[1] : pages();
    return go(qBound(0, first + int(inItem * (end - first)), qMax(0, pages() - 1)));
}

void EngineDocument::estimateChapterPages(const QVector<ChapterLink> &links,
                                          ChapterSelection &result) const
{
    // A chapter can start or end in text that has no layout. Its pages
    // then come from the estimate, as the page numbers of the book do.
    // Until the estimate has a total, the pages of the layout stay.
    if (windowFirst_ < 0 || result.current < 0)
        return;
    QStringList positions{ anchor_, links[result.current].position };
    if (result.next >= 0)
        positions.append(links[result.next].position);
    const auto pages = bookPages(positions);
    if (pages.size() != positions.size() || pages[0] <= 0 || pages[1] <= 0)
        return;
    const int first = qMin(pages[0], pages[1]);
    const int last = result.next < 0
            ? qMax(first, qRound(pageMap().total()))
            : qMax(first, (pages[2] > 0 ? pages[2] : pages[0] + 1) - 1);
    result.pages = last - first + 1;
    result.page = qBound(1, pages[0] - first + 1, result.pages);
}
} // namespace kobalt
