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
    // `unsaved` is the open book's reading that the records do not hold yet.
    void setRecords(ActivityRecords records, const QDate &today,
                    const QMap<QDate, qint64> &unsaved);
    void setOpenBook(const ActivityOpenBook &book);
    void showSection(Section section);
    void openBook(const QString &book);

    Section section() const { return current; }

    bool bookOpen() const { return detail == Detail::Book; }

    const ActivityRecords &data() const { return records; }

    const QDate &today() const { return summary.today; }

    std::function<void()> closed;
    // The book page opened; the reader answers with setOpenBook() for its book.
    std::function<void(const QString &book)> bookRequested;
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
    // A book's page or every milestone, opened from a section with a back cell.
    enum class Detail { None, Book, Milestones };
    Detail detail = Detail::None;
    bool loading = true, unavailable = false;
    QWidget *header;
    QLabel *title;
    ui::CellButton *back, *close;
    QVector<ui::CellButton *> tabs;
    QLabel *message;
    class ActivityOverview *overview;
    class ActivityCalendar *calendar;
    class ActivityBooks *books;
    class ActivityRecordsPage *recordsPage;
    class ActivityBookPage *bookPage;
    class ActivityMilestones *milestonesPage;
};
} // namespace kobalt
