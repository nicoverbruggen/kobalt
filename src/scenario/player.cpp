#include "player.h"
#include "app/reader_window.h"
#include "sync/kosync.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include "support/qt_input.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <QWindow>
#include <deque>
#include <memory>

namespace kobalt::scenario {
namespace {
// Polls every millisecond without sleeping, so a wait never adds to a measured
// time. done() is called until it first holds and never again, because a check
// that reads events consumes what it reads.
bool waitUntil(const std::function<bool()> &done, int timeout)
{
    if (done())
        return true;
    bool met = false;
    QEventLoop loop;
    QTimer poll;
    QElapsedTimer clock;
    clock.start();
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        met = done();
        if (met || clock.elapsed() > timeout)
            loop.quit();
    });
    poll.start(1);
    loop.exec();
    return met;
}

// Diagnostics events from any thread, in the order they were written.
class Events
{
public:
    Events()
    {
        setDiagnosticObserver([this](const QJsonObject &event) {
            QMutexLocker lock(&mutex);
            queue.push_back(event);
        });
    }

    ~Events() { setDiagnosticObserver({}); }

    // Passes over the events that arrived so far. A step that acts calls it,
    // so the next event step matches only what that action caused.
    void skip()
    {
        QMutexLocker lock(&mutex);
        passed.insert(passed.end(), queue.begin(), queue.end());
        queue.clear();
    }

    // Forgets the events that earlier steps passed over or left queued.
    // Called as each sample starts, so `earlier` sees this sample's only.
    void begin()
    {
        QMutexLocker lock(&mutex);
        passed.clear();
        queue.clear();
    }

    // Consumes events until one matches, and returns it. `where` lists
    // further fields the event must have, such as an operation name. With
    // `earlier`, the events that this sample's steps skipped or passed over
    // are searched first, for work that can end before or after the step.
    QJsonObject next(const QString &name, const QString &panel, const QJsonObject &where,
                     bool earlier, int timeout)
    {
        const auto matches = [&](const QJsonObject &event) {
            bool match = event.value("event").toString() == name
                    && (panel.isEmpty() || event.value("panel").toString() == panel);
            for (auto it = where.begin(); match && it != where.end(); ++it)
                match = event.value(it.key()) == it.value();
            return match;
        };
        QJsonObject found;
        if (earlier) {
            QMutexLocker lock(&mutex);
            for (auto it = passed.begin(); it != passed.end(); ++it) {
                if (matches(*it)) {
                    found = *it;
                    passed.erase(it);
                    return found;
                }
            }
        }
        waitUntil(
                [&] {
                    QMutexLocker lock(&mutex);
                    while (!queue.empty()) {
                        const auto event = queue.front();
                        queue.pop_front();
                        if (matches(event)) {
                            found = event;
                            return true;
                        }
                        passed.push_back(event);
                    }
                    return false;
                },
                timeout);
        return found;
    }

private:
    QMutex mutex;
    std::deque<QJsonObject> queue;
    // Events that left the queue without a match since the sample started.
    std::deque<QJsonObject> passed;
};

// A closed sheet keeps its names until its deferred deletion. Qt 5 runs that
// only after the player's nested event loops end, so the next sheet of the
// same kind exists beside it. Prefer a visible widget over a hidden one.
QWidget *find(const Host &host, const QString &name)
{
    QWidget *hidden = nullptr;
    const auto search = [&](QWidget *root) -> QWidget * {
        auto matches = root->findChildren<QWidget *>(name);
        if (root->objectName() == name)
            matches.prepend(root);
        for (auto *widget : matches) {
            if (widget->isVisible())
                return widget;
            if (!hidden)
                hidden = widget;
        }
        return nullptr;
    };
    if (auto *reader = host.reader ? host.reader() : nullptr) {
        if (auto *widget = search(reader))
            return widget;
    }
    // Popups are windows of their own.
    for (auto *window : QApplication::topLevelWidgets()) {
        if (auto *widget = search(window))
            return widget;
    }
    return hidden;
}

