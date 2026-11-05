#include "font_catalog.h"
#include <QDir>
#include <QFile>
#include <QMap>
#include <algorithm>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

namespace kobalt {
QVector<ReadingFont> orderedReadingFonts(QVector<ReadingFont> fonts, const QStringList &selected)
{
    const auto available = fonts;
    fonts.erase(std::remove_if(fonts.begin(), fonts.end(),
                               [&](const ReadingFont &font) {
                                   if (!font.family.startsWith("KF ")
                                       || selected.contains(font.family, Qt::CaseInsensitive))
                                       return false;
                                   // ebook-fonts' Kobo preset replaces NV with KF. Its unprefixed
                                   // families also gain KF. Match complete family names, not display
                                   // labels or filenames, and require an available counterpart.
                                   const auto base = font.family.mid(3);
                                   const bool unprefixed = base == "Libron" || base == "Sourcerer"
                                           || base == "Cartisse" || base == "Readerly";
                                   return std::any_of(
                                           available.cbegin(), available.cend(),
                                           [&](const ReadingFont &other) {
                                               return other.family.compare("NV " + base,
                                                                           Qt::CaseInsensitive)
                                                       == 0
                                                       || (unprefixed
                                                           && other.family.compare(
                                                                      base, Qt::CaseInsensitive)
                                                                   == 0);
                                           });
                               }),
                fonts.end());
    std::stable_sort(fonts.begin(), fonts.end(), [](const auto &a, const auto &b) {
        if (a.origin != b.origin)
            return a.origin < b.origin;
        if (a.origin == FontOrigin::Recommended && (a.family == "Libron") != (b.family == "Libron"))
            return a.family == "Libron";
        return a.family.compare(b.family, Qt::CaseInsensitive) < 0;
    });
    return fonts;
}

namespace {
// Sideloaded families often keep their own folder, and downloaded families
// often nest their files one level further, as in Family/static/. Hidden
// entries, such as macOS `._` files and `.Trashes`, are skipped.
constexpr int sideloadFolderDepth = 2;

void collectSideloadedFonts(const QDir &directory, int depth, QStringList &result)
{
    for (const auto &name : directory.entryList({ "*.ttf", "*.otf" }, QDir::Files, QDir::Name)) {
        const auto path = directory.filePath(name);
        QFile font(path);
        if (!font.open(QIODevice::ReadOnly)) {
            continue;
        }
        // Kobo can place its own wrapped fonts here, as in fonts/kobo/.
        // FreeType cannot read them, and only firmware fonts are decoded.
        const auto header = font.peek(3);
        if (header == "QTE" || header == "QTD") {
            continue;
        }
        result.append(path);
    }
    if (depth >= sideloadFolderDepth) {
        return;
    }
    for (const auto &name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        collectSideloadedFonts(QDir(directory.filePath(name)), depth + 1, result);
    }
}
} // namespace

QStringList sideloadedFontFiles(const QString &directory)
{
    if (directory.isEmpty()) {
        return {};
    }
    QStringList result;
    collectSideloadedFonts(QDir(directory), 0, result);
    return result;
}

bool FontSources::operator==(const FontSources &other) const
{
    return bundled == other.bundled && systemDirectory == other.systemDirectory
            && sideloadDirectory == other.sideloadDirectory && systemFiles == other.systemFiles;
}

bool EngineFontInputs::operator==(const EngineFontInputs &other) const
{
    return sources == other.sources && fallbackFiles == other.fallbackFiles
            && hyphenationDirectory == other.hyphenationDirectory;
}

QString fontFamilyKey(const QString &family)
{
    auto key = family.normalized(QString::NormalizationForm_C).trimmed().toCaseFolded();
    // Earlier Kobalt builds shipped this family under its old collection name.
    if (key == "kf libron")
        key = "libron";
    return key;
}

FontWeightAxis fontWeightAxis(const QVector<ReadingFont> &fonts, const QString &family)
{
    for (const auto &font : fonts)
        if (fontFamilyKey(font.family) == fontFamilyKey(family))
            return font.weightAxis;
    return {};
}

double normalizedOpticalSize(const FontOpticalSizeAxis &axis, double points)
{
    if (!std::isfinite(points) || points <= 0)
        points = axis.defaultValue;
    if (!axis.available())
        return points > 0 ? std::round(points) : 0;
    return qBound(std::ceil(axis.minimum), std::round(points), std::floor(axis.maximum));
}

FontOpticalSizeAxis fontOpticalSizeAxis(const QVector<ReadingFont> &fonts, const QString &family)
{
    for (const auto &font : fonts)
        if (fontFamilyKey(font.family) == fontFamilyKey(family))
            return font.opticalSizeAxis;
    return {};
}

QVector<ReadingFont> discoverReadingFonts(const FontSources &sources)
{
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library))
        return {};
    QVector<ReadingFont> result;
    QMap<QString, int> seen;
    const auto add = [&](const QString &path, FontOrigin origin) {
        FT_Face face = nullptr;
        if (FT_New_Face(library, path.toUtf8().constData(), 0, &face))
            return;
        const auto family = QString::fromUtf8(face->family_name);
        FontWeightAxis axis;
        FontOpticalSizeAxis optical;
        FT_MM_Var *variations = nullptr;
        if (!FT_Get_MM_Var(face, &variations) && variations) {
            for (FT_UInt i = 0; i < variations->num_axis; ++i) {
                const auto &value = variations->axis[i];
                if (value.tag == FT_MAKE_TAG('w', 'g', 'h', 't'))
                    axis = { int(value.minimum / 65536), int(value.maximum / 65536),
                             int(value.def / 65536) };
                if (value.tag == FT_MAKE_TAG('o', 'p', 's', 'z'))
                    optical = { value.minimum / 65536.0, value.maximum / 65536.0,
                                value.def / 65536.0 };
            }
            FT_Done_MM_Var(library, variations);
        }
        FT_Done_Face(face);
        if (family.isEmpty())
            return;
        const auto key = fontFamilyKey(family);
        auto found = seen.constFind(key);
        if (found != seen.cend()) {
            auto &entry = result[*found];
            if (entry.origin == origin && !entry.files.contains(path)) {
                entry.files.append(path);
                if (entry.opticalSizeAxis.available() && optical.available()) {
                    auto &range = entry.opticalSizeAxis;
                    range.minimum = qMax(range.minimum, optical.minimum);
                    range.maximum = qMin(range.maximum, optical.maximum);
                    if (range.available())
                        range.defaultValue =
                                qBound(range.minimum, range.defaultValue, range.maximum);
                    else
                        range = {};
                } else
                    entry.opticalSizeAxis = {};
                if (entry.weightAxis.available() && axis.available()) {
                    entry.weightAxis.minimum = qMax(entry.weightAxis.minimum, axis.minimum);
                    entry.weightAxis.maximum = qMin(entry.weightAxis.maximum, axis.maximum);
                    if (entry.weightAxis.available())
                        entry.weightAxis.defaultValue =
                                qBound(entry.weightAxis.minimum, entry.weightAxis.defaultValue,
                                       entry.weightAxis.maximum);
                    else
                        entry.weightAxis = {};
                } else {
                    entry.weightAxis = {};
                }
            }
            return;
        }
        seen.insert(key, result.size());
        result.append({ family, { path }, origin, axis, optical });
    };
    const auto core = [](const QString &path) {
        return path.contains("/ebook-core/") || path.contains("/ebook-core-");
    };
    for (const auto &path : sources.bundled)
        if (core(path))
            add(path, FontOrigin::Recommended);
    for (const auto &path : sources.systemFiles) {
        add(path, FontOrigin::BuiltIn);
    }
    if (!sources.systemDirectory.isEmpty()) {
        const QDir directory(sources.systemDirectory);
        for (const auto &name :
             directory.entryList({ "*.ttf", "*.otf" }, QDir::Files, QDir::Name)) {
            add(directory.filePath(name), FontOrigin::BuiltIn);
        }
    }
    for (const auto &path : sideloadedFontFiles(sources.sideloadDirectory)) {
        add(path, FontOrigin::Sideloaded);
    }
    // The UI family remains available for renderer-generated image notices,
    // but it is not a recommended reading font from the Core Collection.
    for (const auto &path : sources.bundled)
        if (!core(path))
            add(path, FontOrigin::Interface);
    FT_Done_FreeType(library);
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.origin == b.origin ? a.family.compare(b.family, Qt::CaseInsensitive) < 0
                                    : a.origin < b.origin;
    });
    return result;
}
} // namespace kobalt
