#include "ui/controls/text.h"
#include "sliders.h"
#include "icons.h"
#include "ui/style/metrics.h"
#include "ui/style/theme.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "support/strings.h"
#include <QPainter>
#include <QResizeEvent>
#include "buttons.h"
#include "rows.h"
#include "ui/input/input.h"
#include <QStyle>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QtMath>
#include <algorithm>

namespace kobalt::ui {
SettingSlider::SettingSlider(QWidget *parent, int minimum, int maximum, int step)
    : QSlider(Qt::Horizontal, parent)
{
    setRange(minimum, maximum);
    setSingleStep(step);
    setMinimumHeight(metrics::control::sliderHeight);
    // Nickel fixes QSlider at 80 pixels, which clips inside our scaled rows.
    readerStyle(this,
                QString("QSlider{min-height:%1px;max-height:%1px;}")
                        .arg(int(metrics::control::sliderHeight)));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_AcceptTouchEvents);
}

void SettingSlider::setMarkers(const QVector<int> &values, MarkerMode mode)
{
    QVector<int> next;
    for (const int value : values) {
        if (value < minimum() || value > maximum() || next.contains(value))
            continue;
        next.append(value);
    }
    std::sort(next.begin(), next.end());
    if (markers == next && markerMode == mode)
        return;
    markers = next;
    markerMode = mode;
    update();
}

void SettingSlider::setMarkerLabels(const QStringList &labels)
{
    markerLabels = labels;
    const int height = metrics::control::sliderHeight
            + (labels.isEmpty() ? 0 : int(metrics::control::sliderLabelHeight));
    readerStyle(this, QString("QSlider{min-height:%1px;max-height:%1px;}").arg(height));
    updateGeometry();
    update();
}

void SettingSlider::nudge(int steps)
{
    if (adjusting)
        adjusting();
    setValue(qBound(minimum(), value() + steps * singleStep(), maximum()));
    if (committed)
        committed(value());
}

void SettingSlider::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    const int x = positionForValue(value());
    const int labelHeight = markerLabels.isEmpty() ? 0 : int(metrics::control::sliderLabelHeight);
    const int center = (height() + labelHeight) / 2;
    painter.setFont(interfaceFont(metrics::control::captionValueText));
    painter.setPen(palette().color(QPalette::WindowText));
    int labelRight = -int(metrics::control::captionGap);
    for (int index = 0; index < markers.size() && index < markerLabels.size(); ++index) {
        const int labelWidth = interfaceTextWidth(markerLabels[index], painter.font());
        const int left = qBound(0, positionForValue(markers[index]) - labelWidth / 2,
                                qMax(0, width() - labelWidth));
        // Keep the first label, including Publisher, when a narrow panel cannot
        // fit every label. The ticks and current value remain available.
        if (left < labelRight + int(metrics::control::captionGap))
            continue;
        drawInterfaceText(painter, QRect(left, 0, labelWidth, labelHeight), markerLabels[index],
                          Qt::AlignCenter);
        labelRight = left + labelWidth;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(
            QPen(palette().color(QPalette::WindowText), metrics::control::sliderMarkerWidth));
    for (const int marker : markers) {
        if (marker < minimum() || marker > maximum())
            continue;
        if (dotMarkers) {
            painter.setBrush(palette().color(QPalette::WindowText));
            painter.drawEllipse(QPoint(positionForValue(marker), center),
                                int(metrics::control::sliderMarkerHeight) / 2,
                                int(metrics::control::sliderMarkerHeight) / 2);
        } else {
            painter.drawLine(positionForValue(marker),
                             labelHeight + metrics::control::sliderMarkerTop,
                             positionForValue(marker),
                             labelHeight + metrics::control::sliderMarkerTop
                                     + metrics::control::sliderMarkerHeight);
        }
    }
    painter.setPen(QPen(palette().color(QPalette::WindowText), metrics::control::border));
    painter.drawLine(metrics::control::sliderInset, center, width() - metrics::control::sliderInset,
                     center);
    painter.fillRect(QRect(metrics::control::sliderInset,
                           center - metrics::control::sliderFillHeight / 2,
                           x - metrics::control::sliderInset, metrics::control::sliderFillHeight),
                     palette().color(QPalette::WindowText));
    painter.setBrush(palette().color(QPalette::Window));
    painter.setPen(QPen(palette().color(QPalette::WindowText), metrics::control::border));
    painter.drawEllipse(QPoint(x, center), metrics::control::sliderRadius,
                        metrics::control::sliderRadius);
    painter.setPen(QPen(palette().color(QPalette::WindowText), metrics::control::border));
    painter.drawEllipse(QPoint(x, center), metrics::control::sliderInnerRadius,
                        metrics::control::sliderInnerRadius);
}

