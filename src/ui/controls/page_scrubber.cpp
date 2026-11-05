#include "page_scrubber.h"
#include "render/bookmark_paint.h"
#include "buttons.h"
#include "panels.h"
#include "text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include "ui/input/input.h"
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTouchEvent>

namespace kobalt::ui {
namespace scrubber = metrics::scrubber;

namespace {
// Both card sizes hang the ribbon from the page border.
void paintThumbnail(QPainter &painter, const QRect &card, const QImage &image, bool bookmarked,
                    const QColor &ink)
{
    if (image.isNull()) {
        return;
    }
    const QRect room = card.adjusted(scrubber::padding, scrubber::padding, -scrubber::padding,
                                     -scrubber::padding);
    const QSize fitted = image.size().scaled(room.size(), Qt::KeepAspectRatio);
    const QRect target(room.center() - QPoint(fitted.width() / 2, fitted.height() / 2), fitted);
    // Ordinary cards already have fitted pixels. Oversized desktop previews
    // keep the bounded master and use smooth sampling without a larger cache.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, target.size() != image.size());
    painter.drawImage(target, image);
    if (bookmarked) {
        const qreal width = fitted.width() * .036;
        render::paintBookmarkRibbon(painter,
                                    QRectF(target.right() - fitted.width() * .03 - width,
                                           card.top(), width, fitted.height() * .045),
                                    ink);
    }
}

constexpr int initialZoomMaximumSpan = 1000;
constexpr int zoomScales[] = { 1, 2, 4, 8, 16, 24, 36 };
constexpr int lastZoomLevel = sizeof(zoomScales) / sizeof(zoomScales[0]) - 1;

// Qt 5.2 sends a button's touches to an accepting ancestor before it can
// synthesize a mouse click. Handle the navigation buttons directly, as the slider does.
template <class Base>
class NavigationButton : public Base
{
public:
    template <class... Args>
    explicit NavigationButton(QWidget *parent, Args... args) : Base(args..., parent)
    {
        this->setAttribute(Qt::WA_AcceptTouchEvents);
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::TouchCancel) {
            tracking = false;
            this->setDown(false);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
            || event->type() == QEvent::TouchEnd) {
            const auto points = touchPositions(static_cast<QTouchEvent *>(event));
            if (event->type() == QEvent::TouchBegin) {
                tracking = points.size() == 1 && this->isEnabled();
            }
            if (points.size() != 1) {
                tracking = false;
            }
            this->setDown(tracking && points.size() == 1
                          && this->rect().contains(points.first().toPoint()));
            if (event->type() == QEvent::TouchEnd) {
                const bool activate = tracking && this->isDown();
                tracking = false;
                this->setDown(false);
                if (activate) {
                    this->click();
                }
            }
            event->accept();
            return true;
        }
        return Base::event(event);
    }

private:
    bool tracking = false;
};

} // namespace

class PageThumbnail : public Button
{
public:
    using Button::Button;

    // Keep only the fitted side image here. PageScrubber owns the promotion masters.
    void setImage(const QImage &source)
    {
        image = {};
        if (!source.isNull()) {
            const QSize room = rect().adjusted(scrubber::padding, scrubber::padding,
                                               -scrubber::padding, -scrubber::padding)
                                       .size();
            const QSize fitted = source.size().scaled(room, Qt::KeepAspectRatio);
            image = fitted.width() <= 1024 && fitted.height() <= 1024
                    ? source.scaled(fitted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    : source;
        }
    }

    QImage image;
    bool bookmarked = false;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Base));
        paintEdges(painter, rect(),
                   Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                   palette().color(QPalette::Text));
        paintThumbnail(painter, rect(), image, bookmarked, palette().color(QPalette::Text));
    }
};

