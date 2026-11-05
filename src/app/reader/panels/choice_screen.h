#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/text.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace kobalt {
// `page` is the zero-based page a chapter starts on, or -1 when it is unknown.
struct Choice
{
    QString label, value, name;
    QString section = {};
    int depth = 0;
    int page = -1;
    QString detail = {};
    bool placeholder = false;
    QStringList fontFiles = {};
};

// The chapter list covers the header's action row with its own Chapters bar.
// Its close cell and the Chapters tab both close it.
class ChapterContents : public ui::Panel
{
public:
    explicit ChapterContents(QWidget *parent);
    void showChapters(const QString &title, QVector<Choice> choices, const QString &current);
    void setPosition(const QString &text);

    int currentPage() const { return page; }

    std::function<void(const QString &)> selected;
    std::function<void()> back, changed;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void arrange();
    void redraw();
    int pageCount() const;
    QVector<Choice> items;
    QVector<QPushButton *> buttons;
    QVector<QLabel *> labels;
    QString current;
    QLabel *title, *position, *summary, *pageHeading, *empty;
    ui::ListHeader *listHeader;
    ui::CellButton *close;
    ui::PagerBar *pager;
    int page = 0, rows = 1;
};

class ChoiceScreen : public kobalt::ui::Panel
{
public:
    enum class Mode { Chapters, Fonts, Sizes };
    std::function<void(const QString &)> selected;
    std::function<void()> back, changed;
    std::function<void(const QString &, bool instructions)> defaultRequested;
    QJsonObject fontSamples() const;
    void setFontAppearance(const ui::SampleAppearance &appearance);
    explicit ChoiceScreen(QWidget *parent);
    void showChoices(const QString &heading, QVector<Choice> choices, const QString &current = {},
                     Mode presentation = Mode::Chapters);
    void setDropdownGeometry(const QRect &bounds);
    void setChapterPosition(const QString &text);
    void selectValue(const QString &value);
    int chapterPage() const;
    int fontPanelHeight() const;

    bool isFontPicker() const { return mode == Mode::Fonts; }

    void setFontGeometry(const QRect &bounds);

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    int pageCount() const;
    int itemIndex(int row) const;
    int pageForIndex(int index) const;
    void layoutChoices();
    QVector<Choice> items;
    ui::SampleAppearance fontAppearance;
    QVector<QVector<int>> fontPages;
    QVector<QPushButton *> buttons;
    QVector<kobalt::ui::SampleLabel *> labels;
    QVector<ui::ListRow *> sizeRows;
    kobalt::ui::PageNavigation navigation;
    QString selectedValue;
    Mode mode = Mode::Chapters;
    int page = 0, pageSize = 1;
    bool fittingHeight = false;
    void redraw();
    ChapterContents *contents;
    void redrawFonts();
    ui::Panel *fontHeader;
    QPushButton *fontClose;
    QLabel *fontTitle, *fontSection;
    ui::Panel *fontSectionBar;
    ui::PagerBar *fontPager;
};

} // namespace kobalt
