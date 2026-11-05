#include "options.h"
#include "icons.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "ui/style/private_text.h"
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
           pressedFill == other.pressedFill && leadingIndicator == other.leadingIndicator &&
           opticalLabel == other.opticalLabel && valueBelowLabel == other.valueBelowLabel;
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
void DescribedOption::setDescription(const QString &text) {
    if (description == text) return;
    description = text;
    setAccessibleDescription(text);
    updateGeometry();
    update();
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
    appearance.minimumRowHeight = description.isEmpty() ? m::control::stepSize : m::settings::optionHeight;
    setMinimumHeight(appearance.minimumRowHeight);
    setFocusPolicy(Qt::NoFocus);
}
void DescribedOption::setPreview(QWidget *widget, QSize size) {
    if (preview != widget)
        delete preview;
    preview = widget;
    previewSize = size;
    preview->setParent(this);
    preview->setAttribute(Qt::WA_TransparentForMouseEvents);
    preview->show();
    updateGeometry();
    update();
}
void DescribedOption::setLeadingIcon(ControlIcon icon) {
    leadingIcon = int(icon);
    updateGeometry();
    update();
}
int DescribedOption::iconSpace() const {
    return leadingIcon < 0 ? 0 : int(m::chrome::cellIcon) + px(20);
}
int DescribedOption::labelHeight() const {
    // The line grows only by what large text adds, so standard text keeps
    // the checkbox's line.
    if (textScale() == 1)
        return m::settings::checkboxSize;
    QFont labelFont = font(), standard = font();
    labelFont.setPixelSize(appearance.labelPixels);
    standard.setPixelSize(qRound(appearance.labelPixels / textScale()));
    return m::settings::checkboxSize + qMax(0, interfaceLineHeight(labelFont) - interfaceLineHeight(standard));
}
int DescribedOption::labelRaise(const QFont &labelFont) const {
    // Interface text is centered by its ink, which includes descenders, so
    // the capitals of a label sit below the middle of its line, most of all
    // in a word without descenders. A label that stands alone beside its
    // indicator is raised by that distance, so its capitals center on the
    // indicator at every text size. A label with a description moves with
    // its block.
    if (!description.isEmpty() || appearance.opticalLabel || text().isEmpty())
        return 0;
    const auto ink = interfaceInkBounds(text(), labelFont);
    const int top = qMin(ink.top(), -labelFont.pixelSize()), bottom = qMax(ink.bottom() + 1, 0);
    // The ink of a capital H is the cap height; Qt 5.2 has no capHeight().
    const int capital = interfaceInkBounds(QStringLiteral("H"), labelFont).height();
    const qreal capitalsBelowMiddle = -top - (bottom - top) / 2.0 - capital / 2.0;
    return qMax(0, qRound(capitalsBelowMiddle));
}
int DescribedOption::descriptionTop() const {
    return m::settings::descriptionTop + labelHeight() - m::settings::checkboxSize;
}
int DescribedOption::valueHeight() const {
    if (!appearance.valueBelowLabel || (value.isEmpty() && !valueStyle.battery)) return 0;
    const auto font = valueStyle.sampleWeight >= 0 ? valueStyle.sampleFont : chromeFont(m::control::captionValueText);
    return interfaceLineHeight(font) + px(8);
}

