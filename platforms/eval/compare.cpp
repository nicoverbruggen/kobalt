// Compares two evaluation runs written by platforms/eval/run.cmake and writes
// report.json, report.md and crops of changed screenshots. It only reads its
// inputs; deciding what a difference means is left to the reader of the report.
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace {
QJsonObject readObject(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject();
}

// A device run's results. The built-in scenario player writes scenarios.jsonl:
// a header line, then one line per scenario. Older runners wrote device.json.
QJsonObject readDevice(const QString &directory)
{
    QFile lines(directory + "/scenarios.jsonl");
    if (!lines.open(QIODevice::ReadOnly))
        return readObject(directory + "/device.json");
    QJsonObject document;
    QJsonArray scenarios;
    for (const auto &line : lines.readAll().split('\n')) {
        const auto entry = QJsonDocument::fromJson(line).object();
        if (entry.value("kind").toString() == "kobalt-device-run") {
            document["target"] = entry.value("target");
            continue;
        }
        if (entry.value("id").toString().isEmpty())
            continue;
        auto scenario = entry;
        scenario["samples_ms"] = entry.value("samples");
        scenario["metric_source"] = "event";
        scenarios.append(scenario);
    }
    document["scenarios"] = scenarios;
    return document;
}

double median(QVector<double> values)
{
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

// Median absolute deviation, a spread that one outlier cannot move far.
double deviation(const QVector<double> &values)
{
    const double centre = median(values);
    QVector<double> distances;
    for (const double value : values)
        distances.append(std::abs(value - centre));
    return median(distances);
}

QString number(double value)
{
    return value >= 100  ? QString::number(value, 'f', 0)
            : value >= 1 ? QString::number(value, 'f', 2)
                         : QString::number(value, 'f', 3);
}

struct Run
{
    QString directory;
    QJsonObject meta;
};

// Keep scenario outcomes even when a failed or skipped run has no timing samples.
QJsonObject deviceOutcomes(const Run &run)
{
    QJsonObject outcomes;
    for (const auto &directory : QDir(run.directory).entryList({ "device-*" }, QDir::Dirs)) {
        const auto document = readDevice(run.directory + '/' + directory);
        const auto prefix = "device." + document["target"].toString() + '.';
        for (const auto &value : document["scenarios"].toArray()) {
            const auto scenario = value.toObject();
            const auto id = scenario["id"].toString();
            if (!id.isEmpty())
                outcomes[prefix + id] = QJsonObject{ { "status", scenario["status"] },
                                                     { "reason", scenario["reason"] } };
        }
    }
    return outcomes;
}

QJsonObject compareDeviceOutcomes(const Run &base, const Run &target, QStringList &failed,
                                  QStringList &lines)
{
    const auto before = deviceOutcomes(base), after = deviceOutcomes(target);
    QStringList skipped;
    for (const auto *side : { "base", "target" }) {
        const auto &outcomes = QString(side) == "base" ? before : after;
        for (auto it = outcomes.begin(); it != outcomes.end(); ++it) {
            const auto outcome = it.value().toObject();
            const auto status = outcome["status"].toString();
            const auto message = QString("%1: %2 %3 (%4)")
                                         .arg(side, it.key(), status, outcome["reason"].toString());
            if (status == "failed")
                failed << message;
            else if (status == "skipped")
                skipped << message;
        }
    }
    QJsonArray missing;
    const bool deviceRequested = target.meta["parts"].toArray().contains("device")
            || !QDir(target.directory).entryList({ "device-*" }, QDir::Dirs).isEmpty();
    if (deviceRequested)
        for (auto it = before.begin(); it != before.end(); ++it)
            if (it.value().toObject()["status"].toString() == "completed"
                && !after.contains(it.key())) {
                missing.append(it.key());
                failed << "target: missing device scenario " + it.key();
            }
    if (!skipped.isEmpty())
        lines << "**Skipped device scenarios:** " + skipped.join("; ") + '.' << "";
    return { { "base", before }, { "target", after }, { "missing", missing } };
}

// The target's device checks: each check scenario with its failed checks, and
// the USB and power-off saves that collect-eval.cmake read from the event log.
// A failed check scenario is already a failed part; a failed save adds one.
QJsonObject reportDeviceChecks(const Run &target, QStringList &failed, QStringList &lines)
{
    QJsonObject result;
    QStringList rows;
    for (const auto &directory : QDir(target.directory).entryList({ "device-*" }, QDir::Dirs)) {
        const auto path = target.directory + '/' + directory;
        const auto document = readDevice(path);
        const auto device = "device." + document["target"].toString();
        for (const auto &value : document["scenarios"].toArray()) {
            const auto scenario = value.toObject();
            if (scenario["kind"].toString() != "check")
                continue;
            const auto status = scenario["status"].toString();
            QStringList missed;
            for (const auto &check : scenario["checks"].toArray())
                if (!check.toObject()["pass"].toBool())
                    missed << check.toObject()["check"].toString();
            const auto id = device + '.' + scenario["id"].toString();
            result[id] = QJsonObject{ { "status", status },
                                      { "checks", scenario["checks"].toArray().size() },
                                      { "failed_checks", QJsonArray::fromStringList(missed) } };
            const auto word = status == "completed" ? "pass"
                    : status == "skipped"           ? "skip"
                                                    : "fail";
            rows << QString("| `%1` | %2 | %3 |")
                            .arg(id, word,
                                 status == "completed"
                                         ? QString::number(scenario["checks"].toArray().size())
                                                 + (scenario["checks"].toArray().size() == 1
                                                            ? " check"
                                                            : " checks")
                                         : scenario["reason"].toString().replace('|', '/'));
        }
        const auto manual = readObject(path + "/manual-checks.json");
        for (const auto *key : { "usb_save", "power_off_save" }) {
            if (!manual.contains(key))
                continue;
            const auto outcome = manual[key].toString();
            const auto id = device + '.' + key;
            result[id] = QJsonObject{ { "status", outcome } };
            rows << QString("| `%1` | %2 | manual, from the event log |").arg(id, outcome);
            if (outcome == "failed")
                failed << "target: " + id + " failed";
        }
    }
    if (!rows.isEmpty())
        lines << "## Device checks" << "" << "| Check | Outcome | Detail |" << "| --- | --- | --- |"
              << rows << "";
    return result;
}

// Test outcomes from CTest's JUnit file, by test name.
QMap<QString, QString> testOutcomes(const QString &path)
{
    QMap<QString, QString> outcomes;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return outcomes;
    QXmlStreamReader xml(&file);
    QString current;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("testcase")) {
            current = xml.attributes().value("name").toString();
            const auto status = xml.attributes().value("status").toString();
            outcomes[current] = status == "fail"                 ? "failed"
                    : status == "notrun" || status == "disabled" ? "skipped"
                                                                 : "passed";
        } else if (xml.isStartElement() && !current.isEmpty()
                   && (xml.name() == QLatin1String("failure")
                       || xml.name() == QLatin1String("error"))) {
            outcomes[current] = "failed";
        } else if (xml.isStartElement() && !current.isEmpty()
                   && xml.name() == QLatin1String("skipped")) {
            outcomes[current] = "skipped";
        } else if (xml.isEndElement() && xml.name() == QLatin1String("testcase")) {
            current.clear();
        }
    }
    return outcomes;
}

