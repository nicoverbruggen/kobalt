// The fake Nickel functions that firmware 4, 5 and 6 share. See nickel.h.
#include "nickel.h"
#include <QSqlQuery>
#include <QStringList>

using pewter::record;

void Content::setReadStatus(ReadingStatus status)
{
    record("Content::setReadStatus", { { "status", int(status) } });
    pewter::contentData(this).values.insert("ReadStatus", int(status));
}

void Content::setFirstTimeReading(const QVariant &value)
{
    record("Content::setFirstTimeReading", { { "value", value } });
    pewter::contentData(this).values.insert("FirstTimeReading", value);
}

void Content::setPercentRead(const QVariant &value)
{
    record("Content::setPercentRead", { { "value", value } });
    pewter::contentData(this).values.insert("___PercentRead", value);
}

Volume::~Volume()
{
    delete data;
}

QVariantMap Volume::getDbValues() const
{
    return data ? data->values : QVariantMap();
}

void Volume::setRestOfBookEstimate(const QVariant &value)
{
    record("Volume::setRestOfBookEstimate", { { "value", value } });
    data->values.insert("RestOfBookEstimate", value);
}

void Volume::setCurrentChapterEstimate(const QVariant &value)
{
    record("Volume::setCurrentChapterEstimate", { { "value", value } });
    data->values.insert("CurrentChapterEstimate", value);
}

void Volume::setCurrentChapterProgress(const QVariant &value)
{
    record("Volume::setCurrentChapterProgress", { { "value", value } });
    data->values.insert("CurrentChapterProgress", value);
}

Device *Device::getCurrentDevice()
{
    static Device device;
    return &device;
}

const QString &Device::getDbName() const
{
    static const QString name = QStringLiteral("KoboReader.sqlite");
    return name;
}

bool Device::isTolino() const
{
    return pewter::options().tolino;
}

bool DatabaseAdapter::beginTransaction(QString name)
{
    record("DatabaseAdapter::beginTransaction", { { "database", name } });
    // Nickel starts BEGIN IMMEDIATE and refuses a nested transaction.
    if (pewter::options().beginFails || pewter::transactionOpen())
        return false;
    QSqlQuery query(pewter::nickelDatabase());
    if (!query.exec(QStringLiteral("BEGIN IMMEDIATE")))
        return false;
    pewter::setTransactionOpen(true);
    return true;
}

bool DatabaseAdapter::endTransaction(QString name, bool commit)
{
    record("DatabaseAdapter::endTransaction", { { "database", name }, { "commit", commit } });
    if (!pewter::transactionOpen())
        return false;
    const bool rollback = !commit || pewter::options().commitFails;
    QSqlQuery query(pewter::nickelDatabase());
    const bool ended = query.exec(rollback ? QStringLiteral("ROLLBACK") : QStringLiteral("COMMIT"));
    if (ended)
        pewter::setTransactionOpen(false);
    return ended && !(commit && pewter::options().commitFails);
}

Volume VolumeManager::getById(const QString &id, const QString &database)
{
    record("VolumeManager::getById", { { "id", id }, { "database", database } });
    const auto cached = pewter::options().cachedVolume;
    return Volume(new VolumeData{
            id, cached.isEmpty() ? pewter::loadContent(pewter::nickelDatabase(), id) : cached });
}

bool VolumeManager::updateFields(const QString &id, const QMap<QString, QVariant> &fields,
                                 const QMap<QString, QVariant> &where, const QString &database)
{
    record("VolumeManager::updateFields",
           { { "id", id }, { "fields", fields }, { "where", where }, { "database", database } });
    if (pewter::options().updateFails)
        return false;
    if (fields.isEmpty())
        return true;
    QStringList assignments, conditions;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        assignments.append(it.key() + QStringLiteral(" = ?"));
    for (auto it = where.cbegin(); it != where.cend(); ++it)
        conditions.append(it.key() + QStringLiteral(" = ?"));
    QSqlQuery query(pewter::nickelDatabase());
    query.prepare(QStringLiteral("UPDATE content SET %1 WHERE %2")
                          .arg(assignments.join(QStringLiteral(", ")),
                               conditions.join(QStringLiteral(" AND "))));
    for (const auto &value : fields)
        query.addBindValue(value);
    for (const auto &value : where)
        query.addBindValue(value);
    return query.exec() && query.numRowsAffected() == 1;
}

void pewter::storeBookmark(Volume &volume, const QString &location, const QDateTime &date)
{
    auto &values = volume.data->values;
    values.insert("ChapterIDBookmarked", location);
    values.insert("DateLastRead", date.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ")));
    values.insert("ReadStateSynced", false);
}

template <class T>
class Cache;

class Repository
{
public:
    template <class T>
    static Cache<T> *refreshCache(const QString &database);
};

template <class T>
Cache<T> *Repository::refreshCache(const QString &database)
{
    record("Repository::refreshCache", { { "database", database } });
    if (pewter::options().refreshClearsCache)
        pewter::options().cachedVolume.clear();
    return nullptr;
}

template Cache<Volume> *Repository::refreshCache<Volume>(const QString &);

class BookmarkSyncManagerBase : public QObject
{
public:
    bool syncReadingState(const Volume &volume) const;
};

class BookmarkSyncManager : public BookmarkSyncManagerBase
{
public:
    static BookmarkSyncManager *sharedInstance();
    void sendBookmark(const Volume &volume);
};

bool BookmarkSyncManagerBase::syncReadingState(const Volume &volume) const
{
    record("BookmarkSyncManagerBase::syncReadingState", { { "id", volume.data->id } });
    return pewter::options().syncEligible;
}

BookmarkSyncManager *BookmarkSyncManager::sharedInstance()
{
    static BookmarkSyncManager manager;
    return &manager;
}

void BookmarkSyncManager::sendBookmark(const Volume &volume)
{
    const auto &values = volume.data->values;
    record("BookmarkSyncManager::sendBookmark",
           { { "id", volume.data->id },
             { "ChapterIDBookmarked", values.value("ChapterIDBookmarked") },
             { "___PercentRead", values.value("___PercentRead") },
             { "ReadStatus", values.value("ReadStatus") } });
}

class ActivityManager
{
public:
    static bool makeRecentBookActivity(const QByteArray &id, const QString &database, bool enabled,
                                       bool notify);
};

bool ActivityManager::makeRecentBookActivity(const QByteArray &id, const QString &database,
                                             bool enabled, bool notify)
{
    record("ActivityManager::makeRecentBookActivity",
           { { "id", QString::fromUtf8(id) },
             { "database", database },
             { "enabled", enabled },
             { "notify", notify } });
    return true;
}

class N3PowerWorkflowManager
{
public:
    static N3PowerWorkflowManager *sharedInstance();
    bool sleeping() const;
    bool sleepViewVisible() const;
    bool pinCodeInputDialogVisible() const;
};

N3PowerWorkflowManager *N3PowerWorkflowManager::sharedInstance()
{
    static N3PowerWorkflowManager manager;
    return &manager;
}

bool N3PowerWorkflowManager::sleeping() const
{
    return pewter::options().sleeping;
}

bool N3PowerWorkflowManager::sleepViewVisible() const
{
    return false;
}

bool N3PowerWorkflowManager::pinCodeInputDialogVisible() const
{
    return false;
}
