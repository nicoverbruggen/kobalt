#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include "reading/reading_bridge.h"
#include "reading/word_estimates.h"
#include "support/strings.h"
#include "support/main_thread.h"
#include "platforms/kobo/common/native_database_wait.h"
#include "platforms/kobo/common/native_lifecycle.h"
#include "platforms/kobo/common/native_progress.h"
#include "platforms/kobo/common/native_location.h"
#include "platforms/kobo/common/tolino_progress.h"
#include <QDateTime>
#include <QMetaType>
#include <QPointer>
#ifndef KOBALT_READER_HARDWARE
#  include <QFile>
#  include <QJsonDocument>
#  include <QSaveFile>
#endif
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

struct Volume
{
    void *words[3];
    Volume(const Volume &) = delete;

    ~Volume() { symbol<void (*)(void *)>("_ZN6VolumeD1Ev")(this); }
};

struct Location
{
    void *words[2];
    Location(const Location &) = delete;

    ~Location() { symbol<void (*)(void *)>("_ZN15ContentLocationD1Ev")(this); }
};

static_assert(sizeof(Volume) == 12 && sizeof(Location) == 8);

struct Api
{
    Volume (*get)(const QString &, const QString &) =
            symbol<decltype(get)>("_ZN13VolumeManager7getByIdERK7QStringS2_");
    QVariantMap (*values)(const void *) = symbol<decltype(values)>("_ZNK6Volume11getDbValuesEv");
    void *(*device)() = symbol<decltype(device)>("_ZN6Device16getCurrentDeviceEv");
    const QString &(*dbName)(const void *) = symbol<decltype(dbName)>("_ZNK6Device9getDbNameEv");
    bool (*tolino)(const void *) = symbol<decltype(tolino)>("_ZNK6Device8isTolinoEv");
    QSqlDatabase (*connection)(const QString &, bool) =
            symbol<decltype(connection)>("_ZN15DatabaseAdapter12dbConnectionERK7QStringb");
    Location (*location)(QString, QString) =
            symbol<decltype(location)>("_ZN15ContentLocation6createE7QStringS0_");
    bool (*validLocation)(const Location *) =
            symbol<decltype(validLocation)>("_ZNK15ContentLocation7isValidEv");
    QString (*teng)(const Location *) =
            symbol<decltype(teng)>("_ZNK15ContentLocation12toTENGStringEv");
    QString (*serialized)(const Location *) =
            symbol<decltype(serialized)>("_ZNK15ContentLocation8toStringEv");
    void (*bookmark)(void *, const QString &, int, const QDateTime &, int) =
            symbol<decltype(bookmark)>(
                    "_ZN13VolumeManager14updateBookmarkER6VolumeRK7QStringiRK9QDateTimei");
    void (*status)(void *,
                   int) = symbol<decltype(status)>("_ZN7Content13setReadStatusE13ReadingStatus");
    void (*firstTime)(void *, const QVariant &) =
            symbol<decltype(firstTime)>("_ZN7Content19setFirstTimeReadingERK8QVariant");
    void (*percent)(void *, const QVariant &) =
            symbol<decltype(percent)>("_ZN7Content14setPercentReadERK8QVariant");
    void (*setBookEstimate)(Volume *, const QVariant &) =
            symbol<decltype(setBookEstimate)>("_ZN6Volume21setRestOfBookEstimateERK8QVariant");
    void (*setChapterEstimate)(Volume *, const QVariant &) = symbol<decltype(setChapterEstimate)>(
            "_ZN6Volume25setCurrentChapterEstimateERK8QVariant");
    // Present in firmware 4.46 and 6.0.279225. The sync upload sends it as
    // ContentSourceProgressPercent and the stats screen shows it.
    void (*setChapterProgress)(Volume *, const QVariant &) = symbol<decltype(setChapterProgress)>(
            "_ZN6Volume25setCurrentChapterProgressERK8QVariant");
    // Optional. Firmware 4.46 and 6.0: Nickel's reader calls it when a book
    // closes, with enabled set because the book's storage is connected.
    bool (*recentBook)(const QByteArray &, const QString &, bool, bool) =
            symbol<decltype(recentBook)>(
                    "_ZN15ActivityManager22makeRecentBookActivityERK10QByteArrayRK7QStringbb");
    void (*setPages)(void *, const QVariant &) =
            symbol<decltype(setPages)>("_ZN7Content11setNumPagesERK8QVariant");
    bool (*update)(const QString &, const QVariantMap &, const QVariantMap &, const QString &) =
            symbol<decltype(update)>(
                    "_ZN13VolumeManager12updateFieldsERK7QStringRK4QMapIS0_8QVariantES7_S2_");
    void *(*refresh)(const QString &) = symbol<decltype(refresh)>(
            "_ZN10Repository12refreshCacheI6VolumeEEP5CacheIT_ERK7QString");
    bool (*begin)(QString) =
            symbol<decltype(begin)>("_ZN15DatabaseAdapter16beginTransactionE7QString");
    bool (*end)(QString,
                bool) = symbol<decltype(end)>("_ZN15DatabaseAdapter14endTransactionE7QStringb");
    QObject *(*manager)() = symbol<decltype(manager)>("_ZN19BookmarkSyncManager14sharedInstanceEv");
    bool (*syncEligible)(const QObject *, const void *) = symbol<decltype(syncEligible)>(
            "_ZNK23BookmarkSyncManagerBase16syncReadingStateERK6Volume");
    void (*sendBookmark)(QObject *, const void *) =
            symbol<decltype(sendBookmark)>("_ZN19BookmarkSyncManager12sendBookmarkERK6Volume");

