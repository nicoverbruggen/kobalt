#pragma once
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QFile>
#include <QDir>
#include <QSet>
#include <QJsonObject>
#include <QFileInfo>
#include <QMap>
#include <algorithm>
#include <memory>
#include <vector>

namespace kobalt::test {
// Each worker owns a process because CREngine fonts and caches are process globals.
// All worker output stays in the parent's temporary tree until validation passes.
// A selection maps a case to the scenes to capture in it. Each selected case
// gets its own worker, which stops after its last selected scene.
inline bool screenshotJobs(const QString &executable, const QString &root,
                           const QString &destination, bool light, QJsonArray &captures,
                           const QMap<QString, QStringList> &selection = {})
{
    bool valid = true;
    const auto setting = qEnvironmentVariable("KOBALT_GALLERY_JOBS");
    const int jobs =
            setting.isEmpty() ? qBound(1, QThread::idealThreadCount(), 8) : setting.toInt(&valid);
    if (!valid || jobs < 1 || jobs > 32) {
        qCritical() << "KOBALT_GALLERY_JOBS must be between 1 and 32";
        return false;
    }
    QProcess inventory;
    inventory.start(executable, { "--screenshot-worker", root + "/inventory", "-datatags" });
    if (!inventory.waitForFinished(30000) || inventory.exitStatus() != QProcess::NormalExit
        || inventory.exitCode()) {
        qCritical() << "Cannot list screenshot cases:" << inventory.readAllStandardError();
        return false;
    }
    QStringList cases;
    for (const auto &line : QString::fromUtf8(inventory.readAllStandardOutput()).split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() != 3 || fields.first() != "ScreenshotTests")
            continue;
        if ((fields[1] == "primaryViews") != light)
            continue;
        cases.append(fields[1] + ':' + fields[2]);
    }
    if (cases.isEmpty() || QSet<QString>(cases.begin(), cases.end()).size() != cases.size()) {
        qCritical() << "Invalid screenshot case inventory";
        return false;
    }
    const auto selectedCases =
            qEnvironmentVariable("KOBALT_GALLERY_CASE").split(',', Qt::SkipEmptyParts);
    if (!selectedCases.isEmpty()) {
        if (light || !selection.isEmpty()) {
            qCritical() << "KOBALT_GALLERY_CASE cannot be combined with light or scene selection";
            return false;
        }
        QStringList filtered;
        for (const auto &name : selectedCases) {
            bool found = false;
            for (const auto &item : cases)
                if (item.section(':', 0, 0) == name) {
                    if (!filtered.contains(item))
                        filtered.append(item);
                    found = true;
                }
            if (!found) {
                qCritical() << "Unknown screenshot case:" << name;
                return false;
            }
        }
        cases = filtered;
    }
    QList<QStringList> batches;
    if (!selection.isEmpty()) {
        for (auto it = selection.cbegin(); it != selection.cend(); ++it) {
            if (!cases.contains(it.key())) {
                qCritical().noquote() << "Screenshot case no longer exists:" << it.key()
                                      << "- run the full gallery first";
                return false;
            }
            batches.append(QStringList{ it.key() });
        }
    } else {
        // Keep workers alive for several cases so immutable font inputs and Qt font
        // registration are prepared once per worker, rather than once per case.
        for (int i = 0; i < qMin(jobs, int(cases.size())); ++i)
            batches.append(QStringList{});
        for (int i = 0; i < cases.size(); ++i)
            batches[i % batches.size()].append(cases.at(i));
    }

    struct Worker
    {
        QProcess process;
        QElapsedTimer timer;
        QString directory;
        QStringList selectors;

        ~Worker()
        {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(5000);
            }
        }
    };

    std::vector<std::unique_ptr<Worker>> active;
    int next = 0, completed = 0;
    while (completed < batches.size()) {
        while (next < batches.size() && int(active.size()) < jobs) {
            auto worker = std::make_unique<Worker>();
            worker->directory = root + "/worker-" + QString::number(next);
            worker->selectors = batches.at(next++);
            if (!QDir().mkpath(worker->directory + "/tmp"))
                return false;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("TMPDIR", worker->directory + "/tmp");
            if (!selection.isEmpty())
                environment.insert("KOBALT_GALLERY_CAPTURE",
                                   selection.value(worker->selectors.first()).join(','));
            worker->process.setProcessEnvironment(environment);
            worker->process.setProcessChannelMode(QProcess::MergedChannels);
            worker->timer.start();
            worker->process.start(executable,
                                  QStringList{ "--screenshot-worker", worker->directory }
                                          + worker->selectors);
            if (!worker->process.waitForStarted(5000))
                return false;
            active.push_back(std::move(worker));
        }
        for (auto it = active.begin(); it != active.end();) {
            auto &worker = **it;
            worker.process.waitForFinished(10);
            if (worker.timer.elapsed() > (light ? 90000 : 480000)) {
                qCritical() << "Screenshot worker timed out:" << worker.selectors;
                return false;
            }
            if (worker.process.state() != QProcess::NotRunning) {
                ++it;
                continue;
            }
            if (worker.process.exitStatus() != QProcess::NormalExit || worker.process.exitCode()) {
                qCritical().noquote() << worker.selectors.join(',') << worker.process.readAll();
                return false;
            }
            QFile record(worker.directory + "/part.json");
            const auto images = record.open(QIODevice::ReadOnly)
                    ? QJsonDocument::fromJson(record.readAll()).array()
                    : QJsonArray{};
            if (images.isEmpty()) {
                qCritical().noquote()
                        << "Screenshot worker recorded no captures:" << worker.selectors.join(',');
                return false;
            }
            for (const auto &image : images) {
                const auto name = image.toObject()["file"].toString();
                if (QFileInfo(name).fileName() != name || !name.endsWith(".png")
                    || !QFile::rename(worker.directory + '/' + name, destination + '/' + name)) {
                    // Another case may already have written this file name.
                    qCritical().noquote() << "Cannot collect screenshot" << name << "from"
                                          << worker.selectors.join(',');
                    return false;
                }
                captures.append(image);
            }
            ++completed;
            qInfo().noquote() << "Gallery worker" << completed << '/' << batches.size()
                              << worker.selectors.size() << "cases"
                              << QString::number(worker.timer.elapsed() / 1000.0, 'f', 2) + "s";
            it = active.erase(it);
        }
    }
    // Completion order changes with scheduling. Keep the manifest stable.
    QList<QJsonObject> ordered;
    for (const auto &image : captures)
        ordered.append(image.toObject());
    std::sort(ordered.begin(), ordered.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a["file"].toString() < b["file"].toString();
    });
    captures = {};
    for (const auto &image : ordered)
        captures.append(image);
    return true;
}
} // namespace kobalt::test
