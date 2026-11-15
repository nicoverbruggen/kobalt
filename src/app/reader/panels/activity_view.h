#pragma once
#include "reading/reading_activity.h"
#include "ui/controls/panels.h"
#include <QHash>
#include <QImage>
#include <QSize>
#include <QPair>
#include <QVector>
#include <functional>

namespace kobalt {
namespace ui {
class CellButton;
class ListHeader;
class PagerBar;
} // namespace ui

class StatsCell;

// What only the open book can add to its page: where each session started and
// ended in the current layout, and the time left at the reader's pace.
struct ActivityOpenBook
{
    QString book;
    // Percent at the start and end of each session, by its start time.
    QHash<qint64, QPair<int, int>> ranges;
    // The book's progress now.
    int percent = -1;
    // -1 without an estimate.
    qint64 remainingMilliseconds = -1;
};

// Activity: a full-screen view of the reading history with Overview,
// Calendar, Books and Records tabs, and a page for each book. It shows what
// the reader hands it and reads nothing itself.
class ActivityView final : public ui::Panel
{
public:
    enum class Section { Overview, Calendar, Books, Records };
    explicit ActivityView(QWidget *parent);
    // Until the records arrive.
    void setLoading();
    void setUnavailable();
    // The records and their summary, which the caller computes off the
    // interface thread.
    void setRecords(ActivityRecords records, activity::Summary summary);
    void setOpenBook(const ActivityOpenBook &book);
    // The book open in the reader. Its activity cannot be deleted while it
    // is open: the reader is still writing it.
    void setCurrentBook(const QString &book);
    void showSection(Section section);
    void openBook(const QString &book);
    // Leaves a book's page for where it opened from.
    void closeBook(const QString &book);

    Section section() const { return current; }

    bool bookOpen() const { return detail == Detail::Book; }

    const ActivityRecords &data() const { return records; }

    const QDate &today() const { return summary.today; }

    std::function<void()> closed;
    // The book page opened; the reader answers with setOpenBook() for its book.
    std::function<void(const QString &book)> bookRequested;
    // A book's page asked to hide the book from the lists or show it again.
    std::function<void(const QString &book, bool hidden)> hideRequested;
    // A book's page asked to delete its activity. The reader confirms first.
    std::function<void(const QString &book, const QString &title)> deleteRequested;
    // The books on the shown page of Books whose covers are not known yet,
    // and the cover size. The reader answers with setCover() for each.
    std::function<void(const QVector<activity::BookTotal> &books, QSize size)> coversRequested;
    // `missing` marks a book whose file is no longer on the device.
    void setCover(const QString &book, const QImage &cover, bool missing);

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void arrange();
    ActivityRecords records;
    activity::Summary summary;
    ActivityOpenBook openBookData;
    QString bookKey;
    Section current = Section::Overview;
    // A book's page, every milestone or the hidden books, opened from a
    // section with a back cell. A book's page returns to where it opened.
    enum class Detail { None, Book, Milestones, Hidden };
    Detail detail = Detail::None, bookReturn = Detail::None;
    QString currentBook;
    bool loading = true, unavailable = false;
    QWidget *header;
    QLabel *title;
    ui::CellButton *back, *close, *hideCell, *deleteCell;
    QVector<ui::CellButton *> tabs;
    QLabel *message;
    class ActivityOverview *overview;
    class ActivityCalendar *calendar;
    class ActivityBooks *books, *hiddenPage;
    class ActivityRecordsPage *recordsPage;
    class ActivityBookPage *bookPage;
    class ActivityMilestones *milestonesPage;
};
} // namespace kobalt
