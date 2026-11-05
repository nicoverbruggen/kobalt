#include "ui/controls/text.h"
#include "paging.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "support/strings.h"
#include "ui/input/input.h"
#include "sliders.h"
#include "page_scrubber.h"
#include <QApplication>
#include <QAbstractSlider>
#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPainter>
#include <QResizeEvent>

namespace kobalt::ui {
bool turnVisiblePageControl(QWidget *scope, int direction)
{
    if (!scope || !scope->isVisible() || !scope->isEnabled() || (direction != -1 && direction != 1))
        return false;
    QWidget *selected = nullptr;
    int deepest = -1;
    auto widgets = scope->findChildren<QWidget *>();
    widgets.prepend(scope);
    for (auto *widget : widgets) {
        if (!widget->isVisible()
            || (!dynamic_cast<PagerBar *>(widget) && !dynamic_cast<PageControls *>(widget)))
            continue;
        int depth = 0;
        for (auto *parent = widget; parent && parent != scope; parent = parent->parentWidget())
            ++depth;
        if (depth >= deepest) {
            selected = widget;
            deepest = depth;
        }
    }
    QPushButton *button = nullptr;
    if (auto *pager = dynamic_cast<PagerBar *>(selected))
        button = direction > 0 ? pager->next : pager->previous;
    else if (auto *pager = dynamic_cast<PageControls *>(selected))
        button = direction > 0 ? pager->next : pager->previous;
    if (button && button->isVisible() && button->isEnabled())
        button->click();
    return selected;
}

namespace {
// Observe descendant events too: a swipe normally starts on a row button,
// not on the panel behind it. New rows need no separate registration.
class PageSwipes final : public QObject
{
public:
    explicit PageSwipes(QWidget *pager) : QObject(pager), pager(pager)
    {
        qApp->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        // An application-wide filter: check the event type first.
        switch (event->type()) {
        case QEvent::TouchCancel:
        case QEvent::Hide:
        case QEvent::WindowDeactivate:
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
            break;
        default:
            return false;
        }
        auto *widget = qobject_cast<QWidget *>(object);
        if (!widget)
            return false;
        if ((event->type() == QEvent::TouchCancel || event->type() == QEvent::Hide
             || event->type() == QEvent::WindowDeactivate)
            && (widget == target || widget == pager->parentWidget())) {
            target.clear();
            return false;
        }
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove
            && event->type() != QEvent::MouseButtonRelease)
            return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (event->type() == QEvent::MouseButtonPress) {
            if (mouse->button() != Qt::LeftButton) {
                target.clear();
                return false;
            }
            if (!eligible(widget))
                return false;
            target = widget;
            start = mousePosition(mouse).toPoint();
            timestamp = quint32(mouse->timestamp());
            elapsed.start();
            return false;
        }
        if (widget != target || !target)
            return false;
        const QPoint delta = mousePosition(mouse).toPoint() - start;
        const bool swipe = qAbs(delta.x()) > 4 * QApplication::startDragDistance()
                && qAbs(delta.x()) > 2 * qAbs(delta.y());
        if (event->type() != QEvent::MouseButtonRelease)
            return false;
        auto *receiver = target.data();
        target.clear();
        // A hold may already have opened a menu. It must not also turn a page,
        // including when queued input arrives before the hold timer fires.
        const quint32 duration = quint32(mouse->timestamp()) - timestamp;
        const bool held = receiver->property("reader_swipe_hold").toBool()
                && (elapsed.elapsed() >= metrics::input::longPressMilliseconds
                    || (timestamp && duration >= metrics::input::longPressMilliseconds));
        if (mouse->button() != Qt::LeftButton || !swipe || !eligible(receiver))
            return false;
        QPointer<QWidget> alive = pager;
        if (auto *button = qobject_cast<QAbstractButton *>(receiver))
            button->setDown(false);
        QTouchEvent cancel(QEvent::TouchCancel);
        QApplication::sendEvent(receiver, &cancel);
        if (alive && !held)
            turnVisiblePageControl(pager->parentWidget(), delta.x() < 0 ? 1 : -1);
        mouse->accept();
        return true;
    }

private:
    bool eligible(QWidget *widget) const
    {
        if (!pager->isVisible() || !pager->isEnabled())
            return false;
        auto *scope = pager->parentWidget();
        if (!scope || (widget != scope && !scope->isAncestorOf(widget)))
            return false;
        for (auto *ancestor = widget; ancestor; ancestor = ancestor->parentWidget()) {
            if (ancestor == pager || ancestor->property("reader_owns_swipes").toBool()
                || qobject_cast<QAbstractSlider *>(ancestor) || qobject_cast<QLineEdit *>(ancestor)
                || qobject_cast<QAbstractScrollArea *>(ancestor)
                || dynamic_cast<RangeOverview *>(ancestor)
                || dynamic_cast<PageScrubber *>(ancestor))
                return false;
            // The innermost paginated panel owns the gesture.
            for (auto *child :
                 ancestor->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
                if (child != pager && child->isVisible()
                    && (dynamic_cast<PagerBar *>(child) || dynamic_cast<PageControls *>(child)))
                    return false;
            if (ancestor == scope)
                return true;
        }
        return false;
    }

    QWidget *pager;
    QPointer<QWidget> target;
    QPoint start;
    quint32 timestamp = 0;
    QElapsedTimer elapsed;
};

class ContinuationHint final : public InterfaceLabel
{
public:
    using InterfaceLabel::InterfaceLabel;

