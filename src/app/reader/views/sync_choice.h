#pragma once
#include "reading/chapter.h"
#include <QDateTime>
#include <QVector>
#include <QWidget>

class QPainter;

namespace kobalt::reader {
// What the KOSync position choice shows. Pages count from zero in this
// device's layout. A negative page is unknown.
struct SyncJump
{
    struct Start
    {
        int page = -1;
        QString title, parent;
        // A top-level entry of nested contents, such as a Book or a Part.
        bool top = false;
    };

    int here = -1, synced = -1, pages = 0;
    // Chapter starts in page order, at most one per page.
    QVector<Start> starts;
    bool nested = false;
    QString fromChapter, fromParent, toChapter, toParent;
    // From 0 to 100, or negative when unknown.
    double fromPercent = -1, toPercent = -1;
    QString device, time;

    // Both pages are known and differ, so the graph can show the jump.
    bool drawable() const { return here >= 0 && synced >= 0 && here != synced && pages > 1; }

    // Zero when either page is unknown.
    int direction() const
    {
        return here < 0 || synced < 0 ? 0 : synced > here ? 1 : synced < here ? -1 : 0;
    }

    // Both positions are in one chapter and the graph shows the jump.
    bool sameChapter() const;

    // Chapter starts that the jump crosses.
    int startsCrossed() const;
};

// Chapter starts from the table of contents. Entries that start on the same
// page are one start on screen, named by the last of them.
QVector<SyncJump::Start> syncChapterStarts(const QVector<ChapterLink> &links);
// The title of the entry that contains the chapter `title` starting at or
// before `page`, for nested contents. Empty for a top-level chapter.
QString syncChapterParent(const QVector<ChapterLink> &links, const QString &title, int page);
// "Today at 09:00", "Yesterday at 21:14" or the date and time.
QString syncTimeText(const QDateTime &time, const QDateTime &now);

// The jump graph and the From, To, Device and Synced rows below it.
class SyncChoiceView final : public QWidget
{
public:
    // Page boxes for a short jump, a track zoomed in on the jump, or the whole book.
    enum class Scale { Pages, Track, Book };

    explicit SyncChoiceView(QWidget *parent);

    void setJump(const SyncJump &jump);

    const SyncJump &jump() const { return value; }

    Scale scale() const;
    // The label above the arrow, such as "6 pages ahead · 3 chapters".
    QString jumpLabel() const;

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    // Each returns the height it uses. Without a painter it only measures.
    int pagesGraph(int width, QPainter *painter) const;
    int trackGraph(int width, QPainter *painter) const;
    int rows(int width, int top, QPainter *painter) const;
    int layout(int width, QPainter *painter) const;
    void paintJumpLabel(QPainter &painter, int center, int top, int width) const;

    SyncJump value;
};
} // namespace kobalt::reader
