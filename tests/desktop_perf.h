#pragma once
// Performance scenarios for `kobalt-desktop-tests --perf OUTPUT`. Included by
// desktop.cpp after the shared fixture. Each scenario writes raw samples;
// statistics belong to the comparison step, so every run stays comparable.
//
// Micro-benchmarks call the text layout API directly. Every other scenario
// comes from platforms/eval/scenarios.json and runs through the reader's own
// scenario player, as it does on a device. Timings come from the reader's own
// diagnostics events.

#include "platform/api/dictionary_service.h"
#include "scenario/player.h"
#include "support/diagnostics.h"
#include <QElapsedTimer>
#include <QWindow>
#include <deque>
#include <functional>

namespace perf {
struct Scenario
{
    QString id, kind;
    QStringList paths;
    QString unit = "ms";
    QJsonObject fixture;
    QVector<double> samples;
    QMap<QString, QVector<double>> stages;
};

QJsonArray numbers(const QVector<double> &values)
{
    QJsonArray result;
    for (const double value : values)
        result.append(value);
    return result;
}

// A Wiktionary-shaped entry. The salt makes each text new to the layout cache,
// as a word looked up for the first time is.
QString definition(int senses, int salt)
{
    static const QStringList words = { "to",         "put",     "place",     "something",
                                       "down",       "in",      "a",         "specified",
                                       "position",   "arrange", "fix",       "establish",
                                       "determine",  "the",     "state",     "of",
                                       "being",      "ready",   "condition", "group",
                                       "collection", "things",  "belong",    "together" };
    QString html = "<p><b>set</b> <i>verb</i></p><ol>";
    for (int s = 0; s < senses; ++s) {
        QString sense;
        for (int w = 0; w < 18 + (s * 7 + salt) % 20; ++w)
            sense += words[(s * 13 + w * 5 + salt) % words.size()] + ' ';
        html += "<li>" + sense + QString::number(salt) + "<br><i>“" + sense.left(60) + "”</i></li>";
    }
    return html + "</ol>";
}

// Stands in for an installed dictionary. "set" has a long entry, as it has in
// a real English dictionary, and other words have short ones.
class Dictionary final : public kobalt::DictionaryService
{
public:
    QVector<kobalt::DictionaryChoice> choices() const override
    {
        return { { "perf", "Performance English", "en", false } };
    }

    bool select(const QString &, const QString &, QString *) override { return true; }

    kobalt::DictionaryResult lookup(const QString &, const QString &word) override
    {
        kobalt::DictionaryResult result;
        result.word = word;
        result.html = definition(word.compare("set", Qt::CaseInsensitive) == 0 ? 120 : 4, ++salt);
        result.dictionary = "Performance English";
        result.dictionaryId = "perf";
        result.language = "en";
        result.received = result.restored = true;
        return result;
    }

private:
    int salt = 0;
};

class Runner
{
public:
    Runner(QString filter, int repetitions)
        : filter(std::move(filter)), repetitions(repetitions) { }

    // A deque keeps references from add() valid while later scenarios are added.
    std::deque<Scenario> scenarios;
    bool failed = false;

    void run()
    {
        text();
        player();
    }

    // A visible run at real speed: the player scenarios only, each named as it starts.
    bool watch = false;

    void runPlayer() { player(); }

private:
    QString filter;
    int repetitions;

    bool wanted(const QString &prefix) const
    {
        return filter.isEmpty() || prefix.startsWith(filter) || filter.startsWith(prefix);
    }

    Scenario &add(const QString &id, const QString &kind, const QStringList &paths,
                  QJsonObject fixture = {})
    {
        scenarios.push_back({ id, kind, paths, "ms", std::move(fixture), {}, {} });
        return scenarios.back();
    }

    bool require(bool ok, const QString &message)
    {
        if (!ok) {
            qCritical().noquote() << "Performance scenario failed:" << message;
            failed = true;
        }
        return ok;
    }

    template <class Action>
    void micro(Scenario &scenario, int count, Action action)
    {
        for (int i = 0; i < count; ++i) {
            QElapsedTimer timer;
            timer.start();
            action(i);
            scenario.samples.append(timer.nsecsElapsed() / 1e6);
        }
    }

