#include "ui/controls/text.h"
#include "modals.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "support/strings.h"
#include <QPainter>
#include <QResizeEvent>
#include "ui/controls/buttons.h"
#include "ui/controls/rows.h"
#include "ui/controls/options.h"
#include "ui/controls/paging.h"
#include "ui/controls/text.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVariant>
#include <algorithm>

namespace kobalt::ui {
PopupCard::PopupCard(QWidget *parent)
    : Panel(parent, Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge)
{
    setEdgeWidth(metrics::notice::cardBorder);
    readerStyle(this, styles::popup());
    contentRows = new QVBoxLayout(this);
    contentRows->setContentsMargins(metrics::notice::cardMargins);
    contentRows->setSpacing(metrics::notice::cardGap);
}

PopupCard::Heading PopupCard::heading(const QString &text, const QString &description,
                                      const QString &titleName, const QString &descriptionName)
{
    contentRows->setContentsMargins(metrics::notice::cardBorder, metrics::notice::cardBorder,
                                    metrics::notice::cardBorder, metrics::notice::cardBorder);
    contentRows->setSpacing(0);
    auto *header = new QWidget(this);
    header->setObjectName("rrModalHeading");
    auto *rows = new QVBoxLayout(header);
    rows->setContentsMargins(metrics::chrome::inset, metrics::notice::cardMargins.top(),
                             metrics::chrome::inset, metrics::notice::cardMargins.bottom());
    rows->setSpacing(metrics::notice::headingGap);
    const auto label = [header, rows](const QString &value, const QString &name, bool bold) {
        auto *result = new InterfaceLabel(value, header);
        result->setObjectName(name);
        result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true);
        result->setBalancedWrapping(true);
        result->setAlignment(Qt::AlignCenter);
        readerStyle(result,
                    styles::font(bold ? int(metrics::chrome::labelText)
                                      : int(metrics::chrome::descriptionText),
                                 bold));
        rows->addWidget(result);
        return result;
    };
    Heading result{ label(text, titleName, true), nullptr };
    if (!description.isEmpty())
        result.description = label(description, descriptionName, false);
    contentRows->addWidget(header);
    return result;
}

void PopupCard::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void PopupCard::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

QLabel *PopupCard::message(const QString &text, const QString &name)
{
    auto *label = new ui::InterfaceLabel(text, this);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setBalancedWrapping(true);
    label->setAlignment(Qt::AlignCenter);
    contentRows->addWidget(label);
    return label;
}

QLabel *PopupCard::title(const QString &text, const QString &name)
{
    auto *label = message(text, name);
    readerStyle(label, styles::font(metrics::chrome::labelText, true));
    return label;
}

QHBoxLayout *PopupCard::actions()
{
    auto *row = horizontalRow();
    row->setSpacing(metrics::notice::cardGap);
    contentRows->addLayout(row);
    return row;
}

void PopupCard::image(const QImage &image)
{
    artworkImage = image;
    if (!artwork) {
        artwork = new ui::InterfaceLabel(this);
        artwork->setAlignment(Qt::AlignCenter);
        contentRows->insertWidget(0, artwork);
    }
    artwork->setVisible(!image.isNull());
}

void PopupCard::adaptiveActions(const QVector<QPushButton *> &buttons)
{
    Q_ASSERT(!actionRows && !buttons.isEmpty() && buttons.size() <= 2);
    actionButtons = buttons;
    actionRows = new QBoxLayout(QBoxLayout::LeftToRight);
    actionRows->setSpacing(0);
    for (auto *button : buttons)
        actionRows->addWidget(button, 1);
    contentRows->addLayout(actionRows);
}

