#include "nickel.h"
#include "platforms/kobo/common/native_progress.h"
#include "reading/reading_bridge.h"
#include "support/main_thread.h"
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

// Checks the firmware adapter's progress writeback against pewter, the fake
// Nickel: which Nickel functions Kobalt calls, with which values, in which
// order and on which thread, what reaches the book row, and that a refused
// or failed save leaves the row as it was. doc/sync-data.md describes the
// contract.
namespace {
using kobalt::NativeProgress;
using kobalt::ReadingActivity;
using kobalt::ReadingEstimates;
using kobalt::ReadingProgress;

constexpr bool firmware4 = QT_VERSION < QT_VERSION_CHECK(6, 0, 0);
const QString book = QStringLiteral("file:///mnt/onboard/pewter.kepub.epub");
const QString chapterRow = book + QStringLiteral("!OEBPS!chapter2.xhtml");
const QString openedAt = QStringLiteral("2026-01-02T03:04:05Z");
// Kobalt's position: chapter file, element and character offset. Firmware 4
// stores the element only; firmware 5 and 6 keep the offset.
const QString position = QStringLiteral("OEBPS/chapter2.xhtml#kobo.3.1:12");
const QString storedPosition =
        firmware4 ? QStringLiteral("OEBPS/chapter2.xhtml#kobo.3.1") : position;

void addBook()
{
    pewter::addContent({ { "ContentID", book },
                         { "ContentType", 6 },
                         { "MimeType", "application/x-kobo-epub+zip" },
                         { "Title", "Pewter" },
                         { "ChapterIDBookmarked", "OEBPS/chapter1.xhtml#kobo.1.1" },
                         { "DateLastRead", openedAt },
                         { "ReadStatus", 1 },
                         { "___PercentRead", 10 },
                         { "ReadStateSynced", true },
                         { "FirstTimeReading", true } });
    pewter::addContent({ { "ContentID", chapterRow },
                         { "ContentType", 9 },
                         { "BookID", book },
                         { "___PercentRead", 0 } });
}

// Page 15 of 100, page 5 of 9 in the second chapter.
ReadingActivity activity()
{
    ReadingActivity value;
    value.position = position;
    value.page = 14;
    value.pages = 100;
    value.chapters = {
        { QStringLiteral("One"), QStringLiteral("OEBPS/chapter1.xhtml"), 0, 0, 2000 },
        { QStringLiteral("Two"), QStringLiteral("OEBPS/chapter2.xhtml"), 10, 0, 3000 }
    };
    value.chapter.current = 1;
    value.chapter.page = 5;
    value.chapter.pages = 9;
    return value;
}

const ReadingEstimates estimates{ 120000, 3600000, false };

ReadingProgress current(const NativeProgress &progress)
{
    QString error;
    const auto value = progress.read(&error);
    require(value.valid && error.isEmpty(), "Kobalt reads the stored progress before it writes");
    return value;
}

bool write(NativeProgress &progress, const QString &location, double percent, bool finished,
           QString *error, ReadingProgress *verified = nullptr)
{
    // The reader writes at a lifecycle boundary, where no input can arrive.
    kobalt::mainthread::AllowBlocking allow("pewter: a lifecycle boundary");
    const auto expected = current(progress);
    pewter::clearCalls();
    ReadingProgress result;
    const auto shown = activity();
    return progress.write(expected, location, percent, finished, verified ? verified : &result,
                          error, &estimates, &shown);
}

int indexOf(const QByteArray &name)
{
    return pewter::callNames().indexOf(name);
}

// The calls that open, save and close the transaction and request the upload.
QList<QByteArray> boundaries()
{
    static const QList<QByteArray> names{ "DatabaseAdapter::beginTransaction",
                                          "VolumeManager::getById",
                                          "VolumeManager::updateFields",
                                          "DatabaseAdapter::endTransaction",
                                          "BookmarkSyncManagerBase::syncReadingState",
                                          "BookmarkSyncManager::sendBookmark" };
    QList<QByteArray> result;
    for (const auto &name : pewter::callNames())
        if (names.contains(name))
            result.append(name);
    return result;
}

void requireMainThread()
{
    for (const auto &call : pewter::calls())
        require(call.mainThread, "Kobalt calls Nickel on Nickel's interface thread");
}

void requireRefusedWrite(const QVariantMap &before, const char *message)
{
    require(pewter::row(book) == before, message);
    require(!pewter::find("BookmarkSyncManager::sendBookmark"),
            "a refused save asks Nickel for no upload");
}

void markReading(NativeProgress &progress)
{
    pewter::clearCalls();
    {
        kobalt::mainthread::AllowBlocking allow("pewter: the book opens before input");
        require(progress.markReading(), "opening marks the book as reading");
    }
    requireNames(
            boundaries(),
            (QList<QByteArray>{ "DatabaseAdapter::beginTransaction", "VolumeManager::updateFields",
                                "DatabaseAdapter::endTransaction" }),
            "opening updates the status in one transaction");
    const auto *update = pewter::find("VolumeManager::updateFields");
    require(update->inTransaction, "the status update runs inside Nickel's transaction");
    const QVariantMap fields{ { "ReadStatus", 1 },
                              { "FirstTimeReading", false },
                              { "ReadStateSynced", false },
                              { "HideFromHomePage", false } };
    requireSame(update->arguments.value("fields"), fields,
                "opening sets the status fields of Nickel's updateReadingStatus only");
    requireSame(pewter::find("DatabaseAdapter::endTransaction")->arguments.value("commit"), true,
                "opening commits the status");
    require(!pewter::row(book).value("ReadStateSynced").toBool(),
            "opening marks the reading state for upload");
    requireSame(pewter::row(book).value("DateLastRead"), openedAt, "opening keeps DateLastRead");
    requireMainThread();
}

void writePartway(NativeProgress &progress)
{
    QString error;
    ReadingProgress verified;
    require(write(progress, position, 15, false, &error, &verified), qPrintable(error));
    requireNames(boundaries(),
                 (QList<QByteArray>{ "DatabaseAdapter::beginTransaction", "VolumeManager::getById",
                                     "VolumeManager::getById", "VolumeManager::updateFields",
                                     "DatabaseAdapter::endTransaction",
                                     "BookmarkSyncManagerBase::syncReadingState",
                                     "BookmarkSyncManager::sendBookmark" }),
                 "a save fills a fresh Volume, updates, commits, then asks for the upload");
    for (const auto &setter :
         { "VolumeManager::updateBookmark", "Content::setReadStatus",
           "Content::setFirstTimeReading", "Content::setPercentRead",
           "Volume::setRestOfBookEstimate", "Volume::setCurrentChapterEstimate",
           "Volume::setCurrentChapterProgress" }) {
        const int index = indexOf(setter);
        require(index > indexOf("VolumeManager::getById")
                        && index < indexOf("VolumeManager::updateFields"),
                setter);
    }
    const auto *update = pewter::find("VolumeManager::updateFields");
    require(update->inTransaction, "the progress update runs inside Nickel's transaction");
    requireSame(update->arguments.value("where"), (QVariantMap{ { "ContentID", book } }),
                "the update selects the book by its content ID");
    const auto fields = update->arguments.value("fields").toMap();
    for (const auto &unchanged : { "ContentID", "Title", "ReadStatus", "MimeType" })
        require(!fields.contains(unchanged), "Kobalt writes only the fields that changed");
    requireSame(pewter::find("DatabaseAdapter::endTransaction")->arguments.value("commit"), true,
                "the save commits");
    require(!pewter::find("BookmarkSyncManager::sendBookmark")->inTransaction,
            "Kobalt asks for the upload after the commit");
    requireMainThread();

    const auto row = pewter::row(book);
    requireSame(row.value("ChapterIDBookmarked"), storedPosition,
                "the position reaches the book row");
    requireSame(verified.location,
                firmware4 ? QStringLiteral("OEBPS/chapter2.xhtml#kobo.3.1") : position,
                "Kobalt reports the position Nickel stored");
    requireSame(row.value("___PercentRead").toInt(), 15, "the book row stores Kobalt's percentage");
    requireSame(row.value("ReadStatus").toInt(), 1, "the book stays in reading");
    require(!row.value("ReadStateSynced").toBool(), "the save marks the reading state for upload");
    require(!row.value("FirstTimeReading").toBool(), "the save clears FirstTimeReading");
    requireSame(row.value("RestOfBookEstimate").toInt(), 3600, "the book estimate is in seconds");
    requireSame(row.value("CurrentChapterEstimate").toInt(), 120,
                "the chapter estimate is in seconds");
    require(std::abs(row.value("CurrentChapterProgress").toDouble() - 4.0 / 9) < 1e-9,
            "chapter progress counts the pages before this one: page 5 of 9 is 4/9");
    const auto date = row.value("DateLastRead").toString();
    require(QRegularExpression(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ$"))
                    .match(date)
                    .hasMatch(),
            "DateLastRead is UTC in whole seconds");
    require(date > openedAt, "DateLastRead moves forward");
    const auto *upload = pewter::find("BookmarkSyncManager::sendBookmark");
    requireSame(upload->arguments.value("ChapterIDBookmarked"), storedPosition,
                "the upload request carries the stored position");
    if (firmware4) {
        const auto *chapter = pewter::find("ShortcoverManager::updatePercentRead");
        require(chapter, "firmware 4 also writes the KEPUB chapter row");
        require(chapter->inTransaction, "the chapter row is part of the same transaction");
        requireSame(chapter->arguments.value("id"), chapterRow,
                    "Kobalt finds the current chapter's row");
        requireSame(pewter::row(chapterRow).value("___PercentRead").toInt(), 55,
                    "the chapter row counts the page on screen: page 5 of 9 is 55");
    } else {
        require(!pewter::find("ShortcoverManager::updatePercentRead"),
                "firmware 5 and 6 save only the book row");
    }
}

void writeLastPage(NativeProgress &progress)
{
    QString error;
    require(write(progress, position, 100, false, &error), qPrintable(error));
    // Nickel's firmware 4 reader keeps 99 until the book is finished;
    // firmware 5 and 6 store 100 on the last page.
    requireSame(pewter::row(book).value("___PercentRead").toInt(), firmware4 ? 99 : 100,
                "the last page stores the firmware's own percentage");
    requireSame(pewter::row(book).value("ReadStatus").toInt(), 1,
                "the last page does not finish the book");
}

void writeFinished(NativeProgress &progress)
{
    QString error;
    require(write(progress, position, 100, true, &error), qPrintable(error));
    const auto row = pewter::row(book);
    requireSame(row.value("ReadStatus").toInt(), 2, "a finished book has ReadStatus 2");
    requireSame(row.value("___PercentRead").toInt(), 100, "a finished book stores 100");
    requireSame(row.value("RestOfBookEstimate").toInt(), 0, "a finished book has no time left");
    requireSame(row.value("CurrentChapterEstimate").toInt(), 0,
                "a finished chapter has no time left");
    require(!pewter::find("ShortcoverManager::updatePercentRead"),
            "a finished book leaves the chapter row alone");
    require(pewter::find("BookmarkSyncManager::sendBookmark"),
            "a finished book asks for the upload");
}

void writeTimeOnly(NativeProgress &progress)
{
    // A plain EPUB position that Nickel cannot store: the time, the status
    // and the percentage only.
    {
        QSqlQuery query(pewter::database());
        query.exec(QStringLiteral("UPDATE content SET ReadStateSynced = 1 WHERE ContentID = '%1'")
                           .arg(book));
    }
    const auto before = pewter::row(book);
    QString error;
    require(write(progress, QString(), 20, false, &error), qPrintable(error));
    const auto row = pewter::row(book);
    require(!pewter::find("VolumeManager::updateBookmark"), "no position, no bookmark call");
    requireSame(row.value("ChapterIDBookmarked"), before.value("ChapterIDBookmarked"),
                "Nickel keeps its own position");
    requireSame(row.value("___PercentRead").toInt(), 20, "the percentage is still written");
    require(row.value("DateLastRead").toString() > before.value("DateLastRead").toString(),
            "the book still becomes the most recently read one");
    require(row.value("ReadStateSynced").toBool(), "only the bookmark call clears the sync flag");
    require(!pewter::find("BookmarkSyncManager::sendBookmark"),
            "a time-only record asks for no upload");
}

void refuseFailures(NativeProgress &progress)
{
    QString error;
    auto before = pewter::row(book);

    pewter::options().beginFails = true;
    require(!write(progress, position, 30, false, &error), "a busy database refuses the save");
    require(!error.isEmpty(), "a refused save reports why");
    require(!pewter::find("VolumeManager::updateFields"), "a busy database gets no update");
    requireRefusedWrite(before, "a busy database leaves the row as it was");
    pewter::options().beginFails = false;

    error.clear();
    pewter::options().updateFails = true;
    require(!write(progress, position, 30, false, &error), "a rejected update fails the save");
    requireNames(
            pewter::callNames().mid(
                    pewter::callNames().lastIndexOf("DatabaseAdapter::endTransaction")),
            (QList<QByteArray>{ "DatabaseAdapter::endTransaction", "Repository::refreshCache" }),
            "a rejected update rolls back and discards Nickel's cache");
    requireSame(pewter::find("DatabaseAdapter::endTransaction")->arguments.value("commit"), false,
                "a rejected update does not commit");
    requireRefusedWrite(before, "a rejected update leaves the row as it was");
    pewter::options().updateFails = false;

    error.clear();
    pewter::options().commitFails = true;
    require(!write(progress, position, 30, false, &error), "a failed commit fails the save");
    require(pewter::find("Repository::refreshCache"), "a failed commit discards Nickel's cache");
    requireRefusedWrite(before, "a failed commit leaves the row as it was");
    pewter::options().commitFails = false;

    error.clear();
    {
        // Another writer, such as Nickel's sync, moves the position after
        // Kobalt read it.
        kobalt::mainthread::AllowBlocking allow("pewter: a lifecycle boundary");
        const auto expected = current(progress);
        QSqlQuery query(pewter::database());
        require(query.exec(QStringLiteral("UPDATE content SET ChapterIDBookmarked = "
                                          "'OEBPS/chapter9.xhtml#kobo.1.1' WHERE ContentID = '%1'")
                                   .arg(book)),
                "the test moves the stored position");
        before = pewter::row(book);
        pewter::clearCalls();
        ReadingProgress verified;
        const auto shown = activity();
        require(!progress.write(expected, position, 30, false, &verified, &error, &estimates,
                                &shown),
                "a position changed by another writer refuses the save");
    }
    require(!pewter::find("VolumeManager::updateFields"), "a changed position gets no update");
    requireRefusedWrite(before, "a changed position stays as the other writer left it");

    error.clear();
    pewter::options().sleeping = true;
    require(!write(progress, position, 30, false, &error), "a sleeping device refuses the save");
    require(!pewter::find("DatabaseAdapter::beginTransaction"),
            "a sleeping device does not touch Nickel's database");
    pewter::options().sleeping = false;

    error.clear();
    pewter::options().syncEligible = false;
    require(write(progress, position, 30, false, &error), qPrintable(error));
    require(pewter::find("BookmarkSyncManagerBase::syncReadingState")
                    && !pewter::find("BookmarkSyncManager::sendBookmark"),
            "Nickel decides whether the reading state is uploaded");
    pewter::options().syncEligible = true;
}

void recoverStaleCache(NativeProgress &progress)
{
    auto stale = pewter::row(book);
    stale.insert("ChapterIDBookmarked", "OEBPS/chapter9.xhtml#kobo.1.1");
    pewter::options().cachedVolume = stale;
    QString error;
    require(DatabaseAdapter::beginTransaction("KoboReader.sqlite"),
            "the test opens another native transaction");
    require(!write(progress, position, 30, false, &error),
            "a nested native transaction must refuse the save");
    require(pewter::transactionOpen() && !pewter::find("Repository::refreshCache")
                    && !pewter::find("DatabaseAdapter::endTransaction"),
            "cache recovery touched another native transaction");
    require(DatabaseAdapter::endTransaction("KoboReader.sqlite", false),
            "the test releases its native transaction");
    error.clear();
    require(write(progress, position, 30, false, &error),
            "a stale cached Volume must recover in the same save");
    const auto names = pewter::callNames();
    require(names.count("Repository::refreshCache") == 1
                    && names.count("VolumeManager::updateFields") == 1
                    && names.count("BookmarkSyncManager::sendBookmark") == 1,
            "cache recovery repeated its refresh, write or upload");
    require(!pewter::find("Repository::refreshCache")->inTransaction,
            "the cache refresh ran inside a transaction");
    const auto before = pewter::row(book);
    pewter::options().cachedVolume = stale;
    pewter::options().refreshClearsCache = false;
    error.clear();
    require(!write(progress, position, 30, false, &error),
            "a cache that stays stale must refuse the save");
    require(pewter::callNames().count("Repository::refreshCache") == 1,
            "a persistent stale cache caused repeated refreshes");
    require(!pewter::find("VolumeManager::updateFields"), "a stale cache gets no update");
    requireRefusedWrite(before, "a stale cache leaves committed progress intact");
    QSqlQuery lock(pewter::database());
    require(lock.exec("BEGIN IMMEDIATE"), "a stale-cache failure releases its transaction");
    require(lock.exec("ROLLBACK"), "the test releases its lock");
    pewter::options().cachedVolume.clear();
    pewter::options().refreshClearsCache = true;
}

void markRecent(NativeProgress &progress)
{
    pewter::clearCalls();
    {
        kobalt::mainthread::AllowBlocking allow("pewter: the book closes");
        progress.markRecent();
    }
    const auto *recent = pewter::find("ActivityManager::makeRecentBookActivity");
    require(recent, "closing records the book for Nickel's Home screen");
    requireSame(recent->arguments.value("id"), book, "the recent book is this book");
    requireSame(recent->arguments.value("enabled"), true, "the book's storage is connected");
    requireSame(recent->arguments.value("notify"), true, "Nickel's Home screen is notified");
    requireMainThread();
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "a scratch directory for KoboReader.sqlite");
    pewter::start(directory.path());
    addBook();
    require(NativeProgress::available(), "the adapter resolves every Nickel function it requires");
    NativeProgress progress(book);
    markReading(progress);
    recoverStaleCache(progress);
    writePartway(progress);
    writeLastPage(progress);
    writeFinished(progress);
    writeTimeOnly(progress);
    refuseFailures(progress);
    markRecent(progress);
    std::printf("pewter progress checks passed (Qt %s)\n", qVersion());
    return 0;
}
