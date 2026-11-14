#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/engine.h"
#include "engine/blaze/adapter/reader_render_callback.h"
#include <QGuiApplication>
#include <cstdio>
#include <thread>

using namespace kobalt;

namespace {
void queryStates()
{
    const LayoutGeneration first{ 1, 1 }, second{ 1, 2 };
    const Query<int> unknown;
    require(unknown.state() == QueryState::PendingLayout && !unknown.ready(),
            "A new query must be pending, not zero");
    const auto exact = Query<int>::exact(0, first);
    require(exact.ready() && exact.exact() && exact.value() == 0,
            "An exact zero must stay a value");
    require(exact.currentFor(first) && !exact.currentFor(second),
            "A value of another layout must not be current");
    const auto estimated = Query<int>::estimated(120, QueryBasis::Words, first);
    require(estimated.ready() && !estimated.exact() && estimated.basis() == QueryBasis::Words,
            "An estimate must carry its basis and must not be exact");
    const auto pending = Query<int>::pending(first);
    require(!pending.ready() && pending.state() == QueryState::PendingLayout,
            "A pending query has no value");
    const auto unsupported = Query<int>::unsupported("No page total in this engine");
    require(unsupported.state() == QueryState::Unsupported && !unsupported.ready()
                    && !unsupported.reason().isEmpty(),
            "An unsupported query must say why");
    const auto failed = Query<int>::failed("Layout failed");
    require(failed.state() == QueryState::Failed && !failed.ready()
                    && failed.state() != unsupported.state(),
            "A failed query is not an unsupported one");
}

void open(Engine &engine, const test::BookFiles &files)
{
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto source = std::make_shared<ResourceCache>(
            files.keys(),
            [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    QString error;
    require(engine.openResources(source, sizes, { 600, 800 },
                                 { { "font_pixels", 28 }, { "line_spacing_version", 1 } }, &error),
            qPrintable(error));
}

// Chapter changes can retain a view after its opening callback leaves scope.
// The callback belongs to that view, even after its unique owner moves.
void callbacksFollowViews()
{
    const auto book = createEngine(test::readerFonts());
    open(*book, test::engineBook());
    auto view = std::make_unique<ColumnView>();
    {
        QList<int> progress;
        OpeningCallback outer(
                view, [&](OpeningStage, int percent) { progress.append(percent); },
                [] { return true; });
        outer.OnFormatProgress(40);
        {
            OpeningCallback inner(view, {}, [] { return false; });
            require(view->getCallback() == &inner, "The inner callback must receive rendering");
            inner.OnFormatProgress(10);
            inner.OnFormatProgress(70);
            require(progress == QList<int>({ 40, 70 }),
                    "Nested pagination must advance the caller's progress");
            require(view->getCallback()->OnRenderCancelRequested(),
                    "A nested layout ignored the outer cancellation request");
        }
        require(view->getCallback() == &outer,
                "A nested layout discarded the outer opening callback");
        require(view->getCallback()->OnRenderCancelRequested(),
                "A nested layout lost the outer cancellation request");
    }
    require(!view->getCallback(), "The last callback must detach from the view");

    std::unique_ptr<ColumnView> retained;
    {
        OpeningCallback callback(view, {});
        retained = std::move(view);
    }
    require(!retained->getCallback(), "A retained view kept a dead opening callback");

    view = std::make_unique<ColumnView>();
    // Reusing the old owner must not let the first guard clear a callback
    // installed on a different view.
    std::unique_ptr<OpeningCallback> replacement;
    {
        OpeningCallback callback(view, {});
        retained = std::move(view);
        view = std::make_unique<ColumnView>();
        replacement = std::make_unique<OpeningCallback>(view, OpeningProgress{});
    }
    require(!retained->getCallback() && view->getCallback() == replacement.get(),
            "A moved callback guard detached from the wrong view");
    replacement.reset();

    // A failed opening can destroy the view before its callback guard exits.
    {
        OpeningCallback callback(view, {});
        view.reset();
    }
}

// A whole-book layout can park while the reader lays out a chapter on
// another thread. Nested flows must poll the chapter's callback.
void parkedLayoutCallbacks()
{
    struct Polls : LVDocViewCallback
    {
        int count = 0;

        bool OnRenderCancelRequested() override
        {
            ++count;
            return false;
        }
    };

    Polls background;
    LVRendPageContext parked(nullptr, 800);
    parked.setCallback(&background, 10);
    parked.updateRenderProgress(1);
    Polls foreground;
    std::thread chapter([&] {
        LVRendPageContext current(nullptr, 800);
        current.setCallback(&foreground, 10);
        current.updateRenderProgress(1);
        LVRendPageContext nested(nullptr, 800);
        nested.updateRenderProgress(1);
        current.Abandon();
    });
    chapter.join();
    parked.Abandon();
    require(background.count == 1 && foreground.count == 2,
            "A nested chapter flow polled the parked whole-book callback");
}

// The engine that reads books today lays out the whole book, so it must
// report every page value as exact and equal to its page numbers.
void wholeBookPages()
{
    const auto engine = createEngine(test::readerFonts());
    require(!engine->pageInfo().bookPages.ready(), "An engine without a book has no pages");
    open(*engine, test::engineBook());
    require(engine->pages() > 3, "The fixture needs several pages");
    const auto generation = engine->pageInfo().bookPages.generation();
    for (int page = 0; page < engine->pages(); ++page) {
        require(engine->go(page), "Cannot go to a page");
        const auto info = engine->pageInfo();
        require(info.bookPage.exact() && info.bookPage.value() == page,
                "The book page must be exact");
        require(info.bookPages.exact() && info.bookPages.value() == engine->pages(),
                "The book page total must be exact");
        require(info.bookProgress.exact() && info.bookProgress.value() >= 0
                        && info.bookProgress.value() < 1,
                "Book progress must be a part of the book");
        const auto chapter = engine->chapterProgress();
        require(info.chapterPage.exact() && info.chapterPages.exact()
                        && info.chapterPage.value() == chapter.page - 1
                        && info.chapterPages.value() == chapter.pages,
                "Chapter pages must match the chapter progress");
        require(info.bookPages.currentFor(generation), "Page turns must not change the layout");
    }
    require(engine->style(36, 100), "Cannot change the font size");
    require(!engine->pageInfo().bookPages.currentFor(generation),
            "A new layout must have a new generation");
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    queryStates();
    wholeBookPages();
    callbacksFollowViews();
    parkedLayoutCallbacks();
    std::printf("PASS: query states and exact page values of the whole-book engine\n");
    return 0;
}
