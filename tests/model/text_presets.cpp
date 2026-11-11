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
    // A profile without a list starts with the three examples; Comfort is the book default.
    const auto shipped = load({});
    require(shipped.size() == 3 && shipped[0].id == "comfort" && shipped[1].id == "compact"
                    && shipped[2].id == "large" && shipped[0].example,
            "A new profile did not get the example presets");
    require(shipped[0].fontFace == defaultFontFace && shipped[0].halfPoints == 22
                    && shipped[0].lineSpacing == defaultLineSpacing,
            "Comfort no longer matches the book defaults");
    // An empty saved list stays empty: deleting every preset is respected.
    QJsonObject empty;
    store(empty, {});
    require(load(empty).isEmpty(), "Deleting every preset brought the examples back");

    // Values round-trip; invalid entries, duplicates and extras are dropped.
    auto preset = shipped[1];
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
    for (const int ppi : { 300, 212, 450 }) {
        readingPpi = ppi;
        auto state = values(shipped[2]);
        state["margin_pixels"] = 99;
        require(matches(shipped[2], state) && !matches(shipped[1], state),
                "A preset did not match its own values");
        require(capture(state).halfPoints == 30, "The size changed with screen density");
        state["line_spacing"] = 141;
        require(!matches(shipped[2], state), "A changed line spacing still matched");
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
    require(suggestedName(shipped, shipped[1]) == "Literata 10",
            "A suggested name kept the NV prefix");
    require(nameTaken(shipped, "  comfort ") && !nameTaken(shipped, "Comfort", "comfort"),
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
    auto choosing = qt::object({ { newBookKey, "large" } });
    require(newBookChoice(choosing) == "large", "A preset was not kept for new books");
    const auto defaults = appearance::bookDefaults(choosing);
    require(defaults.value("font_face").toString() == "NV Charis"
                    && defaults.value("font_pixels").toInt() == halfPointPixels(30)
                    && defaults.value("line_spacing").toInt() == 140,
            "New books did not start with the chosen preset");
    choosing[newBookKey] = "deleted";
    require(newBookChoice(choosing) == "last", "A deleted preset was still used for new books");
    return 0;
}
