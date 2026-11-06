#include "reader_storage.h"
#include "support/main_thread.h"
#include "annotation_database.h"
#include "profile_database.h"
#include "bookmark_store.h"
#include "reading/appearance_settings.h"
#include "reading/sleep_screen.h"
#include "support/diagnostics.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QRegularExpression>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QCryptographicHash>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>

namespace kobalt::storage {
namespace {
QString child(const QString &directory, const QString &name)
{
    return directory.isEmpty() ? QString() : directory + '/' + name;
}
} // namespace

QString Paths::preferences() const
{
    return child(directory, "reader-settings.json");
}

QString Paths::syncAccount() const
{
    return child(directory, "kosync.json");
}

QString Paths::readingSpeed() const
{
    return child(directory, "reading-speed.json");
}

QString Paths::selection() const
{
    return child(directory, "config.json");
}

QString Paths::annotations() const
{
    return child(directory, "annotations");
}

QString Paths::database() const
{
    return child(directory, "kobalt.sqlite");
}

QString Paths::layoutCache() const
{
    return child(directory, "cache/reopen-layout.json");
}

QString Paths::sleepScreen() const
{
    return child(directory, "cache/sleep-screen.png");
}

QString Paths::documentCache() const
{
    return child(directory, ".cache");
}

QString Paths::bookState(const QString &identity) const
{
    return child(directory,
                 bookPrefix
                         + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(),
                                                                        QCryptographicHash::Sha256)
                                                       .toHex())
                         + ".json");
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    mainthread::BlockingOperation blockingOperation("json-write");
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly)) {
        reportDescriptorExhaustion("json-write");
        return false;
    }
    return file.write(bytes) == bytes.size() && file.commit();
}

JsonRead readJsonFile(const QString &path, qint64 maxBytes)
{
    QFile file(path);
    if (!file.exists())
        return { {}, ReadStatus::Missing };
    if (!file.open(QIODevice::ReadOnly)) {
        reportDescriptorExhaustion("json-read");
        return { {}, ReadStatus::Unreadable };
    }
    maxBytes = qMax<qint64>(0, maxBytes);
    if (file.size() > maxBytes)
        return { {}, ReadStatus::TooLarge };
    // The file can grow after size() is checked. Read in chunks and stop one
    // byte past the limit, so a large limit never reserves a large buffer.
    QByteArray bytes;
    for (;;) {
        const qint64 left = maxBytes - bytes.size();
        const auto chunk = file.read(qMin<qint64>(left, 64 * 1024 - 1) + 1);
        if (chunk.isEmpty())
            break;
        bytes += chunk;
        if (bytes.size() > maxBytes)
            return { {}, ReadStatus::TooLarge };
    }
    if (file.error() != QFileDevice::NoError)
        return { {}, ReadStatus::Unreadable };
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return { {}, ReadStatus::Invalid };
    return { document.object(), ReadStatus::Ok };
}

const char *readStatusName(ReadStatus status)
{
    switch (status) {
    case ReadStatus::Ok:
        return "ok";
    case ReadStatus::Missing:
        return "missing";
    case ReadStatus::Unreadable:
        return "unreadable";
    case ReadStatus::TooLarge:
        return "too-large";
    case ReadStatus::Invalid:
        return "invalid";
    case ReadStatus::Unsupported:
        return "unsupported";
    case ReadStatus::DatabaseError:
        return "database-error";
    }
    return "unknown";
}

QJsonObject readJson(const QString &path)
{
    return readJsonFile(path).object;
}

