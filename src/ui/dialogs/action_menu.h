#pragma once
#include "modals.h"
#include "ui/controls/icons.h"
#include <functional>

namespace kobalt::ui {
// What an action menu is about: a thumbnail, such as a book cover, a bold
// title, a detail line and an uppercase caption. Every part is optional.
struct ActionMenuHeader
{
    QImage image;
    QString title, detail, caption;
};

struct ActionMenuItem
{
    QString text, name;
    // Rows without an icon align their label with the icon rows' labels.
    bool hasIcon = false;
    ControlIcon icon = ControlIcon::More;
    std::function<void()> triggered;
    bool chevron = false;
};

inline ActionMenuItem actionItem(const QString &text, const QString &name, ControlIcon icon,
                                 std::function<void()> triggered, bool chevron = false)
{
    return { text, name, true, icon, std::move(triggered), chevron };
}

// A card with a header and a list of actions, one full-width row each, with
// dividers between them. Choosing a row calls `dismissed`, then the row's
// action, so an action can open another modal. Tapping outside the card or
// Escape calls `dismissed` only. Center the card with fitCard(), or place it
// next to the control that opened it with anchorTo().
class ActionMenu final : public ModalOverlay
{
public:
    ActionMenu(QWidget *parent, const ActionMenuHeader &header,
               const QVector<ActionMenuItem> &items);
    QPushButton *action(const QString &name) const;

    const QVector<QPushButton *> &actions() const { return rows; }

private:
    QVector<QPushButton *> rows;
};
} // namespace kobalt::ui
