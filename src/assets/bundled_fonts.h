#pragma once
#include <QStringList>
#include <array>
#include "font_catalog.h"

namespace kobalt {
inline constexpr std::array<const char *, 4> serifFallbackFontFiles{"NotoSerif-Regular.ttf", "NotoSerif-Bold.ttf",
                                                                    "NotoSerif-Italic.ttf", "NotoSerif-BoldItalic.ttf"};
inline constexpr std::array<const char *, 18> scriptFallbackFontFiles{
    "NotoNaskhArabic-Regular.ttf",   "NotoNaskhArabic-Bold.ttf",      "NotoSerifHebrew-Regular.ttf",
    "NotoSerifHebrew-Bold.ttf",      "NotoSerifArmenian-Regular.ttf", "NotoSerifArmenian-Bold.ttf",
    "NotoSerifGeorgian-Regular.ttf", "NotoSerifGeorgian-Bold.ttf",    "NotoSerifDevanagari-Regular.ttf",
    "NotoSerifDevanagari-Bold.ttf",  "NotoSerifBengali-Regular.ttf",  "NotoSerifBengali-Bold.ttf",
    "NotoSerifTamil-Regular.ttf",    "NotoSerifTamil-Bold.ttf",       "NotoSerifTamil-Italic.ttf",
    "NotoSerifTamil-BoldItalic.ttf", "NotoSerifThai-Regular.ttf",     "NotoSerifThai-Bold.ttf"};
inline QString supplementalFontFamily(QString language) {
    language = language.toLower().replace('_', '-');
    const auto base = language.section('-', 0, 0);
    if (base == "ar" || base == "fa" || base == "ur")
        return "Noto Naskh Arabic";
    if (base == "he" || base == "yi")
        return "Noto Serif Hebrew";
    if (base == "hy")
        return "Noto Serif Armenian";
    if (base == "ka")
        return "Noto Serif Georgian";
    if (base == "hi" || base == "mr" || base == "ne" || base == "sa")
        return "Noto Serif Devanagari";
    if (base == "bn")
        return "Noto Serif Bengali";
    if (base == "ta")
        return "Noto Serif Tamil";
    if (base == "th")
        return "Noto Serif Thai";
    if (base == "ja")
        return "Noto Serif CJK JP";
    if (base == "ko")
        return "Noto Serif CJK KR";
    if (base == "zh") {
        if (language.contains("-hk") || language.contains("-mo"))
            return "Noto Serif CJK HK";
        if (language.contains("-hant") || language.contains("-tw"))
            return "Noto Serif CJK TC";
        return "Noto Serif CJK SC";
    }
    if (base == "el" || base == "ru" || base == "uk" || base == "bg" || base == "sr" || base == "mk" || base == "be")
        return "Noto Serif";
    return {};
}
class AssetBundle;
class FontCopies;
QStringList bundledFontFiles(const AssetBundle &assets);
EngineFontInputs engineFontInputs(const AssetBundle &assets, QString systemDirectory, QString sideloadDirectory,
                                  QString removableRoot);
// Call on the GUI thread. Keep copies alive for Qt's font registrations.
QString placeholderFontFamily(const AssetBundle &assets, FontCopies &copies);
}
