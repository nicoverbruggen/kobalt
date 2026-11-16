#include "support/qt_compat.h"
#include "platforms/kobo/nickel-qt5/native_types.h"
#include "platforms/kobo/common/native_lifecycle.h"
#include "platforms/kobo/common/native_database_wait.h"
#include "platforms/kobo/common/publication_source.h"
#include "platforms/kobo/common/native_reading_session.h"
#include "platforms/kobo/common/native_session_volume.h"
#include "native_location.h"
#include "session_calls.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include <QElapsedTimer>
#include <QDateTime>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QVariantMap>
#include <cstddef>
#include <dlfcn.h>
#include <limits>
#include <utility>
#ifndef KOBALT_READER_HARDWARE
#  include <QFile>
#endif

namespace kobalt {
namespace {
#ifndef KOBALT_READER_HARDWARE
NativeReadingSession *testActive = nullptr;
#endif
// In 4.46.23836, the two session wrappers read only PluginState's double
// at offset 40. Nickel formats it for Kobo Plus; no native reader is created.
struct SessionPosition
{
    char unused[40]{};
    double percent = 0;
};

static_assert(offsetof(SessionPosition, percent) == 40);

struct Api
{
    Volume (*get)(const QString &, const QString &) =
            symbol<decltype(get)>("_ZN13VolumeManager7getByIdERK7QStringS2_");
    void (*percent)(void *, const QVariant &) =
            symbol<decltype(percent)>("_ZN7Content14setPercentReadERK8QVariant");
    void (*bookmark)(void *, const QString &, int, const QDateTime &) = symbol<decltype(bookmark)>(
            "_ZN13VolumeManager14updateBookmarkER6VolumeRK7QStringiRK9QDateTime");
    QVariantMap (*values)(const void *) = symbol<decltype(values)>("_ZNK6Volume11getDbValuesEv");
    void *(*device)() = symbol<decltype(device)>("_ZN6Device16getCurrentDeviceEv");
    const QString &(*dbName)(const void *) = symbol<decltype(dbName)>("_ZNK6Device9getDbNameEv");
    bool (*tolino)(const void *) = symbol<decltype(tolino)>("_ZNK6Device8isTolinoEv");
    QSqlDatabase (*connection)(QString, bool) =
            symbol<decltype(connection)>("_ZN15DatabaseAdapter12dbConnectionE7QStringb");
    QObject *(*engine)() = symbol<decltype(engine)>("_ZN11EventEngine14sharedInstanceEv");
    nickel_qt5::SessionCalls<Volume, SessionPosition> sessions{ [](const char *name) {
        return dlsym(RTLD_DEFAULT, name);
    } };
    void (*fire)(QObject *, const char *, const Volume &, const QVariantMap &) =
            symbol<decltype(fire)>(
                    "_ZN11EventEngine9fireEventEPKcRK6VolumeRK4QMapI7QString8QVariantE");
    int (*time)(const Volume *) = symbol<decltype(time)>("_ZNK6Volume16timeSpentReadingEv");
    double (*pace)(const Volume &) =
            symbol<decltype(pace)>("_ZN22ReadingVelocityTracker14secondsPerWordERK6Volume");
    void (*setTime)(Volume *, const QVariant &) =
            symbol<decltype(setTime)>("_ZN6Volume19setTimeSpentReadingERK8QVariant");
    bool (*begin)(QString) =
            symbol<decltype(begin)>("_ZN15DatabaseAdapter16beginTransactionE7QString");
    bool (*commit)(QString, bool) =
            symbol<decltype(commit)>("_ZN15DatabaseAdapter14endTransactionE7QStringb");
    bool (*update)(const QString &, const QVariantMap &, const QVariantMap &, const QString &) =
            symbol<decltype(update)>(
                    "_ZN13VolumeManager12updateFieldsERK7QStringRK4QMapIS0_8QVariantES7_S2_");
    void *(*refresh)(const QString &) = symbol<decltype(refresh)>(
            "_ZN10Repository12refreshCacheI6VolumeEEP5CacheIT_ERK7QString");
    // The size check validates Volume, but exports do not validate the
    // inspected PluginState offset on a different firmware build.
    bool archiveAvailable = native::publicationSource().available();