    bool ready() const
    {
        auto type = QMetaType(symbol<const QtPrivate::QMetaTypeInterface *>(
                "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI6VolumeE8metaTypeE"));
        return type.sizeOf() == sizeof(Volume) && get && values && device && dbName && tolino
                && connection && location && validLocation && teng && serialized && bookmark
                && status && firstTime && percent && update && refresh && begin && end && manager
                && syncEligible && sendBookmark && symbol<void *>("_ZN6VolumeD1Ev")
                && symbol<void *>("_ZN15ContentLocationD1Ev");
    }
};

const Api &api()
{
    static const Api value;
    return value;
}

bool tolinoBookmark(const QSqlDatabase &database, const QString &identity, QVariantMap *fields)
{
    fields->clear();
    QSqlQuery query(database);
    if (!query.prepare("SELECT position, currentPosition, lastPosition, progress, modified, "
                       "modifiedLocally, deletedLocally FROM tolino_bookmark WHERE uuid = ?"))
        return false;
    query.addBindValue(identity);
    if (!query.exec())
        return false;
    if (query.next()) {
        const auto record = query.record();
        for (int i = 0; i < record.count(); ++i)
            fields->insert(record.fieldName(i), query.value(i));
    }
    return !query.lastError().isValid();
}

ReadingProgress snapshot(const QVariantMap &values)
{
    ReadingProgress result;
    result.book = values.value("ContentID").toString();
    result.status = values.value("ReadStatus").toInt();
    auto date = values.value("DateLastRead").toDateTime();
    if (!date.isValid())
        date = QDateTime::fromString(values.value("DateLastRead").toString(), Qt::ISODate);
    // Nickel stores some library actions without a zone and reads them as
    // local time. Preserve that interpretation when comparing its cache.
    result.modified = date.isValid() ? date.toMSecsSinceEpoch() : 0;
    const auto raw =
            publicationLocation(result.book, values.value("ChapterIDBookmarked").toString());
    if (!unsetNativeLocation(raw)) {
        const auto location = api().location(raw, QString());
        if (!api().validLocation(&location))
            return result;
        result.location = api().teng(&location);
        if (result.location.isEmpty())
            return result;
    }
    result.valid = !result.book.isEmpty();
    return result;
}
} // namespace

