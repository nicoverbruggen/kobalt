#include "sleep_screen.h"

#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "support/strings.h"

#include <QFontMetrics>
#include <QPainter>

namespace kobalt::reader {
QImage sleepScreenImage(QImage page, const QString &progress, const QString &remaining, bool dark) {
    if (page.isNull()) {
        return {};
    }
    namespace m = ui::metrics::sleep;
    QPainter painter(&page);
    painter.fillRect(page.rect(), QColor(0, 0, 0, m::dimAlpha));
    const int inset = qMin(int(m::inset), page.width() / 12);
    const int padding = qMin(int(m::padding), page.width() / 16);
    const QFont title = ui::brandFont(m::titleText);
    const QFont detail = ui::interfaceFont(m::detailText);
    const int titleHeight = QFontMetrics(title).height();
    const int detailHeight = QFontMetrics(detail).height();
    const int height = padding * 2 + titleHeight + int(m::gap) + detailHeight +
                       (remaining.isEmpty() ? 0 : detailHeight + int(m::lineGap));
    const QRect card(inset, qMax(inset, page.height() - height - int(m::bottom)),
                     qMin(int(m::width), page.width() - 2 * inset), height);
    const QColor paper(dark ? m::darkPaper : m::lightPaper);
    const QColor ink(dark ? Qt::white : Qt::black);
    painter.fillRect(card, paper);
    painter.setPen(QPen(ink, int(m::border)));
    painter.drawRect(card.adjusted(0, 0, -1, -1));
    const int left = card.left() + padding;
    const int width = card.width() - 2 * padding;
    int top = card.top() + padding;
    painter.setFont(title);
    ui::drawInterfaceText(painter, QRect(left, top, width, titleHeight), strings::get("sleep.sleeping"));
    top += titleHeight + int(m::gap);
    painter.setFont(detail);
    ui::drawInterfaceText(painter, QRect(left, top, width, detailHeight),
                          ui::elideInterfaceText(progress, detail, width));
    if (!remaining.isEmpty()) {
        top += detailHeight + int(m::lineGap);
        ui::drawInterfaceText(painter, QRect(left, top, width, detailHeight),
                              ui::elideInterfaceText(remaining, detail, width));
    }
    painter.end();
    return page;
}
}
