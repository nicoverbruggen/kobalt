#include "player.h"
#include "resources.h"
#include "app/reader_window.h"
#include "reading/rendering_mode.h"
#include "sync/kosync.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include "support/qt_input.h"
#include "ui/style/metrics.h"
#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <QWindow>
#include <deque>
#include <memory>

namespace kobalt::scenario {
namespace {
class WaitLoop : public QEventLoop
{
public:
    void wait(int milliseconds)
    {
        QTimer deadline;
        deadline.setTimerType(Qt::PreciseTimer);
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, this, &QEventLoop::quit);
        deadline.start(qMax(0, milliseconds));
        exec();
    }

    bool until(const std::function<bool()> &done, int timeout)
    {
        QTimer poll;
        bool met = false;
        QObject::connect(&poll, &QTimer::timeout, this, [&] {
            if (done()) {
                met = true;
                quit();
            }
        });
        poll.start(16);
        wait(timeout);
        return met || done();
    }
};

// Other host predicates have no change notification. Limit how often the
// player samples them so a slow operation can leave the reader thread asleep.
bool waitUntil(const std::function<bool()> &done, int timeout)
{
    return done() || WaitLoop().until(done, timeout);
}

// Diagnostics events from any thread, in the order they were written.
class Events
{
public:
    Events()
    {
        // The diagnostics module calls a copy of the observer outside its
        // lock, so a worker's event can arrive after the player has removed
        // it. The observer keeps the state alive until such a call ends.
        setDiagnosticObserver([state = state](const QJsonObject &event) {
            QMutexLocker lock(&state->mutex);
            state->queue.push_back(event);
            // Counted by name, and operations also by their own name, so a
            // sample can count events that its steps never wait for.
            const auto name = event.value("event").toString();
            ++state->counted[name];
            if (name == QLatin1String("operation-start"))
                ++state->counted[name + '/' + event.value("operation").toString()];
            if (state->notify)
                state->notify(event);
        });
    }

    ~Events() { setDiagnosticObserver({}); }

    // Passes over the events that arrived so far. A step that acts calls it,
    // so the next event step matches only what that action caused.
    void skip()
    {
        QMutexLocker lock(&state->mutex);
        state->passed.insert(state->passed.end(), state->queue.begin(), state->queue.end());
        state->queue.clear();
    }

    // Forgets the events that earlier steps passed over or left queued.
    // Called as each sample starts, so `earlier` sees this sample's only.
    void begin()
    {
        QMutexLocker lock(&state->mutex);
        state->passed.clear();
        state->queue.clear();
    }

    // How many events of each kind arrived since the player started.
    QMap<QString, int> counts()
    {
        QMutexLocker lock(&state->mutex);
        return state->counted;
    }

    bool waitForClose(const std::function<void()> &request, const std::function<bool()> &done,
                      int timeout)
    {
        WaitLoop loop;
        {
            QMutexLocker lock(&state->mutex);
            // Observe the close before requesting it. Keep the event queued
            // for an explicit event step and check host state on the GUI thread.
            state->notify = [&](const QJsonObject &event) {
                if (event.value("event").toString() == QLatin1String("book-close"))
                    qt::invoke(&loop, [&] {
                        if (done())
                            loop.quit();
                    }, Qt::QueuedConnection);
            };
        }
        request();
        const bool completed = done() || loop.until(done, timeout);
        QMutexLocker lock(&state->mutex);
        state->notify = {};
        return completed;
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
        WaitLoop loop;
        QMutexLocker lock(&state->mutex);
        if (earlier) {
            auto &passed = state->passed;
            for (auto it = passed.begin(); it != passed.end(); ++it) {
                if (matches(*it)) {
                    found = *it;
                    passed.erase(it);
                    return found;
                }
            }
        }
        const auto takeMatch = [&] {
            while (!state->queue.empty()) {
                auto event = state->queue.front();
                state->queue.pop_front();
                if (matches(event))
                    return event;
                state->passed.push_back(event);
            }
            return QJsonObject{};
        };
        found = takeMatch();
        if (!found.isEmpty())
            return found;

        // Register before unlocking so an event cannot arrive between checking
        // the queue and starting the wait. Workers only post to the reader loop.
        bool posted = false;
        state->notify = [&](const QJsonObject &event) {
            if (!posted && matches(event)) {
                posted = true;
                QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
            }
        };
        lock.unlock();
        loop.wait(timeout);
        lock.relock();
        state->notify = {};
        return takeMatch();
    }

private:
    struct State
    {
        QMutex mutex;
        std::function<void(const QJsonObject &)> notify;
        std::deque<QJsonObject> queue;
        // Events that left the queue without a match since the sample started.
        std::deque<QJsonObject> passed;
        QMap<QString, int> counted;
    };

