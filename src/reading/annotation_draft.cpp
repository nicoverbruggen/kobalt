#include "annotation_draft.h"
#include "engine/crengine/engine_activity.h"
#include "reader_session.h"
#include "storage/highlight_store.h"
#include <QPointer>
#include <memory>
#include <utility>

namespace kobalt {
AnnotationDraft::AnnotationDraft(ReaderSession &session, std::function<bool()> interactive)
    : AnnotationDraft(
              { [&session](Endpoints endpoints, std::function<void(TextRange)> done) {
                   session.annotationRange(endpoints.start.start, endpoints.start.end,
                                           endpoints.end.start, endpoints.end.end, std::move(done),
                                           endpoints.endPolicy, endpoints.startPolicy);
               },
                [&session](QString start, QString end, Ranges ranges,
                           std::function<void(QVector<int>)> done) {
                    session.intersectingRanges(start, end, std::move(ranges), std::move(done));
                },
                [&session](QString start, QString end,
                           std::function<void(AnnotationGeometry)> done) {
                    session.annotationGeometry(start, end, std::move(done));
                } },
              std::move(interactive))
{
}

AnnotationDraft::AnnotationDraft(Queries queries, std::function<bool()> interactive)
    : queries_(std::move(queries)), interactive_(std::move(interactive))
{
}

QString AnnotationDraft::end() const
{
    return range_.end.isEmpty() ? endpoints_.start.end : range_.end;
}

void AnnotationDraft::invalidate()
{
    ++generation_;
    ++query_;
    ++geometryQuery_;
}

void AnnotationDraft::clear()
{
    invalidate();
    ++saveRevision_;
    endpoints_ = {};
    range_ = {};
    editing_.clear();
    geometry_ = {};
    dirty_ = false;
}

void AnnotationDraft::edit(const Highlight &highlight)
{
    clear();
    editing_ = highlight.id;
    range_.start = highlight.start;
    range_.end = highlight.end;
    endpoints_.start.start =
            highlight.sentenceStartPolicy ? highlight.chosenStart : highlight.start;
    endpoints_.start.end = endpoints_.start.start;
    endpoints_.end.start = highlight.sentenceEndPolicy ? highlight.chosenEnd : highlight.end;
    endpoints_.end.end = endpoints_.end.start;
    endpoints_.startPolicy = range_.sentenceStartPolicy = highlight.sentenceStartPolicy;
    endpoints_.endPolicy = range_.sentenceEndPolicy = highlight.sentenceEndPolicy;
    range_.chosenStart = endpoints_.start.start;
    range_.chosenEnd = endpoints_.end.start;
}

bool AnnotationDraft::setViewport(const QString &viewport)
{
    if (viewport_ == viewport)
        return false;
    invalidate();
    viewport_ = viewport;
    return true;
}

AnnotationDraft::Endpoints AnnotationDraft::moved(const AnnotationToken &token, int boundary) const
{
    auto result = endpoints_;
    if (boundary < 0) {
        result = { token, token, !token.punctuation, !token.punctuation };
    } else if (boundary == 0) {
        if (range_.end.isEmpty()) {
            result.end = result.start;
            result.endPolicy = result.startPolicy;
        }
        result.start = token;
        result.startPolicy = !token.punctuation;
    } else {
        result.end = token;
        result.endPolicy = !token.punctuation;
    }
    return result;
}

bool AnnotationDraft::current(quint64 generation, quint64 query) const
{
    return generation_ == generation && query_ == query && interactive_();
}

bool AnnotationDraft::apply(Endpoints endpoints, TextRange range)
{
    const bool reversed = range.chosenStart != endpoints.start.start;
    if (reversed)
        std::swap(endpoints.start, endpoints.end);
    endpoints.startPolicy = range.sentenceStartPolicy;
    endpoints.endPolicy = range.sentenceEndPolicy;
    endpoints_ = std::move(endpoints);
    range_ = std::move(range);
    return reversed;
}

void AnnotationDraft::resolve(Endpoints endpoints, Ranges excluded,
                              std::function<void(bool)> accepted, std::function<void()> ready,
                              std::function<void()> rejected)
{
    const auto generation = generation_, query = ++query_;
    ++geometryQuery_;
    QPointer<AnnotationDraft> guard(this);
    queries_.range(endpoints,
                   [guard, generation, query, endpoints, excluded, accepted, ready,
                    rejected](TextRange range) {
                       if (!guard || !guard->current(generation, query))
                           return;
                       if (range.text.isEmpty()) {
                           if (rejected)
                               rejected();
                           return;
                       }
                       guard->queries_.overlaps(
                               range.start, range.end, excluded,
                               [guard, generation, query, endpoints, range, accepted, ready,
                                rejected](QVector<int> hits) {
                                   if (!guard || !guard->current(generation, query))
                                       return;
                                   if (!hits.isEmpty()) {
                                       if (rejected)
                                           rejected();
                                       return;
                                   }
                                   const bool reversed = guard->apply(endpoints, range);
                                   guard->dirty_ = true;
                                   if (accepted)
                                       accepted(reversed);
                                   if (guard && guard->current(generation, query))
                                       guard->refreshGeometry(ready);
                               });
                   });
}

void AnnotationDraft::refreshGeometry(std::function<void()> ready)
{
    const auto generation = generation_, query = ++geometryQuery_;
    if (range_.start.isEmpty()) {
        geometry_.rectangles.clear();
        return;
    }
    QPointer<AnnotationDraft> guard(this);
    queries_.geometry(range_.start, end(),
                      [guard, generation, query, ready](AnnotationGeometry geometry) {
                          if (!guard || guard->generation_ != generation
                              || guard->geometryQuery_ != query || !guard->interactive_())
                              return;
                          guard->geometry_ = std::move(geometry);
                          if (ready)
                              ready();
                      });
}

void AnnotationDraft::acceptFrame(const QString &start, const QString &end,
                                  AnnotationGeometry geometry, bool unchangedViewport)
{
    const bool matches = start == range_.start && end == this->end();
    // Image decoding can deliver older markup for the same text viewport.
    if (matches || !unchangedViewport)
        geometry_ = matches ? std::move(geometry) : AnnotationGeometry{};
}

void AnnotationDraft::prepareSave(std::function<void(Prepared, quint64)> ready)
{
    invalidate();
    const auto revision = ++saveRevision_;
    const auto endpoints = endpoints_;
    const auto drain = std::make_shared<EngineDrain>();
    QPointer<AnnotationDraft> guard(this);
    queries_.range(endpoints, [guard, revision, endpoints, ready, drain](TextRange range) {
        if (!guard)
            return;
        if (guard->saveRevision_ != revision) {
            ready(Prepared::Stale, revision);
            return;
        }
        if (range.text.isEmpty()) {
            ready(Prepared::Rejected, revision);
            return;
        }
        guard->apply(endpoints, std::move(range));
        ready(Prepared::Ready, revision);
    });
}

bool AnnotationDraft::finishSave(quint64 revision, bool saved)
{
    if (saveRevision_ != revision)
        return false;
    if (saved)
        clear();
    return true;
}
} // namespace kobalt