bool NativeProgress::available()
{
    return api().ready();
}

QVariantMap NativeProgress::persisted(QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QVariantMap();
    };
    if (!available())
        return fail(strings::get("error.native_progress_api_is_unavailable"));
    const auto &native = api();
    const auto existing = native.connection(native.dbName(native.device()), false);
    if (!existing.isValid() || !existing.isOpen())
        return fail(strings::get("error.nickel_database_is_unavailable"));
    // A separate read-only connection must see committed data, not Nickel's
    // cache or an uncommitted transaction on its own connection.
    const auto name = QString("kobalt-progress-%1").arg(connectionId);
    QVariantMap result;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(existing.databaseName());
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare(
                    "SELECT ContentID, ChapterIDBookmarked, DateLastRead, ReadStatus, ___PercentRead, ReadStateSynced, FirstTimeReading, RestOfBookEstimate, CurrentChapterEstimate, CurrentChapterProgress, ___NumPages FROM content WHERE ContentID = ? AND ContentType = 6");
            query.addBindValue(book);
            if (query.exec() && query.next()) {
                const auto record = query.record();
                for (int i = 0; i < record.count(); ++i)
                    result.insert(record.fieldName(i), query.value(i));
            }
        } else {
            reportDescriptorExhaustion("nickel-database-open");
        }
        if (!result.isEmpty() && native.tolino(native.device())) {
            QVariantMap bookmark;
            if (!tolinoBookmark(db, book, &bookmark))
                result.clear();
            else if (!bookmark.isEmpty())
                result.insert("tolino_bookmark", bookmark);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    if (result.isEmpty())
        return fail(strings::get("error.cannot_verify_persisted_nickel_progress"));
    return result;
}

ReadingProgress NativeProgress::read(QString *error) const
{
    auto value = snapshot(persisted(error));
    if (value.book.isEmpty())
        value.book = book;
    if (!value.valid && error && error->isEmpty())
        *error = strings::get("error.nickel_reading_position_could_not_be_decoded");
    return value;
}

bool NativeProgress::markReading()
{
    mainthread::BlockingOperation operation("nickel-opening-status");
    if (!available() || !NativeLifecycle::nativeProgressSaveAllowed() || deviceTransition())
        return false;
    const auto &native = api();
    const auto db = native.dbName(native.device());
    NativeDatabaseWait wait(native.connection(db, true), true);
    if (!wait.ready() || !native.begin(db))
        return false;
    // Firmware 4.46 and 6.0 updateReadingStatus sets these fields, without DateLastRead.
    // Keep positions, percentages and estimates untouched. No Volume crosses threads.
    const QVariantMap fields{ { "ReadStatus", 1 },
                              { "FirstTimeReading", false },
                              { "ReadStateSynced", false },
                              { "HideFromHomePage", false } };
    const bool saved = native.update(book, fields, { { "ContentID", book } }, db);
    if (!saved || !native.end(db, true)) {
        native.end(db, false);
        return false;
    }
    native.refresh(db);
    return true;
}

void NativeProgress::markRecent()
{
    kobalt::mainthread::BlockingOperation operation("nickel-recent-book");
    const auto &native = api();
    if (!native.recentBook || !available() || !NativeLifecycle::nativeProgressSaveAllowed()
        || deviceTransition())
        return;
    const auto db = native.dbName(native.device());
    NativeDatabaseWait wait(native.connection(db, true), true);
    // A busy database skips this; it only orders Nickel's recent books.
    const bool saved = wait.ready() && native.recentBook(book.toUtf8(), db, true, true);
    if (!saved)
        diagnostic("nickel-recent-book", kobalt::qt::object({ { "saved", false } }),
                   DiagnosticSeverity::Warning);
}

