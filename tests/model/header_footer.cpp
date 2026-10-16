#include "reading/header_footer.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "check.h"

#include <algorithm>
#include <iterator>

using namespace kobalt;
using namespace kobalt::headerfooter;

int main() {
    // Default draws what the reader drew before presets existed.
    const auto standard = presetLayout(Preset::Default);
    require(standard.slotFields[0] == Field::Time && standard.slotFields[1] == Field::Chapter &&
                standard.slotFields[2] == Field::ChapterPage && standard.slotFields[4] == Field::BookPage &&
                standard.slotFields[3] == Field::Nothing && standard.slotFields[5] == Field::Nothing,
            "Default does not keep the clock, chapter and positions");
    require(standard.uppercase[1] && standard.progressBar && standard.headerSize == "small" &&
                standard.footerSize == "small",
            "Default changed its case, bar or sizes");
    const auto a = presetLayout(Preset::AlternativeA);
    require(a.slotFields[0] == Field::Time && a.slotFields[1] == Field::ChapterAndPage &&
                a.slotFields[4] == Field::TitleAndPage && a.uppercase[1] && a.uppercase[4] && a.progressBar,
            "Alternative A does not centre a name and a position twice");
    const auto b = presetLayout(Preset::AlternativeB);
    require(b.slotFields[1] == Field::Time && b.slotFields[3] == Field::ChapterPagesLeft && !b.uppercase[3] &&
                !b.progressBar,
            "Alternative B is not the quiet preset");

    // Values round-trip, and unknown values fall back instead of failing.
    for (const auto field : fields)
        require(headerfooter::field(fieldValue(field)) == field, "A field did not round-trip");
    for (const auto value : presets)
        require(preset(presetValue(value)) == value, "A preset did not round-trip");
    require(preset("missing") == Preset::Default && headerfooter::field("missing") == Field::Nothing,
            "Unknown values did not fall back");
    auto edited = a;
    edited.slotFields[5] = Field::Battery;
    edited.headerSize = QStringLiteral("off");
    edited.uppercase[2] = false;
    require(fromJson(toJson(edited), standard) == edited, "A layout did not round-trip");
    auto lower = fromJson(qt::object({{"uppercase", false}}), standard);
    require(!lower.uppercase[0] && !lower.uppercase[5], "A single uppercase value did not apply to every slot");
    const auto partial = fromJson(qt::object({{"top_left", "author"}, {"footer_size", "huge"}}), standard);
    require(partial.slotFields[0] == Field::Author && partial.slotFields[1] == Field::Chapter &&
                partial.footerSize == "small",
            "Missing or invalid layout values did not keep the fallback");

    // A strip with nothing in it takes no space, even when its size is set.
    auto empty = standard;
    empty.slotFields[3] = empty.slotFields[4] = empty.slotFields[5] = Field::Nothing;
    require(empty.header() && !empty.footer(), "An empty footer kept its strip");
    empty.headerSize = QStringLiteral("off");
    require(!empty.header(), "Off kept the header");

    // The preset is chosen per book; Custom is one layout for every book.
    const auto custom = toJson(edited);
    auto selection = select(qt::object({{presetKey, "alternative_b"}, {customKey, custom}}));
    require(selection.preset == Preset::AlternativeB && selection.layout == b, "A saved preset was not used");
    selection = select(qt::object({{presetKey, "custom"}, {customKey, custom}}));
    require(selection.preset == Preset::Custom && selection.layout == edited, "Custom did not use the saved layout");
    selection = select(qt::object({{presetKey, "custom"}}));
    require(selection.preset == Preset::Custom && selection.layout == standard,
            "Custom without a saved layout did not start from Default");

    // Books from before presets keep their old header and footer.
    selection = select({});
    require(selection.preset == Preset::Default && selection.layout == standard,
            "Old default settings did not select Default");
    selection = select(qt::object({{"show_clock", false}, {"footer_text_size", "large"}}));
    require(selection.preset == Preset::Custom && selection.layout.slotFields[0] == Field::Nothing &&
                selection.layout.slotFields[1] == Field::Chapter && selection.layout.footerSize == "large",
            "Old custom settings were not kept as Custom");
    selection = select(qt::object({{"show_header", false}, {"show_footer", false}, {"show_progress_bar", false}}));
    require(selection.layout.slotFields[0] == Field::Time && selection.layout.slotFields[1] == Field::Nothing &&
                selection.layout.headerSize == "small" && selection.layout.footerSize == "off" &&
                !selection.layout.progressBar,
            "The old clock without a header was lost");
    selection = select(qt::object({{"show_header", false}, {"show_clock", false}}));
    require(!selection.layout.header(), "An old hidden header and clock came back");
    selection = select(qt::object({{"show_clock", false}, {customKey, custom}}));
    require(selection.preset == Preset::Custom && selection.layout == edited,
            "An old custom book replaced the saved custom layout");

    Values values;
    values.title = QStringLiteral("Alice");
    values.author = QStringLiteral("Lewis Carroll");
    values.chapter = QStringLiteral("Down the Rabbit-Hole");
    values.time = QStringLiteral("9:00");
    values.chapterPage = 6;
    values.chapterPages = 20;
    values.page = 6;
    values.pages = 193;
    values.battery = 80;
    const auto chapterPage = strings::get("common.page_fraction").arg(6).arg(20);
    require(content(Field::ChapterPage, values, false).text == chapterPage, "Page in chapter is wrong");
    // Beside a name the page reads "6 of 20"; alone it keeps the slash.
    require(content(Field::ChapterAndPage, values, false).text ==
                strings::get("reader.chapter_position").arg(values.chapter, QStringLiteral("6 of 20")),
            "Chapter and page is wrong");
    require(content(Field::TitleAndPage, values, false).text.endsWith(QStringLiteral(" 6 of 193")),
            "Title and page is wrong");
    require(pageCount(Field::ChapterPage) && pageCount(Field::BookPage) && !pageCount(Field::ChapterAndPage),
            "Only lone page counts keep half spaces");
    require(content(Field::Chapter, values, true).text == QStringLiteral("DOWN THE RABBIT-HOLE"),
            "Uppercase did not apply");
    require(content(Field::ChapterPagesLeft, values, false).text.startsWith(QStringLiteral("14 ")),
            "Pages left counts the current page");
    const auto battery = content(Field::Battery, values, false);
    require(battery.batteryIcon && battery.text.isEmpty() && battery.battery == 80, "Battery is not an icon");
    require(content(Field::BatteryPercent, values, false).text == strings::get("common.percent").arg(80),
            "Battery percentage is missing");
    // Percent done counts the page being read; the last page is 100%.
    require(content(Field::ChapterPercent, values, false).text == strings::get("common.percent").arg(30),
            "Percent of chapter is wrong");
    require(content(Field::BookPercent, values, false).text == strings::get("common.percent").arg(3),
            "Percent of book is wrong");
    {
        auto last = values;
        last.page = last.pages;
        require(content(Field::BookPercent, last, false).text == strings::get("common.percent").arg(100),
                "The last page is not 100%");
    }
    // Battery warning shows the battery only below its threshold.
    require(content(Field::BatteryWarning, values, false).empty(), "Battery warning showed at 80%");
    {
        auto low = values;
        low.battery = batteryWarningBelow - 1;
        const auto warning = content(Field::BatteryWarning, low, false);
        require(warning.batteryIcon && warning.text == strings::get("common.percent").arg(batteryWarningBelow - 1),
                "Battery warning did not show a low battery");
        low.battery = batteryWarningBelow;
        require(content(Field::BatteryWarning, low, false).empty(), "Battery warning showed at the threshold");
    }
    // Only the fields whose names do not explain them carry a description.
    for (const auto field : fields)
        require(fieldDescription(field).isEmpty() ==
                    (field != Field::BatteryWarning && field != Field::ChapterTimeLeft && field != Field::BookTimeLeft),
                "A field has the wrong description");
    // Every field is offered, each once.
    for (const auto field : fields)
        require(std::count(std::begin(choosableFields), std::end(choosableFields), field) == 1,
                "A field is not offered exactly once");
    require(content(Field::ChapterTimeLeft, values, false).empty(), "Time left showed without a chapter length");
    values.learningPace = true;
    require(content(Field::ChapterTimeLeft, values, false).text == strings::get("header_footer.learning_pace") &&
                content(Field::BookTimeLeft, values, false).text == strings::get("header_footer.learning_pace"),
            "Time left did not say the pace is still being learned");
    values.learningPace = false;
    values.chapterMilliseconds = 18 * 60000;
    values.bookMilliseconds = 4 * 3600000 + 5 * 60000;
    require(content(Field::ChapterTimeLeft, values, false).text ==
                strings::get("header_footer.time_left").arg(strings::get("stats.duration_minutes").arg(18)),
            "Time left in chapter is wrong");
    require(content(Field::BookTimeLeft, values, false).text ==
                strings::get("header_footer.time_left").arg(strings::get("stats.duration_hours").arg(4).arg(5)),
            "Time left in book is wrong");
    values.deviceStatus = false;
    require(content(Field::Battery, values, false).empty(), "The battery showed on a host without device status");
    require(content(Field::Time, values, false).text == values.time, "The time hid with device status");
    for (int slot = 0; slot < slotCount; ++slot)
        require(!slotLabel(slot).isEmpty(), "A slot has no name");
    for (const auto field : fields)
        require(!fieldLabel(field).startsWith(QStringLiteral("header_footer.")), "A field has no label");
    for (const auto value : presets)
        require(!presetLabel(value).startsWith(QStringLiteral("header_footer.")), "A preset has no label");
    return 0;
}