bool SettingSlider::event(QEvent *event)
{
    // The slider draws no hover state. See Button::event.
    if (isHoverEvent(event))
        return true;
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
        || event->type() == QEvent::TouchEnd) {
        const auto points = touchPositions(static_cast<QTouchEvent *>(event));
        if (event->type() == QEvent::TouchBegin) {
            tracking = points.size() == 1;
            original = value();
        }
        if (points.size() != 1)
            cancel();
        if (tracking && !points.isEmpty())
            moveTo(points.first().x());
        if (event->type() == QEvent::TouchEnd)
            finish();
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        cancel();
        event->accept();
        return true;
    }
    return QSlider::event(event);
}

void SettingSlider::mousePressEvent(QMouseEvent *event)
{
    if (!isPointerMouse(event) || event->button() != Qt::LeftButton)
        return;
    tracking = true;
    original = value();
    moveTo(mousePosition(event).x());
}

void SettingSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (tracking)
        moveTo(mousePosition(event).x());
}

void SettingSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (!isPointerMouse(event) || event->button() != Qt::LeftButton)
        return;
    if (tracking)
        moveTo(mousePosition(event).x());
    finish();
}

void SettingSlider::hideEvent(QHideEvent *event)
{
    cancel();
    QSlider::hideEvent(event);
}

int SettingSlider::positionForValue(int value) const
{
    const int span = qMax(1, width() - 2 * metrics::control::sliderInset);
    if (markerMode != MarkerMode::Positions && markers.size() > 1) {
        if (value <= markers.first()) {
            return metrics::control::sliderInset;
        }
        for (int index = 1; index < markers.size(); ++index) {
            if (value <= markers[index]) {
                const double fraction =
                        double(value - markers[index - 1]) / (markers[index] - markers[index - 1]);
                return metrics::control::sliderInset
                        + qRound(span * (index - 1 + fraction) / (markers.size() - 1));
            }
        }
        return metrics::control::sliderInset + span;
    }
    return metrics::control::sliderInset
            + QStyle::sliderPositionFromValue(minimum(), maximum(), value, span);
}

void SettingSlider::moveTo(qreal x)
{
    if (adjusting)
        adjusting();
    if (markerMode == MarkerMode::Checkpoints && markers.size() > 1) {
        const int span = qMax(1, width() - 2 * metrics::control::sliderInset);
        const double position =
                qBound(0.0, double(x - metrics::control::sliderInset) * (markers.size() - 1) / span,
                       double(markers.size() - 1));
        const int index = qMin(int(position), int(markers.size() - 2));
        setValue(qRound(markers[index]
                        + (position - index) * (markers[index + 1] - markers[index])));
        return;
    }
    if (markerMode == MarkerMode::Stops && markers.size() > 1) {
        const int span = qMax(1, width() - 2 * metrics::control::sliderInset);
        const int index =
                qBound(0, qRound((x - metrics::control::sliderInset) * (markers.size() - 1) / span),
                       int(markers.size() - 1));
        int target = markers[index];
        qreal distance = qAbs(x - positionForValue(target));
        for (const int stop : additionalStops) {
            if (stop < minimum() || stop > maximum())
                continue;
            const qreal candidate = qAbs(x - positionForValue(stop));
            if (candidate < distance || (candidate == distance && stop > target)) {
                target = stop;
                distance = candidate;
            }
        }
        setValue(target);
        return;
    }
    const int raw = QStyle::sliderValueFromPosition(
            minimum(), maximum(), qRound(x) - metrics::control::sliderInset,
            qMax(1, width() - 2 * metrics::control::sliderInset));
    setValue(qBound(minimum(),
                    minimum() + qRound(double(raw - minimum()) / singleStep()) * singleStep(),
                    maximum()));
}