bool NativeProgress::write(const ReadingProgress &expected, const QString &position, double percent,
                           bool finished, ReadingProgress *verified, QString *error,
                           const ReadingEstimates *estimates, const ReadingActivity *activity)
{
    // Nickel's Volume API runs on the GUI thread at lifecycle boundaries.
    kobalt::mainthread::BlockingOperation blockingOperation("nickel-progress-write");
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!NativeLifecycle::nativeProgressSaveAllowed() || deviceTransition())
        return fail(strings::get(
                "error.nickel_save_deferred_while_progress_or_device_state_is_unresolved"));
    const auto &native = api();
    if (!available())
        return fail(strings::get("error.native_progress_api_is_unavailable"));
    const bool tolino = native.tolino(native.device());
    if (tolino
        && (!native.setPages || !activity || activity->pages <= 0 || activity->page < 0
            || activity->page >= activity->pages))
        return fail(strings::get("error.nickel_rejected_the_exact_reading_position"));
    // Without a position Nickel can store, as for some plain EPUB files, record only
    // the time, the reading status and the percentage. Nickel keeps its own
    // position, and the book still becomes the most recently read one.
    const bool timeOnly = position.isEmpty();
    const auto location = native.location(position, QString());
    if (!timeOnly && (!native.validLocation(&location) || native.teng(&location) != position))
        return fail(strings::get("error.nickel_rejected_the_exact_reading_position"));
    const auto db = native.dbName(native.device());
    NativeDatabaseWait wait(native.connection(db, true), true);
    if (!wait.ready()) {
        return fail(strings::get("error.nickel_database_is_busy_local_position_was_retained"));
    }
    // Nickel starts BEGIN IMMEDIATE and refuses a nested transaction. Never
    // commit someone else's pending work. The lock covers comparison and save.
    if (!native.begin(db))
        return fail(strings::get("error.nickel_database_is_busy_local_position_was_retained"));

    struct Transaction
    {
        QString db;
        bool owned = true;

        ~Transaction()
        {
            if (owned)
                api().end(db, false);
        }

        bool commit()
        {
            const bool ok = api().end(db, true);
            if (ok)
                owned = false;
            return ok;
        }
    } transaction{ db };

    const auto before = persisted(error);
    if (!expected.valid || !(snapshot(before) == expected))
        return fail(strings::get("error.nickel_progress_changed_before_saving"));
    // A round trip can leave Nickel's cache behind its committed row. Only
    // refresh after releasing our own transaction, then check the row again.
    const auto cacheMatches = [&](const QVariantMap &values) {
        const auto cached = snapshot(values);
        return cached.samePosition(expected) && cached.modified == expected.modified / 1000 * 1000;
    };
    bool currentCache = false;
    {
        const auto preflight = native.get(book, db);
        currentCache = cacheMatches(native.values(&preflight));
    }
    if (!currentCache) {
        if (!native.end(db, false))
            return fail(strings::get("error.nickel_s_cached_progress_is_not_current"));
        transaction.owned = false;
        native.refresh(db);
        if (!native.begin(db))
            return fail(strings::get("error.nickel_database_is_busy_local_position_was_retained"));
        transaction.owned = true;
        if (!(snapshot(persisted(error)) == expected))
            return fail(strings::get("error.nickel_progress_changed_before_saving"));
    }
    // Detach a fresh native value and let Nickel create its bookmark fields.
    // Persist only those changed fields, never an opening-time Volume snapshot.
    auto volume = native.get(book, db);
    const auto original = native.values(&volume);
    // getDbValues formats DateLastRead to whole seconds, even when SQLite
    // retains milliseconds. The committed-row comparison above stays exact.
    if (!cacheMatches(original))
        return fail(strings::get("error.nickel_s_cached_progress_is_not_current"));
    auto stamp = QDateTime::fromMSecsSinceEpoch(
            qMax(QDateTime::currentMSecsSinceEpoch(), expected.modified + 1000), Qt::UTC);
    if (tolino) {
        // Tolino derives its fraction and library percentage from this renderer's
        // current/total pages. The built-in reader repaginates when it opens.
        native.status(&volume, finished ? 2 : 1);
        native.setPages(&volume, QVariant(activity->pages));
        native.bookmark(&volume,
                        timeOnly ? original.value("ChapterIDBookmarked").toString()
                                 : native.serialized(&location),
                        original.value("adobe_location").toInt(), stamp,
                        finished ? activity->pages : activity->page);
    } else {
        if (!timeOnly)
            native.bookmark(&volume, native.serialized(&location),
                            original.value("adobe_location").toInt(), stamp, 0);
        native.status(&volume, finished ? 2 : 1);
        // Firmware 6's own reader stores 100 on the last page while reading.
        native.percent(&volume, QVariant(finished ? 100.0 : qBound(0.0, percent, 100.0)));
    }
    native.firstTime(&volume, QVariant(false));
    if (estimates && native.setBookEstimate && native.setChapterEstimate) {
        if (estimates->bookMilliseconds >= 0)
            native.setBookEstimate(
                    &volume, QVariant(finished ? 0 : int(estimates->bookMilliseconds / 1000)));
        if (estimates->chapterMilliseconds >= 0)
            native.setChapterEstimate(
                    &volume, QVariant(finished ? 0 : int(estimates->chapterMilliseconds / 1000)));
    }
    if (activity && native.setChapterProgress) {
        const double chapter = wordEstimates::chapterProgress(*activity);
        if (chapter >= 0)
            native.setChapterProgress(&volume, QVariant(chapter));
    }
    auto after = native.values(&volume);
    if (timeOnly && !tolino) {
        // Nickel stores the time in whole seconds, as updateBookmark does.
        stamp = QDateTime::fromMSecsSinceEpoch((stamp.toMSecsSinceEpoch() + 999) / 1000 * 1000,
                                               Qt::UTC);
        after.insert("DateLastRead", stamp.toString(Qt::ISODate));
    }
    QVariantMap changed;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (it.value() != original.value(it.key()))
            changed.insert(it.key(), it.value());
    // The native query builder shares parameter names between SET and WHERE.
    // Compare the bookmark under the transaction lock, then update by identity.
    bool saved = native.update(book, changed, { { "ContentID", book } }, db);
    const int page = tolino ? (finished ? activity->pages : activity->page) : 0;
    if (saved && tolino) {
        // Check the native helper's side effects before committing either row.
        QVariantMap bookmark;
        saved = tolinoBookmark(native.connection(db, false), book, &bookmark)
                && tolinoBookmarkMatches(bookmark, after.value("ChapterIDBookmarked").toString(),
                                         page, activity->pages);
    }
    bool commitAllowed = saved;
