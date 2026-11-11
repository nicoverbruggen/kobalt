#include "platforms/kobo/common/native_event_history.h"
#include <QCoreApplication>
#include <QThread>
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        std::_Exit(1);
    }
}

enum SocialEventType { PageTurn = 46, ReaderShown = 1020, ReaderHidden = 1021 };

struct EventData
{
    SocialEventType type;
    QVariantMap map;
};

// Match Event's two-word handle and nontrivial return ABI. Only the fake
// accessors dereference its data; the production adapter treats it as opaque.
class Event
{
public:
    explicit Event(EventData *data) : data(data) { }

    Event(const Event &) = delete;
    ~Event();
    const SocialEventType &type() const;
    QString contentId() const;
    QVariantMap extraData() const;
    void *unused = nullptr;
    EventData *data;
};

Event::~Event() = default;

const SocialEventType &Event::type() const
{
    return data->type;
}

QString Event::contentId() const
{
    return QStringLiteral("test-book");
}

QVariantMap Event::extraData() const
{
    return data->map;
}

class EventManager
{
public:
    static Event eventWithType(const SocialEventType &, const QString &, const QString &);
};

static EventData starts{ ReaderShown, {} }, ends{ ReaderHidden, {} };
static int reads = 0;

Event EventManager::eventWithType(const SocialEventType &type, const QString &id, const QString &db)
{
    check(id == "test-book" && db.isEmpty(), "wrong event identity or database");
    ++reads;
    return Event(int(type) == 1020 ? &starts : &ends);
}

class EventEngine
{
public:
    static QObject *sharedInstance();
};

static QObject *engine;

QObject *EventEngine::sharedInstance()
{
    return engine;
}

static QVariantMap history(int sessions, int turns)
{
    QVariantList times, words, opened, closed;
    uint time = 1700000000;
    for (int s = 0; s < sessions; ++s) {
        opened.append(time);
        for (int p = 0; p < turns; ++p) {
            time += 20;
            times.append(time);
            words.append(p);
        }
        closed.append(++time);
        time += 3600;
    }
    starts.map = { { "eventTimestamps", opened }, { "unknownMarkerField", "keep" } };
    ends.map = { { "eventTimestamps", closed } };
    return { { "eventTimestamps", times },        { "wordCounts", words },
             { "readingSeconds", 500000 },        { "readingSessions", sessions },
             { "pagesTurnedThisSession", turns }, { "unknown", QVariantList{ 42, "keep" } } };
}

static QVariantMap saved;

static void original(void *, QVariantMap data)
{
    saved = data;
}

class Worker : public QThread
{
public:
    EventData *data = nullptr;

    void run() override
    {
        Event event(data);
        kobalt_event_set_extra_data(&event, data->map);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using kobalt::nickel::pageHistoryPrefix;
    auto full = history(250, 100);
    const auto markers = starts.map;
    check(pageHistoryPrefix(full, starts.map, ends.map) == 20000, "25k should retain 5k");
    auto exact = history(100, 100);
    check(pageHistoryPrefix(exact, starts.map, ends.map) == 5000, "10k must trigger");
    auto below = history(99, 101);
    check(pageHistoryPrefix(below, starts.map, ends.map) == 0, "9999 must not trigger");
    auto uneven = history(120, 83);
    check(pageHistoryPrefix(uneven, starts.map, ends.map) == 0, "below trigger");
    uneven = history(121, 83);
    const int cut = pageHistoryPrefix(uneven, starts.map, ends.map);
    check(cut % 83 == 0 && 10043 - cut == 5063, "retain whole boundary session");
    auto longSession = history(1, 25000);
    check(pageHistoryPrefix(longSession, starts.map, ends.map) == 0, "do not split long session");
    full = history(250, 100);
    auto broken = full;
    broken["wordCounts"] = full.value("wordCounts").toList().mid(1);
    check(pageHistoryPrefix(broken, starts.map, ends.map) == 0, "mismatched lists");
    auto times = full.value("eventTimestamps").toList();
    times[20000] = "1700000000";
    broken = full;
    broken["eventTimestamps"] = times;
    check(pageHistoryPrefix(broken, starts.map, ends.map) == 0, "string timestamp");
    times = full.value("eventTimestamps").toList();
    times[100] = 1u;
    broken["eventTimestamps"] = times;
    check(pageHistoryPrefix(broken, starts.map, ends.map) == 0, "unordered timestamps");
    times = full.value("eventTimestamps").toList();
    times[19999] = starts.map.value("eventTimestamps").toList().at(200);
    broken["eventTimestamps"] = times;
    check(pageHistoryPrefix(broken, starts.map, ends.map) == 0, "ambiguous page/start tie");
    auto badEnds = ends.map;
    auto closed = badEnds.value("eventTimestamps").toList();
    closed[199] = starts.map.value("eventTimestamps").toList().at(200);
    badEnds["eventTimestamps"] = closed;
    check(pageHistoryPrefix(full, starts.map, badEnds) == 0, "ambiguous end/start tie");
    check(pageHistoryPrefix(full, {}, ends.map) == 0, "missing starts");
    for (const auto &badWord :
         QVariantList{ -1, 1.5, "100", QVariant::fromValue<qulonglong>(1ULL << 32) }) {
        broken = full;
        auto words = full.value("wordCounts").toList();
        words[0] = badWord;
        broken["wordCounts"] = words;
        check(pageHistoryPrefix(broken, starts.map, ends.map) == 0, "invalid word count");
    }
    const auto endMarkers = ends.map;
    kobalt::nativeEventSetExtraData = original;
    EventData data{ PageTurn, full };
    Event event(&data);
    QObject eventEngine;
    engine = &eventEngine;
    Worker worker;
    worker.data = &data;
    eventEngine.moveToThread(&worker);
    kobalt_event_set_extra_data(&event, full);
    check(saved == full && reads == 0, "GUI thread must pass through without reading markers");
    Worker wrongThread;
    wrongThread.data = &data;
    wrongThread.start();
    check(wrongThread.wait(5000), "wrong-thread test timed out");
    check(saved == full && reads == 0, "unrelated worker must pass through");
    worker.start();
    check(worker.wait(5000), "event-thread test timed out");
    auto expected = full;
    expected["eventTimestamps"] = full.value("eventTimestamps").toList().mid(20000);
    expected["wordCounts"] = full.value("wordCounts").toList().mid(20000);
    check(saved == expected && reads == 2, "only paired suffix may change on event thread");
    check(starts.map == markers && ends.map == endMarkers, "markers changed");
    data.type = ReaderShown;
    worker.start();
    check(worker.wait(5000), "non-page test timed out");
    check(saved == full && reads == 2, "non-page event must pass through");
    data.type = PageTurn;
    data.map = expected;
    worker.start();
    check(worker.wait(5000), "repeat test timed out");
    check(saved == expected && reads == 2, "retained history should be left intact");
    puts("pewter.event-history passed");
    std::_Exit(0);
}