    QSize sizeHint() const override
    {
        const auto label = InterfaceLabel::sizeHint();
        return { label.width() + px(12) + px(28), qMax(label.height(), int(px(28))) };
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setFont(font());
        const auto ink = palette().color(QPalette::WindowText);
        painter.setPen(ink);
        const int textWidth = interfaceTextWidth(text(), font()), gap = px(12), icon = px(28);
        const int left = (width() - textWidth - gap - icon) / 2;
        drawInterfaceText(painter, QRect(left, 0, textWidth, height()), text(),
                          Qt::AlignLeft | Qt::AlignVCenter);
        paintOpeningIcon(painter, ControlIcon::ArrowDownRight,
                         QRect(left + textWidth + gap, (height() - icon) / 2, icon, icon), ink);
    }
};
} // namespace

PagerBar::PagerBar(QWidget *parent, const QString &prefix) : QWidget(parent)
{
    new PageSwipes(this);
    setAttribute(Qt::WA_OpaquePaintEvent);
    counter = new ui::InterfaceLabel(this);
    counter->setObjectName(prefix + "Page");
    counter->setAlignment(Qt::AlignCenter);
    readerStyle(counter, styles::font(metrics::chrome::countText) + "background:transparent;");
    previous = new CellButton(ControlIcon::Previous, this);
    previous->setObjectName(prefix + "Previous");
    next = new CellButton(ControlIcon::Next, this);
    next->setObjectName(prefix + "Next");
    previous->setAccessibleName(strings::get("common.previous_page"));
    next->setAccessibleName(strings::get("common.next_page"));
    previous->setRule(Qt::RightEdge);
    next->setRule(Qt::LeftEdge);
    setFixedHeight(metrics::chrome::actionHeight);
    continuationHint =
            new ContinuationHint(strings::get("common.more_next_page"), parent ? parent : this);
    continuationHint->setObjectName(prefix + "ContinuationHint");
    continuationHint->setTextFormat(Qt::PlainText);
    continuationHint->setAlignment(Qt::AlignCenter);
    continuationHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    readerStyle(
            continuationHint,
            styles::font(metrics::chrome::descriptionText)
                    + "font-style:italic;color:palette(placeholder-text);background:transparent;");
    continuationHint->hide();
}

PagerBar::~PagerBar()
{
    delete continuationHint.data();
}

void PagerBar::setBlankArea(const QRect &bounds)
{
    blankArea = bounds;
    updateHint();
}

void PagerBar::updateHint()
{
    const auto hint = continuationHint->sizeHint();
    const int minimumHeight = qMax(ui::px(48), hint.height() + 2 * ui::px(8));
    const bool fits = blankArea.height() >= minimumHeight && blankArea.width() >= hint.width();
    if (page + 1 >= count || !fits) {
        // Hide at the old bounds so Qt5 invalidates every painted hint pixel.
        // Resizing to an empty blank area first can leave those pixels behind.
        continuationHint->hide();
        return;
    }
    continuationHint->setGeometry(
            { blankArea.topLeft(),
              QSize(qMax(0, blankArea.width()), qMax(0, blankArea.height())) });
    continuationHint->show();
    continuationHint->raise();
}

void PagerBar::resizeEvent(QResizeEvent *)
{
    const int top = metrics::control::border, cell = metrics::chrome::cellWidth;
    previous->setGeometry(0, top, cell, height() - top);
    next->setGeometry(width() - cell, top, cell, height() - top);
    counter->setGeometry(cell, top, qMax(1, width() - 2 * cell), height() - top);
}

void PagerBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    painter.fillRect(0, 0, width(), metrics::control::border, dividerColor(this));
}