namespace {
bool putDocument(QSqlDatabase &db, const QString &name, const QJsonObject &object, bool exists)
{
    const bool book = name.startsWith("book:");
    const QString table = book ? "book_state" : "profile_state";
    const QString column = book ? "identity" : "name";
    const auto key = book ? name.mid(5) : name;
    QSqlQuery query(db);
    const auto sql = exists
            ? "UPDATE " + table + " SET payload=CAST(? AS TEXT),revision=revision+1 WHERE " + column
                    + "=CAST(? AS TEXT)"
            : "INSERT INTO " + table + "(payload," + column
                    + ",format) VALUES(CAST(? AS TEXT),CAST(? AS TEXT),1)";
    if (!query.prepare(sql))
        return false;
    query.addBindValue(QJsonDocument(object).toJson(QJsonDocument::Compact));
    query.addBindValue(key.toUtf8());
    return query.exec() && query.numRowsAffected() == 1;
}

// Archiving follows commit. If it is interrupted, the import ledger still
// prevents the source from overwriting newer database data on the next run.
void archiveSource(const QString &directory, const QString &source, const QByteArray &digest)
{
    if (source.isEmpty() || digest.isEmpty())
        return;
    const QFileInfo info(source);
    if (info.isSymLink() || info.absolutePath() != QDir(directory).absolutePath())
        return;
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || hash.result() != digest)
        return;
    file.close();
    const auto backup = directory + "/migration-backup";
    if (!QDir().mkpath(backup))
        return;
    const auto destination = backup + '/' + info.fileName();
    if (!QFileInfo::exists(destination))
        QFile::rename(source, destination);
    else {
        QFile previous(destination);
        QCryptographicHash archived(QCryptographicHash::Sha256);
        if (previous.open(QIODevice::ReadOnly) && archived.addData(&previous)
            && archived.result() == digest)
            QFile::remove(source);
    }
}

bool recordTransaction(const QString &directory, const QString &name, const QString &legacy,
                       qint64 maxBytes, JsonRead *read, const QJsonObject *replacement,
                       QSqlDatabase *database = nullptr)
{
    if (directory.isEmpty() || name.isEmpty() || maxBytes < 0) {
        read->status = ReadStatus::Invalid;
        return false;
    }
    QByteArray importedDigest;
    const auto operation = [&](QSqlDatabase &db) {
        const bool book = name.startsWith("book:");
        const QString table = book ? "book_state" : "profile_state";
        const QString column = book ? "identity" : "name";
        const auto key = book ? name.mid(5) : name;
        if (key.isEmpty())
            return false;
        QSqlQuery query(db);
        // Bound the payload in SQL before copying it into the process.
        if (!query.prepare(
                    "SELECT format,length(CAST(payload AS BLOB)),CASE WHEN length(CAST(payload AS BLOB))<=? "
                    "THEN CAST(payload AS BLOB) END FROM "
                    + table + " WHERE " + column + "=CAST(? AS TEXT)"))
            return false;
        query.addBindValue(maxBytes);
        query.addBindValue(key.toUtf8());
        if (!query.exec())
            return false;
        bool exists = query.next();
        QJsonObject object;
        if (exists) {
            if (query.value(0).toDouble() != 1) {
                read->status = ReadStatus::Unsupported;
                return false;
            }
            if (query.value(1).toLongLong() > maxBytes) {
                read->status = ReadStatus::TooLarge;
                return false;
            }
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(query.value(2).toByteArray(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                read->status = ReadStatus::Invalid;
                return false;
            }
            object = document.object();
            if (object.contains("schema") && object.value("schema").toDouble(-1) != 1) {
                read->status = ReadStatus::Unsupported;
                return false;
            }
            if (book && object.value("book").toString() != key) {
                read->status = ReadStatus::Invalid;
                return false;
            }
            query.finish();
            if (!query.prepare(
                        "SELECT CAST(digest AS BLOB) FROM imports WHERE source=? AND record=?"))
                return false;
            query.addBindValue(legacy);
            query.addBindValue(name);
            if (!query.exec())
                return false;
            if (query.next())
                importedDigest = query.value(0).toByteArray();
        } else if (!legacy.isEmpty()) {
            *read = readJsonFile(legacy, maxBytes);
            if (read->status != ReadStatus::Missing && read->status != ReadStatus::Ok)
                return false;
            object = read->object;
            if (read->status == ReadStatus::Ok && name.startsWith("book:")
                && object.value("book").toString() != name.mid(5)) {
                read->status = ReadStatus::Invalid;
                return false;
            }
            if (object.contains("schema") && object.value("schema").toDouble(-1) != 1) {
                read->status = ReadStatus::Unsupported;
                return false;
            }
            if (read->status == ReadStatus::Ok) {
                QFile source(legacy);
                if (!source.open(QIODevice::ReadOnly))
                    return false;
                const auto bytes = source.read(maxBytes + 1);
                if (bytes.size() > maxBytes || QJsonDocument::fromJson(bytes).object() != object)
                    return false;
                importedDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
                if (!putDocument(db, name, object, false))
                    return false;
                exists = true;
                if (!query.prepare("INSERT INTO imports(source,digest,record) VALUES(?,?,?)"))
                    return false;
                query.addBindValue(legacy);
                query.addBindValue(importedDigest);
                query.addBindValue(name);
                if (!query.exec())
                    return false;
            }
        }
        if (replacement && (!exists || *replacement != object)
            && !putDocument(db, name, *replacement, exists))
            return false;
        read->object = replacement ? *replacement : object;
        read->status = exists || replacement || !importedDigest.isEmpty() ? ReadStatus::Ok
                                                                          : ReadStatus::Missing;
        return true;
    };
    const bool ok =
            database ? operation(*database) : storage::profileTransaction(directory, operation);
    if (ok && !database)
        archiveSource(directory, legacy, importedDigest);
    if (!ok) {
        read->object = {};
        if (read->status == ReadStatus::Ok || read->status == ReadStatus::Missing)
            read->status = ReadStatus::DatabaseError;
    }
    return ok;
}
} // namespace

