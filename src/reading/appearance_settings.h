#pragma once
#include "reading/reading_density.h"
#include <QJsonObject>
#include <array>

namespace kobalt {
inline constexpr auto defaultFontFace = "Libron";
inline constexpr int defaultLineSpacing = 150;
inline constexpr int maximumLineSpacing = 300;
inline constexpr int defaultVariableFontWeight = 450;
inline constexpr int defaultColumnSpacingPercent = 150;

namespace appearance {
inline constexpr std::array<int, 4> dictionaryFontSizes{80, 100, 120, 140};
inline bool validDictionaryFontSize(int percent) {
    for (int size : dictionaryFontSizes)
        if (size == percent)
            return true;
    return false;
}
// Global generic-family fallbacks. An empty value keeps the bundled default.
inline constexpr std::array<const char *, 3> fallbackFontKeys{"serif_font_face", "sans_font_face",
                                                              "monospace_font_face"};
// Global layout choices. Legacy book values are read only during migration.
inline constexpr std::array<const char *, 8> layoutKeys{"header_footer",    "show_header",      "show_footer",
                                                        "header_text_size", "footer_text_size", "show_clock",
                                                        "orientation",      "auto_rotate"};
// Legacy keys retained for migration.
inline constexpr auto lastPresetKey = "last_header_footer";
inline constexpr auto inheritTextLayoutKey = "inherit_text_layout";
inline constexpr auto inheritHeaderFooterKey = "inherit_header_footer";
inline constexpr auto lastTextLayoutKey = "last_text_layout";
inline constexpr std::array<const char *, 9> typographyKeys{
    "font_face",     "font_pixels",       "line_spacing",         "line_spacing_version", "text_alignment",
    "weight_steps",  "font_weight",       "font_optical_size",    "hyphenation"};
inline constexpr std::array<const char *, 3> marginKeys{"margin_pixels", "top_margin_pixels", "bottom_margin_pixels"};
inline constexpr auto pageMarginsKey = "page_margins";
// Defaults for a book without saved typography, in the current screen density.
QJsonObject bookDefaults(const QJsonObject &preferences);
void applyBookDefaults(QJsonObject &saved, const QJsonObject &defaults);
// Records only text. Global layout and reading position stay separate.
void rememberTextLayout(QJsonObject &preferences, const QJsonObject &state);
// Stores applied margins globally in 300 ppi units, independent of host density.
void rememberPageMargins(QJsonObject &preferences, const QJsonObject &state);
// Global layout takes precedence over legacy book values.
// Converts the legacy global Bionic Reading key unless Bold Reading is already set.
QJsonObject readerPreferences(const QJsonObject &global, const QJsonObject &book);
// Keeps global layout and removes retired inheritance keys.
QJsonObject globalPreferences(QJsonObject preferences);
QJsonObject cleanPreferences(QJsonObject preferences);
// The saved settings that decide how the book is laid out and drawn. A
// retained layout is reused only when all of them match.
inline constexpr std::array<const char *, 3> beautifierKeys{
    "beautifier_initials", "beautifier_spacing", "beautifier_indent"};
inline QJsonObject beautifierSettings(const QJsonObject &settings) {
    QJsonObject result;
    const bool legacy = settings.contains("book_beautifier");
    const bool enabled = settings.value("book_beautifier").toBool(false);
    for (const auto *key : beautifierKeys)
        result[key] = (!legacy || enabled) && settings.value(key).toBool(legacy && enabled);
    return result;
}
inline constexpr auto normalizationPresetKey = "book_normalization_preset";
inline constexpr std::array<const char *, 5> normalizationKeys{
    "normalize_page_margins", "reduce_chapter_spacing", "beautifier_initials", "beautifier_spacing", "beautifier_indent"};
QJsonObject normalizationSettings(const QJsonObject &settings);
QString normalizationPreset(const QJsonObject &settings);
QJsonObject normalizationPresetSettings(const QString &preset);
inline constexpr std::array<const char *, 35> keys{
    "line_spacing_version",   "font_pixels",     "line_spacing", "paragraph_spacing", "paragraph_indent", "font_face",
    "default_font_face",      "margin_pixels",   "top_margin_pixels", "bottom_margin_pixels",
    "text_alignment",         "bold_reading",    "weight_steps",      "font_weight",
    "font_optical_size",      "hyphenation",     "dark_mode",         "hide_nonlinear_fragments",
    "beautifier_initials", "beautifier_spacing", "beautifier_indent", "normalize_page_margins", "reduce_chapter_spacing", "serif_font_face", "sans_font_face",    "monospace_font_face",
    "vertical_justification", "vertical_spacing_scope", "column_spacing", "columns", "two_columns_landscape", "column_spacing_percent", "word_spacing", "justification_tracking", "widows_orphans"};
// Preserve the Boolean used by early development builds of page filling.
inline QString verticalSpacingMode(const QJsonValue &value) {
    if (value.isBool()) return value.toBool() ? QStringLiteral("expand") : QStringLiteral("off");
    const auto mode = value.toString();
    return mode == "expand" || mode == "center" ? mode : QStringLiteral("off");
}
inline QString verticalSpacingScope(const QJsonValue &value) {
    return value.toString() == "protection" ? QStringLiteral("protection") : QStringLiteral("all");
}
inline bool validWordSpacing(int percent) {

    return percent == 80 || percent == 100 || percent == 120;
}
// -1 keeps publisher spacing; other values are percentages of the text size.
inline int paragraphSpacing(const QJsonValue &value) {
    const int percent = value.toInt(-1);
    return value.isDouble() && value.toDouble() == percent && percent >= 0 && percent <= 200 ? percent : -1;
}
// -1 keeps publisher indentation; zero removes it.
inline int paragraphIndent(const QJsonValue &value) {
    const int percent = value.toInt(-1);
    return value.isDouble() && value.toDouble() == percent && percent >= 0 && percent <= 300 ? percent : -1;
}
inline bool validTracking(int percent) {
    return percent == 0 || percent == 5 || percent == 15;
}
inline bool validWidowsOrphans(const QString &mode) {
    return mode == "publisher" || mode == "avoid" || mode == "allow";
}
// Copies every appearance key from `from` into `to`. A key that `from` does
// not have is removed from `to`, so the engine uses its own default for it.
void copy(QJsonObject &to, const QJsonObject &from);
// Converts the old double-scaled spacing once, preserving its approximate height.
inline void migrateLineSpacing(QJsonObject &saved) {
    if (saved.value("line_spacing_version").toInt() >= 1) {
        return;
    }
    if (!saved.value("line_spacing").isDouble()) {
        return;
    }
    const int old = qBound(100, saved.value("line_spacing").toInt(122), 180);
    saved["line_spacing"] = old == 122 ? 150 : qMin(maximumLineSpacing, (old * old + 50) / 100);
    saved["line_spacing_version"] = 1;
}
// True when both objects have the same value, or no value, for every key.
bool same(const QJsonObject &a, const QJsonObject &b);
// The typography that Restore appearance defaults selects.
QJsonObject typographyDefaults();
// Empty for every identity except the benchmark-owned EPUB.
QJsonObject benchmarkTypography(const QString &identity);
// A global default selects the override for new books, not Publisher Fonts.
// Existing saved book selections stay unchanged.
void applyFontDefault(QJsonObject &saved, const QJsonObject &preferences);
}
}
