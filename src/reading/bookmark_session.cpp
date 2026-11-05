#include "bookmark_session.h"
#include "reader_session.h"
#include "appearance_settings.h"
#include "support/diagnostics.h"
#include "storage/profile_database.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QMap>
#include <QPointer>

namespace kobalt {
QString bookmarkLayoutKey(int pages, const QJsonObject &state, const QSize &pageArea)
{
    QJsonObject layout;
    appearance::copy(layout, state);
    layout["pages"] = pages;
    layout["page_width"] = pageArea.width();
    layout["page_height"] = pageArea.height();
    return QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact));
}

BookmarkSession::BookmarkSession(ReaderSession &session, std::function<QString()> layout,
                                 const QString &annotations, const QString &identity,
                                 const QByteArray &fingerprint, const QString &title,
                                 const QString &database)
    : session(session),
      layout(std::move(layout)),
      store(std::make_unique<BookmarkStore>(annotations, identity, fingerprint, title, database))
{
    if (!store->available()) {
        store.reset();
        return;
    }
    QString error;
    if (!store->load(&error)) {
        loadFailed_ = true;
        error_ = strings::get("bookmarks.load_failed");
        diagnostic("bookmarks-load", { { "error", error } }, DiagnosticSeverity::Error);
    }
    saved = store->bookmarks();
}

const QVector<Bookmark> &BookmarkSession::bookmarks() const
{
    static const QVector<Bookmark> none;
    return store ? store->bookmarks() : none;
}

bool BookmarkSession::resolved() const
{
    return store && !layout_.isEmpty() && layoutRevision_ == session.layoutRevision()
            && layout_ == layout();
}

bool BookmarkSession::marked(int page) const
{
    return resolved() && pages_.contains(page);
}

int BookmarkSession::page(int index) const
{
    return resolved() ? pages_.value(index, -1) : -1;
}

int BookmarkSession::pageOf(const QString &position) const
{
    const auto &list = bookmarks();
    for (int i = 0; i < list.size(); ++i)
        if (list[i].position == position)
            return page(i);
    return -1;
}

bool BookmarkSession::sync()
{
    if (!store || loadFailed_ || session.busy() || session.pages() <= 0)
        return false;
    const auto key = layout();
    const auto revision = session.layoutRevision();
    if (key == layout_ && layoutRevision_ == revision)
        return true;
    // There are no source positions to resolve. Queueing a lookup here leaves
    // the first tap after opening or reflow temporarily unable to add a mark.
    if (store->bookmarks().isEmpty()) {
        ++lookupRequest_;
        lookup_.clear();
        layout_ = key;
        layoutRevision_ = revision;
        pages_.clear();
        return true;
    }
    if (lookup_ == key && lookupRevision_ == revision && lookupGeneration_ == generation)
        return false;
    lookup_ = key;
    lookupRevision_ = revision;
    lookupGeneration_ = generation;
    const auto request = ++lookupRequest_;
    QStringList positions;
    for (const auto &bookmark : store->bookmarks())
        positions << bookmark.position;
    QPointer<BookmarkSession> guard(this);
    session.bookmarkPages(positions,
                          [guard, key, revision, request, positions,
                           generation = generation](QVector<int> pages) {
                              if (!guard || guard->lookupRequest_ != request)
                                  return;
                              guard->lookup_.clear();
                              // An empty answer for a non-empty list means the layout changed
                              // meanwhile. A changed list or window layout needs a new lookup too.
                              if (pages.size() != positions.size()
                                  || guard->generation != generation || guard->layout() != key
                                  || guard->session.layoutRevision() != revision) {
                                  if (guard->changed)
                                      guard->changed();
                                  return;
                              }
                              guard->layout_ = key;
                              guard->layoutRevision_ = revision;
                              guard->pages_ = pages;
                              guard->merge();
                              if (guard && guard->changed)
                                  guard->changed();
                          });
    return false;
}

