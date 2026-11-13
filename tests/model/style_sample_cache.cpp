#include "reading/style_sample_cache.h"
#include <QGuiApplication>
#include <QJsonObject>
#include <cstdio>
#include <functional>
#include <memory>
#include "check.h"

using namespace kobalt;

namespace {
QImage page(int width, int height, QRgb paper)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(paper);
    image.setPixel(1, 1, qRgb(40, 40, 40));
    image.setDevicePixelRatio(2);
    return image;
}

StyleSample sample(int width, int height, QRgb paper = qRgb(255, 255, 255))
{
    StyleSample result;
    result.spreads.append({ page(width, height, paper), page(width, height, paper) });
    result.crops.append(page(width, height / 2, paper));
    result.details.append(page(width / 2, height / 2, paper));
    return result;
}
} // namespace

// Checks the Styling sample cache: its key, the two orientations, the size limit and grey storage.
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "minimal");
    QGuiApplication app(argc, argv);

    EngineFontInputs fonts;
    fonts.sources.bundled = QStringList{ "a.ttf" };
    StyleSampleRequest request;
    request.settings = QJsonObject{ { "dark_mode", false }, { "word_spacing", 100 } };
    request.key = "word_spacing";
    request.values = { 80, 100, 120 };
    request.page = { 600, 800 };
    request.thumbnail = { 150, 200 };
    const auto key = styleSampleKey(fonts, request);
    require(key == styleSampleKey(fonts, request), "The same request gave two keys");
    // Every input of a sample changes the key.
    const auto differs = [&](const char *what, std::function<void(StyleSampleRequest &)> change) {
        auto other = request;
        change(other);
        require(styleSampleKey(fonts, other) != key, what);
    };
    differs("The theme is not in the key",
            [](StyleSampleRequest &r) { r.settings["dark_mode"] = true; });
    differs("A Styling value is not in the key",
            [](StyleSampleRequest &r) { r.settings["paragraph_spacing"] = 120; });
    differs("The varied setting is not in the key",
            [](StyleSampleRequest &r) { r.key = "justification_tracking"; });
    differs("The options are not in the key", [](StyleSampleRequest &r) { r.values = { 80 }; });
    differs("The page is not in the key", [](StyleSampleRequest &r) { r.page = { 800, 600 }; });
    differs("The thumbnail is not in the key",
            [](StyleSampleRequest &r) { r.thumbnail = { 100, 200 }; });
    differs("The crop is not in the key", [](StyleSampleRequest &r) { r.crop = { 10, 10 }; });
    differs("The crop origin is not in the key",
            [](StyleSampleRequest &r) { r.cropOrigin = { 1, 0 }; });
    differs("The crop ratio is not in the key", [](StyleSampleRequest &r) { r.cropRatio = 2; });
    differs("The margin guides are not in the key",
            [](StyleSampleRequest &r) { r.marginGuides = true; });
    differs("The reading area is not in the key",
            [](StyleSampleRequest &r) { r.readingArea = { 1, 1, 5, 5 }; });
    differs("The detail is not in the key", [](StyleSampleRequest &r) { r.detail = { 5, 5 }; });
    differs("The bold heading is not in the key",
            [](StyleSampleRequest &r) { r.boldHeading = true; });
    // The displayed frame is not an input: a page turn keeps the sample.
    const QJsonObject frame{ { "word_spacing", 100 }, { "page", 1 }, { "cached_pages", 1 } };
    auto turned = frame;
    turned["page"] = 2;
    turned["next_page"] = 3;
    turned["cached_pages"] = 2;
    turned["chapter_title"] = "Two";
    turned["chapter_page"] = 1;
    turned["chapter_pages"] = 4;
    require(styleSampleSettings(turned) == styleSampleSettings(frame),
            "The displayed frame changed the sample settings");
    // A book that shows one chapter at a time reports where that part is,
    // its estimated pages and how long it took to load. None of it is an
    // input: the next opening of the book must find the kept sample.
    auto chapter = frame;
    chapter["book_fraction"] = 0.25;
    chapter["slice_start"] = "/body/DocFragment[3].0";
    chapter["continues_before"] = true;
    chapter["continues_after"] = true;
    chapter["chapter_load_ms"] = 31.5;
    chapter["page_estimate"] = QJsonObject{ { "version", 1 } };
    chapter["rough_page"] = 12;
    chapter["rough_pages"] = 300;
    chapter["rough_item_page"] = 2;
    chapter["rough_item_pages"] = 9;
    chapter["rough_pages_estimated"] = true;
    chapter["rough_item_pages_estimated"] = true;
    chapter["koblaze_first_spine_item"] = 3;
    chapter["koblaze_last_spine_item"] = 3;
    require(styleSampleSettings(chapter) == styleSampleSettings(frame),
            "The chapter that is shown changed the sample settings");
    auto otherFonts = fonts;
    otherFonts.fallbackFiles = QStringList{ "b.ttf" };
    require(styleSampleKey(otherFonts, request) != key, "The font inputs are not in the key");
    // The same font file in another folder is the same input: Kobo's
    // system fonts are unpacked into a folder with a new name on every start.
    auto firstStart = fonts, secondStart = fonts;
    firstStart.sources.systemFiles = QStringList{ "/tmp/run-a/system/Serif.ttf" };
    secondStart.sources.systemFiles = QStringList{ "/tmp/run-b/system/Serif.ttf" };
    require(styleSampleKey(firstStart, request) == styleSampleKey(secondStart, request),
            "The folder of a system font changed the key");
    secondStart.sources.systemFiles = QStringList{ "/tmp/run-b/system/Sans.ttf" };
    require(styleSampleKey(firstStart, request) != styleSampleKey(secondStart, request),
            "Another system font did not change the key");
    // A cancellation flag is not an input.
    auto cancellable = request;
    cancellable.cancelled = std::make_shared<std::atomic_bool>(false);
    require(styleSampleKey(fonts, cancellable) == key, "The cancellation flag changed the key");

    const auto grey = sample(100, 80);
    const qint64 full = 4 * (2 * 100 * 80 + 100 * 40 + 50 * 40);
    StyleSampleCache cache(full);
    StyleSample found;
    require(!cache.find(false, key, &found), "An empty cache had a sample");
    cache.insert(false, key, {});
    require(cache.count(false) == 0, "An invalid sample was kept");
    cache.insert(false, key, grey);
    // A grey page takes one byte per pixel, rows padded to four bytes.
    require(cache.count(false) == 1 && cache.bytes(false) < full / 3,
            "A grey sample was not stored with one byte per pixel");
    require(cache.find(false, key, &found), "A stored sample was not found");
    require(found.spreads == grey.spreads && found.crops == grey.crops
                    && found.details == grey.details,
            "A grey sample came back with other pixels");
    require(found.crops.first().format() == QImage::Format_RGB32
                    && found.crops.first().devicePixelRatio() == 2,
            "A grey sample came back in another format or scale");
    require(!cache.find(true, key, &found), "Landscape found a portrait sample");

    // A page with colour keeps its format.
    const auto colour = sample(100, 80, qRgb(250, 240, 220));
    cache.insert(true, key, colour);
    require(cache.bytes(true) == full && cache.find(true, key, &found)
                    && found.spreads == colour.spreads,
            "A colour sample was changed");
    require(cache.count(false) == 1, "Landscape replaced a portrait sample");

    // The limit holds for each orientation. The sample used longest ago leaves first.
    const QByteArray second("second"), third("third");
    cache.insert(true, second, colour);
    require(cache.count(true) == 1 && cache.find(true, second, &found)
                    && !cache.find(true, key, &found),
            "The limit did not remove the older sample");
    StyleSampleCache three(3 * cache.bytes(false));
    three.insert(false, key, grey);
    three.insert(false, second, grey);
    three.insert(false, third, grey);
    require(three.find(false, key, &found), "The first sample left too early");
    three.insert(false, "fourth", grey);
    require(three.count(false) == 3 && three.find(false, key, &found)
                    && !three.find(false, second, &found),
            "The limit did not remove the sample used longest ago");
    StyleSampleCache small(10);
    small.insert(false, key, grey);
    require(small.count(false) == 0, "A sample larger than the limit was kept");
    cache.clear();
    require(cache.count(false) == 0 && cache.count(true) == 0, "Clearing kept a sample");
    std::puts("PASS: style sample cache keys, orientations, limit and grey storage");
}
