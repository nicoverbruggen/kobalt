#include "reading/annotation_draft.h"
#include "storage/highlight_store.h"
#include "check.h"
#include <QCoreApplication>
#include <memory>

using namespace kobalt;

namespace {
struct Queries
{
    QVector<std::function<void(TextRange)>> ranges;
    QVector<std::function<void(QVector<int>)>> overlaps;
    QVector<std::function<void(AnnotationGeometry)>> geometries;
    QVector<AnnotationDraft::Endpoints> endpoints;
    AnnotationDraft::Ranges excluded;
    bool interactive = true;

    AnnotationDraft::Queries callbacks()
    {
        return { [this](AnnotationDraft::Endpoints value, std::function<void(TextRange)> done) {
                    endpoints.append(value);
                    ranges.append(std::move(done));
                },
                 [this](QString, QString, AnnotationDraft::Ranges value,
                        std::function<void(QVector<int>)> done) {
                     excluded = value;
                     overlaps.append(std::move(done));
                 },
                 [this](QString, QString, std::function<void(AnnotationGeometry)> done) {
                     geometries.append(std::move(done));
                 } };
    }
};

AnnotationToken token(const QString &position, bool punctuation = false)
{
    AnnotationToken result;
    result.start = result.end = result.text = position;
    result.punctuation = punctuation;
    return result;
}

TextRange range(const QString &start = "a", const QString &end = "b")
{
    TextRange result;
    result.start = start;
    result.end = end;
    result.text = "selected text";
    result.chosenStart = start;
    result.chosenEnd = end;
    result.sentenceStartPolicy = true;
    return result;
}

AnnotationGeometry geometry(int x)
{
    AnnotationGeometry result;
    result.rectangles = { QRect(x, 2, 30, 10) };
    result.vertical = true;
    return result;
}

Highlight highlight()
{
    Highlight result;
    result.id = "saved";
    result.start = "a";
    result.end = "b";
    result.chosenStart = "chosen-a";
    result.chosenEnd = "chosen-b";
    result.sentenceStartPolicy = true;
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    {
        Queries queries;
        AnnotationDraft draft(queries.callbacks(), [&] { return queries.interactive; });
        draft.edit(highlight());
        require(draft.editingId() == "saved" && !draft.dirty()
                        && draft.endpoints().start.start == "chosen-a"
                        && draft.endpoints().end.start == "b",
                "Editing must retain provenance and exact legacy endpoints");
        const auto candidate = draft.moved(token("z", true), 1);
        require(!candidate.endPolicy && draft.endpoints().end.start == "b",
                "Moving a handle must not publish unvalidated endpoints");
        int accepted = 0, ready = 0, rejected = 0;
        bool reversed = false;
        const auto resolve = [&] {
            draft.resolve(
                    candidate, { { "other-start", "other-end" } },
                    [&](bool value) {
                        ++accepted;
                        reversed = value;
                    },
                    [&] { ++ready; }, [&] { ++rejected; });
        };
        resolve();
        queries.ranges[0](range("z", "chosen-a"));
        require(queries.excluded.size() == 1 && draft.range().end == "b",
                "Range resolution must wait for overlap validation");
        queries.overlaps[0]({ 0 });
        require(rejected == 1 && accepted == 0 && !draft.dirty()
                        && draft.endpoints().end.start == "b" && queries.geometries.isEmpty(),
                "An overlapping candidate must leave the draft unchanged");
        resolve();
        queries.ranges[1](range("z", "chosen-a"));
        queries.overlaps[1]({});
        require(accepted == 1 && reversed && draft.dirty() && draft.endpoints().start.start == "z"
                        && draft.endpoints().startPolicy && !draft.endpoints().endPolicy,
                "Accepted reversed endpoints must use the resolved punctuation policies");
        queries.geometries[0](geometry(10));
        require(ready == 1 && draft.geometry().rectangles == geometry(10).rectangles,
                "Accepted geometry must reach the owner after the range");
        draft.acceptFrame("older", "range", geometry(20), true);
        require(draft.geometry().rectangles == geometry(10).rectangles,
                "An image decode with older markup must keep the current selection");
        draft.acceptFrame("older", "range", geometry(20), false);
        require(draft.geometry().rectangles.isEmpty(),
                "A different viewport must discard unrelated geometry");
        draft.acceptFrame(draft.range().start, draft.end(), geometry(30), false);
        draft.refreshGeometry([&] { ++ready; });
        draft.hideRectangles();
        require(draft.geometry().vertical && draft.geometry().rectangles.isEmpty(),
                "Fallback paint must hide rectangles without losing the writing axis");
        queries.geometries[1](geometry(40));
        require(ready == 2 && draft.geometry().rectangles == geometry(40).rectangles,
                "Fallback paint must not cancel its pending geometry query");
    }
    {
        Queries queries;
        AnnotationDraft draft(queries.callbacks(), [&] { return queries.interactive; });
        int accepted = 0, ready = 0, rejected = 0;
        const auto resolve = [&](QString position) {
            draft.resolve(
                    draft.moved(token(position), -1), {}, [&](bool) { ++accepted; },
                    [&] { ++ready; }, [&] { ++rejected; });
        };
        resolve("a");
        resolve("c");
        queries.ranges[0](range());
        require(queries.overlaps.isEmpty(), "A superseded range must not start an overlap query");
        queries.ranges[1](range("c", "d"));
        draft.setViewport("new-page");
        queries.overlaps[0]({});
        require(accepted == 0 && draft.range().start.isEmpty(),
                "A previous viewport's overlap response must not publish a draft");
        resolve("e");
        queries.ranges[2](range("e", "f"));
        queries.overlaps[1]({});
        draft.refreshGeometry([&] { ++ready; });
        queries.geometries[0](geometry(1));
        require(ready == 0, "An older geometry response must not replace a newer request");
        queries.interactive = false;
        queries.geometries[1](geometry(2));
        require(ready == 0 && rejected == 0 && draft.geometry().rectangles.isEmpty(),
                "Hidden selection queries must not publish geometry or UI callbacks");
    }
    {
        Queries queries;
        AnnotationDraft draft(queries.callbacks(), [&] { return queries.interactive; });
        draft.edit(highlight());
        int completed = 0;
        quint64 revision = 0;
        AnnotationDraft::Prepared result = AnnotationDraft::Prepared::Rejected;
        const auto prepared = [&](AnnotationDraft::Prepared value, quint64 ticket) {
            ++completed;
            result = value;
            revision = ticket;
        };
        draft.prepareSave(prepared);
        queries.interactive = false;
        draft.setViewport("sleep");
        queries.ranges[0](range());
        require(completed == 1 && result == AnnotationDraft::Prepared::Ready,
                "Viewport invalidation and sleep must not abandon save preparation");
        require(draft.finishSave(revision, false) && draft.range().start == "a",
                "A failed persistence write must retain the draft");
        draft.prepareSave(prepared);
        draft.clear();
        draft.edit(highlight());
        queries.ranges[1](range("old", "range"));
        require(completed == 2 && result == AnnotationDraft::Prepared::Stale
                        && draft.range().start == "a" && !draft.finishSave(revision, true),
                "An abandoned save must report completion without replacing a new draft");
        draft.prepareSave(prepared);
        queries.ranges[2]({});
        require(result == AnnotationDraft::Prepared::Rejected && draft.range().start == "a",
                "An empty save range must preserve the editable draft");
        draft.prepareSave(prepared);
        queries.ranges[3](range());
        require(draft.finishSave(revision, true) && draft.range().start.isEmpty()
                        && draft.editingId().isEmpty() && !draft.dirty(),
                "A verified save completion must clear the saved draft");
    }
    {
        Queries queries;
        auto draft = std::make_unique<AnnotationDraft>(queries.callbacks(), [] { return true; });
        int called = 0;
        draft->resolve(
                draft->moved(token("a"), -1), {}, [&](bool) { draft.reset(); }, [&] { ++called; },
                [&] { ++called; });
        queries.ranges[0](range());
        queries.overlaps[0]({});
        require(!draft && called == 0 && queries.geometries.isEmpty(),
                "Destroying the owner after acceptance must not start a geometry request");
        draft = std::make_unique<AnnotationDraft>(queries.callbacks(), [] { return true; });
        draft->prepareSave([&](AnnotationDraft::Prepared, quint64) { ++called; });
        draft.reset();
        queries.ranges[1](range());
        require(called == 0, "A save response must not call a destroyed draft's observer");
    }
    return 0;
}
