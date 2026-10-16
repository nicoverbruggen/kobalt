#include "options.h"
#include "../style/metrics.h"
#include "../style/styles.h"
#include "../style/fonts.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "../style/private_text.h"
#include "panels.h"
#include "text.h"
#include <QPainter>
#include <QResizeEvent>
#include <QtMath>
#include <QMouseEvent>
#include <QPointer>

namespace kobalt::ui {
namespace m = metrics;
void DescribedOption::mouseReleaseEvent(QMouseEvent *event) {
    // Batch Qt's released-state paint with the changes made by clicked.
    const bool enabled = updatesEnabled();
    if (enabled)
        setUpdatesEnabled(false);
    QPointer<DescribedOption> alive(this);
    QPointer<QWidget> parent = parentWidget();
    const QRect oldGeometry = geometry();
    const bool visible = isVisible();
    QCheckBox::mouseReleaseEvent(event);
    if (alive && enabled)
        alive->setUpdatesEnabled(true);
    if (enabled && visible && parent && (!alive || !alive->isVisible()))
        parent->update(oldGeometry);
}
DescribedOption::Presentation::Presentation()
    : labelPixels(m::text::body), descriptionPixels(m::text::secondary),
      indicatorSize(m::settings::toggleWidth, m::settings::toggleHeight) {}
bool DescribedOption::Presentation::operator==(const Presentation &other) const {
    return indicator == other.indicator && dividers == other.dividers && horizontalInset == other.horizontalInset &&
           minimumRowHeight == other.minimumRowHeight && labelPixels == other.labelPixels &&
           descriptionPixels == other.descriptionPixels && indicatorSize == other.indicatorSize &&
           pressedFill == other.pressedFill && leadingIndicator == other.leadingIndicator;
}
void DescribedOption::setPresentation(const Presentation &value) {
    if (appearance == value)
        return;
    appearance = value;
    setMinimumHeight(appearance.minimumRowHeight);
    updateGeometry();
    update();
}
void DescribedOption::setValue(const QString &next) {
    setValue(next, ValueStyle());
}
void DescribedOption::setValue(const QString &next, const ValueStyle &style) {
    if (value == next && valueStyle == style)
        return;
    value = next;
    valueStyle = style;
    update();
}
DescribedOption::DescribedOption(const QString &text, const QString &description, QWidget *parent)
    : QCheckBox(text, parent), description(description) {
    setAccessibleDescription(description);
    auto policy = sizePolicy();
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setMinimumHeight(description.isEmpty() ? m::control::stepSize : m::settings::optionHeight);
    setFocusPolicy(Qt::NoFocus);
}
int DescribedOption::heightForWidth(int width) const {
    if (description.isEmpty())
        return m::control::stepSize;
    QFont descriptionFont = font();
    descriptionFont.setPixelSize(appearance.descriptionPixels);
    descriptionFont.setWeight(QFont::Normal);
    descriptionFont.setItalic(false);
    const auto bounds =
        QFontMetrics(descriptionFont)
            .boundingRect(QRect(0, 0,
                                qMax(1, width - 2 * appearance.horizontalInset - m::settings::labelLeft -
                                            appearance.indicatorSize.width() - m::settings::toggleGap),
                                0),
                          Qt::TextWordWrap, description);
    return qMax(appearance.minimumRowHeight,
                m::settings::descriptionTop + bounds.height() + m::settings::descriptionBottom);
}
void DescribedOption::resizeEvent(QResizeEvent *event) {
    QCheckBox::resizeEvent(event);
    setMinimumHeight(heightForWidth(event->size().width()));
}
bool DescribedOption::hitButton(const QPoint &position) const {
    return rect().contains(position);
}
void DescribedOption::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto ink = palette().color(QPalette::WindowText);
    const auto paper = palette().color(QPalette::Window);
    QFont descriptionFont = font();
    descriptionFont.setPixelSize(appearance.descriptionPixels);
    descriptionFont.setWeight(QFont::Normal);
    descriptionFont.setItalic(false);
    const int textWidth = qMax(1, width() - 2 * appearance.horizontalInset - m::settings::labelLeft -
                                      appearance.indicatorSize.width() - m::settings::toggleGap);
    const int textBottom =
        description.isEmpty()
            ? m::settings::checkboxTop + m::settings::checkboxSize
            : m::settings::descriptionTop + QFontMetrics(descriptionFont)
                                                .boundingRect(QRect(0, 0, textWidth, 0), Qt::TextWordWrap, description)
                                                .height();
    const int textOffset = (height() - (textBottom - m::settings::checkboxTop)) / 2 - m::settings::checkboxTop;
    const int boxTop = (height() - appearance.indicatorSize.height()) / 2;
    const int boxLeft = appearance.leadingIndicator
                            ? appearance.horizontalInset + m::settings::labelLeft
                            : width() - appearance.horizontalInset - appearance.indicatorSize.width();
    const QRect box(boxLeft, boxTop, appearance.indicatorSize.width(), appearance.indicatorSize.height());
    const int textLeft = appearance.leadingIndicator ? box.right() + 1 + m::settings::toggleGap
                                                     : appearance.horizontalInset + m::settings::labelLeft;
    const bool held = appearance.pressedFill && isEnabled() && isDown();
    // The pressed gray of list rows such as Chapters.
    painter.fillRect(rect(), held ? (darkTheme(this) ? QColor("#333333") : QColor("#dddddd")) : paper);
    if (appearance.dividers) {
        paintEdges(painter, rect(), appearance.dividers, dividerColor(this));
    }
    if (!isEnabled())
        painter.setOpacity(.45);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF track = QRectF(box).adjusted(1, 1, -1, -1);
    if (appearance.indicator == Indicator::Radio) {
        painter.setPen(QPen(ink, px(2)));
        painter.setBrush(paper);
        painter.drawEllipse(track);
        if (isChecked() || held) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            const qreal inset = track.width() / 4;
            painter.drawEllipse(track.adjusted(inset, inset, -inset, -inset));
        }
    } else if (appearance.indicator == Indicator::Square) {
        painter.setPen(QPen(ink, px(2)));
        painter.setBrush(isChecked() ? ink : paper);
        painter.drawRect(track);
        if (isChecked()) {
            painter.setPen(QPen(paper, px(4), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            QPolygonF check;
            // The tick is drawn for a 48 pixel box and scales with it.
            check << QPointF(box.left() + px(11), box.top() + px(24))
                  << QPointF(box.left() + px(21), box.top() + px(34))
                  << QPointF(box.left() + px(37), box.top() + px(14));
            painter.drawPolyline(check);
        }
    } else {
        painter.setPen(QPen(ink, m::control::border));
        painter.setBrush(isChecked() ? ink : paper);
        const qreal inset = int(m::control::border) / 2.0;
        const QRectF roundedTrack = QRectF(box).adjusted(inset, inset, -inset, -inset);
        painter.drawRoundedRect(roundedTrack, roundedTrack.height() / 2, roundedTrack.height() / 2);
        const int thumbInset = px(6), diameter = box.height() - 2 * thumbInset;
        const int thumbX = isChecked() ? box.right() - thumbInset - diameter + 1 : box.left() + thumbInset;
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? paper : ink);
        painter.drawEllipse(QRect(thumbX, box.top() + thumbInset, diameter, diameter));
    }
    painter.setPen(ink);
    QFont labelFont = font();
    labelFont.setPixelSize(appearance.labelPixels);
    labelFont.setWeight(QFont::Normal);
    labelFont.setItalic(false);
    painter.setFont(labelFont);
    const QRect labelLine(textLeft, m::settings::checkboxTop + textOffset, textWidth, m::settings::checkboxSize);
    QRect labelBounds = labelLine;
    if (!value.isEmpty() || valueStyle.battery) {
        // The value keeps the right of the line; the label keeps at least three fifths.
        const int labelWidth = qMin(interfaceTextWidth(text(), labelFont), labelLine.width() * 3 / 5);
        labelBounds.setWidth(labelWidth);
        QFont valueFont = chromeFont(m::control::captionValueText);
        valueFont.setLetterSpacing(QFont::AbsoluteSpacing, m::chrome::captionSpacing);
        painter.save();
        painter.setFont(valueFont);
        // The battery keeps the right edge and the text ends before it.
        const qreal body = qRound(valueFont.pixelSize() * .75);
        const int icon = valueStyle.battery ? qCeil(batteryWidth(body)) : 0;
        const int gap = valueStyle.battery && !value.isEmpty() ? int(m::reading::batteryGap) : 0;
        const int room = qMax(0, labelLine.width() - labelWidth - m::settings::toggleGap - icon - gap);
        const int textRight = labelLine.right() + 1 - icon - gap;
        const qreal halfSpaced = valueStyle.halfSpaces ? halfSpacedWidth(painter.fontMetrics(), value) : 0;
        if (valueStyle.halfSpaces && halfSpaced <= room)
            drawHalfSpaced(painter, textRight - halfSpaced, labelLine, value);
        else {
            const auto shown = qt::textWidth(painter.fontMetrics(), value) > room
                                   ? painter.fontMetrics().elidedText(value, Qt::ElideRight, room)
                                   : value;
            painter.drawText(QRect(textRight - room, labelLine.y(), room, labelLine.height()),
                             Qt::AlignRight | Qt::AlignVCenter, shown);
        }
        if (valueStyle.battery) {
            const qreal width = body * int(m::panel::batteryBodyWidth) / int(m::panel::batteryBodyHeight);
            paintBattery(
                painter,
                QRectF(labelLine.right() + 1 - icon, labelLine.y() + (labelLine.height() - body) / 2.0, width, body),
                valueStyle.percent, valueStyle.charging, palette().color(QPalette::WindowText),
                palette().color(QPalette::Window));
        }
        painter.restore();
    }
    const auto label = interfaceTextWidth(text(), labelFont) > labelBounds.width()
                           ? elideInterfaceText(text(), labelFont, labelBounds.width())
                           : text();
    drawInterfaceText(painter, labelBounds, label, Qt::AlignLeft | Qt::AlignVCenter);
    labelFont.setPixelSize(appearance.descriptionPixels);
    labelFont.setItalic(false);
    painter.setFont(labelFont);
    painter.setPen(darkTheme(this) ? QColor(m::text::explanationDark) : QColor(m::text::explanationLight));
    painter.drawText(
        QRect(textLeft, m::settings::descriptionTop + textOffset, textWidth, textBottom - m::settings::descriptionTop),
        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, description);
}

