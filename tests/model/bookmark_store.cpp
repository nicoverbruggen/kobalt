#include "storage/bookmark_store.h"
#include "support/qt_compat.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>

#include <utime.h>
#include <QTemporaryDir>
#include <cstdio>
#include "check.h"

using kobalt::Bookmark;
using kobalt::BookmarkStore;

namespace {
const QByteArray fingerprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

Bookmark mark(const QString &position, const QString &created)
{
    return { position, "Chapter", "First words",
             QDateTime::fromString(created, Qt::ISODate).toUTC() };
}

QByteArray read(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read bookmark fixture");
    return file.readAll();
}
} // namespace

// Checks the JSON bookmark store: book keys, edited-file adoption, damaged and oversized files, failed writes, page merging and excerpts.
int main()
{
    // These cases obstruct legacy JSON files; SQLite behavior has its own database regressions.
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    // Reject the complete list instead of silently keeping only valid rows.
    {
        const auto valid = kobalt::bookmarkJson(mark("preserved", "2026-09-20T10:00:00Z"));
        QJsonArray excess;
        for (int i = 0; i <= BookmarkStore::maxBookmarks; ++i)
            excess.append(valid);
        for (const auto &rows : { kobalt::qt::array({ valid, QJsonObject{} }),
                                  kobalt::qt::array({ valid, 42 }), excess }) {
            QTemporaryDir damagedRoot;
            BookmarkStore store(damagedRoot.path(), "file:///malformed.epub", fingerprint,
                                "Damaged");
            store.setBookmarks({ mark("preserved", "2026-09-20T10:00:00Z") });
            require(store.save(), "Cannot create malformed bookmark fixture");
            const auto path = store.folder() + "/bookmarks.json";
            const auto original =
                    QJsonDocument(kobalt::qt::object({ { "version", 1 }, { "bookmarks", rows } }))
                            .toJson();
            QFile file(path);
            require(file.open(QIODevice::WriteOnly) && file.write(original) == original.size(),
                    "Cannot write malformed bookmark rows");
            file.close();
            QString error;
            require(!store.load(&error) && error == "invalid" && store.bookmarks().isEmpty(),
                    "Malformed or excess bookmark rows were silently dropped");
            store.setBookmarks({ mark("replacement", "2026-09-20T10:00:00Z") });
            require(!store.save(&error) && error == "load-failed" && read(path) == original,
                    "Editing malformed bookmarks replaced the original bytes");
            BookmarkStore adopted(damagedRoot.path(), "file:///malformed.epub", QByteArray(64, 'b'),
                                  "Edited");
            require(!adopted.load(&error), "An edited book adopted only part of a malformed list");
            adopted.setBookmarks({ mark("replacement", "2026-09-20T10:00:00Z") });
            require(!adopted.save(&error) && error == "load-failed" && read(path) == original,
                    "A failed adoption made a malformed bookmark list writable");
        }
    }
    // Keep legacy UUID keys, hash other server IDs, and fingerprint files.
    require(BookmarkStore::bookKey("3F2A1B4C-0000-4000-8000-00000000ABCD", {})
                    == "kobo-3f2a1b4c-0000-4000-8000-00000000abcd",
            "A purchased book is not keyed by its Kobo ID");
    require(BookmarkStore::bookKey("file:///mnt/onboard/a.epub", fingerprint)
                    == "file-" + QString::fromLatin1(fingerprint),
            "A sideloaded book is not keyed by its fingerprint");
    require(BookmarkStore::bookKey("file:///mnt/onboard/a.epub", "short").isEmpty(),
            "An invalid fingerprint gave a key");
    require(BookmarkStore::bookKey("../../etc", fingerprint).isEmpty(),
            "An unknown identity gave a key");
    const auto numericKey = BookmarkStore::bookKey("12345", {});
    require(numericKey.startsWith("kobo-id-") && numericKey.size() == 72
                    && numericKey == BookmarkStore::bookKey("12345", fingerprint),
            "A numeric server ID did not get a stable bookmark key");
    require(BookmarkStore::bookKey("Server-ID", {}) != BookmarkStore::bookKey("server-id", {}),
            "Distinct server IDs shared their bookmarks");
    for (const auto &identity :
         { QString(), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("folder\\book"),
           QString("book") + QChar(0) + "other" }) {
        require(BookmarkStore::bookKey(identity, fingerprint).isEmpty(),
                "Unsafe server ID got an annotation key");
    }

    QTemporaryDir root;
    require(root.isValid(), "No temporary folder");
    {
        BookmarkStore numeric(root.path(), "12345", fingerprint, "Numeric server book");
        require(numeric.load(), "Bookmarks were unavailable for a numeric server ID");
        numeric.setBookmarks({ mark("/body/DocFragment[1]/body/p[1].0", "2026-09-20T10:00:00Z") });
        require(numeric.save(), "Cannot save a numeric server book's bookmark");
        BookmarkStore reopened(root.path(), "12345", fingerprint, "Numeric server book");
        require(reopened.load() && reopened.bookmarks().size() == 1,
                "Numeric server bookmarks did not survive reopening");
    }
    {
        BookmarkStore store(root.path(), "file:///mnt/onboard/a.epub", fingerprint, "A");
        require(store.load() && store.bookmarks().isEmpty(), "A new book did not start empty");
        store.setBookmarks({ mark("/body/DocFragment[1]/body/p[3].0", "2026-09-20T10:00:00Z"),
                             mark("/body/DocFragment[2]/body/p[1].0", "2026-09-21T10:00:00Z") });
        require(store.save(), "Saving failed");
        require(QFile::exists(store.folder() + "/bookmarks.json")
                        && QFile::exists(store.folder() + "/book.json"),
                "Saving did not write both files");
    }
    {
        BookmarkStore store(root.path(), "file:///mnt/onboard/a.epub", fingerprint, "A");
        require(store.load() && store.bookmarks().size() == 2, "Saved bookmarks did not load");
        require(store.bookmarks()[1].created
                        == QDateTime::fromString("2026-09-21T10:00:00Z", Qt::ISODate),
                "The creation date did not round-trip");
    }
    // An edited sideloaded file has a new fingerprint: its bookmarks follow the path.
    const QByteArray edited = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    {
        BookmarkStore store(root.path(), "file:///mnt/onboard/a.epub", edited, "A");
        require(store.load() && store.bookmarks().size() == 2
                        && store.adoptedFrom() == "file-" + QString::fromLatin1(fingerprint),
                "An edited file lost its bookmarks");
        require(store.save() && store.adoptedFrom().isEmpty()
                        && QFile::exists(store.folder() + "/bookmarks.json"),
                "Adopted bookmarks were not saved under the new fingerprint");
    }
    // A third revision must use the most recently saved bookmarks, not the
    // first fingerprint in alphabetical order.
    {
        BookmarkStore latest(root.path(), "file:///mnt/onboard/a.epub", edited, "A");
        latest.setBookmarks({ mark("latest", "2026-09-22T10:00:00Z") });
        require(latest.save(), "Could not update the second revision");
        QFile oldFile(root.path() + "/file-" + QString::fromLatin1(fingerprint)
                      + "/bookmarks.json");
        QFile newFile(latest.folder() + "/bookmarks.json");
        const struct utimbuf oldTime{ 1000000000, 1000000000 }, newTime{ 1000000010, 1000000010 };
        require(utime(QFile::encodeName(oldFile.fileName()).constData(), &oldTime) == 0
                        && utime(QFile::encodeName(newFile.fileName()).constData(), &newTime) == 0,
                "Could not set revision times");
        BookmarkStore next(root.path(), "file:///mnt/onboard/a.epub", QByteArray(64, 'b'), "A");
        require(next.load() && next.bookmarks().size() == 1
                        && next.bookmarks().first().position == "latest",
                "A third revision adopted stale bookmarks");
    }
    // Reject a save beyond the read limit before replacing the previous list.
    {
        BookmarkStore store(root.path(), "3f2a1b4c-0000-4000-8000-00000000bbbb", {}, "Large");
        store.setBookmarks({ mark("preserved", "2026-09-20T10:00:00Z") });
        require(store.save(), "Could not save the original bookmark");
        QVector<Bookmark> large;
        for (int i = 0; i < BookmarkStore::maxBookmarks; ++i) {
            auto bookmark = mark("p" + QString::number(i), "2026-09-20T10:00:00Z");
            bookmark.chapter = QString(200, QChar(0x4e00));
            bookmark.excerpt = QString(200, QChar(0x4e00));
            large.append(bookmark);
        }
        store.setBookmarks(large);
        QString error;
        require(!store.save(&error) && !error.isEmpty(),
                "An unreadable oversized bookmark file was saved");
        BookmarkStore reopened(root.path(), "3f2a1b4c-0000-4000-8000-00000000bbbb", {}, "Large");
        require(reopened.load() && reopened.bookmarks().size() == 1
                        && reopened.bookmarks().first().position == "preserved",
                "An oversized save replaced readable bookmarks");
    }
    // Another path never adopts them.
    {
        BookmarkStore store(root.path(), "file:///mnt/onboard/b.epub",
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                            "B");
        require(store.load() && store.bookmarks().isEmpty(), "Another book took these bookmarks");
    }
    // A legacy library is indexed once, including a negative lookup. Reopening
    // unrelated unmarked books must not read every book's metadata again.
    {
        QTemporaryDir library;
        require(library.isValid(), "No bookmark library fixture");
        const QString identity = "file:///legacy.epub";
        for (int i = 0; i < 100; ++i) {
            BookmarkStore legacy(library.path(),
                                 i == 42 ? identity : QString("file:///other-%1.epub").arg(i),
                                 QByteArray::number(i, 16).rightJustified(64, '0'), "Legacy");
            legacy.setBookmarks({ mark(QString::number(i), "2026-09-20T10:00:00Z") });
            require(legacy.save(), "Cannot create legacy bookmarks");
        }
        BookmarkStore first(library.path(), "file:///unmarked.epub", QByteArray(64, 'e'),
                            "Unmarked");
        require(first.load() && first.bookmarks().isEmpty() && first.adoptionMetadataReads() == 100,
                "Initial migration did not inspect the legacy library");
        const auto cache = library.path() + "/.index/paths.json";
        require(QFile::exists(cache), "Legacy adoption index was not persisted");
        for (int i = 0; i < 3; ++i) {
            BookmarkStore unmarked(library.path(), QString("file:///unmarked-%1.epub").arg(i),
                                   QByteArray(64, 'e'), "Unmarked");
            require(unmarked.load() && unmarked.bookmarks().isEmpty()
                            && unmarked.adoptionMetadataReads() == 0,
                    "An unmarked opening scanned the indexed library again");
        }
        BookmarkStore changed(library.path(), identity, QByteArray(64, 'f'), "Changed");
        require(changed.load() && changed.adoptionMetadataReads() == 1
                        && changed.bookmarks().size() == 1
                        && changed.bookmarks().first().position == "42",
                "Indexed lookup lost legacy changed-book adoption");
        require(changed.save(), "Cannot save an indexed revision");
        const struct utimbuf indexTime{ 1300000000, 1300000000 };
        require(utime(QFile::encodeName(cache).constData(), &indexTime) == 0,
                "Cannot prepare bookmark-only save timestamp");
        const auto metadataModified = QFileInfo(changed.folder() + "/book.json").lastModified();
        changed.setBookmarks({ mark("bookmark-only edit", "2026-09-20T10:00:00Z") });
        require(changed.save()
                        && QFileInfo(cache).lastModified().toMSecsSinceEpoch() == 1300000000000LL
                        && QFileInfo(changed.folder() + "/book.json").lastModified()
                                == metadataModified,
                "A bookmark-only edit rewrote metadata or the adoption index");
        BookmarkStore again(library.path(), identity, QByteArray(64, 'd'), "Changed again");
        require(again.load() && again.bookmarks().size() == 1 && again.adoptionMetadataReads() == 2,
                "Saving a new revision invalidated the entire library index");
        // Bookmark timestamps can change without a directory change. Select
        // from all path candidates at lookup time, rather than caching a winner.
        const auto oldest = library.path() + "/file-"
                + QString::fromLatin1(QByteArray::number(42, 16).rightJustified(64, '0'));
        const struct utimbuf newestTime{ 2000000000, 2000000000 },
                olderTime{ 1000000000, 1000000000 };
        require(utime(QFile::encodeName(oldest + "/bookmarks.json").constData(), &newestTime) == 0
                        && utime(QFile::encodeName(changed.folder() + "/bookmarks.json")
                                         .constData(),
                                 &olderTime)
                                == 0,
                "Cannot set indexed revision times");
        BookmarkStore retimed(library.path(), identity, QByteArray(64, 'd'), "Retimed");
        require(retimed.load() && retimed.adoptedFrom() == QFileInfo(oldest).fileName()
                        && retimed.adoptionMetadataReads() == 2,
                "Indexed adoption ignored an externally changed bookmark timestamp");
        // Corrupt and oversized derived data must fall back to authoritative
        // folders without disabling edits or accepting an injected path.
        auto unsafeIndex = QJsonDocument::fromJson(read(cache)).object();
        auto unsafePaths = unsafeIndex.value("paths").toObject();
        unsafePaths[unsafePaths.keys().first()] = kobalt::qt::array({ QString("../outside") });
        unsafeIndex["paths"] = unsafePaths;
        for (const auto &bytes : { QByteArray("{broken"), QByteArray(4 * 1024 * 1024 + 1, 'x'),
                                   QJsonDocument(unsafeIndex).toJson() }) {
            QFile file(cache);
            require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                            && file.write(bytes) == bytes.size(),
                    "Cannot damage the derived bookmark index");
            file.close();
            BookmarkStore recover(library.path(), identity, QByteArray(64, 'd'), "Recover");
            require(recover.load() && recover.bookmarks().size() == 1
                            && recover.adoptionMetadataReads() >= 101,
                    "A damaged adoption index did not rebuild from the library");
        }
        // Restore an existing metadata file in place, preserving its length,
        // mtime and root timestamp. Open the new identity first. Only ctime
        // changes, so a size or mtime cache alone cannot discover the restore.
        const struct utimbuf metadataTime{ 1400000000, 1400000000 },
                restoreRootTime{ 1500000000, 1500000000 };
        require(utime(QFile::encodeName(oldest + "/book.json").constData(), &metadataTime) == 0
                        && utime(QFile::encodeName(library.path()).constData(), &restoreRootTime)
                                == 0,
                "Cannot prepare restored metadata timestamps");
        BookmarkStore beforeMetadataRestore(library.path(), "file:///before-metadata.epub",
                                            QByteArray(64, 'd'), "Before metadata restore");
        require(beforeMetadataRestore.load(), "Cannot prepare metadata restore index");
        auto restoredMetadata = read(oldest + "/book.json");
        require(restoredMetadata.contains("file:///legacy.epub"),
                "Missing original metadata identity");
        restoredMetadata.replace("file:///legacy.epub", "file:///edited.epub");
        QFile metadata(oldest + "/book.json");
        require(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && metadata.write(restoredMetadata) == restoredMetadata.size(),
                "Cannot reassign indexed metadata without changing its size");
        metadata.close();
        require(utime(QFile::encodeName(oldest + "/book.json").constData(), &metadataTime) == 0
                        && utime(QFile::encodeName(library.path()).constData(), &restoreRootTime)
                                == 0,
                "Cannot preserve restored metadata timestamps");
        BookmarkStore newIdentity(library.path(), "file:///edited.epub", QByteArray(64, 'd'),
                                  "Reassigned");
        require(newIdentity.load() && newIdentity.bookmarks().size() == 1
                        && newIdentity.adoptionMetadataReads() >= 102,
                "An in-place metadata restore was invisible to its new identity");
        BookmarkStore reassigned(library.path(), identity, QByteArray(64, 'd'), "Reassigned");
        require(reassigned.load()
                        && reassigned.adoptedFrom() == QFileInfo(changed.folder()).fileName()
                        && reassigned.adoptionMetadataReads() == 1,
                "Restored metadata supplied another book's bookmarks");
        // Restores can add folders without changing the directory time on a
        // coarse filesystem. Folder membership must invalidate the inventory.
        const struct utimbuf rootTime{ 1500000000, 1500000000 };
        require(utime(QFile::encodeName(library.path()).constData(), &rootTime) == 0,
                "Cannot set library directory timestamp");
        BookmarkStore beforeRestore(library.path(), "file:///before-restore.epub",
                                    QByteArray(64, 'b'), "Before");
        require(beforeRestore.load(), "Cannot prepare the pre-restore index");
        const auto external = library.path() + "/file-" + QString(64, 'a');
        require(QDir().mkpath(external), "Cannot add external bookmark folder");
        QFile externalBook(external + "/book.json"), externalMarks(external + "/bookmarks.json");
        require(externalBook.open(QIODevice::WriteOnly)
                        && externalBook.write("{\"identity\":\"file:///external.epub\"}") > 0
                        && externalMarks.open(QIODevice::WriteOnly)
                        && externalMarks.write("{\"bookmarks\":[{\"position\":\"external\"}]}") > 0,
                "Cannot populate external bookmark folder");
        externalBook.close();
        externalMarks.close();
        require(utime(QFile::encodeName(library.path()).constData(), &rootTime) == 0,
                "Cannot restore the unchanged library timestamp");
        BookmarkStore externalLookup(library.path(), "file:///external.epub", QByteArray(64, 'b'),
                                     "External");
        require(externalLookup.load() && externalLookup.bookmarks().size() == 1
                        && externalLookup.adoptionMetadataReads() >= 103,
                "An externally added folder stayed absent from the index");
        // Failure to write derived data does not prevent bookmark adoption.
        require(QFile::remove(cache) && QDir().mkdir(cache), "Cannot obstruct the index cache");
        BookmarkStore blocked(library.path(), identity, QByteArray(64, 'd'), "Blocked cache");
        require(blocked.load() && blocked.bookmarks().size() == 1,
                "An unwritable derived index made legacy bookmarks unavailable");
        require(QDir().rmdir(cache), "Cannot clear the obstructed index cache");
        // A library restored without derived data is indexed again.
        BookmarkStore added(library.path(), "file:///added.epub", QByteArray(64, 'c'), "Added");
        added.setBookmarks({ mark("added", "2026-09-20T10:00:00Z") });
        require(added.save(), "Cannot create added bookmark fixture");
        QFile::remove(cache);
        BookmarkStore restored(library.path(), "file:///added.epub", QByteArray(64, 'b'),
                               "Restored");
        require(restored.load() && restored.bookmarks().size() == 1
                        && restored.adoptionMetadataReads() >= 102,
                "Restoring a legacy library lost changed-book adoption");
    }
    // A failed load must not turn the old data into an editable empty list.
    for (const auto &original :
         { QByteArray("{\"bookmarks\": ["), QByteArray("{\"bookmarks\": null}"),
           QByteArray("{\"bookmarks\": \"wrong type\"}"), QByteArray("{\"bookmarks\": {}}"),
           QByteArray(BookmarkStore::maxFileBytes + 1, 'x') }) {
        QTemporaryDir damagedRoot;
        BookmarkStore store(damagedRoot.path(), "3f2a1b4c-0000-4000-8000-00000000abcd", {}, "C");
        require(QDir().mkpath(store.folder()), "Cannot create damaged bookmark folder");
        const auto path = store.folder() + "/bookmarks.json";
        QFile damaged(path);
        require(damaged.open(QIODevice::WriteOnly) && damaged.write(original) == original.size(),
                "Could not write a damaged file");
        damaged.close();
        QString error;
        require(!store.load(&error) && !error.isEmpty() && store.bookmarks().isEmpty(),
                "A damaged file loaded");
        require(error == (original.size() > BookmarkStore::maxFileBytes ? "too-large" : "invalid"),
                "Load error contains identifying data");
        store.setBookmarks({ mark("new", "2026-09-20T10:00:00Z") });
        require(!store.save(&error) && error == "load-failed" && read(path) == original,
                "Editing after a failed load replaced the previous bookmark bytes");
    }
    {
        QTemporaryDir unreadableRoot;
        BookmarkStore store(unreadableRoot.path(), "3f2a1b4c-0000-4000-8000-00000000abcd", {}, "C");
        const auto path = store.folder() + "/bookmarks.json";
        require(QDir().mkpath(path), "Cannot create unreadable bookmark fixture");
        QString error;
        require(!store.load(&error) && error == "unreadable",
                "Unreadable bookmarks did not report a safe error");
        store.setBookmarks({ mark("new", "2026-09-20T10:00:00Z") });
        require(!store.save(&error) && error == "load-failed" && QFileInfo(path).isDir(),
                "An unreadable bookmark file became writable after its failed load");
    }
    // A metadata failure must not commit a new list before reporting failure.
    {
        QTemporaryDir failedRoot;
        BookmarkStore store(failedRoot.path(), "3f2a1b4c-0000-4000-8000-00000000abcd", {}, "C");
        store.setBookmarks({ mark("original", "2026-09-20T10:00:00Z") });
        require(store.save(), "Cannot create saved bookmark fixture");
        const auto path = store.folder() + "/bookmarks.json";
        const auto original = read(path);
        const auto metadata = store.folder() + "/book.json";
        require(QFile::remove(metadata) && QDir().mkdir(metadata),
                "Cannot obstruct bookmark metadata");
        store.setBookmarks({ mark("replacement", "2026-09-20T10:00:00Z") });
        QString error;
        require(!store.save(&error) && error == "write-metadata" && read(path) == original,
                "A failed metadata save replaced the bookmark list or exposed its path");
    }
    // One bookmark per page: several on one page become one at the page start,
    // with the oldest date. Unresolved positions stay as they are.
    {
        BookmarkStore store(root.path(), "file:///mnt/onboard/a.epub", fingerprint, "A");
        store.setBookmarks(
                { mark("p10", "2026-09-22T10:00:00Z"), mark("p11", "2026-09-20T10:00:00Z"),
                  mark("p40", "2026-09-23T10:00:00Z"), mark("gone", "2026-09-19T10:00:00Z") });
        const auto page = [](const QString &position) {
            if (position == "p10" || position == "p11")
                return 5;
            return position == "p40" ? 12 : -1;
        };
        const auto atPage = [](int page, const QDateTime &created) {
            return Bookmark{ "start" + QString::number(page), "Chapter", "Merged", created };
        };
        require(store.merge(page, atPage), "Two bookmarks on one page did not merge");
        const auto &list = store.bookmarks();
        require(list.size() == 3 && list[0].position == "start5"
                        && list[0].created
                                == QDateTime::fromString("2026-09-20T10:00:00Z", Qt::ISODate),
                "The merged bookmark is not at the page start with the oldest date");
        require(list[1].position == "p40" && list[2].position == "gone",
                "A single or unresolved bookmark changed");
        require(!store.merge(page, atPage), "Merging again changed something");
    }
    // A page that opens a chapter starts with its heading; the excerpt leaves it out.
    {
        using kobalt::excerptAfterChapter;
        require(excerptAfterChapter("CHAPTER I. Down the Rabbit-Hole Alice was beginning",
                                    "CHAPTER I. Down the Rabbit-Hole")
                        == "Alice was beginning",
                "The chapter title stayed in the excerpt");
        require(excerptAfterChapter("chapter i.  down the rabbit-hole Alice",
                                    "CHAPTER I. Down the Rabbit-Hole")
                        == "Alice",
                "Case or spacing kept the chapter title in the excerpt");
        require(excerptAfterChapter("Chapter 10 Alice", "Chapter 1") == "Chapter 10 Alice",
                "A title that ends mid-word was removed");
        require(excerptAfterChapter("Alice was beginning", "CHAPTER I.") == "Alice was beginning",
                "An excerpt without the title changed");
        require(excerptAfterChapter("CHAPTER I.", "CHAPTER I.") == "CHAPTER I.",
                "A heading-only excerpt became empty");
        require(excerptAfterChapter("Alice", "") == "Alice", "An empty title changed the excerpt");
    }
    std::puts(
            "PASS: bookmark keys, saving, loading, edited files, damaged files, one bookmark per page and excerpts");
}