QJsonObject compareTests(const Run &base, const Run &target, QStringList &lines)
{
    const auto before = testOutcomes(base.directory + "/tests.xml");
    const auto after = testOutcomes(target.directory + "/tests.xml");
    QJsonArray newlyFailing, newlyPassing, added, removed, stillFailing;
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (!before.contains(it.key()))
            added.append(it.key());
        else if (it.value() == "failed" && before[it.key()] != "failed")
            newlyFailing.append(it.key());
        else if (it.value() != "failed" && before[it.key()] == "failed")
            newlyPassing.append(it.key());
        else if (it.value() == "failed")
            stillFailing.append(it.key());
    }
    for (auto it = before.begin(); it != before.end(); ++it)
        if (!after.contains(it.key()))
            removed.append(it.key());
    lines << "## Tests" << "";
    if (before.isEmpty() || after.isEmpty()) {
        lines << "Not compared: a run has no test results." << "";
        return { { "compared", false } };
    }
    lines << QString("%1 tests in the target. Newly failing: %2. Newly passing: %3. Added: %4. Removed: %5. "
                     "Failing in both: %6.")
                     .arg(after.size())
                     .arg(newlyFailing.size())
                     .arg(newlyPassing.size())
                     .arg(added.size())
                     .arg(removed.size())
                     .arg(stillFailing.size());
    for (const auto &name : newlyFailing)
        lines << "- Newly failing: `" + name.toString() + "`";
    for (const auto &name : removed)
        lines << "- Removed: `" + name.toString() + "`";
    lines << "";
    return { { "compared", true },
             { "count", after.size() },
             { "newly_failing", newlyFailing },
             { "newly_passing", newlyPassing },
             { "added", added },
             { "removed", removed },
             { "both_failing", stillFailing } };
}