int DescribedOption::heightForWidth(int width) const {
    if (description.isEmpty() && !preview) {
        const int extra = valueHeight();
        return extra ? qMax<int>(appearance.minimumRowHeight, qMax(int(m::control::stepSize) + extra,
            m::settings::checkboxTop + labelHeight() + extra + m::settings::descriptionBottom))
            : qMax<int>(appearance.minimumRowHeight, m::control::stepSize);
    }
    QFont descriptionFont = font();
    descriptionFont.setPixelSize(appearance.descriptionPixels);
    descriptionFont.setWeight(QFont::Normal);
    descriptionFont.setItalic(false);
    const int textWidth = qMax(1, width - 2 * appearance.horizontalInset - m::settings::labelLeft -
                                      appearance.indicatorSize.width() - m::settings::toggleGap - iconSpace() -
                                      (preview ? previewSize.width() + m::settings::toggleGap : 0));
    const int textHeight = description.isEmpty() ? int(m::control::stepSize) : descriptionTop() + valueHeight() +
                                                 wrappedInterfaceTextHeight(description, descriptionFont, textWidth) +
                                                 m::settings::descriptionBottom;
    return qMax(qMax(appearance.minimumRowHeight, textHeight), preview ? previewSize.height() + px(24) : 0);
}
void DescribedOption::resizeEvent(QResizeEvent *event) {
    QCheckBox::resizeEvent(event);
    setMinimumHeight(heightForWidth(event->size().width()));
    if (preview)
        preview->setGeometry(width() - appearance.horizontalInset - previewSize.width(),
                             (height() - previewSize.height()) / 2, previewSize.width(), previewSize.height());
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
                                      appearance.indicatorSize.width() - m::settings::toggleGap - iconSpace() -
                                      (preview ? previewSize.width() + m::settings::toggleGap : 0));
    const int textBottom =
        description.isEmpty()
            ? m::settings::checkboxTop + labelHeight()
            : descriptionTop() + wrappedInterfaceTextHeight(description, descriptionFont, textWidth);
    const int textOffset = (height() - (textBottom + valueHeight() - m::settings::checkboxTop)) / 2 - m::settings::checkboxTop;
    const int boxTop = (height() - appearance.indicatorSize.height()) / 2;
    const int boxLeft = appearance.leadingIndicator
                            ? appearance.horizontalInset + m::settings::labelLeft
                            : width() - appearance.horizontalInset - appearance.indicatorSize.width();
    const QRect box(boxLeft, boxTop, appearance.indicatorSize.width(), appearance.indicatorSize.height());
    const int textLeft = (appearance.leadingIndicator ? box.right() + 1 + m::settings::toggleGap
                                                      : appearance.horizontalInset + m::settings::labelLeft) +
                         iconSpace();
    const bool held = appearance.pressedFill && isEnabled() && isDown();
    // The pressed gray of list rows such as Chapters.
    painter.fillRect(rect(), held ? themeColors(this).pressedPaper : paper);
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
    if (leadingIcon >= 0) {
        // The same place as a list action row's icon, balancing its bearings.
        const int size = m::chrome::cellIcon;
        controlIcon(ControlIcon(leadingIcon), ink)
            .paint(&painter, QRect(textLeft - iconSpace() - px(7), (height() - size) / 2 + px(1), size, size));
    }
    painter.setPen(ink);
    QFont labelFont = font();
    labelFont.setPixelSize(appearance.labelPixels);
    labelFont.setWeight(QFont::Normal);
    labelFont.setItalic(false);
    painter.setFont(labelFont);
    // Only the label is raised; a value on the same line keeps its place.
    const QRect labelLine(textLeft, m::settings::checkboxTop + textOffset, textWidth, labelHeight());
    QRect labelBounds = labelLine.translated(0, -labelRaise(labelFont));
    if (!value.isEmpty() || valueStyle.battery) {
        // The value keeps the right of the line; the label keeps at least three fifths.
        const int labelWidth = appearance.valueBelowLabel ? 0 : qMin(interfaceTextWidth(text(), labelFont), labelLine.width() * 3 / 5);
        if (!appearance.valueBelowLabel) labelBounds.setWidth(labelWidth);
        const QRect valueLine = appearance.valueBelowLabel
            ? QRect(textLeft, labelLine.bottom() + 1 + px(8), textWidth, valueHeight() - px(8)) : labelLine;
        QFont valueFont =
            valueStyle.sampleWeight >= 0 ? valueStyle.sampleFont : chromeFont(m::control::captionValueText);
        if (valueStyle.sampleWeight < 0)
            valueFont.setLetterSpacing(QFont::AbsoluteSpacing, m::chrome::captionSpacing);
        painter.save();
        // The battery keeps the right edge and the text ends before it.
        const qreal body = qRound(valueFont.pixelSize() * .75);
        if (valueStyle.battery)
            valueFont = batteryTextFont(valueFont, body, valueStyle.sampleWeight);
        painter.setFont(valueFont);
        const int icon = valueStyle.battery ? qCeil(batteryWidth(body)) : 0;
        const int gap = valueStyle.battery && !value.isEmpty() ? int(m::reading::batteryGap) : 0;
        const int room = qMax(0, valueLine.width() - labelWidth - m::settings::toggleGap - icon - gap);
        const int textRight = valueLine.right() + 1 - icon - gap;
        const qreal halfSpaced = valueStyle.halfSpaces ? halfSpacedWidth(painter.font(), value) : 0;
        if (valueStyle.battery)
            drawOpticalPrivateText(painter, QRect(textRight - room, valueLine.y(), room, valueLine.height()),
                                   elideInterfaceText(value, valueFont, room), Qt::AlignRight, valueStyle.sampleWeight);
        else if (valueStyle.halfSpaces && halfSpaced <= room)
            drawHalfSpaced(painter, textRight - halfSpaced, valueLine, value, valueStyle.sampleWeight);
        else {
            const auto shown = qt::textWidth(painter.fontMetrics(), value) > room
                                   ? painter.fontMetrics().elidedText(value, Qt::ElideRight, room)
                                   : value;
            drawInterfaceText(painter, QRect(textRight - room, valueLine.y(), room, valueLine.height()), shown,
                              Qt::AlignRight | Qt::AlignVCenter, valueStyle.sampleWeight);
        }
        if (valueStyle.battery) {
            const qreal width = body * int(m::panel::batteryBodyWidth) / int(m::panel::batteryBodyHeight);
            paintBattery(
                painter,
                QRectF(valueLine.right() + 1 - icon, valueLine.y() + (valueLine.height() - body) / 2.0, width, body),
                valueStyle.percent, valueStyle.charging, palette().color(QPalette::WindowText),
                palette().color(QPalette::Window));
        }
        painter.restore();
    }
    const auto label = interfaceTextWidth(text(), labelFont) > labelBounds.width()
                           ? elideInterfaceText(text(), labelFont, labelBounds.width())
                           : text();
    if (drawFontPreview(painter, labelBounds, text(), fontPreview, m::choices::sampleInkHeight)) {
        // The bitmap has already been aligned within the label bounds.
    } else if (appearance.opticalLabel)
        drawOpticalText(painter, labelBounds, text());
    else
        drawInterfaceText(painter, labelBounds, label, Qt::AlignLeft | Qt::AlignVCenter);
    labelFont.setPixelSize(appearance.descriptionPixels);
    labelFont.setItalic(false);
    painter.setFont(labelFont);
    painter.setPen(themeColors(this).secondaryInk);
    drawWrappedInterfaceText(painter,
                             QRect(textLeft, descriptionTop() + valueHeight() + textOffset, textWidth,
                                   height() - descriptionTop() - valueHeight() - textOffset),
                             description);
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
void RadioList::setUniformRows(bool uniform) {
    uniformRows = uniform;
    arrange();
    updateGeometry();
}
int RadioList::rowHeight(int index, int width) const {
    if (uniformRows || index < 0 || index >= rows.size())
        return rowHeight(width);
    return qMax(minimum, rows[index]->heightForWidth(width));
}
int RadioList::rowHeight(int width) const {
    int height = minimum;
    for (const auto *row : rows)
        height = qMax(height, row->heightForWidth(width));
    return height;
}
int RadioList::heightForWidth(int width) const {
    const int last = count < 0 ? int(rows.size()) : qMin(int(rows.size()), first + count);
    int height = 0;
    for (int index = first; index < last; ++index)
        height += rowHeight(index, width);
    return height;
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
    const int last = count < 0 ? int(rows.size()) : qMin(int(rows.size()), first + count);
    int top = 0;
    for (int index = 0; index < rows.size(); ++index) {
        auto *row = rows[index];
        const bool shown = index >= first && index < last;
        auto look = row->presentation();
        look.dividers = Edges(Qt::BottomEdge) | (topDivider && index == first ? Edges(Qt::TopEdge) : Edges{});
        row->setPresentation(look);
        row->setVisible(shown);
        if (shown) {
            const int height = rowHeight(index, width());
            // Replace the minimum measured at the old width before Qt constrains the new geometry.
            row->setMinimumHeight(height);
            row->setGeometry(0, top, width(), height);
            top += height;
        }
    }
}
}