RadioList::RadioList(QWidget *parent) : QWidget(parent) {
    auto policy = sizePolicy();
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}
DescribedOption *RadioList::add(const QString &name, const QString &label, const QString &description) {
    auto *option = new DescribedOption(label, description, this);
    option->setObjectName(name);
    option->setAutoExclusive(true);
    auto look = option->presentation();
    look.indicator = DescribedOption::Indicator::Radio;
    look.horizontalInset = m::chrome::inset;
    look.labelPixels = m::chrome::labelText;
    look.descriptionPixels = m::chrome::descriptionText;
    look.indicatorSize = {m::chrome::checkSize, m::chrome::checkSize};
    look.minimumRowHeight = minimum;
    look.pressedFill = true;
    look.leadingIndicator = true;
    option->setPresentation(look);
    const int index = rows.size();
    QObject::connect(option, &QCheckBox::clicked, this, [this, index] {
        if (chosen)
            chosen(index);
    });
    rows.append(option);
    arrange();
    updateGeometry();
    return option;
}
void RadioList::setCurrent(int index) {
    for (int row = 0; row < rows.size(); ++row)
        if (row == index)
            rows[row]->setChecked(true);
    // Auto-exclusive buttons cannot all be unchecked; clear them together.
    if (index < 0 || index >= rows.size()) {
        for (auto *row : rows)
            row->setAutoExclusive(false);
        for (auto *row : rows)
            row->setChecked(false);
        for (auto *row : rows)
            row->setAutoExclusive(true);
    }
}
void RadioList::setMinimumRowHeight(int pixels) {
    minimum = pixels;
    for (auto *row : rows) {
        auto look = row->presentation();
        look.minimumRowHeight = pixels;
        row->setPresentation(look);
    }
    arrange();
    updateGeometry();
}
void RadioList::setTopDivider(bool shown) {
    topDivider = shown;
    arrange();
}
int RadioList::rowHeight(int width) const {
    int height = minimum;
    for (const auto *row : rows)
        height = qMax(height, row->heightForWidth(width));
    return height;
}
int RadioList::heightForWidth(int width) const {
    const int shown = count < 0 ? int(rows.size()) : qMin(count, int(rows.size()) - first);
    return qMax(0, shown) * rowHeight(width);
}
QSize RadioList::sizeHint() const {
    return {width(), heightForWidth(width())};
}
void RadioList::showRows(int from, int number) {
    first = qMax(0, from);
    count = number;
    arrange();
    updateGeometry();
}
void RadioList::resizeEvent(QResizeEvent *) {
    arrange();
}
void RadioList::arrange() {
    const int height = rowHeight(width());
    const int last = count < 0 ? int(rows.size()) : qMin(int(rows.size()), first + count);
    for (int index = 0; index < rows.size(); ++index) {
        auto *row = rows[index];
        const bool shown = index >= first && index < last;
        auto look = row->presentation();
        look.dividers = Edges(Qt::BottomEdge) | (topDivider && index == first ? Edges(Qt::TopEdge) : Edges{});
        row->setPresentation(look);
        row->setVisible(shown);
        if (shown)
            row->setGeometry(0, (index - first) * height, width(), height);
    }
}
}
