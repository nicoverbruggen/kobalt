#include "window.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/scale.h"
#include "interfaces/reader/views/reading_page.h"
#include "reading/reading_density.h"
#include "reading/header_footer.h"
#include "support/diagnostics.h"
#ifdef KOBALT_KINDLE_NATIVE_TESTING
#include "captures.h"
#include "input_checks.h"
#include "reading/engine_worker.h"
#include <QSemaphore>
#endif
#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace {
QByteArray pageImageHash(const QImage &image) {
    if (image.isNull())
        return {};
    // Normalize the pixel format and omit scanline padding so the comparison
    // depends on rendered pixels rather than allocation details.
    const auto pixels = image.convertToFormat(QImage::Format_RGBA8888);
    if (pixels.isNull())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(pixels.width()) + 'x' + QByteArray::number(pixels.height()) + ':' +
                 QByteArray::number(int(pixels.format())) + ':');
    for (int row = 0; row < pixels.height(); ++row)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(pixels.constScanLine(row)), pixels.width() * 4));
    return hash.result();
}
volatile std::sig_atomic_t terminationFd = -1;
void requestTermination(int) {
    const int savedErrno = errno;
    const char request = 1;
    if (terminationFd >= 0) {
        const auto ignored = ::write(terminationFd, &request, 1);
        (void)ignored;
    }
    errno = savedErrno;
}
// The launcher forwards TERM. Queue it onto Qt rather than performing reader
// shutdown from a signal handler, so engine work and checkpoints can drain.
class TerminationHandler {
public:
    bool install(QWidget &window) {
        if (::pipe(descriptors) != 0)
            return false;
        for (int fd : descriptors) {
            if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) != 0)
                return false;
        }
        notifier = std::make_unique<QSocketNotifier>(descriptors[0], QSocketNotifier::Read);
        QObject::connect(notifier.get(), &QSocketNotifier::activated, &window, [this, &window] {
            char buffer[64];
            while (::read(descriptors[0], buffer, sizeof(buffer)) > 0) {}
            window.close();
        });
        struct sigaction action {};
        action.sa_handler = requestTermination;
        sigemptyset(&action.sa_mask);
        terminationFd = descriptors[1];
        if (::sigaction(SIGTERM, &action, &previousTerm) != 0)
            return false;
        termInstalled = true;
        if (::sigaction(SIGINT, &action, &previousInt) != 0)
            return false;
        intInstalled = true;
        return true;
    }
    ~TerminationHandler() {
        if (termInstalled)
            ::sigaction(SIGTERM, &previousTerm, nullptr);
        if (intInstalled)
            ::sigaction(SIGINT, &previousInt, nullptr);
        terminationFd = -1;
        notifier.reset();
        for (int fd : descriptors)
            if (fd >= 0)
                ::close(fd);
    }
private:
    int descriptors[2]{-1, -1};
    struct sigaction previousTerm {}, previousInt {};
    bool termInstalled = false, intInstalled = false;
    std::unique_ptr<QSocketNotifier> notifier;
};
}

