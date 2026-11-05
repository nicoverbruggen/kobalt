#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLockFile>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <csignal>
#include <memory>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;

void requestStop(int)
{
    interrupted = 1;
}

const QString root = QStringLiteral(KOBALT_ROOT);
#ifdef Q_OS_MACOS
const QString host = QStringLiteral("mac");
#else
const QString host = QStringLiteral("linux");
#endif
struct Step
{
    QString program;
    QStringList arguments;
    QString directory;
};

enum Action { Desktop, Kobo5, Kobo6, Headless, Visible };

QString output(const QString &platform)
{
    return root + "/build/desktop-" + platform;
}

QVector<Step> plan(Action action, const QString &platform)
{
    const auto desktop = root + "/platforms/desktop";
    if (action == Kobo5 || action == Kobo6)
        return {
            { "bash", { root + "/platforms/kobo/build.sh", action == Kobo5 ? "qt5" : "qt6" }, root }
        };
    QStringList configure{ "--preset", platform };
    if (!QFile::exists(output(platform) + "/CMakeCache.txt"))
        configure << QStringLiteral("-DQt6_DIR=" KOBALT_QT_DIR);
    if (action != Desktop)
        configure << "-DBUILD_TESTING=ON";
    QVector<Step> steps{ { "cmake", configure, desktop } };
    if (action == Desktop) {
        steps.append({ "cmake", { "--build", "--preset", platform }, desktop });
    } else if (action == Headless) {
        // Builds the tests, runs the full suite and records a pass for the validation guard.
        steps.append(
                { "cmake",
                  { "-DPRESET=" + platform, "-P", root + "/platforms/validation-record.cmake" },
                  desktop });
    } else {
        steps.append({ "cmake", { "--build", "--preset", platform + "-light-tests" }, desktop });
        steps.append(
                { "cmake",
                  { "-DOUTPUT=" + root + "/build/screenshot-books",
                    "-DREFERENCES=" + root + "/tests/fixtures/screenshot-books.json",
                    "-DNAMES=war-and-peace", "-P", desktop + "/prepare-screenshot-books.cmake" },
                  root });
        steps.append(
                { "cmake",
                  { "-DTEST_EXECUTABLE=" + output(platform) + "/kobalt-desktop-tests",
                    "-DTEST_ARGUMENTS=--watch", "-DTEST_TMP_ROOT=" + output(platform) + "/test-tmp",
                    "-DTEST_TIMEOUT=1800", "-P", desktop + "/run-test.cmake" },
                  desktop });
    }
    return steps;
}

class Launcher : public QWidget
{
public:
    bool busy = false, succeeded = false, wasCancelled = false;
    QString lastScratch;

