#include "support/qt_compat.h"
#include "platforms/kobo/common/native_lifecycle.h"
#include "platforms/kobo/common/native_database_wait.h"
#include "platforms/kobo/common/native_event_save.h"
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
#include <QSettings>
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
template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

struct Volume
{
    void *words[2];
    Volume(const Volume &) = delete;

    ~Volume() { symbol<void (*)(void *)>("_ZN6VolumeD1Ev")(this); }
};

// In 4.46.23836, the two session wrappers read only PluginState's double
// at offset 40. Nickel formats it for Kobo Plus; no native reader is created.
struct SessionPosition
{
    char unused[40]{};
    double percent = 0;
};

static_assert(sizeof(Volume) == 8 && offsetof(SessionPosition, percent) == 40);

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
    void (*saveEvents)(QObject *) =
            symbol<decltype(saveEvents)>("_ZN11EventEngine17savePendingEventsEv");
    bool (*flushEvents)(const QString &) =
            symbol<decltype(flushEvents)>("_ZN12EventManager5flushERK7QString");
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
                && engine && sessions.ready() && fire && saveEvents && flushEvents && time
                && setTime && begin && commit && update && refresh
                && symbol<void *>("_ZN6VolumeD1Ev");
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

// Run only on the event worker. The default database is also the one used
// by savePendingEvents() and EventManager's transient cache.
bool saveEvents(QObject *engine)
{
    mainthread::BlockingOperation operation("nickel-events-save");
    const QString db;
    NativeDatabaseWait wait(api().connection(db, true), 2000);
    return wait.ready()
            && nickel::saveNativeEvents([&] { return api().begin(db); },
                                        [&] {
                                            api().saveEvents(engine);
                                            return api().flushEvents(db);
                                        },
                                        [&](bool commit) { return api().commit(db, commit); });
}

// Saves the events of a session that just started, on the EventEngine
// thread. The GUI thread does not wait: the next page turn may come first.
// Queued calls run in order, so the start slots still precede this flush and
// a later stop's flushLater(). `done` runs on the GUI thread.
void flushStart(std::function<void()> done)
{
    auto *engine = api().engine();
    auto completion = std::make_shared<std::function<void()>>(std::move(done));
    if (!kobalt::qt::invoke(
                engine,
                [engine, completion] {
                    DiagnosticOperation operation("nickel-session-start-flush");
                    QElapsedTimer timer;
                    timer.start();
                    bool saved = false;
                    if (NativeLifecycle::nativeSessionSaveAllowed() && api().open()) {
                        saved = saveEvents(engine);
                    }
                    QSettings settings("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf",
                                       QSettings::IniFormat);
                    const bool sideload =
                            settings.value("ApplicationPreferences/SideloadedMode", false).toBool();
                    diagnostic("reading-session-start-flush",
                               kobalt::qt::object({ { "persisted", saved },
                                                    { "sideload_mode", sideload },
                                                    { "milliseconds", timer.elapsed() } }));
                    kobalt::qt::invoke(
                            qApp, [completion] { (*completion)(); }, Qt::QueuedConnection);
                },
                Qt::QueuedConnection))
        (*completion)();
}