PageScrubber::PageScrubber(QWidget *parent)
    : QWidget(parent), position(new SettingSlider(this, 0, 0))
{
    setObjectName("rrPageScrubber");
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_AcceptTouchEvents);
    preceding = new NavigationButton<PageThumbnail>(this);
    following = new NavigationButton<PageThumbnail>(this);
    preceding->setObjectName("rrPreviewPreviousPage");
    following->setObjectName("rrPreviewNextPage");
    preceding->setAccessibleName(strings::get("common.previous_page"));
    following->setAccessibleName(strings::get("common.next_page"));
    for (auto *thumbnail : { preceding, following }) {
        readerStyle(thumbnail, "QPushButton{border:none;padding:0;min-height:0;}");
        thumbnail->installEventFilter(this);
        thumbnail->hide();
    }
    connect(preceding, &QPushButton::clicked, this, [this] {
        if (selecting() && candidate > 0) {
            choose(candidate - 1);
            requestPreview();
        }
    });
    connect(following, &QPushButton::clicked, this, [this] {
        if (selecting() && candidate < totalPages - 1) {
            choose(candidate + 1);
            requestPreview();
        }
    });
    const auto button = [this](const char *name, ControlIcon icon, const QString &label) {
        auto *result = new NavigationButton<Button>(this);
        result->setObjectName(name);
        result->setAccessibleName(label);
        result->setPaintMode(ButtonPaintMode::ToolbarCell);
        buttonIcon(result, icon);
        result->setIconSize(QSize(metrics::control::iconSize, metrics::control::iconSize));
        readerStyle(result, "QPushButton{border:none;padding:0;min-height:0;}");
        return result;
    };
    auto *back = button("rrNavigationPrevious", ControlIcon::Previous,
                        strings::get("common.previous_page"));
    auto *forward = button("rrNavigationNext", ControlIcon::Next, strings::get("common.next_page"));
    previous = back;
    next = forward;
    zoomOut = button("rrScrubberZoomOut", ControlIcon::Minus, strings::get("reader.zoom_out"));
    zoomIn = button("rrScrubberZoomIn", ControlIcon::Plus, strings::get("reader.zoom_in"));
    connect(previous, &QPushButton::clicked, this, [this] { step(-1); });
    connect(next, &QPushButton::clicked, this, [this] { step(1); });
    connect(zoomOut, &QPushButton::clicked, this, [this] { zoom(-1); });
    connect(zoomIn, &QPushButton::clicked, this, [this] { zoom(1); });
    overview = new RangeOverview(this);
    overview->setObjectName("rrScrubberOverview");
    overview->setAccessibleName(strings::get("reader.zoom_overview"));
    overview->moved = [this](int first) { pan(first); };
    rangeLabel = new PrivateTextLabel(this);
    rangeLabel->setObjectName("rrScrubberRange");
    overviewLabel = new PrivateTextLabel(this);
    overviewLabel->setObjectName("rrScrubberOverviewLabel");
    rangeStart = new PrivateTextLabel(this);
    rangeStart->setObjectName("rrScrubberRangeStart");
    rangeEnd = new PrivateTextLabel(this);
    rangeEnd->setObjectName("rrScrubberRangeEnd");
    overviewStart = new PrivateTextLabel(this);
    overviewStart->setText(QStringLiteral("1"));
    overviewEnd = new PrivateTextLabel(this);
    for (auto *label :
         { rangeLabel, overviewLabel, rangeStart, rangeEnd, overviewStart, overviewEnd }) {
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAlignment(Qt::AlignCenter);
        captionLabel(label, scrubber::text);
    }
    overviewLabel->setText(strings::get("reader.whole_book").toUpper());
    for (auto *label : { rangeStart, overviewStart }) {
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    for (auto *label : { rangeEnd, overviewEnd }) {
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    position->setObjectName("rrBookPosition");
    position->setAccessibleName(strings::get("reader.book_position"));
    position->setDotMarkers(true);
    connect(position, &QSlider::valueChanged, this, [this](int page) {
        if (position->interacting()) {
            choose(page);
        }
    });
    position->committed = [this](int page) {
        choose(page);
        requestPreview();
    };
    renderTimer.setSingleShot(true);
    renderTimer.setInterval(100);
    connect(&renderTimer, &QTimer::timeout, this, [this] { requestPreview(); });
    captionBackground = new Panel(this, Qt::TopEdge);
    captionBackground->setObjectName("rrScrubberCaptionBackground");
    captionBackground->setAttribute(Qt::WA_TransparentForMouseEvents);
    captionBackground->setEdgeWidth(metrics::chrome::heavyRule);
    captionBackground->setFill(QPalette::Midlight);
    captionBackground->setDividers(Qt::BottomEdge);
    captionBackground->hide();
    chapterLabel = new PrivateTextLabel(this);
    chapterLabel->setObjectName("rrPreviewChapterCaption");
    chapterLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    chapterLabel->setAlignment(Qt::AlignCenter);
    captionLabel(chapterLabel, scrubber::text);
    readerStyle(chapterLabel,
                "QLabel{" + styles::caption(scrubber::text)
                        + "background:transparent;color:palette(window-text);}");
    chapterLabel->hide();
    overviewRule = new Panel(this, Qt::LeftEdge);
    overviewRule->setSoftEdges(true);
    overviewRule->setAttribute(Qt::WA_TransparentForMouseEvents);
    overviewRule->setFill(QPalette::Midlight);
    auto *disclosure = button("rrScrubberOverviewToggle", ControlIcon::Up,
                              strings::get("reader.expand_book_map"));
    disclosure->setFill(QPalette::Midlight);
    overviewToggle = disclosure;
    connect(overviewToggle, &QPushButton::clicked, this, [this] {
        overviewExpanded = !overviewExpanded;
        arrange();
        if (selecting()) {
            clearPreview();
            setProperty("preview_ready", false);
            renderTimer.start();
        }
    });
    barRule = new Panel(this, Qt::TopEdge);
    barRule->setAttribute(Qt::WA_TransparentForMouseEvents);
    barRule->setEdgeWidth(metrics::control::border);
    barRule->setFill(QPalette::Base);
    actions = new QWidget(this);
    actions->setAttribute(Qt::WA_NoSystemBackground);
    actions->setObjectName("rrScrubberActions");
    linksRule = new Panel(actions);
    returnRule = new Panel(actions);
    imageRule = new Panel(actions);
    for (auto *rule : { linksRule, returnRule, imageRule }) {
        rule->setAttribute(Qt::WA_TransparentForMouseEvents);
        rule->setFill(QPalette::Base);
    }
    const auto action = [this](const char *name, ControlIcon icon) {
        auto *result = new NavigationButton<CaptionButton>(
                actions, CaptionButton::Presentation::CompactAction);
        result->setObjectName(name);
        buttonIcon(result, icon);
        result->hide();
        return result;
    };
    linksAction = action("rrScrubberLinks", ControlIcon::Link);
    returnAction = action("rrScrubberReturn", ControlIcon::Undo);
    imageAction = action("rrScrubberImage", ControlIcon::FitImage);
    stay = new NavigationButton<DismissButton>(actions);
    stay->setObjectName("rrScrubberStay");
    stay->setAccessibleName(strings::get("reader.stay_here"));
    stay->setContentsMargins(0, int(scrubber::actionTop) + int(metrics::control::border), 0,
                             metrics::control::border);
    readerStyle(stay, "QPushButton{border:none;padding:0;min-height:0;}");
    stay->hide();
    dismissImage = new NavigationButton<DismissButton>(actions);
    dismissImage->setObjectName("rrScrubberImageDismiss");
    dismissImage->setAccessibleName(strings::get("image.dismiss_suggestion"));
    dismissImage->setContentsMargins(stay->contentsMargins());
    readerStyle(dismissImage, "QPushButton{border:none;padding:0;min-height:0;}");
    dismissImage->hide();
    actions->hide();
    connect(linksAction, &QPushButton::clicked, this, [this] {
        if (linksRequested) {
            linksRequested();
        }
    });
    connect(returnAction, &QPushButton::clicked, this, [this] {
        if (returnRequested) {
            returnRequested();
        }
    });
    connect(stay, &QPushButton::clicked, this, [this] {
        if (stayRequested) {
            stayRequested();
        }
    });
    connect(imageAction, &QPushButton::clicked, this, [this] {
        if (imageRequested)
            imageRequested();
    });
    connect(dismissImage, &QPushButton::clicked, this, [this] {
        if (imageDismissed)
            imageDismissed();
    });
    hide();
}

void PageScrubber::setBook(int page, int pages, const QVector<QPair<int, QString>> &values,
                           QSize size)
{
    const int count = qMax(1, pages);
    const bool changed = count != totalPages || chapters != values;
    const bool unchanged = !changed && current == qBound(0, page, count - 1)
            && (!size.isValid() || pageSize == size) && !rangeLabel->text().isEmpty();
    if (unchanged) {
        return;
    }
    if (changed) {
        cancel();
        totalPages = count;
        chapters = values;
        zoomLevel = initialZoomLevel();
        overviewExpanded = false;
        rangeFirst = 0;
        rangeLast = count - 1;
    }
    current = qBound(0, page, totalPages - 1);
    if (size.isValid()) {
        pageSize = size;
    }
    if (selecting()) {
        return;
    }
    if (changed || current < rangeFirst || current > rangeLast) {
        centerRange(current);
    }
    syncRange(changed);
    arrange();
}

int PageScrubber::maximumZoomLevel() const
{
    int level = 0;
    while (level < lastZoomLevel
           && zoomScales[level + 1] <= totalPages / scrubber::minimumZoomSpan) {
        ++level;
    }
    return level;
}

int PageScrubber::initialZoomLevel() const
{
    int level = 0;
    const int maximum = maximumZoomLevel();
    while (level < maximum && zoomSpan(level) > initialZoomMaximumSpan) {
        ++level;
    }
    return level;
}

bool PageScrubber::zoomAvailable() const
{
    return maximumZoomLevel() > 0;
}

int PageScrubber::zoomSpan(int level) const
{
    const int scale = zoomScales[qBound(0, level, maximumZoomLevel())];
    return totalPages / scale + (totalPages % scale != 0);
}

void PageScrubber::centerRange(int page)
{
    const int span = zoomSpan(zoomLevel);
    rangeFirst = qBound(0, page - (span - 1) / 2, totalPages - span);
    rangeLast = rangeFirst + span - 1;
}

void PageScrubber::syncRange(bool chaptersChanged)
{
    const bool rangeChanged = position->minimum() != rangeFirst || position->maximum() != rangeLast;
    position->setRange(rangeFirst, rangeLast);
    position->setValue(selecting() ? candidate : current);
    if (chaptersChanged || rangeChanged) {
        QVector<int> markers;
        for (const auto &chapter : chapters)
            markers.append(chapter.first);
        position->setMarkers(markers);
    }
    overview->setRange(totalPages, rangeFirst, rangeLast, current);
    position->setAccessibleDescription(strings::get("reader.zoom_range")
                                               .arg(rangeFirst + 1)
                                               .arg(rangeLast + 1)
                                               .arg(totalPages));
    const auto scope = strings::get("reader.zoom_scale").arg(zoomScales[zoomLevel]);
    rangeLabel->setText(scope.toUpper());
    rangeStart->setText(QString::number(rangeFirst + 1));
    rangeEnd->setText(QString::number(rangeLast + 1));
    overviewEnd->setText(QString::number(totalPages));
    updateButtons();
}

void PageScrubber::zoom(int direction)
{
    if ((direction < 0 && zoomLevel == 0)
        || (direction > 0 && zoomSpan(zoomLevel + 1) == zoomSpan(zoomLevel))) {
        return;
    }
    zoomLevel += direction;
    centerRange(selecting() ? candidate : current);
    syncRange();
    arrange();
    if (selecting()) {
        clearPreview();
        setProperty("preview_ready", false);
        renderTimer.start();
    }
}

void PageScrubber::pan(int first)
{
    const int offset = (selecting() ? candidate : current) - rangeFirst;
    const int span = zoomSpan(zoomLevel);
    rangeFirst = qBound(0, first, totalPages - span);
    rangeLast = rangeFirst + span - 1;
    choose(rangeFirst + qBound(0, offset, span - 1));
    syncRange();
}

void PageScrubber::choose(int page)
{
    if (candidate == page) {
        return;
    }
    const int target = qBound(0, page, totalPages - 1);
    QImage retained[3];
    if (selecting() && qAbs(target - candidate) == 1) {
        for (int index = 0; index < 3; ++index) {
            const int oldIndex = target - candidate + index;
            if (oldIndex >= 0 && oldIndex < 3) {
                retained[index] = prepared[oldIndex];
            }
        }
    }
    if (!selecting() || displayedPage < 0 || qAbs(target - displayedPage) > 1) {
        clearPreview();
    }
    pendingPreview = -1;
    for (int index = 0; index < 3; ++index) {
        prepared[index] = std::move(retained[index]);
        requested[index] = !prepared[index].isNull();
    }
    candidate = target;
    ready = false;
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    if (candidate < rangeFirst || candidate > rangeLast) {
        centerRange(candidate);
    }
    syncRange();
    barRule->setSoftEdges(true);
    position->setValue(candidate);
    setProperty("preview_page", page);
    setProperty("preview_ready", false);
    updateButtons();
    updateMask();
    arrange();
    renderTimer.start();
    update();
}

void PageScrubber::setBookmarkedPages(const QVector<int> &pages)
{
    if (bookmarkedPages == pages) {
        return;
    }
    const bool centerChanged = displayedPage >= 0
            && bookmarkedPages.contains(displayedPage) != pages.contains(displayedPage);
    bookmarkedPages = pages;
    if (centerChanged) {
        update(previewBounds());
    }
    const bool previousMarked = displayedPage > 0 && bookmarkedPages.contains(displayedPage - 1);
    const bool nextMarked = displayedPage >= 0 && bookmarkedPages.contains(displayedPage + 1);
    if (preceding->bookmarked != previousMarked) {
        preceding->bookmarked = previousMarked;
        preceding->update();
    }
    if (following->bookmarked != nextMarked) {
        following->bookmarked = nextMarked;
        following->update();
    }
}

void PageScrubber::setChapterJumps(bool enabled)
{
    if (chapterJumps == enabled) {
        return;
    }
    cancel();
    chapterJumps = enabled;
    buttonIcon(previous, enabled ? ControlIcon::PreviousChapter : ControlIcon::Previous);
    buttonIcon(next, enabled ? ControlIcon::NextChapter : ControlIcon::Next);
    previous->setAccessibleName(
            strings::get(enabled ? "reader.previous_chapter" : "common.previous_page"));
    next->setAccessibleName(strings::get(enabled ? "reader.next_chapter" : "common.next_page"));
    updateButtons();
}

void PageScrubber::updateButtons()
{
    position->setEnabled(totalPages > 1);
    const int page = selecting() ? candidate : current;
    previous->setEnabled(chapterJumps ? !chapters.isEmpty() && page > chapters.first().first
                                      : page > 0);
    next->setEnabled(chapterJumps ? !chapters.isEmpty() && page < chapters.last().first
                                  : page < totalPages - 1);
    zoomOut->setEnabled(zoomLevel > 0);
    zoomIn->setEnabled(zoomSpan(zoomLevel + 1) < zoomSpan(zoomLevel));
}

void PageScrubber::step(int direction)
{
    if (!chapterJumps) {
        turnPreview(direction);
        return;
    }
    const int page = selecting() ? candidate : current;
    if (direction > 0) {
        for (const auto &chapter : chapters) {
            if (chapter.first > page) {
                choose(chapter.first);
                requestPreview();
                return;
            }
        }
    } else {
        for (int i = chapters.size() - 1; i >= 0; --i) {
            if (chapters[i].first < page) {
                choose(chapters[i].first);
                requestPreview();
                return;
            }
        }
    }
}

void PageScrubber::turnPreview(int direction)
{
    if (direction != -1 && direction != 1)
        return;
    const int page = selecting() ? candidate : current;
    const int target = qBound(0, page + direction, totalPages - 1);
    if (target != page) {
        choose(target);
        requestPreview();
    }
}

void PageScrubber::requestPreview()
{
    renderTimer.stop();
    if (!selecting() || !previewRequested || pendingPreview >= 0) {
        return;
    }
    // The session accepts one outstanding preview. Keep that contract, but
    // publish the completed strip in one frame instead of each reply separately.
    for (int index : { 1, 0, 2 }) {
        const int page = candidate + index - 1;
        if (page < 0 || page >= totalPages || requested[index]) {
            continue;
        }
        auto bounds = imageBounds().size();
        if (bounds.width() > 1024 || bounds.height() > 1024) {
            bounds.scale(1024, 1024, Qt::KeepAspectRatio);
        }
        requested[index] = true;
        pendingPreview = page;
        previewRequested(page, bounds);
        return;
    }
    publishPreviews();
}

void PageScrubber::setPreview(int page, QImage image)
{
    if (!selecting() || page != pendingPreview || !isVisible()) {
        return;
    }
    pendingPreview = -1;
    const int index = page - candidate + 1;
    if (index >= 0 && index < 3) {
        prepared[index] = std::move(image);
    }
    requestPreview();
}

void PageScrubber::publishPreviews()
{
    if (prepared[1].isNull() || ready) {
        return;
    }
    preview = prepared[1];
    preceding->setImage(prepared[0]);
    following->setImage(prepared[2]);
    displayedPage = candidate;
    preceding->bookmarked = bookmarkedPages.contains(candidate - 1);
    following->bookmarked = bookmarkedPages.contains(candidate + 1);
    ready = true;
    setProperty("preview_ready", true);
    update(QRegion(previewBounds()) | preceding->geometry() | following->geometry());
}

void PageScrubber::clearPreview()
{
    preview = {};
    preceding->image = {};
    following->image = {};
    for (int index = 0; index < 3; ++index) {
        prepared[index] = {};
        requested[index] = false;
    }
    displayedPage = -1;
    ready = false;
    pendingPreview = -1;
    setProperty("preview_ready", false);
    preceding->update();
    following->update();
}

void PageScrubber::showCurrentPage()
{
    if (!isVisible() || !isEnabled() || selecting())
        return;
    choose(current);
    requestPreview();
}

void PageScrubber::cancel()
{
    const bool wasSelecting = selecting();
    renderTimer.stop();
    candidate = -1;
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    barRule->setSoftEdges(true);
    clearPreview();
    pointerActive = false;
    setProperty("preview_page", -1);
    setProperty("preview_ready", false);
    if (current < rangeFirst || current > rangeLast) {
        centerRange(current);
    }
    syncRange();
    arrange();
    update();
    if (wasSelecting && cancelled) {
        cancelled();
    }
}

QRect PageScrubber::previewBounds() const
{
    const int gap = scrubber::gap;
    const int room = qMax(1, width() - 2 * gap);
    const int pageHeight = qMax(1, panelBounds().top() - actionsHeight());
    const int availableHeight = qMax(1, pageHeight - 2 * gap);
    const int targetHeight =
            qMin(availableHeight, qRound(pageHeight * scrubber::previewHeightFraction));
    const QSize imageRoom(qMax(1, room - 2 * int(scrubber::padding)),
                          qMax(1, targetHeight - 2 * int(scrubber::padding)));
    const auto fitted = pageSize.scaled(imageRoom, Qt::KeepAspectRatio);
    const int w = qMin(room, fitted.width() + 2 * int(scrubber::padding));
    const int h = qMin(availableHeight, fitted.height() + 2 * int(scrubber::padding));
    return { (width() - w) / 2, (pageHeight - h) / 2, w, h };
}

QRect PageScrubber::barBounds() const
{
    const int barHeight = barVisible ? qMin(height(), int(scrubber::barHeight)) : 0;
    return { 0, height() - barHeight, width(), barHeight };
}

QRect PageScrubber::panelBounds() const
{
    if (!barVisible) {
        return barBounds();
    }
    const int panelHeight = int(scrubber::barHeight + scrubber::captionHeight)
            + (overviewExpanded && zoomAvailable() ? int(scrubber::overviewHeight) : 0);
    return { 0, qMax(0, height() - panelHeight), width(), qMin(height(), panelHeight) };
}

QRect PageScrubber::imageBounds() const
{
    return previewBounds().adjusted(scrubber::padding, scrubber::padding, -scrubber::padding,
                                    -scrubber::padding);
}

void PageScrubber::updateMask()
{
    const QRegion previousArea = mask().isEmpty() ? QRegion(rect()) : mask();
    if (selecting()) {
        clearMask();
    } else {
        QRegion hitArea = barVisible ? QRegion(panelBounds()) : QRegion();
        if (!linksAction->isHidden()) {
            hitArea += linksFrame;
        }
        if (!imageAction->isHidden()) {
            hitArea += imageFrame;
            hitArea += QRect(actions->pos() + dismissImage->pos(), dismissImage->size());
        }
        if (!returnAction->isHidden()) {
            hitArea += returnFrame;
            hitArea += QRect(actions->pos() + stay->pos(), stay->size());
        }
        const QRegion exposed = previousArea - hitArea;
        const bool reclip = isVisible() && !exposed.isEmpty();
        if (reclip) {
            // Qt 5.2 retains opaque sibling clips when a visible mask shrinks.
            // All child positions are final before rebuilding those clips.
            relocating = true;
            hide();
        }
        setMask(hitArea);
        if (reclip) {
            show();
            relocating = false;
            if (parentWidget()) {
                parentWidget()->update();
            }
        }
    }
}

void PageScrubber::resizeEvent(QResizeEvent *)
{
    arrange();
    if (selecting()) {
        clearPreview();
        renderTimer.start();
    }
}

int PageScrubber::actionsHeight() const
{
    return linksAction->isHidden() && returnAction->isHidden() && imageAction->isHidden()
            ? 0
            : int(scrubber::actionTop + scrubber::actionHeight);
}

void PageScrubber::setLinksAction(const QString &label)
{
    if (linksAction->text() == label) {
        return;
    }
    linksAction->setText(label);
    linksAction->setAccessibleName(label);
    linksAction->setVisible(!label.isEmpty());
    arrange();
}

void PageScrubber::setReturnAction(const QString &label, const QString &note)
{
    if (returnAction->text() == label && returnAction->caption() == note)
        return;
    returnAction->setText(label);
    returnAction->setCaption(note);
    returnAction->setAccessibleName(label);
    returnAction->setAccessibleDescription(note);
    returnAction->setVisible(!label.isEmpty());
    stay->setVisible(!label.isEmpty());
    arrange();
}

void PageScrubber::setImageAction(const QString &label)
{
    if (imageLabel == label)
        return;
    imageLabel = label;
    imageAction->setText(label);
    imageAction->setAccessibleName(label);
    arrange();
}

void PageScrubber::setBarVisible(bool visible)
{
    if (barVisible == visible) {
        return;
    }
    barVisible = visible;
    arrange();
}

void PageScrubber::arrange()
{
    const int edge = metrics::control::border;
    const bool hasLinks = !linksAction->isHidden(), hasReturn = !returnAction->isHidden();
    const int idealLinks = hasLinks ? linksAction->sizeHint().width() + 2 * edge : 0;
    const int idealReturn = hasReturn ? returnAction->sizeHint().width() + edge : 0;
    const int closeWidth = hasReturn ? int(scrubber::closeWidth) : 0;
    const int gap = hasLinks && hasReturn ? int(scrubber::gap) : 0;
    const int idealImage = imageLabel.isEmpty()
            ? 0
            : imageAction->sizeHint().width() + edge + int(scrubber::closeWidth);
    const int imageGap = hasLinks || hasReturn ? int(scrubber::gap) : 0;
    const int available = qMax(0, width() - 2 * int(scrubber::gap));
    const bool hasImage = idealImage
            && (!(hasLinks || hasReturn)
                || idealImage + imageGap + idealLinks + gap + idealReturn + closeWidth
                        <= available);
    imageAction->setVisible(hasImage);
    dismissImage->setVisible(hasImage);
    const int imageWidth = hasImage ? qMin(idealImage, available) : 0;
    const int imageSpace = hasImage ? imageWidth + imageGap : 0;
    const int button = metrics::chrome::cellWidth;
    const bool mapVisible = barVisible && overviewExpanded && zoomAvailable();
    const QRect bar = barBounds();
    const QRect center = previewBounds();
    const QSize side(qMax(1, qRound(center.width() * .92)), qMax(1, qRound(center.height() * .92)));
    const int y = center.center().y() - side.height() / 2;
    preceding->setGeometry(center.left() - int(scrubber::gap) - side.width(), y, side.width(),
                           side.height());
    following->setGeometry(center.right() + 1 + int(scrubber::gap), y, side.width(), side.height());
    preceding->setVisible(selecting() && candidate > 0);
    following->setVisible(selecting() && candidate < totalPages - 1);
    barRule->setVisible(mapVisible);
    previous->setVisible(barVisible);
    next->setVisible(barVisible);
    position->setVisible(barVisible);
    overviewRule->setVisible(barVisible && zoomAvailable());
    overviewToggle->setVisible(barVisible && zoomAvailable());
    buttonIcon(overviewToggle, overviewExpanded ? ControlIcon::Down : ControlIcon::Up);
    overviewToggle->setAccessibleName(
            strings::get(overviewExpanded ? "reader.collapse_book_map" : "reader.expand_book_map"));
    zoomOut->setVisible(mapVisible);
    zoomIn->setVisible(mapVisible);
    rangeLabel->setVisible(barVisible);
    overview->setVisible(mapVisible);
    overview->setEnabled(zoomLevel > 0);
    overviewLabel->setVisible(mapVisible);
    rangeStart->setVisible(barVisible);
    rangeEnd->setVisible(barVisible);
    overviewStart->setVisible(mapVisible);
    overviewEnd->setVisible(mapVisible);
    barRule->setGeometry(0, bar.top(), width(), metrics::control::border);
    previous->setGeometry(0, bar.top(), button, bar.height());
    next->setGeometry(qMax(0, width() - button), bar.top(), button, bar.height());
    zoomOut->setGeometry(0, bar.top() - int(scrubber::overviewHeight), button,
                         scrubber::overviewHeight);
    zoomIn->setGeometry(qMax(0, width() - button), zoomOut->y(), button, scrubber::overviewHeight);
    const int x = previous->geometry().right() + 1 + int(scrubber::gap);
    const int trackWidth = qMax(1, next->x() - int(scrubber::gap) - x);
    position->setGeometry(x, bar.top() + int(scrubber::verticalPadding), trackWidth,
                          position->minimumHeight());
    rangeLabel->setGeometry(x, position->geometry().bottom() + 1, trackWidth,
                            scrubber::rangeLabelHeight);
    overview->setGeometry(x, zoomOut->y() + int(scrubber::verticalPadding), trackWidth,
                          int(scrubber::overviewHeight - scrubber::rangeLabelHeight
                              - 2 * scrubber::verticalPadding));
    overview->setContentsMargins(0, 0, 0, scrubber::overviewBottomPadding);
    overviewLabel->setGeometry(
            x, overview->geometry().bottom() + 1 - int(scrubber::overviewBottomPadding), trackWidth,
            scrubber::rangeLabelHeight);
    const int inset = metrics::control::sliderInset;
    const int endpointWidth = qMax(1, (trackWidth - 2 * inset) / 4);
    for (auto *label : { rangeStart, overviewStart }) {
        label->setGeometry(x + inset, label == rangeStart ? rangeLabel->y() : overviewLabel->y(),
                           endpointWidth, scrubber::rangeLabelHeight);
    }
    for (auto *label : { rangeEnd, overviewEnd }) {
        label->setGeometry(x + trackWidth - inset - endpointWidth,
                           label == rangeEnd ? rangeLabel->y() : overviewLabel->y(), endpointWidth,
                           scrubber::rangeLabelHeight);
    }
    const int rowHeight = actionsHeight();
    actions->setVisible(rowHeight > 0);
    const int room = qMax(0, available - imageSpace - closeWidth - gap);
    const int labels = idealLinks + idealReturn;
    const int linksWidth = labels ? qMin(idealLinks, room * idealLinks / labels) : 0;
    const int returnWidth = labels ? qMin(idealReturn, room - linksWidth) : 0;
    const int groupWidth = imageSpace + linksWidth + gap + returnWidth + closeWidth;
    const int groupTop = panelBounds().top() - rowHeight - int(scrubber::gap);
    actions->setGeometry((width() - groupWidth) / 2, groupTop, groupWidth, rowHeight);
    const int top = scrubber::actionTop, body = scrubber::actionHeight;
    const int imageClose = hasImage ? qMin(imageWidth, int(scrubber::closeWidth)) : 0;
    const int imageButton = imageWidth - imageClose;
    imageAction->setGeometry(edge, top + edge, qMax(0, imageButton - edge), body - 2 * edge);
    imageAction->setRule({});
    dismissImage->setGeometry(imageButton, 0, qMax(0, imageClose - edge), rowHeight);
    linksAction->setGeometry(imageSpace + edge, top + edge, qMax(0, linksWidth - 2 * edge),
                             body - 2 * edge);
    const int returnLeft = imageSpace + linksWidth + gap;
    returnAction->setGeometry(returnLeft + edge, top + edge, qMax(0, returnWidth - edge),
                              body - 2 * edge);
    returnAction->setRule({});
    stay->setGeometry(returnLeft + returnWidth, 0, qMax(0, closeWidth - edge), rowHeight);
    imageFrame = QRect(actions->x(), groupTop + top, imageWidth, body);
    linksFrame = QRect(actions->x() + imageSpace, groupTop + top, linksWidth, body);
    returnFrame = QRect(actions->x() + returnLeft, groupTop + top, returnWidth + closeWidth, body);
    const auto edges = Edges(Qt::TopEdge) | Qt::LeftEdge | Qt::RightEdge | Qt::BottomEdge;
    imageRule->setEdges(edges);
    imageRule->setGeometry(0, top, imageWidth, body);
    imageRule->setVisible(hasImage);
    linksRule->setEdges(edges);
    returnRule->setEdges(edges);
    linksRule->setGeometry(imageSpace, top, linksWidth, body);
    returnRule->setGeometry(returnLeft, top, returnWidth + closeWidth, body);
    linksRule->setVisible(hasLinks);
    returnRule->setVisible(hasReturn);
    barRule->setSoftEdges(true);
    updateCaption();
    updateMask();
    update();
}

void PageScrubber::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor paper = palette().color(QPalette::Base), ink = palette().color(QPalette::Text);
    if (selecting()) {
        painter.fillRect(rect(), paper);
    }
    const QRect bar = barBounds();
    painter.fillRect(panelBounds(), paper);
    const QColor rule = palette().color(QPalette::Mid);
    const int border = metrics::control::border;
    if (barVisible) {
        painter.fillRect(QRect(previous->geometry().right() + 1, bar.top(), border, bar.height()),
                         rule);
        painter.fillRect(QRect(next->x() - border, bar.top(), border, bar.height()), rule);
        if (overviewExpanded && zoomAvailable()) {
            painter.fillRect(
                    QRect(zoomOut->geometry().right() + 1, zoomOut->y(), border, zoomOut->height()),
                    rule);
            painter.fillRect(QRect(zoomIn->x() - border, zoomIn->y(), border, zoomIn->height()),
                             rule);
        }
    }
    if (!selecting()) {
        return;
    }
    const QRect card = previewBounds();
    painter.fillRect(card, paper);
    paintEdges(painter, card, Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
               ink);
    paintThumbnail(painter, previewBounds(), preview,
                   displayedPage >= 0 && bookmarkedPages.contains(displayedPage), ink);
}

