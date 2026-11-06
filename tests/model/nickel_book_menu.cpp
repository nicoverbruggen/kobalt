#include "platforms/kobo/common/book_menu.h"
#include "platforms/kobo/common/book_protection.h"
#include "support/strings.h"
#include "check.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QWidgetAction>

#ifdef __APPLE__
#  define NICKEL_SYMBOL(name) asm("_" name)
#else
#  define NICKEL_SYMBOL(name) asm(name)
#endif
namespace {
QVariantMap fields;
QString identity;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
QString cloudPath;
bool cloudBook = false;
#endif
bool available = true, preferred = true, preview = false, enabled = true;
int openings = 0;

QWidget *menuItem(QMenu *menu, const QString &text, const QString &name)
{
    auto *widget = new QWidget(menu);
    widget->setObjectName(name);
    new QLabel(text, widget);
    return widget;
}

QAction *menuAction(QMenu *menu, QWidget *widget)
{
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(widget);
    menu->addAction(action);
    return action;
}
} // namespace

QVariantMap volumeValues(const void *) NICKEL_SYMBOL("_ZNK6Volume11getDbValuesEv");

QVariantMap volumeValues(const void *)
{
    return fields;
}

bool isPreview(const void *) NICKEL_SYMBOL("_ZNK6Volume9isPreviewEv");

bool isPreview(const void *)
{
    return preview;
}
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
QString volumeId(const void *) NICKEL_SYMBOL("_ZNK7Content5getIdEv");

QString volumeId(const void *)
{
    return identity;
}
#else
QString volumeId(const void *, const QString &) NICKEL_SYMBOL("_ZNK7Content5getIdERK7QString");

QString volumeId(const void *, const QString &)
{
    return identity;
}

bool isAdobe(const void *) NICKEL_SYMBOL("_ZNK7Content10isAdobeDrmEv");

bool isAdobe(const void *)
{
    return false;
}

bool isLcp(const void *) NICKEL_SYMBOL("_ZNK7Content9isLcpEpubEv");

bool isLcp(const void *)
{
    return false;
}

bool isKoboCloud(const void *) NICKEL_SYMBOL("_ZNK6Volume11isKoboCloudEv");

bool isKoboCloud(const void *)
{
    return cloudBook;
}

QString nativeVolumePath(const void *)
        NICKEL_SYMBOL("_ZN13VolumeManager17getVolumeFilePathERK6Volume");

QString nativeVolumePath(const void *)
{
    return cloudPath;
}
#endif
void *device() NICKEL_SYMBOL("_ZN6Device16getCurrentDeviceEv");

void *device()
{
    return &fields;
}

const QString &dbName(const void *) NICKEL_SYMBOL("_ZNK6Device9getDbNameEv");

const QString &dbName(const void *)
{
    static const QString name;
    return name;
}
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
QSqlDatabase connection(QString, bool)
        NICKEL_SYMBOL("_ZN15DatabaseAdapter12dbConnectionE7QStringb");

QSqlDatabase connection(QString, bool)
{
    return QSqlDatabase::database();
}
#else
QSqlDatabase connection(const QString &, bool)
        NICKEL_SYMBOL("_ZN15DatabaseAdapter12dbConnectionERK7QStringb");

QSqlDatabase connection(const QString &, bool)
{
    return QSqlDatabase::database();
}
#endif
QWidget *nativeItem(void *, QMenu *, const QString &, bool, bool, const QString &) NICKEL_SYMBOL(
        "_ZN28AbstractNickelMenuController18createMenuTextItemEP5QMenuRK7QStringbbS4_");

QWidget *nativeItem(void *, QMenu *menu, const QString &text, bool, bool, const QString &name)
{
    return menuItem(menu, text, name);
}

QAction *nativeAction(void *, QMenu *, QWidget *, bool, bool, bool)
        NICKEL_SYMBOL("_ZN22AbstractMenuController12createActionEP5QMenuP7QWidgetbbb");

QAction *nativeAction(void *, QMenu *menu, QWidget *widget, bool, bool, bool)
{
    return menuAction(menu, widget);
}
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
extern "C" void kobalt_book_menu(void *, void *, const void *, QMenu *, const QString &,
                                 const QString &, const void *);