#ifndef KOBALT_READER_HARDWARE
    if (failNextWrite) {
        commitAllowed = false;
        failNextWrite = false;
    }
#endif
    const bool committed = commitAllowed && transaction.commit();
    if (!committed) {
        if (transaction.owned) {
            native.end(db, false);
            transaction.owned = false;
        }
        native.refresh(db);
        return fail(
                strings::get("error.nickel_could_not_commit_progress_local_position_was_retained"));
    }
    const auto stored = persisted(error);
    const auto actual = snapshot(stored);
    const auto wanted = snapshot(after);
    const auto bookmark = stored.value("tolino_bookmark").toMap();
    const bool tolinoMatches = !tolino
            || (tolinoBookmarkMatches(bookmark, after.value("ChapterIDBookmarked").toString(), page,
                                      activity->pages)
                && stored.value("___NumPages").toInt() == activity->pages);
    const bool matches = tolinoMatches && actual == wanted
            && stored.value("___PercentRead").toDouble() == after.value("___PercentRead").toDouble()
            // A time-only record leaves the sync flag alone: Kobo's cloud does not
            // sync sideloaded EPUB files, and only the bookmark call clears it.
            && (timeOnly || !stored.value("ReadStateSynced").toBool())
            && !stored.value("FirstTimeReading").toBool()
            && stored.value("RestOfBookEstimate").toInt()
                    == after.value("RestOfBookEstimate").toInt()
            && stored.value("CurrentChapterEstimate").toInt()
                    == after.value("CurrentChapterEstimate").toInt()
            && qAbs(stored.value("CurrentChapterProgress").toDouble()
                    - after.value("CurrentChapterProgress").toDouble())
                    < 1e-9;
    if (!matches) {
        // A native notification is not proof of persistence. Discard its cache
        // if the independently committed row does not match.
        native.refresh(db);
        return fail(strings::get(
                "error.nickel_persistence_could_not_be_verified_local_position_was_retain"));
    }
    *verified = actual;
    // Keep Nickel's eligibility, network and power policy. Its worker owns the
    // request after this object closes. This is not proof of server delivery.
    if (!tolino && !timeOnly) {
        auto *sync = native.manager();
        if (native.syncEligible(sync, &volume))
            native.sendBookmark(sync, &volume);
    }
    return true;
}

