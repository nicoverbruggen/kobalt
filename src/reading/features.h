#pragma once
#include <QJsonObject>

namespace kobalt::features {
inline constexpr const char *keys[] = { "disable_navigate",        "disable_stats",
                                        "disable_annotations",     "disable_bookmarks",
                                        "disable_kosync",          "disable_words",
                                        "disable_highlight_links", "disable_activity" };

enum Feature { Navigate, Stats, Annotations, Bookmarks, Kosync, Words, HighlightLinks, Activity };

// Settings > System > Developer. Debug mode shows the Debug tab, the debug
// fields of the header and footer picker and the Fast rendering engine.
inline constexpr auto debugModeKey = "debug_mode";

inline bool debugMode(const QJsonObject &preferences)
{
    return preferences.value(debugModeKey).toBool(false);
}

inline bool enabled(const QJsonObject &preferences, Feature feature)
{
    // Activity lives inside Stats. Keep its own preference for when Stats returns.
    return !preferences.value(keys[feature]).toBool(false)
            && (feature != Activity || !preferences.value(keys[Stats]).toBool(false));
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
    if (!enabled(preferences, Stats)) {
        preferences.remove("dense_stats_chart");
        preferences.remove("stats_word_scope");
    }
    if (!enabled(preferences, Annotations)) {
        preferences.remove("minimal_annotation_mode");
        preferences.remove("annotation_interaction");
    }
    if (!enabled(preferences, Annotations) && !enabled(preferences, Bookmarks)
        && !enabled(preferences, Words))
        preferences.remove("marks_sort");
    if (!enabled(preferences, Words))
        preferences.remove("save_looked_up_words");
}
} // namespace kobalt::features
