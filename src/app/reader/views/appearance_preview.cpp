#include "appearance_preview.h"
#include "support/diagnostics.h"
#include "support/strings.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
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

AppearancePreview::AppearancePreview(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrAppearancePreview");
    // A tap in this space still reaches the page and closes the panel.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // It paints its whole rectangle. Without this, an update of the page behind it
    // repaints it too, and each repaint is a visible e-ink update.
    setAttribute(Qt::WA_OpaquePaintEvent);
    hide();
}

void AppearancePreview::setContent(Content next)
{
    // Every repaint is a visible e-ink update. Refreshes that change nothing stay silent.
    const auto same = [](const QImage &left, const QImage &right) {
        return left.cacheKey() == right.cacheKey();
    };
    const bool unchanged = content.page == next.page && content.bodyTop == next.bodyTop
            && content.bodyHeight == next.bodyHeight && content.axis == next.axis
            && content.horizontalMargin == next.horizontalMargin
            && content.verticalMargin == next.verticalMargin && same(content.header, next.header)
            && same(content.footer, next.footer) && same(content.body, next.body)
            && content.text == next.text && content.cropTop == next.cropTop;
    content = std::move(next);
    // Created on first use: a hidden child repaints the Layout preview once more when
    // it first appears, and each repaint is a visible e-ink update.
    if (content.text && !textHeader) {
        textHeader = new ui::ListHeader(this);
        textHeader->setObjectName("rrTextPreviewHeader");
        textHeader->setShaded(true);
        textHeader->setCaptions(strings::get("reader.appearance_preview").toUpper());
        textHeader->setGeometry(0, 0, width(), textHeader->sizeHint().height());
    }
    if (textHeader)
        textHeader->setVisible(content.text);
    if (!unchanged)
        update();
}

void AppearancePreview::followPanel(QWidget *watched, std::function<void()> changed)
{
    panel = watched;
    panelChanged = std::move(changed);
    panel->installEventFilter(this);
}

bool AppearancePreview::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == panel && panelChanged
        && (event->type() == QEvent::Hide || event->type() == QEvent::Resize
            || event->type() == QEvent::Move))
        panelChanged();
    return QWidget::eventFilter(watched, event);
}

qreal AppearancePreview::scale() const
{
    if (content.page.isEmpty())
        return 0;
    const int padding = ui::px(pagePadding);
    return qMax<qreal>(0,
                       qMin(qreal(width() - 2 * padding) / content.page.width(),
                            qreal(height() - 2 * padding) / content.page.height()));
}

QRect AppearancePreview::pageBounds() const
{
    const qreal factor = scale();
    const QSize size(qRound(content.page.width() * factor), qRound(content.page.height() * factor));
    return { (width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
             size.height() };
}

const QImage &AppearancePreview::scaled(ScaledImage &cache, const QImage &source,
                                        QSize target) const
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

namespace {
QFont captionFont()
{
    QFont font = ui::interfaceFont(ui::metrics::chrome::captionText);
    font.setLetterSpacing(QFont::AbsoluteSpacing, ui::metrics::chrome::captionSpacing);
    return font;
}

bool blankRow(const QImage &image, int y, QRgb paper)
{
    for (int x = 0; x < image.width(); ++x)
        if (image.pixel(x, y) != paper)
            return false;
    return true;
}
} // namespace

void AppearancePreview::paintCaption(QPainter &painter, int right) const
{
    // The caption takes only spare space, and only when it fits. Its left edge
    // matches the panel's labels below.
    const QFont font = captionFont();
    const QString caption = strings::get("reader.appearance_preview").toUpper();
    const int left = ui::metrics::panel::contentMargins.left();
    const int top = ui::px(pagePadding);
    if (left + ui::interfaceTextWidth(caption, font) + top > right)
        return;
    painter.save();
    painter.setFont(font);
    painter.setPen(ui::themeColors(this).ink);
    ui::drawInterfaceText(painter, QRect(left, top, right - left, ui::interfaceLineHeight(font)),
                          caption, Qt::AlignLeft | Qt::AlignVCenter);
    painter.restore();
}

QRect AppearancePreview::textCard() const
{
    const int top = textHeader->sizeHint().height();
    int available = qMax(0, height() - top);
    if (content.body.isNull() || available <= 0)
        return { 0, top, width(), available };
    // End at the gap under the last whole line, so that no line is cut in half.
    const QRgb paper = content.body.pixel(0, content.cropTop);
    int last = qMin(content.cropTop + available, content.body.height()) - 1;
    while (last > content.cropTop && !blankRow(content.body, last, paper))
        --last;
    return { 0, top, width(), qMax(1, last + 1 - content.cropTop) };
}

bool AppearancePreview::fillsTextCrop(const QImage &body, int cropTop) const
{
    if (body.isNull() || height() <= 0)
        return false;
    const int bottom = qMin(cropTop + height(), body.height()) - 1;
    const QRgb paper = body.pixel(0, cropTop);
    // Look for text in the lower part of the crop.
    for (int y = bottom; y > cropTop + height() * 2 / 3; --y)
        if (!blankRow(body, y, paper))
            return true;
    return false;
}

void AppearancePreview::paintText(QPainter &painter)
{
    // The header names the preview, like a section in Settings. The sample has the rest.
    const int top = textHeader->sizeHint().height();
    painter.fillRect(0, top, width(), height() - top,
                     content.body.isNull() ? ui::themeColors(this).paper
                                           : QColor(content.body.pixel(0, content.cropTop)));
    const QRect card = textCard();
    if (card.isEmpty() || content.body.isNull())
        return;
    // One image pixel per interface pixel: this is the text's true size.
    painter.drawImage(card.topLeft(), content.body,
                      QRect(0, content.cropTop, card.width(), card.height()));
}

void AppearancePreview::resizeEvent(QResizeEvent *)
{
    if (textHeader)
        textHeader->setGeometry(0, 0, width(), textHeader->sizeHint().height());
}

void AppearancePreview::paintEvent(QPaintEvent *)
{
    DiagnosticOperation operation("appearance-preview-paint");
    const auto colors = ui::themeColors(this);
    QPainter painter(this);
    // The same gray behind a white page as the Styling samples, so it reads as a sample.
    painter.fillRect(rect(), ui::sectionColor(this));
    if (content.text) {
        paintText(painter);
        return;
    }
    const qreal factor = scale();
    if (factor <= 0)
        return;
    const QRect bounds = pageBounds();
    painter.fillRect(bounds, colors.paper);
    paintCaption(painter, bounds.left());
    painter.setClipRect(bounds);

    const auto logicalHeight = [factor](const QImage &image) {
        return qRound(image.height() / image.devicePixelRatio() * factor);
    };
    if (!content.header.isNull()) {
        const QSize target(bounds.width(), logicalHeight(content.header));
        painter.drawImage(bounds.topLeft(), scaled(scaledHeader, content.header, target));
    }
    const int bodyTop = bounds.top() + qRound(content.bodyTop * factor);
    const QRect body(bounds.left(), bodyTop, bounds.width(), qRound(content.bodyHeight * factor));
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
