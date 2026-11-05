#pragma once
#include "storage/bookmark_store.h"
#include "storage/record_writer.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QVector>
#include <functional>
#include <memory>

namespace kobalt {
class ReaderSession;

QString bookmarkLayoutKey(int pages, const QJsonObject &state, const QSize &pageArea);

// Kobalt's own bookmarks for one open book, one per page. The engine thread
// looks up the page of each stored bookmark whenever the layout changes.
// Bookmarks that end up on one page are merged, and every change is saved.
//
// The window names the current layout, because the page area size is part of
// it. The owner destroys this before the ReaderSession it borrows. Replies
// from the engine are dropped after destruction or a layout change. A page
// lookup is also repeated when the list changed meanwhile, because its pages
// pair with the old list.
//
// An edit changes the list at once. Its save runs on a writer thread, because
// each save syncs storage several times. When the newest save fails, the list
// returns to the last saved one and `failed` runs. Destruction waits for a
// running save.
class BookmarkSession final : public QObject
{
public:
    BookmarkSession(ReaderSession &session, std::function<QString()> layout,
                    const QString &annotations, const QString &identity,
                    const QByteArray &fingerprint, const QString &title,
                    const QString &database = {});

    // False when the book has no stable key. The session then does nothing.
    bool available() const { return bool(store); }

    bool writable() const { return store && !loadFailed_; }

    QString error() const { return error_; }

    const QVector<Bookmark> &bookmarks() const;
    // Starts a page lookup when the layout is new. True when the pages of
    // the current layout are known.
    bool sync();
    bool resolved() const;
    bool marked(int page) const;
    // The page of the bookmark at `index` in the current layout, or -1.
    int page(int index) const;
    // The page of the bookmark at `position` in the current layout, or -1.
    int pageOf(const QString &position) const;
    enum class Toggle { Refused, Removed, Adding };
    // Removes the bookmark on `page`, or starts adding one there.
    Toggle toggle(int page);
    bool remove(const QString &position);
    bool setPin(const QString &position, const QString &name, const QString &icon);

    const QVector<int> &pages() const { return pages_; }

    // True until the newest save has reported back.
    bool saving() const { return reported < posted; }

    // Waits for the running and pending save, for example before USB export.
    void flush();

    bool hasFailedSave() const { return retryPending; }

    bool retryFailedSave();

    // One validity check for callers rendering an entire bookmark list.
    QVector<int> resolvedPages() const { return resolved() ? pages_ : QVector<int>{}; }

    // The known pages changed: a lookup finished, needs repeating, or merged
    // bookmarks. The window refreshes.
    std::function<void()> changed;
    // A bookmark from toggle() was added.
    std::function<void()> added;
    // Loading or saving failed. The list and its saved bytes are preserved.
    std::function<void()> failed;

private:
    void merge();
    bool save(QVector<Bookmark> bookmarks, QVector<int> pages, const char *reason);
    // `before` is the error count when the save was posted.
    void finishSave(quint64 save, bool ok, const QString &error, quint64 before,
                    const QVector<Bookmark> &bookmarks, const char *reason);
    ReaderSession &session;
    std::function<QString()> layout;
    std::unique_ptr<BookmarkStore> store;
    QString error_;
    bool loadFailed_ = false;
    // pages_[i] is the page of store->bookmarks()[i] in layout_. While
    // layout_ is set, pages_ has one entry per bookmark.
    QVector<int> pages_;
    QString layout_, lookup_;
    quint64 layoutRevision_ = 0, lookupRevision_ = 0, lookupRequest_ = 0, lookupGeneration_ = 0;
    // Changes with every edit of the list, so an older page lookup is repeated.
    quint64 generation = 0;
    // Pages with an add waiting for the engine. A second tap there waits too.
    QSet<int> adding;
    // The writer thread owns `saver`, a copy of the store that tracks what
    // is on storage. `saved` is the newest list known to be there. Pages of
    // lists replaced since then, in `restore`'s layout, let a failed save
    // return to `saved` without a new lookup.
    std::unique_ptr<storage::RecordWriter> writer;
    std::shared_ptr<BookmarkStore> saver;
    QVector<Bookmark> saved;
    QVector<Bookmark> retryBookmarks;
    bool retryPending = false;
    QHash<QString, int> restore;
    QString restoreLayout;
    quint64 restoreRevision = 0, posted = 0, reported = 0;
    // Counts errors, so a save clears only errors set before it was posted.
    quint64 errors = 0;
};
} // namespace kobalt
