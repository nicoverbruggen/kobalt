#include "reading/appearance_settings.h"
#include "support/qt_compat.h"
#include "check.h"
#include <QJsonArray>
#include <cstdio>
#include <utility>

using namespace kobalt;

int main() {
    // Appearance settings decide the layout. The list must stay complete, because a
    // missing key would let a retained layout survive a changed setting.
    require(appearance::keys.size() == 16, "The appearance keys changed without this check");

    const auto from = qt::object({{"font_pixels", 40},
                                  {"line_spacing", QJsonValue(QJsonValue::Null)},
                                  {"font_face", "Libron"},
                                  {"dark_mode", true},
                                  {"hide_nonlinear_fragments", true},
                                  {"show_header", false}});
    auto to =
        qt::object({{"font_pixels", 30}, {"margin_pixels", 12}, {"hyphenation", "on"}, {"page_animations", true}});
    appearance::copy(to, from);
    require(to.value("font_pixels").toInt() == 40 && to.value("font_face").toString() == "Libron" &&
                to.value("dark_mode").toBool() && to.value("hide_nonlinear_fragments").toBool(),
            "Copy lost an appearance value");
    // A null value stays null, and a key the source lacks is removed, so the
    // engine applies its own default instead of an old value.
    require(to.contains("line_spacing") && to.value("line_spacing").isNull(), "Copy changed a null value");
    require(!to.contains("margin_pixels") && !to.contains("hyphenation"), "Copy kept a value the source does not have");
    // Settings outside the appearance keys belong to other owners.
    require(to.value("page_animations").toBool() && !to.contains("show_header"),
            "Copy touched a setting it does not own");

    require(appearance::same(to, from), "Copied settings do not compare equal");
    for (const auto *key : {"top_margin_pixels", "bottom_margin_pixels"}) {
        auto vertical = from;
        vertical[key] = 36;
        require(!appearance::same(vertical, from), "Vertical margin changes reused an old layout");
        auto copied = from;
        appearance::copy(copied, vertical);
        require(copied.value(key).toInt() == 36, "Appearance copy lost a vertical margin");
    }
    auto bionic = from;
    bionic["bold_reading"] = QStringLiteral("bold");
    require(!appearance::same(bionic, from), "Bionic mode changes reused an old layout");
    auto bionicCopy = from;
    appearance::copy(bionicCopy, bionic);
    require(bionicCopy.value("bold_reading").toString() == "bold", "Appearance copy lost Bionic mode");
    auto optical = from;
    optical["font_optical_size"] = 12;
    require(!appearance::same(optical, from), "Optical size reused an old layout");
    auto axis = from;
    axis["font_weight"] = 450;
    require(!appearance::same(axis, from), "Variable weight reused an old layout");
    auto changed = to;
    changed["weight_steps"] = 1;
    require(!appearance::same(changed, from), "A changed key compared equal");
    auto missing = from;
    missing.remove("line_spacing");
    require(!appearance::same(missing, from), "A missing key compared equal to a null one");
    auto other = from;
    other["show_header"] = true;
    require(appearance::same(other, from), "A setting outside the keys changed the comparison");

    for (const auto &entry : {std::pair<int, int>{100, 100},
                              {120, 144},
                              {122, 150},
                              {140, 196},
                              {150, 225},
                              {173, 299},
                              {174, 300},
                              {180, 300}}) {
        auto legacy = qt::object({{"line_spacing", entry.first}, {"position", "chapter#saved"}});
        appearance::migrateLineSpacing(legacy);
        require(legacy.value("line_spacing").toInt() == entry.second &&
                    legacy.value("position").toString() == "chapter#saved",
                "Legacy spacing migration lost the approximate height or reading position");
        const auto migrated = legacy;
        appearance::migrateLineSpacing(legacy);
        require(legacy == migrated, "Spacing migration ran twice");
    }
    for (const auto &value : {QJsonValue(), QJsonValue(QJsonValue::Null), QJsonValue(QStringLiteral("invalid"))}) {
        auto saved = qt::object({{"line_spacing", value}});
        appearance::migrateLineSpacing(saved);
        require(saved.value("line_spacing") == value, "Missing or invalid spacing was converted to a number");
    }
    QJsonObject fresh;
    appearance::applyFontDefault(fresh, qt::object({{"default_font_face", "NV Ancizar Serif"}}));
    require(fresh["font_face"].toString() == "NV Ancizar Serif" &&
                fresh["default_font_face"].toString() == "NV Ancizar Serif",
            "New books did not use the custom default");
    fresh["font_face"] = QStringLiteral("");
    appearance::applyFontDefault(fresh, {});
    require(fresh["font_face"].toString().isEmpty() && fresh["default_font_face"].toString() == defaultFontFace,
            "Default changes replaced Publisher Fonts");
    fresh["font_face"] = QStringLiteral("NV Literata");
    appearance::applyFontDefault(fresh, qt::object({{"default_font_face", "NV Ancizar Serif"}}));
    require(fresh["font_face"].toString() == "NV Literata", "Default changes replaced an explicit book font");
    auto newDefault = from;
    newDefault["default_font_face"] = QStringLiteral("NV Ancizar Serif");
    require(!appearance::same(newDefault, from), "Default changes did not invalidate the layout");

    auto hidden = from;
    hidden["hide_nonlinear_fragments"] = false;
    require(!appearance::same(hidden, from), "Non-linear paging changes did not invalidate the retained model");

    const auto global = qt::object(
        {{"show_clock", true}, {"header_text_size", "small"}, {"bold_reading", "bold"}, {"page_animations", true}});
    const auto book = qt::object({{"show_clock", false}, {"header_text_size", "large"}, {"bold_reading", "off"}});
    const auto preferences = appearance::readerPreferences(global, book);
    require(!preferences["show_clock"].toBool() && preferences["header_text_size"].toString() == "large",
            "Book layout did not override legacy global settings");
    require(preferences["bold_reading"].toString() == "bold", "Book state overrode global Bionic reading");
    const auto persistedGlobal = appearance::globalPreferences(preferences);
    require(!persistedGlobal.contains("show_clock") && !persistedGlobal.contains("header_text_size") &&
                persistedGlobal["page_animations"].toBool(),
            "Global save mixed book layout with overflow settings");
    for (const auto *mode : {"bold", "off", "synthetic"}) {
        const auto legacy = qt::object({{"bionic_reading", mode}, {"page_animations", true}});
        const auto migrated = appearance::readerPreferences(legacy, book);
        require(migrated.value("bold_reading").toString() == mode && !migrated.contains("bionic_reading") &&
                    migrated.value("page_animations").toBool(),
                "Legacy global Bionic reading did not migrate to Bold reading");
        require(appearance::readerPreferences(migrated, book) == migrated,
                "Bold reading migration changed an existing choice");
        const auto persisted = appearance::globalPreferences(migrated);
        require(persisted.value("bold_reading").toString() == mode && !persisted.contains("bionic_reading"),
                "Global save lost migrated Bold reading or kept the legacy key");
    }
    for (const auto &value : {QJsonValue(QStringLiteral("bold")), QJsonValue(QStringLiteral("off")),
                              QJsonValue(QJsonValue::Null), QJsonValue(QStringLiteral("invalid"))}) {
        const auto legacyMode = value.toString() == "bold" ? "off" : "bold";
        const auto existing = qt::object({{"bold_reading", value}, {"bionic_reading", legacyMode}});
        const auto migrated = appearance::readerPreferences(existing, book);
        require(migrated.value("bold_reading") == value && !migrated.contains("bionic_reading"),
                "Legacy Bionic reading replaced an explicit Bold reading choice");
    }
    // A book without a header and footer of its own starts with the last preset;
    // a book with one, even from before presets, keeps it.
    const auto last = qt::object({{appearance::lastPresetKey, "alternative_b"}});
    require(appearance::readerPreferences(last, {}).value("header_footer").toString() == "alternative_b" &&
                appearance::readerPreferences(last, qt::object({{"orientation", "portrait"}}))
                        .value("header_footer")
                        .toString() == "alternative_b",
            "A new book did not start with the last preset");
    require(appearance::readerPreferences(last, qt::object({{"header_footer", "default"}}))
                        .value("header_footer")
                        .toString() == "default" &&
                !appearance::readerPreferences(last, qt::object({{"show_clock", false}})).contains("header_footer") &&
                !appearance::readerPreferences({}, {}).contains("header_footer"),
            "The last preset replaced a book's own header and footer");
    const auto custom = qt::object({{"header_footer", "custom"}, {appearance::lastPresetKey, "default"}});
    require(appearance::globalPreferences(custom) == qt::object({{appearance::lastPresetKey, "custom"}}),
            "A global save did not record the book's preset as the last one");
    require(appearance::globalPreferences(qt::object({{"header_footer", "default"}})).isEmpty(),
            "A profile that only uses Kobalt saved a last preset");
    require(!appearance::readerPreferences({}, qt::object({{"bionic_reading", "bold"}})).contains("bold_reading") &&
                !appearance::readerPreferences({}, {}).contains("bold_reading"),
            "A book-specific legacy setting or a fresh profile enabled global Bold reading");
    for (const auto density : {212, 227, 265, 300}) {
        readingPpi.store(density);
        require(defaultFontPixels() == halfPointPixels(22), "Default text size is not 11 points");
        require(minimumFontPixels() == halfPointPixels(14) && maximumFontPixels() == halfPointPixels(96),
                "Engine size limits do not match the 7 to 48 point controls");
    }
    readingPpi.store(300);
    auto inherited = appearance::typographyDefaults();
    inherited["font_face"] = QStringLiteral("NV Literata");
    inherited["font_pixels"] = 60;
    inherited["line_spacing"] = 175;
    inherited["margin_pixels"] = 60;
    inherited["top_margin_pixels"] = 30;
    inherited["bottom_margin_pixels"] = 36;
    inherited["text_alignment"] = QStringLiteral("justify");
    inherited["font_weight"] = 550;
    inherited["weight_steps"] = 2;
    inherited["font_optical_size"] = 12;
    inherited["hyphenation"] = QStringLiteral("on");
    inherited["position"] = QStringLiteral("chapter#last");
    inherited["dark_mode"] = true;
    inherited["header_footer"] = QStringLiteral("alternative_a");
    QJsonObject nextBook;
    appearance::rememberTextLayout(nextBook, inherited);
    require(nextBook.value(appearance::lastTextLayoutKey).toObject().size() == 13,
            "Last text and layout included unrelated book state");
    auto initial = appearance::bookDefaults(nextBook);
    for (const auto *key : appearance::typographyKeys) {
        require(initial.value(key) == inherited.value(key), "New book lost inherited typography or margins");
    }
    require(!initial.contains("position") && !initial.contains("dark_mode") && !initial.contains("header_footer"),
            "New book inherited unrelated book state");
    QJsonObject blank;
    appearance::applyBookDefaults(blank, initial);
    require(blank.value("font_face") == inherited.value("font_face"), "Empty saved state ignored inherited font");
    auto own = qt::object({{"font_pixels", 40}, {"position", "chapter#own"}});
    appearance::applyBookDefaults(own, initial);
    require(own.value("font_pixels").toInt() == 40 && !own.contains("margin_pixels") &&
                own.value("position").toString() == "chapter#own",
            "Inheritance overwrote a book with partial saved typography");
    nextBook[appearance::inheritTextLayoutKey] = false;
    auto fixed = appearance::bookDefaults(nextBook);
    require(fixed.value("font_pixels").toInt() == defaultFontPixels() &&
                fixed.value("font_face").toString() == defaultFontFace,
            "Disabling text inheritance did not restore new-book defaults");
    nextBook["default_font_face"] = QStringLiteral("NV Ancizar Serif");
    require(appearance::bookDefaults(nextBook).value("font_face").toString() == "NV Ancizar Serif",
            "Disabling inheritance ignored the user's default font");
    nextBook[appearance::inheritTextLayoutKey] = true;
    readingPpi.store(212);
    initial = appearance::bookDefaults(nextBook);
    require(initial.value("font_pixels").toInt() == 42 && initial.value("margin_pixels").toInt() == 42 &&
                initial.value("top_margin_pixels").toInt() == 21 &&
                initial.value("bottom_margin_pixels").toInt() == 25 && initial.value("line_spacing").toInt() == 175,
            "Inherited pixel values did not follow the screen density");
    readingPpi.store(300);
    auto disabledChrome = last;
    disabledChrome[appearance::inheritHeaderFooterKey] = false;
    require(appearance::readerPreferences(disabledChrome, {}).value("header_footer").toString() == "default" &&
                appearance::readerPreferences(disabledChrome, qt::object({{"header_footer", "alternative_a"}}))
                        .value("header_footer")
                        .toString() == "alternative_a",
            "Disabling header/footer inheritance changed a saved book or kept new books on the last preset");
    const auto retired = qt::object({{"smooth_images", false}, {"heavier_interface_text", false}});
    require(appearance::globalPreferences(retired).isEmpty() && appearance::readerPreferences(retired, {}).isEmpty(),
            "Retired settings survived global preference loading or saving");
    const auto defaults = appearance::typographyDefaults();
    require(defaults.value("font_face").toString() == defaultFontFace &&
                defaults.value("font_pixels").toInt() == defaultFontPixels() &&
                defaults.value("line_spacing").toInt() == defaultLineSpacing &&
                defaults.value("margin_pixels").toInt() == defaultMarginPixels() &&
                defaults.value("top_margin_pixels").toInt() == 24 &&
                defaults.value("bottom_margin_pixels").toInt() == 24 &&
                defaults.value("text_alignment").toString() == "left" && defaults.value("weight_steps").toInt() == 0 &&
                defaults.value("font_weight").toInt() == 450 && defaults.value("hyphenation").toString() == "off",
            "Typography defaults changed");
    require(!defaults.contains("dark_mode") && !defaults.contains("smooth_images") &&
                !defaults.contains("bold_reading") && defaults.size() == 12,
            "Restoring typography must not change display choices");
    readingPpi.store(212);
    require(appearance::typographyDefaults().value("font_pixels").toInt() == defaultFontPixels(),
            "Default text size did not follow the screen density");
    readingPpi.store(300);
    const auto benchmark = appearance::benchmarkTypography("file:///mnt/onboard/kobalt-benchmark.kepub.epub");
    require(benchmark.size() == 15 && benchmark.value("font_face").toString().isEmpty() &&
                benchmark.value("font_pixels").toInt() == defaultFontPixels() &&
                benchmark.value("margin_pixels").toInt() == 60 && benchmark.value("top_margin_pixels").toInt() == 60 &&
                benchmark.value("line_spacing").toInt() == 144 && !benchmark.value("dark_mode").toBool() &&
                benchmark.value("hyphenation").toString() == "off",
            "Benchmark typography changed");
    require(appearance::benchmarkTypography("file:///mnt/onboard/other.epub").isEmpty() &&
                appearance::benchmarkTypography("file:///mnt/onboard/KOBALT-benchmark.kepub.epub").isEmpty(),
            "Benchmark profile matched another book");
    readingPpi.store(212);
    require(appearance::benchmarkTypography("file:///mnt/onboard/kobalt-benchmark.kepub.epub")
                    .value("font_pixels")
                    .toInt() == defaultFontPixels(),
            "Benchmark profile did not follow screen density");
    std::puts("PASS: appearance keys, copy, comparison and typography defaults");
    return 0;
}
