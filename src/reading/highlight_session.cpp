#include "highlight_session.h"
#include "reader_session.h"
#include "support/diagnostics.h"
#include "storage/profile_database.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QPointer>
#include <QHash>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace kobalt {
HighlightSession::HighlightSession(ReaderSession &session, std::function<QString()> layout,
                                   const QString &annotations, const QString &identity,
                                   const QByteArray &fingerprint, const QString &title,
                                   const QString &database)
    : session_(session),
      layout(std::move(layout)),
      store_(std::make_unique<HighlightStore>(annotations, identity, fingerprint, title, database))
{
    if (!store_->available()) {
        store_.reset();
        return;
    }
    loadFailed_ = !store_->loadCompact(&error_);
    if (loadFailed_) {
        diagnostic("highlights-load", { { "error", error_ } }, DiagnosticSeverity::Error);
    }
    saved_ = store_->highlights();
}

HighlightSession::~HighlightSession()
{
    cancelLookup();
}

void HighlightSession::cancelLookup()
{
    if (lookupCancelled_)
        lookupCancelled_->store(true);
    lookupCancelled_.reset();
    ++lookupRequest_;
    lookup_.clear();
}

const QVector<Highlight> &HighlightSession::highlights() const
{
    static const QVector<Highlight> none;
    return store_ ? store_->highlights() : none;
}

bool HighlightSession::excerpts(const QStringList &ids, QVector<HighlightExcerpt> *rows)
{
    // Known exception: annotation lists, the editor and export read saved
    // rows on the main thread when the user opens them.
    mainthread::AllowBlocking allow("annotation read");
    if (!store_ || loadFailed_)
        return false;
    QStringList missing;
    for (const auto &id : ids)
        if (!known_.contains(id))
            missing.append(id);
    if (!missing.isEmpty()) {
        adoptSaved();
        QVector<HighlightExcerpt> loaded;
        if (!store_->excerpts(missing, &loaded, &error_)) {
            ++errors_;
            loadFailed_ = true;
            return false;
        }
        // Keep the newest rows read, so a refresh after an edit is served here.
        QHash<QString, HighlightExcerpt> kept;
        for (const auto &h : store_->highlights())
            if (h.detailsLoaded && known_.contains(h.id))
                kept.insert(h.id, known_.value(h.id));
        known_ = std::move(kept);
        for (const auto &row : loaded)
            known_.insert(row.id, row);
    }
    QVector<HighlightExcerpt> result;
    for (const auto &id : ids) {
        if (!known_.contains(id)) {
            error_ = QStringLiteral("missing-highlight");
            ++errors_;
            return false;
        }
        result.append(known_.value(id));
    }
    *rows = std::move(result);
    return true;
}

bool HighlightSession::detail(const QString &id, Highlight *row)
{
    // Known exception: annotation lists, the editor and export read saved
    // rows on the main thread when the user opens them.
    mainthread::AllowBlocking allow("annotation read");
    if (!store_ || loadFailed_)
        return false;
    for (const auto &h : highlights()) {
        if (h.id == id && h.detailsLoaded) {
            *row = h;
            return true;
        }
    }
    adoptSaved();
    QVector<Highlight> loaded;
    if (!store_->details({ id }, &loaded, &error_) || loaded.size() != 1) {
        ++errors_;
        loadFailed_ = true;
        return false;
    }
    *row = std::move(loaded.first());
    return true;
}

bool HighlightSession::exportDetails(QVector<Highlight> *rows)
{
    // Known exception: annotation lists, the editor and export read saved
    // rows on the main thread when the user opens them.
    mainthread::AllowBlocking allow("annotation read");
    if (!store_ || loadFailed_ || !resolved())
        return false;
    QStringList ids;
    for (const auto &row : highlights())
        if (!row.detailsLoaded)
            ids.append(row.id);
    adoptSaved();
    QVector<Highlight> loaded;
    if (!store_->details(ids, &loaded, &error_)) {
        ++errors_;
        return false;
    }
    QHash<QString, Highlight> stored;
    for (auto &row : loaded)
        stored.insert(row.id, std::move(row));
    rows->clear();
    for (const auto &row : highlights())
        rows->append(row.detailsLoaded ? row : stored.value(row.id));
    QHash<QString, int> order;
    for (int i = 0; i < highlights().size(); ++i) {
        const int rank = order_.value(i * 2, -1);
        order.insert(highlights()[i].id, rank < 0 ? std::numeric_limits<int>::max() : rank);
    }
    std::stable_sort(rows->begin(), rows->end(), [&order](const Highlight &a, const Highlight &b) {
        return order.value(a.id) < order.value(b.id);
    });
    return true;
}

