#include "appearance_settings.h"
#include "support/qt_compat.h"

namespace kobalt::appearance {
QJsonObject readerPreferences(const QJsonObject &global, const QJsonObject &book) {
    auto result = global;
    if (!result.contains("bold_reading") && result.contains("bionic_reading")) {
        result["bold_reading"] = result.value("bionic_reading");
    }
    result.remove("bionic_reading");
    for (const auto *key : layoutKeys)
        if (book.contains(key))
            result[key] = book.value(key);
    // A book with no header and footer of its own, old or new, starts with
    // the preset of the book read last.
    bool chromeSaved = false;
    for (const auto *key :
         {"header_footer", "show_header", "show_footer", "header_text_size", "footer_text_size", "show_clock"}) {
        chromeSaved = chromeSaved || book.contains(key);
    }
    if (!chromeSaved && global.value(lastPresetKey).isString()) {
        result["header_footer"] = global.value(lastPresetKey);
    }
    return result;
}
QJsonObject globalPreferences(QJsonObject preferences) {
    // No saved last preset means Kobalt, so a profile that only uses Kobalt never saves one.
    const auto preset = preferences.value("header_footer");
    if (preset.isString() && (preferences.contains(lastPresetKey) || preset.toString() != "default")) {
        preferences[lastPresetKey] = preset;
    }
    for (const auto *key : layoutKeys)
        preferences.remove(key);
    return preferences;
}
void copy(QJsonObject &to, const QJsonObject &from) {
    for (const auto *key : keys)
        to[key] = from.value(key);
}
bool same(const QJsonObject &a, const QJsonObject &b) {
    for (const auto *key : keys)
        if (a.value(key) != b.value(key))
            return false;
    return true;
}
void applyFontDefault(QJsonObject &saved, const QJsonObject &preferences) {
    auto face = preferences.value("default_font_face").toString(defaultFontFace);
    if (face.isEmpty())
        face = defaultFontFace;
    saved["default_font_face"] = face;
    if (!saved.contains("font_face"))
        saved["font_face"] = face;
}
QJsonObject benchmarkTypography(const QString &identity) {
    if (identity != QStringLiteral("file:///mnt/onboard/kobalt-benchmark.kepub.epub"))
        return {};
    return qt::object({{"font_face", QString()},
                       {"default_font_face", QStringLiteral("Libron")},
                       {"font_pixels", defaultFontPixels()},
                       {"line_spacing", 144},
                       {"line_spacing_version", 1},
                       {"margin_pixels", readingPixels(60)},
                       {"top_margin_pixels", readingPixels(60)},
                       {"bottom_margin_pixels", readingPixels(60)},
                       {"text_alignment", QStringLiteral("publisher")},
                       {"weight_steps", 0},
                       {"font_weight", 400},
                       {"font_optical_size", 0},
                       {"bold_reading", "off"},
                       {"hyphenation", QStringLiteral("off")},
                       {"dark_mode", false},
                       {"smooth_images", true}});
}
QJsonObject typographyDefaults() {
    return qt::object({{"font_face", QString::fromLatin1(defaultFontFace)},
                       {"font_pixels", defaultFontPixels()},
                       {"line_spacing", defaultLineSpacing},
                       {"line_spacing_version", 1},
                       {"margin_pixels", defaultMarginPixels()},
                       {"top_margin_pixels", defaultVerticalMarginPixels(defaultMarginPixels())},
                       {"bottom_margin_pixels", defaultVerticalMarginPixels(defaultMarginPixels())},
                       {"text_alignment", QStringLiteral("left")},
                       {"weight_steps", 0},
                       {"font_weight", defaultVariableFontWeight},
                       {"font_optical_size", 0},
                       {"hyphenation", QStringLiteral("off")}});
}
}
