#include "text_presets.h"
#include "appearance_settings.h"
#include "reading_density.h"
#include "support/strings.h"

#include <algorithm>
#include <QUuid>
#include <QJsonArray>
#include <QSet>

namespace kobalt::textpresets {
namespace {
bool validAlignment(const QString &value)
{
    return value == "left" || value == "justify" || value == "publisher";
}

bool validHyphenation(const QString &value)
{
    return value == "off" || value == "on" || value == "auto";
}

bool integer(const QJsonValue &value, int minimum, int maximum)
{
    return value.isDouble() && value.toDouble() == value.toInt() && value.toInt() >= minimum
            && value.toInt() <= maximum;
}

QJsonObject toJson(const Preset &preset)
{
    QJsonObject object;
    object["id"] = preset.id;
    object["name"] = preset.name;
    object["font_face"] = preset.fontFace;
    object["half_points"] = preset.halfPoints;
    object["line_spacing"] = preset.lineSpacing;
    object["text_alignment"] = preset.alignment;
    object["hyphenation"] = preset.hyphenation;
    object["weight_steps"] = preset.weightSteps;
    object["font_weight"] = preset.fontWeight;
    object["font_optical_size"] = preset.opticalSize;
    if (preset.example)
        object["example"] = true;
    return object;
}

// Returns false for an entry that cannot be used. Optional fields that are
// missing or invalid use the defaults.
bool fromJson(const QJsonValue &value, Preset &preset)
{
    if (!value.isObject())
        return false;
    const auto object = value.toObject();
    const auto id = object.value("id").toString();
    const auto name = object.value("name").toString().simplified();
    // An empty face is Publisher Fonts, as in book state.
    const auto face = object.value("font_face").toString();
    if (id.isEmpty() || id.size() > 64 || name.isEmpty() || name.size() > maximumName
        || !object.value("font_face").isString() || face.size() > 512
        || !integer(object.value("half_points"), minimumHalfPoints, maximumHalfPoints))
        return false;
    preset = {};
    preset.id = id;
    preset.name = name;
    preset.fontFace = face;
    preset.halfPoints = object.value("half_points").toInt();
    const auto spacing = object.value("line_spacing");
    preset.lineSpacing =
            integer(spacing, 100, maximumLineSpacing) ? spacing.toInt() : defaultLineSpacing;
    const auto alignment = object.value("text_alignment").toString();
    preset.alignment = validAlignment(alignment) ? alignment : QStringLiteral("left");
    const auto hyphenation = object.value("hyphenation").toString();
    preset.hyphenation = validHyphenation(hyphenation) ? hyphenation : QStringLiteral("off");
    const auto steps = object.value("weight_steps");
    preset.weightSteps = integer(steps, 0, 12) ? steps.toInt() : 0;
    const auto weight = object.value("font_weight");
    preset.fontWeight = integer(weight, 100, 900) ? weight.toInt() : defaultVariableFontWeight;
    const auto optical = object.value("font_optical_size");
    preset.opticalSize = integer(optical, 0, 72) ? optical.toInt() : 0;
    preset.example = object.value("example").toBool(false);
    return true;
}

Preset example(const char *id, const char *label, const char *face, int halfPoints, int lineSpacing,
               const char *alignment, const char *hyphenation, int weightSteps = 0)
{
    Preset preset;
    preset.id = QString::fromLatin1(id);
    preset.name = strings::get(label);
    preset.fontFace = QString::fromLatin1(face);
    preset.halfPoints = halfPoints;
    preset.lineSpacing = lineSpacing;
    preset.alignment = QString::fromLatin1(alignment);
    preset.hyphenation = QString::fromLatin1(hyphenation);
    preset.fontWeight = defaultVariableFontWeight;
    preset.weightSteps = weightSteps;
    preset.example = true;
    return preset;
}
} // namespace

bool Preset::operator==(const Preset &other) const
{
    return id == other.id && name == other.name && fontFace == other.fontFace
            && alignment == other.alignment && hyphenation == other.hyphenation
            && halfPoints == other.halfPoints && lineSpacing == other.lineSpacing
            && weightSteps == other.weightSteps && fontWeight == other.fontWeight
            && opticalSize == other.opticalSize && example == other.example;
}

QVector<Preset> examples()
{
    return {
        // Kobalt matches the book defaults. Modern uses the same text in Jost.
        // Bookish and Classic use slightly larger, heavier text.
        example("kobalt", "text_presets.example_kobalt", defaultFontFace, 22, defaultLineSpacing,
                "left", "off"),
        example("bookish", "text_presets.example_bookish", "NV Ancizar Serif", 23, 140, "left",
                "off", 2),
        example("modern", "text_presets.example_modern", "NV Jost", 22, defaultLineSpacing, "left",
                "off"),
        example("classic", "text_presets.example_classic", "NV Garamond", 23, 145, "left", "off",
                // Garamond's thin strokes read better two steps heavier.
                2),
    };
}

QVector<Preset> restoreExamples(const QVector<Preset> &presets)
{
    const auto shipped = examples();
    QVector<Preset> own;
    for (const auto &preset : presets)
        if (!find(shipped, preset.id))
            own.append(preset);
    auto restored = own;
    restored += shipped.mid(0, qMax(0, maximumCount - int(own.size())));
    return restored;
}

QVector<Preset> load(const QJsonObject &preferences)
{
    if (!preferences.contains(listKey))
        return examples();
    QVector<Preset> presets;
    QSet<QString> ids;
    for (const auto &value : preferences.value(listKey).toArray()) {
        Preset preset;
        if (!fromJson(value, preset) || ids.contains(preset.id))
            continue;
        ids.insert(preset.id);
        presets.append(preset);
        if (presets.size() == maximumCount)
            break;
    }
    // Bundled ids survive renames and updates, including older updates that
    // cleared the example flag. Keep each group's saved order.
    const auto shipped = examples();
    std::stable_partition(presets.begin(), presets.end(),
                          [&shipped](const Preset &preset) { return !find(shipped, preset.id); });
    return presets;
}

void store(QJsonObject &preferences, const QVector<Preset> &presets)
{
    QJsonArray list;
    for (const auto &preset : presets)
        list.append(toJson(preset));
    preferences[listKey] = list;
}

void clean(QJsonObject &preferences)
{
    if (preferences.contains(listKey)) {
        if (!preferences.value(listKey).isArray())
            preferences.remove(listKey);
        else
            store(preferences, load(preferences));
    }
    if (preferences.contains(newBookKey)) {
        const auto value = preferences.value(newBookKey).toString();
        if (value.isEmpty() || value.size() > 64)
            preferences.remove(newBookKey);
    }
    if (preferences.contains(rowKey) && !preferences.value(rowKey).isBool())
        preferences.remove(rowKey);
}

Preset capture(const QJsonObject &state)
{
    Preset preset;
    preset.fontFace = state.value("font_face").toString(QString::fromLatin1(defaultFontFace));
    preset.halfPoints =
            qBound(minimumHalfPoints,
                   pixelHalfPoints(state.value("font_pixels").toInt(defaultFontPixels())),
                   maximumHalfPoints);
    preset.lineSpacing = state.value("line_spacing").toInt(defaultLineSpacing);
    const auto alignment = state.value("text_alignment").toString();
    preset.alignment = validAlignment(alignment) ? alignment : QStringLiteral("left");
    const auto hyphenation = state.value("hyphenation").toString();
    preset.hyphenation = validHyphenation(hyphenation) ? hyphenation : QStringLiteral("off");
    preset.weightSteps = state.value("weight_steps").toInt(0);
    preset.fontWeight = state.value("font_weight").toInt(defaultVariableFontWeight);
    preset.opticalSize = state.value("font_optical_size").toInt(0);
    return preset;
}

QJsonObject values(const Preset &preset)
{
    QJsonObject result;
    result["font_face"] = preset.fontFace;
    result["font_pixels"] = halfPointPixels(preset.halfPoints);
    result["line_spacing"] = preset.lineSpacing;
    result["line_spacing_version"] = 1;
    result["text_alignment"] = preset.alignment;
    result["hyphenation"] = preset.hyphenation;
    result["weight_steps"] = preset.weightSteps;
    result["font_weight"] = preset.fontWeight;
    result["font_optical_size"] = preset.opticalSize;
    return result;
}

bool matches(const Preset &preset, const QJsonObject &state)
{
    auto current = capture(state);
    // At low display densities two half-point sizes can render at the same pixel size.
    if (state.value("font_pixels").toInt(defaultFontPixels()) == halfPointPixels(preset.halfPoints))
        current.halfPoints = preset.halfPoints;
    current.id = preset.id;
    current.name = preset.name;
    current.example = preset.example;
    return current == preset;
}

const Preset *find(const QVector<Preset> &presets, const QString &id)
{
    for (const auto &preset : presets)
        if (preset.id == id)
            return &preset;
    return nullptr;
}

QString newId(const QVector<Preset> &presets)
{
    // Ids are never reused: a book keeps the id of the preset it applied, and
    // a new preset must not take over a deleted one's books.
    for (;;) {
        const auto id =
                QStringLiteral("preset-") + QUuid::createUuid().toString().remove('{').remove('}');
        if (!find(presets, id))
            return id;
    }
}

bool nameTaken(const QVector<Preset> &presets, const QString &name, const QString &exceptId)
{
    const auto wanted = name.simplified();
    for (const auto &preset : presets)
        if (preset.id != exceptId && preset.name.compare(wanted, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QString suggestedName(const QVector<Preset> &presets, const Preset &preset)
{
    auto face =
            preset.fontFace.isEmpty() ? strings::get("reader.publisher_fonts") : preset.fontFace;
    if (face.startsWith("NV ") || face.startsWith("KF "))
        face = face.mid(3);
    const auto size = QString::number(preset.halfPoints / 2.0, 'f', preset.halfPoints % 2);
    const auto base = QStringLiteral("%1 %2").arg(face, size).left(maximumName).trimmed();
    if (!nameTaken(presets, base))
        return base;
    for (int number = 2;; ++number) {
        const auto suffix = QStringLiteral(" (%1)").arg(number);
        const auto name = base.left(maximumName - suffix.size()).trimmed() + suffix;
        if (!nameTaken(presets, name))
            return name;
    }
}

QString newBookChoice(const QJsonObject &preferences)
{
    const auto value = preferences.value(newBookKey).toString();
    if (value == "book" || value == "last")
        return value;
    if (!value.isEmpty() && find(load(preferences), value))
        return value;
    return preferences.value(appearance::inheritTextLayoutKey).toBool(true)
            ? QStringLiteral("last")
            : QStringLiteral("book");
}

QString sizeLabel(int halfPoints)
{
    return strings::get("reader.text_size_value")
            .arg(QString::number(halfPoints / 2.0, 'f', halfPoints % 2));
}
} // namespace kobalt::textpresets
