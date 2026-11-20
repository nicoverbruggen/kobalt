#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/column_view.h"
#include "engine/blaze/adapter/engine_document.h"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <cstdio>
#if __has_include(<koblaze.h>)
#  include <koblaze.h>
#endif

using namespace kobalt;

namespace kobalt {
struct ReaderLocationProbe
{
    static ColumnView &view(EngineDocument &model) { return *model.engine_; }
};
} // namespace kobalt

namespace {
void open(EngineDocument &model, const test::BookFiles &files)
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
    require(model.openResources(source, sizes, { 600, 800 },
                                { { "font_pixels", 28 }, { "line_spacing_version", 1 } }, &error),
            qPrintable(error));
}

QStringList pageStarts(const EngineDocument &model)
{
    QStringList starts;
    for (int page = 0; page < model.pages(); ++page)
        starts.append(model.pageStart(page));
    return starts;
}

bool setFont(EngineDocument &model, int pixels)
{
    return model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", pixels } }));
}

#if __has_include(<koblaze.h>)
void rejectedPageList()
{
    LVRendPageList pages;
    pages.add(new LVRendPageInfo(0, 100, 0));
    pages.add(new LVRendPageInfo(100, 100, 1));
    pages[1]->justificationPrefix = 101;
    SerialBuf saved(128);
    require(pages.serialize(saved), "Cannot save test page list");
    SerialBuf input(saved.buf(), saved.pos());
    LVRendPageList restored;
    require(!restored.deserialize(input) && restored.empty(),
            "A rejected page record left a partial page list");
    pages[1]->justificationPrefix = 0;
    saved.reset();
    require(pages.serialize(saved), "Cannot save the corrected page list");
    SerialBuf incomplete(saved.buf(), saved.pos() / 2);
    require(!restored.deserialize(incomplete) && restored.empty(),
            "An incomplete list restored pages");
    SerialBuf complete(saved.buf(), saved.pos());
    require(restored.deserialize(complete) && restored.length() == 2 && restored[1]->start == 100,
            "A valid page list failed to restore after a rejected list");
}

// A stop after the page split must discard the unfinished layout before another render.
void cancelledPageSplit()
{
    EngineDocument reader(test::readerFonts());
    open(reader, test::engineBook(false, 2, 120));
    auto &view = ReaderLocationProbe::view(reader);
    view.setFontSize(40);
    LVRendPageList pages;
    koblazeSetCancelCheck([](void *p) { return static_cast<LVRendPageList *>(p)->length() > 1; },
                          &pages);
    bool stopped = false;
    try {
        view.Render(0, 0, &pages);
    } catch (const KoblazeRenderCancelled &) {
        stopped = true;
    }
    koblazeSetCancelCheck(nullptr, nullptr);
    require(stopped, "The render did not stop after splitting the pages");
    require(pages.length() == 0 && !view.getDocument()->koblazeSaveLayout(),
            "A stopped render kept the split pages or accepted an unfinished layout");
    view.Render();
    const int count = view.getPageList()->length();
    require(count > 1 && view.koblazeSaveLayout() && view.koblazeRestoreLayout()
                    && view.getPageList()->length() == count,
            "The retry did not produce a restorable page list");
}

// A style pass can stop with a fragment stylesheet active after the old page geometry was saved.
void cancelledStylesDiscardSavedLayout()
{
    EngineDocument reader(test::readerFonts());
    open(reader, test::engineBook(false, 2, 120));
    auto &view = ReaderLocationProbe::view(reader);
    require(view.koblazeSaveLayout(), "Cannot save the layout before a style change");

    struct StopDuringStyles : LVDocViewCallback
    {
        bool entered = false;

        void OnNodeStylesUpdateProgress(int) override { entered = true; }
    } callback;

    auto *previous = view.getCallback();
    view.setCallback(&callback);
    view.setFontSize(40);
    koblazeSetCancelCheck([](void *p) { return static_cast<StopDuringStyles *>(p)->entered; },
                          &callback);
    bool stopped = false;
    try {
        view.Render();
    } catch (const KoblazeRenderCancelled &) {
        stopped = true;
    }
    koblazeSetCancelCheck(nullptr, nullptr);
    view.setCallback(previous);
    require(stopped, "The layout did not stop inside the style pass");
    require(!view.koblazeRestoreLayout(), "Restored old geometry onto partially updated styles");
    require(!view.getDocument()->getStyleSheet()->pop(),
            "The stopped style pass left a stylesheet pushed");
}
#endif

