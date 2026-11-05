#include "reading_history.h"
#include "profile_database.h"
#include "bookmark_store.h"
#include "support/diagnostics.h"
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QSet>
#include <algorithm>
#include <mutex>
#include <utility>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kobalt {
namespace {
QString bounded(QString text)
{
    text.truncate(4096);
    if (!text.isEmpty() && text.at(text.size() - 1).isHighSurrogate())
        text.chop(1);
    return text;
}

QByteArray utf8(const QString &text)
{
    return text.isEmpty() ? QByteArray("") : text.toUtf8();
}
} // namespace

struct ReadingHistory::Saved
{
    std::mutex mutex;
    quint64 successful = 0, finished = 0;
    bool ok = true;
    QVector<Interval> intervals;
};

ReadingHistory::ReadingHistory(QString directory, QString identity, QByteArray fingerprint,
                               QString sourceKey, QString title, QString author, UtcClock utc)
    : directory(std::move(directory)),
      identity(std::move(identity)),
      book(BookmarkStore::bookKey(this->identity, fingerprint)),
      sourceKey(sourceKey.isEmpty() ? this->identity : std::move(sourceKey)),
      title(bounded(std::move(title))),
      author(bounded(std::move(author))),
      fingerprint(std::move(fingerprint)),
      utc(utc ? std::move(utc) : UtcClock([] { return QDateTime::currentMSecsSinceEpoch(); })),
      saved(std::make_shared<Saved>())
{
}

ReadingHistory::~ReadingHistory() = default;

void ReadingHistory::start(qint64 activeMilliseconds, const QString &position)
{
    paused = false;
    if (!pending.isEmpty() && pending.last().status == "active")
        return;
    baseline = activeMilliseconds;
    const auto now = qMax<qint64>(0, utc());
    pending.append({ QUuid::createUuid().toString(), bounded(position), bounded(position), "active",
                     now, now, 0 });
    dirty = true;
    ++generation;
}

void ReadingHistory::update(qint64 activeMilliseconds, const QString &position)
{
    if (paused || pending.isEmpty() || pending.last().status != "active")
        return;
    auto &interval = pending.last();
    interval.milliseconds =
            qMax(interval.milliseconds,
                 activeMilliseconds >= baseline ? activeMilliseconds - baseline : 0);
    interval.observed = qMax(interval.observed, utc());
    interval.endPosition = bounded(position);
    dirty = true;
    ++generation;
}

void ReadingHistory::stop(qint64 activeMilliseconds, const QString &position)
{
    update(activeMilliseconds, position);
    paused = false;
    if (!pending.isEmpty() && pending.last().status == "active") {
        pending.last().status = "completed";
        dirty = true;
        ++generation;
    }
}

void ReadingHistory::pause(qint64 activeMilliseconds, const QString &position)
{
    if (paused || pending.isEmpty() || pending.last().status != "active")
        return;
    update(activeMilliseconds, position);
    paused = true;
}

bool ReadingHistory::saving() const
{
    return writer && !writer->idle();
}

bool ReadingHistory::savesSucceeded()
{
    acknowledge();
    return !dirty;
}

void ReadingHistory::acknowledge()
{
    std::lock_guard<std::mutex> lock(saved->mutex);
    if (saved->successful > acknowledged) {
        QSet<QString> completed;
        for (const auto &interval : saved->intervals)
            if (interval.status != "active")
                completed.insert(interval.id);
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Interval &interval) {
                                         return completed.contains(interval.id);
                                     }),
                      pending.end());
        acknowledged = saved->successful;
        if (saved->successful == generation)
            dirty = false;
    }
    if (saved->finished == posted && !saved->ok)
        posted = 0;
}

bool ReadingHistory::save(bool background)
{
    acknowledge();
    if (!dirty)
        return true;
    if (book.isEmpty())
        return false;
    if (background && posted == generation)
        return true;
    if (!writer)
        writer = std::make_unique<storage::RecordWriter>();
    posted = generation;
    const auto pending = snapshot();
    auto write = [directory = directory, pending] {
        bool ok = false;
        try {
            ok = storage::profileTransaction(directory, pending.write);
        } catch (...) {
        }
        pending.complete(ok);
        return ok;
    };
    if (background) {
        writer->post(std::move(write), {});
        return true;
    }
    const bool ok = writer->writeNow(std::move(write));
    acknowledge();
    return ok;
}