    // Text layout and drawing with new text in every sample.
    void text()
    {
        if (!wanted("text."))
            return;
        using namespace kobalt::ui;
        const QStringList paths{ "src/ui/style/", "src/ui/controls/" };
        const int width = 992, height = 560, count = qMax(20, repetitions * 6);
        const auto body = DictionaryPanel::definitionFont({}, 100);
        auto layout = [&](const QString &id, const QFont &font,
                          std::function<void(PrivateRichText &, int)> fill, QJsonObject fixture) {
            auto &scenario = add(id, "micro", paths, fixture);
            micro(scenario, count, [&](int i) {
                PrivateRichText document;
                document.setFont(font);
                fill(document, i);
                document.layout(width, height);
            });
        };
        layout("text.layout.dictionary-entry", body,
               [](PrivateRichText &document, int i) { document.setHtml(definition(14, 1000 + i)); },
               { { "chars", definition(14, 0).size() } });
        layout("text.layout.footnote", interfaceFont(36),
               [](PrivateRichText &document, int i) {
                   document.setHtml("<p>" + definition(2, 2000 + i).left(600) + "</p>");
               },
               { { "chars", 600 } });
        QFile notice(":/kobalt/notices/AGPL-3");
        if (require(notice.open(QIODevice::ReadOnly), "the AGPL notice is not bundled")) {
            const auto licence = QString::fromUtf8(notice.readAll());
            layout("text.layout.licence", chromeFont(24),
                   [&](PrivateRichText &document, int i) {
                       // A changed first word makes the notice new to the layout cache.
                       document.setPlainText(QString::number(3000 + i) + licence);
                   },
                   { { "chars", licence.size() } });
        }
        auto &draw =
                add("text.draw.page", "micro", paths, { { "chars", definition(14, 0).size() } });
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        for (int i = 0; i < count; ++i) {
            PrivateRichText document;
            document.setFont(body);
            document.setHtml(definition(14, 4000 + i));
            document.layout(width, height);
            image.fill(Qt::white);
            QPainter painter(&image);
            QElapsedTimer timer;
            timer.start();
            document.draw(painter, QRectF(0, 0, width, height), QPalette());
            draw.samples.append(timer.nsecsElapsed() / 1e6);
        }
        auto &label = add("text.label.height", "micro", paths, { { "chars", 220 } });
        micro(label, count, [&](int i) {
            wrappedInterfaceTextHeight(definition(1, 5000 + i).left(220), interfaceFont(32), 900);
        });
        auto &elide = add("text.elide", "micro", paths, { { "chars", 120 } });
        micro(elide, count, [&](int i) {
            elidePrivateText(definition(1, 6000 + i).left(120), interfaceFont(32), 600);
        });
    }

