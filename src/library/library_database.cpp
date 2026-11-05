#include "library_index.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

namespace kobalt::library {
namespace {
const QStringList fields{ "key",      "folder",       "name",     "title",    "author", "cover",
                          "series",   "series_index", "size",     "modified", "added",  "opened",
                          "metadata", "percent",      "finished", "mark",     "hidden" };

bool textColumn(int column)
{
    return column <= 6 || column == 15;
}

bool boolColumn(int column)
{
    return column == 14 || column == 16;
}

QString text(const QVariant &value)
{
    const auto bytes = QByteArray(" ") + value.toByteArray();
    return QString::fromUtf8(bytes.constData(), bytes.size()).mid(1);
}

QJsonObject options(const Settings &settings)
{
    const char *views[] = { "list", "grid", "home" };
    const char *sorts[] = { "opened", "added", "title", "author", "series" };
    const char *covers[] = { "small", "medium", "large" };
    QJsonObject result;
    result["view"] = QLatin1String(views[int(settings.view)]);
    result["sort"] = QLatin1String(sorts[int(settings.sort)]);
    result["cover_size"] = QLatin1String(covers[int(settings.coverSize)]);
    result["hide_finished"] = settings.hideFinished;
    result["check_at_start"] = settings.checkAtStart;
    return result;
}

Settings parseSettings(const QJsonObject &object)
{
    Settings result;
    const auto view = object.value("view").toString();
    if (view == "list")
        result.view = View::List;
    else if (view == "home")
        result.view = View::Home;
    const auto sort = object.value("sort").toString();
    const QStringList sorts{ "opened", "added", "title", "author", "series" };
    if (sorts.contains(sort))
        result.sort = Sort(sorts.indexOf(sort));
    const auto cover = object.value("cover_size").toString();
    if (cover == "small")
        result.coverSize = CoverSize::Small;
    else if (cover == "large")
        result.coverSize = CoverSize::Large;
    result.hideFinished = object.value("hide_finished").toBool(false);
    result.checkAtStart = object.value("check_at_start").toBool(true);
    return result;
}

bool saveRows(QSqlDatabase &db, const Index &index)
{
    QSqlQuery query(db);
    if (!query.exec("SELECT key FROM library_books"))
        return false;
    QStringList removed;
    while (query.next())
        if (!index.books.contains(query.value(0).toString()))
            removed.append(query.value(0).toString());
    if (query.lastError().isValid())
        return false;
    query.finish();
    for (const auto &key : removed) {
        if (!query.prepare("DELETE FROM library_books WHERE key=?"))
            return false;
        query.addBindValue(key);
        if (!query.exec())
            return false;
    }
    QStringList columns, placeholders, matches;
    for (int i = 0; i < fields.size(); ++i) {
        columns.append(fields[i]);
        placeholders.append(textColumn(i) ? "CAST(? AS TEXT)" : "?");
        matches.append(columns.last() + "=" + placeholders.last());
    }
    const auto statement = "INSERT OR REPLACE INTO library_books(" + columns.join(',') + ") SELECT "
            + placeholders.join(',') + " WHERE NOT EXISTS(SELECT 1 FROM library_books WHERE "
            + matches.join(" AND ") + ')';
    for (const auto &book : index.books) {
        auto object = book.json();
        if (!query.prepare(statement))
            return false;
        for (int repeat = 0; repeat < 2; ++repeat)
            for (int i = 0; i < fields.size(); ++i) {
                const auto value = object.value(fields[i]);
                if (textColumn(i)) {
                    auto bytes = value.toString().toUtf8();
                    if (bytes.isNull())
                        bytes = QByteArray("");
                    query.addBindValue(bytes);
                } else if (boolColumn(i))
                    query.addBindValue(value.toBool() ? 1 : 0);
                else
                    query.addBindValue(value.toDouble());
            }
        if (!query.exec())
            return false;
    }
    if (!query.exec("DELETE FROM library_folders"))
        return false;
    for (int i = 0; i < index.folders.size(); ++i) {
        if (!query.prepare("INSERT INTO library_folders VALUES(CAST(? AS TEXT),CAST(? AS TEXT),?)"))
            return false;
        query.addBindValue(index.folders[i].key.toUtf8());
        query.addBindValue(index.folders[i].label.isEmpty() ? QByteArray("")
                                                            : index.folders[i].label.toUtf8());
        query.addBindValue(i);
        if (!query.exec())
            return false;
    }
    return true;
}
} // namespace

bool Index::loadDatabase(const QString &directory)
{
    Index loaded;
    const auto legacy = directory + "/library.json";
    bool imported = false;
    const bool ok = storage::profileTransaction(directory, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (!query.exec("SELECT format FROM profile_state WHERE name='library'"))
            return false;
        const bool exists = query.next();
        if (exists && query.value(0).toInt() != 1)
            return false;
        query.finish();
        auto preferences = storage::readPreferences(directory + "/reader-settings.json");
        if (preferences.status != storage::ReadStatus::Ok
            && preferences.status != storage::ReadStatus::Missing)
            return false;
        if (!exists) {
            const auto source = storage::readJsonFile(legacy, 16 * 1024 * 1024);
            if (source.status != storage::ReadStatus::Ok
                && source.status != storage::ReadStatus::Missing)
                return false;
            if (source.status == storage::ReadStatus::Ok && !loaded.restore(source.object))
                return false;
            if (!preferences.object.contains("library")) {
                preferences.object["library"] = options(loaded.settings);
                if (!storage::writePreferences(directory + "/reader-settings.json",
                                               preferences.object))
                    return false;
            }
            if (!saveRows(db, loaded)
                || !query.exec(
                        "INSERT INTO profile_state(name,format,payload) VALUES('library',1,'{}')"))
                return false;
            if (source.status == storage::ReadStatus::Ok) {
                QFile file(legacy);
                if (!file.open(QIODevice::ReadOnly))
                    return false;
                const auto bytes = file.read(16 * 1024 * 1024 + 1);
                if (bytes.size() > 16 * 1024 * 1024
                    || QJsonDocument::fromJson(bytes).object() != source.object)
                    return false;
                if (!query.prepare("INSERT INTO imports VALUES(?,?,'library')"))
                    return false;
                query.addBindValue(legacy);
                query.addBindValue(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
                if (!query.exec())
                    return false;
                imported = true;
            }
        } else {
            if (!query.exec(
                        "SELECT CAST(key AS BLOB),CAST(label AS BLOB) FROM library_folders ORDER BY ordinal"))
                return false;
            while (query.next())
                loaded.folders.append({ text(query.value(0)), text(query.value(1)) });
            if (query.lastError().isValid())
                return false;
            query.finish();
            QStringList columns;
            for (int i = 0; i < fields.size(); ++i)
                columns.append(textColumn(i) ? "CAST(" + fields[i] + " AS BLOB)" : fields[i]);
            if (!query.exec("SELECT " + columns.join(',') + " FROM library_books"))
                return false;
            while (query.next()) {
                QJsonObject object;
                for (int i = 0; i < fields.size(); ++i) {
                    if (textColumn(i))
                        object[fields[i]] = text(query.value(i));
                    else if (boolColumn(i))
                        object[fields[i]] = query.value(i).toBool();
                    else
                        object[fields[i]] = query.value(i).toDouble();
                }
                auto book = Book::fromJson(object);
                if (book.key.isEmpty()
                    || (!book.folder.isEmpty() && !loaded.hasFolder(book.folder)))
                    return false;
                loaded.books.insert(book.key, book);
            }
            if (query.lastError().isValid())
                return false;
        }
        loaded.settings = parseSettings(preferences.object.value("library").toObject());
        return true;
    });
    if (!ok)
        return false;
    *this = std::move(loaded);
    // Also retries a rename interrupted after the previous successful commit.
    if (imported || QFile::exists(legacy))
        storage::archiveImportedSource(directory, legacy);
    return true;
}

bool Index::saveDatabase(const QString &directory) const
{
    auto preferences = storage::readPreferences(directory + "/reader-settings.json");
    if (preferences.status != storage::ReadStatus::Ok
        && preferences.status != storage::ReadStatus::Missing)
        return false;
    const auto next = options(this->settings);
    if (preferences.object.value("library").toObject() != next) {
        preferences.object["library"] = next;
        if (!storage::writePreferences(directory + "/reader-settings.json", preferences.object))
            return false;
    }
    return storage::profileTransaction(directory, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        return query.exec("SELECT format FROM profile_state WHERE name='library'") && query.next()
                && query.value(0).toInt() == 1 && saveRows(db, *this);
    });
}
} // namespace kobalt::library