    explicit Launcher() : lock(root + "/build/launcher-actions.lock")
    {
        lock.setStaleLockTime(0);
        setWindowTitle("Kobalt Developer Launcher");
        resize(850, 720);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(12);
        auto *title = new QLabel("Kobalt • Developer Launcher");
        auto font = title->font();
        font.setPointSize(font.pointSize() + 7);
        font.setBold(true);
        title->setFont(font);
        layout->addWidget(title);
        auto *location = new QLabel((host == "mac" ? QString("macOS") : QString("Linux")) + " · Qt "
                                    + qVersion() + "\n" + root);
        location->setTextInteractionFlags(Qt::TextSelectableByMouse);
        location->setWordWrap(true);
        layout->addWidget(location);
        const QStringList labels{ "Build and launch desktop", "Build Kobo · Qt 5",
                                  "Build Kobo · Qt 6", "Build and run tests · headless",
                                  "Build and run tests · desktop app" };
        const QStringList hints{
            "Incremental desktop build, then open Kobalt.",
            "Build and verify the package using the prepared Qt 5 SDK image.",
            "Build and verify the package using the prepared Qt 6 SDK image.",
            "Run the full native assertion suite with isolated profiles.",
            "Watch autonomous UI scenarios in a real window. Full assertions run headlessly."
        };
        for (int i = 0; i < labels.size(); ++i) {
            auto *row = new QHBoxLayout;
            auto *button = new QPushButton(labels[i]);
            button->setMinimumWidth(285);
            button->setMinimumHeight(38);
            button->setObjectName(QString("action%1").arg(i));
            auto *hint = new QLabel(hints[i]);
            hint->setWordWrap(true);
            row->addWidget(button);
            row->addWidget(hint, 1);
            layout->addLayout(row);
            buttons.append(button);
            connect(button, &QPushButton::clicked, this, [this, i] {
                launchDesktop = i == Desktop;
                start(plan(Action(i), host));
            });
        }
        auto *controls = new QHBoxLayout;
        status = new QLabel(
                "Ready. Kobo builds need a running container engine and prepared images.");
        status->setWordWrap(true);
        stop = new QPushButton("Stop");
        stop->setEnabled(false);
        controls->addWidget(status, 1);
        controls->addWidget(stop);
        layout->addLayout(controls);
        progress = new QProgressBar;
        progress->setRange(0, 1);
        progress->setValue(0);
        progress->setTextVisible(false);
        layout->addWidget(progress);
        log = new QPlainTextEdit;
        log->setReadOnly(true);
        log->setMaximumBlockCount(20000);
        log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        layout->addWidget(log, 1);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
        connect(stop, &QPushButton::clicked, this, &Launcher::cancel);
        connect(&process, &QProcess::readyReadStandardOutput, this,
                [this] { append(QString::fromUtf8(process.readAll())); });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false, process.errorString());
        });
        connect(&process, &QProcess::started, this, [this] {
            group = process.processId();
            if (wasCancelled)
                signalGroup(SIGTERM);
        });
        connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
            append(QString::fromUtf8(process.readAll()));
            if (wasCancelled) {
                signalGroup(SIGKILL);
                finish(false, "Stopped. Temporary profiles removed.");
            } else if (code != 0 || exit != QProcess::NormalExit)
                finish(false, QString("Failed (exit %1). See output below.").arg(code));
            else {
                ++index;
                next();
            }
        });
    }

    ~Launcher() override
    {
        if (busy) {
            signalGroup(SIGKILL);
            process.kill();
            process.waitForFinished(5000);
        }
    }

    void start(QVector<Step> commands)
    {
        if (busy)
            return;
        launchDesktop = launchDesktop && commands.size() == 2;
        if (!lock.tryLock()) {
            status->setText("Another launcher is already running a job in this worktree.");
            return;
        }
        const auto base = root + "/build/launcher-" + host;
        QDir().mkpath(base + "/runs");
        scratch = std::make_unique<QTemporaryDir>(base + "/runs/job-XXXXXX");
        if (!scratch->isValid()) {
            lock.unlock();
            status->setText("Cannot create an isolated run directory.");
            return;
        }
        lastScratch = scratch->path();
        logFile.setFileName(base + "/latest.log");
        if (!logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            scratch.reset();
            lock.unlock();
            status->setText("Cannot open the job log.");
            return;
        }
        ++generation;
        steps = std::move(commands);
        index = 0;
        busy = true;
        succeeded = false;
        wasCancelled = false;
        log->clear();
        progress->setRange(0, 0);
        stop->setEnabled(true);
        for (auto *button : buttons)
            button->setEnabled(false);
        next();
    }

    void cancel()
    {
        if (!busy || wasCancelled)
            return;
        wasCancelled = true;
        stop->setEnabled(false);
        status->setText("Stopping the job and cleaning up…");
        signalGroup(SIGTERM);
        const auto stoppedGeneration = generation;
        QTimer::singleShot(4000, this, [this, stoppedGeneration] {
            if (busy && wasCancelled && generation == stoppedGeneration) {
                signalGroup(SIGKILL);
                process.kill();
            }
        });
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (busy) {
            closing = true;
            cancel();
            event->ignore();
        } else
            event->accept();
    }

