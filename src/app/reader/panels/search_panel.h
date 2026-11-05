#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/text.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "reading/chapter.h"
#include "engine/crengine/engine_document.h"
#include "platform/api/keyboard_controller.h"
#include <QLineEdit>
#include <QTimer>

namespace kobalt {
class SearchPanel : public ui::Panel
{
public:
    SearchPanel(QWidget *parent, KeyboardFactory makeKeyboard);
    std::function<void(QString)> requested;
    std::function<void(SearchHit)> selected;
    std::function<void()> dismissed, returnToStart;
    // `originalPage` is the zero-based page that Return to original position opens.
    // A supplied lookup query replaces the field and runs the normal search.
    void open(bool canReturn, int originalPage, const QString &initialQuery = {});
    // Excerpts are book text, so they use the family the book is displayed in.
    void setExcerptFont(const QString &family);
    void setChapters(const QVector<ChapterLink> &links);
    void append(const SearchBatch &batch, int totalPages);
    void stop();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    std::unique_ptr<KeyboardController> keyboard;
    ui::CellButton *back, *submit;
    ui::PagerBar *pager;
    QLineEdit *query;
    ui::CaptionButton *original;
    QPushButton *clear;
    QLabel *status, *empty, *hint;
    ui::CaptionButton *edit;
    QString submittedQuery, excerptFamily;
    QVector<ChapterLink> chapters;
    bool canReturn = false;
    QVector<SearchHit> hits;
    QVector<QPushButton *> buttons;
    QVector<ui::WholeLineLabel *> labels;
    QVector<QLabel *> metadata;
    int resultPage = 0, pageSize = 1;
    bool scanning = false;
    QTimer updates;
    QString pendingStatus;
    int rowHeight = 0, measuredWidth = 0;
    QVector<QString> renderedStarts;
    QVector<QSize> renderedSizes;
    QFont excerptFont() const;
    QString caption(const SearchHit &hit) const;
    void invalidateRows();
    void flush();
    void run();
    void redraw();
};
} // namespace kobalt