namespace {
class ModalButton final : public Button
{
public:
    ModalButton(const ModalAction &action, bool primary, QWidget *parent)
        : Button(action.text, parent)
    {
        setObjectName(action.name);
        setFocusPolicy(Qt::StrongFocus);
        setModalActionPrimary(this, primary);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void setTopDivider(bool shown)
    {
        if (topDivider == shown)
            return;
        topDivider = shown;
        update();
    }

    QSize sizeHint() const override
    {
        auto size = Button::sizeHint();
        const int vertical = metrics::control::outlinedVerticalPadding + metrics::control::border;
        size.setHeight(
                qMax<int>(metrics::chrome::actionHeight, fontMetrics().height() + 2 * vertical));
        return size;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto paper = palette().color(QPalette::Window);
        const auto ink = palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid);
        painter.fillRect(rect(), isEnabled() && isDown() ? sectionColor(this) : paper);
        auto edges = topDivider ? Edges(Qt::TopEdge) : Edges{};
        if (property("reader_modal_left_rule").toBool())
            edges |= Qt::LeftEdge;
        paintEdges(painter, rect(), edges, dividerColor(this), metrics::control::border);
        painter.setFont(font());
        const int inset = metrics::control::outlinedHorizontalPadding + metrics::control::border;
        painter.setPen(ink);
        drawWrappedInterfaceText(painter,
                                 rect().adjusted(inset, metrics::control::outlinedVerticalPadding,
                                                 -inset,
                                                 -metrics::control::outlinedVerticalPadding),
                                 text(), Qt::AlignCenter);
    }

private:
    bool topDivider = true;
};

PopupCard::Heading modalContent(PopupCard *card, const ModalContent &content)
{
    Q_ASSERT(!content.title.trimmed().isEmpty());
    if (!content.image.isNull())
        card->image(content.image);
    return card->heading(content.title, content.description,
                         content.titleName.isEmpty() ? QStringLiteral("rrModalTitle")
                                                     : content.titleName,
                         "rrModalDescription");
}
} // namespace

void PopupCard::updateListDividers()
{
    // Lists own their bottom rule. The card supplies a top rule only when
    // the preceding visible content does not already close that boundary.
    const auto visit = [](auto &&visit, QBoxLayout *layout, bool preceded,
                          bool previousBottom) -> bool {
        for (int i = 0; i < layout->count(); ++i) {
            auto *widget = layout->itemAt(i)->widget();
            if (!widget || widget->isHidden())
                continue;
            const bool top = preceded && (!previousBottom || layout->spacing() > 0);
            if (auto *row = dynamic_cast<ListRow *>(widget)) {
                row->setTopDivider(top);
                previousBottom = true;
            } else if (auto *list = dynamic_cast<RadioList *>(widget)) {
                list->setTopDivider(top);
                previousBottom = !list->options().isEmpty()
                        && list->heightForWidth(list->width()) == list->height();
            } else if (auto *header = dynamic_cast<ListHeader *>(widget)) {
                header->setTopDivider(top);
                previousBottom = true;
            } else if (auto *pager = dynamic_cast<PagerBar *>(widget)) {
                pager->setTopDivider(top);
                previousBottom = false;
            } else if (auto *nested = dynamic_cast<QBoxLayout *>(widget->layout());
                       nested && nested->direction() == QBoxLayout::TopToBottom) {
                previousBottom = visit(visit, nested, preceded,
                                       previousBottom && layout->spacing() == 0
                                               && nested->contentsMargins().top() == 0);
                if (nested->contentsMargins().bottom() > 0)
                    previousBottom = false;
            } else {
                previousBottom = false;
            }
            preceded = true;
        }
        return previousBottom;
    };
    const bool bottom = visit(visit, contentRows, false, false);
    if (actionRows) {
        int visible = 0;
        for (int i = 0; i < actionRows->count(); ++i) {
            auto *button = dynamic_cast<ModalButton *>(actionRows->itemAt(i)->widget());
            if (!button || button->isHidden())
                continue;
            const bool stacked = actionRows->direction() == QBoxLayout::TopToBottom;
            button->setTopDivider(!bottom || contentRows->spacing() > 0
                                  || (stacked && visible > 0));
            ++visible;
        }
    }
}