void interruptedViewportChange()
{
    QByteArray body;
    for (int i = 0; i < 60; ++i)
        body += "<p style='page-break-before:always'>Passage " + QByteArray::number(i) + ". "
                + QByteArray("The chapter continues. ").repeated(20) + "</p>";
    const auto files = test::publisherPackage("en", "Viewport changes",
                                              { { "chapter.xhtml", test::publisherHtml(body) } },
                                              "<itemref idref=\"chapter.xhtml\"/>");
    EngineDocument model(test::readerFonts()), reference(test::readerFonts());
    for (auto *reader : { &model, &reference }) {
        open(*reader, files);
        reader->setChapterSlices(3);
        require(reader->showSpineItems(0, 0) && reader->go(0), "Cannot open the sliced chapter");
        // With a single spine item, continuesBefore() can only mean a later slice.
        while (!reader->continuesBefore())
            require(reader->turnInWindow(1), "Cannot reach a later slice");
        require(reader->turnInWindow(1), "Cannot go beyond the slice boundary");
    }
    const auto compare = [&] {
        const auto start = reference.pageStart(reference.page());
        require(!start.isEmpty() && model.pageStart(model.page()) == start
                        && model.position() == reference.position(),
                "The interrupted viewport change selected different text");
        const auto image = reference.image(reference.page());
        require(!image.isNull() && model.image(model.page()) == image,
                "The interrupted viewport change painted a different page");
    };
    compare();
    const auto anchor = reference.position();
    const QSize narrow(460, 720), original(600, 800);
    require(reference.applyAppearance({}, narrow), "Cannot resize the reference reader");
    int checks = 0;
    model.setLayoutInterrupt([&] { return ++checks == 3; });
    require(!model.applyAppearance({}, narrow) && model.layoutInterrupted() && checks == 3,
            "Resizing a later slice must stop when requested");
    model.setLayoutInterrupt({});
    require(model.resumeLayout() && !model.layoutInterrupted() && model.position() == anchor,
            "Resuming the viewport change lost the saved position");
    compare();
    require(reference.applyAppearance({}, original) && model.applyAppearance({}, original)
                    && model.position() == anchor,
            "Returning to the original viewport lost the saved position");
    compare();
    for (const int direction : { 1, -1 }) {
        for (int i = 0; i < 8; ++i) {
            require(reference.turnInWindow(direction) && model.turnInWindow(direction),
                    "Cannot retrace pages after the viewport change");
            compare();
        }
    }
}
} // namespace

// A layout that is stopped must leave the book usable: the next layout gives
// the same pages as a layout that was never stopped, for the new settings
// and for the settings of the last complete layout.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
#if __has_include(<koblaze.h>)
    // A layout destructor can reach a second cancellation check while the first unwinds.
    int cancellationChecks = 0;
    koblazeSetCancelCheck(
            [](void *context) {
                ++*static_cast<int *>(context);
                return true;
            },
            &cancellationChecks);
    try {
        struct CheckOnUnwind
        {
            ~CheckOnUnwind() { koblazeCheckCancel(); }
        } check;

        koblazeCheckCancel();
        require(false, "The first check did not cancel layout");
    } catch (const KoblazeRenderCancelled &) {
        require(cancellationChecks == 1, "Layout asked to cancel again while unwinding");
    }
    koblazeSetCancelCheck(nullptr, nullptr);
    interruptedViewportChange();
    cancelledStylesDiscardSavedLayout();
    rejectedPageList();
    cancelledPageSplit();
    EngineDocument model(test::readerFonts());
    open(model, test::engineBook(false, 8, 120));
    require(model.go(model.pages() / 2), "Cannot go to the middle of the book");
    const auto position = model.position();
    const auto small = pageStarts(model);
    require(setFont(model, 36), "Cannot lay out with the large font");
    const auto large = pageStarts(model);
    require(large != small && model.position() == position, "The font change must make new pages");
    require(setFont(model, 28) && pageStarts(model) == small, "The first layout must come back");

    int polls = 0;
    model.setLayoutInterrupt([&polls] {
        ++polls;
        return false;
    });
    require(setFont(model, 36) && !model.layoutInterrupted(),
            "A layout that is not stopped must finish");
    const int blocks = polls;
    require(blocks > 100, "The engine must ask after each block of text");
    require(setFont(model, 28), "Cannot go back to the small font");

    for (const int stopAfter : { 1, 7, blocks / 3, blocks - 2 }) {
        polls = 0;
        QElapsedTimer timer;
        qint64 latency = -1;
        model.setLayoutInterrupt([&] {
            if (++polls < stopAfter)
                return false;
            timer.start();
            return true;
        });
        require(!setFont(model, 36) && model.layoutInterrupted(),
                "The layout must stop when asked");
        latency = timer.nsecsElapsed() / 1000;
        require(polls == stopAfter, "The layout must stop at the block where it was asked");
        // A second stop while the layout runs again.
        polls = 0;
        require(!model.resumeLayout() && model.layoutInterrupted(),
                "A resumed layout can stop again");
        model.setLayoutInterrupt({});
        require(model.resumeLayout() && !model.layoutInterrupted(),
                "The layout must finish when not stopped");
        require(pageStarts(model) == large,
                "A stopped layout changed the pages of the next layout");
        require(model.position() == position, "A stopped layout lost the reading position");
        // The settings of the last complete layout must not reuse the stopped one.
        require(setFont(model, 28) && pageStarts(model) == small,
                "The earlier layout came back changed after a stopped layout");
        std::printf("stopped after block %d of %d: returned in %lld microseconds\n", stopAfter,
                    blocks, (long long)latency);
    }
    std::printf("PASS: a stopped layout leaves the book and the next layout intact\n");
#else
    std::printf("SKIP: stock CREngine cannot stop a layout\n");
#endif
    return 0;
}