void SettingSlider::cancel()
{
    if (tracking)
        setValue(original);
    tracking = false;
}

void SettingSlider::finish()
{
    const bool changed = tracking && value() != original;
    tracking = false;
    if (changed && committed)
        committed(value());
}

RangeOverview::RangeOverview(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_AcceptTouchEvents);
    setFocusPolicy(Qt::NoFocus);
}

void RangeOverview::setRange(int values, int begin, int end, int reading)
{
    values = qMax(1, values);
    begin = qBound(0, begin, values - 1);
    end = qBound(begin, end, values - 1);
    reading = qBound(0, reading, values - 1);
    if (count == values && first == begin && last == end && current == reading) {
        return;
    }
    count = values;
    first = begin;
    last = end;
    current = reading;
    setAccessibleDescription(
            strings::get("reader.zoom_range").arg(first + 1).arg(last + 1).arg(count));
    update();
}

QRect RangeOverview::trackBounds() const
{
    const int inset = qMin(int(metrics::control::sliderInset), qMax(0, (width() - 1) / 2));
    const int center = contentsRect().top() + contentsRect().height() / 2;
    return { inset, center - int(metrics::scrubber::overviewTrack) / 2,
             qMax(1, width() - 2 * inset), metrics::scrubber::overviewTrack };
}

int RangeOverview::xAt(int value) const
{
    const QRect track = trackBounds();
    return track.left() + qRound(qreal(value) * (track.width() - 1) / qMax(1, count - 1));
}

qreal RangeOverview::valueAt(qreal x) const
{
    const QRect track = trackBounds();
    return (x - track.left()) * qMax(1, count - 1) / qMax(1, track.width() - 1);
}

QRect RangeOverview::windowBounds() const
{
    const QRect track = trackBounds();
    const int left = xAt(first), right = xAt(last);
    const int span = count == 1 ? track.width() : right - left + 1;
    const int grip = qMin(track.width(), qMax(int(metrics::scrubber::overviewGrip), span));
    const int x = qBound(track.left(), left + (span - grip) / 2, track.right() - grip + 1);
    const int center = contentsRect().top() + contentsRect().height() / 2;
    return { x, center - int(metrics::scrubber::overviewWindow) / 2, grip,
             metrics::scrubber::overviewWindow };
}

