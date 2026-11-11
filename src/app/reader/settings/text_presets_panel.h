#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/panels.h"
#include "ui/style/font_preview.h"

#include <QHash>
#include <QImage>
#include <QTimer>
#include <QVector>
#include <functional>

class QLabel;

namespace kobalt::ui {
class CellButton;
class ListHeader;
class PagerBar;
} // namespace kobalt::ui

namespace kobalt::reader {
// What the Text panel and the All presets screen show of one preset.
struct TextPresetView
{
    QString id, name;
    // The size and spacing caption, such as "11 PT · 150%".
    QString detail;
    ui::FontPreviewSource font;
    // The preset's thickness steps, and its variable weight when the font
    // has a weight axis, so the card's sample is as heavy as the page text.
    int thickness = 0, axisWeight = 0;
};

// Calls `held` after a long press and suppresses the click that would follow.
class HoldTimer : public QObject
{
public:
    HoldTimer(QWidget *owner, std::function<void()> held);

    bool consumed() const { return fired; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QTimer timer;
    bool fired = false;
};

// The cell at the right of the Text presets bar. It names the preset in
// force and opens or closes the cards, like the book map toggle in Navigate:
// its own cell, with a rule on its left and a chevron.
class TextPresetToggle final : public ui::Button
{
public:
    explicit TextPresetToggle(QWidget *parent);
    void setState(const QString &label, bool open);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString label;
    bool open = false;
};

// A preset as "Aa" in its font with the name below, in the look of the
// header and footer preset cards. Save and All use the same card with an icon.
class TextPresetCard final : public ui::Button
{
public:
    explicit TextPresetCard(QWidget *parent);
    void setPreset(const TextPresetView &view, bool selected, bool modified);
    void setAction(ui::ControlIcon icon, const QString &name);
    void setDivider(bool shown);

    const QString &presetId() const { return id; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString id, name;
    ui::FontPreviewSource font;
    int thickness = 0, axisWeight = 0;
    ui::ControlIcon icon = ui::ControlIcon::Plus;
    bool action = false, selected = false, modified = false, divider = true;
};

// The top of the Text panel: the Text presets bar, and when open, the cards
// and, after a change, Update and Save as new. The reader supplies the
// presets and handles the requests.
class TextPresetSection final : public QWidget
{
public:
    // The presets the row shows before All.
    static constexpr int cardCount = 4;

    explicit TextPresetSection(QWidget *parent);
    // `selected` is the preset in force, or empty. `cards` are the presets the
    // row shows, in order.
    void setState(const QVector<TextPresetView> &cards, const QString &selected,
                  const QString &selectedName, bool modified, bool open);

    bool isOpen() const { return open; }

    QSize sizeHint() const override;

    std::function<void()> toggled, allRequested, updateRequested, saveAsRequested;
    std::function<void(const QString &)> chosen, held;

    TextPresetToggle *toggle;
    QVector<TextPresetCard *> cards;
    TextPresetCard *allCard;
    ui::Button *updateButton, *saveAsButton;

protected:
    void resizeEvent(QResizeEvent *) override;
    void paintEvent(QPaintEvent *) override;

private:
    void arrange();
    ui::ListHeader *header;
    QWidget *row, *actions;
    bool open = false, modified = false;
};

// All presets: a full-screen grid of large previews, six to a page or three
// on a phone in portrait, with a
// title and close cell like the sleep screen gallery. Each preview is the
// reader's own rendering of a sample paragraph in that preset. The last tile
// saves the current text.
class TextPresetGallery final : public ui::Panel
{
public:
    explicit TextPresetGallery(QWidget *parent);
    void setPresets(const QVector<TextPresetView> &presets, const QString &selected, bool canSave,
                    const QString &currentDetail);
    // Text for new books reuses the grid with its own title and hint.
    void setHeading(const QString &title, const QString &hint);
    // The reader renders a page's previews in the background and hands them
    // over together, so the grid repaints once.
    void setPreviews(const QHash<QString, QImage> &images);
    void clearPreviews();
    QSize previewSize() const;
    // Previews the reader should render for the page on show.
    QStringList visibleIds() const;

    std::function<void(const QString &)> chosen, held;
    std::function<void()> closed, saveRequested, pageChanged;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    friend class TextPresetGrid;
    void showPage(int page);
    // Two tiles side by side, or one on a narrow screen such as a phone in
    // portrait. Three rows either way.
    int columns() const;
    int perPage() const;
    // The page of the preset in force, or the current page without one.
    int selectedPage() const;
    int pageCount() const;
    int tileCount() const;

    QVector<TextPresetView> presets;
    QHash<QString, QImage> previews;
    // The preview size the previews were rendered for. A resize that keeps
    // it keeps them.
    QSize renderedSize;
    QString selected, currentDetail;
    bool canSave = true;
    QLabel *title;
    ui::ListHeader *hint;
    class TextPresetGrid *grid;
    ui::PagerBar *pager;
    int page = 0, arrangedPerPage = 0;
};
} // namespace kobalt::reader
