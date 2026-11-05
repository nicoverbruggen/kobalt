#pragma once
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace kobalt::strings {
struct Entry
{
    const char *key;
    const char *value;
};

inline const Entry *entries(int *count)
{
    static const Entry values[] = {
#include <kobalt_strings.inc>
    };
    *count = sizeof(values) / sizeof(values[0]);
    return values;
}

inline std::atomic_bool &keyDisplay()
{
    static std::atomic_bool enabled(qgetenv("KOBALT_SHOW_STRING_KEYS") == "1");
    return enabled;
}

// Tests can exercise real widgets without coupling assertions to editable copy.
inline void showKeys(bool enabled)
{
    keyDisplay().store(enabled);
}

// Mark deferred display-copy keys without translating them at the producer.
inline constexpr const char *key(const char *value)
{
    return value;
}

inline QString get(const char *key)
{
    int count = 0;
    const auto *begin = entries(&count);
    const auto *entry =
            std::lower_bound(begin, begin + count, key, [](const Entry &item, const char *name) {
                return std::strcmp(item.key, name) < 0;
            });
    if (entry == begin + count || std::strcmp(entry->key, key) != 0)
        return QString::fromLatin1(key);
    const QString value = QString::fromUtf8(entry->value);
    if (!keyDisplay().load())
        return value;
    QString result = QString::fromLatin1(key);
    // Retain argument slots so key mode also exercises formatted messages.
    const QRegularExpression placeholders("%[1-9][0-9]?");
    auto matches = placeholders.globalMatch(value);
    QStringList seen;
    while (matches.hasNext()) {
        const auto slot = matches.next().captured();
        if (!seen.contains(slot))
            seen.append(slot);
    }
    if (!seen.isEmpty())
        result += " [" + seen.join(", ") + "]";
    return result;
}
} // namespace kobalt::strings
