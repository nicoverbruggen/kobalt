#include "column_view.h"
#include "engine_document.h"
#include "reader_locations.h"
#include "reader_render_callback.h"
#include "reader_source.h"
#include <QElapsedTimer>
#include <lvdocview.h>

// Layout of one spine item in slices, with the koblaze fork. See
// EngineDocument::setChapterSlices().
namespace kobalt {
namespace {
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

Engine::RoughPages EngineDocument::roughPages() const
{
    RoughPages rough;
    if (windowFirst_ < 0 || spineItemCount_ <= 0 || !source_ || pages() <= 0)
        return rough;
    auto &bytes = source_->spineBytes;
    if (bytes.size() != spineItemCount_) {
        bytes.clear();
        for (const auto &href : spineItems()) {
            auto stream = source_->container->OpenStream(
                    Utf8ToUnicode(href.toUtf8().constData()).c_str(), LVOM_READ);
            bytes.append(stream.isNull() ? 0 : qint64(stream->GetSize()));
        }
    }
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
    if (target != fragment && target != body && !countText(body, target, count))
        return -1;
    if (target->isText())
        count += qBound(0, pointer.getOffset(), int(target->getText().length()));
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

int EngineDocument::sliceReach() const
{
    // A third more than the slice, to find a page that starts a block, and
    // as much again for each search that found none.
    const int size = sliceSize();
    return size + (1 + sliceExtra_) * (size / 3);
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
    OpeningCallback callback(engine_, {});
    // The blocks outside the last slice have no layout method. A new slice
    // needs the styles again.
    engine_->getDocument()->forceReinitStyles();
    engine_->requestRender();
    if (!renderLayout())
        return false;
    // Dense text with long paragraphs can have no page that starts a block.
    // Lay out more and look again, a few times.
    while (!sliceCut_ && sliceExtra_ < 3) {
        ++sliceExtra_;
        cache_.clear();
        engine_->getDocument()->forceReinitStyles();
        engine_->requestRender();
        if (!renderLayout())
            return false;
    }
    windowLoadMs_ = timer.elapsed();
    return true;
}

bool EngineDocument::enterNextSlice()
{
    if (!pastSliceEnd())
        return false;
    // The next slice starts with the page at visiblePages_, so the pages
    // after it keep their order.
    const int ahead = page() - visiblePages_;
    sliceHistory_.append({ sliceFirst_, sliceForcedEnd_ });
    if (!layoutSlice(sliceEnd_))
        return false;
    return go(qBound(0, ahead, qMax(0, pages() - 1)));
}

void EngineDocument::prepareAhead()
{
    enterNextSlice();
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