    // Runs the shared scenario file through the reader's own scenario player.
    void player()
    {
        QFile source(QStringLiteral(KOBALT_SCENARIOS));
        const auto file = source.open(QIODevice::ReadOnly)
                ? QJsonDocument::fromJson(source.readAll()).object()
                : QJsonObject();
        if (!require(!file.isEmpty(), "the scenario file could not be read"))
            return;
        Fixture fixture(false, false, false, true, false, false, [](ReaderHostServices &services) {
            services.downloadableDictionaries = false;
            services.dictionary = [] {
                return std::unique_ptr<kobalt::DictionaryService>(new Dictionary);
            };
        });
        const auto books = file.value("books").toObject();
        kobalt::scenario::Host host;
        host.reader = [&]() -> QWidget * { return fixture.window.readerWindow(); };
        host.open = [&](const QString &key) {
            const auto path = books.value(key).toObject().value("file").toString();
            if (path.isEmpty())
                return false;
            fixture.window.openBook(QStringLiteral(KOBALT_SOURCE_ROOT) + '/' + path);
            return true;
        };
        host.busy = [&] {
            auto *reader = fixture.window.readerWindow();
            return reader && reader->busy();
        };
        host.idle = [&] { return fixture.window.idle(); };
        // Parsed books and layouts are disposable caches.
        host.forget = [&](const QString &) {
            QDir(fixture.paths.documentCache()).removeRecursively();
            QFile::remove(fixture.paths.layoutCache());
        };
        // The reader is drawn into the window's canvas scene, so input goes to
        // the canvas viewport's window at the scene position.
        host.locate = [&](QWidget *widget, const QPoint &point) -> QPair<QWindow *, QPointF> {
            auto *reader = fixture.window.readerWindow();
            auto *canvas = fixture.window.findChild<QGraphicsView *>();
            if (!reader || !canvas)
                return {};
            auto *viewport = canvas->viewport();
            const auto at = canvas->mapFromScene(widget->mapTo(reader, point));
            return { viewport->window()->windowHandle(),
                     QPointF(viewport->mapTo(viewport->window(), at)) };
        };
        kobalt::scenario::Options options;
        options.filter = filter;
        options.repeatScale = repetitions / 5.0;
        if (watch)
            options.started = [](const QString &id) {
                std::printf("%s\n", qPrintable(id));
                std::fflush(stdout);
            };
        for (const auto &result : kobalt::scenario::run(file, host, options)) {
            const auto id = result.value("id").toString();
            if (result.value("status").toString() == "skipped") {
                qInfo().noquote() << "Skipped" << id + ":" << result.value("reason").toString();
                continue;
            }
            if (!require(result.value("status").toString() == "completed",
                         id + ": " + result.value("reason").toString()))
                continue;
            QStringList paths;
            for (const auto &path : result.value("paths").toArray())
                paths.append(path.toString());
            auto &scenario = add(id, result.value("kind").toString(), paths,
                                 result.value("fixture").toObject());
            scenario.unit = result.value("unit").toString();
            for (const auto &sample : result.value("samples").toArray())
                scenario.samples.append(sample.toDouble());
            const auto stages = result.value("stages").toObject();
            for (auto it = stages.begin(); it != stages.end(); ++it)
                for (const auto &sample : it.value().toArray())
                    scenario.stages[it.key()].append(sample.toDouble());
        }
    }
};

// Runs the player scenarios in a visible window and prints what ran. The
// timings are not saved.
int watch()
{
    QTemporaryDir logs(QDir::tempPath() + "/kobalt-watch-XXXXXX");
    if (!logs.isValid())
        return 2;
    kobalt::initializeDiagnostics(logs.path());
    Runner runner(qEnvironmentVariable("KOBALT_PERF_FILTER"), 5);
    runner.watch = true;
    runner.runPlayer();
    for (const auto &scenario : runner.scenarios)
        std::printf("  %-24s %d samples\n", qPrintable(scenario.id), int(scenario.samples.size()));
    return runner.failed ? 2 : 0;
}

// Writes OUTPUT only when every selected scenario completed.
int run(const QString &output)
{
    QTemporaryDir logs(QDir::tempPath() + "/kobalt-perf-XXXXXX");
    if (!logs.isValid())
        return 2;
    kobalt::initializeDiagnostics(logs.path());
    const auto filter = qEnvironmentVariable("KOBALT_PERF_FILTER");
    bool valid = true;
    const auto setting = qEnvironmentVariable("KOBALT_PERF_REPETITIONS");
    const int repetitions = setting.isEmpty() ? 5 : setting.toInt(&valid);
    if (!valid || repetitions < 1 || repetitions > 100) {
        qCritical() << "KOBALT_PERF_REPETITIONS must be between 1 and 100";
        return 2;
    }
    QElapsedTimer timer;
    timer.start();
    Runner runner(filter, repetitions);
    runner.run();
    // An audit can keep the reader's own events next to the samples.
    const auto events = qEnvironmentVariable("KOBALT_PERF_EVENTS");
    if (!events.isEmpty()) {
        QFile::remove(events);
        kobalt::flushDiagnostics();
        if (!QFile::copy(logs.path() + "/events.jsonl", events))
            qCritical().noquote() << "Could not keep the events in" << events;
    }
    if (runner.failed || runner.scenarios.empty())
        return 2;
    QJsonArray scenarios;
    for (const auto &scenario : runner.scenarios) {
        if (scenario.samples.isEmpty())
            continue;
        QJsonObject stages;
        for (auto it = scenario.stages.begin(); it != scenario.stages.end(); ++it)
            stages[it.key()] = numbers(it.value());
        scenarios.append(QJsonObject{ { "id", scenario.id },
                                      { "kind", scenario.kind },
                                      { "unit", scenario.unit },
                                      { "paths", QJsonArray::fromStringList(scenario.paths) },
                                      { "fixture", scenario.fixture },
                                      { "samples_ms", numbers(scenario.samples) },
                                      { "stages", stages } });
    }
    const QJsonObject result{ { "schema", 1 },
                              { "kind", "kobalt-perf" },
                              { "commit", kobaltCommit },
                              { "modified", bool(KOBALT_COMMIT_MODIFIED) },
                              { "version", KOBALT_VERSION },
                              { "qt", qVersion() },
                              { "build_type", KOBALT_BUILD_TYPE },
                              { "repetitions", repetitions },
                              { "filter", filter },
                              { "duration_s", timer.elapsed() / 1000.0 },
                              { "scenarios", scenarios } };
    if (!QDir().mkpath(QFileInfo(output).absolutePath()))
        return 2;
    QSaveFile file(output);
    const auto json = QJsonDocument(result).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
        return 2;
    qInfo().noquote() << "Saved" << scenarios.size() << "performance scenarios to" << output << "in"
                      << timer.elapsed() / 1000.0 << "seconds";
    return 0;
}
} // namespace perf