private:
    QProcess process;
    QLockFile lock;
    QFile logFile;
    std::unique_ptr<QTemporaryDir> scratch;
    QVector<Step> steps;
    QVector<QPushButton *> buttons;
    QPlainTextEdit *log;
    QLabel *status;
    QPushButton *stop;
    QProgressBar *progress;
    int index = 0, generation = 0;
    qint64 group = 0;
    bool closing = false, launchDesktop = false;

    void signalGroup(int signal)
    {
        if (group > 0)
            ::kill(-pid_t(group), signal);
    }

    void append(const QString &text)
    {
        if (text.isEmpty())
            return;
        log->moveCursor(QTextCursor::End);
        log->insertPlainText(text);
        log->moveCursor(QTextCursor::End);
        if (logFile.isOpen()) {
            logFile.write(text.toUtf8());
            logFile.flush();
        }
    }

    void next()
    {
        group = 0;
        if (index == steps.size()) {
            if (launchDesktop) {
                const auto executable = host == "mac"
                        ? output(host) + "/Kobalt.app/Contents/MacOS/Kobalt"
                        : output(host) + "/Kobalt";
                bool opened;
                if (host == "mac")
                    opened = QProcess::startDetached("/usr/bin/open",
                                                     { output(host) + "/Kobalt.app" });
                else
                    opened = QProcess::startDetached(executable, {}, output(host));
                if (!opened) {
                    finish(false, "Build succeeded, but the desktop app could not be launched.");
                    return;
                }
            }
            finish(true,
                   launchDesktop ? "Build complete. Desktop app launched."
                                 : "Completed successfully.");
            return;
        }
        const auto &step = steps[index];
        status->setText(
                QString("Step %1 of %2 · %3").arg(index + 1).arg(steps.size()).arg(step.program));
        append("\n→ " + step.program + ' ' + step.arguments.join(' ') + "\n");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("KOBALT_TEST_TMP_ROOT", scratch->path());
        environment.insert("TMPDIR", scratch->path());
        environment.insert("QT_FORCE_STDERR_LOGGING", "1");
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(step.directory);
        process.start(step.program, step.arguments);
    }

    void finish(bool success, const QString &message)
    {
        busy = false;
        succeeded = success;
        group = 0;
        launchDesktop = false;
        scratch.reset();
        lock.unlock();
        status->setText(message);
        append("\n" + message + "\n");
        logFile.close();
        progress->setRange(0, 1);
        progress->setValue(success ? 1 : 0);
        stop->setEnabled(false);
        for (auto *button : buttons)
            button->setEnabled(true);
        if (closing)
            QTimer::singleShot(0, this, &QWidget::close);
    }
};

int selfTest(Launcher &launcher)
{
    const auto capture = qEnvironmentVariable("KOBALT_LAUNCHER_CAPTURE");
    if (!capture.isEmpty()) {
        launcher.show();
        QApplication::processEvents();
        if (!launcher.grab().save(capture))
            return 1;
        launcher.hide();
    }
    for (const auto &platform : { QString("mac"), QString("linux") }) {
        if (plan(Desktop, platform).last().arguments.last() != platform
            || plan(Headless, platform).last().arguments
                    != QStringList{ "-DPRESET=" + platform, "-P",
                                    root + "/platforms/validation-record.cmake" }
            || !plan(Visible, platform).last().arguments.contains("-DTEST_ARGUMENTS=--watch")
            || plan(Kobo5, platform).last().arguments.last() != "qt5"
            || plan(Kobo6, platform).last().arguments.last() != "qt6")
            return 1;
    }
    auto wait = [&] {
        QEventLoop loop;
        QTimer poll, deadline;
        poll.setInterval(10);
        deadline.setSingleShot(true);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (!launcher.busy)
                loop.quit();
        });
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        poll.start();
        deadline.start(10000);
        loop.exec();
        return !launcher.busy && !QDir(launcher.lastScratch).exists();
    };
    launcher.start({ { "cmake", { "-E", "echo", "launcher check" }, root } });
    if (!wait() || !launcher.succeeded)
        return 1;
    launcher.start(
            { { "cmake", { "-E", "false" }, root }, { "cmake", { "-E", "sleep", "30" }, root } });
    if (!wait() || launcher.succeeded)
        return 1;
    launcher.start({ { root + "/build/missing-launcher-test-command", {}, root } });
    if (!wait() || launcher.succeeded)
        return 1;
    launcher.start({ { "cmake", { "-E", "sleep", "30" }, root } });
    QTimer::singleShot(100, &launcher, [&] { launcher.cancel(); });
    if (!wait() || !launcher.wasCancelled)
        return 1;
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && qstrcmp(argv[1], "--self-test") == 0)
        qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    QApplication app(argc, argv);
    app.setApplicationName("Kobalt Developer Launcher");
    app.setStyle("Fusion");
    Launcher launcher;
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer interruptPoll;
    interruptPoll.setInterval(100);
    QObject::connect(&interruptPoll, &QTimer::timeout, &launcher, [&] {
        if (interrupted) {
            interrupted = 0;
            launcher.close();
        }
    });
    interruptPoll.start();
    if (app.arguments().contains("--self-test"))
        return selfTest(launcher);
    launcher.show();
    return app.exec();
}
