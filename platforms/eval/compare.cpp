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
    QStringList paths;
    QVector<double> processMedians, samples;
    // Device runs also record the time observed from input to result.
    QVector<double> observedMedians, observed;
};

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
            return c > 0 ? d / c : 0;
        };
        const double baseMs = centre(a), targetMs = centre(b);
        const double ratio = baseMs > 0 ? targetMs / baseMs : 0;
        // A few A/A pairs are not enough to trust a band below the default.
        const QJsonValue calibrated = calibration["pairs"].toInt() >= 3
                ? calibration["bands"].toObject().value(it.key())
                : QJsonValue();
        const double band = calibrated.isDouble()
                ? calibrated.toDouble()
                : config["bands"].toObject()[b.kind].toDouble(0.2);
        // Memory is sampled in KiB, so its floor is too.
        const double floor = b.unit == "KiB"
                ? config["floors_kib"].toObject()[b.kind].toDouble(1024)
                : config["floors_ms"].toObject()[b.kind].toDouble(1);
        const double noise = std::max(spread(a), spread(b));
        const double change = std::abs(std::log(ratio > 0 ? ratio : 1));
        QString verdict = "same";
        if (std::abs(targetMs - baseMs) >= floor && change > std::log(1 + band))
            verdict = ratio > 1 ? "slower" : "faster";
        // A noisy metric keeps only changes far larger than its own spread.
        if (verdict != "same" && noise > band && change < 3 * std::log(1 + noise))
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
                         { "processes",
                           QJsonArray{ a.processMedians.size(), b.processMedians.size() } },
                         { "touched_by", QJsonArray::fromStringList(touched) } };
        rows.append(row);
        if (verdict == "slower" || verdict == "faster") {
            const bool unexpected = touched.isEmpty();
            flags.append(QString("%1 %2 %3× (%4 → %5 %6)%7")
                                 .arg(it.key(), verdict, QString::number(ratio, 'f', 2),
                                      number(baseMs), number(targetMs), b.unit,
                                      unexpected ? ", unexpected: no changed file touches it"
                                                 : QString()));
        }
        lines << QString("| `%1`%8 | %2 | %3 | %4 | %5 | %6 | %7 |")
                         .arg(it.key(), number(baseMs) + " " + b.unit,
                              number(targetMs) + " " + b.unit, QString::number(ratio, 'f', 2),
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
    report["failed_parts"] = QJsonArray::fromStringList(failed);
    report["tests"] = compareTests(base, target, lines);
    report["screenshots"] = compareScreens(base, target, config, output, lines);
    report["perf"] =
            compareMetrics(base, target, false, config, calibration, changes, lines, flags);
    report["device"] =
            compareMetrics(base, target, true, config, calibration, changes, lines, flags);
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
    lines.insert(5,
                 !failed.isEmpty() ? "Evaluation contains failed or missing results.\n"
                         : !timed  ? "Timings were not compared: a run has no timing results.\n"
                         : summary.isEmpty() ? "No timing changes beyond the noise bands.\n"
                                             : "Timing changes:\n\n" + summary.join('\n') + "\n");
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