JsonRead readRecord(const QString &directory, const QString &name, const QString &legacy,
                    qint64 maxBytes)
{
    JsonRead result;
    recordTransaction(directory, name, legacy, maxBytes, &result, nullptr);
    return result;
}

void archiveImportedSource(const QString &directory, const QString &source)
{
    QByteArray digest;
    const bool ok = storage::profileTransaction(directory, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (!query.prepare("SELECT digest FROM imports WHERE source=?"))
            return false;
        query.addBindValue(source);
        if (!query.exec())
            return false;
        if (query.next())
            digest = query.value(0).toByteArray();
        return true;
    });
    if (ok)
        archiveSource(directory, source, digest);
}

bool writeRecord(const QString &directory, const QString &name, const QString &legacy,
                 const QJsonObject &value, qint64 maxBytes)
{
    if (QJsonDocument(value).toJson(QJsonDocument::Compact).size() > maxBytes
        || (value.contains("schema") && value.value("schema").toDouble(-1) != 1))
        return false;
    JsonRead result;
    return recordTransaction(directory, name, legacy, maxBytes, &result, &value);
}

JsonRead readBookState(const Paths &paths, const QString &identity, const QString &legacy)
{
    auto result = readRecord(paths.directory, "book:" + identity,
                             legacy.isEmpty() ? paths.bookState(identity) : legacy);
    if (result.status == ReadStatus::Ok && result.object.value("book").toString() != identity)
        return { {}, ReadStatus::Invalid };
    return result;
}

bool writeBookState(const Paths &paths, const QString &identity, const QJsonObject &value,
                    const QString &legacy)
{
    return value.value("book").toString() == identity
            && writeRecord(paths.directory, "book:" + identity,
                           legacy.isEmpty() ? paths.bookState(identity) : legacy, value);
}

bool writeReadingActivity(const Paths &paths, const QString &identity, const QJsonObject &book,
                          const QJsonObject &speed,
                          const std::function<bool(QSqlDatabase &)> &history, const QString &legacy)
{
    if ((book.contains("schema") && book.value("schema").toDouble(-1) != 1)
        || (speed.contains("schema") && speed.value("schema").toDouble(-1) != 1)
        || book.value("book").toString() != identity
        || QJsonDocument(book).toJson(QJsonDocument::Compact).size() > maxJsonBytes
        || QJsonDocument(speed).toJson(QJsonDocument::Compact).size() > maxJsonBytes)
        return false;
    return storage::profileTransaction(paths.directory, [&](QSqlDatabase &db) {
        JsonRead result;
        if (!recordTransaction(paths.directory, "book:" + identity,
                               legacy.isEmpty() ? paths.bookState(identity) : legacy, maxJsonBytes,
                               &result, &book, &db))
            return false;
        if (!speed.isEmpty()
            && !recordTransaction(paths.directory, "reading-speed", paths.readingSpeed(),
                                  maxJsonBytes, &result, &speed, &db))
            return false;
        return !history || history(db);
    });
}