void PopupCard::fitToWidth(int width)
{
    setFixedWidth(width);
    const int textWidth =
            width - contentRows->contentsMargins().left() - contentRows->contentsMargins().right();
    if (artwork && !artworkImage.isNull()) {
        const auto size = artworkImage.size().scaled(textWidth, ui::px(180), Qt::KeepAspectRatio);
        artwork->setPixmap(QPixmap::fromImage(
                artworkImage.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        artwork->setFixedHeight(size.height());
    }
    if (actionRows) {
        QVector<QPushButton *> visible;
        int longest = 0;
        for (auto *button : actionButtons) {
            if (button->isHidden())
                continue;
            visible.append(button);
            button->ensurePolished();
            longest = qMax(longest,
                           button->fontMetrics().boundingRect(button->text()).width()
                                   + 2
                                           * int(metrics::control::outlinedHorizontalPadding
                                                 + metrics::control::border));
        }
        bool stacked = visible.size() == 2 && 2 * longest + actionRows->spacing() > textWidth;
        if (visible.size() == 2 && !stacked) {
            const int labelWidth = (textWidth - actionRows->spacing()) / 2
                    - 2
                            * int(metrics::control::outlinedHorizontalPadding
                                  + metrics::control::border);
            for (auto *button : visible) {
                const auto bounds = button->fontMetrics().boundingRect(
                        QRect(0, 0, qMax(1, labelWidth), 10000), Qt::TextWordWrap, button->text());
                if (bounds.height() > button->fontMetrics().height())
                    stacked = true;
            }
        }
        while (auto *item = actionRows->takeAt(0))
            delete item;
        actionRows->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        if (stacked)
            std::reverse(visible.begin(), visible.end());
        for (int index = 0; index < visible.size(); ++index) {
            auto *button = visible[index];
            button->setProperty("reader_modal_left_rule", !stacked && index > 0);
            actionRows->addWidget(button, 1);
        }
        const int buttonWidth = visible.isEmpty() ? textWidth
                : stacked
                ? textWidth
                : (textWidth - (visible.size() - 1) * actionRows->spacing()) / visible.size();
        for (auto *button : visible) {
            const int horizontal =
                    metrics::control::outlinedHorizontalPadding + metrics::control::border;
            const int vertical =
                    metrics::control::outlinedVerticalPadding + metrics::control::border;
            const auto bounds = button->fontMetrics().boundingRect(
                    QRect(0, 0, qMax(1, buttonWidth - 2 * horizontal), 10000), Qt::TextWordWrap,
                    button->text());
            button->setFixedHeight(
                    qMax<int>(metrics::chrome::actionHeight, bounds.height() + 2 * vertical));
        }
    }
    for (auto *label : findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (label == artwork)
            continue;
        label->ensurePolished();
        // Clear the previous height before measuring. heightForWidth respects
        // that minimum and otherwise keeps old wrapping after a resize.
        label->setMinimumHeight(0);
        label->setMaximumHeight(QWIDGETSIZE_MAX);
        label->setFixedHeight(qMax(0, label->heightForWidth(textWidth)));
    }
    // A changed label invalidates its own layout immediately, but the parent
    // layout still caches the heading widget's height until LayoutRequest is
    // delivered. fitCard() can run in that same callback, so invalidate the
    // enclosing widget measurements before asking for the card's height.
    for (auto *widget : findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (widget->layout())
            widget->updateGeometry();
    // Measure nested wrapped content at the fixed width. adjustSize() uses
    // the layout's natural width and can leave too little height for a footer.
    const int height = contentRows->hasHeightForWidth() ? contentRows->totalHeightForWidth(width)
                                                        : contentRows->totalSizeHint().height();
    resize(width, height);
    contentRows->activate();
    updateListDividers();
}

void PopupCard::fitToHeight(int height)
{
    const auto &previousRows = pagedRows;
    for (const auto &row : previousRows)
        if (row)
            row->show();
    pagedRows.clear();
    rowPages.clear();
    if (rowPager)
        rowPager->hide();
    const auto measure = [this] {
        return contentRows->hasHeightForWidth() ? contentRows->totalHeightForWidth(width())
                                                : contentRows->totalSizeHint().height();
    };
    const int natural = measure();
    if (natural <= height) {
        resize(width(), natural);
        contentRows->activate();
        updateListDividers();
        return;
    }
    if (!rowPager) {
        const auto owner = parentWidget() ? parentWidget()->objectName() : QString();
        rowPager = new PagerBar(this, owner.isEmpty() ? QStringLiteral("rrCard") : owner);
        rowPager->setFixedHeight(metrics::chrome::actionHeight);
        const auto turn = [this](int direction) {
            showRowPage(rowPager->turnTarget(direction));
            if (auto *overlay = dynamic_cast<ModalOverlay *>(parentWidget()))
                overlay->centerCard();
        };
        connect(rowPager->previous, &QPushButton::clicked, this, [turn] { turn(-1); });
        connect(rowPager->next, &QPushButton::clicked, this, [turn] { turn(1); });
    }
    // The pager sits above the actions, or last when the card has none.
    contentRows->removeWidget(rowPager);
    int actionsAt = -1;
    for (int index = 0; actionRows && index < contentRows->count(); ++index)
        if (contentRows->itemAt(index) == actionRows) {
            actionsAt = index;
            break;
        }
    contentRows->insertWidget(actionsAt < 0 ? contentRows->count() : actionsAt, rowPager);
    const int textWidth = width() - contentRows->contentsMargins().left()
            - contentRows->contentsMargins().right();
    const auto rowHeight = [textWidth](QWidget *row) {
        return row->hasHeightForWidth() ? row->heightForWidth(textWidth) : row->sizeHint().height();
    };
    QVector<QWidget *> rows;
    int pagedHeight = 0;
    for (int index = 1; index < contentRows->count(); ++index) {
        auto *row = contentRows->itemAt(index)->widget();
        if (!row || row == rowPager || row->isHidden())
            continue;
        rows.append(row);
        pagedHeight += rowHeight(row) + contentRows->spacing();
    }
    const int room = qMax(1,
                          height - (natural - pagedHeight) - int(metrics::chrome::actionHeight)
                                  - contentRows->spacing());
    int used = 0;
    for (auto *row : rows) {
        const int needed = rowHeight(row) + contentRows->spacing();
        if (rowPages.isEmpty() || (used && used + needed > room)) {
            rowPages.append(QVector<QWidget *>());
            used = 0;
        }
        rowPages.last().append(row);
        used += needed;
        pagedRows.append(row);
    }
    rowPager->show();
    showRowPage(rowPage);
}

void PopupCard::showRowPage(int page)
{
    if (rowPages.isEmpty())
        return;
    rowPage = qBound(0, page, int(rowPages.size()) - 1);
    for (int index = 0; index < rowPages.size(); ++index) {
        const auto &rows = rowPages[index];
        for (auto *row : rows)
            row->setVisible(index == rowPage);
    }
    rowPager->setPage(rowPage, rowPages.size());
    const int height = contentRows->hasHeightForWidth() ? contentRows->totalHeightForWidth(width())
                                                        : contentRows->totalSizeHint().height();
    resize(width(), height);
    // Place the page's rows now, so they take touches where they are drawn.
    contentRows->activate();
    updateListDividers();
}

namespace {
class AnchorPointer final : public QWidget
{
public:
    explicit AnchorPointer(QWidget *parent) : QWidget(parent)
    {
        setObjectName("rrAnchorPointer");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool below = property("card_below").toBool();
        const qreal base = below ? height() : 0;
        const qreal tip = below ? 1 : height() - 1;
        QPolygonF triangle;
        if (property("card_left").toBool())
            triangle << QPointF(0, 1) << QPointF(width() - 1, height() / 2.0)
                     << QPointF(0, height() - 1);
        else
            triangle << QPointF(1, base) << QPointF(width() / 2.0, tip)
                     << QPointF(width() - 1, base);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawPolygon(triangle);
        painter.setPen(QPen(palette().color(QPalette::WindowText), 2));
        painter.drawPolyline(triangle);
    }
};
} // namespace

ButtonFeedback::ButtonFeedback(QWidget *parent) : ModalOverlay(parent), hideTimer(this)
{
    setInputBarrier(false);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    label = card()->message({}, "rrButtonFeedbackText");
    readerStyle(label, styles::font(metrics::chrome::labelText));
    hideTimer.setSingleShot(true);
    hideTimer.setInterval(1500);
    connect(&hideTimer, &QTimer::timeout, this, &QWidget::hide);
    hide();
}

void ButtonFeedback::showFor(QWidget *button, const QString &text, const Placement &placement)
{
    if (!button || !button->isVisible())
        return;
    target = button;
    this->placement = placement;
    label->setText(text);
    setGeometry(parentWidget()->rect());
    reposition();
    show();
    raise();
    hideTimer.start();
}

void ButtonFeedback::reposition()
{
    if (!target)
        return;
    const auto margins = card()->rows()->contentsMargins();
    const int wanted =
            interfaceTextWidth(label->text(), label->font()) + margins.left() + margins.right();
    card()->fitToWidth(qMin<int>(wanted, width() - 2 * metrics::notice::sideClearance));
    const QRect area = placement.area.isEmpty() ? target->rect() : placement.area;
    QRect bounds(mapFromParent(target->mapTo(parentWidget(), area.topLeft())), area.size());
    // Overlap the bar with the card's border as well as its triangle.
    const int overlap = qMin(px(24), bounds.height() / 4);
    if (placement.side != AnchorSide::Left)
        bounds.adjust(0, overlap, 0, -overlap);
    anchorTo(bounds, placement.side, placement.sideOffset);
}

void ButtonFeedback::hideEvent(QHideEvent *event)
{
    hideTimer.stop();
    ModalOverlay::hideEvent(event);
}

ModalOverlay::ModalOverlay(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrModalBackdrop");
    setProperty("reader_popup_overlay", true);
    // Override the reader's general QWidget background, not its child controls.
    readerStyle(this, "QWidget[reader_popup_overlay=\"true\"]{background:transparent;}");
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_NoMousePropagation);
    content = new PopupCard(this);
    content->setObjectName("rrModalCard");
    content->installEventFilter(this);
    pointer = new AnchorPointer(this);
    pointer->hide();
    parent->installEventFilter(this);
    setGeometry(parent->rect());
    hide();
}

void ModalOverlay::setInputBarrier(bool enabled)
{
    if (blocksInput == enabled) {
        return;
    }
    blocksInput = enabled;
    updateInputRegion();
    update();
}

void ModalOverlay::setVisible(bool visible)
{
    if (visible) {
        updateInputRegion();
    } else if (blocksInput && !inputExclusions.isEmpty()) {
        // Circular grip cutouts split the exposed page into hundreds of strips.
        // Remove them before Qt computes hide damage, then restore them on show.
        clearMask();
    }
    QWidget::setVisible(visible);
}

void ModalOverlay::setInputExclusions(const QRegion &region)
{
    if (inputExclusions == region)
        return;
    inputExclusions = region;
    updateInputRegion();
}

void ModalOverlay::setDimmed(bool enabled)
{
    if (dimmed == enabled) {
        return;
    }
    dimmed = enabled;
    update();
}

void ModalOverlay::paintEvent(QPaintEvent *)
{
    if (dimmed && blocksInput) {
        QPainter painter(this);
        const auto &colors = themeColors(this);
        painter.fillRect(rect(), colors.modalFade);
        QPen lines(colors.modalPattern);
        // Keep diagonal strokes one output pixel wide at Native resolution.
        lines.setWidth(1);
        lines.setCosmetic(true);
        painter.setPen(lines);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const int pitch = qMax(1, int(metrics::notice::backdropPitch));
        for (int x = -height(); x < width(); x += pitch)
            painter.drawLine(x, 0, x + height(), height());
    }
}

void ModalOverlay::updateInputRegion()
{
    if (blocksInput && inputExclusions.isEmpty()) {
        clearMask();
        return;
    }
    QRegion region = blocksInput ? QRegion(rect()).subtracted(inputExclusions) : QRegion();
    // The card and arrow retain priority if a handle hit area reaches them.
    region += content->geometry();
    if (!pointer->isHidden()) {
        region += pointer->geometry();
    }
    setMask(region);
}

void ModalOverlay::centerCard()
{
    if (anchor.isEmpty()) {
        // A card taller than the screen pages its rows instead. Cards placed
        // above a keyboard keep their own fitting, and their input stays shown.
        const int room = height() - 2 * int(metrics::notice::sideClearance);
        // Long wrapped descriptions can exceed one page. Use spare landscape
        // width before paging rows, since a single row cannot be split.
        const int availableWidth = width() - 2 * int(metrics::notice::sideClearance);
        if (cardArea.isEmpty() && room > 0 && content->height() > room && width() > height()
            && content->width() < availableWidth)
            content->fitToWidth(availableWidth);
        if (cardArea.isEmpty() && room > 0
            && (content->height() > room || content->property("card_paged").toBool())) {
            content->setProperty("card_paged", true);
            content->fitToHeight(room);
        }
        pointer->hide();
        content->move((cardArea.isEmpty() ? rect() : cardArea).center() - content->rect().center());
        updateInputRegion();
        return;
    }
    const int gap = ui::px(16), inset = ui::px(24);
    const int half = ui::px(16);
    const int left = anchor.left() - content->width() - gap;
    if (anchorSide == AnchorSide::Left && left >= inset) {
        const int x = qBound(inset, left + anchorOffset.x(), anchor.left() - content->width() - 4);
        const int y = qBound(inset, anchor.center().y() - content->height() / 2 + anchorOffset.y(),
                             qMax(inset, height() - content->height() - inset));
        content->move(x, y);
        const int center =
                qBound(y + half + 2, anchor.center().y(), content->geometry().bottom() - half - 2);
        pointer->setProperty("card_left", true);
        const int base = content->geometry().right() - 1;
        pointer->setGeometry(base, center - half, anchor.left() - base, 2 * half);
        pointer->show();
        pointer->raise();
        pointer->update();
        updateInputRegion();
        return;
    }
    pointer->setProperty("card_left", false);
    const int x = qBound(inset, anchor.center().x() - content->width() / 2,
                         qMax(inset, width() - content->width() - inset));
    const int below = anchor.bottom() + gap;
    const int above = anchor.top() - content->height() - gap;
    const int y = anchorSide != AnchorSide::Below && above >= inset ? above
            : below + content->height() <= height() - inset         ? below
                                                                    : above;
    content->move(x, qBound(inset, y, qMax(inset, height() - content->height() - inset)));
    const bool cardBelow = content->y() > anchor.center().y();
    const int center = qBound(content->x() + half + 2, anchor.center().x(),
                              content->geometry().right() - half - 2);
    pointer->setProperty("card_below", cardBelow);
    pointer->setGeometry(center - half,
                         cardBelow ? content->y() - gap : content->geometry().bottom() - 1,
                         2 * half, gap + 2);
    pointer->show();
    pointer->raise();
    pointer->update();
    updateInputRegion();
}

bool ModalOverlay::eventFilter(QObject *object, QEvent *event)
{
    if (object == parentWidget() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    if (object == content && event->type() == QEvent::Resize)
        centerCard();
    return QWidget::eventFilter(object, event);
}

void ModalOverlay::fitCard()
{
    content->fitToWidth(qMin<int>(metrics::notice::cardWidth,
                                  qMax(1, width() - 2 * metrics::notice::sideClearance)));
    centerCard();
}

QRect ModalOverlay::progressUpdateRect(int percent, const QString &message) const
{
    if (!loadingProgress || !isVisible() || !loadingProgress->isVisible()
        || loadingProgress->value() == qBound(0, percent, 100)
        || (!message.isEmpty() && loadingMessage->text() != message)) {
        return {};
    }
    return QRect(loadingProgress->mapTo(window(), QPoint()), loadingProgress->size());
}

void ModalOverlay::setDelayedProgress(bool active, qint64 elapsedMilliseconds, int percent,
                                      const QString &message)
{
    if (!loadingProgress) {
        loadingMessage = content->findChild<QLabel *>("rrUpdatingText");
        if (!loadingMessage)
            loadingMessage = content->message(message, "rrUpdatingText");
        loadingProgress = new ProgressBar(content);
        loadingProgress->setObjectName("rrApplyingProgress");
        content->rows()->addWidget(loadingProgress);
        loadingProgress->hide();
        setProperty("reader_standard_modal", true);
    }
    const bool showBar = active && elapsedMilliseconds >= 1000;
    const bool wasVisible = isVisible();
    const bool changed = loadingProgress->isHidden() == showBar
            || (!message.isEmpty() && loadingMessage->text() != message);
    if (!message.isEmpty())
        loadingMessage->setText(message);
    // Hide before resetting so dismissal paints only the restored page.
    if (!showBar) {
        hide();
        loadingProgress->hide();
    }
    const int value = active ? qBound(0, percent, 100) : 0;
    loadingProgress->setValue(value);
    loadingProgress->setVisible(showBar);
    if (changed)
        fitCard();
    setVisible(showBar);
    if (isVisible() && !wasVisible)
        raise();
}

void ModalOverlay::resizeEvent(QResizeEvent *)
{
    if (property("reader_standard_modal").toBool())
        fitCard();
    else
        centerCard();
}

void ModalOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && dismissed)
        dismissed();
    event->accept();
}

void ModalOverlay::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

// The card accepts its own taps, so only a tap on the backdrop arrives here.
void ModalOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (dismissed)
        dismissed();
    event->accept();
}