// Several bookmarks on one page, for example after a font change, become one
// at the page start with the oldest date.
void BookmarkSession::merge()
{
    QMap<int, int> counts;
    for (int p : pages_)
        if (p >= 0)
            ++counts[p];
    QList<int> shared;
    for (auto it = counts.begin(); it != counts.end(); ++it)
        if (it.value() > 1)
            shared.append(it.key());
    if (shared.isEmpty())
        return;
    auto summaries = std::make_shared<QMap<int, QPair<QString, QString>>>();
    auto remaining = std::make_shared<int>(shared.size());
    const auto key = layout_;
    const auto revision = layoutRevision_;
    QPointer<BookmarkSession> guard(this);
    for (int p : shared) {
        session.pageSummary(
                p, [guard, p, key, revision, summaries, remaining](QString start, QString excerpt) {
                    if (!guard)
                        return;
                    (*summaries)[p] = { start, excerpt };
                    // A layout change drops the merge; the next lookup merges again.
                    // A list edit does not: the merge below reads the current list and
                    // its pages, and the summaries still describe this layout.
                    if (--*remaining > 0 || guard->layout_ != key || guard->layout() != key
                        || guard->session.layoutRevision() != revision)
                        return;
                    // An unavailable source cannot supply a replacement bookmark.
                    for (auto it = summaries->begin(); it != summaries->end(); ++it) {
                        if (!it.value().first.isEmpty())
                            continue;
                        guard->layout_.clear();
                        if (guard->changed)
                            guard->changed();
                        return;
                    }
                    QMap<QString, int> pageOf;
                    const auto &list = guard->store->bookmarks();
                    for (int i = 0; i < list.size() && i < guard->pages_.size(); ++i)
                        pageOf[list[i].position] = guard->pages_[i];
                    // The list can have changed since the merge started. Every group in
                    // it must have a summary; otherwise look the pages up again.
                    QMap<int, int> current;
                    for (int page : guard->pages_)
                        if (page >= 0)
                            ++current[page];
                    for (auto it = current.begin(); it != current.end(); ++it) {
                        if (it.value() > 1 && !summaries->contains(it.key())) {
                            guard->layout_.clear();
                            if (guard->changed)
                                guard->changed();
                            return;
                        }
                    }
                    auto &session = guard->session;
                    const auto previous = guard->store->bookmarks();
                    const bool merged = guard->store->merge(
                            [&pageOf](const QString &position) {
                                return pageOf.value(position, -1);
                            },
                            [&session, summaries](int page, const QDateTime &created) {
                                QString chapter;
                                for (const auto &link : session.chapters())
                                    if (link.page >= 0 && link.page <= page)
                                        chapter = link.title;
                                const auto summary = summaries->value(page);
                                return Bookmark{ summary.first, chapter, summary.second, created };
                            });
                    if (!merged)
                        return;
                    const auto mergedBookmarks = guard->store->bookmarks();
                    guard->store->setBookmarks(previous);
                    // Each replacement starts on a known page. Keep that pairing rather
                    // than resolving the shorter list again after every merge.
                    for (auto it = summaries->begin(); it != summaries->end(); ++it)
                        pageOf.insert(it.value().first, it.key());
                    QVector<int> mergedPages;
                    for (const auto &bookmark : mergedBookmarks)
                        mergedPages.append(pageOf.value(bookmark.position, -1));
                    if (!guard->save(mergedBookmarks, mergedPages, "merged"))
                        return;
                    if (guard->changed)
                        guard->changed();
                });
    }
}

bool BookmarkSession::save(QVector<Bookmark> bookmarks, QVector<int> pages, const char *reason)
{
    if (bookmarks.size() > BookmarkStore::maxBookmarks) {
        diagnostic("bookmarks-saved",
                   { { "reason", reason },
                     { "count", int(store->bookmarks().size()) },
                     { "failed", true },
                     { "error", "bookmark-limit" } },
                   DiagnosticSeverity::Warning);
        error_ = strings::get("bookmarks.save_failed");
        ++errors;
        if (failed)
            failed();
        return false;
    }
    // Remember the pages of the list being replaced, so a failed save can
    // return to the saved list in this layout.
    if (restoreLayout != layout_ || restoreRevision != layoutRevision_) {
        restore.clear();
        restoreLayout = layout_;
        restoreRevision = layoutRevision_;
    }
    const auto &previous = store->bookmarks();
    if (!layout_.isEmpty() && pages_.size() == previous.size())
        for (int i = 0; i < previous.size(); ++i)
            restore.insert(previous[i].position, pages_[i]);
    store->setBookmarks(bookmarks);
    ++generation;
    pages_ = std::move(pages);
    if (!writer) {
        writer = std::make_unique<storage::RecordWriter>();
        saver = std::make_shared<BookmarkStore>(*store);
    }
    const auto id = ++posted;
    const auto copy = saver;
    const QPointer<BookmarkSession> guard(this);
    auto error = std::make_shared<QString>();
    writer->post(
            [copy, bookmarks, error] {
                storage::RoutineWrites routine;
                copy->setBookmarks(bookmarks);
                return copy->save(error.get());
            },
            [guard, id, bookmarks, error, reason, before = errors](bool ok) {
                kobalt::qt::invoke(
                        QCoreApplication::instance(),
                        [guard, id, bookmarks, error, before, reason, ok] {
                            if (guard)
                                guard->finishSave(id, ok, *error, before, bookmarks, reason);
                        },
                        Qt::QueuedConnection);
            });
    return true;
}