// Changed screens with the bounding box of changed pixels and three crops.
QJsonObject compareScreens(const Run &base, const Run &target, const QJsonObject &config,
                           const QString &output, QStringList &lines)
{
    const auto load = [](const Run &run) {
        QMap<QString, QJsonObject> images;
        for (const auto &value :
             readObject(run.directory + "/screenshots/manifest.json")["images"].toArray())
            images[value.toObject()["file"].toString()] = value.toObject();
        return images;
    };
    const auto before = load(base), after = load(target);
    lines << "## Screenshots" << "";
    if (before.isEmpty() || after.isEmpty()) {
        lines << "Not compared: a run has no screenshots." << "";
        return { { "compared", false } };
    }
    // Screens that show the commit, the time or a temporary path change on every run.
    const auto volatileScreen = [&config](const QString &screen) {
        for (const auto &value : config["volatile_screens"].toArray())
            if (screen.startsWith(value.toObject()["prefix"].toString()))
                return true;
        return false;
    };
    QJsonArray changed, added, removed;
    int unchanged = 0;
    QDir().mkpath(output + "/screens");
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (!before.contains(it.key())) {
            added.append(it.key());
            continue;
        }
        if (before[it.key()]["sha256"] == it.value()["sha256"]) {
            ++unchanged;
            continue;
        }
        QJsonObject entry{ { "file", it.key() },
                           { "screen", it.value()["screen"] },
                           { "theme", it.value()["theme"] },
                           { "device", it.value()["device"] },
                           { "volatile", volatileScreen(it.value()["screen"].toString()) } };
        const QImage first(base.directory + "/screenshots/" + it.key()),
                second(target.directory + "/screenshots/" + it.key());
        if (!first.isNull() && first.size() == second.size()) {
            const auto a = first.convertToFormat(QImage::Format_ARGB32),
                       b = second.convertToFormat(QImage::Format_ARGB32);
            QRect box;
            qint64 pixels = 0;
            QImage mask(a.size(), QImage::Format_ARGB32);
            mask.fill(Qt::white);
            for (int y = 0; y < a.height(); ++y) {
                const auto *left = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                const auto *right = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                auto *marks = reinterpret_cast<QRgb *>(mask.scanLine(y));
                for (int x = 0; x < a.width(); ++x)
                    if (left[x] != right[x]) {
                        ++pixels;
                        box |= QRect(x, y, 1, 1);
                        marks[x] = qRgb(220, 0, 0);
                    }
            }
            // A new checksum without a changed pixel is PNG metadata only.
            if (!pixels) {
                ++unchanged;
                continue;
            }
            entry["pixels"] = double(pixels);
            entry["box"] = QJsonArray{ box.x(), box.y(), box.width(), box.height() };
            const auto crop = box.adjusted(-24, -24, 24, 24).intersected(a.rect());
            const auto stem = output + "/screens/" + QFileInfo(it.key()).completeBaseName();
            first.copy(crop).save(stem + "-base.png");
            second.copy(crop).save(stem + "-target.png");
            mask.copy(crop).save(stem + "-mask.png");
        } else {
            entry["note"] = "image missing or size changed";
        }
        changed.append(entry);
    }
    for (auto it = before.begin(); it != before.end(); ++it)
        if (!after.contains(it.key()))
            removed.append(it.key());
    int unexpected = 0;
    for (const auto &value : changed)
        unexpected += !value.toObject()["volatile"].toBool();
    lines << QString("%1 compared, %2 unchanged, %3 changed (%4 outside screens that vary by design), %5 added, %6 removed.")
                     .arg(after.size())
                     .arg(unchanged)
                     .arg(changed.size())
                     .arg(unexpected)
                     .arg(added.size())
                     .arg(removed.size());
    for (const auto &value : changed) {
        const auto entry = value.toObject();
        if (entry["volatile"].toBool())
            continue;
        const auto box = entry["box"].toArray();
        lines << QString("- `%1`: %2 pixels in %3×%4 at %5,%6. Crops in `screens/`.")
                         .arg(entry["file"].toString())
                         .arg(entry["pixels"].toDouble())
                         .arg(box.at(2).toInt())
                         .arg(box.at(3).toInt())
                         .arg(box.at(0).toInt())
                         .arg(box.at(1).toInt());
    }
    lines << "";
    return { { "compared", true },
             { "unchanged", unchanged },
             { "changed", changed },
             { "added", added },
             { "removed", removed } };
}

struct Metric
{
    QString kind, source, unit = "ms";
    // True for a stage of a resource scenario. Its values can be zero or
    // negative, so the verdict uses the difference, not the ratio.
    bool resource = false;
    QStringList paths;
    QVector<double> processMedians, samples;
    // Device runs also record the time observed from input to result.
    QVector<double> observedMedians, observed;
};