    const std::shared_ptr<State> state = std::make_shared<State>();
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

// The value at a dotted path, such as `display_settings.dark_mode`.
QJsonValue lookup(const QJsonObject &object, const QString &path)
{
    QJsonValue value = object;
    for (const auto &key : path.split('.'))
        value = value.toObject().value(key);
    return value;
}

// A value as a report shows it.
QString text(const QJsonValue &value)
{
    if (value.isString())
        return '"' + value.toString() + '"';
    if (value.isBool())
        return QString::fromLatin1(value.toBool() ? "true" : "false");
    if (value.isDouble())
        return QString::number(value.toDouble());
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    return QString::fromLatin1(value.isNull() ? "null" : "missing");
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
        // Any step runs only `if` a widget shows, or `unless` it shows.
        if (step.contains("unless") && visible(host, step.value("unless").toString()))
            return {};
        if (step.contains("if") && !visible(host, step.value("if").toString()))
            return {};
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
            const auto name = pick(step.value("press"), repetition).toString();
            return press(find(host, name), name, pick(step.value("at"), repetition));
        }
        if (!host.busy && (step.contains("hold") || step.contains("settle"))) {
            unsupported = true;
            return "the host cannot report whether the reader is busy";
        }
        if (step.contains("hold"))
            return hold(step.value("hold").toObject(), step.value("ms").toInt(650), repetition);
        if (step.contains("open"))
            return open(step.value("open").toString(), step.value("fresh").toBool());
        // Removes the disposable caches of a book, so a later open parses it.
        if (step.contains("forget")) {
            if (!host.forget) {
                unsupported = true;
                return "the host cannot remove caches";
            }
            if (!host.reader) {
                unsupported = true;
                return "the host cannot report whether a book is open";
            }
            if (host.reader())
                return "close the book before removing caches";
            if (!host.forget(step.value("forget").toString()))
                return "could not clear the book caches";
            return {};
        }
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
            return close(reader) ? QString() : "the book did not close";
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
            // Counted first, so the time and the CPU time do not include it.
            markCounts = events.counts();
            markResources = sampleResources();
            mark.start();
            markEnd = -1;
            return {};
        }
        // Ends the time since `mark`, so later steps can wait for more events.
        if (step.contains("mark_end")) {
            if (!mark.isValid())
                return "no mark";
            markEnd = mark.nsecsElapsed();
            endResources = sampleResources();
            endCounts = events.counts();
            return {};
        }
        // Lets time pass, for example a sync interval, with events running.
        // One timer, so the device sleeps in between.
        if (step.contains("wait_ms")) {
            const int wait = qMax(0, step.value("wait_ms").toInt());
            if (!host.realMilliseconds) {
                WaitLoop().wait(wait);
                return {};
            }
            // Count real time. A Kobo in standby stops the process's timers,
            // so check again at least every second of awake time.
            const auto end = host.realMilliseconds() + wait;
            for (qint64 left = wait; left > 0; left = end - host.realMilliseconds()) {
                WaitLoop().wait(int(qMin<qint64>(left, 1000)));
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
                if (!close(reader))
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
        // Keeps the reader's state, or one widget property, under a name, so
        // a later `expect` can compare with it.
        if (step.contains("remember")) {
            const auto name = step.value("remember").toString();
            QJsonValue value;
            QJsonObject state;
            const auto failure = step.contains("widget") ? property(step, value) : inspect(state);
            if (!failure.isEmpty())
                return failure;
            remembered[name] = step.contains("widget") ? qt::object({ { "value", value } }) : state;
            if (step.contains("widget"))
                widgetValues.insert(name);
            else
                widgetValues.remove(name);
            return {};
        }
        if (step.contains("expect") || step.contains("widget"))
            return expect(step);
        if (step.contains("reveal"))
            return reveal(step.value("reveal").toString(),
                          step.value("next").toString("rrSettingsOptionsNext"),
                          step.value("previous").toString("rrSettingsOptionsPrevious"));
        // Puts text into a field, as a person types it on the keyboard.
        if (step.contains("type")) {
            const auto name = step.value("into").toString();
            auto *field = find(host, name);
            if (!field || !field->isVisible())
                return name + " is not visible";
            if (field->metaObject()->indexOfProperty("text") < 0)
                return name + " takes no text";
            field->setProperty("text", step.value("type").toString());
            return {};
        }
        if (step.contains("choose"))
            return choose(step);
        return "unknown step";
    }

    // The reader's state as its state report would hold it now.
    QString inspect(QJsonObject &state)
    {
        auto *reader = dynamic_cast<ReaderWindow *>(host.reader ? host.reader() : nullptr);
        if (!reader)
            return "no book is open";
        state = reader->inspect();
        return state.isEmpty() ? QStringLiteral("the reader reported no state") : QString();
    }

    // A widget's property, `checked` unless the step names another.
    QString property(const QJsonObject &step, QJsonValue &value)
    {
        const auto name = step.value("widget").toString();
        const auto *widget = find(host, name);
        if (!widget)
            return name + " does not exist";
        const auto key = step.value("property").toString("checked");
        const auto variant = widget->property(key.toLatin1().constData());
        if (!variant.isValid())
            return name + " has no property " + key;
        value = QJsonValue::fromVariant(variant);
        return {};
    }

    // Compares a value of the reader's state, or a widget property, with a
    // literal (`is`, `is_not`) or with a remembered value (`same_as`, `not_as`,
    // `above`, `below`, optionally `by` an exact amount). It waits until the
    // comparison holds or the time runs out, and records the outcome as a check.
    QString expect(const QJsonObject &step)
    {
        const bool widget = step.contains("widget");
        const auto path = step.value("expect").toString();
        QString relation;
        for (const auto *name : { "is", "is_not", "same_as", "not_as", "above", "below" })
            if (relation.isEmpty() && step.contains(QLatin1String(name)))
                relation = QLatin1String(name);
        if (relation.isEmpty())
            return "the check has no comparison";
        const bool literal = relation.startsWith(QLatin1String("is"));
        const auto reference = step.value(relation).toString();
        if (!literal && !remembered.contains(reference))
            return "nothing was remembered as " + reference;
        QJsonValue actual, wanted = literal ? step.value(relation) : QJsonValue();
        QString failure;
        const auto holds = [&] {
            QJsonObject state;
            failure = widget ? property(step, actual) : inspect(state);
            if (!failure.isEmpty())
                return false;
            if (!widget)
                actual = lookup(state, path);
            if (!literal) {
                const auto &snapshot = remembered[reference];
                wanted = widgetValues.contains(reference)
                        ? snapshot.value("value")
                        : lookup(snapshot, step.value("field").toString(path));
            }
            if (relation == "is" || relation == "same_as")
                return actual == wanted;
            if (relation == "is_not" || relation == "not_as")
                return actual != wanted;
            if (!actual.isDouble() || !wanted.isDouble())
                return false;
            const double change =
                    (actual.toDouble() - wanted.toDouble()) * (relation == "above" ? 1 : -1);
            return step.contains("by") ? qAbs(change - step.value("by").toDouble()) < 1e-9
                                       : change > 0;
        };
        // Reading the state lays nothing out, but polls slower than other waits.
        QElapsedTimer waited;
        waited.start();
        bool passed = holds();
        while (!passed && waited.elapsed() < step.value("timeout").toInt(5000)) {
            WaitLoop().wait(100);
            passed = holds();
        }
        const auto subject = widget
                ? step.value("widget").toString() + '.' + step.value("property").toString("checked")
                : path;
        const QString word = relation == "is" || relation == "same_as" ? QString()
                : relation == "is_not" || relation == "not_as"         ? QStringLiteral("not ")
                                                                       : relation + ' ';
        const auto name = step.value("check").toString(
                subject + " is " + word + (literal ? text(wanted) : "as " + reference));
        checks.append(qt::object({ { "check", name },
                                   { "pass", passed },
                                   { "actual", actual },
                                   { "expected", wanted } }));
        if (passed)
            return {};
        if (!failure.isEmpty())
            return name + ": " + failure;
        return name + ": " + subject + " is " + text(actual) + ", expected " + word + text(wanted)
                + (step.contains("by") ? " by " + text(step.value("by")) : QString());
    }

    // Pages a list until `name` can be pressed. Settings options sit on
    // pages of their own.
    QString reveal(const QString &name, const QString &next, const QString &previous)
    {
        const auto shown = [&] {
            auto *target = find(host, name);
            QPoint point;
            return target && target->isVisible() && pointIn(target, {}, point);
        };
        QString failure;
        if (page(next, previous, [&] { return waitUntil(shown, 500); }, failure))
            return {};
        return failure.isEmpty() ? name + " is not on any page" : failure;
    }

    // Goes back to the first page of a list with `previous`, then forward
    // with `next` until `found` holds. Returns whether it held; `failure` is
    // set when a press failed.
    bool page(const QString &next, const QString &previous, const std::function<bool()> &found,
              QString &failure)
    {
        const auto turn = [&](const QString &name) {
            auto *button = name.isEmpty() ? nullptr : find(host, name);
            if (!button || !button->isVisible() || !button->isEnabled())
                return false;
            failure = press(button, name, {});
            WaitLoop().wait(50);
            return failure.isEmpty();
        };
        int back = 0;
        while (back < 20 && turn(previous))
            ++back;
        if (!failure.isEmpty())
            return false;
        if (back == 20) {
            failure = previous + " did not reach the first page";
            return false;
        }
        for (int forward = 0; forward < 20; ++forward) {
            if (found())
                return true;
            if (!turn(next))
                return false;
        }
        return false;
    }

    // Presses the option labelled `choose` in the list `in`, paging with
    // `next` and `previous`. With `keep`, it first keeps the label of the
    // checked option, so a later `choose` of "@<keep>" returns to it.
    QString choose(const QJsonObject &step)
    {
        const auto list = step.value("in").toString(), next = step.value("next").toString(),
                   previous = step.value("previous").toString();
        auto label = step.value("choose").toString();
        if (label.startsWith('@')) {
            label = labels.value(label.mid(1));
            if (label.isEmpty())
                return "no label was kept as " + step.value("choose").toString().mid(1);
        }
        const auto options = [&] {
            QList<QAbstractButton *> shown;
            if (auto *parent = find(host, list))
                for (auto *option : parent->findChildren<QAbstractButton *>())
                    if (option->isVisible())
                        shown.append(option);
            return shown;
        };
        if (!waitUntil([&] { return !options().isEmpty(); }, 5000))
            return list + " shows no options";
        QString failure;
        if (step.contains("keep")) {
            QString checked;
            page(
                    next, previous,
                    [&] {
                        for (auto *option : options())
                            if (option->isChecked())
                                checked = option->text();
                        return !checked.isEmpty();
                    },
                    failure);
            if (checked.isEmpty())
                return failure.isEmpty() ? list + " has no checked option to keep" : failure;
            labels[step.value("keep").toString()] = checked;
        }
        QAbstractButton *target = nullptr;
        if (!page(
                    next, previous,
                    [&] {
                        for (auto *option : options())
                            if (option->text() == label)
                                target = option;
                        return target != nullptr;
                    },
                    failure))
            return failure.isEmpty() ? list + " has no option " + label : failure;
        return press(target, label, {});
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

    // An inherited book has no known rendering mode until the player opens it.
    QString book(const QString &key, const QString &rendering)
    {
        requestedRendering = rendering;
        if (key.isEmpty()
            || (key == current && host.reader && host.reader()
                && (rendering.isEmpty() || rendering == openedRendering)))
            return {};
        return open(key, false);
    }

    // Closes an open book first, as a person returns to the library.
    QString open(const QString &key, bool fresh)
    {
        if (!host.open || !host.reader || !host.busy || (host.opens && !host.opens(key))) {
            unsupported = true;
            return "the host cannot open " + key;
        }
        if (fresh && !host.forget) {
            unsupported = true;
            return "the host cannot remove caches";
        }
        if (auto *reader = host.reader(); reader && !close(reader))
            return "the open book did not close";
        if (fresh && !host.forget(key))
            return "could not clear the book caches";
        events.skip();
        if (!host.open(key)) {
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
        openedRendering = requestedRendering;
        return {};
    }

    bool close(QWidget *reader)
    {
        return events.waitForClose(
                [this, reader] {
                    reader->close();
                    current.clear();
                },
                [this] { return !host.reader() && (!host.idle || host.idle()); }, 60000);
    }

    // Presses `target` at `at`, a fraction of its size, or at its centre.
    QString press(QWidget *target, const QString &name, const QJsonValue &at)
    {
        if (!target || !target->isVisible() || !target->isEnabled())
            return name + " is not visible";
        QPoint point;
        if (!pointIn(target, at, point))
            return name + " is covered";
        // Tap no faster than a person after a close, so the close guard
        // does not swallow the press. The wait comes before the sample.
        auto *reader = dynamic_cast<ReaderWindow *>(host.reader ? host.reader() : nullptr);
        if (reader
            && !waitUntil([reader] { return !reader->closeGuardActive(); },
                          2 * ui::metrics::contents::closeGuardMilliseconds))
            return name + " stayed behind the close guard";
        events.skip();
        return click(target, point, 0) ? QString() : name + " has no window";
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
                WaitLoop().wait(milliseconds);
            host.press(widget, point, false);
            return true;
        }
        const auto input = host.locate(widget, point);
        if (!input.first)
            return false;
        qt::sendMouse(input.first, input.second, true);
        if (milliseconds > 0)
            WaitLoop().wait(milliseconds);
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
            const bool ended = markEnd >= 0;
            value = (ended ? markEnd : mark.nsecsElapsed()) / 1e6;
            // What the system counted from `mark` to `mark_end`, or to now,
            // and the events named in `count`.
            if (spec.value("resources").toBool()) {
                const auto end = ended ? endResources : sampleResources();
                const auto counts = ended ? endCounts : events.counts();
                const auto measured = resourceStages(markResources, end);
                for (auto it = measured.begin(); it != measured.end(); ++it)
                    stages[it.key()] = it.value();
                const auto threads = threadStages(markResources, end);
                for (auto it = threads.begin(); it != threads.end(); ++it)
                    stages["thread_cpu_ms." + it.key()] = it.value();
                const auto counted = spec.value("count").toObject();
                for (auto it = counted.begin(); it != counted.end(); ++it) {
                    const auto key = it.value().toString();
                    stages[it.key()] = counts.value(key) - markCounts.value(key);
                }
            }
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
    // The outcome of each `expect` of the current scenario.
    QJsonArray checks;

    // Starts a scenario: forgets the values and labels of the one before.
    void start()
    {
        checks = {};
        remembered.clear();
        widgetValues.clear();
        labels.clear();
    }

private:
    QMap<QString, QJsonObject> remembered;
    // Names of values in `remembered` that are one widget property.
    QSet<QString> widgetValues;
    // Option labels kept by `choose`.
    QMap<QString, QString> labels;
    const Host &host;
    Events &events;
    QMap<QString, QJsonObject> matched;
    QElapsedTimer mark;
    // Nanoseconds from `mark` to `mark_end`, or -1.
    qint64 markEnd = -1;
    // The system's counters and the event counts at `mark` and `mark_end`.
    Resources markResources, endResources;
    QMap<QString, int> markCounts, endCounts;
    // The key of the open book, if the player knows it.
    QString current;
    QString requestedRendering, openedRendering;
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
        // A resource scenario tells the comparison which stages are system
        // counters and event counts, and which counts must stay at a limit.
        const auto sampleSpec = scenario.value("sample").toObject();
        const bool resources = sampleSpec.value("resources").toBool();

        struct RestoreLogging
        {
            bool active;
            bool previous;

            ~RestoreLogging()
            {
                if (active)
                    setDiagnosticFileWrites(previous);
            }
        } logging{ resources, resources && setDiagnosticFileWrites(false) };

        if (resources) {
            if (!waitUntil(diagnosticWriterIdle, 30000)) {
                result["status"] = QStringLiteral("failed");
                result["reason"] =
                        QStringLiteral("Diagnostic writer did not drain before sampling");
                results.append(result);
                continue;
            }
            result["diagnostic_file_writes"] = false;
            result["resources"] = true;
            result["counts"] =
                    QJsonArray::fromStringList(sampleSpec.value("count").toObject().keys());
            if (scenario.contains("expect"))
                result["expect"] = scenario.value("expect").toObject();
        }
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

        // A scenario that names a rendering mode opens its books in it,
        // whatever the preference or the environment says. The override ends
        // with the scenario, also after a failure.
        struct Rendering
        {
            bool set;

            ~Rendering()
            {
                if (set)
                    renderingMode::setOverride(-1);
            }
        } rendering{ scenario.contains("rendering") };

        if (rendering.set) {
            const auto mode = scenario.value("rendering").toString();
            renderingMode::setOverride(mode == "hybrid" ? 2 : mode == "fast" ? 1 : 0);
        }
        if (needs(scenario, "light") && !(host.light && host.light())) {
            result["status"] = QStringLiteral("skipped");
            result["reason"] = QStringLiteral("the device has no front light");
            result["samples"] = samples;
            result["stages"] = stages;
            results.append(result);
            continue;
        }
        if (needs(scenario, "rotation") && !(host.rotation && host.rotation())) {
            result["status"] = QStringLiteral("skipped");
            result["reason"] = QStringLiteral("the host cannot turn the screen");
            result["samples"] = samples;
            result["stages"] = stages;
            results.append(result);
            continue;
        }

        // A check passes or fails on its `expect` steps and takes no sample.
        // A wait of the main thread during a check fails it too.
        const bool check = scenario.value("kind").toString() == QLatin1String("check");
        const int blocked = events.counts().value("main-thread-blocked");
        runner.unsupported = false;
        runner.start();
        QString failure = runner.book(scenario.value("book").toString(),
                                      scenario.value("rendering").toString());
        if (failure.isEmpty()) {
            runner.reset();
            failure = runner.steps(scenario.value("setup").toArray(), 0);
        }
        const int warmup = scenario.value("warmup").toInt();
        const int repeat = qMax(
                1, qRound(scenario.value("repeat").toInt(check ? 1 : 5) * options.repeatScale));
        for (int index = 0; failure.isEmpty() && index < warmup + repeat; ++index) {
            runner.reset();
            failure = runner.steps(scenario.value("steps").toArray(), index);
            double sample = 0;
            QJsonObject parts;
            if (check)
                continue;
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
        // Leave the reader as the next scenario expects it. A check undoes
        // its changes also after a failure, so the next check starts clean.
        if (failure.isEmpty())
            failure = runner.steps(scenario.value("teardown").toArray(), 0);
        else if (check && !runner.unsupported) {
            const auto cleanup = runner.steps(scenario.value("teardown").toArray(), 0);
            if (!cleanup.isEmpty())
                result["cleanup"] = cleanup;
        }
        if (check && failure.isEmpty() && events.counts().value("main-thread-blocked") > blocked)
            failure = QStringLiteral("the main thread was blocked during the check");
        result["status"] = QString(failure.isEmpty()            ? "completed"
                                           : runner.unsupported ? "skipped"
                                                                : "failed");
        if (!failure.isEmpty())
            result["reason"] = failure;
        result["samples"] = samples;
        result["stages"] = stages;
        if (check)
            result["checks"] = runner.checks;
        if (!details.isEmpty())
            result["details"] = details;
        results.append(result);
    }
    return results;
}
} // namespace kobalt::scenario