bool HighlightSession::resolved() const
{
    return store_ && !layout_.isEmpty() && layoutRevision_ == session_.layoutRevision()
            && layout_ == layout();
}

BookmarkLocations HighlightSession::locations() const
{
    return resolved() ? BookmarkLocations{ pages_, order_ } : BookmarkLocations{};
}

int HighlightSession::page(int index) const
{
    return resolved() && index >= 0 ? pages_.value(index * 2, -1) : -1;
}

int HighlightSession::endPage(int index) const
{
    return resolved() && index >= 0 ? pages_.value(index * 2 + 1, -1) : -1;
}

bool HighlightSession::sync()
{
    if (!store_ || loadFailed_ || session_.busy() || session_.pages() <= 0) {
        return false;
    }
    const auto key = layout();
    const auto revision = session_.layoutRevision();
    if (layout_ == key && layoutRevision_ == revision) {
        return true;
    }
    if (highlights().isEmpty()) {
        cancelLookup();
        layout_ = key;
        layoutRevision_ = revision;
        pages_.clear();
        order_.clear();
        return true;
    }
    if (lookup_ == key && lookupRevision_ == revision && lookupGeneration_ == rangesGeneration_) {
        return false;
    }
    cancelLookup();
    lookupCancelled_ = std::make_shared<std::atomic_bool>(false);
    lookup_ = key;
    lookupRevision_ = revision;
    lookupGeneration_ = rangesGeneration_;
    const auto request = lookupRequest_;
    QStringList positions;
    for (const auto &h : highlights()) {
        positions << h.start << h.end;
    }
    QPointer<HighlightSession> guard(this);
    session_.bookmarkLocations(
            positions,
            [guard, key, revision, request, count = positions.size(),
             generation = rangesGeneration_](BookmarkLocations locations) {
                if (!guard || guard->lookupRequest_ != request) {
                    return;
                }
                guard->lookup_.clear();
                guard->lookupCancelled_.reset();
                if (guard->rangesGeneration_ == generation
                    && guard->session_.layoutRevision() == revision && guard->layout() == key
                    && locations.pages.size() == count && locations.order.size() == count) {
                    guard->layout_ = key;
                    guard->layoutRevision_ = revision;
                    guard->pages_ = std::move(locations.pages);
                    guard->order_ = std::move(locations.order);
                }
                if (guard->changed) {
                    guard->changed();
                }
            },
            true, true, lookupCancelled_);
    return false;
}

bool HighlightSession::persist(QVector<Highlight> highlights)
{
    if (!writable()) {
        if (failed) {
            failed();
        }
        return false;
    }
    if (highlights.size() > HighlightStore::maxHighlights) {
        error_ = QStringLiteral("storage-limit");
        ++errors_;
        diagnostic("highlights-save", { { "error", error_ } }, DiagnosticSeverity::Error);
        if (failed)
            failed();
        return false;
    }
    if (!writer_) {
        writer_ = std::make_unique<storage::RecordWriter>();
        saver_ = std::make_shared<HighlightStore>(*store_);
    }
    const auto id = ++posted_;
    const auto copy = saver_;
    const QPointer<HighlightSession> guard(this);
    auto error = std::make_shared<QString>();
    writer_->post(
            [copy, highlights, error] {
                storage::RoutineWrites routine;
                copy->setHighlights(highlights);
                return copy->save(error.get());
            },
            [guard, id, highlights, error, before = errors_](bool ok) {
                kobalt::qt::invoke(
                        QCoreApplication::instance(),
                        [guard, id, highlights, error, before, ok] {
                            if (guard)
                                guard->finishSave(id, ok, *error, before, highlights);
                        },
                        Qt::QueuedConnection);
            });
    apply(std::move(highlights));
    return true;
}

void HighlightSession::finishSave(quint64 id, bool ok, const QString &error, quint64 before,
                                  const QVector<Highlight> &highlights)
{
    reported_ = qMax(reported_, id);
    if (ok)
        saved_ = highlights;
    else
        diagnostic("highlights-save", { { "error", error } }, DiagnosticSeverity::Error);
    // A newer save writes the whole list again, so only its result counts.
    if (id != posted_)
        return;
    if (ok) {
        // An error set after this save started, such as an overlap, stays.
        if (errors_ == before)
            error_.clear();
        // The list on screen is the saved one. Edited passages are cached in
        // `known_`; their full payload returns to storage, as after loading.
        store_->dropDetails();
        retryPending_ = false;
        retryHighlights_.clear();
        return;
    }
    retryHighlights_ = highlights;
    retryPending_ = true;
    error_ = error;
    ++errors_;
    known_.clear();
    apply(saved_);
    if (failed)
        failed();
    if (saveFailed)
        saveFailed();
}

