#pragma once
#include <QJsonObject>
#include <QString>

// Rendering in Settings: Fast lays out only the chapter on screen, Accurate
// lays out the whole book. The choice applies when a book opens.
namespace kobalt::renderingMode {
inline constexpr auto preferenceKey = "rendering_mode";

enum class Mode { Fast, Accurate };

// A missing or invalid value is Fast.
Mode fromPreferences(const QJsonObject &preferences);
QString value(Mode mode);

// The mode that the next book opens in. An override from setOverride() wins.
// Then KOBALT_CHAPTER_MODE in the environment: 1 is Fast, any other value is
// Accurate. Then the preference.
Mode effective(Mode preferred);

// Selects the mode of the books that open from now on. Hosts call it with the
// saved preferences before they open a book.
void apply(const QJsonObject &preferences);

// For tests and the benchmark: 1 is Fast, 0 is Accurate, -1 removes the
// override.
void setOverride(int mode);
} // namespace kobalt::renderingMode
