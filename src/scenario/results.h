#pragma once
#include "storage/record_writer.h"
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

namespace kobalt::scenario {
// Saves the device report and delivers the result on the application thread.
void saveResults(storage::RecordWriter &writer, const QString &path, const QJsonObject &header,
                 const QVector<QJsonObject> &results, std::function<void(bool)> done);
} // namespace kobalt::scenario