void PageScrubber::updateCaption()
{
    if (!barVisible) {
        overviewRule->hide();
        overviewToggle->hide();
        captionBackground->hide();
        chapterLabel->hide();
        return;
    }
    const int page = selecting() ? candidate : current;
    QString chapterName =
            chapters.isEmpty() ? QString() : strings::get("reader.front_matter").toUpper();
    for (const auto &chapter : chapters) {
        if (chapter.first <= page) {
            chapterName = chapter.second.toUpper();
        }
    }
    const QString pageLabel = strings::get("reader.menu_page_of").arg(page + 1).arg(totalPages);
    const QString label =
            chapterName.isEmpty() ? pageLabel : pageLabel + QStringLiteral(" · ") + chapterName;
    captionBackground->setGeometry(0, panelBounds().top(), width(), scrubber::captionHeight);
    captionBackground->show();
    chapterLabel->setGeometry(captionBackground->geometry());
    const int toggleWidth = zoomAvailable() ? int(metrics::chrome::cellWidth) : 0;
    overviewToggle->setGeometry(
            width() - toggleWidth, captionBackground->y() + int(metrics::chrome::heavyRule),
            toggleWidth,
            int(scrubber::captionHeight - metrics::chrome::heavyRule - metrics::control::border));
    overviewRule->setGeometry(
            overviewToggle->geometry().adjusted(-int(metrics::control::border), 0, 0, 0));
    chapterLabel->setText(
            elideInterfaceText(label, chapterLabel->font(),
                               chapterLabel->width() - 2 * (int(scrubber::padding) + toggleWidth)));
    chapterLabel->setAccessibleName(label);
    chapterLabel->show();
}