bool visible(const Host &host, const QString &name)
{
    const auto *widget = find(host, name);
    return widget && widget->isVisible();
}

// A scenario value that is a list of names or of points takes the entry for
// this repetition, so samples can alternate between targets.
QJsonValue pick(const QJsonValue &value, int repetition)
{
    const auto list = value.toArray();
    if (list.isEmpty() || !(list.first().isString() || list.first().isArray()))
        return value;
    return list.at(repetition % list.size());
}

// The point to press: the given fraction of the widget, or its centre. When a
// panel covers the centre, try higher up, as a person would.
bool pointIn(QWidget *target, const QJsonValue &at, QPoint &point)
{
    auto *window = target->window();
    const auto hits = [&](const QPoint &candidate) {
        auto *hit = window->childAt(target->mapTo(window, candidate));
        return hit == target || target->isAncestorOf(hit) || (window == target && !hit);
    };
    const auto fraction = at.toArray();
    if (fraction.size() == 2) {
        point = QPoint(qRound(fraction.at(0).toDouble() * (target->width() - 1)),
                       qRound(fraction.at(1).toDouble() * (target->height() - 1)));
        return hits(point);
    }
    const auto centre = target->rect().center();
    for (const int y : { centre.y(), target->height() / 3, target->height() / 4 }) {
        point = QPoint(centre.x(), y);
        if (hits(point))
            return true;
    }
    return false;
}

class Runner
{
public:
    Runner(const Host &host, Events &events, QString open)
        : host(host), events(events), current(std::move(open))
    {
    }

