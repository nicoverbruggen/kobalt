#pragma once
#include <QJsonObject>

namespace kobalt::features {
inline constexpr const char *keys[] = { "disable_navigate", "disable_stats", "disable_annotations",
                                        "disable_bookmarks", "disable_kosync" };

enum Feature { Navigate, Stats, Annotations, Bookmarks, Kosync };

inline bool enabled(const QJsonObject &preferences, Feature feature)
{
    return !preferences.value(keys[feature]).toBool(false);
}

// Normalize dependent choices together with the switches, including profiles
// loaded from disk. Re-enabling a feature starts with its default options.
inline void normalize(QJsonObject &preferences)
{
    for (const auto *key : keys)
        if (preferences.contains(key) && !preferences.value(key).isBool())
            preferences.remove(key);
    const auto view = preferences.value("default_menu_view").toString();
    if ((!enabled(preferences, Navigate) && view == "seek")
        || (!enabled(preferences, Stats) && view == "stats"))
        preferences["default_menu_view"] = QStringLiteral("none");
    if (!enabled(preferences, Navigate))
        preferences.remove("navigation_bar_buttons");
    if (!enabled(preferences, Stats))
        preferences.remove("dense_stats_chart");
    if (!enabled(preferences, Annotations))
        preferences.remove("minimal_annotation_mode");
    if (!enabled(preferences, Annotations) && !enabled(preferences, Bookmarks))
        preferences.remove("marks_sort");
}
} // namespace kobalt::features