    bool ready() const
    {
        return archiveAvailable
                && QMetaType::sizeOf(QMetaType::type("Volume")) == int(sizeof(Volume)) && get
                && percent && bookmark && values && device && dbName && tolino && connection
                && engine && sessions.ready() && fire && time && setTime && begin && commit
                && update && refresh && symbol<void *>("_ZN6VolumeD1Ev");
    }

    bool open() const
    {
        const auto db = connection(dbName(device()), false);
        return db.isValid() && db.isOpen();
    }
};

const Api &api()
{
    static const Api value;
    return value;
}

void prepareVolume(Volume &volume, const ReadingActivity &activity)
{
    const auto original = api().values(&volume);
    nickel::prepareSessionVolume(
            volume, activity,
            [&original](Volume &value, const QString &position) {
                const auto location = nickel_qt5::stored(position);
                if (!location.isEmpty())
                    api().bookmark(&value, location, original.value("adobe_location").toInt(),
                                   QDateTime::currentDateTimeUtc());
            },
            [](Volume &value, double percent) { api().percent(&value, QVariant(percent)); });
}

bool workerReady()
{
    auto *engine = api().engine();
    auto *thread = engine ? engine->thread() : nullptr;
    return thread && thread != QThread::currentThread() && thread->isRunning();
}

// Once per run with verbose diagnostics: which connections a reading-time
// transaction of its own uses on the GUI thread, and Nickel's busy timeouts.
void describeConnections()
{
    static bool described = false;
    if (described || !diagnosticsActive())
        return;
    described = true;
    const auto describe = [](const QString &name) {
        auto connection = api().connection(name, true);
        QSqlQuery query(connection);
        const int timeout = query.exec(QStringLiteral("PRAGMA busy_timeout")) && query.next()
                ? query.value(0).toInt()
                : -1;
        return kobalt::qt::object({ { "connection", connection.connectionName() },
                                    { "database", connection.databaseName() },
                                    { "busy_timeout_ms", timeout } });
    };
    const auto &a = api();
    diagnostic("native-db-connections",
               kobalt::qt::object({ { "default", describe(QString()) },
                                    { "named", describe(a.dbName(a.device())) } }));
}

} // namespace

struct NativeReadingSession::Record
{
    explicit Record(const QString &book) : volume(api().get(book, api().dbName(api().device()))) { }

