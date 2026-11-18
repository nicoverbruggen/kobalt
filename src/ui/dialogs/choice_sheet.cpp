#include "choice_sheet.h"
#include "modals.h"
#include "ui/controls/options.h"
#include "ui/controls/paging.h"
#include "ui/style/metrics.h"

namespace kobalt::ui {
namespace {
class ChoiceContents final : public QObject
{
public:
    ChoiceContents(ModalOverlay *overlay, RadioList *list)
        : QObject(overlay), overlay(overlay), list(list)
    {
    }

    void fit()
    {
        const int previous = starts.value(page, -1);
        starts.clear();
        if (pager)
            pager->hide();
        list->showRows(0, -1);
        list->setFixedHeight(0);
        const int availableWidth = qMax(1, overlay->width() - 2 * metrics::notice::sideClearance);
        width = qMin<int>(metrics::notice::cardWidth, availableWidth);
        const int availableHeight = overlay->height() - 2 * metrics::notice::sideClearance;
        auto *card = overlay->card();
        card->fitToWidth(width);
        auto rowsWidth = width - 2 * metrics::notice::cardBorder;
        int listHeight = list->heightForWidth(rowsWidth);
        if (card->height() + listHeight > availableHeight && overlay->width() > overlay->height()
            && width < availableWidth) {
            width = availableWidth;
            card->fitToWidth(width);
            rowsWidth = width - 2 * metrics::notice::cardBorder;
            listHeight = list->heightForWidth(rowsWidth);
        }
        if (card->height() + listHeight <= availableHeight) {
            page = 0;
            list->setFixedHeight(listHeight);
            card->fitToWidth(width);
            overlay->centerCard();
            return;
        }
        if (!pager) {
            // Leave the name unchanged for existing choice sheet controls.
            QString name = overlay->objectName();
            if (name.endsWith(QStringLiteral("Popup")))
                name.chop(5);
            pager = new PagerBar(card, name);
            card->rows()->addWidget(pager);
            connect(pager->previous, &QPushButton::clicked, this,
                    [this] { showPage(pager->turnTarget(-1)); });
            connect(pager->next, &QPushButton::clicked, this,
                    [this] { showPage(pager->turnTarget(1)); });
        }
        room = qMax(1, availableHeight - card->height() - int(metrics::chrome::actionHeight));
        starts.append(0);
        int wanted = previous, used = 0;
        for (int index = 0; index < list->options().size(); ++index) {
            if (previous < 0 && list->options()[index]->isChecked())
                wanted = index;
            const int height = list->rowHeight(index, rowsWidth);
            if (used && used + height > room) {
                starts.append(index);
                used = 0;
            }
            used += height;
        }
        page = 0;
        for (int index = 1; index < starts.size() && starts[index] <= wanted; ++index)
            page = index;
        pager->show();
        showPage(page);
    }

private:
    void showPage(int index)
    {
        page = index;
        const int first = starts[page];
        const int last = starts.value(page + 1, int(list->options().size()));
        list->showRows(first, last - first);
        list->setFixedHeight(room);
        pager->setPage(page, starts.size());
        overlay->card()->fitToWidth(width);
        overlay->centerCard();
    }

    ModalOverlay *overlay;
    RadioList *list;
    PagerBar *pager = nullptr;
    QVector<int> starts;
    int page = 0, width = 0, room = 0;
};
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
    card->rows()->addWidget(list);
    return { overlay, list };
}

void fitChoiceSheet(ModalOverlay *overlay, RadioList *list)
{
    if (!overlay->fitChoiceContents) {
        auto *contents = new ChoiceContents(overlay, list);
        overlay->fitChoiceContents = [contents] { contents->fit(); };
    }
    overlay->fitChoiceContents();
}

void presentChoiceSheet(ModalOverlay *overlay, RadioList *list)
{
    fitChoiceSheet(overlay, list);
    overlay->show();
    overlay->raise();
}
} // namespace kobalt::ui
