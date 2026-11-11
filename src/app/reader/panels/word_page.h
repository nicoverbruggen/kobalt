#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/paged_text.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/controls/rows.h"
#include "storage/saved_words.h"
#include <QLabel>
#include <functional>

namespace kobalt {
// One saved word: the definition saved at lookup, the sentence it came from
// and a way back to that passage. Pages turn through a long definition.
class WordPage : public ui::Panel
{
public:
    explicit WordPage(QWidget *parent);
    // `action` names the button, such as "Go to page 5". Without one, `note`
    // replaces the button; by default it says the book is not on this device.
    void open(const SavedWord &word, const QString &action, const QString &note = {});
    void setFonts(const QString &bookFamily, const QString &definitionFamily,
                  int definitionPercent);

    const SavedWord &word() const { return current; }

    std::function<void(const SavedWord &)> passageRequested, removeRequested;
    std::function<void()> back;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    SavedWord current;
    QString bookFamily, action;
    // On a short screen the passage action moves into the bar as a cell.
    ui::CellButton *backButton, *remove, *passageCell;
    QLabel *headword, *context, *facts, *missing;
    ui::ListHeader *definitionHeading, *contextHeading;
    ui::PagedText *definition;
    QPushButton *passage;
    ui::PagerBar *pager;
    QFont bookFont(int pixels) const;
    void arrange();
};
} // namespace kobalt
