#include "loading.h"
#include "ui/style/metrics.h"

#include <QHideEvent>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>

namespace kobalt::ui {
namespace loading = metrics::loading;

LoadingIndicator::LoadingIndicator(Style value, QWidget *parent)
    : QWidget(parent), style(value), timer(new QTimer(this))
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this] { advance(); });
}

int LoadingIndicator::count() const
{
    return style == Style::Dots ? 3 : 6;
}

int LoadingIndicator::interval() const
{
    return style == Style::Dots ? loading::dotMilliseconds : loading::segmentMilliseconds;
}

QSize LoadingIndicator::sizeHint() const
{
    if (style == Style::Logo) {
        return { loading::logoWidth, loading::logoHeight };
    }
    const int side = style == Style::Dots ? int(loading::dot) : int(loading::square);
    const int gap = style == Style::Dots ? int(loading::dotGap) : int(loading::squareGap);
    return { count() * side + (count() - 1) * gap, side };
}

void LoadingIndicator::restart()
{
    timer->stop();
    filled = 0;
    revealed = false;
    update();
    if (running && isVisible()) {
        timer->start(delay);
    }
}

void LoadingIndicator::start(int delayMilliseconds)
{
    delay = qMax(0, delayMilliseconds);
    running = true;
    restart();
}

void LoadingIndicator::stop()
{
    running = false;
    restart();
}

void LoadingIndicator::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    restart();
}

void LoadingIndicator::hideEvent(QHideEvent *event)
{
    timer->stop();
    QWidget::hideEvent(event);
}

void LoadingIndicator::advance()
{
    if (!running || !isVisible()) {
        return;
    }
    if (!revealed) {
        revealed = true;
    } else {
        filled = (filled + 1) % (count() + 1);
    }
    update();
    timer->start(interval());
}

void LoadingIndicator::paintEvent(QPaintEvent *)
{
    if (!running || !revealed) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, style == Style::Dots);
    const auto ink = palette().color(QPalette::WindowText);
    const qreal stroke = metrics::control::border;
    painter.setPen(QPen(ink, stroke));
    const QSize natural = sizeHint();
    const QSize fitted = natural.scaled(size(), Qt::KeepAspectRatio);
    painter.translate((width() - fitted.width()) / 2.0, (height() - fitted.height()) / 2.0);
    painter.scale(qreal(fitted.width()) / natural.width(),
                  qreal(fitted.height()) / natural.height());
    for (int i = 0; i < count(); ++i) {
        QRectF shape;
        if (style == Style::Logo) {
            const qreal scale = qreal(natural.width()) / metrics::opening::icon.width();
            shape = QRectF(
                    0, i * metrics::opening::iconBarPitch * scale,
                    (i == 5 ? metrics::opening::finalBarWidth : metrics::opening::icon.width())
                            * scale,
                    metrics::opening::iconBarHeight * scale);
        } else {
            const int side = style == Style::Dots ? int(loading::dot) : int(loading::square);
            const int gap = style == Style::Dots ? int(loading::dotGap) : int(loading::squareGap);
            shape = QRectF(i * (side + gap), 0, side, side);
        }
        shape.adjust(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2);
        painter.setBrush(i < filled ? QBrush(ink) : QBrush(Qt::NoBrush));
        if (style == Style::Dots) {
            painter.drawEllipse(shape);
        } else {
            painter.drawRect(shape);
        }
    }
}
} // namespace kobalt::ui