void HighlightSession::adoptSaved()
{
    if (!writer_ || adopted_ == posted_)
        return;
    // The writer finishes the newest save before this thread reads its state.
    writer_->flush();
    store_->adoptSaved(*saver_);
    adopted_ = posted_;
}

void HighlightSession::flush()
{
    if (writer_)
        writer_->flush();
}

bool HighlightSession::retryFailedSave()
{
    return !retryPending_ || persist(retryHighlights_);
}

void HighlightSession::remember(const QVector<Highlight> &highlights)
{
    QHash<QString, HighlightExcerpt> kept;
    for (const auto &h : highlights) {
        if (h.detailsLoaded)
            kept.insert(h.id, { h.id, h.text, h.chapter });
        else if (known_.contains(h.id))
            kept.insert(h.id, known_.value(h.id));
    }
    known_ = std::move(kept);
}

// Shows a new list and keeps the endpoint pages that still pair with it.
void HighlightSession::apply(QVector<Highlight> highlights)
{
    const auto previous = store_->highlights();
    store_->setHighlights(std::move(highlights));
    remember(store_->highlights());
    ++generation_;
    const auto &current = store_->highlights();
    bool sameRanges = previous.size() == current.size();
    for (int i = 0; sameRanges && i < current.size(); ++i)
        sameRanges = previous[i].id == current[i].id && previous[i].start == current[i].start
                && previous[i].end == current[i].end;
    if (!sameRanges) {
        ++rangesGeneration_;
        cancelLookup();
        // Removing passages preserves the order of every remaining endpoint.
        // Added or changed endpoints need a new batch to assign relative ranks.
        QHash<QString, int> oldIndices;
        for (int i = 0; i < previous.size(); ++i)
            oldIndices.insert(previous[i].id, i);
        bool retained = resolved();
        QVector<int> pages, order;
        for (const auto &h : current) {
            const int index = oldIndices.value(h.id, -1);
            if (!retained || index < 0 || previous[index].start != h.start
                || previous[index].end != h.end) {
                retained = false;
                break;
            }
            pages << pages_.value(index * 2, -1) << pages_.value(index * 2 + 1, -1);
            order << order_.value(index * 2, -1) << order_.value(index * 2 + 1, -1);
        }
        if (retained) {
            QVector<int> indices;
            for (int i = 0; i < order.size(); ++i)
                if (order[i] >= 0)
                    indices.append(i);
            std::sort(indices.begin(), indices.end(),
                      [&order](int a, int b) { return order[a] < order[b]; });
            for (int rank = 0; rank < indices.size(); ++rank)
                order[indices[rank]] = rank;
            pages_ = std::move(pages);
            order_ = std::move(order);
        } else {
            layout_.clear();
            pages_.clear();
            order_.clear();
        }
    }
    if (changed) {
        changed();
    }
}

