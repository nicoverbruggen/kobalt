#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace kobalt::textpresets {
// Global settings: the saved presets, in the order the Text panel shows them.
inline constexpr auto listKey = "text_presets";
// Global settings: what a new book starts with: "book" for the book defaults,
// "last" for the last book's text, or a preset id.
inline constexpr auto newBookKey = "new_book_text";
// Global settings: whether the Text panel shows the preset cards.
inline constexpr auto rowKey = "text_presets_open";
// Book state: the id of the preset last applied to this book.
inline constexpr auto bookKey = "text_preset";

inline constexpr int maximumCount = 8;
inline constexpr int maximumName = 24;

// A saved set of text choices. It holds only typography, never margins or
// Styling, and stores the size in half points so it looks the same at every
// screen density. Applying a preset copies its values into the book; a later
// edit of the preset does not change books that used it.
struct Preset
{
    QString id, name;
    QString fontFace, alignment, hyphenation;
    int halfPoints = 22, lineSpacing = 150, weightSteps = 0, fontWeight = 450, opticalSize = 0;
    bool example = false;

    bool operator==(const Preset &other) const;

    bool operator!=(const Preset &other) const { return !(*this == other); }
};

// The examples that ship with Kobalt. Comfort matches the book defaults.
QVector<Preset> examples();
// The saved list. A profile that never saved one gets the examples.
QVector<Preset> load(const QJsonObject &preferences);
void store(QJsonObject &preferences, const QVector<Preset> &presets);
// Removes invalid entries and keeps at most maximumCount, so a damaged list
// never blocks other settings. Used by the settings cleanup.
void clean(QJsonObject &preferences);

// The preset values of a reader state, which holds typography in device pixels.
Preset capture(const QJsonObject &state);
// The reader state keys a preset sets, in device pixels.
QJsonObject values(const Preset &preset);
// True when the state shows exactly this preset's text.
bool matches(const Preset &preset, const QJsonObject &state);

const Preset *find(const QVector<Preset> &presets, const QString &id);
// A new id that no preset in the list uses.
QString newId(const QVector<Preset> &presets);
// A name for a new preset that no other preset uses, such as "Libron 13".
QString suggestedName(const QVector<Preset> &presets, const Preset &preset);
bool nameTaken(const QVector<Preset> &presets, const QString &name, const QString &exceptId = {});

// "book", "last" or a preset id that exists. Falls back to the legacy
// inherit_text_layout choice.
QString newBookChoice(const QJsonObject &preferences);

// The displayed size, such as "11 pt" or "10.5 pt".
QString sizeLabel(int halfPoints);
} // namespace kobalt::textpresets