bool ModalOverlay::focusNextPrevChild(bool next)
{
    auto *candidate = focusWidget();
    if (!candidate)
        candidate = this;
    auto *start = candidate;
    do {
        candidate = next ? candidate->nextInFocusChain() : candidate->previousInFocusChain();
        if (candidate != this && isAncestorOf(candidate) && candidate->isVisible()
            && candidate->isEnabled() && (candidate->focusPolicy() & Qt::TabFocus)) {
            candidate->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            return true;
        }
    } while (candidate != start);
    return true;
}

void setModalActionPrimary(QPushButton *button, bool primary)
{
    button->setProperty("reader_primary_action", primary);
    readerStyle(button,
                "QPushButton{border:none;padding:0;min-height:0;"
                        + styles::font(metrics::chrome::labelText, primary) + "}");
}

QPushButton *makeModalButton(QWidget *parent, const ModalAction &action, bool primary)
{
    return new ModalButton(action, primary, parent);
}

NoticeModal::NoticeModal(QWidget *parent, const ModalContent &content, const ModalAction &action)
    : ModalOverlay(parent)
{
    setDimmed(true);
    const auto heading = modalContent(card(), content);
    title = heading.title;
    description = heading.description;
    primary = makeModalButton(card(), action, true);
    card()->adaptiveActions({ primary });
    setProperty("reader_standard_modal", true);
    fitCard();
}

ChoiceModal::ChoiceModal(QWidget *parent, const ModalContent &content,
                         const ModalAction &secondaryAction, const ModalAction &primaryAction)
    : ModalOverlay(parent)
{
    setDimmed(true);
    const auto heading = modalContent(card(), content);
    title = heading.title;
    description = heading.description;
    secondary = makeModalButton(card(), secondaryAction, false);
    primary = makeModalButton(card(), primaryAction, true);
    card()->adaptiveActions({ secondary, primary });
    setProperty("reader_standard_modal", true);
    fitCard();
}

PopupWindow::PopupWindow(QWidget *source, bool dark)
    : QDialog(source, Qt::Dialog | Qt::FramelessWindowHint)
{
    if (source)
        background = source->grab();
    setWindowModality(Qt::ApplicationModal);
    setProperty("reader_dark", dark);
    const auto colors = readerPalette(palette(), dark);
    setPalette(colors);
    setFont(interfaceFont());
    readerStyle(this, styles::reader());
    overlay = new ModalOverlay(this);
    overlay->show();
}

void PopupWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (!background.isNull())
        painter.drawPixmap(rect(), background);
}
} // namespace kobalt::ui
