#include "native_event_history.h"
#include "platforms/kobo/common/native_symbol.h"
#include "support/main_thread.h"
#include <QCoreApplication>
#include <QThread>
#include <limits>
#include <utility>

namespace kobalt {
void (*nativeEventSetExtraData)(void *, QVariantMap);

namespace {
const QString timestamps = QStringLiteral("eventTimestamps");
const QString wordCounts = QStringLiteral("wordCounts");

bool unsignedValue(const QVariant &value, uint &result)
{
    switch (value.userType()) {
    case QMetaType::Int:
    case QMetaType::LongLong:
        if (value.toLongLong() < 0)
            return false;
        break;
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        break;
    default:
        return false;
    }
    const auto number = value.toULongLong();
    if (number > std::numeric_limits<uint>::max())
        return false;
    result = uint(number);
    return true;
}

bool orderedTimes(const QVariantList &values)
{
    uint previous = 0;
    for (const auto &value : values) {
        uint time;
        if (!unsignedValue(value, time) || !time || time < previous)
            return false;
        previous = time;
    }
    return true;
}

// Event is a virtual ScObject and a shared-data pointer on both firmware
// families. Keep it nontrivial so the native return uses the hidden pointer.
struct Event
{
    void *words[2];
    Event(const Event &) = delete;

    ~Event() { symbol<void (*)(void *)>("_ZN5EventD1Ev")(this); }
};

static_assert(sizeof(Event) == 8);

struct Api
{
    QObject *(*engine)() = symbol<decltype(engine)>("_ZN11EventEngine14sharedInstanceEv");
    const int &(*type)(const void *) = symbol<decltype(type)>("_ZNK5Event4typeEv");
    QString (*contentId)(const void *) = symbol<decltype(contentId)>("_ZNK5Event9contentIdEv");
    Event (*get)(const int &, const QString &, const QString &) = symbol<decltype(get)>(
            "_ZN12EventManager13eventWithTypeERK15SocialEventTypeRK7QStringS5_");
    QVariantMap (*extra)(const void *) = symbol<decltype(extra)>("_ZNK5Event9extraDataEv");

    bool ready() const
    {
        return engine && type && contentId && get && extra && symbol<void *>("_ZN5EventD1Ev");
    }
};

void prune(void *event, QVariantMap &data)
{
    // The base handler first adds a timestamp; the page handler then adds
    // its word count. Skip the intermediate, unpaired lists. Other updates
    // may also pass a complete map, such as a new session resetting its
    // counter. Let Nickel cache and persist this same updated map.
    const auto times = data.value(timestamps);
    const auto words = data.value(wordCounts);
    if (times.userType() != QMetaType::QVariantList || words.userType() != QMetaType::QVariantList
        || times.toList().size() < nickel::pageHistoryTrigger
        || times.toList().size() != words.toList().size())
        return;
    if (!qApp || QThread::currentThread() == qApp->thread())
        return;
    static const Api api;
    if (!api.ready() || api.type(event) != 46)
        return;
    auto *engine = api.engine();
    if (!engine || engine->thread() != QThread::currentThread())
        return;
    const auto id = api.contentId(event);
    if (id.isEmpty())
        return;
    // An empty database name selects Nickel's default connection and its
    // transient cache, including session markers that have not flushed yet.
    mainthread::blocking("nickel-event-history-markers");
    const auto starts = api.get(1020, id, QString());
    const auto ends = api.get(1021, id, QString());
    const int prefix = nickel::pageHistoryPrefix(data, api.extra(&starts), api.extra(&ends));
    if (prefix) {
        data[timestamps] = times.toList().mid(prefix);
        data[wordCounts] = words.toList().mid(prefix);
    }
}
} // namespace

int nickel::pageHistoryPrefix(const QVariantMap &pages, const QVariantMap &starts,
                              const QVariantMap &ends)
{
    const auto timeValue = pages.value(timestamps);
    const auto wordValue = pages.value(wordCounts);
    const auto startValue = starts.value(timestamps);
    const auto endValue = ends.value(timestamps);
    if (timeValue.userType() != QMetaType::QVariantList
        || wordValue.userType() != QMetaType::QVariantList
        || startValue.userType() != QMetaType::QVariantList
        || endValue.userType() != QMetaType::QVariantList)
        return 0;
    const auto times = timeValue.toList();
    const auto words = wordValue.toList();
    const auto opened = startValue.toList();
    const auto closed = endValue.toList();
    if (times.size() < pageHistoryTrigger || times.size() != words.size() || !orderedTimes(times)
        || !orderedTimes(opened) || !orderedTimes(closed))
        return 0;
    for (const auto &word : words) {
        uint count;
        if (!unsignedValue(word, count))
            return 0;
    }
    const uint target = times.at(times.size() - pageHistoryRetained).toUInt();
    uint boundary = 0;
    for (const auto &start : opened) {
        if (start.toUInt() >= target)
            break;
        boundary = start.toUInt();
    }
    if (!boundary)
        return 0;
    int prefix = 0;
    while (prefix < times.size() && times.at(prefix).toUInt() < boundary)
        ++prefix;
    // Equal timestamps have ambiguous event ordering. Never invent a page
    // duration by cutting through that tie or through a session.
    if (!prefix || prefix == times.size() || times.at(prefix).toUInt() == boundary)
        return 0;
    const uint first = times.at(prefix).toUInt();
    for (const auto &end : closed) {
        const uint time = end.toUInt();
        if (time >= boundary && time <= first)
            return 0;
    }
    return prefix;
}
} // namespace kobalt

extern "C" __attribute__((visibility("default"))) void kobalt_event_set_extra_data(void *event,
                                                                                   QVariantMap data)
{
    kobalt::prune(event, data);
    kobalt::nativeEventSetExtraData(event, std::move(data));
}
