#include "margin_preview.h"
#include "support/diagnostics.h"
#include "ui/style/metrics.h"
#include "ui/style/theme.h"
#include <QEvent>
#include <QPainter>

namespace kobalt::reader {
namespace {
constexpr int pagePadding = 20;

QSize deviceSize(QSize logical, qreal ratio)
{
    return { qMax(1, qRound(logical.width() * ratio)), qMax(1, qRound(logical.height() * ratio)) };
}
} // namespace

MarginPreview::MarginPreview(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrMarginPreview");
    // A tap in this space still reaches the page and closes the panel.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // It paints its whole rectangle. Without this, an update of the page behind it
    // repaints it too, and each repaint is a visible e-ink update.
    setAttribute(Qt::WA_OpaquePaintEvent);
    hide();
}

void MarginPreview::setContent(Content next)
{
    // Every repaint is a visible e-ink update. Refreshes that change nothing stay silent.
    const auto same = [](const QImage &left, const QImage &right) {
        return left.cacheKey() == right.cacheKey();
    };
    const bool unchanged = content.page == next.page && content.bodyTop == next.bodyTop
            && content.bodyHeight == next.bodyHeight && content.axis == next.axis
            && content.horizontalMargin == next.horizontalMargin
            && content.verticalMargin == next.verticalMargin && same(content.header, next.header)
            && same(content.footer, next.footer) && same(content.body, next.body);
    content = std::move(next);
    if (!unchanged)
        update();
}

void MarginPreview::followPanel(QWidget *watched, std::function<void()> changed)
{
    panel = watched;
    panelChanged = std::move(changed);
    panel->installEventFilter(this);
}

bool MarginPreview::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == panel && panelChanged
        && (event->type() == QEvent::Hide || event->type() == QEvent::Resize
            || event->type() == QEvent::Move))
        panelChanged();
    return QWidget::eventFilter(watched, event);
}

qreal MarginPreview::scale() const
{
    if (content.page.isEmpty())
        return 0;
    const int padding = ui::px(pagePadding);
    return qMax<qreal>(0,
                       qMin(qreal(width() - 2 * padding) / content.page.width(),
                            qreal(height() - 2 * padding) / content.page.height()));
}

QRect MarginPreview::pageBounds() const
{
    const qreal factor = scale();
    const QSize size(qRound(content.page.width() * factor), qRound(content.page.height() * factor));
    return { (width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
             size.height() };
}

const QImage &MarginPreview::scaled(ScaledImage &cache, const QImage &source, QSize target) const
{
    if (cache.source != source.cacheKey() || cache.size != target) {
        const qreal ratio = devicePixelRatio();
        cache.source = source.cacheKey();
        cache.size = target;
        cache.image = source.scaled(deviceSize(target, ratio), Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation);
        cache.image.setDevicePixelRatio(ratio);
    }
    return cache.image;
}

void MarginPreview::paintEvent(QPaintEvent *)
{
    DiagnosticOperation operation("margin-preview-paint");
    const auto colors = ui::themeColors(this);
    QPainter painter(this);
    painter.fillRect(rect(), colors.paper);
    const qreal factor = scale();
    if (factor <= 0)
        return;
    const QRect bounds = pageBounds();
    painter.setClipRect(bounds);

    const auto logicalHeight = [factor](const QImage &image) {
        return qRound(image.height() / image.devicePixelRatio() * factor);
    };
    if (!content.header.isNull()) {
        const QSize target(bounds.width(), logicalHeight(content.header));
        painter.drawImage(bounds.topLeft(), scaled(scaledHeader, content.header, target));
    }
    const int bodyTop = bounds.top() + qRound(content.bodyTop * factor);
    const QRect body(bounds.left(), bodyTop, bounds.width(),
                     qRound(content.bodyHeight * factor));
    if (!content.body.isNull()) {
        painter.drawImage(body.topLeft(), scaled(scaledBody, content.body, body.size()));
    }
    if (!content.footer.isNull()) {
        const QSize target(bounds.width(), logicalHeight(content.footer));
        painter.drawImage(QPoint(bounds.left(), bounds.bottom() + 1 - target.height()),
                          scaled(scaledFooter, content.footer, target));
    }
    painter.setClipping(false);

    if (content.axis != Axis::None) {
        QPen guide(colors.ink, 1, Qt::DashLine);
        painter.setPen(guide);
        if (content.axis == Axis::Horizontal) {
            const int left = bounds.left() + qRound(content.horizontalMargin * factor);
            const int right = bounds.right() - qRound(content.horizontalMargin * factor);
            // Margins apply to the text area. The header and footer have their own insets.
            painter.drawLine(left, body.top(), left, body.bottom());
            painter.drawLine(right, body.top(), right, body.bottom());
        } else {
            const int top = body.top() + qRound(content.verticalMargin * factor);
            const int bottom = body.bottom() - qRound(content.verticalMargin * factor);
            painter.drawLine(bounds.left(), top, bounds.right(), top);
            painter.drawLine(bounds.left(), bottom, bounds.right(), bottom);
        }
    }
    ui::paintEdges(painter, bounds,
                   ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                   colors.ink, ui::metrics::control::border);
}
} // namespace kobalt::reader
