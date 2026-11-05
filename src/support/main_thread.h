#pragma once
#include <functional>
#include <QElapsedTimer>
#include "diagnostics.h"

// The page turn is sacred. While a screen takes input, the main thread must
// not write storage or wait for another thread: a slow SD card or a busy
// Nickel database would delay the next tap, and a reader who taps again then
// turns two pages.
//
// Operations that write storage or wait for another thread call blocking().
// On the main thread, while a registered screen takes input, that is a
// violation unless an AllowBlocking scope names why the wait is necessary.
// With KOBALT_STRICT_MAIN_THREAD=1 in the environment, as the native tests
// run, a violation aborts with the operation's name. With
// KOBALT_STRICT_MAIN_THREAD=trace, desktop builds log the call stack of each
// distinct violation and continue, to list them all in one run. Otherwise a
// violation is written once per operation to the diagnostic log.
namespace kobalt::mainthread {
// A screen that takes input while `active` returns true. `active` runs only
// when a guarded operation runs on the main thread. Construct and destroy on
// the main thread.
class Interaction
{
public:
    explicit Interaction(std::function<bool()> active);
    ~Interaction();
    Interaction(const Interaction &) = delete;
    Interaction &operator=(const Interaction &) = delete;

private:
    std::function<bool()> active;
};

// A deliberate wait on the main thread. Use it only where the next input
// cannot arrive before the wait ends, or where the user asked for the result,
// and say which in `reason`.
class AllowBlocking
{
public:
    explicit AllowBlocking(const char *reason);
    ~AllowBlocking();
    AllowBlocking(const AllowBlocking &) = delete;
    AllowBlocking &operator=(const AllowBlocking &) = delete;

private:
    const char *previous;
};

// Marks the delivery of one event. A test application wraps
// QApplication::notify with it and calls checkEventsOnly(true). The guard
// then checks what the reader does in response to input, timers and queued
// calls, and ignores the storage reads that the test itself runs between
// events to inspect results. The app always runs inside event delivery.
// Call checkEventsOnly(true) where the test body starts: deliveries already
// open at that point belong to the test runner.
class EventDelivery
{
public:
    EventDelivery();
    ~EventDelivery();
    EventDelivery(const EventDelivery &) = delete;
    EventDelivery &operator=(const EventDelivery &) = delete;
};

void checkEventsOnly(bool enabled);
// Reports an operation that writes storage or waits for another thread.
void blocking(const char *operation);

// Times the actual guarded operation, including deliberate waits. Declare
// beside the work so destruction records its completion, not its admission.
class BlockingOperation
{
public:
    explicit BlockingOperation(const char *operation, bool enabled = true);
    ~BlockingOperation();
    BlockingOperation(const BlockingOperation &) = delete;
    BlockingOperation &operator=(const BlockingOperation &) = delete;

private:
    DiagnosticOperation diagnosticOperation;
    const char *operation;
    const char *reason;
    QElapsedTimer elapsed;
    bool observe;
};

// True on the main thread while a registered screen takes input and no
// AllowBlocking scope is open.
bool interactive();
// Receives each violation instead of the default handling. Tests use it to
// check the guard itself. An empty function restores the default.
void setViolationHandler(std::function<void(const char *operation)> handler);
} // namespace kobalt::mainthread
