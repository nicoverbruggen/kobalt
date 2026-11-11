#include "reading/appearance_settings.h"
#include "reading/reading_density.h"
#include "reading/text_presets.h"
#include "support/qt_compat.h"
#include "check.h"
#include <QJsonArray>

using namespace kobalt;
using namespace kobalt::textpresets;

// Checks text presets: examples, cleanup of damaged lists, matching, density, names and the new-book choice.
int main()
{
    // A profile without a list starts with the four examples; Kobalt is the book default.
    const auto shipped = load({});
    require(shipped.size() == 4 && shipped[0].id == "kobalt" && shipped[1].id == "bookish"
                    && shipped[2].id == "modern" && shipped[3].id == "classic",
            "A new profile did not get the example presets");
    for (const auto &preset : shipped)
        require(preset.example, "A shipped preset is not marked as an example");
    require(shipped[0].name == "Kobalt" && shipped[1].name == "Bookish"
                    && shipped[2].name == "Modern" && shipped[3].name == "Classic"
                    && shipped[1].fontFace == "NV Ancizar Serif" && shipped[1].lineSpacing == 140
                    && shipped[2].fontFace == "NV Jost" && shipped[3].fontFace == "NV Garamond"
                    && shipped[3].halfPoints == 24,
            "The example presets changed their names or fonts");
    require(shipped[0].fontFace == defaultFontFace && shipped[0].halfPoints == 22
                    && shipped[0].lineSpacing == defaultLineSpacing,
            "Kobalt no longer matches the book defaults");
    // An empty saved list stays empty: deleting every preset is respected.
    QJsonObject empty;
    store(empty, {});
    require(load(empty).isEmpty(), "Deleting every preset brought the examples back");

    // Values round-trip; invalid entries, duplicates and extras are dropped.
    auto preset = shipped[2];
    preset.id = "mine";
    preset.name = "Evening";
    preset.weightSteps = 2;
    QJsonObject preferences;
    store(preferences, { preset });
    require(load(preferences) == QVector<Preset>{ preset }, "A preset did not round-trip");
    auto list = preferences.value(listKey).toArray();
    list.append(qt::object({ { "id", "mine" },
                             { "name", "Twin" },
                             { "font_face", "Libron" },
                             { "half_points", 22 } }));
    list.append(qt::object(
            { { "id", "bad" }, { "name", "" }, { "font_face", "Libron" }, { "half_points", 22 } }));
    list.append(qt::object({ { "id", "tiny" },
                             { "name", "Tiny" },
                             { "font_face", "Libron" },
                             { "half_points", 2 } }));
    list.append(QStringLiteral("not an object"));
    for (int index = 0; index < maximumCount + 2; ++index)
        list.append(qt::object({ { "id", QStringLiteral("x%1").arg(index) },
                                 { "name", QStringLiteral("X %1").arg(index) },
                                 { "font_face", "Libron" },
                                 { "half_points", 22 },
                                 { "text_alignment", "sideways" } }));
    preferences[listKey] = list;
    const auto cleaned = load(preferences);
    require(cleaned.size() == maximumCount && cleaned[0] == preset && cleaned[1].id == "x0"
                    && cleaned[1].alignment == "left",
            "A damaged list was not cleaned to its valid entries");
    auto settings = qt::object({ { listKey, "broken" }, { newBookKey, 7 }, { rowKey, "yes" } });
    clean(settings);
    require(!settings.contains(listKey) && !settings.contains(newBookKey)
                    && !settings.contains(rowKey),
            "Settings cleanup kept invalid preset fields");
    const auto normalized = appearance::cleanPreferences(qt::object({ { listKey, list } }));
    require(load(normalized).size() == maximumCount
                    && normalized.value(listKey).toArray().size() == maximumCount,
            "The settings cleanup did not normalize the preset list");

    // A state matches a preset when every value agrees, in any density.
    const auto previousPpi = readingPpi.load();
    auto large = shipped[2];
    large.id = "large";
    large.halfPoints = 30;
    for (const int ppi : { 300, 212, 450 }) {
        readingPpi = ppi;
        auto state = values(large);
        state["margin_pixels"] = 99;
        require(matches(large, state) && !matches(shipped[2], state),
                "A preset did not match its own values");
        require(capture(state).halfPoints == 30, "The size changed with screen density");
        state["line_spacing"] = 141;
        require(!matches(large, state), "A changed line spacing still matched");
    }
    readingPpi = previousPpi;

    // Ids and names stay unique.
    // Ids are never reused, so a book that applied a deleted preset does not
    // follow a new one.
    auto growing = shipped;
    for (int i = 0; i < 3; ++i) {
        auto added = shipped[0];
        added.id = newId(growing);
        require(!find(growing, added.id), "A new id was not free");
        growing.append(added);
    }
    const auto deleted = growing.takeLast().id;
    require(newId(growing) != deleted, "The id of a deleted preset was reused");
    auto named = shipped;
    named[0].name = "Libron 11";
    require(suggestedName(named, shipped[0]) == "Libron 11 (2)",
            "A suggested name repeated an existing one");
    require(suggestedName(shipped, shipped[2]) == "Jost 11", "A suggested name kept the NV prefix");
    require(nameTaken(shipped, "  kobalt ") && !nameTaken(shipped, "Kobalt", "kobalt"),
            "Names were not compared without case and spaces");

    // Publisher Fonts is an empty face. It saves, loads and gets a name.
    auto publisher = capture(qt::object({ { "font_face", "" } }));
    require(publisher.fontFace.isEmpty(), "Publisher Fonts was not captured");
    publisher.id = "publisher";
    publisher.name = suggestedName(shipped, publisher);
    require(!publisher.name.startsWith(' ') && publisher.name.size() > 3,
            "Publisher Fonts got no suggested name");
    QJsonObject withPublisher;
    store(withPublisher, { publisher });
    const auto reloaded = load(withPublisher);
    require(reloaded.size() == 1 && reloaded[0].fontFace.isEmpty() && reloaded[0] == publisher,
            "A preset with Publisher Fonts was dropped");
    auto missingFace = withPublisher.value(listKey).toArray();
    auto entry = missingFace[0].toObject();
    entry.remove("font_face");
    missingFace[0] = entry;
    withPublisher[listKey] = missingFace;
    require(load(withPublisher).isEmpty(), "A preset without a face was kept");

    // The new-book choice: a preset, the last book, or the book defaults.
    require(newBookChoice({}) == "last", "New books no longer follow the last book by default");
    require(newBookChoice(qt::object({ { appearance::inheritTextLayoutKey, false } })) == "book",
            "The old inheritance setting was not honoured");
    auto choosing = qt::object({ { newBookKey, "modern" } });
    require(newBookChoice(choosing) == "modern", "A preset was not kept for new books");
    const auto defaults = appearance::bookDefaults(choosing);
    require(defaults.value("font_face").toString() == "NV Jost"
                    && defaults.value("font_pixels").toInt() == halfPointPixels(22)
                    && defaults.value("line_spacing").toInt() == defaultLineSpacing,
            "New books did not start with the chosen preset");
    choosing[newBookKey] = "deleted";
    require(newBookChoice(choosing) == "last", "A deleted preset was still used for new books");

    // Restoring the examples brings back deleted ones and undoes changes to
    // them, first and in their order. The user's own presets follow in theirs.
    Preset mine = shipped[0];
    mine.id = "preset-mine";
    mine.name = "Mine";
    mine.example = false;
    Preset changedClassic = shipped[3];
    changedClassic.name = "Evening";
    changedClassic.halfPoints = 30;
    const auto restored = restoreExamples({ mine, changedClassic });
    require(restored.size() == 5 && restored.mid(0, 4) == shipped && restored[4] == mine,
            "Restoring did not put the examples back before the user's presets");
    // The user's presets are never dropped: only the examples that fit return.
    QVector<Preset> own;
    for (int index = 0; index < maximumCount - 1; ++index) {
        auto preset = mine;
        preset.id = newId(own);
        preset.name = QString("Mine %1").arg(index);
        own.append(preset);
    }
    const auto full = restoreExamples(own);
    require(full.size() == maximumCount && full[0] == shipped[0] && full.mid(1) == own,
            "Restoring dropped a user's preset or went past the limit");
    own.append(mine);
    require(restoreExamples(own) == own, "Restoring a full list changed it");
    return 0;
}
