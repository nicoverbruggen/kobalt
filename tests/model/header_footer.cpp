#include "reading/header_footer.h"
#include "reading/reading_density.h"
#include "reading/remaining_time.h"
#include <limits>
#include "support/qt_compat.h"
#include "support/strings.h"
#include "check.h"

#include <algorithm>
#include <iterator>

using namespace kobalt;
using namespace kobalt::headerfooter;

// Checks header and footer layouts: defaults, legacy flags and sizes, the battery field, time-remaining text and round trips.
int main()
{
    // Default keeps the chapter prominent and the footer free of a bar.
    const auto standard = presetLayout(Preset::Default);
    require(isPhoneScreen({ 720, 1600 }, 300) && isPhoneScreen({ 1600, 720 }, 300),
            "Phone detection changed with rotation");
    require(isPhoneScreen({ 1080, 2400 }, 450), "Phone detection ignored density");
    require(!isPhoneScreen({ 1072, 1448 }, 300) && !isPhoneScreen({ 1448, 1072 }, 300),
            "Six-inch reader detected as a phone");
    require(isPhoneScreen({ 824, 1648 }, 300), "BOOX Palma not detected as a phone");
    require(!isPhoneScreen({ 758, 1024 }, 212), "Six-inch 212 PPI reader detected as a phone");
    const auto phone = select({}, true);
    require(phone.preset == Preset::Phone && !phone.layout.header() && phone.layout.footer(),
            "Phone default did not remove the header");
    require(select(qt::object({ { presetKey, "default" } }), true).layout == standard,
            "Phone default replaced an explicit Kobalt preset");
    require(select(qt::object({ { "show_header", true } }), true).preset != Preset::Phone,
            "Phone default replaced legacy settings");

    require(standard.slotFields[0] == Field::Time && standard.slotFields[1] == Field::Chapter
                    && standard.slotFields[2] == Field::Nothing
                    && standard.slotFields[4] == Field::Nothing
                    && standard.slotFields[3] == Field::ChapterPagesLeft
                    && standard.slotFields[5] == Field::BookPercent,
            "Default does not show the clock, chapter, chapter pages left and book percentage");
    require(standard.uppercase[1] && !standard.progressBar && standard.headerSize == smallSize
                    && standard.footerSize == smallSize,
            "Default lost its uppercase chapter, hidden bar or small sizes");
    const auto a = presetLayout(Preset::AlternativeA);
    require(a.slotFields[0] == Field::Time && a.slotFields[1] == Field::ChapterAndPage
                    && a.slotFields[4] == Field::TitleAndPage && a.uppercase[1] && a.uppercase[4]
                    && a.progressBar,
            "Alternative A does not centre a name and a position twice");
    const auto b = presetLayout(Preset::AlternativeB);
    require(b.slotFields[1] == Field::Time && b.slotFields[3] == Field::ChapterPagesLeft
                    && b.slotFields[5] == Field::BookPercent && !b.uppercase[3] && !b.progressBar,
            "Alternative B is not the quiet preset");

    // Values round-trip, and unknown values fall back instead of failing.
    for (const auto field : fields)
        require(headerfooter::field(fieldValue(field)) == field, "A field did not round-trip");
    for (const auto value : presets)
        require(preset(presetValue(value)) == value, "A preset did not round-trip");
    require(preset("missing") == Preset::Default
                    && headerfooter::field("missing") == Field::Nothing,
            "Unknown values did not fall back");
    auto edited = a;
    edited.slotFields[5] = Field::Battery;
    edited.headerEnabled = false;
    edited.headerSize = maximumSize;
    edited.uppercase[2] = false;
    require(fromJson(toJson(edited), standard) == edited, "A layout did not round-trip");
    auto lower = fromJson(qt::object({ { "uppercase", false } }), standard);
    require(!lower.uppercase[0] && !lower.uppercase[5],
            "A single uppercase value did not apply to every slot");
    const auto partial =
            fromJson(qt::object({ { "top_left", "author" }, { "footer_size", "huge" } }), standard);
    require(partial.slotFields[0] == Field::Author && partial.slotFields[1] == Field::Chapter
                    && partial.footerSize == smallSize,
            "Missing or invalid layout values did not keep the fallback");

    // Old presets map exactly; optional numeric sizes retain a disabled strip's last size.
    for (const auto name : { "small", "medium", "large", "off" }) {
        const auto loaded = fromJson(
                qt::object({ { "header_size", name }, { "footer_size", name } }), standard);
        require(loaded.headerSize == legacySize(name) && loaded.footerSize == legacySize(name)
                        && loaded.headerEnabled == (QString(name) != "off")
                        && loaded.footerEnabled == loaded.headerEnabled,
                "A legacy strip size changed on load");
    }
    for (int size = smallSize; size <= maximumSize; ++size) {
        auto numeric = standard;
        numeric.headerSize = size;
        numeric.footerSize = maximumSize - size + smallSize;
        numeric.headerEnabled = size % 2;
        require(fromJson(toJson(numeric), standard) == numeric,
                "Numeric or disabled sizes did not round-trip");
    }
    for (const QJsonValue &invalid :
         { QJsonValue(23), QJsonValue(49), QJsonValue(27.5), QJsonValue(true), QJsonValue("40"),
           QJsonValue(QJsonValue::Null) }) {
        const auto loaded = fromJson(
                qt::object({ { "header_size", "medium" }, { "header_font_size", invalid } }),
                standard);
        require(loaded.headerSize == mediumSize,
                "Invalid numeric size replaced the valid legacy size");
    }
    const auto disabled = fromJson(
            qt::object({ { "footer_size", "off" }, { "footer_font_size", 39 } }), standard);
    require(!disabled.footerEnabled && disabled.footerSize == 39,
            "Off discarded the remembered numeric size");

    // A strip with nothing in it takes no space, even when its size is set.
    auto empty = standard;
    empty.slotFields[3] = empty.slotFields[4] = empty.slotFields[5] = Field::Nothing;
    require(empty.header() && !empty.footer(), "An empty footer kept its strip");
    empty.headerEnabled = false;
    require(!empty.header(), "Off kept the header");

    // The preset is chosen per book; Custom is one layout for every book.
    const auto custom = toJson(edited);
    auto selection = select(qt::object({ { presetKey, "alternative_b" }, { customKey, custom } }));
    require(selection.preset == Preset::AlternativeB && selection.layout == b,
            "A saved preset was not used");
    selection = select(qt::object({ { presetKey, "custom" }, { customKey, custom } }));
    require(selection.preset == Preset::Custom && selection.layout == edited,
            "Custom did not use the saved layout");
    selection = select(qt::object({ { presetKey, "custom" } }));
    require(selection.preset == Preset::Custom && selection.layout == standard,
            "Custom without a saved layout did not start from Default");

    // Books from before presets keep their old header and footer.
    selection = select({});
    require(selection.preset == Preset::Default && selection.layout == standard,
            "Old default settings did not select Default");
    selection = select(qt::object({ { presetKey, "default" } }));
    require(selection.layout == standard, "An explicit Default did not use the new layout");
    selection = select(qt::object(
            { { "show_header", true }, { "show_footer", true }, { "show_progress_bar", true } }));
    require(selection.preset == Preset::Default && selection.layout == standard,
            "Old default flags prevented the new Default from applying");
    selection = select(qt::object({ { "show_clock", false }, { "footer_text_size", "large" } }));
    require(selection.preset == Preset::Custom && selection.layout.slotFields[0] == Field::Nothing
                    && selection.layout.slotFields[1] == Field::Chapter
                    && selection.layout.footerSize == largeSize,
            "Old custom settings were not kept as Custom");
    selection = select(qt::object({ { "show_header", false },
                                    { "show_footer", false },
                                    { "show_progress_bar", false } }));
    require(selection.layout.slotFields[0] == Field::Time
                    && selection.layout.slotFields[1] == Field::Nothing
                    && selection.layout.headerSize == smallSize && !selection.layout.footerEnabled
                    && !selection.layout.progressBar,
            "The old clock without a header was lost");
    selection = select(qt::object({ { "show_header", false }, { "show_clock", false } }));
    require(!selection.layout.header(), "An old hidden header and clock came back");
    selection = select(qt::object({ { "show_clock", false }, { customKey, custom } }));
    require(selection.preset == Preset::Custom && selection.layout == edited,
            "An old custom book replaced the saved custom layout");

    // Text preferences apply to every preset without changing its slots or progress bar.
    auto enlarged = standard;
    enlarged.headerSize = maximumSize;
    enlarged.footerSize = 37;
    enlarged.headerEnabled = false;
    for (const auto chosen : presets) {
        const auto preferences = qt::object({ { presetKey, presetValue(chosen) },
                                              { customKey, custom },
                                              { textKey, textSettings(enlarged) } });
        const auto resolved = select(preferences);
        const auto expected = chosen == Preset::Custom ? edited : presetLayout(chosen);
        require(resolved.preset == chosen && resolved.layout.slotFields == expected.slotFields
                        && resolved.layout.progressBar == expected.progressBar
                        && resolved.layout.headerSize == maximumSize
                        && resolved.layout.footerSize == 37 && !resolved.layout.headerEnabled
                        && resolved.layout.footerEnabled,
                "Independent sizes changed the preset or were reset by it");
    }
    const auto invalidText = select(qt::object(
            { { presetKey, "custom" },
              { customKey, custom },
              { textKey,
                qt::object({ { "header_font_size", 999 }, { "footer_font_size", "40" } }) } }));
    require(invalidText.layout == edited, "Invalid text overrides did not retain legacy sizes");

    Values values;
    values.title = QStringLiteral("Alice");
    values.author = QStringLiteral("Lewis Carroll");
    values.chapter = QStringLiteral("Down the Rabbit-Hole");
    values.time = QStringLiteral("9:00");
    values.chapterPage = 6;
    values.chapterPages = 20;
    values.page = 6;
    values.pages = 193;
    require(content(standard.slotFields[1], values, standard.uppercase[1]).text
                    == values.chapter.toUpper(),
            "Default did not uppercase the chapter title");
    require(content(standard.slotFields[3], values, false).text.startsWith(QStringLiteral("15 "))
                    && content(standard.slotFields[5], values, false).text
                            == strings::get("common.percent").arg(3),
            "Default footer did not combine chapter pages left with whole-book percentage");
    values.battery = 80;
    const auto emberProgress = content(b.slotFields[5], values, false).text;
    require(emberProgress == strings::get("common.percent").arg(3),
            "Ember does not show whole-book progress");
    auto nextChapter = values;
    nextChapter.chapterPage = 1;
    nextChapter.chapterPages = 4;
    require(content(b.slotFields[5], nextChapter, false).text == emberProgress,
            "Ember progress resets at a chapter boundary");
    const auto chapterPage = strings::get("common.page_fraction").arg(6).arg(20);
    require(content(Field::ChapterPage, values, false).text == chapterPage,
            "Page in chapter is wrong");
    // Beside a name the page reads "6 of 20"; alone it keeps the slash.
    require(content(Field::ChapterAndPage, values, false).text
                    == strings::get("reader.chapter_position")
                               .arg(values.chapter, QStringLiteral("6 of 20")),
            "Chapter and page is wrong");
    const auto combined = content(Field::ChapterAndPage, values, true);
    require(combined.chapterTitle == values.chapter.toUpper()
                    && combined.chapterPosition == QStringLiteral("6 OF 20"),
            "Combined chapter parts lost their case or page count");
    auto missingChapter = values;
    missingChapter.chapter.clear();
    require(content(Field::ChapterAndPage, missingChapter, false).text == QStringLiteral("6 of 20"),
            "An empty chapter left a separator before its page count");
    auto missingTotal = values;
    missingTotal.chapterPages = 0;
    require(content(Field::ChapterAndPage, missingTotal, false).chapterPosition.isEmpty()
                    && content(Field::ChapterAndPage, missingTotal, false).text == values.chapter,
            "An unknown chapter total left a page suffix");
    require(content(Field::TitleAndPage, values, false).text.endsWith(QStringLiteral(" 6 of 193")),
            "Title and page is wrong");
    require(pageCount(Field::ChapterPage) && pageCount(Field::BookPage)
                    && !pageCount(Field::ChapterAndPage),
            "Only lone page counts keep half spaces");
    require(content(Field::Chapter, values, true).text == QStringLiteral("DOWN THE RABBIT-HOLE"),
            "Uppercase did not apply");
    require(content(Field::ChapterPagesLeft, values, false).text.startsWith(QStringLiteral("15 ")),
            "Pages left counts the current page");
    auto lastPage = values;
    lastPage.chapterPage = lastPage.chapterPages;
    require(content(Field::ChapterPagesLeft, lastPage, false).text
                    == strings::get("header_footer.pages_left")
                               .arg(1)
                               .arg(strings::get("stats.page_left")),
            "The last page of a chapter shows 1 page left");
    const auto battery = content(Field::Battery, values, false);
    require(battery.batteryIcon && battery.text.isEmpty() && battery.battery == 80,
            "Battery is not an icon");
    require(content(Field::BatteryPercent, values, false).text
                    == strings::get("common.percent").arg(80),
            "Battery percentage is missing");
    require(content(Field::BatteryPercent, values, false).batteryIcon,
            "Battery with percentage lost its icon");
    for (const bool charging : { false, true }) {
        auto status = values;
        status.charging = charging;
        for (const int percent : { -1, 0, 1, 80, 100 }) {
            status.battery = percent;
            const auto shown = content(Field::BatteryPercentOnly, status, false);
            require(!shown.batteryIcon
                            && shown.text
                                    == (percent < 0 ? QString()
                                                    : strings::get("common.percent").arg(percent)),
                    "Percentage-only battery showed an icon or the wrong text");
        }
        status.deviceStatus = false;
        require(content(Field::BatteryPercentOnly, status, false).empty(),
                "Percentage-only battery showed without device status");
    }
    edited.slotFields[5] = Field::BatteryPercentOnly;
    require(toJson(edited).value("bottom_right").toString()
                            == QStringLiteral("battery_percent_only")
                    && fromJson(toJson(edited), standard) == edited,
            "Percentage-only battery layout did not round-trip");
    require(field(QStringLiteral("battery_percent")) == Field::BatteryPercent,
            "The existing battery percentage choice changed");
    // Nickel's count: the page on screen is read and the result rounds down,
    // so page 6 of 20 is 30% and the last page is 100%.
    require(content(Field::ChapterPercent, values, false).text
                    == strings::get("common.percent").arg(30),
            "Percent of chapter is wrong");
    require(content(Field::BookPercent, values, false).text
                    == strings::get("common.percent").arg(3),
            "Percent of book is wrong");
    {
        auto last = values;
        last.page = last.pages;
        require(content(Field::BookPercent, last, false).text
                        == strings::get("common.percent").arg(100),
                "The last page is not 100%");
    }
    // Battery warning shows the battery only below its threshold.
    require(content(Field::BatteryWarning, values, false).empty(), "Battery warning showed at 80%");
    {
        auto low = values;
        low.battery = batteryWarningBelow - 1;
        const auto warning = content(Field::BatteryWarning, low, false);
        require(warning.batteryIcon
                        && warning.text
                                == strings::get("common.percent").arg(batteryWarningBelow - 1),
                "Battery warning did not show a low battery");
        low.battery = batteryWarningBelow;
        require(content(Field::BatteryWarning, low, false).empty(),
                "Battery warning showed at the threshold");
    }
    // Only the fields whose names do not explain them carry a description.
    for (const auto field : fields)
        require(fieldDescription(field).isEmpty()
                        == (field != Field::BatteryWarning && field != Field::ChapterTimeLeft
                            && field != Field::BookTimeLeft),
                "A field has the wrong description");
    // Every field is offered, each once.
    for (const auto field : fields)
        require(std::count(std::begin(choosableFields), std::end(choosableFields), field) == 1,
                "A field is not offered exactly once");
    require(content(Field::ChapterTimeLeft, values, false).empty(),
            "Time left showed without a chapter length");
    values.learningPace = true;
    require(content(Field::ChapterTimeLeft, values, false).text
                            == strings::get("header_footer.learning_pace")
                    && content(Field::BookTimeLeft, values, false).text
                            == strings::get("header_footer.learning_pace"),
            "Time left did not say the pace is still being learned");
    values.learningPace = false;
    values.chapterMilliseconds = 18 * 60000;
    values.bookMilliseconds = 4 * 3600000 + 5 * 60000;
    require(content(Field::ChapterTimeLeft, values, false).text
                    == strings::get("header_footer.time_left")
                               .arg(strings::get("stats.duration_minutes").arg(18)),
            "Time left in chapter is wrong");
    require(content(Field::BookTimeLeft, values, false).text
                    == strings::get("header_footer.time_left")
                               .arg(strings::get("stats.duration_hours").arg(4).arg(5)),
            "Time left in book is wrong");
    values.learningPace = true;
    require(content(Field::BookTimeLeft, values, false).text.startsWith(QStringLiteral("≈ ")),
            "A rough estimate was shown as a learned pace");
    values.approximateTime = true;

    struct Estimate
    {
        qint64 milliseconds;
        const char *duration;
    };

    for (const auto &estimate : { Estimate{ 1, "about 15 minutes" },
                                  { 15 * 60000, "about 15 minutes" },
                                  { 15 * 60000 + 1, "about 30 minutes" },
                                  { 30 * 60000, "about 30 minutes" },
                                  { 30 * 60000 + 1, "about 45 minutes" },
                                  { 45 * 60000, "about 45 minutes" },
                                  { 45 * 60000 + 1, "about an hour" },
                                  { 60 * 60000, "about an hour" },
                                  { 60 * 60000 + 1, "about 2 hours" },
                                  { 120 * 60000, "about 2 hours" },
                                  { 120 * 60000 + 1, "about 3 hours" } }) {
        values.chapterMilliseconds = values.bookMilliseconds = estimate.milliseconds;
        const auto duration = QString::fromLatin1(estimate.duration);
        require(remainingTime::approximateDuration(estimate.milliseconds) == duration,
                "Approximate duration crossed a rounding boundary incorrectly");
        for (const auto field : { Field::ChapterTimeLeft, Field::BookTimeLeft }) {
            const auto expected = strings::get("header_footer.time_left").arg(duration);
            require(content(field, values, false).text == expected,
                    "Approximate time was not applied to both chapter and book estimates");
            require(content(field, values, true).text == expected.toUpper(),
                    "Approximate time ignored uppercase or added a second learning marker");
        }
    }
    require(!remainingTime::approximateDuration(std::numeric_limits<qint64>::max()).isEmpty(),
            "A large estimate overflowed approximate formatting");
    values.chapterMilliseconds = values.bookMilliseconds = -1;
    require(content(Field::BookTimeLeft, values, false).text
                    == strings::get("header_footer.learning_pace"),
            "Approximate mode lost the unknown-pace message");
    values.learningPace = false;
    require(content(Field::BookTimeLeft, values, false).empty(),
            "Unknown approximate time became a numeric estimate");
    values.chapterMilliseconds = values.bookMilliseconds = 0;
    require(content(Field::ChapterTimeLeft, values, false).empty(),
            "A zero approximate estimate displayed time left");
    values.approximateTime = false;
    values.deviceStatus = false;
    require(content(Field::Battery, values, false).empty(),
            "The battery showed on a host without device status");
    require(content(Field::Time, values, false).text == values.time,
            "The time hid with device status");
    for (int slot = 0; slot < slotCount; ++slot)
        require(!slotLabel(slot).isEmpty(), "A slot has no name");
    for (const auto field : fields)
        require(!fieldLabel(field).startsWith(QStringLiteral("header_footer.")),
                "A field has no label");
    for (const auto value : presets)
        require(!presetLabel(value).startsWith(QStringLiteral("header_footer.")),
                "A preset has no label");
    return 0;
}
