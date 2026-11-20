#pragma once
#include "storage/record_writer.h"
#include "storage/reader_storage.h"
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

namespace kobalt::scenario {
// Clears the disposable book caches before the next scenario step.
bool clearCaches(storage::RecordWriter &writer, const storage::Paths &paths);

// Saves the device report and delivers the result on the application thread.
void saveResults(storage::RecordWriter &writer, const QString &path, const QJsonObject &header,
                 const QVector<QJsonObject> &results, std::function<void(bool)> done);
} // namespace kobalt::scenario
