#include "files.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
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

void postFileWork(storage::RecordWriter &writer, std::function<bool()> work,
                  std::function<void(bool)> done)
{
    const auto finish = [done](bool ok) {
        qt::invoke(qApp, [done, ok] { done(ok); }, Qt::QueuedConnection);
    };
    if (!writer.tryPost([work, finish] {
            const bool ok = work();
            finish(ok);
            return ok;
        }))
        finish(false);
}
} // namespace

bool clearCaches(storage::RecordWriter &writer, const storage::Paths &paths)
{
    if (paths.directory.isEmpty())
        return false;
    QEventLoop waiting;
    QPointer<QEventLoop> guard(&waiting);
    postFileWork(
            writer,
            [paths] {
                mainthread::blocking("scenario-cache-removal");
                const QFileInfo document(paths.documentCache());
                if (document.isSymLink())
                    return QFile::remove(document.filePath());
                return !document.exists()
                        || (document.isDir() && QDir(document.filePath()).removeRecursively());
            },
            [guard](bool ok) {
                if (guard)
                    guard->exit(ok ? 1 : 0);
            });
    // The next step needs cold caches. Continue to dispatch application events until they are ready.
    return waiting.exec() == 1;
}

void saveResults(storage::RecordWriter &writer, const QString &path, const QJsonObject &header,
                 const QVector<QJsonObject> &results, std::function<void(bool)> done)
{
    postFileWork(
            writer, [path, header, results] { return writeResults(path, header, results); },
            std::move(done));
}

QString checkProfile(const QString &directory)
{
    QFile source(directory + QStringLiteral("/scenarios/scenarios.json"));
    if (!source.open(QIODevice::ReadOnly))
        return {};
    const auto filter =
            QJsonDocument::fromJson(source.readAll()).object().value("filter").toString();
    if (!filter.startsWith(QLatin1String("check.")))
        return {};
    return directory + QStringLiteral("/scenarios/profile");
}
} // namespace kobalt::scenario
