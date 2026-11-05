#pragma once
#include "qt_compat.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <cerrno>
#include <functional>

namespace kobalt {
QJsonObject diagnosticDeviceFields(const QByteArray &version);
QString diagnosticErrorCode(const QString &reason);
// Owners publish counters; the worker samples them without touching GUI caches.
void setDiagnosticFontBytes(qint64 bytes);
void setDiagnosticNativeFontBytes(qint64 bytes);
void setDiagnosticPreviewBytes(int bytes);
void pauseDiagnostics(bool paused);
// True only for detailed diagnostics or an evaluation observer. Callers must
// check before collecting timings, memory statistics or per-page details.
bool diagnosticsActive();
void initializeDiagnostics(const QString &directory, bool detailed = true);
bool diagnosticsRequested(const QString &profile);

// Start/end records survive an unfinished operation. They enqueue only small
// fields; JSON formatting and file writes stay on the diagnostic worker.
class DiagnosticOperation
{
public:
    explicit DiagnosticOperation(const char *operation);
    ~DiagnosticOperation();
    DiagnosticOperation(const DiagnosticOperation &) = delete;
    DiagnosticOperation &operator=(const DiagnosticOperation &) = delete;

private:
    const char *operation;
    quint64 id = 0;
    QElapsedTimer elapsed;
};
enum class DiagnosticSeverity { Activity, Warning, Error };
// Normal activity is written in unsynced batches. Warnings, errors and
// explicit lifecycle flushes sync the batch. Formatting runs on the worker.
void diagnostic(const char *event, QJsonObject fields, DiagnosticSeverity severity);
// Compatibility for callers that selected per-event durability. Storage now
// syncs whole batches; this flag does not imply severity.
void diagnostic(const char *event, QJsonObject fields = {}, bool durable = true);
// Defers a pending batch until input is quiet, with a five-second maximum.
void diagnosticInput();
// Call after an open, database or network call fails. When the error says the
// process or system ran out of file descriptors (EMFILE or ENFILE), logs an
// error with the open descriptors: the total, the count by kind and the five
// most opened files. Other errors do nothing. It runs in every build, at most
// once a minute, and keeps errno unchanged.
void reportDescriptorExhaustion(const char *context, int error = errno);
// Explicit performance sample, collected and delivered on the logging worker.
// Once the base is known, it also has `kobalt_rss_kib` and `kobalt_peak_kib`:
// resident and peak memory above that base.
void sampleDiagnosticMemory(std::function<void(QJsonObject)> done);
// Bounded diagnostic sampling on the logging worker, in request order.
// Returns false when the queue is full. The callback must not wait for input.
bool sampleDiagnostics(std::function<void()> sample);
// Records the process's resident memory as the base for Kobalt's own share,
// once per process. Call it just before the first book opens. On Kobo the
// base is Nickel with Kobalt loaded but not yet reading, so the share leaves
// out the host. Logs a `memory-base` event.
void markMemoryBase();
// Waits until queued events are in the log file. False when `milliseconds`
// pass first; a negative value waits without a limit. For tests and tools
// that read the file and lifecycle boundaries where input is blocked.
bool flushDiagnostics(int milliseconds = -1);
// Whether warning, error and lifecycle batches sync storage. Normal
// evaluation uses the same policy as reading. Legacy baselines may opt out.
void setDiagnosticsSync(bool enabled);
// Receives every event that is written, on the thread that wrote it. An empty
// function removes the observer.
void setDiagnosticObserver(std::function<void(const QJsonObject &)> observer);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// Qt 5.2 cannot build a QJsonObject from a braced list of fields.
inline void diagnostic(const char *event, std::initializer_list<qt::JsonField> fields,
                       bool durable = true)
{
    diagnostic(event, qt::object(fields), durable);
}

inline void diagnostic(const char *event, std::initializer_list<qt::JsonField> fields,
                       DiagnosticSeverity severity)
{
    diagnostic(event, qt::object(fields), severity);
}
#endif
} // namespace kobalt
