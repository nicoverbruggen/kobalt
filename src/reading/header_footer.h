#pragma once
#include <QJsonObject>
#include <QString>
#include <array>

namespace kobalt::headerfooter {
// What one slot shows. Saved layouts name fields by fieldValue.
enum class Field {
    Nothing,
    BookTitle,
    Author,
    Chapter,
    ChapterPage,
    BookPage,
    ChapterPagesLeft,
    ChapterAndPage,
    TitleAndPage,
    BatteryPercent,
    Battery,
    Time,
    ChapterTimeLeft,
    BookTimeLeft,
    ChapterPercent,
    BookPercent,
    BatteryWarning,
};
// Every field, for reading saved layouts.
inline constexpr Field fields[] = {
    Field::Nothing,         Field::BookTitle,      Field::Author,           Field::Chapter,
    Field::ChapterPage,     Field::BookPage,       Field::ChapterPagesLeft, Field::ChapterAndPage,
    Field::TitleAndPage,    Field::BatteryPercent, Field::Battery,          Field::Time,
    Field::ChapterTimeLeft, Field::BookTimeLeft,   Field::ChapterPercent,   Field::BookPercent,
    Field::BatteryWarning,
};
// The fields a slot offers, grouped: the book, the chapter, the whole book,
// the device, then the fields that need a description to explain them.
inline constexpr Field choosableFields[] = {
    Field::Nothing,          Field::BookTitle,      Field::Author,         Field::Chapter,  Field::ChapterPage,
    Field::ChapterPagesLeft, Field::ChapterPercent, Field::ChapterAndPage, Field::BookPage, Field::BookPercent,
    Field::TitleAndPage,     Field::Time,           Field::BatteryPercent, Field::Battery,  Field::BatteryWarning,
    Field::ChapterTimeLeft,  Field::BookTimeLeft,
};
// Battery warning shows the battery only below this percentage.
inline constexpr int batteryWarningBelow = 20;

// Slots 0 to 2 are the header's left, centre and right. Slots 3 to 5 are the footer's.
inline constexpr int slotCount = 6;
inline constexpr bool footerSlot(int slot) {
    return slot >= 3;
}

enum class Preset { Default, AlternativeA, AlternativeB, Custom };
inline constexpr Preset presets[] = {Preset::Default, Preset::AlternativeA, Preset::AlternativeB, Preset::Custom};

// Everything a preset decides about the header and the footer.
struct Layout {
    std::array<Field, slotCount> slotFields{};
    // "off", "small", "medium" or "large".
    QString headerSize = QStringLiteral("small"), footerSize = QStringLiteral("small");
    // Each slot chooses its own case.
    std::array<bool, slotCount> uppercase{{true, true, true, true, true, true}};
    bool progressBar = true;
    // A strip is drawn when its size is not Off and one of its slots shows something.
    bool header() const;
    bool footer() const;
    bool operator==(const Layout &other) const;
    bool operator!=(const Layout &other) const {
        return !(*this == other);
    }
};

// The preset is saved per book. Custom is one layout shared by every book.
inline constexpr auto presetKey = "header_footer";
inline constexpr auto customKey = "header_footer_custom";

Layout presetLayout(Preset preset);
QString presetValue(Preset preset);
// An unknown value is Default.
Preset preset(const QString &value);
QString fieldValue(Field field);
// An unknown value is Nothing.
Field field(const QString &value);
QJsonObject toJson(const Layout &layout);
// Missing or unknown values keep the fallback's value.
Layout fromJson(const QJsonObject &object, const Layout &fallback);

struct Selection {
    Preset preset = Preset::Default;
    Layout layout;
};
// The preset a book uses and the layout it draws. A book saved before presets
// existed has no preset. Its old header, footer, clock and progress bar
// settings select Default when they match it, and Custom otherwise. Custom is
// then the saved custom layout, or those old settings when none is saved yet.
Selection select(const QJsonObject &preferences);
// The layout that the old header, footer, clock and progress bar settings describe.
Layout legacyLayout(const QJsonObject &preferences);
// The custom layout, or `fallback` when none is saved.
Layout customLayout(const QJsonObject &preferences, const Layout &fallback);

// What the reader knows about the book and the device. Pages count from 1.
// A negative estimate has no time to show. `learningPace` says why: the
// session has not read long enough for a pace, rather than an unknown length.
struct Values {
    QString title, author, chapter, time;
    int chapterPage = 0, chapterPages = 0, page = 0, pages = 0;
    int battery = -1;
    bool charging = false;
    // False on hosts that hide device status, such as the desktop app. It
    // hides the battery; the time still shows.
    bool deviceStatus = true;
    qint64 chapterMilliseconds = -1, bookMilliseconds = -1;
    bool learningPace = false;
};
// One slot's text, and a battery icon drawn after it when `batteryIcon` is set.
struct Content {
    QString text;
    bool batteryIcon = false;
    int battery = -1;
    bool charging = false;
    bool empty() const {
        return text.isEmpty() && !batteryIcon;
    }
    bool operator==(const Content &other) const {
        return text == other.text && batteryIcon == other.batteryIcon && battery == other.battery &&
               charging == other.charging;
    }
    bool operator!=(const Content &other) const {
        return !(*this == other);
    }
};
Content content(Field field, const Values &values, bool uppercase);
// A page count alone, such as 5 / 33, which is drawn with half-width spaces.
bool pageCount(Field field);

QString fieldLabel(Field field);
// A line that explains a field whose name does not, or an empty string.
QString fieldDescription(Field field);
QString slotLabel(int slot);
QString presetLabel(Preset preset);
}
