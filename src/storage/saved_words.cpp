#include "saved_words.h"
#include "profile_database.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTextDocumentFragment>
#include <QVariant>
#include <mutex>

namespace kobalt {
namespace {
QString bounded(QString text, int limit)
{
    text.truncate(limit);
    if (!text.isEmpty() && text.at(text.size() - 1).isHighSurrogate())
        text.chop(1);
    return text;
}

QByteArray utf8(const QString &text)
{
    return text.isEmpty() ? QByteArray("") : text.toUtf8();
}

// The columns are selected as BLOBs, as in reading sessions, so embedded NUL
// characters survive Qt's SQLite TEXT conversion.
QString text(const QVariant &value)
{
    const auto bytes = value.toByteArray();
    return QString::fromUtf8(bytes.constData(), bytes.size());
}

constexpr const char *columns =
        "CAST(book AS BLOB),CAST(word AS BLOB),CAST(identity AS BLOB),CAST(fingerprint AS BLOB),"
        "CAST(source_key AS BLOB),CAST(title AS BLOB),CAST(author AS BLOB),CAST(chapter AS BLOB),"
        "CAST(context AS BLOB),CAST(position AS BLOB),CAST(dictionary AS BLOB),CAST(definition AS BLOB),"
        "created,looked_up,lookups";

SavedWord row(const QSqlQuery &query)
{
    SavedWord word;
    QString *fields[] = { &word.book,      &word.word,     &word.identity,   &word.fingerprint,
                          &word.sourceKey, &word.title,    &word.author,     &word.chapter,
                          &word.context,   &word.position, &word.dictionary, &word.definition };
    for (int i = 0; i < 12; ++i)
        *fields[i] = text(query.value(i));
    word.created = query.value(12).toLongLong();
    word.lookedUp = query.value(13).toLongLong();
    word.lookups = query.value(14).toInt();
    return word;
}
} // namespace

QString SavedWord::key(const QString &word)
{
    return word.simplified().toCaseFolded();
}

QString SavedWord::gloss() const
{
    auto plain = QTextDocumentFragment::fromHtml(definition).toPlainText().simplified();
    // The entry usually repeats the headword first.
    if (plain.startsWith(word, Qt::CaseInsensitive))
        plain = plain.mid(word.size()).trimmed();
    return plain.left(240);
}

struct SavedWords::Queue
{
    struct Item
    {
        std::function<bool(QSqlDatabase &)> operation;
        std::function<void(bool)> done;
    };

    std::mutex mutex;
    QVector<Item> items;
};

SavedWords::SavedWords(QString directory, QObject *parent)
    : QObject(parent), directory(std::move(directory)), queue(std::make_shared<Queue>())
{
}

// The writer finishes queued words, so a lookup made just before closing is
// kept. Owners destroy the store where the main thread may wait.
SavedWords::~SavedWords() = default;

// A RecordWriter keeps one waiting write and replaces it. Each posted write
// takes every queued item, so a replaced write loses nothing.
void SavedWords::enqueue(std::function<bool(QSqlDatabase &)> operation,
                         std::function<void(bool)> done)
{
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->items.append({ std::move(operation), std::move(done) });
    }
    QPointer<SavedWords> guard(this);
    writer.post(
            [queue = queue, directory = directory, guard] {
                QVector<Queue::Item> items;
                {
                    std::lock_guard<std::mutex> lock(queue->mutex);
                    items.swap(queue->items);
                }
                bool all = true;
                for (const auto &item : items) {
                    bool ok = false;
                    try {
                        ok = storage::profileTransaction(directory, item.operation);
                    } catch (...) {
                    }
                    all = all && ok;
                    if (!ok)
                        diagnostic("saved-words-failed", {}, DiagnosticSeverity::Error);
                    if (item.done && guard)
                        qt::invoke(
                                guard.data(),
                                [guard, done = item.done, ok] {
                                    if (guard)
                                        done(ok);
                                },
                                Qt::QueuedConnection);
                }
                return all;
            },
            {});
}

