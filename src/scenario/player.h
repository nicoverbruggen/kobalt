#pragma once
#include <QJsonObject>
#include <QPair>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QVector>
#include <functional>

class QWidget;
class QWindow;

namespace kobalt::scenario {
// What a platform supplies so the player can drive its reader. The player
// itself only presses widgets by name, sends keys and waits for widgets and
// diagnostics events, so the same scenario file runs on every platform.
struct Host
{
    // The open reader window, or null.
    std::function<QWidget *()> reader;
    // Starts opening the book with this key from the scenario file's `books`.
    // Returns false when the host cannot open books by itself; scenarios that
    // need it are then skipped.
    std::function<bool(const QString &key)> open;
    // Whether the host can open the book with this key at all, asked before
    // the open book closes. Optional; without it every key is assumed.
    std::function<bool(const QString &key)> opens;
    // Whether the reader's engine has work in flight.
    std::function<bool()> busy;
    // Whether no book is open and nothing is still closing. Optional.
    std::function<bool()> idle;
    // Removes the disposable caches of the book with this key, so its next
    // opening starts cold. Called only while no book is open. Optional.
    std::function<void(const QString &key)> forget;
    // Whether a dictionary is installed. Optional; without it the player
    // assumes one is.
    std::function<bool()> dictionary;
    // The window that receives input for `point` in `widget`, and the point in
    // that window. Input goes through the window system, as real input does.
    std::function<QPair<QWindow *, QPointF>(QWidget *widget, const QPoint &point)> locate;
    // Presses or releases at `point` in `widget` instead, for a platform whose
    // input filters must not see synthesized window-system input. Optional.
    std::function<void(QWidget *widget, const QPoint &point, bool press)> press;
    // Evaluation-only sleep persistence benchmark. Returns measurements, or an
    // `error`. An empty request restores overrides and drains pending writes.
    std::function<QJsonObject(const QJsonObject &)> sleepPersistence;
    // Whether KOSync can turn Wi-Fi on and off on this host. Optional;
    // scenarios that need it are skipped without it.
    std::function<bool()> wifiControl;
    // Starts the host's own sync with its store, as the host's sync button
    // does, and returns without waiting for the sync to end. Returns an empty
    // string or why the sync did not start. Optional; scenarios that need it
    // are skipped without it.
    std::function<QString()> nativeSync;
    // Whether that sync is still running. Optional.
    std::function<bool()> nativeSyncing;
    // Turns Wi-Fi off and returns an empty string, or why it could not. It
    // may return before the radio is down. Optional; a `wifi` step without it
    // skips the scenario.
    std::function<QString()> wifiOff;
    // Whether the radio is up. Optional; without it a `wifi` step does not
    // wait for the radio to go down.
    std::function<bool()> wifiUp;
    // Real time in milliseconds, including time the device spent suspended.
    // Optional; without it `wait_ms` counts the process's elapsed time, which
    // stops while a Kobo is in standby.
    std::function<qint64()> realMilliseconds;
    // Keeps the device awake (true) or lets it suspend again (false). The
    // player keeps it awake for the whole run, except for a scenario that sets
    // `"standby": true`. Optional.
    std::function<void(bool)> keepAwake;
};

struct Options
{
    // Runs only scenarios whose id starts with this, or that this starts with.
    QString filter;
    // Multiplies each scenario's repetitions. Warm-up runs stay as they are.
    double repeatScale = 1;
    // The key of the book that is already open, if any.
    QString openBook;
    // Called as each scenario starts, for a run that someone watches.
    std::function<void(const QString &id)> started;
};

// Runs the scenarios in `file`, a parsed scenario file, and returns one result
// per scenario: its id, kind, paths, unit, fixture, status (`completed`,
// `skipped` or `failed`), reason, samples and stages. Diagnostics must be
// active, because samples come from diagnostics events.
QVector<QJsonObject> run(const QJsonObject &file, const Host &host, const Options &options = {});
} // namespace kobalt::scenario
