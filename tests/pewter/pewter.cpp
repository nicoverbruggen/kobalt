// Pewter's test interface: the database, the options, the call log and the
// checks. The fake Nickel classes are in nickel.h.
#include "nickel.h"
#include <QCoreApplication>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace pewter {
namespace {
const auto nickelConnection = QStringLiteral("pewter-nickel");
const auto testConnection = QStringLiteral("pewter-test");

struct State
{
    Options options;
    QList<Call> calls;
    QString databasePath;
    bool transaction = false;
};

State &state()
{
    static State value;
    return value;
}

[[noreturn]] void fail(const char *message, const QSqlError &error = {})
{
    std::fprintf(stderr, "FAIL pewter: %s %s\n", message, qPrintable(error.text()));
    std::fflush(stderr);
    std::_Exit(1);
}

// The columns that Kobalt reads and writes, with Nickel's names and types.
// Real databases have more columns and tables; add them when a test needs
// them.
const char *const schema[] = {
    "CREATE TABLE content ("
    " ContentID TEXT NOT NULL, ContentType TEXT NOT NULL, MimeType TEXT NOT NULL DEFAULT '',"
    " BookID TEXT, BookTitle TEXT, Title TEXT, Attribution TEXT,"
    " DateLastRead TEXT, ChapterIDBookmarked TEXT, adobe_location TEXT,"
    " ReadStatus INT DEFAULT 0, ___PercentRead INTEGER DEFAULT 0, ___NumPages INT DEFAULT -1,"
    " ReadStateSynced BOOL DEFAULT false, FirstTimeReading BOOL DEFAULT true,"
    " HideFromHomePage BOOL DEFAULT false, TimeSpentReading INT DEFAULT 0,"
    " RestOfBookEstimate INT DEFAULT 0, CurrentChapterEstimate INT DEFAULT 0,"
    " CurrentChapterProgress FLOAT DEFAULT 0, LastTimeStartedReading TEXT, ___SyncTime TEXT,"
    " PRIMARY KEY (ContentID))",
    "CREATE TABLE Activity (Id TEXT NOT NULL, Enabled BOOL DEFAULT true, Type TEXT NOT NULL,"
    " Action TEXT, Date TEXT, Data BLOB, PRIMARY KEY (Id, Type))",
};
} // namespace

void record(const char *name, QVariantMap arguments)
{
    Call call;
    call.name = name;
    call.arguments = std::move(arguments);
    call.mainThread = QCoreApplication::instance()
            && QThread::currentThread() == QCoreApplication::instance()->thread();
    call.inTransaction = state().transaction;
    state().calls.append(call);
}

QSqlDatabase nickelDatabase()
{
    return QSqlDatabase::database(nickelConnection, false);
}

QVariantMap loadContent(const QSqlDatabase &db, const QString &id)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT * FROM content WHERE ContentID = ?"));
    query.addBindValue(id);
    if (!query.exec())
        fail("cannot read content", query.lastError());
    QVariantMap result;
    if (query.next()) {
        const auto record = query.record();
        for (int i = 0; i < record.count(); ++i)
            result.insert(record.fieldName(i), query.value(i));
    }
    return result;
}

bool transactionOpen()
{
    return state().transaction;
}

void setTransactionOpen(bool open)
{
    state().transaction = open;
}

void start(const QString &directory)
{
    for (const auto &name : { nickelConnection, testConnection }) {
        if (QSqlDatabase::contains(name)) {
            QSqlDatabase::database(name, false).close();
            QSqlDatabase::removeDatabase(name);
        }
    }
    auto &s = state();
    s = State();
    s.databasePath = directory + QStringLiteral("/KoboReader.sqlite");
    auto nickel = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), nickelConnection);
    nickel.setDatabaseName(s.databasePath);
    if (!nickel.open())
        fail("cannot create KoboReader.sqlite", nickel.lastError());
    for (const char *statement : schema) {
        QSqlQuery query(nickel);
        if (!query.exec(QString::fromLatin1(statement)))
            fail("cannot create the schema", query.lastError());
    }
    auto test = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), testConnection);
    test.setDatabaseName(s.databasePath);
    if (!test.open())
        fail("cannot open the test connection", test.lastError());
}

Options &options()
{
    return state().options;
}

const QList<Call> &calls()
{
    return state().calls;
}

QList<QByteArray> callNames()
{
    QList<QByteArray> names;
    for (const auto &call : state().calls)
        names.append(call.name);
    return names;
}

const Call *find(const QByteArray &name)
{
    for (const auto &call : state().calls)
        if (call.name == name)
            return &call;
    return nullptr;
}

void clearCalls()
{
    state().calls.clear();
}

QSqlDatabase database()
{
    return QSqlDatabase::database(testConnection, false);
}

void addContent(const QVariantMap &columns)
{
    QStringList names, marks;
    for (auto it = columns.cbegin(); it != columns.cend(); ++it) {
        names.append(it.key());
        marks.append(QStringLiteral("?"));
    }
    QSqlQuery query(database());
    query.prepare(QStringLiteral("INSERT INTO content (%1) VALUES (%2)")
                          .arg(names.join(QLatin1Char(',')), marks.join(QLatin1Char(','))));
    for (const auto &value : columns)
        query.addBindValue(value);
    if (!query.exec())
        fail("cannot add content", query.lastError());
}

QVariantMap row(const QString &contentId)
{
    return loadContent(database(), contentId);
}

void requireAt(bool condition, const char *message, const char *file, int line)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message);
    std::fflush(stderr);
    std::_Exit(1);
}

void requireSameAt(const QVariant &actual, const QVariant &expected, const char *message,
                   const char *file, int line)
{
    if (actual == expected)
        return;
    std::fprintf(stderr, "  actual: %s\n  expected: %s\n", qPrintable(actual.toString()),
                 qPrintable(expected.toString()));
    requireAt(false, message, file, line);
}

void requireNamesAt(const QList<QByteArray> &actual, const QList<QByteArray> &expected,
                    const char *message, const char *file, int line)
{
    if (actual == expected)
        return;
    for (const auto &name : actual)
        std::fprintf(stderr, "  called: %s\n", name.constData());
    requireAt(false, message, file, line);
}
} // namespace pewter
