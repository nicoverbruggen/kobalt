#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <sys/types.h>

namespace kobalt {
extern ssize_t (*nativePowerWrite)(int, const void *, size_t);
extern ssize_t (*nativeTouchRead)(int, void *, size_t);

// Only the native power helper's calling thread is traced. Records are held
// in fixed storage until the helper returns, outside the measured write.
class PowerWriteTrace
{
public:
    PowerWriteTrace(const QString &path, quint64 attempt);
    ~PowerWriteTrace();
    PowerWriteTrace(const PowerWriteTrace &) = delete;
    PowerWriteTrace &operator=(const PowerWriteTrace &) = delete;

private:
    bool active = false;
};

quint64 activePowerWrite();
void initializeStandbyDiagnostics();
void sampleStandbyEvidence(quint64 attempt, const char *phase);
// The same bounded read/parser used by the worker, with a fixture root for tests.
void collectStandbyEvidence(const QString &root, quint64 attempt, const char *phase);
void collectStandbyKernelLog(quint64 attempt, const char *phase, quint64 sample,
                             int (*readLog)(int, char *, int));
QStringList standbyKernelMessages(const QByteArray &log, QJsonObject *status = nullptr,
                                  QJsonArray *rejected = nullptr);
void recordStandbyKernelLog(const QByteArray &log, QJsonObject status);
} // namespace kobalt

extern "C" ssize_t kobalt_power_write(int fd, const void *bytes, size_t count);
extern "C" ssize_t kobalt_touch_read(int fd, void *bytes, size_t count);
