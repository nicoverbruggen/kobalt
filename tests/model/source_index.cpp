#include "reader_inputs.h"
#include "reader_source_index_probe.h"
#include "engine_fixtures.h"
#include "check.h"
#include <QGuiApplication>
#include <algorithm>

using namespace kobalt;
using Probe = ReaderSourceIndexProbe;
using Ranges = Probe::Ranges;

namespace {
void open(EngineDocument &model, const test::BookFiles &files, QSize size = { 600, 800 })
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
    require(model.openResources(source, sizes, size,
                                { { "font_pixels", 28 }, { "line_spacing_version", 1 } }, &error),
            qPrintable(error));
}

QStringList endpoints(const Ranges &ranges)
{
    QStringList result;
    for (const auto &range : ranges)
        result << range.first << range.second;
    return result;
}

void compare(EngineDocument &model, const Ranges &ranges, bool geometry = false)
{
    for (int page = 0; page < model.pages(); ++page) {
        model.go(page);
        const auto expected = Probe::visible(model, ranges);
        require(model.visibleRanges(ranges) == expected,
                "Indexed visible ranges differ from original source scan");
        require(model.visibleRanges(ranges) == expected, "Repeated query changed cached ranges");
        const auto retained = Probe::usage(model);
        for (int sample = 0; sample < qMin(6, int(ranges.size())); ++sample) {
            const auto &draft = ranges[sample * (ranges.size() - 1) / 5];
            require(model.rangeVisible(draft.first, draft.second)
                            == !Probe::visible(model, Ranges{ draft }).isEmpty(),
                    "Separate draft visibility changed source-overlap semantics");
            require(Probe::usage(model) == retained, "Draft query changed the saved-range cache");
        }
        if (geometry) {
            const auto groups = model.pageRangeRects(page, ranges);
            for (int i = 0; i < ranges.size(); ++i)
                require(groups[i]
                                == (expected.contains(i)
                                            ? model.rangeRects(ranges[i].first, ranges[i].second)
                                            : QVector<QRect>()),
                        "Indexed geometry changed a range or its input order");
        }
    }
    const auto anchors = endpoints(ranges);
    for (const bool exclusive : { false, true }) {
        QVector<int> expectedOrder, actualOrder;
        const auto expected = Probe::pages(model, anchors, exclusive, &expectedOrder);
        require(model.pagesOf(anchors, exclusive, &actualOrder) == expected
                        && actualOrder == expectedOrder,
                "Cached endpoint pages or source ranks differ from original lookup");
        require(model.pagesOf(anchors, exclusive, &actualOrder) == expected
                        && actualOrder == expectedOrder,
                "Warm endpoint cache changed page or exclusive-end semantics");
    }
    for (int i = 0; i < qMin(6, int(ranges.size())); ++i) {
        const auto &range = ranges[i];
        const auto expected = Probe::overlaps(model, range.first, range.second, ranges);
        require(model.intersectingRanges(range.first, range.second, ranges) == expected,
                "Indexed overlap differs from original strict text-range scan");
        require(model.intersectingRanges(range.first, range.second, ranges) == expected,
                "Warm overlap query changed its result");
    }
}
} // namespace

