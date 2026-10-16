#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "reading/header_footer.h"
#include <QVector>
#include <array>
#include <functional>

namespace kobalt::ui {
class ListHeader;
}
class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace kobalt::reader {
using SlotContents = std::array<headerfooter::Content, headerfooter::slotCount>;

// The six slots as the header and footer draw them, around a few lines that
// stand for the page. The preset page shows it read-only; Custom taps a slot
// to choose what goes in it. Each slot is a button for input and accessibility.
class SlotMap final : public QWidget {
public:
    SlotMap(QWidget *parent, bool editable);
    void setSlots(const headerfooter::Layout &layout, const SlotContents &contents);
    QPushButton *cell(int slot) const;
    std::function<void(int)> chosen;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    class Cell;
    std::array<Cell *, headerfooter::slotCount> cells{};
    headerfooter::Layout layout;
    bool editable;
};

// A preset as a miniature page with its name below. A selected card has a
// heavy border and an inverted name; the card itself stays paper, so choosing
// a preset does not flash the whole panel.
class PresetCard final : public ui::Button {
public:
    PresetCard(headerfooter::Preset preset, QWidget *parent);
    headerfooter::Preset preset() const {
        return value;
    }
    void setLayout(const headerfooter::Layout &layout);
    void setDivider(bool shown);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    headerfooter::Preset value;
    headerfooter::Layout miniature;
    bool divider = false;
};

// Header and footer fills the reading area like Chapters: a bar with a close
// cell and the title takes the place of the header's action row, and the
// tabs stay. Its rows hold the presets and the slot controls. Close returns
// to the Layout panel.
class HeaderFooterView final : public ui::Panel {
public:
    explicit HeaderFooterView(QWidget *parent);
    QWidget *body() const {
        return content;
    }
    QVBoxLayout *rows() const {
        return contentRows;
    }
    std::function<void()> closed;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    ui::CellButton *close;
    QLabel *title;
    QWidget *content;
    QVBoxLayout *contentRows;
};

// One slot's choices, from Nothing to Time left in book, each with what it
// would show now. It fills the reading area like Chapters, with a check cell
// that returns to Header and footer. A choice applies at once and the list stays open.
// Uppercase for this slot sits where Chapters has its pager.
class FieldChooser final : public ui::Panel {
public:
    explicit FieldChooser(QWidget *parent);
    void open(int slot, headerfooter::Field current, const headerfooter::Values &values, bool uppercase);
    int slot() const {
        return currentSlot;
    }
    std::function<void(headerfooter::Field)> selected;
    // The slot's case: true for uppercase.
    std::function<void(bool)> uppercaseChanged;
    std::function<void()> done;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    class Row;
    QLabel *title, *position;
    ui::ListHeader *heading;
    QScrollArea *scroll;
    QVector<Row *> rows;
    QPushButton *finish;
    ui::Panel *options;
    QPushButton *uppercaseButtons[2] = {};
    int currentSlot = -1;
};
}
