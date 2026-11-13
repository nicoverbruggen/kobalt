#pragma once
#include <QJsonObject>
#include <QString>

// Rendering in Settings: Accurate lays out the whole book before it opens.
// Hybrid opens with only the chapter on screen laid out, lays out the whole
// book in idle time and shows it from the next page turn. Fast is Hybrid
// without the whole book. Settings does not offer it for now; tests and the
// environment still select it. Settings can also apply the choice to the
// open book.
namespace kobalt::renderingMode {
inline constexpr auto preferenceKey = "rendering_mode";

enum class Mode { Fast, Accurate, Hybrid };

// A missing or invalid value is Hybrid, and so is a saved "fast".
Mode fromPreferences(const QJsonObject &preferences);
QString value(Mode mode);

// The mode that the next book opens in. An override from setOverride() wins.
// Then KOBALT_CHAPTER_MODE in the environment: 1 is Fast, 2 is Hybrid, any
// other value is Accurate. Then the preference.
Mode effective(Mode preferred);

// True when the books that open from now on count their real pages in idle
// time.
bool countsInBackground();

// Hybrid starts the build of the whole book only after this long without
// input, so that its start does not meet a page turn. Tests set a short
// time.
int buildQuietMilliseconds();
void setBuildQuietMilliseconds(int milliseconds);

// Selects the mode of the books that open from now on. Hosts call it with the
// saved preferences before they open a book.
void apply(const QJsonObject &preferences);

// For tests and the benchmark: 2 is Hybrid, 1 is Fast, 0 is Accurate, -1
// removes the override.
void setOverride(int mode);
} // namespace kobalt::renderingMode
