#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
class QSqlDatabase;

namespace kobalt::storage {
// Hosts choose the directories. Filenames and identity hashing are shared so
// changing a host or edition does not move an existing book's saved state.
struct Paths
{
    QString directory{};
    QString startup{}, active{}, stateReport{}, sessionReport{};
    QString bookPrefix{};
    QString preferences() const;
    QString syncAccount() const;
    QString readingSpeed() const;
    QString selection() const;
    QString annotations() const;
    QString database() const;
    QString layoutCache() const;
    QString sleepScreen() const;
    // Parsed books, owned by CREngine. It deletes any other file it finds there.
    QString documentCache() const;
    QString bookState(const QString &identity) const;
};

bool writeJson(const QString &path, const QJsonObject &object);

enum class ReadStatus { Ok, Missing, Unreadable, TooLarge, Invalid, Unsupported, DatabaseError };

struct JsonRead
{
    QJsonObject object;
    ReadStatus status = ReadStatus::Missing;
};

// Ordinary state files are small. A larger file is refused, not parsed.
inline constexpr qint64 maxJsonBytes = 16384;
// Invalid covers bad JSON and a top level that is not an object.
JsonRead readJsonFile(const QString &path, qint64 maxBytes = maxJsonBytes);
const char *readStatusName(ReadStatus status);
// The object, or an empty one after any failure. Use readJsonFile when a
// damaged file must be told apart from a missing one.
QJsonObject readJson(const QString &path);
// Durable profile records. Legacy JSON is imported once and then archived.
// Each record is one bounded, versioned JSON object in SQLite.
JsonRead readRecord(const QString &directory, const QString &name, const QString &legacy,
                    qint64 maxBytes = maxJsonBytes);
bool writeRecord(const QString &directory, const QString &name, const QString &legacy,
                 const QJsonObject &value, qint64 maxBytes = maxJsonBytes);
JsonRead readBookState(const Paths &, const QString &identity, const QString &legacy = {});
bool writeBookState(const Paths &, const QString &identity, const QJsonObject &,
                    const QString &legacy = {});
// Commit one immutable reading snapshot. Roll back all members and legacy
// imports together if any member fails.
bool writeReadingActivity(const Paths &, const QString &identity, const QJsonObject &book,
                          const QJsonObject &speed,
                          const std::function<bool(QSqlDatabase &)> &history,
                          const QString &legacy = {});
JsonRead readPreferences(const QString &path);
bool writePreferences(const QString &path, const QJsonObject &);

// What profile preparation did with saved data it could not use.
struct ProfileRecovery
{
    // Moved into backups/ and replaced: settings by the defaults, the sync
    // account and the database by none.
    bool settings = false, syncAccount = false, database = false;
    // The backups, relative to the profile directory.
    QStringList backups;
    // A file could not be read, or a damaged file could not be moved aside.
    // Kobalt cannot use this profile until the storage works again.
    bool storageFailed = false;

    bool recovered() const { return settings || syncAccount || database; }
};

// Moves damaged saved data, and saved data from a newer version, into
// backups/ and starts again from defaults. Then imports recognized durable
// files and tidies known transient files. Unknown files and failed imports
// are kept. Safe to retry after an interruption.
bool prepareProfile(const Paths &, ProfileRecovery *recovery = nullptr);
void archiveImportedSource(const QString &directory, const QString &source);
} // namespace kobalt::storage