    // Runs one step for the given repetition. Returns an empty string or the reason it failed.
    QString step(const QJsonObject &step, int repetition)
    {
        if (step.contains("sleep_persistence")) {
            if (!host.sleepPersistence) {
                unsupported = true;
                return "the host has no sleep persistence benchmark";
            }
            const auto result = host.sleepPersistence(step.value("sleep_persistence").toObject());
            matched["sleep-persistence"] = result;
            return result.value("error").toString();
        }
        if (step.contains("press")) {
            if (step.contains("unless") && visible(host, step.value("unless").toString()))
                return {};
            const auto name = pick(step.value("press"), repetition).toString();
            auto *target = find(host, name);
            if (!target || !target->isVisible() || !target->isEnabled())
                return name + " is not visible";
            QPoint point;
            if (!pointIn(target, pick(step.value("at"), repetition), point))
                return name + " is covered";
            events.skip();
            return click(target, point, 0) ? QString() : name + " has no window";
        }
        if (step.contains("hold"))
            return hold(step.value("hold").toObject(), step.value("ms").toInt(650), repetition);
        if (step.contains("open"))
            return open(step.value("open").toString(), step.value("fresh").toBool());
        if (step.contains("close")) {
            // A book that cannot be opened again stays open for later scenarios.
            if (!host.open) {
                unsupported = true;
                return "the host cannot open books again";
            }
            auto *reader = host.reader ? host.reader() : nullptr;
            if (!reader)
                return "no book is open";
            events.skip();
            reader->close();
            current.clear();
            return closed() ? QString() : "the book did not close";
        }
        // Page keys go where every platform's page buttons go.
        if (step.contains("key")) {
            const auto name = step.value("key").toString();
            auto *reader = dynamic_cast<ReaderWindow *>(host.reader ? host.reader() : nullptr);
            if (!reader || (name != "PageDown" && name != "PageUp"))
                return "cannot send key " + name;
            events.skip();
            reader->scriptedPageButton(name == "PageDown" ? 1 : -1);
            return {};
        }
        const int timeout = step.value("timeout").toInt(5000);
        if (step.contains("visible")) {
            const auto name = pick(step.value("visible"), repetition).toString();
            return waitUntil([&] { return visible(host, name); }, timeout)
                    ? QString()
                    : name + " did not appear";
        }
        if (step.contains("hidden")) {
            const auto name = step.value("hidden").toString();
            return waitUntil([&] { return !visible(host, name); }, timeout)
                    ? QString()
                    : name + " did not close";
        }
        if (step.contains("event")) {
            const auto name = step.value("event").toString(),
                       panel = step.value("panel").toString();
            const auto event =
                    events.next(name, panel, step.value("where").toObject(),
                                step.value("earlier").toBool(), step.value("timeout").toInt(10000));
            if (event.isEmpty())
                return "no " + name + " event";
            // `as` names the match, so a sample can keep several events of one kind.
            if (step.contains("as")) {
                matched[step.value("as").toString()] = event;
                return {};
            }
            matched[name] = event;
            if (!panel.isEmpty())
                matched[name + '/' + panel] = event;
            return {};
        }
        if (step.contains("settle"))
            return waitUntil([&] { return !host.busy(); }, step.value("timeout").toInt(10000))
                    ? QString()
                    : "the reader stayed busy";
        if (step.contains("mark")) {
            mark.start();
            markEnd = -1;
            return {};
        }
        // Ends the time since `mark`, so later steps can wait for more events.
        if (step.contains("mark_end")) {
            if (!mark.isValid())
                return "no mark";
            markEnd = mark.nsecsElapsed();
            return {};
        }
        // Lets time pass, for example a sync interval, with events running.
        // One timer, so the device sleeps in between.
        if (step.contains("wait_ms")) {
            const int wait = qMax(0, step.value("wait_ms").toInt());
            if (!host.realMilliseconds) {
                QEventLoop loop;
                QTimer::singleShot(wait, &loop, &QEventLoop::quit);
                loop.exec();
                return {};
            }
            // Count real time. A Kobo in standby stops the process's timers,
            // so check again at least every second of awake time.
            const auto end = host.realMilliseconds() + wait;
            for (qint64 left = wait; left > 0; left = end - host.realMilliseconds()) {
                QEventLoop loop;
                QTimer::singleShot(int(qMin<qint64>(left, 1000)), &loop, &QEventLoop::quit);
                loop.exec();
            }
            return {};
        }
        // A KOSync account that exists for this scenario only, in memory.
        // Automatic sync runs while it is set; an empty object ends it. The
        // open book closes first, so its reader can never sync with the saved
        // account; the next opening uses the scenario account.
        if (step.contains("kosync")) {
            if (auto *reader = host.reader ? host.reader() : nullptr) {
                events.skip();
                reader->close();
                current.clear();
                if (!closed())
                    return "the open book did not close";
            }
            const auto account = step.value("kosync").toObject();
            kosync::overrideAccount(account);
            kosync::pauseAutomatic(account.isEmpty());
            return {};
        }
        // Starts the host's own store sync and does not wait for it. A sync
        // that is still running from an earlier sample ends first, so each
        // sample starts its own.
        if (step.contains("native-sync")) {
            if (!host.nativeSync) {
                unsupported = true;
                return "the host cannot start its own sync";
            }
            if (host.nativeSyncing
                && !waitUntil([&] { return !host.nativeSyncing(); },
                              step.value("timeout").toInt(300000)))
                return "the earlier sync did not end";
            return host.nativeSync();
        }
        // Turns Wi-Fi off and waits until the radio is down, so a scenario
        // starts from a known state whatever an earlier sample or the user left.
        if (step.contains("wifi")) {
            if (step.value("wifi").toString() != "off")
                return "a wifi step can only turn Wi-Fi off";
            if (!host.wifiOff) {
                unsupported = true;
                return "the host cannot turn Wi-Fi off";
            }
            if (host.wifiUp && !host.wifiUp())
                return {};
            if (const auto error = host.wifiOff(); !error.isEmpty())
                return error;
            if (host.wifiUp
                && !waitUntil([&] { return !host.wifiUp(); }, step.value("timeout").toInt(15000)))
                return "Wi-Fi stayed on";
            return {};
        }
        return "unknown step";
    }

