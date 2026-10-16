#include "header_footer.h"
#include "support/strings.h"

#include <QJsonArray>

namespace kobalt::headerfooter {
namespace {
QString stripSize(const QString &value, const QString &fallback) {
    return value == "off" || value == "small" || value == "medium" || value == "large" ? value : fallback;
}
bool anything(const Layout &layout, int first) {
    for (int slot = first; slot < first + 3; ++slot)
        if (layout.slotFields[slot] != Field::Nothing)
            return true;
    return false;
}
QString pageFraction(int page, int pages) {
    return pages > 0 ? strings::get("common.page_fraction").arg(page).arg(pages) : QString();
}
// Without a pace yet, say so instead of leaving the slot empty, as Kindle does.
QString timeLeft(qint64 milliseconds, bool learningPace) {
    if (milliseconds < 0)
        return learningPace ? strings::get("header_footer.learning_pace") : QString();
    const qint64 minutes = qMax<qint64>(1, (milliseconds + 30000) / 60000);
    const auto time = minutes < 60 ? strings::get("stats.duration_minutes").arg(minutes)
                                   : strings::get("stats.duration_hours").arg(minutes / 60).arg(minutes % 60);
    return strings::get("header_footer.time_left").arg(time);
}
// How far along: the last page reads 100%.
QString percentDone(int page, int pages) {
    return pages > 0 ? strings::get("common.percent").arg(qRound(100.0 * qBound(0, page, pages) / pages)) : QString();
}
// Beside a name the page reads 5 of 33, so the slash does not compete with
// the dot between the two parts.
QString pageOf(int page, int pages) {
    return pages > 0 ? strings::get("header_footer.page_of").arg(page).arg(pages) : QString();
}
QString joined(const QString &name, const QString &position) {
    if (name.isEmpty() || position.isEmpty())
        return name.isEmpty() ? position : name;
    return strings::get("reader.chapter_position").arg(name, position);
}
}

bool Layout::header() const {
    return headerSize != "off" && anything(*this, 0);
}
bool Layout::footer() const {
    return footerSize != "off" && anything(*this, 3);
}
bool Layout::operator==(const Layout &other) const {
    return slotFields == other.slotFields && headerSize == other.headerSize && footerSize == other.footerSize &&
           uppercase == other.uppercase && progressBar == other.progressBar;
}

Layout presetLayout(Preset preset) {
    Layout layout;
    switch (preset) {
    case Preset::Default:
    case Preset::Custom:
        layout.slotFields = {Field::Time,    Field::Chapter,  Field::ChapterPage,
                             Field::Nothing, Field::BookPage, Field::Nothing};
        break;
    case Preset::AlternativeA:
        layout.slotFields = {Field::Time,    Field::ChapterAndPage, Field::Nothing,
                             Field::Nothing, Field::TitleAndPage,   Field::Nothing};
        break;
    case Preset::AlternativeB:
        layout.slotFields = {Field::Nothing,          Field::Time,    Field::Nothing,
                             Field::ChapterPagesLeft, Field::Nothing, Field::Nothing};
        layout.uppercase.fill(false);
        layout.progressBar = false;
        break;
    }
    return layout;
}

QString presetValue(Preset preset) {
    switch (preset) {
    case Preset::Default:
        return QStringLiteral("default");
    case Preset::AlternativeA:
        return QStringLiteral("alternative_a");
    case Preset::AlternativeB:
        return QStringLiteral("alternative_b");
    case Preset::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("default");
}

Preset preset(const QString &value) {
    for (const auto preset : presets)
        if (presetValue(preset) == value)
            return preset;
    return Preset::Default;
}

QString fieldValue(Field field) {
    switch (field) {
    case Field::Nothing:
        return QStringLiteral("nothing");
    case Field::BookTitle:
        return QStringLiteral("title");
    case Field::Author:
        return QStringLiteral("author");
    case Field::Chapter:
        return QStringLiteral("chapter");
    case Field::ChapterPage:
        return QStringLiteral("chapter_page");
    case Field::BookPage:
        return QStringLiteral("book_page");
    case Field::ChapterPagesLeft:
        return QStringLiteral("chapter_pages_left");
    case Field::ChapterAndPage:
        return QStringLiteral("chapter_and_page");
    case Field::TitleAndPage:
        return QStringLiteral("title_and_page");
    case Field::BatteryPercent:
        return QStringLiteral("battery_percent");
    case Field::Battery:
        return QStringLiteral("battery");
    case Field::Time:
        return QStringLiteral("time");
    case Field::ChapterTimeLeft:
        return QStringLiteral("chapter_time_left");
    case Field::BookTimeLeft:
        return QStringLiteral("book_time_left");
    case Field::ChapterPercent:
        return QStringLiteral("chapter_percent");
    case Field::BookPercent:
        return QStringLiteral("book_percent");
    case Field::BatteryWarning:
        return QStringLiteral("battery_warning");
    }
    return QStringLiteral("nothing");
}

Field field(const QString &value) {
    for (const auto field : fields)
        if (fieldValue(field) == value)
            return field;
    return Field::Nothing;
}

QJsonObject toJson(const Layout &layout) {
    QJsonObject object;
    static const char *const names[slotCount] = {"top_left",    "top_center",    "top_right",
                                                 "bottom_left", "bottom_center", "bottom_right"};
    for (int slot = 0; slot < slotCount; ++slot)
        object[names[slot]] = fieldValue(layout.slotFields[slot]);
    object["header_size"] = layout.headerSize;
    object["footer_size"] = layout.footerSize;
    QJsonArray uppercase;
    for (const bool value : layout.uppercase)
        uppercase.append(value);
    object["uppercase"] = uppercase;
    object["progress_bar"] = layout.progressBar;
    return object;
}

Layout fromJson(const QJsonObject &object, const Layout &fallback) {
    auto layout = fallback;
    static const char *const names[slotCount] = {"top_left",    "top_center",    "top_right",
                                                 "bottom_left", "bottom_center", "bottom_right"};
    for (int slot = 0; slot < slotCount; ++slot)
        if (object.value(names[slot]).isString())
            layout.slotFields[slot] = field(object.value(names[slot]).toString());
    layout.headerSize = stripSize(object.value("header_size").toString(), fallback.headerSize);
    layout.footerSize = stripSize(object.value("footer_size").toString(), fallback.footerSize);
    // One value per slot. A single value applies to every slot.
    const auto uppercase = object.value("uppercase");
    if (uppercase.isBool())
        layout.uppercase.fill(uppercase.toBool());
    else if (uppercase.isArray())
        for (int slot = 0; slot < slotCount && slot < uppercase.toArray().size(); ++slot)
            if (uppercase.toArray().at(slot).isBool())
                layout.uppercase[slot] = uppercase.toArray().at(slot).toBool();
    layout.progressBar = object.value("progress_bar").toBool(fallback.progressBar);
    return layout;
}

Layout legacyLayout(const QJsonObject &preferences) {
    auto layout = presetLayout(Preset::Default);
    const bool header = preferences.value("show_header").toBool(true);
    const bool clock = preferences.value("show_clock").toBool(true);
    layout.slotFields[0] = clock ? Field::Time : Field::Nothing;
    if (!header) {
        // The old clock kept a strip of its own when the header was off.
        layout.slotFields[1] = layout.slotFields[2] = Field::Nothing;
    }
    // Old sizes were never Off; visibility had its own setting.
    const auto shownSize = [&preferences](const char *key) {
        const auto value = preferences.value(key).toString();
        return value == "medium" || value == "large" ? value : QStringLiteral("small");
    };
    layout.headerSize = header || clock ? shownSize("header_text_size") : QStringLiteral("off");
    layout.footerSize =
        preferences.value("show_footer").toBool(true) ? shownSize("footer_text_size") : QStringLiteral("off");
    layout.progressBar = preferences.value("show_progress_bar").toBool(true);
    return layout;
}

Layout customLayout(const QJsonObject &preferences, const Layout &fallback) {
    return preferences.value(customKey).isObject() ? fromJson(preferences.value(customKey).toObject(), fallback)
                                                   : fallback;
}

Selection select(const QJsonObject &preferences) {
    Selection selection;
    if (preferences.value(presetKey).isString()) {
        selection.preset = preset(preferences.value(presetKey).toString());
        selection.layout = selection.preset == Preset::Custom ? customLayout(preferences, presetLayout(Preset::Default))
                                                              : presetLayout(selection.preset);
        return selection;
    }
    const auto legacy = legacyLayout(preferences);
    if (legacy == presetLayout(Preset::Default)) {
        selection.layout = legacy;
        return selection;
    }
    selection.preset = Preset::Custom;
    selection.layout = customLayout(preferences, legacy);
    return selection;
}

Content content(Field field, const Values &values, bool uppercase) {
    Content result;
    switch (field) {
    case Field::Nothing:
        break;
    case Field::BookTitle:
        result.text = values.title;
        break;
    case Field::Author:
        result.text = values.author;
        break;
    case Field::Chapter:
        result.text = values.chapter;
        break;
    case Field::ChapterPage:
        result.text = pageFraction(values.chapterPage, values.chapterPages);
        break;
    case Field::BookPage:
        result.text = pageFraction(values.page, values.pages);
        break;
    case Field::ChapterPagesLeft:
        if (values.chapterPages > 0) {
            const int left = qMax(0, values.chapterPages - values.chapterPage);
            result.text = strings::get("header_footer.pages_left")
                              .arg(left)
                              .arg(strings::get(left == 1 ? "stats.page_left" : "stats.pages_left"));
        }
        break;
    case Field::ChapterAndPage:
        result.text = joined(values.chapter, pageOf(values.chapterPage, values.chapterPages));
        break;
    case Field::TitleAndPage:
        result.text = joined(values.title, pageOf(values.page, values.pages));
        break;
    case Field::BatteryPercent:
    case Field::Battery:
    case Field::BatteryWarning:
        if (field == Field::BatteryWarning && (values.battery < 0 || values.battery >= batteryWarningBelow))
            break;
        if (values.deviceStatus) {
            if (field != Field::Battery && values.battery >= 0)
                result.text = strings::get("common.percent").arg(values.battery);
            result.batteryIcon = true;
            result.battery = values.battery;
            result.charging = values.charging;
        }
        break;
    case Field::Time:
        // The time is useful on every host; only the battery is device status.
        result.text = values.time;
        break;
    case Field::ChapterTimeLeft:
        result.text = timeLeft(values.chapterMilliseconds, values.learningPace);
        break;
    case Field::BookTimeLeft:
        result.text = timeLeft(values.bookMilliseconds, values.learningPace);
        break;
    case Field::ChapterPercent:
        result.text = percentDone(values.chapterPage, values.chapterPages);
        break;
    case Field::BookPercent:
        result.text = percentDone(values.page, values.pages);
        break;
    }
    if (uppercase)
        result.text = result.text.toUpper();
    return result;
}

bool pageCount(Field field) {
    return field == Field::ChapterPage || field == Field::BookPage;
}

QString fieldLabel(Field field) {
    switch (field) {
    case Field::Nothing:
        return strings::get("header_footer.field.nothing");
    case Field::BookTitle:
        return strings::get("header_footer.field.title");
    case Field::Author:
        return strings::get("header_footer.field.author");
    case Field::Chapter:
        return strings::get("header_footer.field.chapter");
    case Field::ChapterPage:
        return strings::get("header_footer.field.chapter_page");
    case Field::BookPage:
        return strings::get("header_footer.field.book_page");
    case Field::ChapterPagesLeft:
        return strings::get("header_footer.field.chapter_pages_left");
    case Field::ChapterAndPage:
        return strings::get("header_footer.field.chapter_and_page");
    case Field::TitleAndPage:
        return strings::get("header_footer.field.title_and_page");
    case Field::BatteryPercent:
        return strings::get("header_footer.field.battery_percent");
    case Field::Battery:
        return strings::get("header_footer.field.battery");
    case Field::Time:
        return strings::get("header_footer.field.time");
    case Field::ChapterTimeLeft:
        return strings::get("header_footer.field.chapter_time_left");
    case Field::BookTimeLeft:
        return strings::get("header_footer.field.book_time_left");
    case Field::ChapterPercent:
        return strings::get("header_footer.field.chapter_percent");
    case Field::BookPercent:
        return strings::get("header_footer.field.book_percent");
    case Field::BatteryWarning:
        return strings::get("header_footer.field.battery_warning");
    }
    return {};
}

QString fieldDescription(Field field) {
    switch (field) {
    case Field::BatteryWarning:
        return strings::get("header_footer.field_description.battery_warning").arg(batteryWarningBelow);
    case Field::ChapterTimeLeft:
    case Field::BookTimeLeft:
        return strings::get("header_footer.field_description.time_left");
    default:
        return {};
    }
}

QString slotLabel(int slot) {
    static const char *const keys[slotCount] = {
        strings::key("header_footer.slot.top_left"),      strings::key("header_footer.slot.top_center"),
        strings::key("header_footer.slot.top_right"),     strings::key("header_footer.slot.bottom_left"),
        strings::key("header_footer.slot.bottom_center"), strings::key("header_footer.slot.bottom_right")};
    return slot >= 0 && slot < slotCount ? strings::get(keys[slot]) : QString();
}

QString presetLabel(Preset preset) {
    switch (preset) {
    case Preset::Default:
        return strings::get("header_footer.preset.default");
    case Preset::AlternativeA:
        return strings::get("header_footer.preset.alternative_a");
    case Preset::AlternativeB:
        return strings::get("header_footer.preset.alternative_b");
    case Preset::Custom:
        return strings::get("header_footer.preset.custom");
    }
    return {};
}
}