void PageScrubber::hideEvent(QHideEvent *)
{
    if (!relocating) {
        cancel();
    }
}

void PageScrubber::beginPointer(QPoint point)
{
    pressPosition = point;
    pointerActive = selecting() && !panelBounds().contains(point);
    pointerMoved = false;
    pressedCard = previewBounds().contains(point)                             ? 0
            : preceding->isVisible() && preceding->geometry().contains(point) ? 1
            : following->isVisible() && following->geometry().contains(point) ? 2
                                                                              : -1;
}

void PageScrubber::movePointer(QPoint point)
{
    if ((point - pressPosition).manhattanLength() > px(12))
        pointerMoved = true;
}

void PageScrubber::release(QPoint point)
{
    if (!pointerActive || !selecting())
        return;
    movePointer(point);
    pointerActive = false;
    const auto delta = point - pressPosition;
    if (pointerMoved) {
        if (qAbs(delta.x()) >= px(40) && qAbs(delta.x()) > qAbs(delta.y()) * 1.5) {
            choose(qBound(0, candidate + (delta.x() < 0 ? 1 : -1), totalPages - 1));
            requestPreview();
        }
        return;
    }
    if (pressedCard == 0 && previewBounds().contains(point)) {
        if (previewReady() && accepted)
            accepted(candidate);
    } else if (pressedCard == 1 && preceding->geometry().contains(point)) {
        preceding->click();
    } else if (pressedCard == 2 && following->geometry().contains(point)) {
        following->click();
    } else if (previewDismissible && !panelBounds().contains(point)) {
        cancel();
    }
}

