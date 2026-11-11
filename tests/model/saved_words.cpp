#include "storage/saved_words.h"
#include "storage/annotation_database.h"
#include "storage/profile_database.h"
#include "storage/reading_history.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

using namespace kobalt;

namespace {
void require(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

QVariant sql(const QString &directory, const QString &statement)
{
    QVariant value;
    require(storage::profileTransaction(directory,
                                        [&](QSqlDatabase &db) {
                                            QSqlQuery query(db);
                                            if (!query.exec(statement))
                                                return false;
                                            if (query.next())
                                                value = query.value(0);
                                            return true;
                                        }),
            "Saved-words SQL fixture");
    return value;
}

QVariant raw(const QString &directory, const QString &statement)
{
    storage::closeProfile();
    const QString name = "saved-words-schema-probe";
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(directory + "/kobalt.sqlite");
        require(db.open(), "Open raw saved-words fixture");
        QVariant result;
        {
            QSqlQuery query(db);
            require(query.exec(statement), "Raw saved-words SQL");
            if (query.next())
                result = query.value(0);
        }
        db.close();
        QSqlDatabase::removeDatabase(name);
        return result;
    }
}

// Results arrive through the owner's event queue.
template <class Ready>
void wait(Ready ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(ready(), "Saved words did not answer");
}

QVector<SavedWord> load(SavedWords &store, const QString &book = {})
{
    bool done = false, ok = false;
    QVector<SavedWord> words;
    store.load(book, [&](bool success, QVector<SavedWord> result) {
        ok = success;
        words = std::move(result);
        done = true;
    });
    wait([&] { return done; });
    require(ok, "Saved words did not load");
    return words;
}

SavedWord lookup(const QString &book, const QString &word, qint64 when)
{
    SavedWord saved;
    saved.book = book;
    saved.word = word;
    saved.identity = "file:///books/" + book + ".epub";
    saved.fingerprint = QString(64, 'a');
    saved.sourceKey = "/books/" + book + ".epub";
    saved.title = "Type Tester";
    saved.author = "Trevelyan";
    saved.chapter = "Prologue";
    saved.context = "A voice badly set was a kind of discourtesy.";
    saved.position = "/body/p[2]/text().4";
    saved.dictionary = "English";
    saved.definition = "<p><b>" + word + "</b> Rude behaviour.</p>";
    saved.created = saved.lookedUp = when;
    return saved;
}

// A word without a dictionary entry is saved by hand. It never wipes an entry.
void wordsWithoutEntries()
{
    QTemporaryDir root;
    SavedWords store(root.path());
    auto defined = lookup("file-a", "quoin", 1000);
    store.record(defined);
    auto bare = lookup("file-a", "quoin", 2000);
    bare.definition.clear();
    bare.dictionary.clear();
    store.record(bare);
    auto name = lookup("file-a", "Kaladin", 3000);
    name.definition.clear();
    name.dictionary.clear();
    store.record(name);
    const auto words = load(store);
    require(words.size() == 2, "Saved words without entries were lost or doubled");
    require(words[0].word == "Kaladin" && words[0].definition.isEmpty()
                    && words[0].gloss().isEmpty(),
            "A word without an entry gained a definition");
    require(words[1].definition == defined.definition && words[1].dictionary == defined.dictionary
                    && words[1].lookups == 2,
            "A lookup without an entry replaced a saved definition");
}

void recordsAndCounts()
{
    QTemporaryDir root;
    SavedWords store(root.path());
    int changes = 0;
    store.changed = [&] { ++changes; };
    store.record(lookup("file-a", "Discourtesy", 1000));
    auto later = lookup("file-a", "discourtesy", 2000);
    later.context = "Another sentence.";
    later.definition = "<p>Newer definition.</p>";
    store.record(later);
    store.record(lookup("file-b", "estuary", 1500));
    wait([&] { return changes == 3; });
    const auto all = load(store);
    require(all.size() == 2, "Case variants were saved twice or a book was lost");
    require(all[0].book == "file-a" && all[0].lookups == 2 && all[0].lookedUp == 2000
                    && all[0].created == 1000,
            "A repeated lookup was not counted");
    require(all[0].word == "Discourtesy"
                    && all[0].context == "A voice badly set was a kind of discourtesy.",
            "A repeated lookup replaced the first word or sentence");
    require(all[0].definition == "<p>Newer definition.</p>",
            "A repeated lookup kept an old definition");
    require(all[1].word == "estuary", "Words were not ordered by latest lookup");
    require(load(store, "file-b").size() == 1, "A book's list included another book");
    bool found = false, answered = false;
    store.contains("file-a", "DISCOURTESY", [&](bool value) {
        found = value;
        answered = true;
    });
    wait([&] { return answered; });
    require(found, "A saved word was not found in another case");
    store.remove("file-a", "discourtesy");
    wait([&] { return changes == 4; });
    require(load(store, "file-a").isEmpty() && load(store).size() == 1,
            "Removal changed the wrong rows");
}

void survivesBookMoves()
{
    QTemporaryDir root;
    {
        SavedWords store(root.path());
        store.record(lookup("file-a", "quoin", 1000));
        store.relocate("file-a", "/moved/type.epub", "Type Tester, 2nd ed.", "Trevelyan");
    }
    SavedWords reopened(root.path());
    const auto words = load(reopened, "file-a");
    require(words.size() == 1 && words[0].sourceKey == "/moved/type.epub"
                    && words[0].title == "Type Tester, 2nd ed.",
            "A moved book's words lost their new path");
    require(words[0].fingerprint == QString(64, 'a'), "Relocation changed the content fingerprint");
}

void boundsAndText()
{
    QTemporaryDir root;
    SavedWords store(root.path());
    auto word = lookup("file-a", QString::fromUtf8("caf\xc3\xa9"), 10);
    word.context = QString(5000, 'x') + QChar(0) + "tail";
    word.definition = QString(SavedWords::maxDefinition + 10, 'd');
    word.chapter = QString::fromUtf8("\xe6\x97\xa5\xe6\x9c\xac");
    store.record(word);
    const auto words = load(store);
    require(words.size() == 1 && words[0].word == QString::fromUtf8("caf\xc3\xa9")
                    && words[0].chapter == QString::fromUtf8("\xe6\x97\xa5\xe6\x9c\xac"),
            "Unicode did not survive");
    require(words[0].context.size() == SavedWords::maxContext
                    && words[0].definition.size() == SavedWords::maxDefinition,
            "Saved text was not bounded");
    store.record(lookup("", "orphan", 10));
    store.record(lookup("file-a", "   ", 10));
    require(load(store).size() == 1, "A word without a book or text was saved");
}

void migratesFromVersionThree()
{
    QTemporaryDir root;
    require(ReadingHistory::openProfile(root.path()) != nullptr, "Create profile fixture");
    sql(root.path(),
        "INSERT INTO reading_sessions VALUES('s','file-a','id','f','k','t','a',1,2,3,'p','q','completed')");
    sql(root.path(), "DROP TABLE saved_words");
    raw(root.path(), "PRAGMA user_version=3");
    require(raw(root.path(), "PRAGMA user_version").toInt() == 3, "Not a version-3 fixture");
    SavedWords store(root.path());
    require(load(store).isEmpty(), "Migration invented words");
    require(raw(root.path(), "PRAGMA user_version").toInt() == 4,
            "Migration did not advance schema");
    require(sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 1,
            "Migration changed history");
    require(sql(root.path(),
                "SELECT COUNT(*) FROM sqlite_master WHERE name='saved_words_looked_up'")
                            .toInt()
                    == 1,
            "Missing saved-word index");

    QTemporaryDir future;
    SavedWords created(future.path());
    load(created);
    raw(future.path(), "PRAGMA user_version=5");
    bool done = false, ok = true;
    SavedWords rejected(future.path());
    rejected.load({}, [&](bool success, QVector<SavedWord>) {
        ok = success;
        done = true;
    });
    wait([&] { return done; });
    require(!ok && raw(future.path(), "PRAGMA user_version").toInt() == 5,
            "A future schema was opened or changed");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    recordsAndCounts();
    wordsWithoutEntries();
    survivesBookMoves();
    boundsAndText();
    migratesFromVersionThree();
    storage::closeProfile();
    require(QSqlDatabase::connectionNames().isEmpty(),
            "Saved-word checks left SQL connections open");
}