// How a resource stage is summed up per process, and its unit and kind.
// State at the end of a phase is the last sample; amounts are the mean per
// sample, so one write in ten turns still shows; times are the median.
struct ResourceStage
{
    enum class Summary { Median, Mean, Last } summary;
    QString unit, kind;
};

ResourceStage resourceStage(const QString &stage, const QStringList &counts)
{
    using Summary = ResourceStage::Summary;
    if (counts.contains(stage))
        return { Summary::Mean, "count", "resource-count" };
    if (stage == "rss_end_kib" || stage == "heap_end_kib" || stage == "peak_kib")
        return { Summary::Last, "KiB", "resource-memory" };
    if (stage.endsWith("_kib"))
        return { Summary::Mean, "KiB", "resource-memory" };
    if (stage.endsWith("_bytes"))
        return { Summary::Mean, "B", "resource-io" };
    if (stage == "block_writes" || stage == "write_calls")
        return { Summary::Mean, "count", "resource-io" };
    if (stage.endsWith("_ms"))
        return { Summary::Median, "ms", "resource" };
    // Another field of an event, such as the pages of the whole book.
    return { Summary::Mean, "count", "resource-count" };
}

double summarize(const QVector<double> &values, ResourceStage::Summary summary)
{
    if (values.isEmpty())
        return 0;
    if (summary == ResourceStage::Summary::Last)
        return values.last();
    if (summary == ResourceStage::Summary::Mean) {
        double total = 0;
        for (const double value : values)
            total += value;
        return total / values.size();
    }
    return median(values);
}

// Adds a metric for each stage of a resource scenario, as `<id>:<stage>`.
// The CPU time of single threads stays in the raw results only: thread
// names are not stable enough to compare.
void addResourceMetrics(QMap<QString, Metric> &result, const QString &id,
                        const QJsonObject &scenario, const QStringList &paths)
{
    QStringList counts;
    for (const auto &value : scenario["counts"].toArray())
        counts << value.toString();
    const auto stages = scenario["stages"].toObject();
    for (auto it = stages.begin(); it != stages.end(); ++it) {
        if (it.key().startsWith("thread_cpu_ms."))
            continue;
        QVector<double> values;
        for (const auto &value : it.value().toArray())
            values.append(value.toDouble());
        if (values.isEmpty())
            continue;
        const auto stage = resourceStage(it.key(), counts);
        auto &metric = result[id + ':' + it.key()];
        metric.kind = stage.kind;
        metric.unit = stage.unit;
        metric.resource = true;
        metric.paths = paths;
        metric.source = "resources";
        metric.processMedians.append(summarize(values, stage.summary));
        metric.samples += values;
    }
}

// Samples by scenario id. Each process contributes its median, so a slow
// process counts once rather than once per sample.
QMap<QString, Metric> metrics(const Run &run, bool device)
{
    QMap<QString, Metric> result;
    QStringList files;
    if (device) {
        for (const auto &target : QDir(run.directory).entryList({ "device-*" }, QDir::Dirs))
            files << run.directory + "/" + target;
    } else {
        for (const auto &name : QDir(run.directory).entryList({ "perf-*.json" }, QDir::Files))
            files << run.directory + "/" + name;
    }
    for (const auto &file : files) {
        const auto document = device ? readDevice(file) : readObject(file);
        const auto prefix = device ? "device." + document["target"].toString() + "." : QString();
        for (const auto &value : document["scenarios"].toArray()) {
            const auto scenario = value.toObject();
            if (device && scenario["status"].toString() != "completed")
                continue;
            QVector<double> samples;
            for (const auto &sample : scenario["samples_ms"].toArray())
                samples.append(sample.toDouble());
            if (samples.isEmpty())
                continue;
            auto &metric = result[prefix + scenario["id"].toString()];
            metric.kind = device ? "device" : scenario["kind"].toString();
            metric.unit = scenario["unit"].toString("ms");
            metric.paths.clear();
            for (const auto &path : scenario["paths"].toArray())
                metric.paths.append(path.toString());
            metric.processMedians.append(median(samples));
            metric.samples += samples;
            metric.source = scenario["metric_source"].toString("event");
            if (scenario["resources"].toBool())
                addResourceMetrics(result, prefix + scenario["id"].toString(), scenario,
                                   metric.paths);
            QVector<double> observed;
            for (const auto &sample : scenario["stages"].toObject()["observed_ms"].toArray())
                observed.append(sample.toDouble());
            if (!observed.isEmpty()) {
                metric.observedMedians.append(median(observed));
                metric.observed += observed;
            }
        }
    }
    return result;
}