ReadingHistory::Snapshot ReadingHistory::snapshot()
{
    acknowledge();
    const auto intervals = pending;
    const auto version = generation;
    return {
        [book = book, identity = identity, fingerprint = fingerprint, sourceKey = sourceKey,
         title = title, author = author, snapshot = intervals](QSqlDatabase &db) {
            if (book.isEmpty())
                return false;
            for (const auto &interval : snapshot) {
                QSqlQuery insert(db);
                insert.prepare(
                        "INSERT OR IGNORE INTO reading_sessions "
                        "(id,book,identity,fingerprint,source_key,title,author,started,observed,active_ms,start_position,end_position,status) "
                        "VALUES(CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),"
                        "CAST(? AS TEXT),CAST(? AS TEXT),?,?,?,CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT))");
                for (const auto &text :
                     { interval.id, book, identity, QString::fromLatin1(fingerprint), sourceKey,
                       title, author })
                    insert.addBindValue(utf8(text));
                insert.addBindValue(interval.started);
                insert.addBindValue(interval.observed);
                insert.addBindValue(interval.milliseconds);
                insert.addBindValue(utf8(interval.startPosition));
                insert.addBindValue(utf8(interval.endPosition));
                insert.addBindValue(utf8(interval.status));
                if (!insert.exec())
                    return false;
                QSqlQuery update(db);
                update.prepare(
                        "UPDATE reading_sessions SET observed=?,active_ms=?,end_position=CAST(? AS TEXT),status=CAST(? AS TEXT) "
                        "WHERE id=CAST(? AS TEXT) AND book=CAST(? AS TEXT) AND started=? AND active_ms<=? AND observed<=? "
                        "AND (status='active' OR status=CAST(? AS TEXT))");
                update.addBindValue(interval.observed);
                update.addBindValue(interval.milliseconds);
                update.addBindValue(utf8(interval.endPosition));
                update.addBindValue(utf8(interval.status));
                update.addBindValue(utf8(interval.id));
                update.addBindValue(utf8(book));
                update.addBindValue(interval.started);
                update.addBindValue(interval.milliseconds);
                update.addBindValue(interval.observed);
                update.addBindValue(utf8(interval.status));
                if (!update.exec() || update.numRowsAffected() != 1)
                    return false;
            }
            return true;
        },
        [result = saved, generation = version, snapshot = intervals](bool ok) {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->finished = generation;
            result->ok = ok;
            if (ok && generation >= result->successful) {
                result->successful = generation;
                result->intervals = snapshot;
            }
            if (!ok)
                diagnostic("reading-history-unsaved", {}, DiagnosticSeverity::Error);
        }
    };
}

class ReadingHistory::Profile
{
public:
    explicit Profile(const QString &directory)
        : descriptor(::open(QFile::encodeName(directory + "/.reading-history.lock").constData(),
                            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600))
    {
    }

    ~Profile()
    {
        if (descriptor >= 0)
            ::close(descriptor);
    }

    bool acquire(QString *error)
    {
        struct stat info{};
        if (descriptor < 0 || ::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode))
            return false;
        int result;
        do {
            result = ::flock(descriptor, LOCK_EX | LOCK_NB);
        } while (result != 0 && errno == EINTR);
        if (result == 0)
            return true;
        if (error && (errno == EWOULDBLOCK || errno == EAGAIN))
            *error = "profile-in-use";
        return false;
    }

    // Never unlink this inode on release. Another owner may already have it
    // open. The OS drops ownership when the last handle closes or its process dies.
    int descriptor = -1;
};

std::shared_ptr<ReadingHistory::Profile> ReadingHistory::openProfile(const QString &directory,
                                                                     QString *error)
{
    if (error)
        *error = "reading-history-unavailable";
    if (directory.isEmpty() || !QDir().mkpath(directory))
        return {};
    const auto key = QDir(directory).canonicalPath();
    if (key.isEmpty())
        return {};
    static QMutex mutex;
    static QHash<QString, std::weak_ptr<Profile>> profiles;
    QMutexLocker guard(&mutex);
    for (auto it = profiles.begin(); it != profiles.end();) {
        if (it.value().expired())
            it = profiles.erase(it);
        else
            ++it;
    }
    auto owner = profiles.value(key).lock();
    if (!owner) {
        owner = std::make_shared<Profile>(key);
        if (!owner->acquire(error))
            return {};
        if (!recover(key))
            return {};
        profiles.insert(key, owner);
    }
    if (error)
        error->clear();
    return owner;
}

bool ReadingHistory::recover(const QString &directory)
{
    return storage::profileTransaction(directory, [](QSqlDatabase &db) {
        QSqlQuery query(db);
        return query.exec("UPDATE reading_sessions SET status='interrupted' WHERE status='active'");
    });
}
} // namespace kobalt
