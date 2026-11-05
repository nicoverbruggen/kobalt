#include "platforms/kobo/common/native_session_volume.h"
#include "platforms/kobo/nickel-qt5/native_location.h"
#include "check.h"
#include <QVariantMap>

namespace {
// QVariantMap shares its data until a setter detaches it, like Nickel's Volume.
QVariantMap eventVolume(const QVariantMap &persisted, const kobalt::ReadingActivity &activity)
{
    auto volume = persisted;
    kobalt::nickel::prepareSessionVolume(
            volume, activity,
            [](QVariantMap &value, const QString &position) {
                const auto location = kobalt::nickel_qt5::stored(position);
                if (!location.isEmpty()) {
                    value["ChapterIDBookmarked"] = location;
                    // Nickel's bookmark setter can replace the percentage.
                    value["___PercentRead"] = 0;
                }
            },
            [](QVariantMap &value, double percent) { value["___PercentRead"] = percent; });
    return volume;
}
} // namespace

int main()
{
    const QVariantMap persisted{ { "ChapterIDBookmarked", "OEBPS/index00047.xhtml#kobo.55.2" },
                                 { "___PercentRead", 70 },
                                 { "TimeSpentReading", 100 } };
    kobalt::ReadingActivity activity;
    activity.percent = 70;
    activity.nativePosition = "OEBPS/index00047.xhtml#kobo.55.2:0";
    const auto opening = eventVolume(persisted, activity);
    activity.percent = 72;
    activity.nativePosition = "OEBPS/index00050.xhtml#kobo.1.1:4";
    const auto closing = eventVolume(persisted, activity);
    require(opening.value("___PercentRead").toDouble() == 70
                    && closing.value("___PercentRead").toDouble() == 72,
            "The closing event retained the opening percentage");
    require(closing.value("ChapterIDBookmarked").toString() == "OEBPS/index00050.xhtml#kobo.1.1",
            "The closing event retained the opening bookmark");
    require(persisted.value("___PercentRead").toInt() == 70
                    && persisted.value("ChapterIDBookmarked")
                            == opening.value("ChapterIDBookmarked"),
            "Preparing session events changed persisted progress");
    require(closing.value("TimeSpentReading").toInt() == 100,
            "Preparing session events changed reading time");
    // An unmappable page still reports its percentage without inventing a bookmark.
    for (const auto &position : { QString(), QString("chapter.xhtml#point(invalid)") }) {
        activity.nativePosition = position;
        const auto event = eventVolume(persisted, activity);
        require(event.value("___PercentRead").toDouble() == 72
                        && event.value("ChapterIDBookmarked")
                                == persisted.value("ChapterIDBookmarked"),
                "An unavailable mapping fabricated or removed the native bookmark");
    }
    // Match the existing native writeback policy without changing UI percentages.
    for (const auto input : { -3.0, 21.782, 99.5, 100.0 }) {
        activity.percent = input;
        const auto event = eventVolume(persisted, activity);
        require(event.value("___PercentRead").toDouble() == qBound(0.0, input, 99.0),
                "Session analytics changed the native percentage policy");
    }
    // A wake starts from the displayed page even if persistence previously failed.
    activity.percent = 72;
    activity.nativePosition = "OEBPS/index00050.xhtml#kobo.1.1:4";
    const auto resumed = eventVolume(persisted, activity);
    require(resumed == closing, "A resumed interval reverted to stale persisted progress");
}