// The resource scenarios of a run, by id, with their limits and stages.
QMap<QString, QJsonObject> resourceScenarios(const Run &run)
{
    QMap<QString, QJsonObject> result;
    for (const auto &name : QDir(run.directory).entryList({ "perf-*.json" }, QDir::Files))
        for (const auto &value : readObject(run.directory + "/" + name)["scenarios"].toArray()) {
            const auto scenario = value.toObject();
            if (!scenario["resources"].toBool())
                continue;
            // Several processes: keep every sample of every process.
            auto &entry = result[scenario["id"].toString()];
            if (entry.isEmpty()) {
                entry = scenario;
                continue;
            }
            auto stages = entry["stages"].toObject();
            const auto more = scenario["stages"].toObject();
            for (auto it = more.begin(); it != more.end(); ++it) {
                auto list = stages[it.key()].toArray();
                for (const auto &sample : it.value().toArray())
                    list.append(sample);
                stages[it.key()] = list;
            }
            entry["stages"] = stages;
        }
    return result;
}

// The pass or fail of the resource gate. It fails when a sample of the
// target exceeds a limit in its scenario's `expect`, such as one profile
// write during a plain page turn; when a resource scenario of the base is
// missing from the target; and when a resource metric got slower or higher
// beyond its band. Limits are exact: noise never excuses a write.
QJsonObject resourceGate(const Run &base, const Run &target, const QJsonObject &perf,
                         QStringList &lines)
{
    const auto before = resourceScenarios(base), after = resourceScenarios(target);
    QStringList failures;
    int checked = 0;
    for (auto it = before.begin(); it != before.end(); ++it)
        if (!after.contains(it.key()))
            failures << QString("`%1` was not measured in the target").arg(it.key());
    for (auto it = after.begin(); it != after.end(); ++it) {
        const auto expect = it.value()["expect"].toObject();
        const auto stages = it.value()["stages"].toObject();
        for (auto limit = expect.begin(); limit != expect.end(); ++limit) {
            ++checked;
            const auto samples = stages[limit.key()].toArray();
            if (samples.isEmpty()) {
                failures << QString("`%1:%2` has no samples").arg(it.key(), limit.key());
                continue;
            }
            int over = 0;
            double highest = 0;
            for (const auto &sample : samples) {
                if (sample.toDouble() > limit.value().toDouble())
                    ++over;
                highest = std::max(highest, sample.toDouble());
            }
            if (over)
                failures << QString("`%1:%2` is above %3 in %4 of %5 samples, at most %6")
                                    .arg(it.key(), limit.key(),
                                         QString::number(limit.value().toDouble()))
                                    .arg(over)
                                    .arg(samples.size())
                                    .arg(QString::number(highest));
        }
    }
    for (const auto &value : perf["metrics"].toArray()) {
        const auto row = value.toObject();
        const auto verdict = row["verdict"].toString();
        if (row["resource"].toBool() && (verdict == "slower" || verdict == "higher"))
            failures << QString("`%1` is %2: %3 against %4 %5")
                                .arg(row["id"].toString(), verdict,
                                     number(row["target_ms"].toDouble()),
                                     number(row["base_ms"].toDouble()), row["unit"].toString());
    }
    const bool measured = !after.isEmpty() || !before.isEmpty();
    const auto verdict = !measured ? QStringLiteral("not measured")
            : failures.isEmpty()   ? QStringLiteral("pass")
                                   : QStringLiteral("fail");
    lines << "## Resource gate" << "";
    if (!measured) {
        lines << "Not checked: neither run has resource scenarios." << "";
    } else {
        lines << QString("%1. %2 exact limits checked in the target.")
                         .arg(verdict == "pass" ? "Pass" : "Fail")
                         .arg(checked)
              << "";
        for (const auto &failure : failures)
            lines << "- " + failure;
        if (!failures.isEmpty())
            lines << "";
    }
    return { { "verdict", verdict },
             { "checked", checked },
             { "failures", QJsonArray::fromStringList(failures) } };
}