namespace {
constexpr auto sleepDefaultsMigrated = "sleep_defaults_migrated";
}

JsonRead readPreferences(const QString &path)
{
    auto read = readJsonFile(path);
    if (read.status != ReadStatus::Ok)
        return read;
    if (read.object.contains("schema") && read.object.value("schema").toDouble(-1) != 1)
        return { {}, ReadStatus::Unsupported };
    auto clean = appearance::cleanPreferences(read.object);
    if (!clean.value(sleepDefaultsMigrated).toBool()) {
        if (sleep::mode(clean) == sleep::Mode::Stock)
            clean[sleep::preferenceKey] = sleep::value(sleep::Mode::Cover);
        clean[sleepDefaultsMigrated] = true;
    }
    clean["schema"] = 1;
    if (clean != read.object && !writeJson(path, clean))
        diagnostic("settings-cleanup-unsaved", {}, DiagnosticSeverity::Error);
    return { clean, ReadStatus::Ok };
}

bool writePreferences(const QString &path, const QJsonObject &object)
{
    const auto old = readJsonFile(path);
    if (old.status != ReadStatus::Ok && old.status != ReadStatus::Missing)
        return false;
    if ((old.object.contains("schema") && old.object.value("schema").toDouble(-1) != 1)
        || (object.contains("schema") && object.value("schema").toDouble(-1) != 1))
        return false;
    auto clean = appearance::cleanPreferences(object);
    clean[sleepDefaultsMigrated] = true;
    clean["schema"] = 1;
    return writeJson(path, clean);
}

