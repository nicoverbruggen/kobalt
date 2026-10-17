#pragma once
#include "reading/reading_density.h"
#include <QJsonObject>
#include <array>

namespace kobalt {
inline constexpr auto defaultFontFace = "Libron";
inline constexpr int defaultLineSpacing = 150;
inline constexpr int maximumLineSpacing = 300;
inline constexpr int defaultVariableFontWeight = 450;

namespace appearance {
// Saved per book. The old header, footer and clock keys stay for books saved
// before header and footer presets; see header_footer.h.
inline constexpr std::array<const char *, 8> layoutKeys{"header_footer",    "show_header",      "show_footer",
                                                        "header_text_size", "footer_text_size", "show_clock",
                                                        "orientation",      "auto_rotate"};
// Saved globally: the header and footer preset of the book opened or changed
// last. New books inherit it when header/footer inheritance is enabled.
inline constexpr auto lastPresetKey = "last_header_footer";
inline constexpr auto inheritTextLayoutKey = "inherit_text_layout";
inline constexpr auto inheritHeaderFooterKey = "inherit_header_footer";
inline constexpr auto lastTextLayoutKey = "last_text_layout";
inline constexpr std::array<const char *, 12> typographyKeys{
    "font_face",     "font_pixels",       "line_spacing",         "line_spacing_version",
    "margin_pixels", "top_margin_pixels", "bottom_margin_pixels", "text_alignment",
    "weight_steps",  "font_weight",       "font_optical_size",    "hyphenation"};
// Defaults for a book without saved typography, in the current screen density.
QJsonObject bookDefaults(const QJsonObject &preferences);
void applyBookDefaults(QJsonObject &saved, const QJsonObject &defaults);
// Records only text and page margins. Display choices and reading position stay separate.
void rememberTextLayout(QJsonObject &preferences, const QJsonObject &state);
// Book layout takes precedence over legacy global layout values.
// Converts the legacy global Bionic Reading key unless Bold Reading is already set.
QJsonObject readerPreferences(const QJsonObject &global, const QJsonObject &book);
// Removes the book layout and records the book's preset as the last one.
QJsonObject globalPreferences(QJsonObject preferences);
// The saved settings that decide how the book is laid out and drawn. A
// retained layout is reused only when all of them match.
inline constexpr std::array<const char *, 16> keys{
    "line_spacing_version", "font_pixels",   "line_spacing",      "font_face",
    "default_font_face",    "margin_pixels", "top_margin_pixels", "bottom_margin_pixels",
    "text_alignment",       "bold_reading",  "weight_steps",      "font_weight",
    "font_optical_size",    "hyphenation",   "dark_mode",         "hide_nonlinear_fragments"};
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
