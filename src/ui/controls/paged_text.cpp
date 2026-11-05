#include "ui/style/fonts.h"
#include "paged_text.h"
#include "ui/style/metrics.h"
#include "ui/style/theme.h"
#include <QPainter>
#include <QEvent>

namespace kobalt::ui {
PagedText::PagedText(QWidget *parent) : QWidget(parent)
{
    const int margin = metrics::acknowledgments::textMargin;
    margins = QMargins(margin, margin, margin, margin);
    setAttribute(Qt::WA_OpaquePaintEvent);
    QFont font = kobalt::ui::interfaceFont();
    font.setPixelSize(metrics::text::body);
    document.setFont(font);
}

void PagedText::setHtml(const QString &html)
{
    source = html;
    document.setHtml(html);
    current = 0;
    stale = true;
    update();
}

void PagedText::setTextFont(const QFont &font)
{
    document.setFont(font);
    stale = true;
    update();
}

void PagedText::setMinimumLineHeight(int percent)
{
    if (minimumLineHeight == percent)
        return;
    minimumLineHeight = percent;
    document.setHtml(source);
    stale = true;
    update();
}

void PagedText::changeEvent(QEvent *event)
{
    // Rebuild inherited font attributes after interface settings change.
    if (event->type() == QEvent::FontChange) {
        document.setFont(renewedFont(document.font()));
        document.setHtml(source);
        stale = true;
        update();
    }
    QWidget::changeEvent(event);
}

void PagedText::setMargins(const QMargins &value)
{
    margins = value;
    stale = true;
    update();
}

int PagedText::pages() const
{
    ensureLayout();
    return document.pages();
}

void PagedText::setPage(int number)
{
    // A stale layout clamps the page when it is next laid out.
    current = stale ? qMax(0, number) : qBound(0, number, pages() - 1);
    update();
}

void PagedText::resizeEvent(QResizeEvent *)
{
    stale = true;
}

void PagedText::ensureLayout() const
{
    if (!stale)
        return;
    stale = false;
    document.layout(qMax(1, width() - margins.left() - margins.right()),
                    qMax(1, height() - margins.top() - margins.bottom()), minimumLineHeight);
    current = qBound(0, current, document.pages() - 1);
}

void PagedText::paintEvent(QPaintEvent *)
{
    ensureLayout();
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    const int pageHeight = qMax(1, height() - margins.top() - margins.bottom());
    const int start = current * pageHeight;
    painter.translate(margins.left(), margins.top() - start);
    document.draw(painter,
                  QRectF(0, start, qMax(1, width() - margins.left() - margins.right()), pageHeight),
                  palette());
}
} // namespace kobalt::ui
