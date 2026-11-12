#include "column_view.h"
#include "stop_point.h"
#include "engine_document.h"
#include "reader_locations.h"
#include "reader_render_callback.h"
#include "reader_source.h"
#include <QElapsedTimer>
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

Engine::RoughPages EngineDocument::roughPages() const
{
    RoughPages rough;
    if (windowFirst_ < 0 || spineItemCount_ <= 0 || !source_ || pages() <= 0)
        return rough;
    const auto &bytes = spineBytes();
    // The pages of the spine items that have their text.
    double loadedPages = pages(), before = 0;
    if (slices()) {
        const double text = qMax(1, spineItemText());
        const double slice = qMax(1.0, double(sliceEnd_ - sliceFirst_));
        loadedPages = qMax(double(pages()), pages() * text / slice);
        before = loadedPages * sliceFirst_ / text;
    }
    qint64 loadedBytes = 0, earlier = 0, total = 0;
    for (int i = 0; i < bytes.size(); ++i) {
        total += bytes[i];
        if (i < windowFirst_)
            earlier += bytes[i];
        else if (i <= windowLast_)
            loadedBytes += bytes[i];
    }
    if (loadedBytes <= 0)
        return rough;
    const double pagesPerByte = loadedPages / loadedBytes;
    rough.itemPage = qRound(before) + page() + 1;
    rough.itemPages = qMax(rough.itemPage, qRound(loadedPages));
    rough.page = qRound(earlier * pagesPerByte) + rough.itemPage;
    rough.pages = qMax(rough.page, qRound(total * pagesPerByte));
    return rough;
}

double EngineDocument::bookFraction() const
{
    if (windowFirst_ < 0 || spineItemCount_ <= 0)
        return -1;
    const auto rough = roughPages();
    if (rough.pages > 0)
        return qBound(0.0, double(rough.page) / rough.pages, 1.0);
    return double(windowFirst_) / spineItemCount_;
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
    *before = start;
    return true;
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
    if (!current || current())
        prepareNextItem(current);
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
    // The end of the text that has a layout, with the extra pages.
    const auto reach = [&] {
        return sliceForcedEnd_ > 0 ? sliceForcedEnd_ : sliceFirst_ + sliceReach();
    };
    if (before >= sliceFirst_ && before < sliceEnd_ && laidOut())
        return true;
    // On the extra pages after the end of the slice, the position belongs
    // to the next slice. Show it there, so that its page is a whole page.
    if (before >= sliceEnd_ && before < reach() && sliceEnd_ < spineItemText()) {
        sliceHistory_.append({ sliceFirst_, sliceForcedEnd_ });
        if (!layoutSlice(sliceEnd_))
            return false;
        if (before < sliceEnd_ && laidOut())
            return true;
    }
    // A jump has no recorded slices around it.
    sliceHistory_.clear();
    const int size = sliceSize();
    if (!layoutSlice(before / size * size))
        return false;
    for (int attempts = 0; attempts < 8; ++attempts) {
        if (before >= sliceEnd_ && sliceEnd_ < spineItemText()) {
            if (!layoutSlice(sliceEnd_))
                return false;
        } else if (!laidOut() && sliceFirst_ > 0) {
            // A block belongs to the slice in which it starts. A position
            // late in a block that started earlier is in the slice before.
            if (!layoutSlice(qMax(0, sliceFirst_ - size)))
                return false;
        } else {
            break;
        }
    }
    return true;
}
} // namespace kobalt