namespace {
enum class Damage { None, Corrupt, Incompatible, Unreadable };

// Settings without a schema are older settings that reading upgrades. Other
// versioned files need their schema.
Damage jsonDamage(const QString &path, bool schemaRequired)
{
    const auto read = readJsonFile(path);
    switch (read.status) {
    case ReadStatus::Ok:
        break;
    case ReadStatus::Missing:
        return Damage::None;
    case ReadStatus::Unreadable:
        return Damage::Unreadable;
    default:
        return Damage::Corrupt;
    }
    const auto schema = read.object.value("schema");
    if (schema.isUndefined() && !schemaRequired)
        return Damage::None;
    if (!schema.isDouble())
        return Damage::Corrupt;
    const double version = schema.toDouble();
    if (version == 1)
        return Damage::None;
    return version > 1 && version == std::floor(version) ? Damage::Incompatible : Damage::Corrupt;
}

bool syncDirectory(const QString &path)
{
    const int descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
}

bool present(const QString &path)
{
    const QFileInfo file(path);
    return file.exists() || file.isSymLink();
}

// Moves `name` and any of its companion files, such as SQLite's log, into
// backups/ under one new name, so the set stays together. A backup never
// replaces an earlier one. A failed move puts back what it moved. Returns the
// backup's path relative to `directory`, or an empty string.
QString moveToBackups(const QString &directory, const QString &name, bool corrupt,
                      const QStringList &companions)
{
    const QDir profile(directory);
    if (!profile.mkpath("backups"))
        return {};
    const QString kind = corrupt ? QStringLiteral(".corrupted") : QString();
    QString backup;
    for (int copy = 1; backup.isEmpty(); ++copy) {
        const auto candidate = "backups/" + name + kind
                + (copy > 1 ? '.' + QString::number(copy) : QString()) + ".bak";
        bool taken = present(profile.filePath(candidate));
        for (const auto &suffix : companions)
            taken = taken || present(profile.filePath(candidate + suffix));
        if (!taken)
            backup = candidate;
    }
    QStringList moved;
    const auto move = [&](const QString &from, const QString &to) {
        return ::rename(QFile::encodeName(profile.filePath(from)).constData(),
                        QFile::encodeName(profile.filePath(to)).constData())
                == 0;
    };
    for (const auto &suffix : QStringList{ QString() } + companions) {
        if (!suffix.isEmpty() && !present(profile.filePath(name + suffix)))
            continue;
        if (!move(name + suffix, backup + suffix)) {
            for (const auto &done : moved)
                move(backup + done, name + done);
            return {};
        }
        moved.append(suffix);
    }
    if (!syncDirectory(profile.filePath("backups")) || !syncDirectory(directory))
        return {};
    return backup;
}

// Moves damaged saved data, and data from a newer version, out of the way so
// Kobalt can start from defaults. False when the storage cannot be used.
bool recoverSavedData(const Paths &paths, ProfileRecovery *recovery)
{
    const auto failed = [&](const QString &file) {
        recovery->storageFailed = true;
        diagnostic("profile-recovery-failed", { { "file", file } }, DiagnosticSeverity::Error);
        return false;
    };
    const auto moveAside = [&](const QString &file, Damage damage,
                               const QStringList &companions = {}) {
        const auto backup =
                moveToBackups(paths.directory, file, damage == Damage::Corrupt, companions);
        if (backup.isEmpty())
            return false;
        recovery->backups.append(backup);
        diagnostic("profile-recovered", { { "file", file }, { "backup", backup } },
                   DiagnosticSeverity::Warning);
        return true;
    };
    const auto settings = QFileInfo(paths.preferences()).fileName();
    switch (const auto damage = jsonDamage(paths.preferences(), false)) {
    case Damage::None:
        break;
    case Damage::Unreadable:
        return failed(settings);
    default:
        if (!moveAside(settings, damage) || !writePreferences(paths.preferences(), {}))
            return failed(settings);
        recovery->settings = true;
    }
    const auto account = QFileInfo(paths.syncAccount()).fileName();
    switch (const auto damage = jsonDamage(paths.syncAccount(), true)) {
    case Damage::None:
        break;
    case Damage::Unreadable:
        return failed(account);
    default:
        if (!moveAside(account, damage))
            return failed(account);
        recovery->syncAccount = true;
    }
    if (!annotationDatabaseEnabled())
        return true;
    const auto database = QFileInfo(paths.database()).fileName();
    QString error;
    switch (const auto state = inspectProfileDatabase(paths.directory, &error)) {
    case ProfileDatabaseState::Incompatible:
    case ProfileDatabaseState::Corrupt: {
        const auto damage = state == ProfileDatabaseState::Incompatible ? Damage::Incompatible
                                                                        : Damage::Corrupt;
        diagnostic("profile-database-rejected", { { "error_code", error } },
                   DiagnosticSeverity::Warning);
        if (!moveAside(database, damage, { "-wal", "-journal", "-shm" }))
            return failed(database);
        recovery->database = true;
        break;
    }
    case ProfileDatabaseState::Unavailable:
        diagnostic("profile-database-unavailable", { { "error_code", error } },
                   DiagnosticSeverity::Error);
        return failed(database);
    default:
        break;
    }
    return true;
}
} // namespace

