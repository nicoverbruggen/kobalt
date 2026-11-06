#pragma once
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

// Pewter is a fake Nickel for the Kobo adapter tests. It defines the Nickel
// classes and functions that Kobalt resolves at run time, under the same
// symbol names, and keeps the reading data in a KoboReader.sqlite with the
// columns Kobalt reads and writes. The tests link the production adapter
// objects, drive them, and check the recorded calls and the stored rows.
//
// Pewter proves what Kobalt asks Nickel to do. It does not prove that Nickel
// behaves like this; update it when a firmware changes a function.
namespace pewter {
struct Call
{
    QByteArray name;
    QVariantMap arguments;
    bool mainThread = false;
    bool inTransaction = false;
};

// What the fake Nickel answers. start() resets these to the defaults.
struct Options
{
    bool sleeping = false; // N3PowerWorkflowManager::sleeping
    bool tolino = false; // Device::isTolino
    bool beginFails = false; // DatabaseAdapter::beginTransaction refuses
    bool updateFails = false; // VolumeManager::updateFields refuses
    bool commitFails = false; // DatabaseAdapter::endTransaction(commit) rolls back
    bool syncEligible = true; // BookmarkSyncManagerBase::syncReadingState
    QVariantMap cachedVolume; // getById returns this stale row until refreshCache
    bool refreshClearsCache = true;
};

// Creates a fresh KoboReader.sqlite in `directory` and resets the options and
// the call log. Call it on the main thread after the application exists.
void start(const QString &directory);
Options &options();

const QList<Call> &calls();
QList<QByteArray> callNames();
const Call *find(const QByteArray &name);
void clearCalls();

// The test's own connection, for setup and inspection outside Nickel's.
QSqlDatabase database();
void addContent(const QVariantMap &columns);
QVariantMap row(const QString &contentId);

// Stop the test at the first failed check and name its source line. They
// exit without static destructors: Qt 5.2 can crash in them while the
// application object is still alive, which would hide the failure.
void requireAt(bool condition, const char *message, const char *file, int line);
void requireSameAt(const QVariant &actual, const QVariant &expected, const char *message,
                   const char *file, int line);
void requireNamesAt(const QList<QByteArray> &actual, const QList<QByteArray> &expected,
                    const char *message, const char *file, int line);
} // namespace pewter

#define require(condition, message) \
    ::pewter::requireAt(static_cast<bool>(condition), (message), __FILE__, __LINE__)
#define requireSame(actual, expected, message) \
    ::pewter::requireSameAt((actual), (expected), (message), __FILE__, __LINE__)
#define requireNames(actual, expected, message) \
    ::pewter::requireNamesAt((actual), (expected), (message), __FILE__, __LINE__)
