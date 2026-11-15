#include "reader_p.h"
#include "platform/api/kobo_refresh.h"
#include "reading/reading_activity.h"
#include "reading/reading_progress.h"
#include "support/strings.h"
#include "ui/style/styles.h"
#include <QDateTime>
#include <QUrl>

namespace kobalt::reader {
using kobalt::platform::textRefresh;

namespace {
// Today's reading for the Activity row: "Just started reading", "30 min
// today" or "1h 30m today".
QString todayCaption(qint64 milliseconds)
{
    const qint64 minutes = qMax<qint64>(0, (milliseconds + 30000) / 60000);
    if (!minutes)
        return strings::get("activity.row_started");
    return minutes < 60 ? strings::get("activity.row_minutes").arg(minutes)
                        : strings::get("activity.row_hours").arg(minutes / 60).arg(minutes % 60);
}

// Rows and pages appear as the records arrive, so each load prepares them
// for the e-ink update again.
void prepareRefresh(QWidget *view)
{
    textRefresh(view);
    for (auto *widget : view->findChildren<QWidget *>())
        textRefresh(widget);
}
} // namespace

QDate Reader::activityToday() const
{
    return activity::localDate(services.activityMilliseconds ? services.activityMilliseconds()
                                                             : QDateTime::currentMSecsSinceEpoch());
}

// Stats loads the history when it opens, for the Activity row's summary and
// for Activity when it is open. The read runs on the profile writer thread.
void Reader::loadActivity()
{
    if (!book || !book->tracksReadingHistory())
        return;
    if (!activityRecords)
        activityRecords = std::make_unique<ActivityRecordStore>(book->paths().directory);
    // The summary is computed with the read, off the interface thread: years
    // of sessions take a while on a Kobo.
    const auto today = activityToday();
    const auto unsaved = book->readingDays();
    auto summary = std::make_shared<activity::Summary>();
    QPointer<Reader> guard(this);
    activityRecords->load(
            book->readingOpeningId(),
            [summary, today, unsaved](const ActivityRecords &records) {
                *summary = activity::summarize(records, today, unsaved);
            },
            [guard, summary](bool ok, ActivityRecords records) {
                if (!guard || !guard->book)
                    return;
                if (!ok) {
                    if (guard->activityView)
                        guard->activityView->setUnavailable();
                    return;
                }
                const auto time = todayCaption(summary->todayMilliseconds);
                guard->chapterGraph->setActivity(summary->current.days >= 2
                                                         ? strings::get("activity.row_streak")
                                                                   .arg(time)
                                                                   .arg(summary->current.days)
                                                         : time);
                if (!guard->activityView)
                    return;
                guard->activityView->setRecords(std::move(records), std::move(*summary));
                prepareRefresh(guard->activityView);
            });
}

// Hiding keeps the book's time and only takes it out of the lists, so it
// needs no confirmation. Both changes are written on the profile writer.
void Reader::hideActivityBook(const QString &key, bool hidden)
{
    if (!activityRecords || !book)
        return;
    QPointer<Reader> guard(this);
    activityRecords->hide(key, hidden, [guard](bool) {
        if (guard)
            guard->loadActivity();
    });
}

void Reader::deleteActivityBook(const QString &key, const QString &title)
{
    if (!activityRecords || !book || key == wordsBook())
        return;
    auto *choice = new ui::ChoiceModal(
            this,
            { strings::get("activity.delete_detail").arg(title),
              strings::get("activity.delete_title"),
              {} },
            { strings::get("common.cancel"), "rrActivityKeepBook" },
            { strings::get("activity.delete_confirm"), "rrActivityConfirmDelete" });
    choice->setObjectName("rrActivityDelete");
    choice->setDimmed(true);
    choice->setGeometry(rect());
    ui::readerStyle(choice->descriptionLabel(),
                    ui::styles::font(ui::metrics::chrome::descriptionText)
                            + "color:palette(placeholder-text);");
    QPointer<ui::ChoiceModal> modal(choice);
    QPointer<Reader> guard(this);
    const auto finish = [modal, guard, key](bool remove) {
        if (!modal)
            return;
        modal->hide();
        modal->deleteLater();
        if (!remove || !guard || !guard->activityRecords || !guard->book)
            return;
        if (guard->activityView)
            guard->activityView->closeBook();
        guard->activityRecords->remove(key, guard->book->readingOpeningId(), [guard](bool) {
            if (guard)
                guard->loadActivity();
        });
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, choice, [finish] { finish(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, choice, [finish] { finish(true); });
    choice->dismissed = [finish] { finish(false); };
    choice->card()->fitToWidth(qMin<int>(ui::metrics::notice::cardWidth,
                                         width() - 2 * ui::metrics::notice::sideClearance));
    choice->centerCard();
    prepareRefresh(choice);
    choice->show();
    choice->raise();
    refresh();
}

void Reader::openActivity()
{
    if (activityView || !book || !book->tracksReadingHistory())
        return;
    session->cancelPendingTurn();
    activityView = new ActivityView(this);
    activityView->setGeometry(textPresetGalleryBounds());
    activityView->closed = [this] { closeActivity(); };
    activityView->setCurrentBook(wordsBook());
    activityView->hideRequested = [this](const QString &key, bool hidden) {
        hideActivityBook(key, hidden);
    };
    activityView->deleteRequested = [this](const QString &key, const QString &title) {
        deleteActivityBook(key, title);
    };
    activityView->bookRequested = [this](const QString &key) { mapActivityBook(key); };
    activityView->coversRequested = [this](const QVector<activity::BookTotal> &books, QSize size) {
        loadActivityCovers(books, size);
    };
    prepareRefresh(activityView);
    activityView->show();
    activityView->raise();
    // The open book's routine save runs now instead of later, in the
    // background, so Activity lists the current session with today's time.
    // The writer thread runs jobs in the order they are posted, so the save
    // lands before the read.
    book->saveReadingActivity();
    loadActivity();
    if (!preferences.value(QStringLiteral("activity_welcome_seen")).toBool(false))
        showActivityWelcome();
    refresh();
}

// History lives on this device only and does not sync, so the first opening
// says so once. The choice is saved on the writer thread; a failed save shows
// the welcome again next time, which costs nothing.
void Reader::showActivityWelcome()
{
    if (activityWelcome)
        return;
    auto *notice = new ui::NoticeModal(this,
                                       { strings::get("activity.welcome_description"),
                                         strings::get("activity.welcome_title"),
                                         {},
                                         "rrActivityWelcomeTitle" },
                                       { strings::get("common.ok"), "rrActivityWelcomeClose" });
    activityWelcome = notice;
    notice->setObjectName("rrActivityWelcome");
    notice->descriptionLabel()->setObjectName("rrActivityWelcomeText");
    notice->setGeometry(rect());
    notice->dismissed = [this] { closePopup(activityWelcome); };
    connect(notice->primaryButton(), &QPushButton::clicked, this,
            [this] { closePopup(activityWelcome); });
    notice->card()->fitToWidth(qMin<int>(ui::metrics::notice::cardWidth,
                                         width() - 2 * ui::metrics::notice::sideClearance));
    notice->centerCard();
    prepareRefresh(notice);
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    preferences[QStringLiteral("activity_welcome_seen")] = true;
    // Do not overtake a pending dark mode, button or graph save, as the other
    // settings do; the welcome then shows once more on the next opening. The
    // save edits only this field.
    if (savingButtonControls || savingDarkMode >= 0 || savingDenseGraph)
        return;
    preferencesWriter.post(
            [path = book->paths().preferences()] {
                return kobalt::storage::updatePreferences(
                        path, qt::object({ { "activity_welcome_seen", false } }),
                        qt::object({ { "activity_welcome_seen", true } }));
            },
            {});
}

// Covers come from the book files. A sideloaded book's identity is its file;
// otherwise the source key names it when it is a path. Other books, such as
// Android's document-provider books, keep the placeholder.
void Reader::loadActivityCovers(const QVector<activity::BookTotal> &books, QSize size)
{
    if (!activityView)
        return;
    if (!activityCovers)
        activityCovers = std::make_unique<library::CoverThumbnails>(
                book->paths().previews() + QStringLiteral("/activity-covers"));
    QVector<QPair<QString, QString>> files;
    for (const auto &item : books) {
        const QUrl identity(item.identity);
        const auto file = identity.isLocalFile() ? identity.toLocalFile()
                : item.sourceKey.startsWith('/') ? item.sourceKey
                                                 : QString();
        files.append({ item.book, file });
    }
    QPointer<Reader> guard(this);
    activityCovers->request(files, size,
                            [guard](const QString &key, const QImage &cover, bool missing) {
                                if (guard && guard->activityView)
                                    guard->activityView->setCover(key, cover, missing);
                            });
}

void Reader::closeActivity()
{
    if (!activityView)
        return;
    activityView->hide();
    activityView->deleteLater();
    activityView.clear();
    // Covers still being read are not needed any more; the loader stops
    // without waiting, and the disk cache keeps the covers already read.
    activityCovers.reset();
    refresh();
}

// The open book's sessions get their place in the current layout, so its page
// can show how far each session went and the progress over time.
void Reader::mapActivityBook(const QString &key)
{
    if (!activityView || key != wordsBook())
        return;
    const auto sessions = activity::bookSessions(activityView->data(), key, activityToday());
    QStringList positions;
    for (const auto &item : sessions)
        positions << item.startPosition << item.endPosition;
    ActivityOpenBook open;
    open.book = key;
    open.percent =
            book->finishedHere() ? 100 : pageProgressPercent(session->page(), session->pages());
    open.remainingMilliseconds = book->estimates().bookMilliseconds;
    activityView->setOpenBook(open);
    if (positions.isEmpty())
        return;
    QPointer<Reader> guard(this);
    session->bookmarkPages(positions, [guard, sessions, open](QVector<int> pages) mutable {
        if (!guard || !guard->activityView || pages.size() != 2 * sessions.size())
            return;
        const int total = guard->session->pages();
        for (int index = 0; index < sessions.size(); ++index) {
            const int start = pages[2 * index], end = pages[2 * index + 1];
            if (start < 0 || end < 0 || total <= 0)
                continue;
            open.ranges.insert(
                    sessions[index].started,
                    { pageProgressPercent(start, total), pageProgressPercent(end, total) });
        }
        guard->activityView->setOpenBook(open);
        prepareRefresh(guard->activityView);
        guard->refresh();
    });
}
} // namespace kobalt::reader