void HighlightSession::save(const QString &start, const QString &end, const QString &id,
                            std::function<void(bool)> finished, const QString &chosenEnd,
                            bool sentenceEndPolicy, const QString &chosenStart,
                            bool sentenceStartPolicy, const QString &color, const QString &style)
{
    if (!writable()) {
        if (failed) {
            failed();
        }
        if (finished) {
            finished(false);
        }
        return;
    }
    if ((!color.isEmpty() && !isHighlightColor(color))
        || (!style.isEmpty() && !isHighlightStyle(style))) {
        error_ = QStringLiteral("invalid-appearance");
        ++errors_;
        if (failed) {
            failed();
        }
        if (finished) {
            finished(false);
        }
        return;
    }
    QPointer<HighlightSession> guard(this);
    const auto generation = generation_;
    const auto revision = session_.layoutRevision();
    session_.textRange(
            start, end,
            [guard, id, generation, revision, finished, chosenEnd, sentenceEndPolicy, chosenStart,
             sentenceStartPolicy, color, style](TextRange range) {
                if (!guard) {
                    return;
                }
                if (range.text.isEmpty() || guard->generation_ != generation
                    || guard->session_.layoutRevision() != revision) {
                    if (finished) {
                        finished(false);
                    }
                    return;
                }
                auto list = guard->highlights();
                int index = -1;
                for (int i = 0; i < list.size(); ++i) {
                    if (list[i].id == id) {
                        index = i;
                        break;
                    }
                }
                if (!id.isEmpty() && index < 0) {
                    if (finished) {
                        finished(false);
                    }
                    return;
                }
                // An adjusted passage must preserve a note that was
                // deliberately left out of the rendering metadata.
                if (index >= 0 && !list[index].detailsLoaded && !guard->detail(id, &list[index])) {
                    if (finished)
                        finished(false);
                    return;
                }
                Highlight h{ id.isEmpty() ? QUuid::createUuid().toString().mid(1, 36) : id,
                             range.start,
                             range.end,
                             range.text,
                             {},
                             QDateTime::currentDateTimeUtc() };
                h.color = color.isEmpty() ? (index < 0 ? QStringLiteral("gray") : list[index].color)
                                          : color;
                h.style = style.isEmpty()
                        ? (index < 0 ? QStringLiteral("shaded") : list[index].style)
                        : style;
                h.note = index < 0 ? QString() : list[index].note;
                h.chosenEnd = sentenceEndPolicy ? chosenEnd : QString();
                h.sentenceEndPolicy = sentenceEndPolicy;
                h.chosenStart = sentenceStartPolicy ? chosenStart : QString();
                h.sentenceStartPolicy = sentenceStartPolicy;
                QVector<QPair<QString, QString>> others;
                for (const auto &item : list) {
                    if (item.id != id)
                        others.append({ item.start, item.end });
                }
                guard->session_.intersectingRanges(
                        range.start, range.end, others,
                        [guard, list, h, index, generation, revision,
                         finished](QVector<int> hits) mutable {
                            if (!guard)
                                return;
                            if (guard->generation_ != generation
                                || guard->session_.layoutRevision() != revision
                                || !hits.isEmpty()) {
                                if (!hits.isEmpty()) {
                                    guard->error_ = QStringLiteral("overlap");
                                    ++guard->errors_;
                                    if (guard->failed)
                                        guard->failed();
                                }
                                if (finished)
                                    finished(false);
                                return;
                            }
                            // Chapter lookup follows the start endpoint, even after a cross-page selection.
                            const auto positions = QStringList{ h.start };
                            guard->session_.bookmarkPages(
                                    positions,
                                    [guard, list, h, index, generation, revision,
                                     finished](QVector<int> pages) mutable {
                                        if (!guard) {
                                            return;
                                        }
                                        if (pages.size() != 1 || guard->generation_ != generation
                                            || guard->session_.layoutRevision() != revision) {
                                            if (finished) {
                                                finished(false);
                                            }
                                            return;
                                        }
                                        for (const auto &chapter : guard->session_.chapters()) {
                                            if (chapter.page >= 0 && chapter.page <= pages[0]) {
                                                h.chapter = chapter.title.left(512);
                                            }
                                        }
                                        if (index < 0) {
                                            list.append(h);
                                        } else {
                                            h.created = list[index].created;
                                            list[index] = h;
                                        }
                                        const bool saved = guard->persist(std::move(list));
                                        if (finished) {
                                            finished(saved);
                                        }
                                    });
                        });
            });
}

bool HighlightSession::recolor(const QString &id, const QString &color)
{
    return updateAppearance(id, color, {});
}

bool HighlightSession::restyle(const QString &id, const QString &style)
{
    return updateAppearance(id, {}, style);
}

bool HighlightSession::updateAppearance(const QString &id, const QString &color,
                                        const QString &style)
{
    if ((color.isEmpty() && style.isEmpty()) || (!color.isEmpty() && !isHighlightColor(color))
        || (!style.isEmpty() && !isHighlightStyle(style))) {
        error_ = QStringLiteral("invalid-appearance");
        ++errors_;
        if (failed)
            failed();
        return false;
    }
    auto list = highlights();
    for (auto &h : list) {
        if (h.id != id)
            continue;
        const QString chosenColor = color.isEmpty() ? h.color : color;
        const QString chosenStyle = style.isEmpty() ? h.style : style;
        if (h.color == chosenColor && h.style == chosenStyle && writable()) {
            error_.clear();
            return true;
        }
        h.color = chosenColor;
        h.style = chosenStyle;
        return persist(std::move(list));
    }
    return false;
}

bool HighlightSession::remove(const QString &id)
{
    auto list = highlights();
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].id == id) {
            list.removeAt(i);
            return persist(std::move(list));
        }
    }
    return false;
}
} // namespace kobalt