    QString steps(const QJsonArray &list, int repetition)
    {
        for (int index = 0; index < list.size(); ++index) {
            const auto failure = step(list.at(index).toObject(), repetition);
            if (!failure.isEmpty())
                return QString("step %1: %2").arg(index + 1).arg(failure);
        }
        return {};
    }

    // Opens the scenario's book unless it is the open one.
    QString book(const QString &key)
    {
        if (key.isEmpty() || (key == current && host.reader && host.reader()))
            return {};
        return open(key, false);
    }

    // Closes an open book first, as a person returns to the library.
    QString open(const QString &key, bool fresh)
    {
        if (!host.open || (host.opens && !host.opens(key))) {
            unsupported = true;
            return "the host cannot open " + key;
        }
        if (auto *reader = host.reader ? host.reader() : nullptr) {
            reader->close();
            current.clear();
            if (!closed())
                return "the open book did not close";
        }
        if (fresh && host.forget)
            host.forget(key);
        events.skip();
        if (!host.open || !host.open(key)) {
            unsupported = true;
            return "the host cannot open " + key;
        }
        const bool ready = waitUntil(
                [&] {
                    auto *reader = host.reader();
                    return reader && reader->isVisible() && !host.busy();
                },
                120000);
        if (!ready)
            return key + " did not open";
        current = key;
        return {};
    }

    bool closed()
    {
        return waitUntil([&] { return !host.reader() && (!host.idle || host.idle()); }, 60000);
    }

    // Holds a word or a link, as a lookup or a link selection starts.
    QString hold(const QJsonObject &target, int milliseconds, int repetition)
    {
        auto *reader = dynamic_cast<ReaderWindow *>(host.reader ? host.reader() : nullptr);
        auto *page = find(host, "rrPage");
        if (!reader || !page)
            return "no reader page";
        // The reply may come after a timeout, so it must not refer to this frame.
        auto located = std::make_shared<QPoint>(-2, -2);
        const auto receive = [located](QPoint point) { *located = point; };
        const auto text = pick(target.value("text"), repetition).toString();
        if (!text.isEmpty())
            reader->locateText(text, receive);
        else
            reader->locateLink(target.value("link").toString(), receive);
        const auto name = text.isEmpty() ? target.value("link").toString() : text;
        if (!waitUntil([&] { return located->x() != -2; }, 10000) || located->x() < 0)
            return "could not find " + name;
        // A turn to the word's page finishes before the hold.
        if (!waitUntil([&] { return !host.busy(); }, 10000))
            return "the reader stayed busy";
        events.skip();
        return click(page, *located, milliseconds) ? QString() : "the page has no window";
    }

    // Presses, holds for `milliseconds` and releases.
    bool click(QWidget *widget, const QPoint &point, int milliseconds)
    {
        if (host.press) {
            host.press(widget, point, true);
            if (milliseconds > 0)
                waitUntil([] { return false; }, milliseconds);
            host.press(widget, point, false);
            return true;
        }
        const auto input = host.locate(widget, point);
        if (!input.first)
            return false;
        qt::sendMouse(input.first, input.second, true);
        if (milliseconds > 0)
            waitUntil([] { return false; }, milliseconds);
        qt::sendMouse(input.first, input.second, false);
        return true;
    }