void SavedWords::record(SavedWord word, std::function<void(bool)> finished)
{
    const auto key = SavedWord::key(word.word);
    if (word.book.isEmpty() || key.isEmpty()) {
        if (finished)
            finished(false);
        return;
    }
    word.word = bounded(word.word.simplified(), 256);
    for (auto *field : { &word.identity, &word.fingerprint, &word.sourceKey, &word.title,
                         &word.author, &word.chapter, &word.position, &word.dictionary })
        *field = bounded(*field, maxText);
    word.context = bounded(word.context.simplified(), maxContext);
    word.definition = bounded(word.definition, maxDefinition);
    word.created = qMax<qint64>(0, word.created);
    word.lookedUp = qMax(word.created, word.lookedUp);
    enqueue(
            [word, key](QSqlDatabase &db) {
                // Kobo's Qt 5 SQLite can predate upsert, so update first, then insert.
                QSqlQuery update(db);
                update.prepare(
                        "UPDATE saved_words SET lookups=lookups+1,looked_up=max(looked_up,?),"
                        "dictionary=CASE WHEN ? THEN CAST(? AS TEXT) ELSE dictionary END,"
                        "definition=CASE WHEN ? THEN CAST(? AS TEXT) ELSE definition END,"
                        "identity=CAST(? AS TEXT),fingerprint=CAST(? AS TEXT),source_key=CAST(? AS TEXT),"
                        "title=CAST(? AS TEXT),author=CAST(? AS TEXT) "
                        "WHERE book=CAST(? AS TEXT) AND word_key=CAST(? AS TEXT)");
                // A word saved without a dictionary entry keeps an entry it had.
                const bool defined = !word.definition.isEmpty();
                update.addBindValue(word.lookedUp);
                update.addBindValue(defined);
                update.addBindValue(utf8(word.dictionary));
                update.addBindValue(defined);
                update.addBindValue(utf8(word.definition));
                for (const auto &value : { word.identity, word.fingerprint, word.sourceKey,
                                           word.title, word.author, word.book, key })
                    update.addBindValue(utf8(value));
                if (!update.exec())
                    return false;
                if (update.numRowsAffected() == 1)
                    return true;
                QSqlQuery insert(db);
                insert.prepare(
                        "INSERT INTO saved_words(book,word_key,word,identity,fingerprint,source_key,title,author,"
                        "chapter,context,position,dictionary,definition,created,looked_up,lookups) "
                        "VALUES(CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),"
                        "CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),"
                        "CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),?,?,1)");
                for (const auto &value :
                     { word.book, key, word.word, word.identity, word.fingerprint, word.sourceKey,
                       word.title, word.author, word.chapter, word.context, word.position,
                       word.dictionary, word.definition })
                    insert.addBindValue(utf8(value));
                insert.addBindValue(word.created);
                insert.addBindValue(word.lookedUp);
                return insert.exec();
            },
            [this, finished](bool ok) {
                if (ok && changed)
                    changed();
                if (finished)
                    finished(ok);
            });
}

void SavedWords::remove(const QString &book, const QString &word,
                        std::function<void(bool)> finished)
{
    enqueue(
            [book, key = SavedWord::key(word)](QSqlDatabase &db) {
                QSqlQuery query(db);
                query.prepare(
                        "DELETE FROM saved_words WHERE book=CAST(? AS TEXT) AND word_key=CAST(? AS TEXT)");
                query.addBindValue(utf8(book));
                query.addBindValue(utf8(key));
                return query.exec();
            },
            [this, finished](bool ok) {
                if (ok && changed)
                    changed();
                if (finished)
                    finished(ok);
            });
}

void SavedWords::relocate(const QString &book, const QString &sourceKey, const QString &title,
                          const QString &author)
{
    if (book.isEmpty())
        return;
    enqueue(
            [book, sourceKey = bounded(sourceKey, maxText), title = bounded(title, maxText),
             author = bounded(author, maxText)](QSqlDatabase &db) {
                QSqlQuery query(db);
                query.prepare(
                        "UPDATE saved_words SET source_key=CAST(? AS TEXT),title=CAST(? AS TEXT),"
                        "author=CAST(? AS TEXT) WHERE book=CAST(? AS TEXT) AND (source_key<>CAST(? AS TEXT) OR "
                        "title<>CAST(? AS TEXT) OR author<>CAST(? AS TEXT))");
                for (const auto &value :
                     { sourceKey, title, author, book, sourceKey, title, author })
                    query.addBindValue(utf8(value));
                return query.exec();
            },
            {});
}

void SavedWords::load(const QString &book, Loaded loaded)
{
    auto result = std::make_shared<QVector<SavedWord>>();
    enqueue(
            [book, result](QSqlDatabase &db) {
                QSqlQuery query(db);
                query.setForwardOnly(true);
                query.prepare(
                        QString("SELECT %1 FROM saved_words %2 ORDER BY looked_up DESC,word_key")
                                .arg(columns, book.isEmpty() ? "" : "WHERE book=CAST(? AS TEXT)"));
                if (!book.isEmpty())
                    query.addBindValue(utf8(book));
                if (!query.exec())
                    return false;
                while (query.next())
                    result->append(row(query));
                return true;
            },
            [loaded, result](bool ok) { loaded(ok, ok ? *result : QVector<SavedWord>()); });
}

void SavedWords::contains(const QString &book, const QString &word,
                          std::function<void(bool)> answer)
{
    auto found = std::make_shared<bool>(false);
    enqueue(
            [book, key = SavedWord::key(word), found](QSqlDatabase &db) {
                QSqlQuery query(db);
                query.prepare(
                        "SELECT 1 FROM saved_words WHERE book=CAST(? AS TEXT) AND word_key=CAST(? AS TEXT)");
                query.addBindValue(utf8(book));
                query.addBindValue(utf8(key));
                if (!query.exec())
                    return false;
                *found = query.next();
                return true;
            },
            [answer, found](bool ok) { answer(ok && *found); });
}
} // namespace kobalt