// A successful commit consumes the delta even when independent readback is
// temporarily unavailable. Further attempts only verify that same write.
bool addTime(const QString &book, int seconds, nickel::NativeReadingTime &write)
{
    kobalt::mainthread::BlockingOperation blockingOperation("nickel-reading-time");
    if (seconds <= 0 || seconds >= 86400)
        return seconds == 0;
    const auto &a = api();
    if (!NativeLifecycle::nativeSessionSaveAllowed() || !a.open())
        return false;
    const auto db = a.dbName(a.device());
    NativeDatabaseWait wait(a.connection(db, true), true);
    if (!wait.ready())
        return false;
    return write.save(
            [&](QVariantMap &expected) {
                if (!a.begin(db))
                    return false;
                auto volume = a.get(book, db);
                const auto before = a.values(&volume);
                if (before.value("ContentID").toString() != book) {
                    write.uncertain = !a.commit(db, false);
                    return false;
                }
                const auto total = qint64(a.time(&volume)) + seconds;
                if (total > std::numeric_limits<int>::max()) {
                    write.uncertain = !a.commit(db, false);
                    return false;
                }
                a.setTime(&volume, QVariant(int(total)));
                const auto after = a.values(&volume);
                QVariantMap changed;
                for (auto it = after.cbegin(); it != after.cend(); ++it)
                    if (it.value() != before.value(it.key()))
                        changed.insert(it.key(), it.value());
                // A fresh Volume and a field-only update preserve incoming bookmarks.
                const bool updated = a.update(book, changed, { { "ContentID", book } }, db);
                const bool committed = a.commit(db, updated);
                if (!updated || !committed) {
                    // endTransaction(false) is the rollback when update failed. When
                    // COMMIT failed, explicitly roll back before retrying the delta.
                    const bool rolledBack = updated ? a.commit(db, false) : committed;
                    write.uncertain = !rolledBack;
                    a.refresh(db);
                    return false;
                }
                expected = std::move(changed);
                return true;
            },
            [&](const QVariantMap &expected) {
                bool verified = false;
                {
                    auto check = QSqlDatabase::addDatabase("QSQLITE", "kobalt-session-check");
                    check.setDatabaseName(a.connection(db, false).databaseName());
                    check.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=0");
                    verified = check.open();
                    if (verified) {
                        QSqlQuery query(check);
                        query.prepare(
                                "SELECT * FROM content WHERE ContentID = ? AND ContentType = 6");
                        query.addBindValue(book);
                        verified = query.exec() && query.next();
                        if (verified)
                            for (auto it = expected.cbegin(); it != expected.cend(); ++it)
                                if (query.value(it.key()) != it.value())
                                    verified = false;
                    }
                    check.close();
                }
                QSqlDatabase::removeDatabase("kobalt-session-check");
                if (!verified)
                    a.refresh(db);
                return verified;
            });
}

void flushLater(const std::weak_ptr<NativeReadingSession *> &guard, std::function<void(bool)> done)
{
    auto *engine = api().engine();
    if (!workerReady() || !NativeLifecycle::nativeSessionSaveAllowed()) {
        done(false);
        return;
    }
    // This is the same EventEngine thread used by flush(). No firmware
    // wrappers or GUI-owned Volume instances move to a different thread.
    auto completion = std::make_shared<std::function<void(bool)>>(std::move(done));
    if (!kobalt::qt::invoke(
                engine,
                [engine, guard, completion] {
                    DiagnosticOperation operation("nickel-session-stop-flush");
                    bool saved = false;
                    if (!guard.expired() && NativeLifecycle::nativeSessionSaveAllowed()
                        && api().open()) {
                        QElapsedTimer timer;
                        timer.start();
                        saved = saveEvents(engine);
                        diagnostic("reading-session-flush",
                                   kobalt::qt::object({ { "saved", saved },
                                                        { "milliseconds", timer.elapsed() } }),
                                   saved ? DiagnosticSeverity::Activity
                                         : DiagnosticSeverity::Error);
                    }
                    kobalt::qt::invoke(
                            qApp, [completion, saved] { (*completion)(saved); },
                            Qt::QueuedConnection);
                },
                Qt::QueuedConnection))
        (*completion)(false);
}

// Adds a stopped session's time on the same EventEngine thread, after its
// events are flushed. addTime() builds its own Volume there, as the pace query
// does, so no GUI-owned firmware object changes thread.
void writeLater(const std::weak_ptr<NativeReadingSession *> &guard, const QString &book,
                int seconds, const std::shared_ptr<nickel::NativeReadingTime> &write,
                std::function<void(bool)> done)
{
    auto *engine = api().engine();
    if (!workerReady()) {
        done(false);
        return;
    }
    auto completion = std::make_shared<std::function<void(bool)>>(std::move(done));
    if (!kobalt::qt::invoke(
                engine,
                [guard, book, seconds, write, completion] {
                    bool saved = false;
                    if (!guard.expired()) {
                        QElapsedTimer timer;
                        timer.start();
                        saved = addTime(book, seconds, *write);
                        diagnostic("reading-session-end",
                                   kobalt::qt::object({ { "seconds", seconds },
                                                        { "persisted", saved },
                                                        { "events_saved", true },
                                                        { "time_saved", saved },
                                                        { "time_committed", write->committed },
                                                        { "milliseconds", timer.elapsed() } }),
                                   saved ? DiagnosticSeverity::Activity
                                         : DiagnosticSeverity::Error);
                    }
                    kobalt::qt::invoke(
                            qApp, [completion, saved] { (*completion)(saved); },
                            Qt::QueuedConnection);
                },
                Qt::QueuedConnection))
        (*completion)(false);
}
} // namespace

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
    // Owners drain finish() before disposal. Invalidate queued work before
    // member destruction, including paths that are forced to abandon a save.
    alive.reset();
    if (running || ending || saves.pending())
        diagnostic("reading-session-disposed-unsaved", {}, DiagnosticSeverity::Error);
}

