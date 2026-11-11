#include "choice_sheet.h"
#include "modals.h"
#include "ui/controls/options.h"
#include "ui/style/metrics.h"

namespace kobalt::ui {
namespace {
int cardWidth(const QWidget *host)
{
    return qMin<int>(metrics::notice::cardWidth,
                     host->width() - 2 * metrics::notice::sideClearance);
}
} // namespace

ChoiceSheet makeChoiceSheet(QWidget *host, const QString &name, const QString &title,
                            const QString &hint, const QString &titleName, const QString &hintName)
{
    auto *overlay = new ModalOverlay(host);
    overlay->setObjectName(name + "Popup");
    overlay->setDimmed(true);
    overlay->setGeometry(host->rect());
    auto *card = overlay->card();
    card->heading(title, hint, titleName, hintName);
    // The shared pick-one list: rows share the tallest height a choice needs.
    auto *list = new RadioList(card);
    list->setObjectName(name + "Choices");
    list->setMinimumRowHeight(metrics::chrome::listActionHeight);
    list->setTopDivider(true);
    card->rows()->addWidget(list);
    return { overlay, list };
}

void fitChoiceSheet(ModalOverlay *overlay, RadioList *list)
{
    const int width = cardWidth(overlay->parentWidget());
    // The card sizes from its layout. The list fills the card inside its
    // border on both sides.
    list->setFixedHeight(list->heightForWidth(width - 2 * metrics::notice::cardBorder));
    overlay->card()->fitToWidth(width);
    overlay->centerCard();
}

void presentChoiceSheet(ModalOverlay *overlay, RadioList *list)
{
    fitChoiceSheet(overlay, list);
    overlay->show();
    overlay->raise();
}
} // namespace kobalt::ui