void PagerBar::setPage(int page, int count, const QString &label, bool wrap)
{
    count = qMax(1, count);
    page = qBound(0, page, count - 1);
    counter->setText(label.isEmpty()
                             ? strings::get("navigation.page_count").arg(page + 1).arg(count)
                             : label);
    this->page = page;
    this->count = count;
    this->wrap = wrap;
    blankArea = {};
    updateHint();
    previous->setEnabled(count > 1 && (wrap || page > 0));
    next->setEnabled(count > 1 && (wrap || page + 1 < count));
    setVisible(count > 1);
}

int PagerBar::turnTarget(int direction) const
{
    if (count <= 1 || direction == 0) {
        return page;
    }
    const int target = page + (direction > 0 ? 1 : -1);
    if (wrap) {
        return target < 0 ? count - 1 : target >= count ? 0 : target;
    }
    return qBound(0, target, count - 1);
}

PageControls::PageControls(QWidget *parent, const QString &prefix, bool compact)
    : QWidget(parent), compact(compact)
{
    new PageSwipes(this);
    // The enclosing panel owns the background and perimeter. A full-width
    // pager must not paint over its dropdown's left and right borders.
    setProperty("reader_paging", true);
    readerStyle(this, "QWidget[reader_paging=\"true\"]{background:transparent;}");
    counter = new ui::InterfaceLabel(this);
    counter->setObjectName(prefix + "Page");
    counter->setAlignment(Qt::AlignCenter);
    previous = new Button(this);
    previous->setObjectName(prefix + "Previous");
    next = new Button(this);
    next->setObjectName(prefix + "Next");
    iconAction(previous, ControlIcon::Previous, strings::get("common.previous_page"));
    iconAction(next, ControlIcon::Next, strings::get("common.next_page"));
    for (auto *button : { previous, next })
        button->setFocusPolicy(Qt::NoFocus);
    setFixedHeight(metrics::navigation::height);
}

void PageControls::setCompact(bool value)
{
    compact = value;
    arrange();
}

void PageControls::resizeEvent(QResizeEvent *)
{
    arrange();
}

void PageControls::arrange()
{
    const int margin = compact ? metrics::navigation::compactMargin : metrics::navigation::margin;
    const int buttonWidth =
            compact ? metrics::navigation::compactButtonWidth : metrics::navigation::buttonWidth;
    const int side = margin + buttonWidth + metrics::control::itemGap;
    previous->setGeometry(margin, 0, buttonWidth, height());
    next->setGeometry(width() - margin - buttonWidth, 0, buttonWidth, height());
    counter->setGeometry(side, 0, qMax(1, width() - 2 * side), height());
}

void PageControls::setPage(int page, int count)
{
    count = qMax(1, count);
    page = qBound(0, page, count - 1);
    counter->setText(strings::get("common.page_fraction").arg(page + 1).arg(count));
    previous->setEnabled(page > 0);
    next->setEnabled(page + 1 < count);
}

PageNavigation::PageNavigation(QWidget *parent, const QString &prefix)
{
    title = new PrivateTextLabel(parent);
    title->setObjectName(prefix + "Title");
    title->setAlignment(Qt::AlignCenter);
    title->setTextFormat(Qt::PlainText);
    readerStyle(title, styles::font(metrics::text::pageHeading, true));
    back = new BackButton(strings::get("common.back"), parent);
    back->setObjectName(prefix + "Back");
    back->setFocusPolicy(Qt::NoFocus);
    paging = new PageControls(parent, prefix);
    counter = paging->counter;
    previous = paging->previous;
    next = paging->next;
}

void PageNavigation::layout(const QSize &size, bool compact)
{
    const int width = size.width(), height = size.height();
    title->setVisible(!compact);
    back->setVisible(!compact);
    title->setGeometry(metrics::navigation::headingSide, metrics::navigation::headingTop,
                       qMax(1, width - 2 * metrics::navigation::headingSide),
                       metrics::navigation::height);
    back->setGeometry(metrics::navigation::backLeft, metrics::navigation::headingTop,
                      metrics::navigation::backWidth, metrics::navigation::height);
    const int y =
            height - (compact ? metrics::navigation::compactBottom : metrics::navigation::bottom);
    paging->setGeometry(0, y, width, metrics::navigation::height);
    paging->setCompact(compact);
}

void PageNavigation::setPage(int page, int count)
{
    paging->setPage(page, count);
}

void PageNavigation::setPagingVisible(bool visible)
{
    paging->setVisible(visible);
}
} // namespace kobalt::ui