QJsonObject compareMetrics(const Run &base, const Run &target, bool device,
                           const QJsonObject &config, const QJsonObject &calibration,
                           const QStringList &changes, QStringList &lines, QJsonArray &flags)
{
    const auto before = metrics(base, device), after = metrics(target, device);
    lines << (device ? "## Device" : "## Performance") << "";
    if (before.isEmpty() || after.isEmpty()) {
        lines << "Not compared: a run has no " + QString(device ? "device" : "performance")
                        + " results."
              << "";
        return { { "compared", false } };
    }
    lines << "| Metric | Base | Target | Ratio | Spread | Verdict | Touched by |"
          << "|---|---:|---:|---:|---:|---|---|";
    QJsonArray rows;
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (!before.contains(it.key()))
            continue;
        auto a = before[it.key()], b = it.value();
        // A build without the metric's event reports observed time instead.
        // Compare like with like: observed time on both sides.
        QString basis = a.source;
        if (a.source != b.source) {
            if (a.observedMedians.isEmpty() || b.observedMedians.isEmpty()) {
                rows.append(QJsonObject{ { "id", it.key() }, { "verdict", "not comparable" } });
                lines << QString("| `%1` | | | | | not comparable: the runs measured different things | |")
                                 .arg(it.key());
                continue;
            }
            for (auto *metric : { &a, &b }) {
                metric->processMedians = metric->observedMedians;
                metric->samples = metric->observed;
            }
            basis = "observed";
        }
        // Several processes give a spread between processes; one process
        // only has the spread of its own samples.
        const auto centre = [](const Metric &m) { return median(m.processMedians); };
        const auto spread = [](const Metric &m) {
            const double c = median(m.processMedians);
            const double d = m.processMedians.size() > 1 ? deviation(m.processMedians)
                                                         : deviation(m.samples);
            return c != 0 ? d / std::abs(c) : 0;
        };
        const double baseMs = centre(a), targetMs = centre(b);
        // Negative amounts of the same sign compare by size.
        const double ratio = baseMs != 0 && targetMs / baseMs > 0 ? targetMs / baseMs : 0;
        // A few A/A pairs are not enough to trust a band below the default.
        const QJsonValue calibrated = calibration["pairs"].toInt() >= 3
                ? calibration["bands"].toObject().value(it.key())
                : QJsonValue();
        const double band = calibrated.isDouble()
                ? calibrated.toDouble()
                : config["bands"].toObject()[b.kind].toDouble(0.2);
        // Each unit has its own floor: memory in KiB, bytes, counts and times.
        const double floor = b.unit == "KiB"
                ? config["floors_kib"].toObject()[b.kind].toDouble(1024)
                : b.unit == "B"     ? config["floors_bytes"].toObject()[b.kind].toDouble(4096)
                : b.unit == "count" ? config["floors_count"].toObject()[b.kind].toDouble(1)
                                    : config["floors_ms"].toObject()[b.kind].toDouble(1);
        const double noise = std::max(spread(a), spread(b));
        const double change = std::abs(std::log(ratio > 0 ? ratio : 1));
        QString verdict = "same";
        // A resource amount can be zero or negative, such as writes during a
        // page turn or memory freed by a close. Two amounts of the same sign
        // compare by the ratio of their sizes. A change from or to zero, or
        // across zero, has no ratio; the floor alone decides it. Counted
        // events, such as profile transactions, are exact: any change beyond
        // the floor counts, whatever the ratio.
        const bool sameSign = (baseMs > 0 && targetMs > 0) || (baseMs < 0 && targetMs < 0);
        const bool signedAmount = b.resource && (!sameSign || b.kind == "resource-count");
        const auto more = b.unit == "ms" ? QStringLiteral("slower") : QStringLiteral("higher");
        const auto less = b.unit == "ms" ? QStringLiteral("faster") : QStringLiteral("lower");
        if (std::abs(targetMs - baseMs) >= floor && (signedAmount || change > std::log(1 + band)))
            verdict = targetMs > baseMs ? more : less;
        // From a negative amount to a small positive one, such as memory
        // that the system reclaimed during one base sample, is not growth:
        // the target must itself reach the floor.
        if (b.resource && !sameSign && verdict == more && targetMs < floor)
            verdict = "same";
        // A noisy metric keeps only changes far larger than its own spread.
        if (!signedAmount && verdict != "same" && noise > band && change < 3 * std::log(1 + noise))
            verdict = "noisy";
        QStringList touched;
        for (const auto &file : changes)
            for (const auto &path : b.paths)
                if (file.startsWith(path) && !touched.contains(file))
                    touched << file;
        QJsonObject row{ { "id", it.key() },
                         { "kind", b.kind },
                         { "basis", basis },
                         { "unit", b.unit },
                         { "base_ms", baseMs },
                         { "target_ms", targetMs },
                         { "ratio", ratio },
                         { "band", band },
                         { "spread", noise },
                         { "verdict", verdict },
                         { "resource", b.resource },
                         { "processes",
                           QJsonArray{ a.processMedians.size(), b.processMedians.size() } },
                         { "touched_by", QJsonArray::fromStringList(touched) } };
        rows.append(row);
        const auto shownRatio =
                baseMs > 0 && targetMs > 0 ? QString::number(ratio, 'f', 2) : QStringLiteral("n/a");
        if (verdict == "noisy" && b.resource) {
            flags.append(QString("%1 is noisy (%2 → %3 %4). Measure again with more runs.")
                                 .arg(it.key(), number(baseMs), number(targetMs), b.unit));
        } else if (verdict != "same" && verdict != "noisy") {
            const bool unexpected = touched.isEmpty();
            flags.append(QString("%1 %2 %3× (%4 → %5 %6)%7")
                                 .arg(it.key(), verdict, shownRatio, number(baseMs),
                                      number(targetMs), b.unit,
                                      unexpected ? ", unexpected: no changed file touches it"
                                                 : QString()));
        }
        lines << QString("| `%1`%8 | %2 | %3 | %4 | %5 | %6 | %7 |")
                         .arg(it.key(), number(baseMs) + " " + b.unit,
                              number(targetMs) + " " + b.unit, shownRatio,
                              QString::number(noise * 100, 'f', 0) + "%", verdict,
                              touched.isEmpty() ? QString()
                                                : QString::number(touched.size()) + " files",
                              basis == "observed" ? " (observed)" : "");
    }
    lines << "";
    return { { "compared", true }, { "metrics", rows } };
}
} // namespace