void BookmarkSession::finishSave(quint64 id, bool ok, const QString &error, quint64 before,
                                 const QVector<Bookmark> &bookmarks, const char *reason)
{
    reported = qMax(reported, id);
    diagnostic("bookmarks-saved",
               { { "reason", reason },
                 { "count", int(bookmarks.size()) },
                 { "failed", !ok },
                 { "error", error } },
               ok ? DiagnosticSeverity::Activity : DiagnosticSeverity::Error);
    if (ok)
        saved = bookmarks;
    // A newer save writes the whole list again, so only its result counts.
    if (id != posted)
        return;
    if (ok) {
        // An error set after this save started stays.
        if (errors == before)
            error_.clear();
        restore.clear();
        retryPending = false;
        retryBookmarks.clear();
        return;
    }
    retryBookmarks = bookmarks;
    retryPending = true;
    // Return to the saved list. Its pages are known when every saved
    // bookmark was on screen in this layout; otherwise look them up again.
    QVector<int> pages;
    if (!layout_.isEmpty() && restoreLayout == layout_ && restoreRevision == layoutRevision_) {
        QHash<QString, int> known = restore;
        const auto &current = store->bookmarks();
        for (int i = 0; i < current.size() && i < pages_.size(); ++i)
            known.insert(current[i].position, pages_[i]);
        for (const auto &bookmark : saved) {
            if (!known.contains(bookmark.position)) {
                pages.clear();
                break;
            }
            pages.append(known.value(bookmark.position));
        }
    }
    store->setBookmarks(saved);
    ++generation;
    if (pages.size() == saved.size()) {
        pages_ = std::move(pages);
    } else {
        layout_.clear();
        pages_.clear();
    }
    restore.clear();
    error_ = strings::get("bookmarks.save_failed");
    ++errors;
    if (changed)
        changed();
    if (failed)
        failed();
}

void BookmarkSession::flush()
{
    if (writer)
        writer->flush();
}

bool BookmarkSession::retryFailedSave()
{
    if (!retryPending)
        return true;
    // Resolve the retried list against the current layout after persistence.
    layout_.clear();
    return save(retryBookmarks, {}, "retry");
}

bool BookmarkSession::remove(const QString &position)
{
    if (!store)
        return false;
    if (loadFailed_) {
        if (failed)
            failed();
        return false;
    }
    QVector<Bookmark> kept;
    QVector<int> keptPages;
    const auto &current = store->bookmarks();
    for (int i = 0; i < current.size(); ++i)
        if (current[i].position != position) {
            kept.append(current[i]);
            keptPages.append(pages_.value(i, -1));
        }
    if (kept.size() == current.size())
        return false;
    return save(kept, keptPages, "removed");
}

bool BookmarkSession::setPin(const QString &position, const QString &name, const QString &icon)
{
    if (!writable() || name.size() > 128 || icon.size() > 64)
        return false;
    auto list = store->bookmarks();
    int count = 0;
    for (const auto &item : list)
        count += item.pinned();
    for (auto &item : list) {
        if (item.position != position)
            continue;
        if (!icon.isEmpty() && !item.pinned() && count >= BookmarkStore::maxPins)
            return false;
        item.name = icon.isEmpty() ? QString() : name.trimmed();
        item.icon = icon;
        if (!save(list, pages_, "pin"))
            return false;
        if (changed)
            changed();
        return true;
    }
    return false;
}

BookmarkSession::Toggle BookmarkSession::toggle(int current)
{
    if (loadFailed_) {
        if (failed)
            failed();
        return Toggle::Refused;
    }
    if (!store || session.busy() || !resolved()) {
        diagnostic("bookmark-toggle-refused",
                   { { "busy", session.busy() },
                     { "layout_known", resolved() },
                     { "lookup_running", !lookup_.isEmpty() } },
                   false);
        return Toggle::Refused;
    }
    if (pages_.contains(current)) {
        QVector<Bookmark> kept;
        QVector<int> keptPages;
        const auto &list = store->bookmarks();
        for (int i = 0; i < list.size(); ++i)
            if (pages_.value(i, -1) != current) {
                kept.append(list[i]);
                keptPages.append(pages_.value(i, -1));
            }
        return save(kept, keptPages, "removed") ? Toggle::Removed : Toggle::Refused;
    }
    if (adding.contains(current))
        return Toggle::Adding;
    adding.insert(current);
    const auto key = layout();
    QPointer<BookmarkSession> guard(this);
    // The reply appends to the current list, so an edit of the list meanwhile
    // does not matter. A layout change or page turn does.
    session.pageSummary(current, [guard, current, key](QString start, QString excerpt) {
        if (!guard)
            return;
        guard->adding.remove(current);
        if (guard->pages_.contains(current))
            return;
        if (start.isEmpty() || guard->layout() != key || guard->session.page() != current) {
            diagnostic("bookmark-toggle-refused",
                       { { "start_empty", start.isEmpty() },
                         { "layout_changed", guard->layout() != key },
                         { "page_changed", guard->session.page() != current } },
                       false);
            return;
        }
        auto list = guard->store->bookmarks();
        list.append(
                { start, guard->session.chapterTitle(), excerpt, QDateTime::currentDateTimeUtc() });
        // Known pages always pair with the list. When a merge or new lookup
        // has made them unknown meanwhile, the next lookup finds this page too.
        const bool known = guard->resolved();
        auto pages = guard->pages_;
        if (known)
            pages.append(current);
        else
            pages.clear();
        if (!guard->save(list, pages, "added"))
            return;
        if (guard->added)
            guard->added();
        if (!known && guard && guard->changed)
            guard->changed();
    });
    return Toggle::Adding;
}
} // namespace kobalt