void populate(void *, void *, const void *, QMenu *menu, const QString &, const QString &,
              const void *)
{
#else
extern "C" void kobalt_book_menu(void *, void *, const void *, QMenu *, const QString &,
                                 const QString &, bool, const void *);
void populate(void *, void *, const void *, QMenu *menu, const QString &, const QString &, bool,
              const void *)
{
#endif
    auto *read = menuAction(
            menu,
            menuItem(menu, QCoreApplication::translate("ReadBookActionProxy", "Read now"), "read"));
    read->setEnabled(enabled);
    QObject::connect(read, &QAction::triggered, menu, [] { ++openings; });
    menu->addSeparator();
    menuAction(menu, menuItem(menu, "Book details", "details"));
}

QAction *alternative(QMenu &menu)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    kobalt_book_menu(nullptr, nullptr, &fields, &menu, {}, {}, nullptr);
#else
    kobalt_book_menu(nullptr, nullptr, &fields, &menu, {}, {}, false, nullptr);
#endif
    for (auto *action : menu.actions()) {
        auto *item = qobject_cast<QWidgetAction *>(action);
        if (item && item->defaultWidget()->objectName() == "rrAlternativeReaderAction")
            return action;
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    require(argc == 2, "EPUB fixture argument");
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QString path = directory.path() + "/book.epub";
    require(QFile::copy(QString::fromLocal8Bit(argv[1]), path), "copy EPUB fixture");
    identity = "file://" + path;
    fields["MimeType"] = "application/epub+zip";
    auto database = QSqlDatabase::addDatabase("QSQLITE");
    database.setDatabaseName(":memory:");
    require(database.open(), "metadata database");
    QSqlQuery query(database);
    require(query.exec("CREATE TABLE content_keys(volumeid TEXT)"), "Kobo metadata table");
    require(query.exec("CREATE TABLE tolino_contentItem(uuid TEXT, contentProtectionType INTEGER)"),
            "Tolino metadata table");
    require(query.exec("CREATE TABLE tolino_drmKeysKobo(uuid TEXT)"),
            "Tolino key presence table without key values");
    using Protection = kobalt::BookProtection;
    const auto metadata = [&] { return kobalt::bookProtection(database, identity); };
    require(metadata() == Protection::None, "sideload without protection records");
    query.prepare("INSERT INTO tolino_contentItem VALUES (?, ?)");
    query.addBindValue(identity);
    query.addBindValue(8);
    require(query.exec(), "DRM-free purchase metadata");
    for (const auto &sample : QList<QPair<int, Protection>>{ { 0, Protection::None },
                                                             { 8, Protection::None },
                                                             { 16, Protection::None },
                                                             { 1, Protection::Other },
                                                             { 2, Protection::Kobo },
                                                             { 4, Protection::Other },
                                                             { 3, Protection::Other },
                                                             { 18, Protection::Kobo },
                                                             { 32, Protection::Other },
                                                             { -1, Protection::Other } }) {
        query.prepare("UPDATE tolino_contentItem SET contentProtectionType=?");
        query.addBindValue(sample.first);
        require(query.exec(), "change protection metadata");
        require(metadata() == sample.second,
                "protection types distinguish encryption from watermarks");
        const auto route = kobalt::readerBookRoute(&fields);
        require(route
                        == (sample.second == Protection::None ? kobalt::ReaderBookRoute::Direct
                                                              : kobalt::ReaderBookRoute::Protected),
                "opening route rechecks protection metadata without file replacement");
        require(kobalt::readerBookCandidateRoute(&fields) == route,
                "candidate cannot bypass protection metadata");
    }
    require(query.exec("DELETE FROM tolino_contentItem"), "clear synthetic purchase");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto usbIdentity = identity;
    identity = "cloud-upload-id";
    cloudBook = true;
    cloudPath = directory.path() + "/" + identity;
    require(QFile::copy(path, cloudPath), "copy extensionless Kobo Cloud EPUB fixture");
    QString resolved;
    using Route = kobalt::ReaderBookRoute;
    require(kobalt::readerBookCandidateRoute(&fields, nullptr, &resolved) == Route::Direct
                    && resolved == cloudPath,
            "Kobo Cloud candidate uses the path resolved by Nickel");
    require(kobalt::readerBookRoute(&fields) == Route::Direct,
            "Kobo Cloud DRM-free EPUB passes full inspection");
    require(query.exec("INSERT INTO content_keys VALUES ('cloud-upload-id')"),
            "Kobo Cloud protected-book fixture");
    require(kobalt::readerBookRoute(&fields) == Route::Protected,
            "Kobo Cloud does not bypass protection checks");
    require(query.exec("DELETE FROM content_keys WHERE volumeid = 'cloud-upload-id'"),
            "remove the Cloud key-presence fixture");
    cloudPath = directory.path() + "/missing.epub";
    require(kobalt::readerBookCandidateRoute(&fields) == Route::Unsupported,
            "Kobo Cloud book needs a downloaded file");
    identity = usbIdentity;
    cloudBook = false;
    require(kobalt::readerBookRoute(&fields) == Route::Direct, "sideload still uses the USB path");
#endif
    for (const auto &table :
         { QStringLiteral("content_keys"), QStringLiteral("tolino_drmKeysKobo") }) {
        query.prepare("INSERT INTO " + table + " VALUES (?)");
        query.addBindValue(identity);
        require(query.exec(), "matching key-presence record");
        require(metadata() == Protection::Kobo, "key presence is sufficient without reading keys");
        require(kobalt::readerBookRoute(&fields) == kobalt::ReaderBookRoute::Protected,
                "keys keep OSS books native");
        require(query.exec("DELETE FROM " + table), "remove synthetic key record");
    }
    require(query.exec("DROP TABLE tolino_drmKeysKobo"), "optional older schema");
    require(metadata() == Protection::None, "older firmware without Tolino tables remains usable");
    require(query.exec(
                    "ALTER TABLE tolino_contentItem RENAME COLUMN contentProtectionType TO unsupported"),
            "incompatible metadata schema");
    require(metadata() == Protection::Unavailable, "query errors cannot admit a protected book");
    require(kobalt::readerBookRoute(&fields) == kobalt::ReaderBookRoute::Unsupported,
            "metadata failure retains built-in reader");
    require(query.exec("DROP TABLE tolino_contentItem"), "restore older schema");
    kobalt::nativeBookMenu = populate;
    kobalt::configureBookMenu([] { return available; }, [] { return preferred; });
    {
        QMenu menu;
        auto *action = alternative(menu);
        require(action, "unprotected EPUB must offer Nickel when Kobalt is preferred");
        auto *item = static_cast<QWidgetAction *>(action)->defaultWidget();
        require(item->findChild<QLabel *>()->text()
                        == kobalt::strings::get("native_ui.read_with_nickel"),
                "Nickel action label");
        require(menu.actions().indexOf(action) == 2 && menu.actions()[1]->isSeparator(),
                "alternate follows Read now");
        action->trigger();
        require(openings == 1 && !kobalt::readerChoice(&fields, true),
                "Nickel choice uses the native opening once");
        require(kobalt::readerChoice(&fields, true), "choice must not change the default reader");
    }
    fields["AdobeLocation"] = "protected";
    {
        QMenu menu;
        require(alternative(menu), "protected EPUB must still offer Open in Nickel");
    }
    fields.remove("AdobeLocation");
    kobalt::rememberNativeBook(identity, path, false);
    {
        QMenu menu;
        require(alternative(menu), "remembered fallback must still offer Open in Nickel");
    }
    preferred = false;
    {
        QMenu menu;
        require(!alternative(menu),
                "remembered fallback must not offer unreadable content to Kobalt");
    }
    const QString other = directory.path() + "/other.epub";
    require(QFile::copy(path, other), "copy second EPUB");
    identity = "file://" + other;
    {
        QMenu menu;
        require(alternative(menu), "direct EPUB may offer Kobalt when Nickel is preferred");
    }
    fields["AdobeLocation"] = "protected";
    {
        QMenu menu;
        require(!alternative(menu), "protected EPUB must not bypass Kobalt eligibility");
    }
    preferred = true;
    preview = true;
    {
        QMenu menu;
        require(!alternative(menu), "preview must not offer an alternate reader");
    }
    preview = false;
    fields["MimeType"] = "application/pdf";
    {
        QMenu menu;
        require(!alternative(menu), "unsupported format must retain its native menu");
    }
    fields["MimeType"] = "application/epub+zip";
    available = false;
    {
        QMenu menu;
        require(!alternative(menu), "disabled integration must retain its native menu");
    }
    available = true;
    enabled = false;
    {
        QMenu menu;
        require(!alternative(menu), "disabled native Read now must not be bypassed");
    }
}