int main(int argc, char **argv)
{
    // QImage needs a Gui application but no screen.
    qputenv("QT_QPA_PLATFORM", "minimal");
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOptions(
            { { "base", "Base run directory.", "dir" },
              { "target", "Target run directory.", "dir" },
              { "output", "Report directory.", "dir" },
              { "config", "Evaluation defaults (eval.json).", "file" },
              { "calibration", "Measured noise bands for this machine.", "file" },
              { "changes", "Changed files between base and target, one per line.", "file" },
              { "commits", "Commits between base and target, one per line.", "file" },
              { "record-calibration", "Add this A/A comparison to the calibration file." } });
    parser.process(app);
    const Run base{ parser.value("base"), readObject(parser.value("base") + "/meta.json") };
    const Run target{ parser.value("target"), readObject(parser.value("target") + "/meta.json") };
    const auto output = parser.value("output");
    if (base.meta.isEmpty() || target.meta.isEmpty() || output.isEmpty()
        || !QDir().mkpath(output)) {
        qCritical(
                "Usage: kobalt-eval-compare --base RUN --target RUN --output DIR [--config eval.json] "
                "[--calibration file] [--changes file] [--commits file]");
        return 2;
    }
    const auto config = readObject(parser.value("config"));
    const auto calibration = readObject(parser.value("calibration"));
    const auto readLines = [](const QString &path) {
        QFile file(path);
        QStringList result;
        if (file.open(QIODevice::ReadOnly))
            for (const auto &line : QString::fromUtf8(file.readAll()).split('\n'))
                if (!line.trimmed().isEmpty())
                    result << line.trimmed();
        return result;
    };
    const auto changes = readLines(parser.value("changes"));
    const auto commits = readLines(parser.value("commits"));
    const auto subject = [](const Run &run) {
        const auto s = run.meta["subject"].toObject();
        return s["label"].toString() + " (" + s["sha"].toString().left(12)
                + (s["dirty"].toBool() ? ", modified" : "") + ")";
    };
    QStringList lines{
        "# Evaluation report", "",
        "Base: " + subject(base) + ", run " + QFileInfo(base.directory).fileName() + ".",
        "Target: " + subject(target) + ", run " + QFileInfo(target.directory).fileName() + ".", ""
    };
    // Timings only compare on the same machine, power source and profile.
    QStringList notes;
    for (const auto *key : { "hw_model", "power", "low_power_mode" }) {
        const QJsonValue a = base.meta["host"].toObject().value(key),
                         b = target.meta["host"].toObject().value(key);
        if (a != b)
            notes << QString("%1 differs: %2 against %3")
                             .arg(key, a.toVariant().toString(), b.toVariant().toString());
    }
    if (base.meta["profile"] != target.meta["profile"])
        notes << "the runs used different profiles";
    if (base.meta["device_filter"].toString() != target.meta["device_filter"].toString())
        notes << "the device runs used different scenario filters";
    if (!notes.isEmpty())
        lines << "**Measure again before trusting timings:** " + notes.join("; ") + "." << "";
    // A part that failed must not read like a part that was never run.
    QStringList failed;
    for (const auto *side : { "base", "target" }) {
        const auto &meta = QString(side) == "base" ? base.meta : target.meta;
        QStringList parts;
        for (const auto *key : { "tests_exit", "screenshots_exit" }) {
            const auto exit = meta[key].toString();
            if (!exit.isEmpty() && exit != "0")
                parts << QString(key).section('_', 0, 0) + " (exit " + exit + ")";
        }
        if (meta["perf_failures"].toInt())
            parts << QString("performance (%1 failed)").arg(meta["perf_failures"].toInt());
        for (const auto &target : meta["device_failures"].toArray())
            parts << "device " + target.toString();
        if (!parts.isEmpty())
            failed << QString(side) + ": " + parts.join(", ");
    }
    const auto deviceScenarios = compareDeviceOutcomes(base, target, failed, lines);
    QStringList checkLines;
    const auto deviceChecks = reportDeviceChecks(target, failed, checkLines);
    if (!failed.isEmpty())
        lines << "**Failed parts:** " + failed.join("; ") + ". See the logs in each run directory."
              << "";
    QJsonArray flags;
    QJsonObject report{ { "schema", 1 },
                        { "kind", "kobalt-eval-report" },
                        { "base", base.meta },
                        { "target", target.meta },
                        { "comparable_notes", QJsonArray::fromStringList(notes) } };
    report["device_scenarios"] = deviceScenarios;
    report["device_checks"] = deviceChecks;
    lines << checkLines;
    report["failed_parts"] = QJsonArray::fromStringList(failed);
    report["tests"] = compareTests(base, target, lines);
    report["screenshots"] = compareScreens(base, target, config, output, lines);
    report["perf"] =
            compareMetrics(base, target, false, config, calibration, changes, lines, flags);
    report["device"] =
            compareMetrics(base, target, true, config, calibration, changes, lines, flags);
    report["gate"] = resourceGate(base, target, report["perf"].toObject(), lines);
    lines << "## Changes" << ""
          << QString("%1 commits and %2 changed files between base and target.")
                     .arg(commits.size())
                     .arg(changes.size())
          << "";
    for (const auto &commit : commits)
        lines << "- " + commit;
    if (!commits.isEmpty())
        lines << "";
    QStringList summary;
    for (const auto &flag : flags)
        summary << "- " + flag.toString();
    const bool timed = report["perf"].toObject()["compared"].toBool()
            || report["device"].toObject()["compared"].toBool();
    const auto gate = report["gate"].toObject()["verdict"].toString();
    if (gate != "not measured")
        lines.insert(5, QString("Resource gate: %1.\n").arg(gate));
    lines.insert(5,
                 !failed.isEmpty() ? "Evaluation contains failed or missing results.\n"
                         : !timed  ? "Timings were not compared: a run has no timing results.\n"
                         : summary.isEmpty() ? "No timing changes beyond the noise bands.\n"
                                             : "Metrics to review:\n\n" + summary.join('\n') + "\n");
    report["flags"] = flags;
    // Two runs of the same subject show how far each metric moves by itself.
    // Each band allows 1.5 times the largest such move on a ratio scale, and at
    // least 5%. Comparisons use the bands once three pairs are recorded.
    if (parser.isSet("record-calibration") && !parser.value("calibration").isEmpty()) {
        auto observed = calibration["observed"].toObject();
        QJsonObject bands;
        for (const auto *section : { "perf", "device" })
            for (const auto &value : report[section].toObject()["metrics"].toArray()) {
                const auto row = value.toObject();
                // A resource amount at zero has no ratio to learn from.
                if (row["ratio"].toDouble() <= 0)
                    continue;
                auto history = observed[row["id"].toString()].toArray();
                history.append(std::abs(std::log(row["ratio"].toDouble(1))));
                while (history.size() > 20)
                    history.removeFirst();
                observed[row["id"].toString()] = history;
            }
        for (auto it = observed.begin(); it != observed.end(); ++it) {
            double largest = 0;
            for (const auto &move : it.value().toArray())
                largest = std::max(largest, move.toDouble());
            bands[it.key()] = std::max(0.05, std::exp(1.5 * largest) - 1);
        }
        const QJsonObject updated{ { "schema", 1 },
                                   { "kind", "kobalt-eval-calibration" },
                                   { "pairs", calibration["pairs"].toInt() + 1 },
                                   { "observed", observed },
                                   { "bands", bands } };
        QSaveFile file(parser.value("calibration"));
        const auto bytes = QJsonDocument(updated).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return 2;
    }
    report["changes"] = QJsonObject{ { "commits", QJsonArray::fromStringList(commits) },
                                     { "files", QJsonArray::fromStringList(changes) } };
    QSaveFile json(output + "/report.json"), markdown(output + "/report.md");
    const auto bytes = QJsonDocument(report).toJson();
    const auto text = lines.join('\n').toUtf8() + '\n';
    if (!json.open(QIODevice::WriteOnly) || json.write(bytes) != bytes.size() || !json.commit()
        || !markdown.open(QIODevice::WriteOnly) || markdown.write(text) != text.size()
        || !markdown.commit())
        return 2;
    return 0;
}