    // The sample for one repetition, and its stages.
    bool sample(const QJsonObject &spec, double &value, QJsonObject &stages)
    {
        // Request memory explicitly; normal events do not sample the process.
        if (spec.contains("memory")) {
            auto sample = std::make_shared<QJsonObject>();
            sampleDiagnosticMemory([sample](QJsonObject fields) {
                qt::invoke(qApp, [sample, fields] { *sample = fields; }, Qt::QueuedConnection);
            });
            if (!waitUntil([sample] { return !sample->isEmpty(); }, 5000))
                return false;
            const auto field = spec.value("memory").toString();
            value = sample->value(field).toDouble();
            return sample->value(field).isDouble();
        }
        if (spec.contains("timer")) {
            if (!mark.isValid())
                return false;
            value = (markEnd >= 0 ? markEnd : mark.nsecsElapsed()) / 1e6;
            const auto parts = spec.value("stages").toObject();
            for (auto it = parts.begin(); it != parts.end(); ++it) {
                const auto source = it.value().toObject();
                stages[it.key()] = matched.value(source.value("event").toString())
                                           .value(source.value("field").toString());
            }
            return true;
        }
        auto key = spec.value("event").toString();
        if (spec.contains("panel"))
            key += '/' + spec.value("panel").toString();
        const auto event = matched.value(key);
        const auto field = spec.value("field").toString();
        if (!event.value(field).isDouble())
            return false;
        value = event.value(field).toDouble();
        if (spec.value("stages").toBool())
            for (auto it = event.begin(); it != event.end(); ++it)
                if (it.key().endsWith("_ms") && it.key() != field && it.key() != "total_ms"
                    && it.key() != "monotonic_ms")
                    stages[it.key()] = it.value();
        return true;
    }

    void reset()
    {
        matched.clear();
        mark.invalidate();
        markEnd = -1;
        events.begin();
    }

    QJsonObject details() const { return matched.value("sleep-persistence"); }

    // Set when a step needed a book the host cannot open.
    bool unsupported = false;

private:
    const Host &host;
    Events &events;
    QMap<QString, QJsonObject> matched;
    QElapsedTimer mark;
    // Nanoseconds from `mark` to `mark_end`, or -1.
    qint64 markEnd = -1;
    // The key of the open book, if the player knows it.
    QString current;
};

// Qt 5.2's QJsonArray cannot search itself.
bool needs(const QJsonObject &scenario, const QString &what)
{
    for (const auto &value : scenario.value("needs").toArray())
        if (value.toString() == what)
            return true;
    return false;
}

bool wanted(const QString &id, const QString &filter)
{
    return filter.isEmpty() || id.startsWith(filter) || filter.startsWith(id);
}
} // namespace

