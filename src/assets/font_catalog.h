#pragma once
#include <QStringList>
#include <QVector>
#include <cmath>

namespace kobalt {
enum class FontOrigin { Recommended, BuiltIn, Sideloaded, Interface };

struct FontWeightAxis
{
    int minimum = 0, maximum = 0, defaultValue = 400;

    bool available() const { return minimum < maximum; }
};

struct FontOpticalSizeAxis
{
    double minimum = 0, maximum = 0, defaultValue = 0;

    bool available() const
    {
        return minimum < maximum && std::ceil(minimum) <= std::floor(maximum);
    }
};

struct ReadingFont
{
    QString family;
    QStringList files;
    FontOrigin origin;
    FontWeightAxis weightAxis;
    FontOpticalSizeAxis opticalSizeAxis;
};

struct FontSources
{
    QStringList bundled;
    QString systemDirectory, sideloadDirectory;
    QStringList systemFiles = {};
    bool operator==(const FontSources &other) const;
};

struct EngineFontInputs
{
    FontSources sources;
    QStringList fallbackFiles;
    QString hyphenationDirectory;
    bool operator==(const EngineFontInputs &other) const;
};

// Each source owns all styles of a family. Lower-priority copies are ignored.
// Empty directories disable discovery for that source.
QVector<ReadingFont> discoverReadingFonts(const FontSources &sources);
// Plain font files in the directory and up to two folder levels below it.
// Kobo's wrapped fonts are skipped. An empty directory returns no files.
QStringList sideloadedFontFiles(const QString &directory);

inline QString fontDisplayName(const QString &family)
{
    return family.startsWith("NV ") ? family.mid(3) : family;
}

// Filter eligible picker rows only, never the registration catalog. Selected
// families remain visible. Use source groups, Libron first, then family name.
QVector<ReadingFont> orderedReadingFonts(QVector<ReadingFont> fonts,
                                         const QStringList &selected = {});
QString fontFamilyKey(const QString &family);
double normalizedOpticalSize(const FontOpticalSizeAxis &axis, double points);
FontOpticalSizeAxis fontOpticalSizeAxis(const QVector<ReadingFont> &fonts, const QString &family);
FontWeightAxis fontWeightAxis(const QVector<ReadingFont> &fonts, const QString &family);
} // namespace kobalt
