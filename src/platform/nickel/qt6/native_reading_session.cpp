#include "support/qt_compat.h"
#include "platform/nickel/common/native_lifecycle.h"
#include "platform/nickel/common/native_database_wait.h"
#include "platform/nickel/common/native_reading_session.h"
#include "support/diagnostics.h"
#include <QElapsedTimer>
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
#include <QFile>
#endif

namespace kobalt {
namespace {
#ifndef KOBALT_READER_HARDWARE
NativeReadingSession *testActive = nullptr;
#endif
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
struct Volume {
    void *words[3];
    Volume(const Volume &) = delete;
    ~Volume() {
        symbol<void (*)(void *)>("_ZN6VolumeD1Ev")(this);
    }
};
struct User {
    void *words[2];
    User(const User &) = delete;
    ~User() {
        symbol<void (*)(void *)>("_ZN4UserD1Ev")(this);
    }
};
// In 5.18.270971, the two session wrappers read only PluginState's double
// at offset 80. Nickel formats it for Kobo Plus; no native reader is created.
struct SessionPosition {
    char unused[80]{};
    double percent = 0;
};
static_assert(sizeof(Volume) == 12 && sizeof(User) == 8 && offsetof(SessionPosition, percent) == 80);
struct Api {
    Volume (*get)(const QString &, const QString &) = symbol<decltype(get)>("_ZN13VolumeManager7getByIdERK7QStringS2_");
    QVariantMap (*values)(const void *) = symbol<decltype(values)>("_ZNK6Volume11getDbValuesEv");
    void *(*device)() = symbol<decltype(device)>("_ZN6Device16getCurrentDeviceEv");
    const QString &(*dbName)(const void *) = symbol<decltype(dbName)>("_ZNK6Device9getDbNameEv");
    bool (*tolino)(const void *) = symbol<decltype(tolino)>("_ZNK6Device8isTolinoEv");
    QSqlDatabase (*connection)(const QString &,
                               bool) = symbol<decltype(connection)>("_ZN15DatabaseAdapter12dbConnectionERK7QStringb");
    User (*user)(const QString &) = symbol<decltype(user)>("_ZN11UserManager15getUserOnDeviceERK7QString");
    QString (*userId)(const User *) = symbol<decltype(userId)>("_ZNK4User2idEv");
    QObject *(*engine)() = symbol<decltype(engine)>("_ZN11EventEngine14sharedInstanceEv");
    void (*start)(QObject *, Volume &, const SessionPosition &, QVariantMap) = symbol<decltype(start)>(
        "_ZN11EventEngine19startReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE");
    int (*end)(QObject *, Volume &, const SessionPosition &, QVariantMap) =
        symbol<decltype(end)>("_ZN11EventEngine17endReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE");
    void (*fire)(QObject *, const char *, const Volume &, const QVariantMap &) =
        symbol<decltype(fire)>("_ZN11EventEngine9fireEventEPKcRK6VolumeRK4QMapI7QString8QVariantE");
    void (*saveEvents)(QObject *) = symbol<decltype(saveEvents)>("_ZN11EventEngine17savePendingEventsEv");
    bool (*flushEvents)(const QString &) = symbol<decltype(flushEvents)>("_ZN12EventManager5flushERK7QString");
    int (*time)(const Volume *) = symbol<decltype(time)>("_ZNK6Volume16timeSpentReadingEv");
    double (*pace)(const Volume &) = symbol<decltype(pace)>("_ZN22ReadingVelocityTracker14secondsPerWordERK6Volume");
    void (*setTime)(Volume *,
                    const QVariant &) = symbol<decltype(setTime)>("_ZN6Volume19setTimeSpentReadingERK8QVariant");
    bool (*begin)(QString) = symbol<decltype(begin)>("_ZN15DatabaseAdapter16beginTransactionE7QString");
    bool (*commit)(QString, bool) = symbol<decltype(commit)>("_ZN15DatabaseAdapter14endTransactionE7QStringb");
    bool (*update)(const QString &, const QVariantMap &, const QVariantMap &, const QString &) =
        symbol<decltype(update)>("_ZN13VolumeManager12updateFieldsERK7QStringRK4QMapIS0_8QVariantES7_S2_");
    void *(*refresh)(const QString &) =
        symbol<decltype(refresh)>("_ZN10Repository12refreshCacheI6VolumeEEP5CacheIT_ERK7QString");
    bool ready() const {
        const auto type = QMetaType(symbol<const QtPrivate::QMetaTypeInterface *>(
            "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI6VolumeE8metaTypeE"));
        return type.sizeOf() == sizeof(Volume) && get && values && device && dbName && tolino && connection && user &&
               userId && engine && start && end && fire && saveEvents && flushEvents && time && setTime && begin &&
               commit && update && refresh && symbol<void *>("_ZN6VolumeD1Ev") && symbol<void *>("_ZN4UserD1Ev");
    }
    QString account() const {
        const auto value = user(dbName(device()));
        return userId(&value);
    }
    bool open() const {
        const auto db = connection(dbName(device()), false);
        return db.isValid() && db.isOpen();
    }
};
const Api &api() {
    static const Api value;
    return value;
}
bool workerReady() {
    auto *engine = api().engine();
    auto *thread = engine ? engine->thread() : nullptr;
    return thread && thread != QThread::currentThread() && thread->isRunning();
}
bool flush() {
    auto *engine = api().engine();
    bool saved = false;
    const bool nonblocking = NativeLifecycle::usbSaveAllowed();
    const auto save = [engine, &saved, nonblocking] {
        NativeDatabaseWait wait(api().connection(api().dbName(api().device()), true), nonblocking);
        if (!wait.ready()) {
            return;
        }
        api().saveEvents(engine);
        // EventManager's transient cache accepts only the default database.
        saved = api().flushEvents(QString());
    };
    // Nickel's own flush uses BlockingQueuedConnection. This also orders the
    // start/end slots before sleep, USB release and the next session start.
    if (!kobalt::qt::invoke(engine, save, Qt::BlockingQueuedConnection))
        return false;
    return saved;
}
bool addTime(const QString &book, int seconds) {
    if (seconds <= 0 || seconds >= 86400)
        return seconds == 0;
    const auto &a = api();
    const auto db = a.dbName(a.device());
    NativeDatabaseWait wait(a.connection(db, true), NativeLifecycle::usbSaveAllowed());
    if (!wait.ready() || !a.begin(db)) {
        return false;
    }
    auto volume = a.get(book, db);
    const auto before = a.values(&volume);
    if (before.value("ContentID").toString() != book) {
        a.commit(db, false);
        return false;
    }
    const auto total = qint64(a.time(&volume)) + seconds;
    if (total > std::numeric_limits<int>::max()) {
        a.commit(db, false);
        return false;
    }
    a.setTime(&volume, QVariant(int(total)));
    const auto after = a.values(&volume);
    QVariantMap changed;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (it.value() != before.value(it.key()))
            changed.insert(it.key(), it.value());
    // A fresh Volume and a field-only update preserve incoming bookmarks.
    const bool updated = a.update(book, changed, {{"ContentID", book}}, db);
    const bool committed = a.commit(db, updated);
    if (!updated || !committed) {
        if (updated && !committed)
            a.commit(db, false);
        a.refresh(db);
        return false;
    }
    bool verified = true;
    {
        auto check = QSqlDatabase::addDatabase("QSQLITE", "kobalt-session-check");
        check.setDatabaseName(a.connection(db, false).databaseName());
        check.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        verified = check.open();
        if (verified) {
            QSqlQuery query(check);
            query.prepare("SELECT * FROM content WHERE ContentID = ? AND ContentType = 6");
            query.addBindValue(book);
            verified = query.exec() && query.next();
            if (verified)
                for (auto it = changed.cbegin(); it != changed.cend(); ++it)
                    if (query.value(it.key()) != it.value())
                        verified = false;
        }
        check.close();
    }
    QSqlDatabase::removeDatabase("kobalt-session-check");
    if (!verified)
        a.refresh(db);
    return verified;
}
}
NativeReadingSession::NativeReadingSession(QString identity) : book(std::move(identity)) {}
NativeReadingSession::~NativeReadingSession() {
    stop();
}
bool NativeReadingSession::available() {
    return api().ready();
}
void NativeReadingSession::start(const ReadingActivity &activity) {
    if (running || !available() || !api().open() || !workerReady() || api().tolino(api().device()))
        return;
    auto *engine = api().engine();
    auto volume = api().get(book, api().dbName(api().device()));
    if (api().values(&volume).value("ContentID").toString() != book)
        return;
    account = api().account();
    percent = qBound(0.0, activity.percent, 100.0);
    activity_.start(activity);
    paceDirty_ = true;
    SessionPosition state;
    state.percent = percent;
    QElapsedTimer timer;
    timer.start();
    api().start(engine, volume, state, {{"ViewType", "ReadingView"}});
    running = true;
#ifndef KOBALT_READER_HARDWARE
    testActive = this;
#endif
    const bool persisted = flush();
    QSettings settings("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf", QSettings::IniFormat);
    const bool sideload = settings.value("ApplicationPreferences/SideloadedMode", false).toBool();
    diagnostic("reading-session-start",
               {{"persisted", persisted}, {"sideload_mode", sideload}, {"milliseconds", timer.elapsed()}});
}
void NativeReadingSession::page(const ReadingActivity &activity) {
    percent = qBound(0.0, activity.percent, 100.0);
    if (!running)
        return;
    int words = 0;
    if (!activity_.observe(activity, &words))
        return;
    if (!api().open() || api().account() != account) {
        running = false;
        secondsPerWord_ = -1;
        return;
    }
    auto volume = api().get(book, api().dbName(api().device()));
    if (api().values(&volume).value("ContentID").toString() != book)
        return;
    // Nickel handles page activity and subscription accounting. Its event
    // engine intentionally ignores page events in sideload mode.
    api().fire(api().engine(), "pageTurned", volume, {{"wordsRead", words}});
    paceDirty_ = true;
}
bool NativeReadingSession::estimates(const ReadingActivity &activity, ReadingEstimates *result) {
    if (!api().pace || !api().open() || !workerReady() || (!running && secondsPerWord_ <= 0))
        return false;
    if (running && paceDirty_) {
        double pace = -1;
        const bool queried = kobalt::qt::invoke(
            api().engine(),
            [&] {
                const auto volume = api().get(book, api().dbName(api().device()));
                if (api().values(&volume).value("ContentID").toString() == book)
                    pace = api().pace(volume);
            },
            Qt::BlockingQueuedConnection);
        if (!queried || !std::isfinite(pace) || pace <= 0) {
            *result = ReadingEstimates{};
            return true;
        }
        secondsPerWord_ = pace;
        paceDirty_ = false;
    }
    *result = nickel::readingEstimates(activity, secondsPerWord_);
    return true;
}
void NativeReadingSession::stop() {
#ifndef KOBALT_READER_HARDWARE
    if (testActive == this)
        testActive = nullptr;
#endif
    if (!running)
        return;
    ReadingEstimates remaining;
    estimates(activity_.current(), &remaining);
    running = false;
    if (!api().open() || !workerReady() || api().account() != account) {
        secondsPerWord_ = -1;
        diagnostic("reading-session-discarded");
        return;
    }
    auto volume = api().get(book, api().dbName(api().device()));
    if (api().values(&volume).value("ContentID").toString() != book) {
        diagnostic("reading-session-discarded");
        return;
    }
    QElapsedTimer timer;
    timer.start();
    SessionPosition state;
    state.percent = percent;
    const int seconds = api().end(api().engine(), volume, state, {});
    const bool events = flush();
    const bool time = addTime(book, seconds);
    diagnostic("reading-session-end", {{"seconds", seconds},
                                       {"persisted", events && time},
                                       {"events_saved", events},
                                       {"time_saved", time},
                                       {"milliseconds", timer.elapsed()}});
}
#ifndef KOBALT_READER_HARDWARE
void NativeReadingSession::testCommand() {
    QFile request("/instance/reading-session-state");
    if (!request.exists())
        return;
    // Read the native active counter only for the generated fixture. It is
    // cached while reading and removed by Nickel when the session ends.
    if (testActive && testActive->book == "file:///mnt/onboard/reader-fixture.kepub.epub" && testActive->running &&
        workerReady()) {
        const auto book = testActive->book;
        const auto activity = testActive->activity_.current();
        kobalt::qt::invoke(
            api().engine(),
            [book, activity] {
                struct Event {
                    void *words[2];
                    Event(const Event &) = delete;
                    ~Event() {
                        symbol<void (*)(void *)>("_ZN5EventD1Ev")(this);
                    }
                };
                const auto get = symbol<Event (*)(const int &, const QString &, const QString &)>(
                    "_ZN12EventManager13eventWithTypeERK15SocialEventTypeRK7QStringS5_");
                const auto extra = symbol<QVariantMap (*)(const Event *)>("_ZNK5Event9extraDataEv");
                const int type = 46;
                auto event = get(type, book, QString());
                const auto data = extra(&event);
                const auto volume = api().get(book, api().dbName(api().device()));
                const double pace = api().pace ? api().pace(volume) : -1;
                const auto estimate = nickel::readingEstimates(activity, pace);
                diagnostic("reading-session-pages",
                           {{"pages", data.value("PagesTurnedThisSession").toInt()},
                            {"word_samples", data.value("wordCounts").toList().size()},
                            {"last_words", data.value("wordCounts").toList().isEmpty()
                                               ? 0
                                               : data.value("wordCounts").toList().last().toInt()},
                            {"seconds_per_word", pace},
                            {"chapter_ms", double(estimate.chapterMilliseconds)},
                            {"book_ms", double(estimate.bookMilliseconds)}},
                           false);
            },
            Qt::BlockingQueuedConnection);
    }
    request.remove();
}
#endif
}