QVector<QJsonObject> run(const QJsonObject &file, const Host &host, const Options &options)
{
    QVector<QJsonObject> results;
    // Keep normal warning/error and lifecycle syncs in the measurements.
    // Older baselines that disabled them are not directly comparable.
    // Nor may a run reach a sync server: it would measure the network and
    // upload the test books' positions. A Kobo set up for scenarios pauses
    // syncs for its whole session, so keep a pause that was already there.
    const bool syncPaused = kosync::automaticPaused();
    kosync::pauseAutomatic(true);
    // Keep the device awake for the run, so standby does not stretch waits
    // and samples. A scenario that sets "standby" lets it suspend.
    if (host.keepAwake)
        host.keepAwake(true);

    struct Restore
    {
        bool syncPaused;
        const Host &host;

        ~Restore()
        {
            kosync::overrideAccount({});
            kosync::pauseAutomatic(syncPaused);
            if (host.keepAwake)
                host.keepAwake(false);
        }
    } restore{ syncPaused, host };

    Events events;
    Runner runner(host, events, options.openBook);
    for (const auto &value : file.value("scenarios").toArray()) {
        const auto scenario = value.toObject();
        const auto id = scenario.value("id").toString();
        if (!wanted(id, options.filter))
            continue;
        if (scenario.value("opt_in").toBool() && options.filter.isEmpty())
            continue;
        if (options.started)
            options.started(id);

        // A scenario that measures with standby lets the device suspend until
        // it ends, however it ends.
        struct Standby
        {
            const Host &host;
            bool allowed;

            ~Standby()
            {
                if (allowed && host.keepAwake)
                    host.keepAwake(true);
            }
        } standby{ host, scenario.value("standby").toBool() };

        if (standby.allowed && host.keepAwake)
            host.keepAwake(false);
        auto result = qt::object({ { "id", id },
                                   { "kind", scenario.value("kind") },
                                   { "paths", scenario.value("paths") },
                                   { "unit", scenario.value("unit").toString("ms") },
                                   { "fixture", scenario.value("fixture").toObject() } });
        QJsonArray samples;
        QJsonArray details;
        QJsonObject stages;
        if (needs(scenario, "sleep-persistence") && !host.sleepPersistence) {
            result["status"] = QStringLiteral("skipped");
            result["reason"] = QStringLiteral("the host has no sleep persistence benchmark");
            result["samples"] = samples;
            result["stages"] = stages;
            results.append(result);
            continue;
        }

        // Cleanup runs after failures too. Sleep overrides exist only in memory.
        struct SleepCleanup
        {
            const Host &host;
            bool enabled;

            ~SleepCleanup()
            {
                if (enabled)
                    host.sleepPersistence({});
            }
        } sleepCleanup{ host, needs(scenario, "sleep-persistence") && bool(host.sleepPersistence) };

        if (needs(scenario, "native-sync") && !host.nativeSync) {
            result["status"] = QStringLiteral("skipped");
            result["reason"] = QStringLiteral("the host cannot start its own sync");
            result["samples"] = samples;
            result["stages"] = stages;
            results.append(result);
            continue;
        }
        if (needs(scenario, "wifi-control") && !(host.wifiControl && host.wifiControl())) {
            result["status"] = QStringLiteral("skipped");
            result["reason"] = QStringLiteral("the host cannot turn Wi-Fi on and off");
            result["samples"] = samples;
            result["stages"] = stages;
            results.append(result);
            continue;
        }

        // A scenario's KOSync account ends with it, also after a failure.
        struct AccountCleanup
        {
            ~AccountCleanup()
            {
                kosync::overrideAccount({});
                kosync::pauseAutomatic(true);
            }
        } accountCleanup;

        if (needs(scenario, "dictionary") && host.dictionary && !host.dictionary()) {
            result["status"] = QStringLiteral("skipped");
            result["reason"] = QStringLiteral("no dictionary is installed");
            result["samples"] = samples;
            result["stages"] = stages;
            results.append(result);
            continue;
        }
        runner.unsupported = false;
        QString failure = runner.book(scenario.value("book").toString());
        if (failure.isEmpty()) {
            runner.reset();
            failure = runner.steps(scenario.value("setup").toArray(), 0);
        }
        const int warmup = scenario.value("warmup").toInt();
        const int repeat = qMax(1, qRound(scenario.value("repeat").toInt(5) * options.repeatScale));
        for (int index = 0; failure.isEmpty() && index < warmup + repeat; ++index) {
            runner.reset();
            failure = runner.steps(scenario.value("steps").toArray(), index);
            double sample = 0;
            QJsonObject parts;
            if (failure.isEmpty()
                && !runner.sample(scenario.value("sample").toObject(), sample, parts))
                failure = "no sample";
            if (failure.isEmpty())
                failure = runner.steps(scenario.value("after").toArray(), index);
            if (needs(scenario, "sleep-persistence")) {
                auto detail = runner.details();
                detail["trial"] = index;
                detail["warmup"] = index < warmup;
                detail["status"] = QString(failure.isEmpty() ? "completed" : "failed");
                if (failure.isEmpty()) {
                    detail["sample"] = sample;
                    detail["sample_stages"] = parts;
                } else
                    detail["error"] = failure;
                details.append(detail);
            }
            if (!failure.isEmpty() || index < warmup)
                continue;
            samples.append(sample);
            for (auto it = parts.begin(); it != parts.end(); ++it) {
                auto list = stages.value(it.key()).toArray();
                list.append(it.value());
                stages[it.key()] = list;
            }
        }
        // Leave the reader as the next scenario expects it.
        if (failure.isEmpty())
            failure = runner.steps(scenario.value("teardown").toArray(), 0);
        result["status"] = QString(failure.isEmpty()            ? "completed"
                                           : runner.unsupported ? "skipped"
                                                                : "failed");
        if (!failure.isEmpty())
            result["reason"] = failure;
        result["samples"] = samples;
        result["stages"] = stages;
        if (!details.isEmpty())
            result["details"] = details;
        results.append(result);
    }
    return results;
}
} // namespace kobalt::scenario