// Checks cached annotation visibility and overlap against the previous algorithms, and bounded anchor-name lookups.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    EngineDocument model(test::readerFonts());
    auto book = test::engineBook(false, 3, 5);
    book["OEBPS/c1.xhtml"].replace("</body>",
                                   "<p style=\"display:none\">Invisible marker.</p></body>");
    open(model, book);
    for (const bool normalized : { false, true })
        require(Probe::positionLookupPreservesNames(model, normalized),
                "Invalid position lookup added a name to the document");
    require(model.pages() > 8, "Fixture does not span enough pages");
    Ranges marks;
    for (int page = 0; page < model.pages(); ++page) {
        const auto hits = model.searchPage("reader", page).hits;
        for (const auto &hit : hits)
            marks.append({ hit.start, hit.end });
    }
    require(marks.size() >= 20, "Fixture supplies too few source ranges");
    const auto firstWord = marks.first();
    const QString before = Probe::shifted(model, firstWord.first, -1),
                  after = Probe::shifted(model, firstWord.second, 1);
    const QString hiddenStart = Probe::textPoint(model, "Invisible marker", 0);
    const QString hiddenEnd = Probe::textPoint(model, "Invisible marker", 4);
    require(!hiddenStart.isEmpty() && !hiddenEnd.isEmpty(),
            "Hidden text fixture was not retained in DOM");
    Ranges strict{ firstWord,
                   { firstWord.second, firstWord.first },
                   { firstWord.first, firstWord.first },
                   { hiddenStart, hiddenEnd },
                   { firstWord.first, Probe::shifted(model, firstWord.second, 100000) } };
    for (const auto &query : Ranges{ { before, firstWord.first },
                                     { firstWord.second, after },
                                     { before, after },
                                     { firstWord.second, firstWord.first },
                                     { hiddenStart, hiddenEnd },
                                     { "/missing", after },
                                     { firstWord.first, firstWord.first } }) {
        require(model.intersectingRanges(query.first, query.second, strict)
                        == Probe::overlaps(model, query.first, query.second, strict),
                "Strict overlap endpoint validation changed");
        require(model.rangeVisible(query.first, query.second)
                        == !Probe::visible(model, Ranges{ query }).isEmpty(),
                "Single draft visibility changed endpoint validation");
    }
    require(model.intersectingRanges(before, firstWord.first, strict).isEmpty(),
            "Touching start was counted as overlap");
    require(model.intersectingRanges(firstWord.second, after, strict).isEmpty(),
            "Touching end was counted as overlap");
    require(model.intersectingRanges(before, after, strict) == QVector<int>{ 0 },
            "Reversed, hidden or invalid range was accepted");
    model.go(model.pagesOf(QStringList{ firstWord.first }).first());
    Ranges draft(2001, firstWord);
    require(model.visibleRanges(draft).size() == draft.size(),
            "2000 highlights plus a draft lost a range");
    require(model.intersectingRanges(before, after, draft).size() == draft.size(),
            "Draft overlap limit changed");
    draft.append(firstWord);
    require(model.intersectingRanges(before, after, draft).size() == draft.size()
                    && !Probe::cached(model),
            "Large overlap input was truncated or retained beyond the range budget");
    // Shuffle input order. Shared starts/ends, nested spans and a whole-book
    // passage must work without relying on disjoint or lexically sorted text.
    std::reverse(marks.begin(), marks.end());
    marks.append({ marks.last().first, marks.first().second });
    marks.append({ marks.first().second, marks.last().first });
    marks.append(marks[2]);
    marks.append({ marks[0].first, marks[0].first });
    marks.append({ {}, marks[1].second });
    marks.append({ "/body[999]/text().0", marks[0].second });
    marks.append({ model.pageStart(2), model.pageStart(5) });
    compare(model, marks, true);
    const auto pointer = marks[0].first;
    const auto element = Probe::childOffset(model, pointer);
    require(!element.isEmpty(), "Cannot construct element-offset case");
    auto unusual = marks;
    unusual.append({ element, pointer });
    unusual.append({ pointer, element });
    compare(model, unusual);
    // Endpoint edits and deletion change the input vector without changing layout.
    auto changed = marks;
    changed.removeAt(2);
    changed[0].second = marks[4].second;
    compare(model, changed);
    const auto settings = model.state();
    require(model.applyAppearance({ { "font_pixels", model.fontSize() + 8 } }),
            "Cannot reflow font size");
    compare(model, marks);
    require(model.applyAppearance({ { "font_face", test::alternateReadingFace() } }),
            "Cannot reflow reading font");
    compare(model, marks);
    require(model.applyAppearance({ { "columns", 2 } }, { 900, 600 }),
            "Cannot rotate into two columns");
    compare(model, marks, true);
    // A -> B -> A without any intervening index access must discard old DOM paths.
    require(model.applyAppearance(settings, { 600, 800 }), "Cannot restore layout A");
    model.visibleRanges(marks);
    require(model.applyAppearance({ { "font_pixels", model.fontSize() + 5 } }),
            "Cannot set intermediate layout B");
    require(model.applyAppearance(settings, { 600, 800 }), "Cannot restore layout A again");
    require(!Probe::cached(model), "A-B-A reflow retained compiled source pointers");
    compare(model, marks);
    int checks = 0;
    Probe::clear(model);
    const auto anchors = endpoints(marks);
    require(model.pagesOf(anchors, true, nullptr, [&] { return ++checks < 4; }).isEmpty(),
            "Cancelled lookup returned partial pages");
    compare(model, marks);
    require(model.pagesOf(anchors, true, nullptr, [] { return false; }).isEmpty(),
            "Already stale lookup was executed");
    auto spareCapacity = marks;
    spareCapacity.reserve(200000);
    Probe::clear(model);
    require(model.visibleRanges(spareCapacity) == Probe::visible(model, marks),
            "Reserved caller vector changed results");
    require(Probe::usage(model).second <= 4 * 1024 * 1024,
            "Cache retained oversized caller vector allocation");
    spareCapacity = {};
    // Oversized/malformed strings must not create unbounded retained cache data.
    Ranges large;
    for (int i = 0; i < 2000; ++i)
        large.append({ QString(4000, 'x') + QString::number(i), marks[0].second });
    require(model.visibleRanges(large).isEmpty(), "Malformed long anchors resolved");
    require(Probe::usage(model).second <= 4 * 1024 * 1024,
            "Source index exceeds its retained-byte budget");
    model.pagesOf(QStringList{ QString(100000, 'x') });
    require(Probe::usage(model).second <= 4 * 1024 * 1024,
            "Endpoint cache exceeds its retained-byte budget");
    Ranges oversized{ { QString(100000, 'x'), marks[0].second } };
    require(model.visibleRanges(oversized).isEmpty() && !Probe::cached(model),
            "Invalid large range payload stayed cached");
    compare(model, marks);
    QStringList many;
    for (int i = 0; i < 9000; ++i)
        many.append("/missing[" + QString::number(i) + "]");
    require(model.pagesOf(many).size() == many.size(), "Oversized endpoint request lost results");
    require(!Probe::cached(model), "Endpoint count bound retained oversized cache");
    compare(model, marks);
    // Same EngineDocument object opens a different document. Configure clears even
    // if allocator reuse gives the replacement document the same address.
    open(model, test::engineBook(false, 1, 1));
    require(!Probe::cached(model), "Replacing document retained source pointers");
    compare(model, marks);
    open(model, book);
    compare(model, marks);
    fprintf(stderr,
            "Source index: ranges, geometry, pages, reflow, replacement, cancellation and budgets passed\n");
}