#ifndef KOBALT_READER_HARDWARE
// Simulator-only save-failure checks. It accepts one synthetic fixture identity
// and is absent from device packages. Updates still use Nickel's persistence.
void NativeProgress::testCommand()
{
    QFile input("/instance/progress-test.json");
    if (!input.open(QIODevice::ReadOnly))
        return;
    if (input.size() > 4096) {
        input.close();
        input.remove();
        return;
    }
    const auto command = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    input.remove();
    const auto result = testAction(command);
    QSaveFile output("/instance/artifacts/progress-test.json");
    if (output.open(QIODevice::WriteOnly)) {
        output.write(QJsonDocument(result).toJson());
        output.commit();
    }
}

QJsonObject NativeProgress::testAction(const QJsonObject &command)
{
    const QString fixture = "file:///mnt/onboard/progress-integration.kepub.epub";
    QJsonObject result = kobalt::qt::object({ { "id", command.value("id") }, { "ok", false } });
    const auto action = command.value("action").toString();
    if (available() && command.value("book").toString() == fixture) {
        const auto &native = api();
        const auto db = native.dbName(native.device());
        auto volume = native.get(fixture, db);
        if (action == "fail-next-save" && active && active->book == fixture) {
            failNextWrite = true;
            result["ok"] = true;
        } else if ((action == "suspend" || action == "resume") && active
                   && active->book == fixture) {
            if (action == "suspend")
                active->beginTransition();
            else
                active->endTransition();
            result["ok"] = true;
        } else if (action == "persist") {
            const auto location = native.location(command.value("location").toString(), QString());
            if (native.validLocation(&location) && native.begin(db)) {
                const auto before = native.values(&volume);
                native.bookmark(&volume, native.serialized(&location), 0,
                                QDateTime::fromMSecsSinceEpoch(
                                        command.value("modified").toString().toLongLong(), Qt::UTC),
                                0);
                native.status(&volume, command.value("status").toInt(1));
                QVariantMap delta;
                const auto after = native.values(&volume);
                for (auto it = after.cbegin(); it != after.cend(); ++it)
                    if (it.value() != before.value(it.key()))
                        delta.insert(it.key(), it.value());
                const bool saved = delta.isEmpty()
                        || native.update(fixture, delta, { { "ContentID", fixture } }, db);
                result["ok"] = native.end(db, saved) && saved;
                if (!result["ok"].toBool())
                    native.refresh(db);
                else if (active)
                    active->markChanged();
            }
        } else if (action == "snapshot")
            result["ok"] = true;
        result["native"] = snapshot(native.values(&volume)).json();
    }
    return result;
}

#endif
} // namespace kobalt