void RangeOverview::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    const auto ink = palette().color(QPalette::WindowText);
    const auto paper = palette().color(QPalette::Base);
    const auto divider = dividerColor(this);
    const Edges outline = Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge;
    const QRect track = trackBounds();
    painter.fillRect(track, themeColors(this).overviewPaper);
    const QRect window = windowBounds();
    const int border = metrics::control::border;
    painter.fillRect(track.adjusted(border, border, -border, -border).intersected(window), paper);
    paintEdges(painter, track, outline, divider);
    const int center = contentsRect().top() + contentsRect().height() / 2;
    for (int x = track.left() + int(metrics::scrubber::overviewDashSpacing); x < track.right();
         x += int(metrics::scrubber::overviewDashSpacing)) {
        paintEdges(painter,
                   { x, center - int(metrics::scrubber::overviewDashHeight) / 2,
                     int(metrics::control::border), metrics::scrubber::overviewDashHeight },
                   Qt::LeftEdge, divider);
    }
    const int cap =
            qMin(window.width(),
                 qBound(int(metrics::scrubber::overviewBracketMinimum), qRound(window.width() * .1),
                        int(metrics::scrubber::overviewBracketMaximum)));
    paintEdges(painter, { window.left(), window.top(), cap, window.height() },
               Edges(Qt::LeftEdge) | Qt::TopEdge | Qt::BottomEdge, ink);
    paintEdges(painter, { window.right() - cap + 1, window.top(), cap, window.height() },
               Edges(Qt::RightEdge) | Qt::TopEdge | Qt::BottomEdge, ink);
    const int icon = qMin(int(metrics::scrubber::overviewIcon),
                          qMin(window.width(), window.height())
                                  - 2 * int(metrics::scrubber::overviewIconPadding));
    if (icon > 0) {
        const int diameter = qMin(int(metrics::scrubber::overviewLens),
                                  qMin(window.width(), window.height()) - 2 * border);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(paper);
        painter.drawEllipse(QRectF(window.x() + (window.width() - diameter) / 2,
                                   window.y() + (window.height() - diameter) / 2, diameter,
                                   diameter));
        painter.restore();
        controlIcon(ControlIcon::Search, ink)
                .paint(&painter,
                       QRect(window.x() + (window.width() - icon) / 2,
                             window.y() + (window.height() - icon) / 2, icon, icon));
    }
    // Keep the reading marker below the grip, including when it shares the icon's x position.
    paintEdges(painter,
               { xAt(current), window.bottom() + 1 + int(metrics::scrubber::overviewTickGap),
                 int(metrics::control::border), metrics::scrubber::overviewTickHeight },
               Qt::LeftEdge, ink);
}

void RangeOverview::begin(qreal x)
{
    tracking = count > last - first + 1;
    original = first;
    const qreal value = valueAt(x);
    const QRect window = windowBounds();
    grab = x >= window.left() && x <= window.right() ? value - first : (last - first) / 2.0;
    move(x);
}

void RangeOverview::move(qreal x)
{
    if (!tracking) {
        return;
    }
    const int target =
            qRound(qBound(qreal(0), valueAt(x) - grab, qreal(count - 1 - (last - first))));
    if (target != first && moved) {
        moved(target);
    }
}

void RangeOverview::cancel()
{
    const bool restore = tracking && first != original;
    tracking = false;
    if (restore && moved) {
        moved(original);
    }
}

