#include "appearance_settings.h"
#include "support/qt_compat.h"

namespace kobalt::appearance {
QJsonObject readerPreferences(const QJsonObject &global, const QJsonObject &book) {
    auto result = global;
    if (!result.contains("bold_reading") && result.contains("bionic_reading")) {
        result["bold_reading"] = result.value("bionic_reading");
    }
    result.remove("bionic_reading");
    result.remove("smooth_images");
    result.remove("heavier_interface_text");
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
    if (!chromeSaved) {
        if (!global.value(inheritHeaderFooterKey).toBool(true)) {
            result["header_footer"] = QStringLiteral("default");
        } else if (global.value(lastPresetKey).isString()) {
            result["header_footer"] = global.value(lastPresetKey);
        }
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
    preferences.remove("smooth_images");
    preferences.remove("heavier_interface_text");
    return preferences;
}
QJsonObject bookDefaults(const QJsonObject &preferences) {
    auto defaults = typographyDefaults();
    defaults.remove("font_face");
    applyFontDefault(defaults, preferences);
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
        for (const auto *key : {"font_pixels", "margin_pixels", "top_margin_pixels", "bottom_margin_pixels"}) {
            if (last.value(key).isDouble()) {
                defaults[key] = qRound(
                    qBound(0.0, last.value(key).toDouble() * readingPpi.load() / ppi,
                           double(qstrcmp(key, "font_pixels") == 0 ? maximumFontPixels() : maximumMarginPixels())));
            }
        }
    }
    return defaults;
}
void applyBookDefaults(QJsonObject &saved, const QJsonObject &defaults) {
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
}
void rememberTextLayout(QJsonObject &preferences, const QJsonObject &state) {
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
                       {"dark_mode", false}});
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
