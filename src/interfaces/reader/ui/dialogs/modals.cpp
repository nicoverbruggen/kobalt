#include "modals.h"
#include "../style/metrics.h"
#include "../style/styles.h"
#include "../style/fonts.h"
#include "support/strings.h"
#include <QPainter>
#include <QResizeEvent>
#include "../controls/buttons.h"
#include "../controls/rows.h"
#include "../controls/text.h"
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVariant>
#include <algorithm>

namespace kobalt::ui {
namespace m = metrics;
PopupCard::PopupCard(QWidget *parent)
    : Panel(parent, Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge) {
    setEdgeWidth(2);
    readerStyle(this, styles::popup());
    contentRows = new QVBoxLayout(this);
    contentRows->setContentsMargins(m::notice::cardMargins);
    contentRows->setSpacing(m::notice::cardGap);
}
QLabel *PopupCard::message(const QString &text, const QString &name) {
    auto *label = new QLabel(text, this);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    contentRows->addWidget(label);
    return label;
}
QLabel *PopupCard::title(const QString &text, const QString &name) {
    auto *label = message(text, name);
    readerStyle(label, styles::font(m::chrome::labelText, true));
    return label;
}
QHBoxLayout *PopupCard::actions() {
    auto *row = horizontalRow();
    row->setSpacing(m::notice::cardGap);
    contentRows->addLayout(row);
    return row;
}
void PopupCard::image(const QImage &image) {
    artworkImage = image;
    if (!artwork) {
        artwork = new QLabel(this);
        artwork->setAlignment(Qt::AlignCenter);
        contentRows->insertWidget(0, artwork);
    }
    artwork->setVisible(!image.isNull());
}
void PopupCard::adaptiveActions(const QVector<QPushButton *> &buttons) {
    Q_ASSERT(!actionRows && !buttons.isEmpty() && buttons.size() <= 2);
    actionButtons = buttons;
    actionRows = new QBoxLayout(QBoxLayout::LeftToRight);
    actionRows->setSpacing(m::notice::cardGap);
    for (auto *button : buttons)
        actionRows->addWidget(button, 1);
    contentRows->addLayout(actionRows);
}
namespace {
class ModalButton final : public Button {
public:
    ModalButton(const ModalAction &action, bool primary, QWidget *parent) : Button(action.text, parent) {
        setObjectName(action.name);
        setFocusPolicy(Qt::StrongFocus);
        setProperty("reader_primary_action", primary);
        readerStyle(this, (primary ? styles::primary() : styles::outlined()) +
                              QString("QPushButton{%1}").arg(styles::font(m::chrome::labelText)));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text.clear();
        painter.drawControl(QStyle::CE_PushButton, option);
        painter.setFont(font());
        const int inset = m::control::outlinedHorizontalPadding + m::control::border;
        const auto colors = parentWidget()->palette();
        const auto role =
            property("reader_primary_action").toBool() && !isDown() ? QPalette::Window : QPalette::WindowText;
        painter.setPen(isEnabled() ? colors.color(role) : QColor("#999999"));
        painter.drawText(
            rect().adjusted(inset, m::control::outlinedVerticalPadding, -inset, -m::control::outlinedVerticalPadding),
            Qt::AlignCenter | Qt::TextWordWrap, text());
    }
};
QLabel *modalContent(PopupCard *card, const ModalContent &content) {
    Q_ASSERT(!content.title.trimmed().isEmpty());
    if (!content.image.isNull())
        card->image(content.image);
    card->title(content.title, "rrModalTitle")->setTextFormat(Qt::PlainText);
    if (content.description.isEmpty())
        return nullptr;
    auto *description = card->message(content.description, "rrModalDescription");
    description->setTextFormat(Qt::PlainText);
    return description;
}
}
void PopupCard::fitToWidth(int width) {
    setFixedWidth(width);
    const int textWidth = width - contentRows->contentsMargins().left() - contentRows->contentsMargins().right();
    if (artwork && !artworkImage.isNull()) {
        const auto size = artworkImage.size().scaled(textWidth, ui::px(180), Qt::KeepAspectRatio);
        artwork->setPixmap(
            QPixmap::fromImage(artworkImage.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
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
            longest = qMax(longest, button->fontMetrics().boundingRect(button->text()).width() +
                                        2 * int(m::control::outlinedHorizontalPadding + m::control::border));
        }
        bool stacked = visible.size() == 2 && 2 * longest + actionRows->spacing() > textWidth;
        if (visible.size() == 2 && !stacked) {
            const int labelWidth = (textWidth - actionRows->spacing()) / 2 -
                                   2 * int(m::control::outlinedHorizontalPadding + m::control::border);
            for (auto *button : visible) {
                const auto bounds = button->fontMetrics().boundingRect(QRect(0, 0, qMax(1, labelWidth), 10000),
                                                                       Qt::TextWordWrap, button->text());
                if (bounds.height() > button->fontMetrics().height())
                    stacked = true;
            }
        }
        while (auto *item = actionRows->takeAt(0))
            delete item;
        actionRows->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        if (stacked)
            std::reverse(visible.begin(), visible.end());
        for (auto *button : visible)
            actionRows->addWidget(button, 1);
        const int buttonWidth = visible.isEmpty() ? textWidth
                                : stacked         ? textWidth
                                          : (textWidth - (visible.size() - 1) * actionRows->spacing()) / visible.size();
        for (auto *button : visible) {
            const int horizontal = m::control::outlinedHorizontalPadding + m::control::border;
            const int vertical = m::control::outlinedVerticalPadding + m::control::border;
            const auto bounds = button->fontMetrics().boundingRect(
                QRect(0, 0, qMax(1, buttonWidth - 2 * horizontal), 10000), Qt::TextWordWrap, button->text());
            button->setFixedHeight(
                qMax<int>(m::control::outlinedHeight + 2 * vertical, bounds.height() + 2 * vertical));
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
    adjustSize();
}

namespace {
class AnchorPointer final : public QWidget {
public:
    explicit AnchorPointer(QWidget *parent) : QWidget(parent) {
        setObjectName("rrAnchorPointer");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool below = property("card_below").toBool();
        const qreal base = below ? height() : 0;
        const qreal tip = below ? 1 : height() - 1;
        QPolygonF triangle;
        triangle << QPointF(1, base) << QPointF(width() / 2.0, tip) << QPointF(width() - 1, base);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawPolygon(triangle);
        painter.setPen(QPen(palette().color(QPalette::WindowText), 2));
        painter.drawPolyline(triangle);
    }
};
}

ModalOverlay::ModalOverlay(QWidget *parent) : QWidget(parent) {
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
void ModalOverlay::centerCard() {
    if (anchor.isEmpty()) {
        pointer->hide();
        content->move(rect().center() - content->rect().center());
        return;
    }
    const int gap = ui::px(16), inset = ui::px(24);
    const int x =
        qBound(inset, anchor.center().x() - content->width() / 2, qMax(inset, width() - content->width() - inset));
    const int below = anchor.bottom() + gap;
    const int y = below + content->height() <= height() - inset ? below : anchor.top() - content->height() - gap;
    content->move(x, qBound(inset, y, qMax(inset, height() - content->height() - inset)));
    const bool cardBelow = content->y() > anchor.center().y();
    const int half = ui::px(16);
    const int center = qBound(content->x() + half + 2, anchor.center().x(), content->geometry().right() - half - 2);
    pointer->setProperty("card_below", cardBelow);
    pointer->setGeometry(center - half, cardBelow ? content->y() - gap : content->geometry().bottom() - 1, 2 * half,
                         gap + 2);
    pointer->show();
    pointer->raise();
    pointer->update();
}
bool ModalOverlay::eventFilter(QObject *object, QEvent *event) {
    if (object == parentWidget() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    if (object == content && event->type() == QEvent::Resize)
        centerCard();
    return QWidget::eventFilter(object, event);
}
void ModalOverlay::fitCard() {
    content->fitToWidth(qMin<int>(m::notice::cardWidth, qMax(1, width() - 2 * m::notice::sideClearance)));
    centerCard();
}
QRect ModalOverlay::progressUpdateRect(int percent, const QString &message) const {
    if (!loadingProgress || !isVisible() || !loadingProgress->isVisible() ||
        loadingProgress->value() == qBound(0, percent, 100) ||
        (!message.isEmpty() && loadingMessage->text() != message)) {
        return {};
    }
    return QRect(loadingProgress->mapTo(window(), QPoint()), loadingProgress->size());
}
void ModalOverlay::setDelayedProgress(bool active, qint64 elapsedMilliseconds, int percent, const QString &message) {
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
    const bool changed =
        loadingProgress->isHidden() == showBar || (!message.isEmpty() && loadingMessage->text() != message);
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
void ModalOverlay::resizeEvent(QResizeEvent *) {
    if (property("reader_standard_modal").toBool())
        fitCard();
    else
        centerCard();
}
void ModalOverlay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape && dismissed)
        dismissed();
    event->accept();
}
void ModalOverlay::mousePressEvent(QMouseEvent *event) {
    event->accept();
}
void ModalOverlay::mouseReleaseEvent(QMouseEvent *event) {
    if (dismissed)
        dismissed();
    event->accept();
}
bool ModalOverlay::focusNextPrevChild(bool next) {
    auto *candidate = focusWidget();
    if (!candidate)
        candidate = this;
    auto *start = candidate;
    do {
        candidate = next ? candidate->nextInFocusChain() : candidate->previousInFocusChain();
        if (candidate != this && isAncestorOf(candidate) && candidate->isVisible() && candidate->isEnabled() &&
            (candidate->focusPolicy() & Qt::TabFocus)) {
            candidate->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            return true;
        }
    } while (candidate != start);
    return true;
}

QPushButton *makeModalButton(QWidget *parent, const ModalAction &action, bool primary) {
    return new ModalButton(action, primary, parent);
}

NoticeModal::NoticeModal(QWidget *parent, const ModalContent &content, const ModalAction &action)
    : ModalOverlay(parent) {
    description = modalContent(card(), content);
    primary = makeModalButton(card(), action, true);
    card()->adaptiveActions({primary});
    setProperty("reader_standard_modal", true);
    fitCard();
}
ChoiceModal::ChoiceModal(QWidget *parent, const ModalContent &content, const ModalAction &secondaryAction,
                         const ModalAction &primaryAction)
    : ModalOverlay(parent) {
    description = modalContent(card(), content);
    secondary = makeModalButton(card(), secondaryAction, false);
    primary = makeModalButton(card(), primaryAction, true);
    card()->adaptiveActions({secondary, primary});
    setProperty("reader_standard_modal", true);
    fitCard();
}

PopupWindow::PopupWindow(QWidget *source, bool dark) : QDialog(source, Qt::Dialog | Qt::FramelessWindowHint) {
    if (source)
        background = source->grab();
    setWindowModality(Qt::ApplicationModal);
    setProperty("reader_dark", dark);
    QPalette colors = palette();
    colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
    colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
    setPalette(colors);
    setFont(interfaceFont());
    readerStyle(this, styles::reader());
    overlay = new ModalOverlay(this);
    overlay->show();
}
void PopupWindow::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (!background.isNull())
        painter.drawPixmap(rect(), background);
}
}