bool prepareProfile(const Paths &paths, ProfileRecovery *recovery)
{
    if (paths.directory.isEmpty())
        return false;
    ProfileRecovery recovered;
    const bool usable = recoverSavedData(paths, &recovered);
    if (recovery)
        *recovery = recovered;
    if (!usable)
        return false;
    const QDir directory(paths.directory);
    bool ok = true;
    const QRegularExpression bookName("^" + QRegularExpression::escape(paths.bookPrefix)
                                      + "[0-9a-f]{64}\\.json$");
    for (const auto &file : directory.entryInfoList({ "*.json" }, QDir::Files | QDir::NoSymLinks)) {
        if (!bookName.match(file.fileName()).hasMatch())
            continue;
        const auto read = readJsonFile(file.absoluteFilePath());
        const auto identity = read.object.value("book").toString();
        if (read.status != ReadStatus::Ok || identity.isEmpty()
            || QFileInfo(paths.bookState(identity)).fileName() != file.fileName()) {
            ok = false;
            continue;
        }
        if (readBookState(paths, identity).status != ReadStatus::Ok)
            ok = false;
    }
    for (const auto *name : { "reading-speed", "last-book" }) {
        const auto source = directory.filePath(QString::fromLatin1(name) + ".json");
        if (QFileInfo::exists(source)
            && readRecord(paths.directory, QString::fromLatin1(name), source,
                          qstrcmp(name, "last-book") == 0 ? 256 * 1024 : maxJsonBytes)
                            .status
                    != ReadStatus::Ok)
            ok = false;
    }
    if (annotationDatabaseEnabled()) {
        const QDir annotations(paths.annotations());
        const auto folders =
                annotations.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const auto &folder : folders) {
            if (folder.fileName() == ".index")
                continue;
            const auto metadata =
                    readJsonFile(folder.filePath() + "/book.json", BookmarkStore::maxFileBytes);
            const auto identity = metadata.object.value("identity").toString();
            const auto fingerprint = metadata.object.value("fingerprint").toString().toLatin1();
            if (metadata.status != ReadStatus::Ok
                || metadata.object.value("version") != QJsonValue(1) || identity.isEmpty()
                || BookmarkStore::bookKey(identity, fingerprint) != folder.fileName()) {
                ok = false;
                continue;
            }
            bool recognized = true;
            for (const auto &entry :
                 QDir(folder.filePath()).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                if (!entry.isFile() || entry.isSymLink()
                    || (entry.fileName() != "book.json" && entry.fileName() != "bookmarks.json"
                        && entry.fileName() != "highlights.json")) {
                    recognized = false;
                    break;
                }
                const auto source = readJsonFile(entry.filePath(), 8 * 1024 * 1024);
                if (source.status != ReadStatus::Ok
                    || source.object.value("version") != QJsonValue(1))
                    recognized = false;
            }
            if (!recognized) {
                ok = false;
                continue;
            }
            QVector<Bookmark> bookmarks;
            AnnotationDatabaseState state;
            QString error;
            if (!AnnotationDatabase::load({ paths.annotations(), identity,
                                            metadata.object.value("title").toString(),
                                            folder.fileName(), fingerprint, paths.database() },
                                          &bookmarks, &state, &error)) {
                ok = false;
                continue;
            }
            const auto backup = paths.directory + "/migration-backup/annotations";
            if (!QDir().mkpath(backup)
                || !QDir().rename(folder.filePath(), backup + '/' + folder.fileName()))
                ok = false;
        }
        // The path lookup index is derived from the old annotation folders.
        // Keep it with the import snapshots rather than mixing it into caches.
        const auto index = annotations.filePath(".index");
        if (QFileInfo::exists(index) && !QFileInfo(index).isSymLink()) {
            const auto backup = paths.directory + "/migration-backup/annotations";
            if (!QDir().mkpath(backup) || !QDir().rename(index, backup + "/.index"))
                ok = false;
        }
        QDir().rmdir(paths.annotations());
    }
    if (QFileInfo::exists(paths.preferences())) {
        const auto settings = readPreferences(paths.preferences());
        if (settings.status != ReadStatus::Ok)
            ok = false;
    }
    const auto oldCache = directory.filePath("reopen-layout.json");
    if (QFileInfo::exists(oldCache) && !QFileInfo(oldCache).isSymLink()) {
        const auto read = readJsonFile(oldCache);
        if (read.status == ReadStatus::Ok && read.object.value("version").toInt() == 1) {
            QDir().mkpath(QFileInfo(paths.layoutCache()).absolutePath());
            if (!QFileInfo::exists(paths.layoutCache()))
                QFile::rename(oldCache, paths.layoutCache());
            else
                QFile::remove(oldCache); // Both are disposable inspection results.
        }
    }
    // Startup can already have created the new diagnostic log. Keep the old
    // reports with the import snapshots so neither copy replaces the other.
    const auto diagnostics = directory.filePath("migration-backup/diagnostics");
    for (const auto *name :
         { "state.json", "session.json", "events.jsonl", "events.jsonl.previous" }) {
        const auto source = directory.filePath(QString::fromLatin1(name));
        if (QFileInfo::exists(source) && !QFileInfo(source).isSymLink()) {
            if (!QDir().mkpath(diagnostics)
                || !QFile::rename(source, diagnostics + '/' + QString::fromLatin1(name)))
                ok = false;
        }
    }
    return ok;
}
} // namespace kobalt::storage