int main(int argc, char **argv) {
    using namespace kobalt;
    FontCopies copies;
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Kobalt"));
    app.setOrganizationName(QStringLiteral("Kobalt"));
    app.setApplicationVersion(QStringLiteral(KOBALT_VERSION));
    app.setStyle(QStringLiteral("Fusion"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Kobalt standalone Kindle reader prototype"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"profile", "Directory for settings, library and reading progress.", "directory"});
    parser.addOption({"library", "Folder containing EPUB books.", "directory", "/mnt/us/documents"});
    parser.addOption({"assets", "Runtime asset bundle.", "directory",
                      QDir(QCoreApplication::applicationDirPath()).filePath("assets")});
    parser.addOption({"book", "Open this EPUB after starting.", "file"});
    parser.addOption({"smoke-test", "Open --book, turn a page, save, reopen and exit. Requires --profile."});
#ifdef KOBALT_KINDLE_NATIVE_TESTING
    parser.addOption({"preview", "Show a scaled, interactive PW5 preview in a Mac window."});
    parser.addOption({"screenshots", "Capture PW5 screens headlessly. Requires --profile and --book.", "directory"});
#endif
    parser.process(app);
    const bool smoke = parser.isSet("smoke-test");
#ifdef KOBALT_KINDLE_NATIVE_TESTING
    const bool captures = parser.isSet("screenshots"), preview = parser.isSet("preview");
    if (captures && (!parser.isSet("profile") || !parser.isSet("book") || smoke || preview)) {
        qCritical() << "Use --screenshots with --profile and --book, separately from --preview or --smoke-test";
        return 2;
    }
    if (captures && !QDir().mkpath(parser.value("screenshots"))) return 2;
#else
    constexpr bool captures = false, preview = false;
#endif
    if (smoke && (!parser.isSet("profile") || !parser.isSet("book"))) {
        qCritical() << "--smoke-test requires an isolated --profile and --book";
        return 2;
    }
    const auto home = qEnvironmentVariable("KOBALT_KINDLE_HOME", QCoreApplication::applicationDirPath());
    const auto profile = parser.isSet("profile") ? QDir(parser.value("profile")).absolutePath()
                                                : QDir(home).filePath("data");
    if (!QDir().mkpath(profile)) {
        qCritical() << "Cannot create profile" << profile;
        return 2;
    }
    AssetBundle assets(parser.value("assets"), QStringLiteral(KOBALT_VERSION));
    if (!assets.available() || !ui::initializeInterfaceFonts(assets)) {
        qCritical() << "Kobalt bundled assets are unavailable" << assets.root();
        return 2;
    }
    for (const auto &file : bundledFontFiles(assets)) {
        if (QFontDatabase::addApplicationFont(file) < 0) {
            qCritical() << "Cannot register font" << file;
            return 2;
        }
    }
    readingPpi.store(300);
    ui::setDensity(300);
    app.setFont(ui::interfaceFont());
    initializeDiagnostics(profile + "/diagnostics");
    const storage::Paths paths{profile + "/config"};
    if (!QDir().mkpath(paths.directory)) {
        qCritical() << "Cannot create settings directory" << paths.directory;
        return 2;
    }
    if (QDir(paths.directory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        // Seed only a new profile. Existing reader settings and legacy book
        // preferences keep their current behavior.
        auto layout = headerfooter::presetLayout(headerfooter::Preset::Default);
        layout.slotFields[2] = headerfooter::Field::Battery;
        const QJsonObject defaults{{headerfooter::presetKey, headerfooter::presetValue(headerfooter::Preset::Custom)},
                                  {headerfooter::customKey, headerfooter::toJson(layout)},
                                  {sleep::preferenceKey, sleep::value(sleep::Mode::Cover)}};
        if (!storage::writePreferences(paths.preferences(), defaults)) {
            qCritical() << "Cannot save initial Kindle settings";
            return 2;
        }
    }
    kindle::Window window(paths,
                          engineFontInputs(assets, {}, profile + "/fonts"),
                          placeholderFontFamily(assets, copies), parser.value("library"));
    TerminationHandler termination;
    if (!termination.install(window)) {
        qCritical() << "Cannot install graceful termination handler";
        return 2;
    }
    window.resize(1236, 1648);
    if (preview) {
        window.setPreview(true);
        window.setWindowTitle(QStringLiteral("Kobalt Kindle Preview"));
        const auto bounds = app.primaryScreen()->availableGeometry().size() * .85;
        const auto scale = qMin(1.0, qMin(bounds.width() / 618.0, bounds.height() / 824.0));
        window.resize(qRound(618 * scale), qRound(824 * scale));
        window.show();
    } else if (smoke || captures) {
        // The minimal plugin reports a small dummy screen. Keep the PW5
        // viewport so the smoke check exercises the device's book layout.
        window.show();
    } else {
        if (app.primaryScreen())
            window.resize(app.primaryScreen()->geometry().size());
        window.showFullScreen();
    }
#ifndef KOBALT_KINDLE_NATIVE_TESTING
    if (QGuiApplication::platformName() == QStringLiteral("kindle"))
        window.startDeviceLifecycle();
#endif
    const auto book = QFileInfo(parser.value("book")).absoluteFilePath();
#ifdef KOBALT_KINDLE_NATIVE_TESTING
    if (captures)
        kindle::captureScreens(window, book, QFileInfo(parser.value("screenshots")).absoluteFilePath());
#endif
    if (smoke) {
        // Exercise the real standalone controller and its saved checkpoint.
        // The outer timeout closes through the same draining path as Exit.
        struct SmokeState {
            int stage = 0;
            QString position, initialPosition;
            QByteArray initialImage, turnedImage;
            bool failed = false;
#ifdef KOBALT_KINDLE_NATIVE_TESTING
            bool retirementChecked = false;
#endif
        };
        auto state = std::make_shared<SmokeState>();
        auto fail = [&window, state](const QString &message) {
            if (state->failed)
                return;
            state->failed = true;
            qCritical().noquote() << "Kindle smoke failed:" << message;
            if (window.idle())
                QCoreApplication::exit(1);
            else
                window.closeBook();
        };
        QObject::connect(&window, &kindle::Window::bookFailed, &window, fail);
        auto *poll = new QTimer(&window);
        poll->setInterval(200);
        QObject::connect(&window, &kindle::Window::bookOpened, &window, [&window, state, poll] {
            window.readerWindow()->checkpoint();
            state->stage = state->stage == 2 ? 3 : 1;
            poll->start();
        });
        QObject::connect(poll, &QTimer::timeout, &window, [&window, state, poll, book, fail, preview] {
            if (state->failed || !window.readerWindow())
                return;
            auto *reader = window.readerWindow();
            // Presentation readiness includes completed layout, viewport size
            // and image-resource loading. Do not compare an intermediate frame.
            if (!reader->readyForPresentation())
                return;
            if (preview && reader->size() != QSize(1236, 1648)) {
                fail(QStringLiteral("Scaled preview changed the PW5 reader layout dimensions"));
                return;
            }
            auto *page = static_cast<ReadingPage *>(reader->findChild<QWidget *>("rrPage"));
            const auto imageHash = page ? pageImageHash(page->image) : QByteArray();
            if (imageHash.isEmpty())
                return;
            reader->checkpoint();
            const auto saved = window.savedState(book);
            const auto position = saved.value("position").toString();
            if (position.isEmpty())
                return;
            if (state->stage == 1) {
                if (state->initialImage.isEmpty()) {
                    state->initialPosition = position;
                    state->initialImage = imageHash;
                } else if (position != state->initialPosition && imageHash != state->initialImage) {
                    state->position = position;
                    state->turnedImage = imageHash;
                    state->stage = 2;
                    poll->stop();
                    window.closeBook();
                }
                // Neighbour preparation can still be busy when opened fires.
                // Retry the input until a completed frame advances the page.
                if (state->stage == 1)
                    window.readerWindow()->pageButton(1);
            } else if (state->stage == 3) {
#ifdef KOBALT_KINDLE_NATIVE_TESTING
                if (qEnvironmentVariableIsSet("KOBALT_KINDLE_MIGRATION_CHECK") &&
                    !saved.value("kindle_migration_probe").toObject().value("preserve").toBool()) {
                    fail(QStringLiteral("SQLite migration lost the optional book-state field"));
                    return;
                }
#endif
                if (position != state->position) {
                    fail(QStringLiteral("Reopened book did not restore the saved position"));
                    return;
                }
                if (imageHash != state->turnedImage) {
                    fail(QStringLiteral("Reopened book did not restore the rendered page image"));
                    return;
                }
#ifdef KOBALT_KINDLE_NATIVE_TESTING
                poll->stop();
                if (!kindle::checkReaderInput(window, *page)) {
                    fail(QStringLiteral("Reader pointer input regression"));
                    return;
                }
#endif
#ifdef KOBALT_KINDLE_NATIVE_TESTING
                QObject::connect(reader, &QObject::destroyed, &window, [&window, state, fail] {
                    // Resource cleanup can outlive the reader. Keep that real
                    // queue pending until the GUI checks the retained frame.
                    auto gate = std::make_shared<QSemaphore>();
                    QMetaObject::invokeMethod(engineWorker(), [gate] { gate->tryAcquire(1, 2000); },
                                              Qt::QueuedConnection);
                    QTimer::singleShot(0, &window, [&window, state, fail, gate] {
                        const auto frame = window.grab().toImage();
                        bool ink = false;
                        for (int y = 0; y < frame.height() && !ink; y += 4)
                            for (int x = 0; x < frame.width(); x += 4)
                                if (qGray(frame.pixel(x, y)) < 128) {
                                    ink = true;
                                    break;
                                }
                        state->retirementChecked = true;
                        gate->release();
                        if (!ink)
                            fail(QStringLiteral("Reader deletion exposed a blank host before Library recovery"));
                    });
                });
#endif
                state->stage = 4;
                poll->stop();
                window.closeBook();
            }
        });
        QObject::connect(&window, &kindle::Window::bookClosed, &window, [&window, state, book, fail] {
            if (state->failed) {
                QCoreApplication::exit(1);
            } else if (state->stage == 2) {
                if (!window.libraryContains(book)) {
                    fail(QStringLiteral("Opened book is missing from the library"));
                    return;
                }
                QTimer::singleShot(0, &window, [&window, book] { window.openBook(book); });
            } else if (state->stage == 4) {
#ifdef KOBALT_KINDLE_NATIVE_TESTING
                if (!state->retirementChecked) {
                    fail(QStringLiteral("Reader retirement frame was not checked"));
                    return;
                }
                if (!kindle::checkLibraryInput(window)) {
                    fail(QStringLiteral("Library pointer input regression"));
                    return;
                }
#endif
                qInfo() << "Kindle smoke passed: library, EPUB, changed page pixels, checkpoint, restored page pixels, close";
                window.close();
            }
        });
        QTimer::singleShot(60000, &window, [fail] { fail(QStringLiteral("60 second timeout")); });
    }
    if (parser.isSet("book") && !captures)
        QTimer::singleShot(0, &window, [&window, book] { window.openBook(book); });
    const int result = app.exec();
    if (result)
        return result;
    return window.deviceFailed() ? 1 : 0;
}