bool PageScrubber::pointerEvent(QEvent *event, QPoint offset)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove
        || event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (!isPointerMouse(mouse)
            || (event->type() != QEvent::MouseMove && mouse->button() != Qt::LeftButton))
            return false;
        const auto point = mousePosition(mouse).toPoint() + offset;
        if (event->type() == QEvent::MouseButtonPress)
            beginPointer(point);
        else if (event->type() == QEvent::MouseMove && pointerActive)
            movePointer(point);
        else if (event->type() == QEvent::MouseButtonRelease)
            release(point);
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
        || event->type() == QEvent::TouchEnd) {
        const auto points = touchPositions(static_cast<QTouchEvent *>(event));
        // Let child controls own touches inside the navigation panel.
        if (event->type() == QEvent::TouchBegin && points.size() == 1
            && panelBounds().contains(points.first().toPoint() + offset)) {
            event->ignore();
            return false;
        }
        if (points.size() == 1) {
            const auto point = points.first().toPoint() + offset;
            if (event->type() == QEvent::TouchBegin)
                beginPointer(point);
            else if (event->type() == QEvent::TouchEnd)
                release(point);
            else if (pointerActive)
                movePointer(point);
        } else {
            pointerActive = false;
        }
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        pointerActive = false;
        event->accept();
        return true;
    }
    return false;
}

bool PageScrubber::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == preceding || watched == following)
        && pointerEvent(event, static_cast<QWidget *>(watched)->pos()))
        return true;
    return QWidget::eventFilter(watched, event);
}

bool PageScrubber::event(QEvent *event)
{
    return pointerEvent(event) || QWidget::event(event);
}
} // namespace kobalt::ui