bool RangeOverview::event(QEvent *event)
{
    if (isHoverEvent(event)) {
        return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        cancel();
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
        || event->type() == QEvent::TouchEnd) {
        const auto points = touchPositions(static_cast<QTouchEvent *>(event));
        if (points.size() != 1) {
            cancel();
        } else if (event->type() == QEvent::TouchBegin) {
            begin(points.first().x());
        } else {
            move(points.first().x());
        }
        if (event->type() == QEvent::TouchEnd) {
            tracking = false;
        }
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void RangeOverview::mousePressEvent(QMouseEvent *event)
{
    if (isPointerMouse(event) && event->button() == Qt::LeftButton) {
        begin(mousePosition(event).x());
    }
}

void RangeOverview::mouseMoveEvent(QMouseEvent *event)
{
    move(mousePosition(event).x());
}

void RangeOverview::mouseReleaseEvent(QMouseEvent *event)
{
    if (isPointerMouse(event) && event->button() == Qt::LeftButton) {
        move(mousePosition(event).x());
        tracking = false;
    }
}

void RangeOverview::hideEvent(QHideEvent *)
{
    tracking = false;
}

SettingSlider *sliderRow(QWidget *parent, QVBoxLayout *rows, const char *name,
                         const char *labelName, const QString &label, const char *lessName,
                         const char *moreName, int minimum, int maximum, int step,
                         std::function<void(int)> apply, const QString &prefix,
                         const QString &suffix, const QStringList &markerLabels)
{
    auto *row = new ControlRow(parent);
    row->setMinimumHeight(metrics::control::stepSize);
    rows->addWidget(row);
    auto *line = row->controls();
    parent = row;
    auto *caption = row->caption();
    auto *captionRows = new QVBoxLayout(caption);
    const int labelHeight = markerLabels.isEmpty() ? 0 : int(metrics::control::sliderLabelHeight);
    captionRows->setContentsMargins(0, labelHeight, metrics::control::captionRight, 0);
    row->setMarkerBand(labelHeight);
    captionRows->setSpacing(px(2));
    auto *heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(px(6));
    captionRows->addLayout(heading);
    auto *title = new ui::InterfaceLabel(label.section(':', 0, 0), caption);
    title->setObjectName(QString::fromLatin1(name) + "Caption");
    title->setWordWrap(true);
    readerStyle(title, styles::font(metrics::text::secondary));
    heading->addWidget(title, 1);
    auto *current = new QHBoxLayout;
    current->setContentsMargins(0, 0, 0, 0);
    current->setSpacing(px(6));
    captionRows->addLayout(current);
    auto *prefixLabel = new ui::InterfaceLabel(strings::get("controls.currently"), caption);
    prefixLabel->setObjectName(QString::fromLatin1(name) + "CurrentPrefix");
    auto *value = new ui::InterfaceLabel(caption);
    value->setObjectName(labelName);
    const auto secondary =
            styles::font(metrics::control::captionValueText) + "color:palette(placeholder-text);";
    readerStyle(prefixLabel, secondary);
    readerStyle(value, secondary);
    current->addWidget(prefixLabel);
    current->addWidget(value);
    current->addStretch(1);
    // A caption above the controls has the full width. Its value moves up
    // beside the title, which keeps the row one line shorter.
    row->stackedChanged = [captionRows, heading, current, prefixLabel, value](bool stacked) {
        // Above the controls, the value ends at the plus button's edge.
        auto margins = captionRows->contentsMargins();
        margins.setRight(stacked ? 0 : int(metrics::control::captionRight));
        captionRows->setContentsMargins(margins);
        // Collapse "Currently:" rather than hide it: some rows hide it
        // themselves, and that choice must survive the move. It takes no
        // height either, so the emptied second line adds no gap.
        prefixLabel->setMaximumSize(stacked ? QSize(0, 0)
                                            : QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
        if (stacked) {
            current->removeWidget(value);
            heading->addWidget(value, 0, Qt::AlignRight | Qt::AlignVCenter);
        } else {
            heading->removeWidget(value);
            current->insertWidget(1, value);
        }
    };
    row->stackedChanged(row->property("caption_stacked").toBool());
    auto *less = new StepButton(ControlIcon::Minus, parent);
    less->setObjectName(lessName);
    less->setAccessibleName(
            strings::get("controls.decrease").arg(label.section(':', 0, 0).toLower()));
    auto *slider = new SettingSlider(parent, minimum, maximum, step);
    slider->setObjectName(name);
    if (!markerLabels.isEmpty())
        slider->setMarkerLabels(markerLabels);
    auto *more = new StepButton(ControlIcon::Plus, parent);
    more->setObjectName(moreName);
    more->setAccessibleName(
            strings::get("controls.increase").arg(label.section(':', 0, 0).toLower()));
    // Labels sit above the rail. Offset adjacent buttons by the same height,
    // as the labeled header/footer sliders do, to keep their centers aligned.
    const auto addStep = [line, labelHeight](QWidget *button) {
        if (!labelHeight) {
            line->addWidget(button, 0, Qt::AlignVCenter);
            return;
        }
        auto *column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        column->addSpacing(labelHeight);
        column->addWidget(button, 0, Qt::AlignVCenter);
        line->addLayout(column);
    };
    addStep(less);
    line->addWidget(slider, 1);
    addStep(more);
    slider->setAccessibleName(label.section(':', 0, 0));
    QObject::connect(slider, &QSlider::valueChanged, value, [value, prefix, suffix](int v) {
        value->setText(prefix + QString::number(v) + suffix);
    });
    QObject::connect(less, &QPushButton::clicked, slider, [slider] { slider->nudge(-1); });
    QObject::connect(more, &QPushButton::clicked, slider, [slider] { slider->nudge(1); });
    slider->committed = std::move(apply);
    return slider;
}
} // namespace kobalt::ui
