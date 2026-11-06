#include "support/qt_compat.h"
#include "reading/reading_progress.h"
#include "platforms/kobo/common/native_location.h"
#include "platforms/kobo/common/local_book_path.h"
#include "platforms/kobo/nickel-qt5/native_location.h"
#include <QCoreApplication>
#include "check.h"
#include <limits>
using namespace kobalt;

// Checks progress percentages, local and downloaded book paths, Nickel position formats and which of native or local progress wins.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    struct Display
    {
        int page, pages, percent;
    };

    for (const auto &value :
         { Display{ 0, 0, 0 }, Display{ 0, -1, 0 }, Display{ 0, 1, 100 }, Display{ 0, 2, 50 },
           Display{ 1, 2, 100 }, Display{ 1, 3, 66 }, Display{ 1, 7, 28 }, Display{ 1, 9, 22 },
           Display{ -10, 10, 10 }, Display{ 9, 10, 100 }, Display{ 20, 10, 100 },
           Display{ std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 100 },
           // Measured in Nickel's own reader on firmware 4 and 6: 83 pages.
           Display{ 0, 83, 1 }, Display{ 1, 83, 2 }, Display{ 40, 83, 49 }, Display{ 81, 83, 98 },
           Display{ 82, 83, 100 } }) {
        require(pageProgressPercent(value.page, value.pages) == value.percent,
                "Incorrect displayed progress");
        require(pageProgressFraction(value.page, value.pages) >= 0
                        && pageProgressFraction(value.page, value.pages) <= 1,
                "Progress fraction escaped its bounds");
    }
    for (const auto &path : { QStringLiteral("/mnt/onboard/.sync/.books/.hidden.epub"),
                              QStringLiteral("/mnt/onboard/.kobo/dropbox/.nested/book.epub"),
                              QStringLiteral("/mnt/onboard/.kobo/google_drive/book.epub"),
                              QStringLiteral("/mnt/onboard/.adds/kobocloud/library/book.epub"),
                              QStringLiteral("/mnt/sd/kobocloud/book.epub"),
                              QStringLiteral("/mnt/third-party-sync/books/book#part?%20.epub"),
                              QStringLiteral("/mnt/third-party-sync/books/extensionless") }) {
        require(localReaderBookPath("file://" + path) == path,
                "Local EPUB path was restricted by its folder or changed by URL decoding");
    }
    for (const auto &identity :
         { QStringLiteral("https://example.org/book.epub"),
           QStringLiteral("file://server/book.epub"), QStringLiteral("file://relative.epub"),
           QStringLiteral("12345678-1234-1234-1234-123456789abc"), QString() }) {
        require(localReaderBookPath(identity).isEmpty(),
                "Nonlocal identity became a local EPUB path");
    }
    for (const auto &identity :
         { QStringLiteral("12345"), QStringLiteral("12345678-1234-1234-1234-123456789abc"),
           QStringLiteral("server-book_42"), QStringLiteral("book%2Fpart"),
           QString::fromUtf8("caf\xc3\xa9") }) {
        require(downloadedReaderBookPath(identity) == "/mnt/onboard/.kobo/kepub/" + identity,
                "Downloaded book ID was restricted to UUIDs or changed by URL decoding");
    }
    for (const auto &identity :
         { QString(), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("../book"),
           QStringLiteral("folder/book"), QStringLiteral("folder\\book"),
           QStringLiteral("/absolute"), QStringLiteral("https://example.org/book"),
           QString("book") + QChar(0) + "other" }) {
        require(downloadedReaderBookPath(identity).isEmpty(),
                "Downloaded ID escaped its filename boundary");
    }
    for (const auto &name :
         { QStringLiteral("book.kepub.epub"), QStringLiteral("book#part.kepub.epub"),
           QStringLiteral("book?part.kepub.epub"), QStringLiteral("book%20part.kepub.epub"),
           QStringLiteral("book%23part.kepub.epub"),
           QString::fromUtf8("caf\xc3\xa9 book.kepub.epub") }) {
        for (const auto &root : { QStringLiteral("/mnt/onboard/"), QStringLiteral("/mnt/sd/") }) {
            const auto path = root + name;
            const auto identity = QStringLiteral("file://") + path;
            require(nickel_qt5::localBookPath(identity) == path,
                    "Sideloaded filename was decoded as a URL");
            for (const auto &prefix : { identity, path }) {
                require(nickel_qt5::chapter(identity, prefix + "!OEBPS!chapter.xhtml#kobo.1.1")
                                == "OEBPS/chapter.xhtml#kobo.1.1",
                        "Literal sideloaded filename prevented legacy chapter decoding");
            }
        }
    }
    // A plain EPUB without IDs: firmware 4 stores Nickel's DOM point as is, and
    // the shared reader must see the same point, not a percent-encoded ID.
    const QString point = "OEBPS/text/chapter1.xhtml#point(/1/4/24/1:0)";
    require(nickel_qt5::stored(point) == point && nickel_qt5::shared(point) == point,
            "Firmware 4 changed a plain EPUB DOM point");
    require(nickel_qt5::stored("OEBPS/c.xhtml#kobo.8.1:7") == "OEBPS/c.xhtml#kobo.8.1"
                    && nickel_qt5::shared("OEBPS/c.xhtml#kobo.8.1") == "OEBPS/c.xhtml#kobo.8.1",
            "Firmware 4 element positions changed form");
    require(nickel_qt5::stored("OEBPS/c.xhtml#point(/1/4/x)").isEmpty(),
            "A malformed DOM point was stored");
    require(nickel_qt5::localBookPath("12345678-1234-1234-1234-123456789abc").isEmpty()
                    && nickel_qt5::localBookPath("https://example.org/book.epub").isEmpty(),
            "Nonlocal book identity became a sideloaded path");
    require(publicationLocation("book", "book!OEBPS!chapter.xhtml#kobo.1.1:7")
                    == "OEBPS/chapter.xhtml#kobo.1.1:7",
            "Legacy bookmark lost its chapter or offset");
    require(publicationLocation("book", "book!chapter.xhtml") == "chapter.xhtml",
            "Legacy chapter-only bookmark was not decoded");
    require(publicationLocation("book", "OEBPS/chapter!name.xhtml#id!part:3")
                    == "OEBPS/chapter!name.xhtml#id!part:3",
            "Modern path or fragment was decoded as legacy");
    require(publicationLocation("book", "other!OEBPS!chapter.xhtml#id:0")
                    == "other!OEBPS!chapter.xhtml#id:0",
            "Another book's bookmark was accepted");
    require(publicationLocation("", "!chapter.xhtml") == "!chapter.xhtml",
            "Missing book identity decoded a legacy bookmark");
    // A downloaded book without a position yet has "#" in Nickel's database.
    for (const auto &raw : { QString(), QStringLiteral("#"), QStringLiteral("book!#") }) {
        require(unsetNativeLocation(publicationLocation("book", raw))
                        && unsetNativeLocation(nickel_qt5::chapter("book", raw)),
                "A saved position without chapter or anchor was not read as the start");
    }
    for (const auto &raw :
         { QStringLiteral("OEBPS/chapter.xhtml#kobo.1.1"), QStringLiteral("chapter.xhtml"),
           QStringLiteral("#kobo.1.1"), QStringLiteral("book!OEBPS!chapter.xhtml#") }) {
        require(!unsetNativeLocation(publicationLocation("book", raw))
                        && !unsetNativeLocation(nickel_qt5::chapter("book", raw)),
                "A real saved position was read as the start");
    }
    ReadingProgress first{ "synthetic", "chapter.xhtml#p:4", 1000, 1, true };
    auto second = first;
    second.location = "chapter.xhtml#p:12";
    second.modified = 2000;
    require(!preferNativeProgress(first, first),
            "Matching native baseline discarded local position");
    require(preferNativeProgress(second, first), "Newer native position lost to local save");
    require(!preferNativeProgress(first, second),
            "Older native position replaced a newer checkpoint");
    require(!preferNativeProgress({}, second), "Native read failure discarded local checkpoint");
    require(preferNativeProgress(first, {}), "Initial native position was not imported");
    auto duplicate = first;
    ++duplicate.modified;
    require(!preferNativeProgress(duplicate, first),
            "Unchanged position timestamp discarded local progress");
    auto finished = first;
    ++finished.modified;
    finished.status = 2;
    require(preferNativeProgress(finished, first), "Newer finished status was ignored");
    second.modified = first.modified;
    require(!preferNativeProgress(second, first),
            "Ambiguous equal-date position replaced local checkpoint");
    require(ReadingProgress::fromJson(first.json()) == first,
            "Baseline did not survive local persistence");
    require(!ReadingProgress{}.samePosition({}), "Failed native reads compared equal");
    const QJsonObject checkpoint{ { "book", first.book },
                                  { "position", "/body/p[9]" },
                                  { "font_pixels", 42 },
                                  { "nickel_progress", first.json() } };
    auto unread = first;
    unread.status = 0;
    unread.location.clear();
    // Mark unread can clear the date instead of supplying a newer timestamp.
    unread.modified = 0;
    auto reset = checkpoint;
    require(resetNativeUnread(reset, unread) && reset == QJsonObject({ { "book", first.book } }),
            "Nickel's unread status did not reset book settings and position");
    reset = {};
    auto unreadWithBookmark = unread;
    unreadWithBookmark.location = first.location;
    require(resetNativeUnread(reset, unreadWithBookmark)
                    && reset == QJsonObject({ { "book", first.book } }),
            "An unread book without a Kobalt checkpoint reused Nickel's old location");
    reset = checkpoint;
    require(!resetNativeUnread(reset, {}) && reset == checkpoint,
            "Failed native read reset the local checkpoint");
    require(!resetNativeUnread(reset, second) && reset == checkpoint,
            "A normal reading update reset the book settings");
    auto otherUnread = unread;
    otherUnread.book = "another book";
    require(!resetNativeUnread(reset, otherUnread) && reset == checkpoint,
            "Another book's unread status reset this book");
    reset["nickel_progress"] = unread.json();
    const auto acknowledgedUnread = reset;
    require(!resetNativeUnread(reset, unread) && reset == acknowledgedUnread,
            "Unchanged unread baseline repeatedly reset local progress");
    ProgressReconciler choices(first);
    second.modified = 2000;
    choices.observe(second);
    require(choices.pending() && !choices.mayWrite(first), "Unanswered update allowed writeback");
    choices.observe(second);
    require(choices.candidate() == second, "Duplicate replaced pending candidate");
    auto unrelated = second;
    unrelated.book = "another book";
    unrelated.modified = 4000;
    choices.observe(unrelated);
    choices.observe({});
    require(choices.candidate() == second, "Unrelated or invalid update replaced the candidate");
    auto third = second;
    third.location = "chapter.xhtml#p:18";
    third.modified = 3000;
    choices.observe(third);
    choices.acknowledge(second);
    require(choices.pending() && !choices.mayWrite(third),
            "Old alert accepted an unseen newer update");
    choices.acknowledge(third);
    require(!choices.pending() && choices.mayWrite(third),
            "Explicit choice did not allow writeback");
    choices.observe(third);
    require(!choices.pending(), "Own-save echo or duplicate reopened the alert");
    auto dated = third;
    dated.modified = 4000;
    choices.observe(dated);
    require(!choices.pending() && choices.mayWrite(dated),
            "Timestamp-only change reopened the alert");
    finished = dated;
    finished.modified = 5000;
    finished.status = 2;
    choices.observe(finished);
    require(choices.pending(), "Changed finished status did not ask for a choice");
    choices.acknowledge(finished);
    require(choices.mayWrite(finished), "Finished choice remained unresolved");
    choices.observe(third);
    require(choices.pending() && !choices.mayWrite(third),
            "An older committed position did not retain an explicit choice");
    choices.acknowledge(finished);
    require(choices.pending() && !choices.mayWrite(third),
            "A stale alert acknowledged a different current position");
    choices.acknowledge(third);
    require(choices.mayWrite(third),
            "An older current position remained unwritable after a choice");
    // The displayed position can stay the same while its committed date moves back.
    ProgressReconciler redatedChoice(first);
    auto shown = second;
    shown.modified = 2000;
    redatedChoice.observe(shown);
    auto redated = shown;
    redated.modified = 1500;
    redatedChoice.observe(redated);
    redatedChoice.acknowledge(shown);
    require(!redatedChoice.pending() && redatedChoice.mayWrite(redated),
            "The same position required another choice after its timestamp moved back");
    // A different nonzero offset still needs a choice at the same DateLastRead.
    auto exact = first;
    exact.location = "chapter.xhtml#kobo.50.2:105";
    auto normalized = exact;
    normalized.location = "chapter.xhtml#kobo.50.2:17";
    ProgressReconciler equalTime(exact);
    equalTime.observe(normalized);
    require(equalTime.pending() && !equalTime.mayWrite(normalized),
            "Equal-time position change did not offer a conflict choice");
    equalTime.acknowledge(exact);
    require(equalTime.pending() && !equalTime.mayWrite(normalized),
            "An old equal-time choice accepted an unseen position");
    equalTime.acknowledge(normalized);
    require(!equalTime.pending() && equalTime.mayWrite(normalized),
            "Acknowledged equal-time position remained unwritable");
    equalTime.observe(normalized);
    require(!equalTime.pending(), "Equal-time duplicate reopened the conflict");
    // Nickel can return a saved KEPUB span without its character offset.
    for (const auto &location : { QStringLiteral("chapter.xhtml#kobo.50.2:0"),
                                  QStringLiteral("chapter.xhtml#kobo.50.2") }) {
        auto collapsed = exact;
        collapsed.location = location;
        ProgressReconciler roundTrip(exact);
        roundTrip.observe(collapsed);
        require(!roundTrip.pending() && roundTrip.mayWrite(collapsed)
                        && roundTrip.baseline() == collapsed,
                "Dropped KEPUB offset blocked the next precise save");
        roundTrip.observe(collapsed);
        require(!roundTrip.pending(), "Repeated offset normalization reopened a conflict");
        roundTrip.observe(exact);
        require(roundTrip.pending(), "A new precise offset was silently accepted");
    }
    // Normalization applies only to the acknowledged position, not a pending one.
    auto pendingOffset = exact;
    pendingOffset.location = "chapter.xhtml#kobo.51.1:9";
    auto pendingStart = pendingOffset;
    pendingStart.location = "chapter.xhtml#kobo.51.1:0";
    ProgressReconciler unseen(exact);
    unseen.observe(pendingOffset);
    unseen.observe(pendingStart);
    require(unseen.pending() && !unseen.mayWrite(pendingStart),
            "Normalization acknowledged an unseen native position");
    for (const auto &location :
         { QStringLiteral("chapter.xhtml#kobo.50.3:0"), QStringLiteral("other.xhtml#kobo.50.2:0"),
           QStringLiteral("chapter.xhtml#kobo.50.2:1"),
           QStringLiteral("chapter.xhtml#kobo.50.2:00"),
           QStringLiteral("chapter.xhtml#kobo.50.2:-1") }) {
        auto changed = exact;
        changed.location = location;
        ProgressReconciler strict(exact);
        strict.observe(changed);
        require(strict.pending(), "A different native location was treated as normalization");
    }
    for (const auto &location :
         { QStringLiteral("chapter.xhtml#paragraph:105"),
           QStringLiteral("chapter.xhtml#kobo.x.2:105"),
           QStringLiteral("chapter.xhtml#point(/1/4/2:105)"), QStringLiteral("#kobo.50.2:105"),
           QStringLiteral("chapter.xhtml#kobo.50.2:0") }) {
        auto source = exact;
        source.location = location;
        auto changed = source;
        changed.location = location.left(location.lastIndexOf(':')) + ":0";
        if (changed.location == source.location)
            changed.location.chop(2);
        ProgressReconciler strict(source);
        strict.observe(changed);
        require(strict.pending(), "A non-KEPUB or zero offset was silently normalized");
    }
    auto changedMetadata = exact;
    changedMetadata.location = "chapter.xhtml#kobo.50.2:0";
    ++changedMetadata.modified;
    ProgressReconciler newerOffset(exact);
    newerOffset.observe(changedMetadata);
    require(newerOffset.pending(), "A newer span-start position was silently accepted");
    changedMetadata.modified = exact.modified;
    changedMetadata.status = 2;
    ProgressReconciler changedStatus(exact);
    changedStatus.observe(changedMetadata);
    require(changedStatus.pending(), "A changed reading status was silently normalized");
    ProgressReconciler resetWhileReading(first);
    resetWhileReading.observe(unread);
    require(resetWhileReading.pending() && !resetWhileReading.mayWrite(unread),
            "An unread reset became an unrecoverable stale save failure");
    resetWhileReading.acknowledge(resetWhileReading.candidate());
    require(!resetWhileReading.pending() && resetWhileReading.mayWrite(unread),
            "Keeping local progress did not acknowledge the unread reset");
    auto stalePosition = first;
    --stalePosition.modified;
    stalePosition.location = "chapter.xhtml#p:1";
    ProgressReconciler staleNormal(first);
    staleNormal.observe(stalePosition);
    require(staleNormal.pending() && !staleNormal.mayWrite(stalePosition),
            "An older authoritative position bypassed the conflict choice");
    staleNormal.acknowledge(stalePosition);
    require(staleNormal.mayWrite(stalePosition),
            "An older authoritative position remained unwritable after a choice");
    ProgressReconciler failedRead({ "synthetic", {}, 0, 0, false });
    failedRead.observe(first);
    require(failedRead.pending(), "Recovered native position was silently accepted");
    qInfo("Reading progress reconciliation checks passed");
}
