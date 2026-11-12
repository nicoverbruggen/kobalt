#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <cstdio>

using namespace kobalt;

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
} // namespace

// A layout that is stopped must leave the book usable: the next layout gives
// the same pages as a layout that was never stopped, for the new settings
// and for the settings of the last complete layout.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
#if __has_include(<koblaze.h>)
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
