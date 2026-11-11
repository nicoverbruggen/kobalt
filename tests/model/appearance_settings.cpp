#include "reading/appearance_settings.h"
#include "reading/session_timeout.h"
#include "reading/remaining_time.h"
#include "support/qt_compat.h"
#include "check.h"
#include <QJsonArray>
#include <cstdio>
#include <utility>

using namespace kobalt;

// Checks appearance settings: defaults, invalid values, migrations, global and book precedence, new-book inheritance and which changes need a new layout.
int main()
{
    {
        const auto original = qt::object({ { "font_pixels", 40 },
                                           { "line_spacing", 150 },
                                           { "position", "chapter-one" },
                                           { "unknown", 7 } });
        const auto request = appearance::Requested::fromResolvedState(original);
        const auto changed = request.edited(appearance::Patch(qt::object(
                { { "font_pixels", 45 }, { "position", "chapter-two" }, { "unknown", 9 } })));
        require(changed.fields().value("font_pixels").toInt() == 45
                        && changed.fields().value("line_spacing").toInt() == 150
                        && request.fields().value("font_pixels").toInt() == 40,
                "A partial appearance edit lost another requested field or changed its source");
        auto checkpoint = original;
        changed.replaceIn(checkpoint);
        require(checkpoint.value("position").toString() == "chapter-one"
                        && checkpoint.value("unknown").toInt() == 7,
                "Appearance edits must not carry positions or unrelated record fields");
        const appearance::SavedInput saved(qt::object(
                { { "font_pixels", QJsonValue(QJsonValue::Null) }, { "position", "stored" } }));
        saved.restoreIn(checkpoint);
        require(checkpoint.contains("font_pixels") && checkpoint.value("font_pixels").isNull()
                        && !checkpoint.contains("line_spacing")
                        && checkpoint.value("position").toString() == "chapter-one"
                        && checkpoint.value("unknown").toInt() == 7,
                "Persisted appearance must preserve null and absent keys without moving the book");
    }
    for (const auto *mode : { "current", "all", "off" }) {
        const auto cleaned = appearance::cleanPreferences(
                { { "chapter_groups", mode }, { "future_option", 42 } });
        require(cleaned.value("chapter_groups").toString() == mode
                        && cleaned.value("future_option").toInt() == 42,
                "Chapter group cleanup lost a saved choice or an unknown field");
        require(appearance::same({}, cleaned), "Chapter expansion invalidated the book layout");
    }
    for (const auto &invalid : { QJsonValue("invalid"), QJsonValue(true), QJsonValue(1) })
        require(!appearance::cleanPreferences({ { "chapter_groups", invalid } })
                         .contains("chapter_groups"),
                "Invalid chapter group choice did not fall back to Current section");
    require(!remainingTime::approximate({}), "Older preferences did not retain detailed estimates");
    for (const auto *format : { "detailed", "approximate" }) {
        const auto preferences = appearance::cleanPreferences(
                { { remainingTime::preferenceKey, format }, { "future_option", 42 } });
        require(preferences.value(remainingTime::preferenceKey).toString() == format
                        && preferences.value("future_option").toInt() == 42,
                "Time format cleanup lost saved preferences");
        require(remainingTime::approximate(preferences) == (QString(format) == "approximate"),
                "Time format loaded incorrectly");
        require(appearance::same({}, preferences), "Time format invalidated the book layout");
    }
    for (const auto &invalid :
         { QJsonValue("unknown"), QJsonValue(true), QJsonValue(1), QJsonValue() }) {
        const auto preferences =
                appearance::cleanPreferences({ { remainingTime::preferenceKey, invalid } });
        require(!preferences.contains(remainingTime::preferenceKey)
                        && !remainingTime::approximate(preferences),
                "Invalid time format did not fall back to detailed estimates");
    }

    for (bool shown : { false, true }) {
        const auto preferences = appearance::cleanPreferences(
                { { "light_dark_mode_shortcut", shown }, { "optional", "keep" } });
        require(preferences.value("light_dark_mode_shortcut").toBool() == shown
                        && preferences.value("optional") == "keep",
                "Light shortcut cleanup lost a saved choice or optional field");
        require(appearance::same({}, preferences),
                "Light shortcut visibility invalidated the book layout");
    }
    for (const auto &invalid : { QJsonValue("true"), QJsonValue(1), QJsonValue() })
        require(!appearance::cleanPreferences({ { "light_dark_mode_shortcut", invalid } })
                         .value("light_dark_mode_shortcut")
                         .toBool(false),
                "Invalid Light shortcut preference did not default to hidden");
    require(!appearance::cleanPreferences({}).value("light_dark_mode_shortcut").toBool(false),
            "Older preferences enabled the Light shortcut");

    require(sessionTimeout::minutes({}) == 15,
            "Older preferences lost the default session timeout");
    for (const auto minutes : sessionTimeout::choices) {
        const auto preferences = appearance::cleanPreferences(
                { { sessionTimeout::preferenceKey, minutes }, { "optional", "keep" } });
        require(sessionTimeout::minutes(preferences) == minutes
                        && preferences.value("optional").toString() == "keep",
                "Session timeout cleanup lost a supported choice or an optional field");
        require(sessionTimeout::milliseconds(preferences) == qint64(minutes) * 60000,
                "Session timeout did not convert minutes safely");
        require(appearance::same({}, preferences), "Session timeout invalidated the book layout");
    }
    for (const auto &invalid : { QJsonValue(-1), QJsonValue(16), QJsonValue(15.5), QJsonValue("60"),
                                 QJsonValue(true), QJsonValue() }) {
        const auto preferences =
                appearance::cleanPreferences({ { sessionTimeout::preferenceKey, invalid } });
        require(!preferences.contains(sessionTimeout::preferenceKey)
                        && sessionTimeout::minutes(preferences) == 15,
                "Invalid session timeout did not fall back to fifteen minutes");
    }
    for (bool hidden : { false, true }) {
        const auto cleaned = appearance::cleanPreferences(
                { { "native_hide_status_bar", hidden }, { "private_optional", "keep" } });
        require(!cleaned.contains("native_hide_status_bar")
                        && cleaned.value("private_optional").toString() == "keep",
                "Retired desktop status preference survived cleanup or removed an unrelated setting");
    }
    require(!appearance::cleanPreferences({}).contains("tappable_links"),
            "Older settings gained an explicit direct-tap choice");
    for (const auto *mode : { "off", "footnotes", "all" })
        require(appearance::cleanPreferences({ { "tappable_links", mode } })
                                .value("tappable_links")
                                .toString()
                        == mode,
                "Cleaning settings lost a supported direct-tap choice");
    for (const auto &invalid : { QJsonValue("unknown"), QJsonValue(true), QJsonValue(1) })
        require(!appearance::cleanPreferences({ { "tappable_links", invalid } })
                         .contains("tappable_links"),
                "Invalid direct-tap preference did not fall back to the default");

    for (bool show : { false, true }) {
        const auto cleaned = appearance::cleanPreferences({ { "schema", 1 },
                                                            { "show_light_gesture_feedback", show },
                                                            { "edge_light_gestures", true },
                                                            { "future_option", 42 } });
        require(cleaned.value("show_light_gesture_feedback").toBool() == show
                        && cleaned.value("edge_light_gestures").toBool()
                        && cleaned.value("future_option").toInt() == 42
                        && cleaned.value("schema").toInt() == 1,
                "Light feedback cleanup changed independent preferences or schema");
        require(appearance::same({}, cleaned), "Light feedback invalidated the book layout");
    }
    require(appearance::cleanPreferences({}).value("show_light_gesture_feedback").toBool(true),
            "Old preferences no longer default to visible feedback");
    for (const auto &invalid :
         { QJsonValue(), QJsonValue(0), QJsonValue("false"), QJsonValue(QJsonArray{}) })
        require(appearance::cleanPreferences({ { "show_light_gesture_feedback", invalid } })
                        .value("show_light_gesture_feedback")
                        .toBool(true),
                "Invalid feedback preference did not use visible feedback");

    const auto dictionary =
            appearance::cleanPreferences(qt::object({ { "dictionary_font_face", "Libron" },
                                                      { "dictionary_font_size", 120 },
                                                      { "private_optional", "keep" } }));
    require(dictionary.value("dictionary_font_face").toString() == "Libron"
                    && dictionary.value("dictionary_font_size").toInt() == 120
                    && dictionary.value("private_optional").toString() == "keep",
            "Dictionary preferences lost saved values");
    for (const auto &size : { QJsonValue(0), QJsonValue(110), QJsonValue(120.5),
                              QJsonValue(QStringLiteral("120")), QJsonValue(true) })
        require(!appearance::cleanPreferences(qt::object({ { "dictionary_font_size", size } }))
                         .contains("dictionary_font_size"),
                "Invalid dictionary size survived preference cleanup");
    require(!appearance::cleanPreferences(qt::object({ { "dictionary_font_face", 12 } }))
                     .contains("dictionary_font_face"),
            "Invalid dictionary font survived preference cleanup");
    require(appearance::same({}, dictionary),
            "Dictionary UI preferences invalidated the book layout");
    // Appearance settings decide the layout. The list must stay complete, because a
    // missing key would let a retained layout survive a changed setting.
    require(appearance::keys.size() == 36, "The appearance keys changed without this check");
    require(appearance::bookDefaults({}).value("paragraph_indent").toInt() == -1,
            "Paragraph indentation no longer defaults to Publisher");
    for (int percent : { -1, 0, 100, 300 }) {
        const auto defaults = appearance::bookDefaults(
                { { "paragraph_indent", percent }, { "inherit_text_layout", false } });
        QJsonObject book{ { "font_pixels", 40 }, { "paragraph_indent", 200 } };
        appearance::applyBookDefaults(book, defaults);
        require(book.value("paragraph_indent").toInt(-2) == percent,
                "A saved book replaced the global paragraph indentation");
        QJsonObject remembered;
        appearance::rememberTextLayout(remembered, book);
        require(!remembered.value(appearance::lastTextLayoutKey)
                         .toObject()
                         .contains("paragraph_indent"),
                "Global paragraph indentation entered text inheritance");
    }
    for (const auto &invalid : { QJsonValue(), QJsonValue(true), QJsonValue("100"), QJsonValue(-2),
                                 QJsonValue(301), QJsonValue(1.5) })
        require(appearance::paragraphIndent(invalid) == -1,
                "Invalid paragraph indentation did not use Publisher");
    require(!appearance::same({ { "paragraph_indent", 0 } }, { { "paragraph_indent", 100 } }),
            "Paragraph indentation reused a stale layout");
    require(appearance::bookDefaults({}).value("paragraph_spacing").toInt() == -1,
            "Paragraph spacing no longer defaults to Publisher");
    for (int percent : { -1, 0, 50, 200 }) {
        const auto global = appearance::bookDefaults(
                { { "paragraph_spacing", percent }, { "inherit_text_layout", false } });
        QJsonObject book{ { "font_pixels", 40 }, { "paragraph_spacing", 100 } };
        appearance::applyBookDefaults(book, global);
        require(book.value("paragraph_spacing").toInt(-2) == percent,
                "A book-specific gap replaced the global paragraph spacing");
        QJsonObject remembered;
        appearance::rememberTextLayout(remembered, book);
        require(!remembered.value(appearance::lastTextLayoutKey)
                         .toObject()
                         .contains("paragraph_spacing"),
                "Global paragraph spacing entered text inheritance");
    }
    for (const auto &invalid :
         { QJsonValue(), QJsonValue(true), QJsonValue("50"), QJsonValue(201), QJsonValue(0.5) })
        require(appearance::paragraphSpacing(invalid) == -1,
                "Invalid paragraph spacing did not use Publisher");
    require(!appearance::same({ { "paragraph_spacing", 0 } }, { { "paragraph_spacing", 50 } }),
            "Paragraph gap reused a stale layout");

    const QJsonObject fillOn{ { "vertical_justification", "expand" } },
            fillOff{ { "vertical_justification", "off" } };
    require(!appearance::same(fillOn, fillOff), "Vertical fill reused a retained layout");
    auto copiedFill = fillOff;
    appearance::copy(copiedFill, fillOn);
    require(copiedFill.value("vertical_justification").toString() == "expand",
            "Preview copy lost vertical fill");
    const auto fill = appearance::bookDefaults({ { "vertical_justification", true } });
    auto oldBook = QJsonObject{ { "vertical_justification", false } };
    appearance::applyBookDefaults(oldBook, fill);
    require(oldBook.value("vertical_justification").toString() == "expand",
            "global vertical fill was lost to book settings");
    appearance::applyBookDefaults(oldBook, appearance::bookDefaults({}));
    require(oldBook.value("vertical_justification").toString() == "off",
            "missing vertical fill did not default off");
    const auto invalidFill = appearance::cleanPreferences(
            { { "vertical_justification", "yes" }, { "future_option", 42 } });
    require(invalidFill.value("vertical_justification").toString() == "off"
                    && invalidFill.value("future_option").toInt() == 42,
            "vertical fill validation changed unknown preferences");
    for (const auto &mode : { QString("off"), QString("expand"), QString("center") }) {
        const auto cleaned = appearance::cleanPreferences(
                { { "vertical_justification", mode }, { "vertical_spacing_scope", "protection" } });
        require(cleaned.value("vertical_justification").toString() == mode
                        && cleaned.value("vertical_spacing_scope").toString() == "protection",
                "Vertical mode or scope was lost");
    }
    require(appearance::verticalSpacingMode(true) == "expand"
                    && appearance::verticalSpacingMode(false) == "off",
            "Legacy Boolean page filling did not migrate");
    require(appearance::verticalSpacingScope("invalid") == "all",
            "Invalid spacing scope did not default to all pages");
    require(!appearance::same({ { "vertical_spacing_scope", "all" } },
                              { { "vertical_spacing_scope", "protection" } }),
            "Changing the spacing scope reused an old appearance");
    const auto from = qt::object({ { "font_pixels", 40 },
                                   { "line_spacing", QJsonValue(QJsonValue::Null) },
                                   { "font_face", "Libron" },
                                   { "dark_mode", true },
                                   { "hide_nonlinear_fragments", true },
                                   { "normalize_page_margins", false },
                                   { "show_header", false } });
    auto to = qt::object({ { "font_pixels", 30 },
                           { "margin_pixels", 12 },
                           { "hyphenation", "on" },
                           { "page_animations", true } });
    appearance::copy(to, from);
    require(to.value("font_pixels").toInt() == 40 && to.value("font_face").toString() == "Libron"
                    && to.value("dark_mode").toBool()
                    && to.value("hide_nonlinear_fragments").toBool()
                    && to.contains("normalize_page_margins")
                    && !to.value("normalize_page_margins").toBool(),
            "Copy lost an appearance value");
    // A null value stays null, and a key the source lacks is removed, so the
    // engine applies its own default instead of an old value.
    require(to.contains("line_spacing") && to.value("line_spacing").isNull(),
            "Copy changed a null value");
    require(!to.contains("margin_pixels") && !to.contains("hyphenation"),
            "Copy kept a value the source does not have");
    // Settings outside the appearance keys belong to other owners.
    require(to.value("page_animations").toBool() && !to.contains("show_header"),
            "Copy touched a setting it does not own");

    require(appearance::same(to, from), "Copied settings do not compare equal");
    for (bool enabled : { false, true }) {
        const auto global = appearance::cleanPreferences({ { "book_beautifier", enabled },
                                                           { "beautifier_initials", true },
                                                           { "beautifier_spacing", false },
                                                           { "beautifier_indent", false } });
        QJsonObject stale{ { "book_beautifier", !enabled }, { "beautifier_spacing", true } };
        appearance::applyBookDefaults(stale, appearance::bookDefaults(global));
        require(!stale.contains("book_beautifier")
                        && stale.value("beautifier_initials").toBool() == enabled
                        && !stale.value("beautifier_spacing").toBool()
                        && !stale.value("beautifier_indent").toBool(),
                "Global beautifier did not replace a stale book snapshot");
    }
    const auto directRules = appearance::bookDefaults({ { "beautifier_initials", true } });
    require(directRules.value("beautifier_initials").toBool()
                    && !directRules.value("beautifier_spacing").toBool()
                    && !directRules.value("beautifier_indent").toBool(),
            "A checked rule required the retired master switch");
    for (const auto *key : appearance::beautifierKeys)
        require(!appearance::bookDefaults({}).value(key).toBool(), "A rule was enabled by default");
    const auto invalidBeautifier = appearance::beautifierSettings(appearance::cleanPreferences(
            { { "book_beautifier", "yes" }, { "beautifier_initials", "no" } }));
    require(!invalidBeautifier.value("book_beautifier").toBool()
                    && !invalidBeautifier.value("beautifier_initials").toBool(),
            "Invalid beautifier settings did not use safe defaults");
    const auto recommended = appearance::normalizationPresetSettings("recommended");
    const auto full = appearance::normalizationPresetSettings("full");
    require(appearance::normalizationPreset({}) == "recommended"
                    && appearance::normalizationSettings({})
                            == appearance::normalizationSettings(recommended),
            "Recommended changed the existing defaults");
    for (const auto *key : appearance::normalizationKeys)
        require(full.value(key).toBool(), "Full did not enable every rule");
    for (bool enabled : { false, true }) {
        const auto migrated = appearance::cleanPreferences({ { "avoid_wasteful_styling", enabled },
                                                             { "beautifier_spacing", true },
                                                             { "unknown_preference", 17 } });
        require(!migrated.contains("avoid_wasteful_styling")
                        && migrated.value("normalize_page_margins").toBool() == enabled
                        && migrated.value("reduce_chapter_spacing").toBool() == enabled
                        && migrated.value("beautifier_spacing").toBool()
                        && migrated.value("unknown_preference").toInt() == 17
                        && appearance::normalizationPreset(migrated) == "custom",
                "Legacy styling choices were not preserved");
        require(appearance::cleanPreferences(migrated) == migrated,
                "Normalization migration was not stable");
    }
    const auto split = appearance::cleanPreferences(
            { { "avoid_wasteful_styling", false }, { "normalize_page_margins", true } });
    require(split.value("normalize_page_margins").toBool()
                    && !split.value("reduce_chapter_spacing").toBool(),
            "An explicit margin rule was overwritten by the legacy switch");
    auto normalizationCustom = recommended;
    normalizationCustom[appearance::normalizationPresetKey] = "custom";
    require(appearance::normalizationPreset(appearance::cleanPreferences(normalizationCustom))
                    == "custom",
            "Explicit Custom was replaced by a matching preset");
    auto empty = normalizationCustom;
    for (const auto *key : appearance::normalizationKeys)
        empty[key] = false;
    QJsonObject staleRules = full;
    appearance::applyBookDefaults(staleRules, appearance::bookDefaults(empty));
    for (const auto *key : appearance::normalizationKeys)
        require(!staleRules.value(key).toBool(),
                "A stale book retained a global normalization rule");
    for (const auto *key : appearance::normalizationKeys) {
        auto changed = recommended;
        changed[key] = !recommended.value(key).toBool();
        require(!appearance::same(changed, recommended),
                "A normalization rule reused an old layout");
        auto copied = recommended;
        appearance::copy(copied, changed);
        require(copied.value(key) == changed.value(key), "Preview lost a normalization rule");
    }
    const auto advanced = qt::object({ { "word_spacing", 80 },
                                       { "justification_tracking", 5 },
                                       { "hanging_punctuation", true },
                                       { "widows_orphans", "avoid" } });
    auto advancedBook = qt::object({ { "font_pixels", 40 },
                                     { "word_spacing", 120 },
                                     { "justification_tracking", 15 },
                                     { "widows_orphans", "allow" } });
    appearance::applyBookDefaults(advancedBook, appearance::bookDefaults(advanced));
    require(advancedBook.value("word_spacing").toInt() == 80
                    && advancedBook.value("justification_tracking").toInt() == 5
                    && advancedBook.value("widows_orphans").toString() == "avoid"
                    && advancedBook.value("hanging_punctuation").toBool(),
            "An existing book replaced global advanced text settings");
    appearance::applyBookDefaults(advancedBook, appearance::bookDefaults({}));
    require(advancedBook.value("word_spacing").toInt() == 100
                    && advancedBook.value("justification_tracking").toInt() == 0
                    && advancedBook.value("widows_orphans").toString() == "publisher"
                    && !advancedBook.value("hanging_punctuation").toBool(),
            "Missing global settings retained stale advanced text choices");
    for (const auto *key :
         { "word_spacing", "justification_tracking", "hanging_punctuation", "widows_orphans" }) {
        auto changedAdvanced = from;
        changedAdvanced[key] = advanced.value(key);
        require(!appearance::same(changedAdvanced, from), "Advanced text reused a retained layout");
        auto copy = from;
        appearance::copy(copy, changedAdvanced);
        require(copy.value(key) == advanced.value(key), "Preview copy lost advanced text");
    }
    for (const auto &invalid : { QJsonValue("true"), QJsonValue(1), QJsonValue() })
        require(!appearance::cleanPreferences({ { "hanging_punctuation", invalid } })
                         .value("hanging_punctuation")
                         .toBool(),
                "Invalid hanging punctuation preference did not default to Off");
    for (const auto *key : { "top_margin_pixels", "bottom_margin_pixels" }) {
        auto vertical = from;
        vertical[key] = 36;
        require(!appearance::same(vertical, from), "Vertical margin changes reused an old layout");
        auto copied = from;
        appearance::copy(copied, vertical);
        require(copied.value(key).toInt() == 36, "Appearance copy lost a vertical margin");
    }
    auto publisher = from;
    publisher["normalize_page_margins"] = true;
    require(!appearance::same(publisher, from), "Publisher spacing reused an old layout");
    auto bionic = from;
    bionic["bold_reading"] = QStringLiteral("bold");
    require(!appearance::same(bionic, from), "Bionic mode changes reused an old layout");
    auto bionicCopy = from;
    appearance::copy(bionicCopy, bionic);
    require(bionicCopy.value("bold_reading").toString() == "bold",
            "Appearance copy lost Bionic mode");
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

    for (const auto &entry : { std::pair<int, int>{ 100, 100 },
                               { 120, 144 },
                               { 122, 150 },
                               { 140, 196 },
                               { 150, 225 },
                               { 173, 299 },
                               { 174, 300 },
                               { 180, 300 } }) {
        auto legacy =
                qt::object({ { "line_spacing", entry.first }, { "position", "chapter#saved" } });
        appearance::migrateLineSpacing(legacy);
        require(legacy.value("line_spacing").toInt() == entry.second
                        && legacy.value("position").toString() == "chapter#saved",
                "Legacy spacing migration lost the approximate height or reading position");
        const auto migrated = legacy;
        appearance::migrateLineSpacing(legacy);
        require(legacy == migrated, "Spacing migration ran twice");
    }
    for (const auto &value :
         { QJsonValue(), QJsonValue(QJsonValue::Null), QJsonValue(QStringLiteral("invalid")) }) {
        auto saved = qt::object({ { "line_spacing", value } });
        appearance::migrateLineSpacing(saved);
        require(saved.value("line_spacing") == value,
                "Missing or invalid spacing was converted to a number");
    }
    QJsonObject fresh;
    appearance::applyFontDefault(fresh,
                                 qt::object({ { "default_font_face", "NV Ancizar Serif" } }));
    require(fresh["font_face"].toString() == "NV Ancizar Serif"
                    && fresh["default_font_face"].toString() == "NV Ancizar Serif",
            "New books did not use the custom default");
    fresh["font_face"] = QStringLiteral("");
    appearance::applyFontDefault(fresh, {});
    require(fresh["font_face"].toString().isEmpty()
                    && fresh["default_font_face"].toString() == defaultFontFace,
            "Default changes replaced Publisher Fonts");
    fresh["font_face"] = QStringLiteral("NV Literata");
    appearance::applyFontDefault(fresh,
                                 qt::object({ { "default_font_face", "NV Ancizar Serif" } }));
    require(fresh["font_face"].toString() == "NV Literata",
            "Default changes replaced an explicit book font");
    const auto publisherDefault = qt::object(
            { { "default_font_face", "" }, { appearance::inheritTextLayoutKey, false } });
    auto publisherBook = appearance::bookDefaults(publisherDefault);
    require(publisherBook.value("font_face").toString() == "" && publisherBook.contains("font_face")
                    && publisherBook.value("default_font_face").toString() == defaultFontFace,
            "Publisher default did not preserve publisher fonts with the normal fallback");
    QJsonObject unopened;
    appearance::applyBookDefaults(unopened, publisherBook);
    require(unopened.value("font_face").toString().isEmpty(),
            "New book lost the Publisher default");
    auto explicitBook = qt::object({ { "font_face", "NV Literata" } });
    appearance::applyBookDefaults(explicitBook, publisherBook);
    require(explicitBook.value("font_face").toString() == "NV Literata",
            "Publisher default replaced an existing book's explicit font");
    auto inheritedPublisher = publisherDefault;
    inheritedPublisher[appearance::inheritTextLayoutKey] = true;
    inheritedPublisher[appearance::lastTextLayoutKey] =
            qt::object({ { "font_face", "NV Literata" } });
    require(appearance::bookDefaults(inheritedPublisher).value("font_face").toString()
                    == "NV Literata",
            "Publisher default overrode enabled text inheritance");
    require(appearance::globalPreferences(publisherDefault).value("default_font_face")
                    == QJsonValue(""),
            "Publisher default was removed from existing global preferences");
    for (const auto *key : appearance::fallbackFontKeys) {
        auto changed = from;
        changed[key] = QStringLiteral("Personal Font");
        require(!appearance::same(changed, from),
                "Fallback changes did not invalidate the retained layout");
        auto defaults = appearance::bookDefaults(qt::object({ { key, "Personal Font" } }));
        auto existing = qt::object({ { "font_face", "" }, { key, "Old Font" } });
        appearance::applyBookDefaults(existing, defaults);
        require(existing.value(key).toString() == "Personal Font"
                        && existing.value("font_face").toString().isEmpty(),
                "Existing book did not take the global fallback without replacing Publisher Fonts");
        appearance::applyBookDefaults(existing, appearance::bookDefaults({}));
        require(existing.value(key).toString().isEmpty(),
                "Removed global fallback survived in saved book state");
    }
    auto newDefault = from;
    newDefault["default_font_face"] = QStringLiteral("NV Ancizar Serif");
    require(!appearance::same(newDefault, from), "Default changes did not invalidate the layout");

    auto hidden = from;
    hidden["hide_nonlinear_fragments"] = false;
    require(!appearance::same(hidden, from),
            "Non-linear paging changes did not invalidate the retained model");

    const auto global = qt::object({ { "show_clock", true },
                                     { "header_text_size", "small" },
                                     { "bold_reading", "bold" },
                                     { "page_animations", true } });
    const auto book = qt::object({ { "show_clock", false },
                                   { "header_text_size", "large" },
                                   { "bold_reading", "off" } });
    const auto preferences = appearance::readerPreferences(global, book);
    require(preferences["show_clock"].toBool()
                    && preferences["header_text_size"].toString() == "small",
            "Book layout replaced global settings");
    require(preferences["bold_reading"].toString() == "bold",
            "Book state overrode global Bionic reading");
    const auto persistedGlobal = appearance::globalPreferences(preferences);
    require(persistedGlobal["show_clock"].toBool()
                    && persistedGlobal["header_text_size"].toString() == "small"
                    && persistedGlobal["page_animations"].toBool(),
            "Global save lost shared layout or overflow settings");
    for (const auto *mode : { "bold", "off", "synthetic" }) {
        const auto legacy = qt::object({ { "bionic_reading", mode }, { "page_animations", true } });
        const auto migrated = appearance::readerPreferences(legacy, book);
        require(migrated.value("bold_reading").toString() == mode
                        && !migrated.contains("bionic_reading")
                        && migrated.value("page_animations").toBool(),
                "Legacy global Bionic reading did not migrate to Bold reading");
        require(appearance::readerPreferences(migrated, book) == migrated,
                "Bold reading migration changed an existing choice");
        const auto persisted = appearance::globalPreferences(migrated);
        require(persisted.value("bold_reading").toString() == mode
                        && !persisted.contains("bionic_reading"),
                "Global save lost migrated Bold reading or kept the legacy key");
    }
    for (const auto &value :
         { QJsonValue(QStringLiteral("bold")), QJsonValue(QStringLiteral("off")),
           QJsonValue(QJsonValue::Null), QJsonValue(QStringLiteral("invalid")) }) {
        const auto legacyMode = value.toString() == "bold" ? "off" : "bold";
        const auto existing =
                qt::object({ { "bold_reading", value }, { "bionic_reading", legacyMode } });
        const auto migrated = appearance::readerPreferences(existing, book);
        require(migrated.value("bold_reading") == value && !migrated.contains("bionic_reading"),
                "Legacy Bionic reading replaced an explicit Bold reading choice");
    }
    // Migrate the last preset once. A shared preset replaces stale book choices.
    const auto last = qt::object({ { appearance::lastPresetKey, "alternative_b" } });
    require(appearance::readerPreferences(last, {}).value("header_footer").toString()
                            == "alternative_b"
                    && appearance::readerPreferences(last,
                                                     qt::object({ { "orientation", "portrait" } }))
                                    .value("header_footer")
                                    .toString()
                            == "alternative_b",
            "A new book did not start with the last preset");
    require(appearance::readerPreferences(last, qt::object({ { "header_footer", "default" } }))
                                    .value("header_footer")
                                    .toString()
                            == "alternative_b"
                    && appearance::readerPreferences(last, qt::object({ { "show_clock", false } }))
                                    .value("header_footer")
                                    .toString()
                            == "alternative_b"
                    && !appearance::readerPreferences({}, {}).contains("header_footer"),
            "A stale book replaced shared header and footer settings");
    const auto custom =
            qt::object({ { "header_footer", "custom" }, { appearance::lastPresetKey, "default" } });
    require(appearance::globalPreferences(custom) == qt::object({ { "header_footer", "custom" } }),
            "A global save lost the shared preset or kept the retired last preset");
    require(appearance::globalPreferences(qt::object({ { "header_footer", "default" } }))
                    == qt::object({ { "header_footer", "default" } }),
            "A global save lost the default header and footer choice");
    const auto sharedOrientation = qt::object({ { "orientation", 2 }, { "auto_rotate", true } });
    require(appearance::readerPreferences(
                    sharedOrientation,
                    qt::object({ { "orientation", 1 }, { "auto_rotate", false } }))
                    == sharedOrientation,
            "A stale book replaced shared rotation");
    require(appearance::readerPreferences({}, sharedOrientation) == sharedOrientation,
            "An existing book's rotation could not migrate to shared settings");
    require(!appearance::readerPreferences(qt::object({ { "orientation", 2 } }), sharedOrientation)
                     .contains("auto_rotate"),
            "An old book enabled automatic rotation over a global orientation lock");
    require(!appearance::readerPreferences({}, qt::object({ { "bionic_reading", "bold" } }))
                            .contains("bold_reading")
                    && !appearance::readerPreferences({}, {}).contains("bold_reading"),
            "A book-specific legacy setting or a fresh profile enabled global Bold reading");
    for (const auto density : { 212, 227, 265, 300 }) {
        readingPpi.store(density);
        require(defaultFontPixels() == halfPointPixels(22), "Default text size is not 11 points");
        require(minimumFontPixels() == halfPointPixels(12)
                        && maximumFontPixels() == halfPointPixels(96),
                "Engine size limits do not match the 6 to 48 point controls");
    }
    readingPpi.store(300);
    auto inherited = appearance::typographyDefaults();
    inherited["columns"] = 2;
    inherited["column_spacing"] = 45;
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
    appearance::rememberPageMargins(nextBook, inherited);
    require(nextBook.value(appearance::lastTextLayoutKey).toObject().size() == 10,
            "Last text style included unrelated layout or book state");
    auto initial = appearance::bookDefaults(nextBook);
    for (const auto *key : appearance::typographyKeys) {
        require(initial.value(key) == inherited.value(key),
                "New book lost inherited typography or margins");
    }
    require(!initial.contains("position") && !initial.contains("dark_mode")
                    && !initial.contains("header_footer"),
            "New book inherited unrelated book state");
    QJsonObject blank;
    appearance::applyBookDefaults(blank, initial);
    require(blank.value("font_face") == inherited.value("font_face"),
            "Empty saved state ignored inherited font");
    auto own = qt::object({ { "font_pixels", 40 }, { "position", "chapter#own" } });
    appearance::applyBookDefaults(own, initial);
    require(own.value("font_pixels").toInt() == 40 && own.value("margin_pixels").toInt() == 60
                    && own.value("position").toString() == "chapter#own",
            "Shared margins replaced a book's text style or did not apply");
    auto globalColumns =
            appearance::bookDefaults(qt::object({ { "two_columns_landscape", true },
                                                  { "column_spacing_percent", 200 },
                                                  { appearance::inheritTextLayoutKey, false } }));
    auto savedColumns = qt::object(
            { { "font_pixels", 40 }, { "columns", 2 }, { "column_spacing_percent", 100 } });
    appearance::applyBookDefaults(savedColumns, globalColumns);
    require(savedColumns.value("columns").toInt() == 1
                    && savedColumns.value("two_columns_landscape").toBool()
                    && savedColumns.value("column_spacing_percent").toInt() == 200,
            "Saved book columns overrode the global landscape policy");
    appearance::applyBookDefaults(savedColumns, appearance::bookDefaults({}));
    require(!savedColumns.value("two_columns_landscape").toBool()
                    && savedColumns.value("columns").toInt() == 1
                    && savedColumns.value("column_spacing_percent").toInt() == 150,
            "Disabling global columns retained a book override");
    require(!nextBook.value(appearance::lastTextLayoutKey).toObject().contains("columns")
                    && !nextBook.value(appearance::lastTextLayoutKey)
                                .toObject()
                                .contains("column_spacing"),
            "New-book inheritance retained global display choices");
    nextBook[appearance::inheritTextLayoutKey] = false;
    auto fixed = appearance::bookDefaults(nextBook);
    require(fixed.value("font_pixels").toInt() == defaultFontPixels()
                    && fixed.value("font_face").toString() == defaultFontFace
                    && !fixed.value("two_columns_landscape").toBool()
                    && fixed.value("margin_pixels").toInt() == 60,
            "Disabling text inheritance did not restore new-book defaults");
    nextBook["default_font_face"] = QStringLiteral("NV Ancizar Serif");
    require(appearance::bookDefaults(nextBook).value("font_face").toString() == "NV Ancizar Serif",
            "Disabling inheritance ignored the user's default font");
    nextBook[appearance::inheritTextLayoutKey] = true;
    readingPpi.store(212);
    initial = appearance::bookDefaults(nextBook);
    require(initial.value("font_pixels").toInt() == 42
                    && initial.value("margin_pixels").toInt() == 42
                    && initial.value("top_margin_pixels").toInt() == 21
                    && initial.value("bottom_margin_pixels").toInt() == 25
                    && initial.value("line_spacing").toInt() == 175,
            "Inherited pixel values did not follow the screen density");
    readingPpi.store(300);
    auto disabledChrome = last;
    disabledChrome[appearance::inheritHeaderFooterKey] = false;
    require(appearance::readerPreferences(disabledChrome, {}).value("header_footer").toString()
                            == "alternative_b"
                    && appearance::readerPreferences(
                               disabledChrome, qt::object({ { "header_footer", "alternative_a" } }))
                                    .value("header_footer")
                                    .toString()
                            == "alternative_b"
                    && !appearance::globalPreferences(disabledChrome)
                                .contains(appearance::inheritHeaderFooterKey),
            "Retired header/footer inheritance changed shared layout");
    auto legacyMargins = qt::object({ { appearance::lastTextLayoutKey,
                                        qt::object({ { "reading_ppi", 212 },
                                                     { "margin_pixels", 42 },
                                                     { "top_margin_pixels", 21 },
                                                     { "bottom_margin_pixels", 25 } }) } });
    initial = appearance::bookDefaults(legacyMargins);
    require(initial.value("margin_pixels").toInt() == 59
                    && initial.value("top_margin_pixels").toInt() == 30
                    && initial.value("bottom_margin_pixels").toInt() == 35,
            "Legacy margins did not migrate from the original screen density");
    legacyMargins[appearance::pageMarginsKey] = qt::object({ { "margin_pixels", -5 },
                                                             { "top_margin_pixels", 999 },
                                                             { "bottom_margin_pixels", 12 } });
    initial = appearance::bookDefaults(legacyMargins);
    require(initial.value("margin_pixels").toInt() == 0
                    && initial.value("top_margin_pixels").toInt() == 120
                    && initial.value("bottom_margin_pixels").toInt() == 12,
            "Shared margins did not override legacy values within slider bounds");
    const auto retired =
            qt::object({ { "smooth_images", false }, { "heavier_interface_text", false } });
    require(appearance::globalPreferences(retired).isEmpty()
                    && appearance::readerPreferences(retired, {}).isEmpty(),
            "Retired settings survived global preference loading or saving");
    const auto defaults = appearance::typographyDefaults();
    require(defaults.value("font_face").toString() == defaultFontFace
                    && defaults.value("font_pixels").toInt() == defaultFontPixels()
                    && defaults.value("line_spacing").toInt() == defaultLineSpacing
                    && defaults.value("margin_pixels").toInt() == defaultMarginPixels()
                    && defaults.value("top_margin_pixels").toInt() == 24
                    && defaults.value("bottom_margin_pixels").toInt() == 24
                    && defaults.value("text_alignment").toString() == "left"
                    && defaults.value("weight_steps").toInt() == 0
                    && defaults.value("font_weight").toInt() == 450
                    && defaults.value("hyphenation").toString() == "off",
            "Typography defaults changed");
    require(!defaults.contains("dark_mode") && !defaults.contains("smooth_images")
                    && !defaults.contains("bold_reading") && !defaults.contains("columns")
                    && !defaults.contains("column_spacing") && defaults.size() == 12,
            "Restoring typography must not change display choices");
    readingPpi.store(212);
    require(appearance::typographyDefaults().value("font_pixels").toInt() == defaultFontPixels(),
            "Default text size did not follow the screen density");
    readingPpi.store(300);
    const auto benchmark = appearance::benchmarkTypography();
    require(benchmark.size() == 15 && benchmark.value("font_face").toString().isEmpty()
                    && benchmark.value("font_pixels").toInt() == defaultFontPixels()
                    && benchmark.value("margin_pixels").toInt() == 60
                    && benchmark.value("top_margin_pixels").toInt() == 60
                    && benchmark.value("line_spacing").toInt() == 144
                    && !benchmark.value("dark_mode").toBool()
                    && benchmark.value("hyphenation").toString() == "off",
            "Benchmark typography changed");
    readingPpi.store(212);
    require(appearance::benchmarkTypography().value("font_pixels").toInt() == defaultFontPixels(),
            "Benchmark profile did not follow screen density");
    std::puts("PASS: appearance keys, copy, comparison and typography defaults");
    return 0;
}