    Volume volume;
};

NativeReadingSession::NativeReadingSession(QString identity) : book(std::move(identity))
{
    pace_.changed = [this] {
        if (estimatesChanged) {
            estimatesChanged();
        }
    };
}

NativeReadingSession::~NativeReadingSession()
{
    if (running || ending || pendingSeconds >= 0)
        diagnostic("reading-session-disposed-unsaved", {}, DiagnosticSeverity::Error);
}

bool NativeReadingSession::available()
{
    return api().ready();
}

void NativeReadingSession::start(const ReadingActivity &activity)
{
    if (running)
        return;
    if (ending) {
        // Nickel's session is still open: the reader was hidden, not left.
        running = true;
        ending = false;
        startPace();
        diagnostic("reading-session-resume");
        return;
    }
    if (!available() || !api().open() || !workerReady() || api().tolino(api().device()))
        return;
    auto *engine = api().engine();
    auto volume = api().get(book, api().dbName(api().device()));
    if (api().values(&volume).value("ContentID").toString() != book)
        return;
    percent = qBound(0.0, activity.percent, 100.0);
    activity_.start(activity);
    // The first page event of this session looks the record up again.
    forgetRecord();
    prepareVolume(volume, activity);
    SessionPosition state;
    state.percent = percent;
    QElapsedTimer timer;
    timer.start();
    api().sessions.start(engine, volume, state, { { "ViewType", "ReadingView" } });
    // Nickel records detailed session timestamps separately from reading time.
    api().fire(engine, "readerShown", volume, {});
    running = true;
#ifndef KOBALT_READER_HARDWARE
    testActive = this;
#endif
    startPace();
    diagnostic("reading-session-start",
               kobalt::qt::object({ { "milliseconds", timer.elapsed() } }));
}

void NativeReadingSession::page(const ReadingActivity &activity)
{
    percent = qBound(0.0, activity.percent, 100.0);
    if (!running)
        return;
    int words = 0;
    if (!activity_.observe(activity, &words))
        return;
    report(words);
}

void NativeReadingSession::forgetRecord()
{
    record_.reset();
}

void NativeReadingSession::report(int words)
{
    // Nickel's own reader keeps its Volume for the whole session. Look the
    // record up for the first page event and keep it until useRecord() or a
    // new session drops it, so a page turn does not read Nickel's cache and
    // copy the row.
    if (!record_) {
        std::unique_ptr<Record> found(new Record(book));
        if (api().values(&found->volume).value("ContentID").toString() != book)
            return;
        record_ = std::move(found);
    }
    // Nickel handles page activity and subscription accounting. Its event
    // engine intentionally ignores page events in sideload mode.
    api().fire(api().engine(), "pageTurned", record_->volume, { { "wordsRead", words } });
    if (!cacheRecord)
        record_.reset();
}

bool NativeReadingSession::estimates(const ReadingActivity &activity, ReadingEstimates *result)
{
    if (!api().pace) {
        return false;
    }
    *result = nickel::readingEstimates(activity, pace_.value());
    return true;
}

void NativeReadingSession::startPace()
{
    if (!api().pace)
        return;
    const auto identity = book;
    pace_.start(api().engine(), [identity] {
        if (!api().open())
            return -1.0;
        const auto volume = api().get(identity, api().dbName(api().device()));
        return api().values(&volume).value("ContentID").toString() == identity ? api().pace(volume)
                                                                               : -1.0;
    });
}

void NativeReadingSession::stop()
{
    pace_.stop();
    // Hold no Nickel record while the reader is hidden, asleep or closing.
    forgetRecord();
#ifndef KOBALT_READER_HARDWARE
    if (testActive == this)
        testActive = nullptr;
#endif
    // Nickel's own reader keeps its session open under a dialog. It ends the
    // session only when it leaves the book or the device sleeps, inside the
    // transaction that saves the progress and the reading time. A stop only
    // pauses: endIn() or endNow() ends the session at that boundary.
    if (running) {
        running = false;
        ending = true;
        diagnostic("reading-session-pause");
    }
}

void NativeReadingSession::endSession()
{
    if (!ending)
        return;
    // Nickel's event thread runs at idle priority, so nothing here waits for
    // it. endReadingSession only queues the session's events there; Nickel
    // saves them on its own schedule and at shutdown, as it does for its own
    // reader. Called inside a transaction, that thread's write waits for the
    // commit instead of holding the database before it. A separate Volume
    // keeps the session's position out of the row that the transaction saves.
    auto copy = api().get(book, api().dbName(api().device()));
    prepareVolume(copy, activity_.current());
    SessionPosition state;
    state.percent = percent;
    const int seconds = api().sessions.end(api().engine(), copy, state, {});
    api().fire(api().engine(), "readerHidden", copy, {});
    ending = false;
    if (seconds > 0)
        pendingSeconds = qMax(0, pendingSeconds) + seconds;
    diagnostic("reading-session-stop", kobalt::qt::object({ { "seconds", seconds } }));
}

void NativeReadingSession::endIn(void *target)
{
    auto &volume = *static_cast<Volume *>(target);
    endSession();
    // A delta of a day or more is not a reading session.
    if (pendingSeconds <= 0 || pendingSeconds >= 86400)
        return;
    const auto total = qint64(api().time(&volume)) + pendingSeconds;
    if (total <= std::numeric_limits<int>::max())
        api().setTime(&volume, QVariant(int(total)));
}

void NativeReadingSession::saved(bool committed)
{
    if (committed && !ending)
        pendingSeconds = -1;
}

bool NativeReadingSession::endNow()
{
    stop();
    if (!ending && pendingSeconds < 0)
        return true;
    kobalt::mainthread::BlockingOperation operation("nickel-reading-time");
    const auto &a = api();
    // The same gate as the progress write, which this replaces when no
    // progress was written.
    if (!NativeLifecycle::nativeProgressSaveAllowed() || !a.open()) {
        endSession();
        return false;
    }
    const auto db = a.dbName(a.device());
    describeConnections();
    NativeDatabaseWait wait(a.connection(db, true), true);
    if (!wait.ready() || !a.begin(db)) {
        // End the session all the same, so a resumed reader starts a new one
        // and the time until now is not counted twice or with the sleep. The
        // next progress write or endNow() saves the seconds.
        endSession();
        return false;
    }
    auto volume = a.get(book, db);
    const auto before = a.values(&volume);
    if (before.value("ContentID").toString() != book) {
        a.commit(db, false);
        endSession();
        return false;
    }
    endIn(&volume);
    const auto after = a.values(&volume);
    QVariantMap changed;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (it.value() != before.value(it.key()))
            changed.insert(it.key(), it.value());
    // A fresh Volume and a field-only update preserve incoming bookmarks.
    const bool updated =
            changed.isEmpty() || a.update(book, changed, { { "ContentID", book } }, db);
    // endTransaction(false) is the rollback when the update failed. When
    // COMMIT failed, roll back explicitly so later native saves can begin.
    const bool ended = a.commit(db, updated);
    const bool committed = updated && ended;
    if (!committed) {
        if (updated)
            a.commit(db, false);
        a.refresh(db);
    }
    const int seconds = pendingSeconds;
    saved(committed);
    diagnostic("reading-session-end",
               kobalt::qt::object({ { "persisted", committed }, { "seconds", seconds } }),
               committed ? DiagnosticSeverity::Activity : DiagnosticSeverity::Error);
    return committed;
}

#ifndef KOBALT_READER_HARDWARE
void NativeReadingSession::testCommand()
{
    QFile request("/instance/reading-session-state");
    if (!request.exists())
        return;
    // Read the native active counter only for the generated fixture. It is
    // cached while reading and removed by Nickel when the session ends.
    if (testActive && testActive->book == "file:///mnt/onboard/reader-fixture.kepub.epub"
        && testActive->running && workerReady()) {
        const auto book = testActive->book;
        const auto activity = testActive->activity_.current();
        kobalt::qt::invoke(
                api().engine(),
                [book, activity] {
                    struct Event
                    {
                        void *words[2];
                        Event(const Event &) = delete;
                        ~Event() { symbol<void (*)(void *)>("_ZN5EventD1Ev")(this); }
                    };
                    const auto get = symbol<Event (*)(const int &, const QString &,
                                                      const QString &)>(
                            "_ZN12EventManager13eventWithTypeERK15SocialEventTypeRK7QStringS5_");
                    const auto extra =
                            symbol<QVariantMap (*)(const Event *)>("_ZNK5Event9extraDataEv");
                    const int type = 46;
                    auto event = get(type, book, QString());
                    const auto data = extra(&event);
                    const auto volume = api().get(book, api().dbName(api().device()));
                    const double pace = api().pace ? api().pace(volume) : -1;
                    const auto estimate = nickel::readingEstimates(activity, pace);
                    diagnostic(
                            "reading-session-pages",
                            kobalt::qt::object(
                                    { { "pages", data.value("PagesTurnedThisSession").toInt() },
                                      { "word_samples", data.value("wordCounts").toList().size() },
                                      { "last_words",
                                        data.value("wordCounts").toList().isEmpty()
                                                ? 0
                                                : data.value("wordCounts")
                                                          .toList()
                                                          .last()
                                                          .toInt() },
                                      { "seconds_per_word", pace },
                                      { "chapter_ms", double(estimate.chapterMilliseconds) },
                                      { "book_ms", double(estimate.bookMilliseconds) } }),
                            false);
                },
                Qt::BlockingQueuedConnection);
    }
    request.remove();
}
#endif
} // namespace kobalt
