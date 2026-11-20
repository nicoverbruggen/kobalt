#include "results.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSaveFile>

namespace kobalt::scenario {
namespace {
bool writeResults(const QString &path, const QJsonObject &header,
                  const QVector<QJsonObject> &results)
{
    mainthread::blocking("scenario-results-write");
    QSaveFile output(path);
    const auto writeLine = [&](const QJsonObject &object) {
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        return output.write(bytes) == bytes.size();
    };
    if (!output.open(QIODevice::WriteOnly) || !writeLine(header))
        return false;
    for (const auto &result : results)
        if (!writeLine(result))
            return false;
    return output.commit();
}
} // namespace

void saveResults(storage::RecordWriter &writer, const QString &path, const QJsonObject &header,
                 const QVector<QJsonObject> &results, std::function<void(bool)> done)
{
    const auto finish = [done](bool saved) {
        qt::invoke(qApp, [done, saved] { done(saved); }, Qt::QueuedConnection);
    };
    const bool queued = writer.tryPost([path, header, results, finish] {
        const bool saved = writeResults(path, header, results);
        finish(saved);
        return saved;
    });
    if (!queued)
        finish(false);
}
} // namespace kobalt::scenario
