#include "main_thread.h"
#include "diagnostics.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QSet>
#include <QThread>
#include <QVector>
#include <QtGlobal>
#include <utility>
#if defined(__APPLE__) || defined(__GLIBC__)
#  include <cstdlib>
#  include <execinfo.h>
#  define KOBALT_MAIN_THREAD_TRACE 1
#endif

namespace kobalt::mainthread {
namespace {
// Registration and checks happen on the main thread only.
QVector<const std::function<bool()> *> screens;
std::function<void(const char *)> handler;
QSet<QByteArray> reported;
thread_local const char *allowed = nullptr;
bool eventsOnly = false;
// Deliveries above `baseline` are the app's own; the test body itself may
// run inside one.
int delivering = 0, baseline = 0;

bool mainThread()
{
    const auto *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

QByteArray mode()
{
    static const auto value = qgetenv("KOBALT_STRICT_MAIN_THREAD");
    return value;
}
#ifdef KOBALT_MAIN_THREAD_TRACE
// For audits: logs each distinct caller once and continues.
void trace(const char *operation)
{
    void *frames[64];
    const int count = backtrace(frames, 64);
    QByteArray key(operation);
    for (int i = 0; i < count; ++i)
        key += QByteArray::number(quintptr(frames[i]), 16);
    if (reported.contains(key))
        return;
    reported.insert(key);
    QByteArray text("main-thread-blocked ");
    text += operation;
    text += '\n';
    char **symbols = backtrace_symbols(frames, count);
    for (int i = 0; symbols && i < count; ++i) {
        text += "    ";
        text += symbols[i];
        text += '\n';
    }
    std::free(symbols);
    qWarning("%s", text.constData());
}
#endif
} // namespace

Interaction::Interaction(std::function<bool()> value) : active(std::move(value))
{
    screens.append(&active);
}

Interaction::~Interaction()
{
    screens.removeAll(&active);
}

AllowBlocking::AllowBlocking(const char *reason) : previous(std::exchange(allowed, reason)) { }

AllowBlocking::~AllowBlocking()
{
    allowed = previous;
}

EventDelivery::EventDelivery()
{
    if (mainThread())
        ++delivering;
}

EventDelivery::~EventDelivery()
{
    if (mainThread())
        --delivering;
}

void checkEventsOnly(bool enabled)
{
    eventsOnly = enabled;
    baseline = delivering;
}

bool interactive()
{
    if (allowed || !mainThread() || (eventsOnly && delivering <= baseline))
        return false;
    for (const auto *active : screens)
        if ((*active)())
            return true;
    return false;
}

void blocking(const char *operation)
{
    if (!interactive())
        return;
    if (handler) {
        handler(operation);
        return;
    }
    if (mode() == "1")
        qFatal("The main thread blocked while a screen takes input: %s", operation);
#ifdef KOBALT_MAIN_THREAD_TRACE
    if (mode() == "trace") {
        trace(operation);
        return;
    }
#endif
    // The diagnostic log is written on its own thread.
    if (!reported.contains(operation)) {
        reported.insert(operation);
        diagnostic("main-thread-blocked", { { "operation", QString::fromLatin1(operation) } },
                   DiagnosticSeverity::Warning);
    }
}

void setViolationHandler(std::function<void(const char *)> value)
{
    handler = std::move(value);
}

BlockingOperation::BlockingOperation(const char *name, bool enabled)
    : diagnosticOperation(enabled ? name : nullptr),
      operation(name),
      reason(allowed),
      observe(enabled && mainThread() && (!eventsOnly || delivering > baseline))
{
    if (enabled)
        blocking(name);
    if (observe)
        elapsed.start();
}

BlockingOperation::~BlockingOperation()
{
    if (!observe)
        return;
    const auto milliseconds = elapsed.nsecsElapsed() / 1e6;
    if (milliseconds < 50 && !diagnosticsActive() && mode() != "trace")
        return;
    const auto fields = qt::object({ { "operation", QString::fromLatin1(operation) },
                                     { "allowance", QString::fromLatin1(reason ? reason : "") },
                                     { "elapsed_ms", milliseconds } });
    diagnostic("main-thread-wait", fields,
               milliseconds >= 50 ? DiagnosticSeverity::Warning : DiagnosticSeverity::Activity);
    if (mode() == "trace")
        qWarning("main-thread-wait %s allowance=%s elapsed_ms=%.3f", operation,
                 reason ? reason : "none", milliseconds);
}
} // namespace kobalt::mainthread
