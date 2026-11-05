#pragma once
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "reading/chapter.h"
#include <QVector>

namespace kobalt {
class ChapterBars;
class StatsCell;

// Stats: the current chapter, a chart of every chapter's length and three
// figures. Page figures do not need word counts; only the chart waits for them.
class ChapterGraph : public ui::Panel
{
public:
    explicit ChapterGraph(QWidget *parent);
    void setData(QVector<ChapterLink> chapters, ChapterSelection selection, bool ready,
                 const QString &error);
    void setProgress(const QString &title, int chapterPage, int chapterPages, int nextChapterPages,
                     int page, int pages);
    // `number` counts from 1 in the contents; 0 when the position is before them.
    void setChapterNumber(int number, int count);
    // `words` is the shared whole-book total, or -1 when counting failed.
    // The times are the rest of
    // the chapter and of the book at the session's pace, or -1 without a pace.
    void setReading(int minutes, qint64 words, qint64 chapterMilliseconds = -1,
                    qint64 bookMilliseconds = -1);
    // Dense charts show every chapter on one row; sparse charts page 20 at a time.
    void setDense(bool value);
    void open();

    void setTopPadding(int padding)
    {
        topPadding = padding;
        arrange();
    }

    // The height the panel may take. A short screen, such as a phone in
    // landscape, shortens the chart; the chapter row and the figures keep theirs.
    void setAvailableHeight(int height)
    {
        availableHeight = height;
        arrange();
    }

    int graphPage() const;
    int graphPages() const;
    std::function<void()> changed;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    QVector<ChapterLink> chapters;
    ChapterSelection selection;
    int first = 0;
    int topPadding = 0, availableHeight = 0;
    // The chart's height: the design height, or less on a short screen.
    int chartHeight() const;
    bool ready = false, dense = true;
    qint64 words = -1, chapterTime = -1, bookTime = -1;
    int minutes = 0;

    struct Progress
    {
        int chapterPage = 0, chapterPages = 0, nextChapterPages = 0, page = 0, pages = 0;
    } progress;

    QString error;
    QLabel *message, *chapterCaption, *chapterPage, *chapterTitle;
    QString title;
    int progressWeight = -1;
    ui::PagerBar *pager;
    ChapterBars *bars;
    StatsCell *chapterCell, *bookCell, *sessionCell;
    int capacity() const;
    void refreshCells();
    bool paged() const;
    int chartTop() const;
    void arrange();
    void redraw(bool paging = false);
};
} // namespace kobalt
