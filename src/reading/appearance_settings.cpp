#include "appearance_settings.h"
#include "support/qt_compat.h"
#include "page_navigation.h"
#include "header_footer.h"
#include "sleep_screen.h"
#include "session_timeout.h"
#include <QStringList>

namespace kobalt::appearance {
QJsonObject normalizationSettings(const QJsonObject &settings)
{
    auto result = beautifierSettings(settings);
    const bool legacy = settings.value("avoid_wasteful_styling").toBool(true);
    result["normalize_page_margins"] = settings.value("normalize_page_margins").toBool(legacy);
    result["reduce_chapter_spacing"] = settings.value("reduce_chapter_spacing").toBool(legacy);
    return result;
}

QJsonObject normalizationPresetSettings(const QString &preset)
{
    QJsonObject result;
    for (int i = 0; i < int(normalizationKeys.size()); ++i)
        result[normalizationKeys[i]] = preset == "full" || i < 2;
    result[normalizationPresetKey] = preset;
    return result;
}

QString normalizationPreset(const QJsonObject &settings)
{
    if (settings.value(normalizationPresetKey) == QStringLiteral("custom"))
        return QStringLiteral("custom");
    const auto rules = normalizationSettings(settings);
    for (const auto *preset : { "recommended", "full" }) {
        auto expected = normalizationPresetSettings(preset);
        expected.remove(normalizationPresetKey);
        if (rules == expected)
            return QString::fromLatin1(preset);
    }
    return QStringLiteral("custom");
}

QJsonObject readerPreferences(const QJsonObject &global, const QJsonObject &book)
{
    auto result = globalPreferences(global);
    if (!result.contains("bold_reading") && result.contains("bionic_reading")) {
        result["bold_reading"] = result.value("bionic_reading");
    }
    result.remove("bionic_reading");
    result.remove("smooth_images");
    result.remove("heavier_interface_text");
    bool globalChrome = false;
    for (const auto *key : { "header_footer", "show_header", "show_footer", "header_text_size",
                             "footer_text_size", "show_clock" }) {
        globalChrome = globalChrome || result.contains(key);
    }
    const bool globalRotation = result.contains("orientation") || result.contains("auto_rotate");
    for (const auto *key : layoutKeys) {
        const bool rotation = qstrcmp(key, "orientation") == 0 || qstrcmp(key, "auto_rotate") == 0;
        if (!result.contains(key) && book.contains(key)
            && !(rotation ? globalRotation : globalChrome))
            result[key] = book.value(key);
    }
    return result;
}

QJsonObject globalPreferences(QJsonObject preferences)
{
    if (preferences.contains("schema") && preferences.value("schema").toDouble(-1) != 1)
        return preferences;
    if (!preferences.contains("bold_reading") && preferences.contains("bionic_reading"))
        preferences["bold_reading"] = preferences.value("bionic_reading");
    preferences.remove("bionic_reading");
    if (preferences.contains("book_beautifier")) {
        const auto rules = beautifierSettings(preferences);
        for (const auto *key : beautifierKeys)
            preferences[key] = rules.value(key);
        preferences.remove("book_beautifier");
    }
    bool hasNormalization = preferences.contains("avoid_wasteful_styling")
            || preferences.contains(normalizationPresetKey);
    for (const auto *key : normalizationKeys)
        hasNormalization |= preferences.contains(key);
    if (hasNormalization) {
        const auto normalization = normalizationSettings(preferences);
        for (const auto *key : normalizationKeys)
            preferences[key] = normalization.value(key);
        preferences[normalizationPresetKey] = normalizationPreset(preferences);
        preferences.remove("avoid_wasteful_styling");
    }
    if (!preferences.contains("header_footer") && preferences.value(lastPresetKey).isString())
        preferences["header_footer"] = preferences.value(lastPresetKey);
    preferences.remove(lastPresetKey);
    preferences.remove(inheritHeaderFooterKey);
    preferences.remove("smooth_images");
    preferences.remove("heavier_interface_text");
    preferences.remove("native_hide_status_bar");
    preferences.remove("columns");
    preferences.remove("column_spacing");
    preferences.remove(pageNavigation::barVisibleKey);
    if (!preferences.contains(pageNavigation::menuViewKey)
        && (preferences.contains("pin_stats")
            || preferences.contains(pageNavigation::thumbnailsKey)))
        preferences[pageNavigation::menuViewKey] =
                pageNavigation::value(pageNavigation::menuView(preferences));
    preferences.remove("pin_stats");
    preferences.remove(pageNavigation::thumbnailsKey);
    return preferences;
}

QJsonObject cleanPreferences(QJsonObject preferences)
{
    preferences = globalPreferences(std::move(preferences));
    if (preferences.contains("schema") && preferences.value("schema").toDouble(-1) != 1)
        return preferences;
    preferences.remove("kobalt_sleep_screen");
    // Smart Wi-Fi power saving is not final yet. Until beta 7 its setting is
    // never saved, and a saved one is removed, so the saver stays off.
    preferences.remove("turn_off_wifi_on_open");
    preferences.remove("highlight_help_seen");
    bool oldChrome = false;
    for (const auto *key : { "show_header", "show_footer", "show_clock", "show_progress_bar",
                             "header_text_size", "footer_text_size" })
        oldChrome = oldChrome || preferences.contains(key);
    if (oldChrome && !preferences.contains(headerfooter::presetKey)) {
        const auto layout = headerfooter::select(preferences);
        preferences[headerfooter::presetKey] = headerfooter::presetValue(layout.preset);
        if (layout.preset == headerfooter::Preset::Custom)
            preferences[headerfooter::customKey] = headerfooter::toJson(layout.layout);
    }
    for (const auto *key : { "show_header", "show_footer", "show_clock", "show_progress_bar",
                             "header_text_size", "footer_text_size" })
        preferences.remove(key);
    for (const auto *key : { "dark_mode",
                             "light_dark_mode_shortcut",
                             "hide_tab_labels",
                             "disable_reader_standby",
                             "inherit_text_layout",
                             "two_columns_landscape",
                             "page_animations",
                             "edge_light_gestures",
                             "show_light_gesture_feedback",
                             "dense_stats_chart",
                             "hanging_punctuation",
                             "hide_nonlinear_fragments",
                             "normalize_page_margins",
                             "reduce_chapter_spacing",
                             "minimal_annotation_mode",
                             "show_header",
                             "show_footer",
                             "show_clock",
                             "auto_rotate",
                             "turn_off_wifi_on_open" })
        if (preferences.contains(key) && !preferences.value(key).isBool())
            preferences.remove(key);
    for (const auto *key : beautifierKeys)
        if (preferences.contains(key) && !preferences.value(key).isBool())
            preferences.remove(key);
    const auto choice = [&](const char *key, const QStringList &values) {
        if (preferences.contains(key) && !values.contains(preferences.value(key).toString()))
            preferences.remove(key);
    };
    choice("tappable_links", { "off", "footnotes", "all" });
    choice("bold_reading", { "off", "bold" });
    choice("widows_orphans", { "publisher", "avoid", "allow" });
    if (preferences.contains("vertical_justification"))
        preferences["vertical_justification"] =
                verticalSpacingMode(preferences.value("vertical_justification"));
    if (preferences.contains("vertical_spacing_scope"))
        preferences["vertical_spacing_scope"] =
                verticalSpacingScope(preferences.value("vertical_spacing_scope"));
    choice("marks_sort", { "position", "newest", "oldest" });
    choice("sleep_screen", { "stock", "cover", "page", "overlay" });
    choice("sleep_tile", { "card", "bookmark", "none" });
    choice("time_remaining_format", { "detailed", "approximate" });
    if (preferences.contains(sessionTimeout::preferenceKey)
        && !sessionTimeout::valid(preferences.value(sessionTimeout::preferenceKey)))
        preferences.remove(sessionTimeout::preferenceKey);
    choice("header_footer", { "default", "alternative_a", "alternative_b", "phone", "custom" });
    choice("default_menu_view", { "none", "seek", "stats" });
    choice("navigation_bar_buttons", { "chapters", "pages" });
    choice("chapter_groups", { "current", "all", "off" });
    choice("page_navigation", { "taps_and_swipes", "taps_only", "swipes_only", "buttons_only" });
    choice("tap_zones", { "left_right", "reversed", "top_bottom", "one_handed" });
    choice("screen_refresh", { "smart", "1", "chapter", "6", "10", "20", "manual" });
    for (const auto *key :
         { "default_font_face", "ui_font_face", "ui_mono_font_face", "header_footer_font_face",
           "dictionary_font_face", "serif_font_face", "sans_font_face", "monospace_font_face" })
        if (preferences.contains(key)
            && (!preferences.value(key).isString()
                || preferences.value(key).toString().size() > 512))
            preferences.remove(key);
    if (preferences.contains("orientation")) {
        const auto value = preferences.value("orientation");
        if (!value.isDouble() || (value != 1 && value != 2 && value != 4 && value != 8))
            preferences.remove("orientation");
    }
    const auto number = [&](const char *key, int minimum, int maximum) {
        const auto value = preferences.value(key);
        if (preferences.contains(key)
            && (!value.isDouble() || value.toDouble() < minimum || value.toDouble() > maximum
                || value.toDouble() != value.toInt()))
            preferences.remove(key);
    };
    number("column_spacing_percent", 100, 300);
    if (preferences.contains("paragraph_spacing"))
        preferences["paragraph_spacing"] = paragraphSpacing(preferences.value("paragraph_spacing"));
    if (preferences.contains("paragraph_indent"))
        preferences["paragraph_indent"] = paragraphIndent(preferences.value("paragraph_indent"));
    number("dictionary_font_size", 80, 140);
    if (preferences.contains("dictionary_font_size")
        && !validDictionaryFontSize(preferences.value("dictionary_font_size").toInt()))
        preferences.remove("dictionary_font_size");
    number("header_footer_added_weight", 0, 8);
    if (preferences.value("header_footer_added_weight").toInt() % 2 != 0)
        preferences.remove("header_footer_added_weight");
    if (preferences.contains("word_spacing")
        && (!preferences.value("word_spacing").isDouble()
            || preferences.value("word_spacing").toDouble()
                    != preferences.value("word_spacing").toInt()
            || !validWordSpacing(preferences.value("word_spacing").toInt())))
        preferences.remove("word_spacing");
    if (preferences.contains("justification_tracking")
        && (!preferences.value("justification_tracking").isDouble()
            || preferences.value("justification_tracking").toDouble()
                    != preferences.value("justification_tracking").toInt()
            || !validTracking(preferences.value("justification_tracking").toInt())))
        preferences.remove("justification_tracking");
    return preferences;
}

QJsonObject bookDefaults(const QJsonObject &preferences)
{
    auto defaults = typographyDefaults();
    const auto normalization = normalizationSettings(preferences);
    for (const auto *key : normalizationKeys)
        defaults[key] = normalization.value(key);
    defaults.remove("font_face");
    applyFontDefault(defaults, preferences);
    defaults["vertical_justification"] =
            verticalSpacingMode(preferences.value("vertical_justification"));
    defaults["vertical_spacing_scope"] =
            verticalSpacingScope(preferences.value("vertical_spacing_scope"));
    defaults["two_columns_landscape"] = preferences.value("two_columns_landscape").toBool(false);
    defaults["column_spacing_percent"] = qBound(
            100, preferences.value("column_spacing_percent").toInt(defaultColumnSpacingPercent),
            300);
    // Prefer the global physical margins. Before migration, the last text
    // snapshot supplies the user's latest margins and their original density.
    const auto margins = preferences.value(pageMarginsKey).toObject();
    const auto legacy = preferences.value(lastTextLayoutKey).toObject();
    const int legacyPpi = qMax(1, legacy.value("reading_ppi").toInt(readingPpi.load()));
    for (const auto *key : marginKeys) {
        const int design = margins.value(key).toInt(
                legacy.value(key).isDouble()
                        ? qRound(legacy.value(key).toDouble() * 300 / legacyPpi)
                        : marginDesign(defaults.value(key).toInt()));
        defaults[key] = readingPixels(qBound(0, design, 120));
    }
    defaults["word_spacing"] = preferences.value("word_spacing").toInt(100);
    defaults["paragraph_spacing"] = paragraphSpacing(preferences.value("paragraph_spacing"));
    defaults["paragraph_indent"] = paragraphIndent(preferences.value("paragraph_indent"));
    defaults["justification_tracking"] = preferences.value("justification_tracking").toInt(0);
    defaults["hanging_punctuation"] = preferences.value("hanging_punctuation").toBool(false);
    defaults["widows_orphans"] = preferences.value("widows_orphans").toString("publisher");
    if (!preferences.value(inheritTextLayoutKey).toBool(true)) {
        return defaults;
    }
    auto last = preferences.value(lastTextLayoutKey).toObject();
    migrateLineSpacing(last);
    const int ppi = last.value("reading_ppi").toInt(readingPpi.load());
    for (const auto *key : typographyKeys) {
        const auto value = last.value(key);
        if (value.type() == defaults.value(key).type()) {
            defaults[key] = value;
        }
    }
    if (ppi > 0 && ppi != readingPpi.load()) {
        if (last.value("font_pixels").isDouble())
            defaults["font_pixels"] = qRound(
                    qBound(0.0, last.value("font_pixels").toDouble() * readingPpi.load() / ppi,
                           double(maximumFontPixels())));
    }
    return defaults;
}

void applyBookDefaults(QJsonObject &saved, const QJsonObject &defaults)
{
    bool hasTypography = false;
    for (const auto *key : typographyKeys) {
        hasTypography = hasTypography || saved.contains(key);
    }
    if (!hasTypography) {
        for (const auto *key : typographyKeys) {
            saved[key] = defaults.value(key);
        }
    }
    applyFontDefault(saved, defaults);
    for (const auto *key : marginKeys)
        saved[key] = defaults.value(key);
    const auto normalization = normalizationSettings(defaults);
    for (const auto *key : normalizationKeys)
        saved[key] = normalization.value(key);
    saved.remove("avoid_wasteful_styling");
    saved.remove("book_beautifier");
    // Settings choices replace stale book values.
    saved["columns"] = 1;
    saved["vertical_justification"] = verticalSpacingMode(defaults.value("vertical_justification"));
    saved["vertical_spacing_scope"] =
            verticalSpacingScope(defaults.value("vertical_spacing_scope"));
    saved["two_columns_landscape"] = defaults.value("two_columns_landscape").toBool(false);
    saved["column_spacing_percent"] =
            defaults.value("column_spacing_percent").toInt(defaultColumnSpacingPercent);
    saved["word_spacing"] = defaults.value("word_spacing").toInt(100);
    saved["paragraph_spacing"] = paragraphSpacing(defaults.value("paragraph_spacing"));
    saved["paragraph_indent"] = paragraphIndent(defaults.value("paragraph_indent"));
    saved["justification_tracking"] = defaults.value("justification_tracking").toInt(0);
    saved["hanging_punctuation"] = defaults.value("hanging_punctuation").toBool(false);
    saved["widows_orphans"] = defaults.value("widows_orphans").toString("publisher");
}

void rememberTextLayout(QJsonObject &preferences, const QJsonObject &state)
{
    QJsonObject last;
    for (const auto *key : typographyKeys) {
        if (state.contains(key)) {
            last[key] = state.value(key);
        }
    }
    if (last.isEmpty()) {
        return;
    }
    last["reading_ppi"] = readingPpi.load();
    preferences[lastTextLayoutKey] = last;
}

void rememberPageMargins(QJsonObject &preferences, const QJsonObject &state)
{
    auto margins = preferences.value(pageMarginsKey).toObject();
    for (const auto *key : marginKeys)
        if (state.value(key).isDouble())
            margins[key] = qBound(0, marginDesign(state.value(key).toInt()), 120);
    if (!margins.isEmpty())
        preferences[pageMarginsKey] = margins;
}

void copy(QJsonObject &to, const QJsonObject &from)
{
    for (const auto *key : keys)
        to[key] = from.value(key);
}

bool same(const QJsonObject &a, const QJsonObject &b)
{
    for (const auto *key : keys)
        if (a.value(key) != b.value(key))
            return false;
    return true;
}

void applyFontDefault(QJsonObject &saved, const QJsonObject &preferences)
{
    // Global fallbacks also replace stale values in an existing book's state.
    for (const auto *key : fallbackFontKeys)
        saved[key] = preferences.value(key).toString();
    const auto face = preferences.value("default_font_face").toString(defaultFontFace);
    // Publisher is an empty override. The renderer still needs a concrete fallback.
    saved["default_font_face"] = face.isEmpty() ? QString(defaultFontFace) : face;
    if (!saved.contains("font_face"))
        saved["font_face"] = face;
}

QJsonObject benchmarkTypography(const QString &identity)
{
    if (identity != QStringLiteral("file:///mnt/onboard/kobalt-benchmark.kepub.epub"))
        return {};
    return qt::object({ { "font_face", QString() },
                        { "default_font_face", QStringLiteral("Libron") },
                        { "font_pixels", defaultFontPixels() },
                        { "line_spacing", 144 },
                        { "line_spacing_version", 1 },
                        { "margin_pixels", readingPixels(60) },
                        { "top_margin_pixels", readingPixels(60) },
                        { "bottom_margin_pixels", readingPixels(60) },
                        { "text_alignment", QStringLiteral("publisher") },
                        { "weight_steps", 0 },
                        { "font_weight", 400 },
                        { "font_optical_size", 0 },
                        { "bold_reading", "off" },
                        { "hyphenation", QStringLiteral("off") },
                        { "dark_mode", false } });
}

QJsonObject typographyDefaults()
{
    return qt::object(
            { { "font_face", QString::fromLatin1(defaultFontFace) },
              { "font_pixels", defaultFontPixels() },
              { "line_spacing", defaultLineSpacing },
              { "line_spacing_version", 1 },
              { "margin_pixels", defaultMarginPixels() },
              { "top_margin_pixels", defaultVerticalMarginPixels(defaultMarginPixels()) },
              { "bottom_margin_pixels", defaultVerticalMarginPixels(defaultMarginPixels()) },
              { "text_alignment", QStringLiteral("left") },
              { "weight_steps", 0 },
              { "font_weight", defaultVariableFontWeight },
              { "font_optical_size", 0 },
              { "hyphenation", QStringLiteral("off") } });
}
} // namespace kobalt::appearance