bool NativeReadingSession::available()
{
    return api().ready();
}

void NativeReadingSession::start(const ReadingActivity &activity)
{
    saves.retryIfDue();
    if (running || ending || !available() || !api().open() || !workerReady()
        || api().tolino(api().device()))
        return;
    auto *engine = api().engine();
    auto volume = api().get(book, api().dbName(api().device()));
    if (api().values(&volume).value("ContentID").toString() != book)
        return;
    percent = qBound(0.0, activity.percent, 100.0);
    activity_.start(activity);
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
    ++startFlushes;
    const std::weak_ptr<NativeReadingSession *> started = alive;
    flushStart([started] {
        if (const auto self = started.lock())
            --(*self)->startFlushes;
    });
    if (api().pace) {
        const auto identity = book;
        pace_.start(engine, [identity] {
            if (!api().open()) {
                return -1.0;
            }
            const auto volume = api().get(identity, api().dbName(api().device()));
            return api().values(&volume).value("ContentID").toString() == identity
                    ? api().pace(volume)
                    : -1.0;
        });
    }
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

void NativeReadingSession::report(int words)
{
    auto volume = api().get(book, api().dbName(api().device()));
    if (api().values(&volume).value("ContentID").toString() != book)
        return;
    // Nickel handles page activity and subscription accounting. Its event
    // engine intentionally ignores page events in sideload mode.
    api().fire(api().engine(), "pageTurned", volume, { { "wordsRead", words } });
}

bool NativeReadingSession::estimates(const ReadingActivity &activity, ReadingEstimates *result)
{
    if (!api().pace) {
        return false;
    }
    *result = nickel::readingEstimates(activity, pace_.value());
    return true;
}

void NativeReadingSession::stop()
{
    pace_.stop();
#ifndef KOBALT_READER_HARDWARE
    if (testActive == this)
        testActive = nullptr;
#endif
    if (!running && !ending)
        return;
    if (running) {
        saves.retryFailed();
        running = false;
        ending = true;
    }
    if (!NativeLifecycle::nativeSessionSaveAllowed() || !api().open() || !workerReady()) {
        pace_.clear();
        diagnostic("reading-session-stop-deferred");
        return;
    }
    auto volume = api().get(book, api().dbName(api().device()));
    if (api().values(&volume).value("ContentID").toString() != book) {
        diagnostic("reading-session-stop-deferred");
        return;
    }
    QElapsedTimer timer;
    timer.start();
    prepareVolume(volume, activity_.current());
    SessionPosition state;
    state.percent = percent;
    const int seconds = api().sessions.end(api().engine(), volume, state, {});
    api().fire(api().engine(), "readerHidden", volume, {});
    ending = false;
    diagnostic("reading-session-stop",
               kobalt::qt::object({ { "seconds", seconds }, { "milliseconds", timer.elapsed() } }));
    const auto identity = book;
    const auto write = std::make_shared<nickel::NativeReadingTime>();
    const std::weak_ptr<NativeReadingSession *> guard = alive;
    saves.append([guard](std::function<void(bool)> done) { flushLater(guard, std::move(done)); },
                 [guard, identity, seconds, write](std::function<void(bool)> done) {
                     writeLater(guard, identity, seconds, write, std::move(done));
                 });
}

void NativeReadingSession::finish(std::function<void(bool)> done)
{
    stop();
    if (ending) {
        done(false);
        return;
    }
    saves.finish(std::move(done));
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
