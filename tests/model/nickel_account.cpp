#include "platforms/kobo/nickel-qt5/native_account.h"
#include "check.h"
#include <QCoreApplication>
#include <QTemporaryDir>

// Checks that an unavailable account read differs from a confirmed sign-out.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "Create account fixture");
    const auto path = directory.filePath("account.sqlite");
    auto reader = QSqlDatabase::addDatabase("QSQLITE", "account-reader");
    reader.setDatabaseName(path);
    require(reader.open(), "Open account reader");
    QSqlQuery query(reader);
    require(query.exec("PRAGMA busy_timeout=0") && query.exec("CREATE TABLE user (UserID TEXT)")
                    && query.exec("INSERT INTO user VALUES ('original')"),
            "Create account fixture row");
    auto account = kobalt::nickel_qt5::readAccount(reader);
    require(account.valid && account.identity == "original", "Read initial account");
    auto writer = QSqlDatabase::addDatabase("QSQLITE", "account-writer");
    writer.setDatabaseName(path);
    require(writer.open(), "Open competing account writer");
    QSqlQuery lock(writer);
    require(lock.exec("BEGIN EXCLUSIVE"), "Hold exclusive account transaction");
    account = kobalt::nickel_qt5::readAccount(reader);
    require(!account.valid
                    && kobalt::nickel_qt5::checkAccount(account, "original")
                            == kobalt::nickel_qt5::AccountCheck::Deferred,
            "A busy account read was reported as a confirmed sign-out");
    require(lock.exec("ROLLBACK"), "Release account transaction");
    account = kobalt::nickel_qt5::readAccount(reader);
    require(account.valid && account.identity == "original"
                    && kobalt::nickel_qt5::checkAccount(account, "original")
                            == kobalt::nickel_qt5::AccountCheck::Current,
            "Account read did not recover");
    require(query.exec("UPDATE user SET UserID='replacement'"), "Replace account");
    account = kobalt::nickel_qt5::readAccount(reader);
    require(account.valid && account.identity == "replacement"
                    && kobalt::nickel_qt5::checkAccount(account, "original")
                            == kobalt::nickel_qt5::AccountCheck::Changed,
            "Changed account was concealed");
    require(query.exec("DELETE FROM user"), "Sign out fixture account");
    account = kobalt::nickel_qt5::readAccount(reader);
    require(account.valid && account.identity.isEmpty()
                    && kobalt::nickel_qt5::checkAccount(account, "original")
                            == kobalt::nickel_qt5::AccountCheck::Changed,
            "Confirmed sign-out was deferred");
    require(query.exec("DROP TABLE user"), "Remove unavailable account table");
    require(!kobalt::nickel_qt5::readAccount(reader).valid,
            "A failed account query was reported as a confirmed sign-out");
}
