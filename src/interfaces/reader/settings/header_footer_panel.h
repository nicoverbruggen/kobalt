#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "reading/header_footer.h"
#include <QVector>
#include <array>
#include <functional>

namespace kobalt::ui {
class DescribedOption;
class ListHeader;
class RadioList;
class PagerBar;
}
class QLabel;
class QVBoxLayout;

namespace kobalt::reader {
using SlotContents = std::array<headerfooter::Content, headerfooter::slotCount>;

// The six slots as the header and footer draw them on a short page: header
// text at the top edge, footer text at the bottom edge above the progress bar,
// and a few lines between them. The preset page shows it read-only; Custom taps a slot
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
    // A modified Custom layout shows a star after its name.
    void setModified(bool value);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    headerfooter::Preset value;
    headerfooter::Layout miniature;
    bool divider = false, modified = false;
};

// Header and footer is a sheet over the bottom of the page, like Font: a
// heavy rule, a bold title and a close cell, then rows sized to their
// content, so the page shows above. Its rows hold the presets, the slots and
// the size and progress bar rows. Close returns to the Layout panel.
class HeaderFooterView final : public ui::Panel {
public:
    explicit HeaderFooterView(QWidget *parent);
    QWidget *body() const {
        return content;
    }
    QVBoxLayout *rows() const {
        return contentRows;
    }
    // The title row and the rows' height at this width.
    int preferredHeight(int width) const;
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
// would show now. It fills the reading area like Chapters. A choice applies
// and returns to Header and footer; the close cell returns without one.
// Fields that do not fit go to further pages, with the Chapters pager at the
// bottom. The Uppercase checkbox on the right of the bar keeps the list open.
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
    QLabel *title;
    ui::ListHeader *heading;
    ui::PagerBar *pager;
    ui::RadioList *list;
    QPushButton *finish;
    ui::DescribedOption *uppercase;
    // The row of the slot's field until the page that holds it is chosen.
    int currentSlot = -1, page = 0, openRow = -1;
    // The first row of each page.
    QVector<int> pageStarts() const;
    void arrange();
};
}
