#pragma once
#include <QString>
class QWidget;

namespace kobalt::ui {
class ModalOverlay;
class RadioList;

// A dimmed sheet with a heading and one pick-one list: the common form of a
// settings choice. The overlay is named `name` + "Popup" and the list `name`
// + "Choices".
struct ChoiceSheet
{
    ModalOverlay *overlay;
    RadioList *list;
};

// Builds the sheet over `host`, hidden. Add the options to the list, set its
// `chosen` and the overlay's `dismissed`, then call presentChoiceSheet(). A
// caller with work to do between fitting and showing calls fitChoiceSheet()
// and shows the overlay itself.
// Rows are at least a list action high; a list of one-line rows sets its own
// minimum. An empty `hint` leaves out the line under the title.
ChoiceSheet makeChoiceSheet(QWidget *host, const QString &name, const QString &title,
                            const QString &hint = {}, const QString &titleName = {},
                            const QString &hintName = {});

// Gives the list its height at the card's width, then fits and centers the
// card. Call it once the list has its options.
void fitChoiceSheet(ModalOverlay *overlay, RadioList *list);

// Fits the sheet, then shows and raises it.
void presentChoiceSheet(ModalOverlay *overlay, RadioList *list);
} // namespace kobalt::ui
