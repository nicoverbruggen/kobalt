#include "app/reader/settings/sleep_editor.h"
#include "library/cover_thumbnails.h"
#include "app/reader/views/annotation_exporter.h"
#include "app/reader/panels/search_panel.h"
#include <QVariant>
#include <QSqlQuery>
#include "storage/annotation_database.h"
#include "storage/record_writer.h"
#include "storage/highlight_store.h"
#include "storage/sleep_snapshot.h"
#include "reading/session_timeout.h"
#include "platforms/kobo/common/last_read_book.h"
#include "platforms/kobo/common/reader_coordinator.h"
#include "platforms/kobo/common/sync_exclusion.h"
#include "platforms/kobo/common/browser_handoff.h"
#include "support/paint_regions.h"
#include <QElapsedTimer>
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "app/reader_interface.h"
#include "app/reader/settings/sleep_gallery.h"
#include "app/reader_window.h"
#include "ui/controls/page_scrubber.h"
#include "ui/controls/rows.h"
#include "ui/controls/panels.h"
#include <QtGui/private/qeventpoint_p.h>
#include "ui/dialogs/modals.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/scale.h"
#include "app/reader/views/reading_page.h"
#include "render/page_content.h"
#include "app/reader/panels/bookmark_list.h"
#include "app/reader/views/appearance_preview.h"
#include "app/reader/settings/settings_panel.h"
#include "app/reader/views/kosync_notice.h"
#include "app/reader/views/sync_choice.h"
#include "app/reader/views/image_viewer.h"
#include "app/reader/views/sleep_screen.h"
#include "platforms/standalone/canvas.h"
#include "platforms/desktop/services.h"
#include "platforms/kobo/common/engine_positions.h"
#include "platform/api/reader_orientation.h"
#include "publication/local_resources.h"
#include "publication/native_resources.h"
#include "reading/appearance_settings.h"
#include "reading/reading_book.h"
#include "reading/reading_density.h"
#include "reading/sleep_screen.h"
#include "reading/reader_session.h"
#include "reading/rendering_mode.h"
#include "reading/style_sample.h"
#include "sync/kosync.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include <QLocale>
#include "engine/engine_activity.h"
#include "engine/engine_worker.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include "app/messages/recovery_message.h"
#include "app/messages/benchmark_message.h"
#include "support/reader_inputs.h"
#include "support/book_fixtures.h"
#include "support/engine_fixtures.h"
#include "support/host_presentation.h"
#include "support/input_clock.h"
#include "support/kosync_reply.h"
#include "support/reading_bridge_fixture.h"
#include "support/percentage_text.h"

#include <QApplication>
#include <QBuffer>
#include <QTcpServer>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QTcpSocket>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QLabel>
#include <QLineEdit>
#include <QIntValidator>
#ifndef KOBALT_NO_SENSORS
#  include <QOrientationSensor>
#endif
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#include <qpa/qwindowsysteminterface.h>
#ifndef KOBALT_NO_SENSORS
#  include <QSensorBackend>
#  include <QSensorManager>
#endif
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSlider>
#include <QStyle>
#include <QTemporaryDir>
#include <QSettings>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTime>
#include <QTimeZone>
#include <QUrl>
#include <QCryptographicHash>
#include <QtEndian>
#include <atomic>
#include <utility>
#include <zlib.h>

#ifndef KOBALT_NO_SENSORS
namespace kobalt::test {
class Backend : public QSensorBackend
{
public:
    QOrientationReading reading;
    quint64 timestamp = 0;

    explicit Backend(QSensor *sensor) : QSensorBackend(sensor) { setReading(&reading); }

    void start() override { }

    void stop() override { }

    void sample(QOrientationReading::Orientation value)
    {
        reading.setOrientation(value);
        reading.setTimestamp(++timestamp);
        newReadingAvailable();
    }
};

class Factory : public QSensorBackendFactory
{
public:
    Backend *backend = nullptr;

    QSensorBackend *createBackend(QSensor *sensor) override
    {
        return backend = new Backend(sensor);
    }
};

struct SensorRegistration
{
    explicit SensorRegistration(const QByteArray &id, Factory *factory) : id(id)
    {
        QSensorManager::registerBackend(QOrientationSensor::sensorType, id, factory);
    }

    ~SensorRegistration() { QSensorManager::unregisterBackend(QOrientationSensor::sensorType, id); }

    SensorRegistration(const SensorRegistration &) = delete;
    SensorRegistration &operator=(const SensorRegistration &) = delete;
    const QByteArray id;
};
} // namespace kobalt::test

#endif

using namespace kobalt;
using test::PaintRegions;

namespace {
struct SyncNetworkState
{
    bool connected = false, interactive = false;
    int requests = 0, cancellations = 0, releases = 0;
    std::function<void(bool)> completed;
};

class TestSyncNetwork final : public SyncNetwork
{
public:
    explicit TestSyncNetwork(std::shared_ptr<SyncNetworkState> state) : state(std::move(state)) { }

    bool connected() const override { return state->connected; }

    void prepare(bool interactive, std::function<void(bool)> done) override
    {
        ++state->requests;
        state->interactive = interactive;
        state->completed = std::move(done);
    }

    void cancel() override
    {
        if (state->completed)
            ++state->cancellations;
        state->completed = {};
    }

    void release() override { ++state->releases; }

private:
    std::shared_ptr<SyncNetworkState> state;
};

// Answers the reader's KOSync requests without a server: no remote progress
// for a GET, and the uploaded document for a PUT. While `hold` is set, new
// replies wait in `held` until the test finishes them.
struct KosyncHttpState
{
    int requests = 0, uploads = 0;
    int readStatus = 200, uploadStatus = 200;
    QByteArray readBody = "{}";
    QByteArray uploadBody;
    QJsonObject lastUpload;
    QNetworkRequest lastAuthentication;
    bool hold = false;
    QList<QPointer<test::KosyncReply>> held;
};

class KosyncHttp final : public QNetworkAccessManager
{
public:
    explicit KosyncHttp(std::shared_ptr<KosyncHttpState> state) : state(std::move(state)) { }

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *data) override
    {
        ++state->requests;
        if (request.url().path().endsWith("/users/auth"))
            state->lastAuthentication = request;
        QByteArray body = state->readBody;
        if (operation == PutOperation) {
            ++state->uploads;
            const auto upload =
                    QJsonDocument::fromJson(data ? data->readAll() : QByteArray()).object();
            state->lastUpload = upload;
            body = QJsonDocument(QJsonObject{ { "document", upload.value("document") },
                                              { "timestamp", 1 } })
                           .toJson(QJsonDocument::Compact);
            if (!state->uploadBody.isEmpty())
                body = state->uploadBody;
        }
        auto *reply = new test::KosyncReply(
                request, body, operation == PutOperation ? state->uploadStatus : state->readStatus,
                operation != PutOperation && state->readStatus == 404
                        ? QNetworkReply::ContentNotFoundError
                        : QNetworkReply::NoError,
                this, state->hold);
        if (state->hold)
            state->held.append(reply);
        return reply;
    }

private:
    std::shared_ptr<KosyncHttpState> state;
};

// Observe queued reply delivery without delaying the worker or sleeping.
class NativeTestApplication final : public QApplication
{
public:
    using QApplication::QApplication;
    std::function<void(QObject *, QEvent *)> beforeEvent, afterEvent;

    bool notify(QObject *object, QEvent *event) override
    {
        const auto before = beforeEvent;
        if (before)
            before(object, event);
        mainthread::EventDelivery delivery;
        const bool handled = QApplication::notify(object, event);
        const auto after = afterEvent;
        if (after)
            after(object, event);
        return handled;
    }
};

const QSize nativeSize(1264, 1680);
EngineFontInputs testFonts;
QString testPlaceholder;
QString profile;
QString coverBook, textBook;
std::atomic_bool delayShutdownEncoding{ false }, releaseShutdownEncoding{ false },
        shutdownEncodingReturned{ false };

// A delayed encoding holds the engine worker until the test releases it, so
// the mapping outlasts the reader's deadline without a fixed sleep. The bound
// keeps a failed test from holding the worker for good.
QString shutdownPosition(const Engine &model, QString *error)
{
    if (delayShutdownEncoding) {
        QElapsedTimer held;
        held.start();
        while (!releaseShutdownEncoding && held.elapsed() < 10000)
            QThread::msleep(1);
        shutdownEncodingReturned = true;
    }
    return nickel::positionCodec().encode(model, error);
}

bool prepareSleepBooks(const QString &directory)
{
    auto files = test::engineBook();
    textBook = directory + "/text.epub";
    coverBook = directory + "/deferred-cover.epub";
    if (!test::writeBook(directory, "text.epub", files))
        return false;
    QImage image(128, 64, QImage::Format_ARGB32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x)
            image.setPixel(x, y, qRgba(0, x < 64 ? 192 : 0, x < 64 ? 0 : 255, y < 32 ? 255 : 128));
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return false;
    // Keep the declared cover outside the first page and larger than a probe
    // block, as in the original deferred-cover fixture. It must be decoded later.
    const QByteArray text =
            QByteArray("Comment\0", 8) + QByteArray("cover fixture ").repeated(12000);
    const QByteArray payload = "tEXt" + text;
    QByteArray chunk(4, '\0');
    qToBigEndian<quint32>(text.size(), chunk.data());
    chunk += payload;
    char checksum[4];
    qToBigEndian<quint32>(
            crc32(0, reinterpret_cast<const Bytef *>(payload.constData()), payload.size()),
            checksum);
    chunk.append(checksum, 4);
    png.insert(33, chunk);
    files["OEBPS/colors.png"] = png;
    files["OEBPS/book.opf"].replace("</metadata>",
                                    "<meta name=\"cover\" content=\"colors\"/></metadata>");
    files["OEBPS/book.opf"].replace(
            "</manifest>",
            "<item id=\"colors\" href=\"colors.png\" media-type=\"image/png\"/></manifest>");
    return test::writeBook(directory, "deferred-cover.epub", files);
}

QDateTime testLocalTime(int milliseconds)
{
    return QDateTime(QDate(2026, 1, 15), QTime::fromMSecsSinceStartOfDay(milliseconds),
                     QTimeZone::UTC);
}

storage::Paths testPaths()
{
    storage::Paths paths{ profile };
    paths.stateReport = profile + "/state.json";
    return paths;
}

QString fixture()
{
    return QStringLiteral(KOBALT_TEST_BOOK);
}

// Compose real publication, session, book and window objects. Keeping the session
// handle here permits deterministic worker cancellation and failed-apply tests
// without exposing the reader's private implementation or copying its controller.
class ReaderFixture final : public QObject
{
    Q_OBJECT
public:
    ReaderFixture(storage::Paths paths, EngineFontInputs fonts, QString placeholder)
        : paths(std::move(paths)),
          fonts(std::move(fonts)),
          placeholder(std::move(placeholder)),
          orientation(nullptr, [](Qt::ScreenOrientation) { })
    {
    }

    ~ReaderFixture() override
    {
        shutdown();
        if (!QTest::qWaitFor([this] { return idle(); }, 10000))
            qFatal("Reader lifecycle fixture did not drain");
    }

    ReaderWindow *window() const { return window_; }

    ReaderSession *session() const { return session_; }

    ReadingBook *book() const { return book_; }

    bool idle() const { return !active; }

    ReaderOrientation *rotation = nullptr;
    std::shared_ptr<test::HostProgress> progress;
    PositionCodec positions;
    ReadingHistory::UtcClock utc;
    ReadingTimer::Clock readingClock;
    std::function<void(ReaderHostServices &)> configureServices;

    void open(const QString &path, QSize size = nativeSize,
              std::unique_ptr<ReaderSession> kept = {})
    {
        Q_ASSERT(idle());
        source = native::openLocalPublication(path);
        if (!source) {
            emit failed("Cannot open local publication");
            return;
        }
        active = true;
        lease = std::make_shared<SourceLease>();
        const auto identity = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
        const auto statePath = paths.bookState(identity);
        auto saved = storage::readBookState(paths, identity, statePath).object;
        const auto preferences =
                appearance::readerPreferences(storage::readJson(paths.preferences()), saved);
        saved["dark_mode"] = preferences.value("dark_mode").toBool();
        saved["bold_reading"] = preferences.value("bold_reading").toString("off");
        for (const auto *key : appearance::normalizationKeys)
            saved[key] = appearance::normalizationSettings(preferences).value(key);
        (rotation ? rotation : &orientation)->configure(preferences);
        opening = kept ? std::move(kept) : std::make_unique<ReaderSession>(fonts, placeholder);
        session_ = opening.get();
        OpenRequest request{ identity,
                             path,
                             readerInterface().readingViewport(size, preferences),
                             saved,
                             {},
                             lease,
                             source->resources(lease) };
        request.appearanceDefaults = appearance::bookDefaults(preferences);
        request.positions = positions;
        session_->open(
                std::move(request),
                [this, identity, statePath, size](OpeningResult outcome) {
                    if (closing)
                        return;
                    if (!outcome.ready()) {
                        emit failed("Cannot prepare publication");
                        shutdown();
                        return;
                    }
                    const auto hash = session_->sourceHash();
                    const auto baseline = progress ? progress->current : ReadingProgress{};
                    auto book = std::make_unique<ReadingBook>(
                            std::move(opening), statePath, paths, identity, hash,
                            progress ? test::hostReadingBridge(progress) : nullptr, baseline,
                            baseline, QString(), utc, readingClock);
                    book_ = book.get();
                    auto services = desktop::readerHostServices();
                    services.desktopUi = false;
                    services.suspended = [] { return false; };
                    services.clock = [] { return QStringLiteral("09:00"); };
                    services.openBrowser = [](const QUrl &, QWidget *) { return false; };
                    services.inputMilliseconds = test::inputMilliseconds;
                    if (configureServices)
                        configureServices(services);
                    window_ =
                            createReaderWindow(std::move(book), rotation ? rotation : &orientation,
                                               std::move(services));
                    window_->setAttribute(Qt::WA_DeleteOnClose, false);
                    window_->setWindowFlags(Qt::Widget);
                    window_->setWindowModality(Qt::NonModal);
                    window_->resize(size);
                    window_->finished = [this](std::unique_ptr<ReaderSession>) {
                        QTimer::singleShot(0, this, [this] { shutdown(); });
                    };
                    emit opened();
                },
                {}, {}, [this] { emit reused(); });
    }

    void shutdown()
    {
        if (!active || closing)
            return;
        closing = true;
        // Release injected save failures before fixture cleanup. The real USB
        // barrier deliberately refuses to dispose a reader with failed saves.
        if (progress) {
            progress->failWrite = progress->writeBlocked = progress->holdFinish =
                    progress->failFinish = false;
            progress->completeFinish(true);
        }
        if (window_)
            window_->prepareShutdown([this] { retire(); });
        else
            retire();
    }

signals:
    void reused();
    void opened();
    void closed();
    void failed(QString error);

private:
    void retire()
    {
        if (lease)
            lease->cancel();
        if (window_) {
            window_->finished = {};
            delete window_.data();
            book_ = nullptr;
        }
        opening.reset();
        ReaderSession::drain([this] {
            source.reset();
            releaseResourceConnections([this] {
                lease.reset();
                active = closing = false;
                emit closed();
            });
        });
    }

    storage::Paths paths;
    EngineFontInputs fonts;
    QString placeholder;
    ReaderOrientation orientation;
    std::unique_ptr<native::PublicationEndpoint> source;
    std::shared_ptr<SourceLease> lease;
    std::unique_ptr<ReaderSession> opening;
    QPointer<ReaderSession> session_;
    ReadingBook *book_ = nullptr;
    QPointer<ReaderWindow> window_;
    bool active = false, closing = false;
};

struct CloseHost
{
    ReaderFixture &host;

    ~CloseHost()
    {
        host.shutdown();
        if (!QTest::qWaitFor([this] { return host.idle(); }, 10000))
            qFatal("Reader lifecycle test did not drain");
    }
};

// The reader's armed timers apart from its minute clock: repeating timers and
// single-shot timers due within a minute. The follow-up work after a turn or
// an opening uses such timers and ends on its own. A test that proves that
// nothing more happens waits for this list to empty instead of for a time.
QStringList activeReaderTimers(ReaderWindow *window)
{
    QStringList active;
    for (auto *timer : window->findChildren<QTimer *>())
        if (timer->isActive() && timer->objectName() != "rrClockTimer"
            && (timer->interval() < 60000 || !timer->isSingleShot()))
            active.append(QString("%1 (%2 ms%3)")
                                  .arg(timer->objectName().isEmpty() ? QString("unnamed")
                                                                     : timer->objectName())
                                  .arg(timer->interval())
                                  .arg(timer->isSingleShot() ? ", single shot" : ""));
    return active;
}

// Runs the reader's armed sync deadline now instead of waiting for it.
// Returns false when no single-shot deadline with this interval is armed: 10 s
// for a sync, 2 s for the final sync at close or sleep.
bool runKosyncDeadline(ReaderWindow *window, int interval = 10000)
{
    auto *deadline = window->findChild<QTimer *>("rrKosyncDeadline");
    if (!deadline || !deadline->isActive() || !deadline->isSingleShot()
        || deadline->interval() != interval)
        return false;
    deadline->stop();
    QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
    return true;
}

// The position-mapping deadline of the reader's shutdown request:
// a single-shot 2 s timer. Returns nullptr when there is not exactly one.
QTimer *transitionMappingDeadline(ReaderWindow *window)
{
    const auto deadlines = window->findChildren<QTimer *>("rrTransitionMappingDeadline");
    if (deadlines.size() != 1 || !deadlines.last()->isSingleShot()
        || deadlines.last()->interval() != 2000)
        return nullptr;
    return deadlines.last();
}

class Canvas final : public standalone::WidgetCanvas
{
public:
    Canvas(ReaderWindow *reader, QSize size) : WidgetCanvas(nullptr)
    {
        reader->setFixedSize(size);
        setContent(reader);
        setSceneRect(QRectF(QPointF(), size));
    }

    QWidget *surface() const { return content; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        WidgetCanvas::resizeEvent(event);
        fitInView(sceneRect(), Qt::KeepAspectRatio);
    }
};

bool clickAt(Canvas &canvas, QPoint point)
{
    QCoreApplication::processEvents();
    const auto local = canvas.mapFromScene(point);
    if (!canvas.viewport()->rect().contains(local))
        return false;
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, local);
    return true;
}

bool click(Canvas &canvas, QWidget *widget)
{
    QCoreApplication::processEvents();
    if (!widget || !widget->isVisible() || !widget->isEnabled())
        return false;
    const auto point = widget->mapTo(canvas.surface(), widget->rect().center());
    const auto *receiver = canvas.surface()->childAt(point);
    if (receiver != widget && (!receiver || !widget->isAncestorOf(receiver)))
        return false;
    return clickAt(canvas, point);
}

QWidget *openSettings(ReaderFixture &host, const char *section = nullptr)
{
    if (auto *open = host.window()->findChild<QPushButton *>("rrSettings"))
        open->click();
    // Tests without a canvas leave the window hidden, so ask the panel itself.
    auto *settings = host.window()->findChild<QWidget *>("rrGeneralSettings");
    if (!settings || settings->isHidden())
        return nullptr;
    if (section) {
        auto *tab = settings->findChild<QPushButton *>(section);
        if (!tab)
            return nullptr;
        tab->click();
    }
    return settings;
}

// Visit Sync so its status and actions can be observed while the book is read.
bool buildKosyncPage(ReaderFixture &host)
{
    auto *settings = openSettings(host, "rrSettingsKosync");
    auto *close = host.window()->findChild<QPushButton *>("rrSettingsClose");
    if (!settings || !close)
        return false;
    close->click();
    return QTest::qWaitFor([&] { return settings->isHidden() && !host.session()->busy(); });
}

QPushButton *sizeStep(ReaderFixture &host)
{
    auto *slider = host.window()->findChild<QSlider *>("rrTextSize");
    if (!slider)
        return nullptr;
    return host.window()->findChild<QPushButton *>(
            slider->value() == slider->maximum() ? "rrSmaller" : "rrLarger");
}

QJsonObject inspectReader(ReaderFixture &host)
{
    host.window()->checkpoint();
    return storage::readJson(testPaths().stateReport);
}

int sliderX(const QSlider *slider, int value)
{
    const int inset = ui::metrics::control::sliderInset;
    return inset
            + QStyle::sliderPositionFromValue(slider->minimum(), slider->maximum(), value,
                                              qMax(1, slider->width() - 2 * inset));
}

// One continuous press, move and release through the window, as a finger
// drag arrives on the device. Events go through Qt's grab and enabled checks.
void drag(Canvas &canvas, QSlider *slider, int from, int to,
          const std::function<void()> &moved = {})
{
    QCoreApplication::processEvents();
    const auto at = [&](int value) {
        return canvas.mapFromScene(slider->mapTo(
                canvas.surface(), QPoint(sliderX(slider, value), slider->height() / 2)));
    };
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, at(from));
    QCoreApplication::processEvents();
    const int direction = to >= from ? 1 : -1;
    for (int value = from; value != to + direction; value += direction) {
        QTest::mouseMove(canvas.viewport(), at(value));
        QCoreApplication::processEvents();
        if (moved)
            moved();
    }
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, at(to));
}

// The same gesture by track position, for sliders whose stops are evenly
// spaced rather than proportional to their values.
void dragAcross(Canvas &canvas, QSlider *slider, qreal from, qreal to,
                const std::function<void()> &moved = {})
{
    const int inset = ui::metrics::control::sliderInset;
    const auto at = [&](qreal fraction) {
        const QPoint local(inset + qRound(fraction * (slider->width() - 2 * inset)),
                           slider->height() / 2);
        return canvas.mapFromScene(slider->mapTo(canvas.surface(), local));
    };
    QCoreApplication::processEvents();
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, at(from));
    QCoreApplication::processEvents();
    for (int step = 1; step <= 10; ++step) {
        QTest::mouseMove(canvas.viewport(), at(from + (to - from) * step / 10));
        QCoreApplication::processEvents();
        if (moved)
            moved();
    }
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, at(to));
}

} // namespace

class ReaderLifecycleTests final : public QObject
{
    Q_OBJECT
private slots:

    void init()
    {
        directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/kobalt-lifecycle-XXXXXX");
        QVERIFY(directory->isValid());
        profile = directory->path();
    }

    void cleanup()
    {
        engineActivity().pause(false);
        directory.reset();
    }

    void developmentOptions_data()
    {
        QTest::addColumn<int>("action");
        QTest::addColumn<bool>("available");
        QTest::addColumn<bool>("succeeds");
        QTest::newRow("unsupported") << -1 << false << false;
        QTest::newRow("unavailable-book") << 0 << false << false;
        QTest::newRow("failed") << 0 << true << false;
        QTest::newRow("new-position") << 0 << true << true;
        QTest::newRow("duplicate") << 1 << true << true;
        QTest::newRow("older-position") << 2 << true << true;
        QTest::newRow("finished") << 3 << true << true;
        QTest::newRow("save-failure") << 4 << true << true;
    }

    // Checks host-controlled availability, typed simulation actions and failed results.
    void developmentOptions()
    {
        QFETCH(int, action);
        QFETCH(bool, available);
        QFETCH(bool, succeeds);
        QString queriedIdentity;
        ReadingProgress received;
        int calls = 0;
        auto selected = ProgressSimulation::NewPosition;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            if (action < 0)
                return;
            services.development.progressDescription = "Host progress simulation";
            services.development.progressAvailable = [&](const QString &identity) {
                queriedIdentity = identity;
                return available;
            };
            services.development.simulateProgress = [&](ProgressSimulation kind,
                                                        const ReadingProgress &progress) {
                ++calls;
                selected = kind;
                received = progress;
                return succeeds;
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QVERIFY(openSettings(host, "rrSettingsAbout"));
        auto *entry = host.window()->findChild<QPushButton *>("rrDebugProgress");
        if (action < 0) {
            QVERIFY(!entry);
            return;
        }
        QVERIFY(entry);
        entry->click();
        auto *card = host.window()->findChild<QWidget *>("rrProgressDebug");
        QVERIFY(card);
        QCOMPARE(queriedIdentity, host.book()->identity());
        auto hasMessage = [&](const QString &message) {
            for (auto *label : card->findChildren<QLabel *>())
                if (label->text() == message)
                    return true;
            return false;
        };
        QVERIFY(hasMessage("Host progress simulation"));
        const char *names[] = { "rrDebugNewPosition", "rrDebugDuplicate", "rrDebugStale",
                                "rrDebugFinished", "rrDebugSaveFailure" };
        const ProgressSimulation kinds[] = { ProgressSimulation::NewPosition,
                                             ProgressSimulation::Duplicate,
                                             ProgressSimulation::OlderPosition,
                                             ProgressSimulation::Finished,
                                             ProgressSimulation::SaveFailure };
        for (const auto *name : names) {
            auto *button = card->findChild<QPushButton *>(name);
            QVERIFY(button);
            QCOMPARE(button->isEnabled(), available);
        }
        const auto expected = host.book()->readProgress();
        card->findChild<QPushButton *>(names[action])->click();
        QCOMPARE(calls, available ? 1 : 0);
        if (!available)
            return;
        QCOMPARE(selected, kinds[action]);
        QCOMPARE(received.book, expected.book);
        QCOMPARE(received.location, expected.location);
        QCOMPARE(received.modified, expected.modified);
        QCOMPARE(received.status, expected.status);
        if (succeeds) {
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrProgressDebug"));
        } else {
            QVERIFY(hasMessage(strings::get(
                    "reader.the_simulation_failed_no_successful_update_was_reported")));
        }
    }

    void developmentStateReports_data()
    {
        QTest::addColumn<bool>("details");
        QTest::addColumn<bool>("continuous");
        QTest::newRow("ordinary") << false << false;
        QTest::newRow("detailed") << true << false;
        QTest::newRow("continuous") << false << true;
        QTest::newRow("continuous-detailed") << true << true;
    }

    // Checks opt-in routine and detailed reports without disabling lifecycle snapshots.
    void developmentStateReports()
    {
        QFETCH(bool, details);
        QFETCH(bool, continuous);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.development.detailedStateReports = details;
            services.development.continuousStateReports = continuous;
        };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        QTRY_VERIFY(!host.session()->busy());
        // Opening always takes a snapshot. Drain it before checking whether
        // the later progress choice produces an opt-in routine report.
        QTRY_VERIFY(QFile::exists(testPaths().stateReport));
        QTRY_VERIFY(!host.window()->pendingStorageSave());
        QVERIFY(QFile::remove(testPaths().stateReport));
        host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true });
        host.window()->progressTick();
        QPushButton *keep = nullptr;
        QTRY_VERIFY((keep = host.window()->findChild<QPushButton *>("rrKeepPosition"))
                    && keep->isVisible());
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->pendingStorageSave());
        QCOMPARE(QFile::exists(testPaths().stateReport), continuous);
        host.window()->checkpoint();
        QTRY_VERIFY(!host.window()->pendingStorageSave());
        const auto report = storage::readJson(testPaths().stateReport);
        QVERIFY(report.contains("page"));
        QCOMPARE(report.contains("links"), details);
        QCOMPARE(report.contains("word_counts"), details);
        QCOMPARE(report.contains("reading_estimates"), details);
    }

    // Checks that an event delivered during reader destruction no longer reaches the reader's application filter.
    void teardownRemovesApplicationFilter()
    {
        class Probe : public QWidget
        {
        public:
            using QWidget::QWidget;
            bool received = false;

            bool event(QEvent *event) override
            {
                if (event->type() == QEvent::HoverEnter) {
                    received = true;
                    return true;
                }
                return QWidget::event(event);
            }
        };

        bool delivered = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.presentation = [&](QWidget *reader) {
                auto *probe = new Probe(reader);
                auto presentation = std::make_unique<test::HostPresentation>();
                presentation->resumed = [&, probe] {
                    QEvent event(QEvent::HoverEnter);
                    QCoreApplication::sendEvent(probe, &event);
                    delivered = probe->received;
                };
                return presentation;
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.shutdown();
        QTRY_VERIFY(host.idle());
        QVERIFY2(delivered,
                 "The reader's application filter still consumed events during teardown");
    }

    void settingsTeardownReleasesConfirmation()
    {
        QWidget window;
        reader::SettingsPanelInputs inputs;
        inputs.automaticBookOpeningSupported = true;
        auto settings = std::make_unique<reader::SettingsPanel>(&window, inputs);
        settings->settingsTabButtons[int(reader::SettingsPage::System)]->click();
        auto *automatic = settings->findChild<QCheckBox *>("rrAutomaticBookOpening");
        QVERIFY(automatic && automatic->isEnabled());
        automatic->setChecked(true);
        automatic->click();
        const QPointer<QWidget> dialog =
                window.findChild<QWidget *>("rrAutomaticBookOpeningConfirmation");
        QVERIFY(dialog && dialog->parentWidget() == &window);
        settings.reset();
        QVERIFY2(!dialog, "The detached confirmation still refers to the deleted Settings panel");
    }

    void settingsChoiceSheetReusesRows()
    {
        QWidget window;
        window.resize(720, 1440);
        reader::SettingsPanel settings(&window, {});
        window.show();
        QStringList chosen;
        settings.showTimeRemainingFormats(
                "detailed", [&](const QString &value) { chosen.append("-" + value); });
        QPointer<QWidget> overlay = window.findChild<QWidget *>("rrTimeRemainingFormatPopup");
        QPointer<QCheckBox> first = window.findChild<QCheckBox *>("rrTimeRemainingFormat_detailed");
        QPointer<QCheckBox> second =
                window.findChild<QCheckBox *>("rrTimeRemainingFormat_approximate");
        QVERIFY(overlay && overlay->isVisible() && first && second && first->isChecked());
        QTest::keyClick(overlay, Qt::Key_Escape);
        QVERIFY(overlay && !overlay->isVisible());
        QVERIFY(chosen.isEmpty());

        QString format;
        settings.showPercentageFormats("whole", [&](const QString &value) { format = value; });
        auto *decimal = window.findChild<QCheckBox *>("rrPercentageFormat_decimal");
        QVERIFY(decimal && decimal->isVisible());
        decimal->click();
        QCOMPARE(format, QStringLiteral("decimal"));

        window.resize(1440, 720);
        settings.showTimeRemainingFormats("approximate",
                                          [&](const QString &value) { chosen.append(value); });
        QVERIFY(overlay && overlay->isVisible());
        QCOMPARE(overlay->geometry(), window.rect());
        auto *card = overlay->findChild<QWidget *>("rrModalCard");
        QVERIFY(card && overlay->rect().contains(card->geometry()));
        QVERIFY(first
                && first == overlay->findChild<QCheckBox *>("rrTimeRemainingFormat_detailed"));
        QVERIFY(second
                && second == overlay->findChild<QCheckBox *>("rrTimeRemainingFormat_approximate"));
        QVERIFY(!first->isChecked() && second->isChecked());
        first->click();
        QCOMPARE(chosen, QStringList{ "detailed" });
        QVERIFY(!overlay->isVisible());
    }

    void settingsFontSheetReusesRows()
    {
        QWidget window;
        window.resize(720, 1440);
        reader::SettingsPanel settings(&window, {});
        window.show();
        QVector<ReadingFont> catalog{ { {}, {}, FontOrigin::Recommended, {}, {} },
                                      { "NV Jost", {}, FontOrigin::Recommended, {}, {} },
                                      { "Libron", {}, FontOrigin::BuiltIn, {}, {} } };
        QStringList chosen;
        settings.showFontChoices("Main font", catalog, "NV Jost", "Publisher",
                                 [&](const QString &value) { chosen.append("main:" + value); });
        QPointer<QWidget> sheet = window.findChild<QWidget *>("rrSettingsFontPopup");
        QPointer<QCheckBox> empty = window.findChild<QCheckBox *>("rrSettingsFontChoice0");
        QPointer<QCheckBox> jost = window.findChild<QCheckBox *>("rrSettingsFontChoice1");
        QPointer<QCheckBox> libron = window.findChild<QCheckBox *>("rrSettingsFontChoice2");
        QVERIFY(sheet && empty && jost && libron && jost->isVisible() && jost->isChecked());
        QCOMPARE(empty->text(), QStringLiteral("Publisher"));
        QTest::keyClick(sheet, Qt::Key_Escape);
        QVERIFY(sheet && !sheet->isVisible());
        QVERIFY(chosen.isEmpty());

        window.resize(1440, 720);
        settings.showFontChoices("Strip font", catalog, "Libron", "Default",
                                 [&](const QString &value) { chosen.append("strip:" + value); });
        QVERIFY(sheet && sheet->isVisible());
        QCOMPARE(sheet->geometry(), window.rect());
        auto *card = sheet->findChild<QWidget *>("rrModalCard");
        auto *title = sheet->findChild<QLabel *>("rrSettingsFontTitle");
        QVERIFY(card && sheet->rect().contains(card->geometry()) && title);
        QCOMPARE(title->text(), QStringLiteral("Strip font"));
        QVERIFY(empty && empty == sheet->findChild<QCheckBox *>("rrSettingsFontChoice0"));
        QVERIFY(jost && !jost->isChecked());
        QVERIFY(libron && libron == sheet->findChild<QCheckBox *>("rrSettingsFontChoice2"));
        QCOMPARE(empty->text(), QStringLiteral("Default"));
        QVERIFY(libron->isVisible() && libron->isChecked());
        libron->click();
        QCOMPARE(chosen, QStringList{ "strip:Libron" });

        QPointer<ui::ModalOverlay> activeSheet;
        settings.sheetShown = [&](ui::ModalOverlay *shown) { activeSheet = shown; };
        catalog[2].family = QStringLiteral("NV Bitter");
        catalog[2].origin = FontOrigin::Sideloaded;
        settings.showFontChoices("Strip font", catalog, "NV Bitter", "Default",
                                 [&](const QString &value) { chosen.append(value); });
        QVERIFY(activeSheet && activeSheet->isVisible());
        auto *replacement = activeSheet->findChild<QCheckBox *>("rrSettingsFontChoice2");
        QVERIFY(replacement && replacement->isVisible() && replacement->isChecked());
        QCOMPARE(replacement->property("reader_font_family").toString(),
                 QStringLiteral("NV Bitter"));
        replacement->click();
        QCOMPARE(chosen, (QStringList{ "strip:Libron", "NV Bitter" }));
    }

    void settingsChoiceFitsAfterResize_data()
    {
        QTest::addColumn<QString>("tabName");
        QTest::addColumn<QString>("buttonName");
        QTest::newRow("session-timeout")
                << QString("rrSettingsSystem") << QString("rrSessionTimeout");
        QTest::newRow("screen-refresh")
                << QString("rrSettingsDisplay") << QString("rrScreenRefresh");
    }

    void settingsChoiceFitsAfterResize()
    {
        QFETCH(QString, tabName);
        QFETCH(QString, buttonName);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        const QSize portraitSize(720, 1440);
        host.open(fixture(), portraitSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        auto *window = host.window();
        window->show();
        QTRY_VERIFY(window->readyForPresentation());
        QVERIFY(openSettings(host));
        auto *tab = window->findChild<QPushButton *>(tabName);
        QVERIFY(tab);
        tab->click();
        auto *button = window->findChild<QPushButton *>(buttonName);
        QVERIFY(button);
        button->click();
        const auto popupName = buttonName + QStringLiteral("Popup");
        const QPointer<QWidget> popup = window->findChild<QWidget *>(popupName);
        QVERIFY(popup && popup->isVisible());
        QCOMPARE(popup->geometry(), window->rect());

        auto *card = popup->findChild<QWidget *>("rrModalCard");
        QVERIFY(card && popup->rect().contains(card->geometry()));
        window->resize(portraitSize.transposed());
        QCOMPARE(window->size(), portraitSize.transposed());
        QCOMPARE(popup->geometry(), window->rect());
        QVERIFY2(popup->rect().contains(card->geometry()),
                 "Changing orientation clipped the chooser");
        auto *choices = popup->findChild<QWidget *>(buttonName + QStringLiteral("Choices"));
        auto *next = popup->findChild<QPushButton *>(buttonName + QStringLiteral("Next"));
        auto *previous = popup->findChild<QPushButton *>(buttonName + QStringLiteral("Previous"));
        QVERIFY(choices && next && previous);
        const auto options = choices->findChildren<QCheckBox *>();
        QVERIFY(!options.isEmpty());
        for (int i = 0; i < options.size() && previous->isEnabled(); ++i)
            previous->click();
        QSet<QCheckBox *> seen;
        for (int i = 0; i < options.size(); ++i) {
            for (auto *option : options) {
                if (option->isVisible()) {
                    const QRect bounds(option->mapTo(popup, QPoint()), option->size());
                    QVERIFY(popup->rect().contains(bounds));
                    seen.insert(option);
                }
            }
            if (!next->isEnabled())
                break;
            next->click();
        }
        QCOMPARE(seen.size(), options.size());
        window->resize(portraitSize);
        QCOMPARE(window->size(), portraitSize);
        QVERIFY(popup->rect().contains(card->geometry()));
        QVERIFY(!next->isVisible());
        for (auto *option : options)
            QVERIFY(option->isVisible());
        QTest::keyClick(popup, Qt::Key_Escape);
        QTRY_VERIFY(!popup);
        window->resize(portraitSize.transposed());
        button->click();
        auto *reopened = window->findChild<QWidget *>(popupName);
        QVERIFY(reopened && reopened->isVisible());
        card = reopened->findChild<QWidget *>("rrModalCard");
        QVERIFY2(card && reopened->rect().contains(card->geometry()),
                 "Opening the chooser in landscape clips its card");
    }

    void appearanceTeardownStopsPanelCallbacks_data()
    {
        QTest::addColumn<QString>("buttonName");
        QTest::addColumn<bool>("pending");
        QTest::newRow("text-ready") << "rrNavText" << false;
        QTest::newRow("layout-ready") << "rrNavLayout" << false;
        QTest::newRow("text-pending") << "rrNavText" << true;
        QTest::newRow("layout-pending") << "rrNavLayout" << true;
        QTest::newRow("settings") << "rrSettings" << false;
    }

    // Checks that panel events stop calling the reader before its members are destroyed, including cancelled previews.
    void appearanceTeardownStopsPanelCallbacks()
    {
        QFETCH(QString, buttonName);
        QFETCH(bool, pending);
        bool destroying = false;
        int liveCallbacks = 0, teardownCallbacks = 0;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.presentation = [&](QWidget *) {
                auto presentation = std::make_unique<test::HostPresentation>();
                presentation->resumed = [&] { destroying = true; };
                return presentation;
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QVERIFY(QTest::qWaitForWindowExposed(host.window()));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        page->tapped(page->rect().center());
        auto *button = host.window()->findChild<QPushButton *>(buttonName);
        QVERIFY(button);
        const auto resume = qScopeGuard([] { engineActivity().pause(false); });
        if (pending)
            engineActivity().pause(true);
        button->click();
        if (pending)
            QTRY_VERIFY(inspectReader(host)["preview_busy"].toBool()
                        || inspectReader(host)["preview_queued"].toBool());
        else
            QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool()
                        && !inspectReader(host)["preview_queued"].toBool());
        // Observe the real subscription without accessing a destroyed Reader. This
        // makes a late callback an assertion failure instead of allocator-dependent UB.
        const auto observed = [&] {
            if (destroying)
                ++teardownCallbacks;
            else
                ++liveCallbacks;
        };
        if (buttonName == "rrSettings") {
            auto *settings = static_cast<reader::SettingsPanel *>(
                    host.window()->findChild<QWidget *>("rrGeneralSettings"));
            QVERIFY(settings && settings->isVisible());
            settings->stylingHidden = observed;
            settings->hide();
            settings->show();
        } else {
            auto *preview = static_cast<reader::AppearancePreview *>(
                    host.window()->findChild<QWidget *>("rrAppearancePreview"));
            auto *panel = host.window()->findChild<QWidget *>("rrReadingMenu");
            QVERIFY(preview && panel && panel->isVisible());
            preview->followPanel(panel, observed);
            QEvent moved(QEvent::Move);
            QCoreApplication::sendEvent(panel, &moved);
        }
        QCOMPARE(liveCallbacks, 1);
        host.shutdown();
        engineActivity().pause(false);
        QTRY_VERIFY(host.idle());
        QVERIFY(destroying);
        QCOMPARE(teardownCallbacks, 0);
    }

    // Checks that shutdown ends at the position-conversion deadline with a stalled engine, keeps the local position and writes no native one.
    void shutdownFinalSaveTimesOut()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.progress = std::make_shared<test::HostProgress>();
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        const int writes = host.progress->writes;
        int completed = 0;
        engineActivity().pause(true);
        host.window()->prepareShutdown([&] { ++completed; });
        // The paused engine holds the mapping, so its 2 s deadline ends the
        // wait. Check it and run it now instead of waiting for it.
        auto *deadline = transitionMappingDeadline(host.window());
        const bool armed = deadline && deadline->isActive();
        if (armed) {
            deadline->stop();
            QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
        }
        const bool timedOut = armed && QTest::qWaitFor([&] { return completed == 1; }, 4000);
        // Always release the worker before an assertion or fixture cleanup.
        engineActivity().pause(false);
        QVERIFY(armed);
        QVERIFY(timedOut);
        bool drained = false;
        ReaderSession::drain([&] { drained = true; });
        QTRY_VERIFY(drained);
        QCOMPARE(completed, 1);
        QCOMPARE(host.progress->writes, writes);
        QVERIFY(!storage::readBookState(testPaths(), identity)
                         .object.value("position")
                         .toString()
                         .isEmpty());
    }

    // Checks that a clock tick repaints only the changed reading strips and does not count as a page turn.
    void clockUpdatesOnlyReadingStrips_data()
    {
        QTest::addColumn<bool>("custom");
        QTest::addColumn<bool>("header");
        QTest::newRow("default") << false << true;
        QTest::newRow("custom-right") << true << true;
        QTest::newRow("header-off") << false << false;
    }

    void clockUpdatesOnlyReadingStrips()
    {
        QFETCH(bool, custom);
        QFETCH(bool, header);
        QJsonObject preferences{ { "header_footer", custom ? "custom" : "default" } };
        if (custom)
            preferences["header_footer_custom"] =
                    QJsonObject{ { "top_left", "nothing" }, { "top_right", "time" } };
        if (!header)
            preferences["header_footer_text"] = QJsonObject{ { "header_size", "off" } };
        QVERIFY(storage::writePreferences(testPaths().preferences(), preferences));
        QString time = "09:00";
        auto local = testLocalTime(9 * 3600000 + 30000);
        int reads = 0;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.clock = [&] {
                ++reads;
                return time;
            };
            services.localDateTime = [&] { return local; };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        auto *clock = host.window()->findChild<QTimer *>("rrClockTimer");
        QVERIFY(page && clock);
        QTRY_VERIFY(!page->image.isNull());
        QVERIFY(page->image.text("kobalt.animation_text_only") != "true");
        QCoreApplication::processEvents();
        test::PaintRegions paints;
        page->installEventFilter(&paints);
        const auto body = page->image.cacheKey();
        const auto position = host.session()->state().value("position");
        time = "09:01";
        reads = 0;
        local = testLocalTime(9 * 3600000 + 60050);
        QVERIFY(QMetaObject::invokeMethod(clock, "timeout", Qt::DirectConnection));
        QVERIFY(!host.window()->property("reader_page_turn").toBool());
        QCOMPARE(reads, header ? 1 : 0);
        if (header) {
            QVERIFY(page->headerText().contains(time));
            QTRY_VERIFY(!paints.painted.isEmpty());
        } else
            QCoreApplication::processEvents();
        QVERIFY(!paints.painted.intersects(
                QRect(0, page->topHeight(), page->width(),
                      page->height() - page->topHeight() - page->bottomHeight())));
        QCOMPARE(page->image.cacheKey(), body);
        QCOMPARE(host.session()->state().value("position"), position);
        paints.painted = {};
        QVERIFY(QMetaObject::invokeMethod(clock, "timeout", Qt::DirectConnection));
        QCoreApplication::processEvents();
        QVERIFY(paints.painted.isEmpty());
        QCOMPARE(reads, header ? 1 : 0);

        if (header && !custom) {
            // Keep the same time of day across a new date, then a new UTC offset.
            local = local.addDays(1);
            host.window()->refreshHostPresentation();
            QCOMPARE(reads, 2);
            local = QDateTime(local.date(), local.time(), QTimeZone::fromSecondsAheadOfUtc(3600));
            host.window()->refreshHostPresentation();
            QCOMPARE(reads, 3);
            host.window()->resumeReadingClock();
            QCOMPARE(reads, 3);
            host.window()->sleepReader();
            const int beforeWake = reads;
            time = "9:01 AM";
            host.window()->wakeReader();
            QTRY_VERIFY(page->headerText().contains(time));
            QCOMPARE(reads, beforeWake + 1);
        }
    }

    void preloadedBookOpensWithFreshHost_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("matching-layout") << false;
        QTest::newRow("changed-layout") << true;
    }

    void preloadedBookOpensWithFreshHost()
    {
        QFETCH(bool, dark);
        const auto paths = testPaths();
        const auto path = fixture();
        const auto identity = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
        {
            ReaderFixture previous(paths, testFonts, testPlaceholder);
            CloseHost cleanup{ previous };
            previous.configureServices = [](ReaderHostServices &services) {
                services.clock = [] { return QStringLiteral("09:00"); };
                services.localDateTime = [] { return testLocalTime(9 * 3600000); };
            };
            QSignalSpy opened(&previous, &ReaderFixture::opened);
            previous.open(path);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
            previous.window()->show();
            auto *page =
                    dynamic_cast<ReadingPage *>(previous.window()->findChild<QWidget *>("rrPage"));
            QVERIFY(page);
            QTRY_VERIFY(page->headerText().contains("09:00"));
            previous.shutdown();
            QTRY_VERIFY_WITH_TIMEOUT(previous.idle(), 10000);
        }
        const auto saved = storage::readBookState(paths, identity);
        QCOMPARE(saved.status, storage::ReadStatus::Ok);
        const auto preferences = appearance::readerPreferences(
                storage::readPreferences(paths.preferences()).object, saved.object);
        auto source = native::openLocalPublication(path);
        QVERIFY(source);
        auto lease = std::make_shared<SourceLease>();
        ReaderSession preload(testFonts, testPlaceholder);
        std::unique_ptr<ReaderSession> kept;
        bool completed = false;
        OpenRequest request{ identity,
                             path,
                             readerInterface().readingViewport(nativeSize, preferences),
                             saved.object,
                             {},
                             lease,
                             source->resources(lease) };
        request.appearanceDefaults = appearance::bookDefaults(preferences);
        preload.open(std::move(request), [&](OpeningResult result) {
            if (result.ready())
                kept = preload.retained();
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 30000);
        QVERIFY(kept);
        QVERIFY(lease->revoked());
        source.reset();
        QCOMPARE(storage::readBookState(paths, identity).object, saved.object);

        // An opening must use the settings and device services from the tap.
        if (dark) {
            auto changed = storage::readPreferences(paths.preferences()).object;
            changed["dark_mode"] = true;
            QVERIFY(storage::writePreferences(paths.preferences(), changed));
        }
        int observers = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.clock = [] { return QStringLiteral("9:00 AM"); };
            services.localDateTime = [] { return testLocalTime(9 * 3600000); };
            services.observeBattery = [&](QObject *, std::function<void()>) { ++observers; };
            services.holdWifiOff = [&](QObject *, std::function<bool()>) { ++observers; };
        };
        QSignalSpy reused(&host, &ReaderFixture::reused);
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(path, nativeSize, std::move(kept));
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QCOMPARE(reused.count(), dark ? 0 : 1);
        QCOMPARE(host.session()->state().value("dark_mode").toBool(), dark);
        QVERIFY(!host.session()->image().isNull());
        QCOMPARE(observers, 2);
        host.window()->show();
        auto *page = dynamic_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QTRY_VERIFY(page->headerText().contains("9:00 AM"));
    }

    void clockStopsWhileReaderIsHidden_data()
    {
        QTest::addColumn<bool>("browser");
        QTest::newRow("ordinary-hide") << false;
        QTest::newRow("browser-handoff") << true;
    }

    void clockStopsWhileReaderIsHidden()
    {
        QFETCH(bool, browser);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QTRY_VERIFY(host.window()->isVisible());
        QTRY_VERIFY(!host.session()->busy()
                    && host.session()->resourceStats().value("pending_images").toInt() == 0);
        auto *clock = host.window()->findChild<QTimer *>("rrClockTimer");
        QVERIFY(clock);
        QSignalSpy ticks(clock, &QTimer::timeout);
        const auto verifyStoppedClock = [&] {
            host.book()->stopReading();
            QVERIFY(!host.book()->readingActive());
            const int before = QTime::currentTime().msecsSinceStartOfDay();
            QVERIFY(QMetaObject::invokeMethod(clock, "timeout", Qt::DirectConnection));
            const int after = QTime::currentTime().msecsSinceStartOfDay();
            if (before / 60000 == after / 60000) {
                QVERIFY(clock->interval() >= 60000 - after % 60000 + 50);
                QVERIFY(clock->interval() <= 60000 - before % 60000 + 50);
            }
        };
        const auto appearance = host.session()->state();
        const auto position = appearance.value("position");
        const auto imageKey = host.session()->image().cacheKey();
        BrowserHandoff handoff;
        QDialog browserDialog;
        browserDialog.setObjectName("N3Dialog");
        QWidget browserView(&browserDialog);
        browserView.setObjectName("N3BrowserView");
        for (int cycle = 0; cycle < 3; ++cycle) {
            QVERIFY(clock->isActive());
            // A hide immediately before the deadline must cancel the wakeup.
            clock->start(1);
            if (browser) {
                handoff.begin(host.window(), [] { return true; });
                browserDialog.show();
            } else {
                host.window()->hide();
            }
            QVERIFY(!host.window()->isVisible());
            QTest::qWait(100);
            QCOMPARE(ticks.count(), 0);
            QVERIFY(!clock->isActive());
            if (browser) {
                browserDialog.hide();
            } else {
                host.window()->show();
            }
            QVERIFY(host.window()->isVisible());
            QVERIFY(clock->isActive());
            QVERIFY(clock->interval() > 0);
            QVERIFY(clock->interval() <= 60050);
            QVERIFY(kobalt::appearance::same(appearance, host.session()->state()));
            QCOMPARE(host.session()->state().value("position"), position);
            QCOMPARE(host.session()->image().cacheKey(), imageKey);
        }
        verifyStoppedClock();
        clock->start(120000);
        QVERIFY(host.session()->updated);
        host.session()->updated();
        QVERIFY(host.book()->readingActive());
        QVERIFY(clock->interval() > 0);
        QVERIFY(clock->interval() <= 60050);
    }

    // Checks that the host checkpoint, registered first as on Android, pauses the reading clock before later callbacks.
    void backgroundCheckpointStopsBeforeOtherCallbacks()
    {
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
        bool suspended = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        };
        // Android registers its checkpoint callback before constructing the
        // reader. It can be the last callback to finish before process death.
        bool checkpointSawPausedSession = false;
        connect(qApp, &QGuiApplication::applicationStateChanged, &host,
                [&](Qt::ApplicationState state) {
                    if (state != Qt::ApplicationInactive || !host.window())
                        return;
                    host.window()->checkpoint();
                    storage::profileTransaction(profile, [&](QSqlDatabase &db) {
                        QSqlQuery query(db);
                        if (!query.exec(
                                    "SELECT COUNT(*) FROM reading_sessions WHERE status='active'")
                            || !query.next())
                            return false;
                        checkpointSawPausedSession =
                                query.value(0).toInt() == 1 && !host.book()->readingActive();
                        return true;
                    });
                });
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.window()->checkpoint();
        storage::drainWriterThread();
        bool active = false;
        QVERIFY(storage::profileTransaction(profile, [&](QSqlDatabase &db) {
            QSqlQuery query(db);
            if (!query.exec("SELECT COUNT(*) FROM reading_sessions WHERE status='active'")
                || !query.next())
                return false;
            active = query.value(0).toInt() == 1;
            return true;
        }));
        QVERIFY(active);
        suspended = true;
        qApp->applicationStateChanged(Qt::ApplicationInactive);
        QVERIFY2(checkpointSawPausedSession,
                 "Host checkpoint left the session running until a later callback");
    }

    // Checks that after a failed background save the paused reading time stops growing and is saved into the same session row on resume.
    void backgroundWriteFailureRetriesWithoutCountingPause()
    {
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
        bool suspended = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        };
        const auto value = [&](const QString &statement) {
            QVariant result;
            const bool ok = storage::profileTransaction(profile, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                if (!query.exec(statement))
                    return false;
                if (query.next())
                    result = query.value(0);
                return true;
            });
            if (!ok)
                QTest::qFail("Background retry fixture SQL", __FILE__, __LINE__);
            return result;
        };
        connect(qApp, &QGuiApplication::applicationStateChanged, &host,
                [&](Qt::ApplicationState state) {
                    if (state == Qt::ApplicationInactive && host.window())
                        host.window()->checkpoint();
                });
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.window()->checkpoint();
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(),
                     1);
        value("CREATE TRIGGER fail_background BEFORE UPDATE ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
        suspended = true;
        qApp->applicationStateChanged(Qt::ApplicationInactive);
        const auto paused =
                inspectReader(host).value("session_reading_ms").toVariant().toLongLong();
        QVERIFY(paused > 0);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 1);
        QTest::qWait(40);
        qApp->applicationStateChanged(Qt::ApplicationInactive);
        QCOMPARE(inspectReader(host).value("session_reading_ms").toVariant().toLongLong(), paused);
        value("DROP TRIGGER fail_background");
        suspended = false;
        qApp->applicationStateChanged(Qt::ApplicationActive);
        host.window()->checkpoint();
        QTRY_VERIFY(value("SELECT SUM(active_ms) FROM reading_sessions").toLongLong() >= paused);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions").toInt(), 1);
        qApp->applicationStateChanged(Qt::ApplicationActive);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions").toInt(), 1);
    }

    // Checks reading sessions through checkpoints, paused time while inactive, a new session after a long pause, hide and show, and shutdown.
    void standaloneReadingHistory_data()
    {
        QTest::addColumn<bool>("native");
        QTest::newRow("standalone") << false;
        QTest::newRow("native") << true;
    }

    void standaloneReadingHistory()
    {
        QFETCH(bool, native);
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
        bool suspended = false;
        qint64 utc = 100000;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.utc = [&] { return utc; };
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        };
        const auto value = [&](const QString &statement) {
            QVariant result;
            const bool ok = storage::profileTransaction(profile, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                if (!query.exec(statement) || !query.next())
                    return false;
                result = query.value(0);
                return true;
            });
            if (!ok)
                QTest::qFail("Cannot inspect history", __FILE__, __LINE__);
            return result;
        };
        if (native) {
            host.progress = std::make_shared<test::HostProgress>();
            const auto identity =
                    QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
            host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
            host.positions = nickel::positionCodec();
        }
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.window()->checkpoint();
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(),
                     1);
        QVERIFY(!host.window()->findChild<QTimer *>("rrReadingHistoryTimer"));
        QTest::qWait(10);
        host.book()->saveReadingActivity();
        QTRY_VERIFY(!host.book()->savesPending());
        QTRY_VERIFY(value("SELECT SUM(active_ms) FROM reading_sessions").toLongLong() > 0);
        suspended = true;
        qApp->applicationStateChanged(Qt::ApplicationInactive);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 1);
        const auto paused = value("SELECT SUM(active_ms) FROM reading_sessions").toLongLong();
        QTest::qWait(40);
        QCOMPARE(value("SELECT SUM(active_ms) FROM reading_sessions").toLongLong(), paused);
        suspended = false;
        qApp->applicationStateChanged(Qt::ApplicationActive);
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(),
                     1);
        QTest::qWait(10);
        host.window()->checkpoint();
        QTRY_VERIFY(value("SELECT SUM(active_ms) FROM reading_sessions").toLongLong() > paused);
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        auto *menu = host.window()->findChild<QWidget *>("rrControlsTop");
        auto *sessionLabel = host.window()->findChild<QLabel *>("rrMenuSession");
        QVERIFY(page && menu && sessionLabel);
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(menu->isVisible());
        // Seed an already-visible session label without waiting five minutes.
        sessionLabel->setText("Previous session");
        sessionLabel->show();
        suspended = true;
        qApp->applicationStateChanged(Qt::ApplicationInactive);
        utc += 15 * 60000;
        suspended = false;
        qApp->applicationStateChanged(Qt::ApplicationActive);
        QVERIFY(sessionLabel->text().isEmpty());
        QVERIFY(!sessionLabel->isVisible());
        // Resumed activity stays in memory until a batch or commit point.
        host.book()->saveReadingActivity();
        QTRY_VERIFY(!host.book()->savesPending());
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions").toInt(), 2);
        host.window()->hide();
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='completed'").toInt(),
                 2);
        host.window()->show();
        host.window()->checkpoint();
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(),
                     1);
        host.shutdown();
        QTRY_VERIFY(host.idle());
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='completed'").toInt(),
                 3);
    }

    void navigationDismissalWaitsForPanel_data()
    {
        QTest::addColumn<QString>("tabName");
        QTest::addColumn<QString>("panelName");
        QTest::addColumn<bool>("edit");
        QTest::addColumn<QString>("closeControl");
        QTest::newRow("chapters-tab") << "rrNavChapters" << "rrChoices" << false << "rrNavChapters";
        QTest::newRow("chapters-close")
                << "rrNavChapters" << "rrChoices" << false << "rrContentsClose";
        QTest::newRow("stats") << "rrNavStats" << "rrChapterGraph" << false << "rrNavStats";
        QTest::newRow("text") << "rrNavText" << "rrMenu" << false << "rrNavText";
        QTest::newRow("layout") << "rrNavLayout" << "rrMenu" << false << "rrNavLayout";
        QTest::newRow("edited-text") << "rrNavText" << "rrMenu" << true << "rrNavText";
    }

    // Publishes dismissal only after the panel closes, including an applied appearance edit.
    void navigationDismissalWaitsForPanel()
    {
        QFETCH(QString, tabName);
        QFETCH(QString, panelName);
        QFETCH(bool, edit);
        QFETCH(QString, closeControl);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *tab = host.window()->findChild<QPushButton *>(tabName);
        auto *panel = host.window()->findChild<QWidget *>(panelName);
        QVERIFY(tab);
        QVERIFY(panel);
        // Switching from another panel must not request a dismissal refresh.
        auto *other = host.window()->findChild<QPushButton *>(
                tabName == "rrNavStats" ? "rrNavChapters" : "rrNavStats");
        QVERIFY(other);
        QVERIFY(click(canvas, other));
        QTRY_VERIFY(other->isChecked());
        QVERIFY(click(canvas, tab));
        QTRY_VERIFY(panel->isVisible() && tab->isChecked());
        QVERIFY(!host.window()->property("reader_navigation_dismissal").toBool());
        if (edit) {
            auto *larger = sizeStep(host);
            QVERIFY(larger);
            QTRY_VERIFY(larger->isVisible() && larger->isEnabled());
            QVERIFY(click(canvas, larger));
            QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                        && !inspectReader(host)["preview_busy"].toBool());
        }

        struct DismissalProbe : QObject
        {
            QWidget *panel = nullptr;
            QPushButton *tab = nullptr;
            bool premature = false;
            int publications = 0;

            bool eventFilter(QObject *object, QEvent *event) override
            {
                if (event->type() == QEvent::DynamicPropertyChange
                    && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()
                            == "reader_navigation_dismissal"
                    && object->property("reader_navigation_dismissal").toBool()) {
                    ++publications;
                    premature |= panel->isVisible() || tab->isChecked();
                }
                return false;
            }
        } probe;

        probe.panel = panel;
        probe.tab = tab;
        host.window()->installEventFilter(&probe);
        auto *close = host.window()->findChild<QPushButton *>(closeControl);
        QVERIFY(close);
        QVERIFY(click(canvas, close));
        QTRY_VERIFY(!panel->isVisible() && !tab->isChecked() && !host.session()->busy());
        QTRY_COMPARE(probe.publications, 1);
        QVERIFY(!probe.premature);
        // The desktop has no Kobo adapter to consume the one-shot property.
        host.window()->setProperty("reader_navigation_dismissal", false);
        QEvent update(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(host.window(), &update);
        QVERIFY(!host.window()->property("reader_navigation_dismissal").toBool());
        QCOMPARE(probe.publications, 1);
    }

    void uneditedPanelTransitions_data()
    {
        QTest::addColumn<QString>("start");
        QTest::newRow("text") << "rrNavText";
        QTest::newRow("layout") << "rrNavLayout";
        QTest::newRow("stats") << "rrNavStats";
        QTest::newRow("chapters") << "rrNavChapters";
    }

    void uneditedPanelTransitions()
    {
        QFETCH(QString, start);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        auto *toolbar = host.window()->findChild<QWidget *>("rrControlsTop");
        QVERIFY(page);
        QVERIFY(toolbar);
        if (start == "rrNavStats") {
            auto *position = host.window()->findChild<QLabel *>("rrGraphChapterPosition");
            QVERIFY(position);
            const auto hiddenText = position->text();
            const auto previousPosition = host.session()->state().value("position");
            const bool rtl = host.session()->state().value("page_progression_rtl").toBool();
            QVERIFY(clickAt(canvas,
                            page->mapTo(canvas.surface(),
                                        QPoint(rtl ? page->width() / 6 : page->width() * 5 / 6,
                                               page->height() / 2))));
            QTRY_VERIFY(!host.session()->busy()
                        && host.session()->state().value("position") != previousPosition);
            QCoreApplication::processEvents();
            QCOMPARE(position->text(), hiddenText);
        }
        const auto image = page->image.cacheKey();
        const auto panelState = [&] {
            auto state = host.session()->state();
            // Background estimates update page counts and progress without drawing a page.
            for (const auto *key :
                 { "cached_pages", "page_estimate", "book_fraction", "main_text_first",
                   "main_text_last", "rough_page", "rough_pages", "rough_item_page",
                   "rough_item_pages", "rough_pages_estimated", "rough_item_pages_estimated",
                   "whole_book_ready", "whole_book_failed" })
                state.remove(QLatin1String(key));
            return state;
        };
        const auto state = panelState();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(toolbar->isVisible());

        const QStringList tabs = { "rrNavText", "rrNavLayout", "rrNavLight", "rrNavStats",
                                   "rrNavChapters" };
        const QStringList panels = { "rrMenu", "rrMenu", "rrLightPanel", "rrChapterGraph",
                                     "rrChoices" };
        auto *startButton = host.window()->findChild<QPushButton *>(start);
        QVERIFY(startButton);
        QVERIFY(click(canvas, startButton));
        const int startIndex = tabs.indexOf(start);
        QVERIFY(startIndex >= 0);
        auto *startPanel = host.window()->findChild<QWidget *>(panels[startIndex]);
        QVERIFY(startPanel);
        QTRY_VERIFY(startPanel->isVisible());
        for (int index = 0; index < tabs.size(); ++index) {
            if (tabs[index] == "rrNavLight")
                continue;
            auto *button = host.window()->findChild<QPushButton *>(tabs[index]);
            auto *destination = host.window()->findChild<QWidget *>(panels[index]);
            QVERIFY(button);
            QVERIFY(destination);
            QVERIFY2(button->isVisible() && button->isEnabled(), qPrintable(tabs[index]));
            QVERIFY(click(canvas, button));
            QVERIFY2(!host.session()->busy(), qPrintable(tabs[index]));
            if (tabs[index] == start) {
                QTRY_VERIFY(!startPanel->isVisible());
            } else {
                QTRY_VERIFY(destination->isVisible());
                if (tabs[index] != "rrNavText" && tabs[index] != "rrNavLayout")
                    QVERIFY(!startPanel->isVisible());
            }
            QTRY_VERIFY(!host.session()->busy());
            QCOMPARE(panelState(), state);
            QCOMPARE(page->image.cacheKey(), image);
            QVERIFY(toolbar->isVisible());
            QVERIFY2(startButton->isVisible() && startButton->isEnabled(), qPrintable(start));
            QVERIFY(click(canvas, startButton));
            QVERIFY2(!host.session()->busy(), qPrintable(start));
            QTRY_VERIFY(startPanel->isVisible());
            QVERIFY(!host.session()->busy());
            if (start == "rrNavStats") {
                QTRY_COMPARE(host.window()->findChild<QLabel *>("rrGraphChapterPosition")->text(),
                             strings::get("stats.page_of")
                                     .arg(host.session()->chapterPage())
                                     .arg(headerfooter::pageTotal(
                                             host.session()->chapterPages(),
                                             host.session()->bookPagesEstimated())));
            }
            QCOMPARE(page->image.cacheKey(), image);
            QVERIFY(toolbar->isVisible());
        }
        if (start == "rrNavText") {
            auto *font = host.window()->findChild<QPushButton *>("rrFont");
            auto *choices = host.window()->findChild<QWidget *>("rrChoices");
            QVERIFY(font);
            QVERIFY(choices);
            QVERIFY(click(canvas, font));
            QTRY_VERIFY(choices->isVisible());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
            QTRY_VERIFY(startPanel->isVisible() && !choices->isVisible());
            QCOMPARE(page->image.cacheKey(), image);
            QVERIFY(toolbar->isVisible());
        }
    }

    // Checks that Light replaces the default view, its tab restores that view, and outside taps return to reading.
    void lightReplacesDefaultMenu_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("menuView");
        QTest::addColumn<QString>("action");
        for (bool dark : { false, true })
            for (const auto *view : { "none", "seek", "stats" })
                for (const auto *action : { "tab", "outside", "Text", "Layout", "Chapters",
                                            "Settings", "Seek", "Stats", "dismiss" }) {
                    if ((QString(action) == "Seek" && QString(view) == "seek")
                        || (QString(action) == "Stats" && QString(view) == "stats"))
                        continue;
                    QTest::newRow(qPrintable(
                            QString("%1-%2-%3").arg(view, dark ? "dark" : "light", action)))
                            << dark << QString::fromLatin1(view) << QString::fromLatin1(action);
                }
    }

    void lightReplacesDefaultMenu()
    {
        QFETCH(bool, dark);
        QFETCH(QString, menuView);
        QFETCH(QString, action);

        class Light final : public LightControls
        {
        public:
            bool initialize() override { return true; }

            bool hasWarmth() const override { return false; }

            int warmth() const override { return 0; }

            int warmthMinimum() const override { return 0; }

            int warmthMaximum() const override { return 0; }

            int warmthStep() const override { return 1; }

            bool automaticWarmth() const override { return false; }

            int setWarmth(int) override { return -1; }

            bool saveWarmth() override { return false; }

            bool setAutomaticWarmth(bool) override { return false; }

            bool beginGesture() override { return false; }

            int setGestureLevel(int) override { return -1; }

            void finishGesture(bool) override { }

            int brightness() const override { return 50; }

            bool gesturing() const override { return false; }
        };

        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(),
                                   { { "dark_mode", dark }, { "default_menu_view", menuView } }));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [](ReaderHostServices &services) {
            services.lightAvailable = [] { return true; };
            services.light = [](QWidget *) { return std::make_unique<Light>(); };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        auto *toolbar = host.window()->findChild<QWidget *>("rrControlsTop");
        auto *stats = host.window()->findChild<QWidget *>("rrChapterGraph");
        auto *seek = dynamic_cast<ui::PageScrubber *>(
                host.window()->findChild<QWidget *>("rrPageScrubber"));
        auto *light = host.window()->findChild<QWidget *>("rrLightPanel");
        auto *tab = host.window()->findChild<QPushButton *>("rrNavLight");
        QVERIFY(page && toolbar && stats && seek && light && tab);
        const auto position = host.session()->state().value("position");
        const auto image = page->image.cacheKey();
        const QPoint paper(nativeSize.width() / 2, nativeSize.height() / 4);
        QVERIFY(clickAt(canvas, paper));
        QTRY_VERIFY(toolbar->isVisible());
        QCOMPARE(stats->isVisible(), menuView == "stats");
        if (menuView == "seek")
            QTRY_VERIFY(seek->previewReady());

        class DefaultShows final : public QObject
        {
        public:
            int count = 0;

            bool eventFilter(QObject *, QEvent *event) override
            {
                if (event->type() == QEvent::Show)
                    ++count;
                return false;
            }
        } defaultShows;

        QVERIFY(click(canvas, tab));
        QTRY_VERIFY(light->isVisible());
        QVERIFY(!stats->isVisible());
        QVERIFY(!seek->isVisible());
        QVERIFY(tab->isChecked());
        QVERIFY(toolbar->isVisible());
        if (menuView == "stats")
            stats->installEventFilter(&defaultShows);
        else if (menuView == "seek") {
            // Both the chapter bar and the whole-book slider show this label.
            auto *range = seek->findChild<QWidget *>("rrScrubberRange");
            QVERIFY(range);
            range->installEventFilter(&defaultShows);
        }
        if (action == "tab")
            QVERIFY(click(canvas, tab));
        else if (action == "outside")
            QVERIFY(clickAt(canvas, paper));
        else if (action == "dismiss") {
            // Exercise full-menu dismissal directly through the page action,
            // independently of the outside-Light event filter.
            QVERIFY(page->tapped);
            page->tapped(paper);
        } else {
            const auto name =
                    action == "Settings" ? QString("rrSettings") : QString("rrNav%1").arg(action);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(name)));
        }
        QTRY_VERIFY(!light->isVisible());
        QVERIFY(!tab->isChecked());
        const bool restoring = action == "tab";
        if (restoring) {
            QTRY_COMPARE(stats->isVisible(), menuView == "stats");
            QTRY_COMPARE(seek->isVisible() && seek->barBounds().height() > 0, menuView == "seek");
            if (menuView == "seek")
                QTRY_VERIFY(seek->previewReady());
            QCOMPARE(defaultShows.count, menuView == "none" ? 0 : 1);
            QVERIFY(toolbar->isVisible());
        } else {
            QCOMPARE(defaultShows.count, 0);
            QTRY_COMPARE(stats->isVisible(), action == "Stats");
            QTRY_COMPARE(seek->isVisible() && seek->barBounds().height() > 0, action == "Seek");
            if (action == "dismiss" || action == "outside") {
                QVERIFY(!toolbar->isVisible());
                QVERIFY(!host.window()->findChild<QWidget *>("rrBottomNavigation")->isVisible());
                QVERIFY(clickAt(canvas, paper));
                QTRY_VERIFY(toolbar->isVisible());
                QTRY_COMPARE(stats->isVisible(), menuView == "stats");
                QTRY_COMPARE(seek->isVisible() && seek->barBounds().height() > 0,
                             menuView == "seek");
                QCOMPARE(defaultShows.count, menuView == "none" ? 0 : 1);
            } else {
                const auto panel = action == "Chapters" ? "rrChoices"
                        : action == "Settings"          ? "rrGeneralSettings"
                                                        : "rrMenu";
                if (action == "Text" || action == "Layout" || action == "Chapters"
                    || action == "Settings")
                    QTRY_VERIFY(host.window()->findChild<QWidget *>(panel)->isVisible());
                QVERIFY(toolbar->isVisible());
            }
        }
        QCOMPARE(host.session()->state().value("position"), position);
        QCOMPARE(page->image.cacheKey(), image);
        QCOMPARE(storage::readJson(paths.preferences()).value("default_menu_view").toString(),
                 menuView);
    }

    void appearanceApplyOnNavigation_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("panel");
        QTest::newRow("stats") << "rrNavStats" << "rrChapterGraph";
        QTest::newRow("chapters") << "rrNavChapters" << "rrChoices";
        QTest::newRow("search") << "rrSearchOpen" << "rrSearch";
        QTest::newRow("bookmarks") << "rrBookmarks" << "rrBookmarkList";
        QTest::newRow("seek") << "rrNavSeek" << "rrPageScrubber";
    }

    void appearanceApplyOnNavigation()
    {
        QFETCH(QString, action);
        QFETCH(QString, panel);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *larger = sizeStep(host);
        QVERIFY(larger);
        QTRY_VERIFY(larger->isVisible() && larger->isEnabled());
        QVERIFY(click(canvas, larger));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                    && !inspectReader(host)["preview_busy"].toBool());
        const auto textDraft = inspectReader(host)["preview_settings"].toObject();
        QVERIFY(textDraft["font_pixels"] != original["font_pixels"]);
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
        auto *layout = host.window()->findChild<QPushButton *>("rrNavLayout");
        QVERIFY(layout);
        QVERIFY(click(canvas, layout));
        auto *wider = host.window()->findChild<QPushButton *>("rrWider");
        QVERIFY(wider);
        QTRY_VERIFY(wider->isVisible() && wider->isEnabled());
        for (const auto *name : { "rrMargins", "rrVerticalMargins" }) {
            auto *slider = host.window()->findChild<QSlider *>(name);
            QVERIFY(slider);
            QCOMPARE(slider->minimum(), 0);
            QCOMPARE(slider->maximum(), 120);
            QCOMPARE(slider->singleStep(), 3);
        }
        QCOMPARE(inspectReader(host)["preview_settings"].toObject()["font_pixels"],
                 textDraft["font_pixels"]);
        QVERIFY(click(canvas, wider));
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        auto *verticalMarginsMore =
                host.window()->findChild<QPushButton *>("rrVerticalMarginsMore");
        QVERIFY(verticalMarginsMore);
        QVERIFY(click(canvas, verticalMarginsMore));
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        QVERIFY(draft["margin_pixels"] != original["margin_pixels"]);
        QVERIFY(draft["top_margin_pixels"] != original["top_margin_pixels"]);
        QVERIFY(draft["bottom_margin_pixels"] != original["bottom_margin_pixels"]);
        QCOMPARE(draft["top_margin_pixels"], draft["bottom_margin_pixels"]);
        QCOMPARE(host.session()->state()["top_margin_pixels"], original["top_margin_pixels"]);
        QCOMPARE(host.session()->state()["bottom_margin_pixels"], original["bottom_margin_pixels"]);
        auto *destination = host.window()->findChild<QPushButton *>(action);
        QVERIFY(destination);
        QTRY_VERIFY(destination->isEnabled());
        QVERIFY(click(canvas, destination));
        auto *target = host.window()->findChild<QWidget *>(panel);
        QVERIFY(target);
        QTRY_VERIFY_WITH_TIMEOUT(target->isVisible() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
        QCOMPARE(host.session()->state()["margin_pixels"], draft["margin_pixels"]);
        QCOMPARE(host.session()->state()["top_margin_pixels"], draft["top_margin_pixels"]);
        QCOMPARE(host.session()->state()["bottom_margin_pixels"], draft["bottom_margin_pixels"]);
        QTRY_VERIFY(!inspectReader(host)["preview_ready"].toBool());
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
        QCOMPARE(host.session()->state()["margin_pixels"], draft["margin_pixels"]);
        QCOMPARE(host.session()->state()["top_margin_pixels"], draft["top_margin_pixels"]);
        QCOMPARE(host.session()->state()["bottom_margin_pixels"], draft["bottom_margin_pixels"]);
    }

    void appearanceRelayoutPreservesPassage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto paths = testPaths();
        paths.directory = directory.path();
        paths.startup = paths.directory + "/startup-session";
        paths.active = paths.directory + "/active-session.json";
        paths.stateReport = paths.directory + "/state.json";
        paths.sessionReport = paths.directory + "/session.json";
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto inspect = [&] {
            host.window()->checkpoint();
            return storage::readJson(paths.stateReport);
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *noticeMessage = host.window()->findChild<QLabel *>("rrUpdatingText");
        QVERIFY(noticeMessage);
        auto *notice = noticeMessage->parentWidget();
        while (notice && !dynamic_cast<ui::ModalOverlay *>(notice))
            notice = notice->parentWidget();
        QVERIFY(notice);
        ui::ModalOverlay reference(host.window());
        reference.resize(notice->size());
        reference.setDimmed(true);
        const auto backdrop = [](QWidget *widget) {
            QImage image(32, 32, QImage::Format_ARGB32);
            image.fill(Qt::white);
            widget->render(&image, QPoint(), QRegion(QRect(0, 0, 32, 32)),
                           QWidget::DrawWindowBackground);
            return image;
        };
        QCOMPARE(backdrop(notice), backdrop(&reference));
        reference.setDimmed(false);
        QVERIFY(backdrop(notice) != backdrop(&reference));
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        const auto first = host.session()->state().value("position").toString();
        QVERIFY(!first.isEmpty());
        const bool rtl = host.session()->state().value("page_progression_rtl").toBool();
        QVERIFY(clickAt(canvas,
                        page->mapTo(canvas.surface(),
                                    QPoint(rtl ? page->width() / 6 : page->width() * 5 / 6,
                                           page->height() / 2))));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy()
                                         && host.session()->state().value("position").toString()
                                                 != first,
                                 10000);
        const auto anchor = host.session()->state().value("position").toString();
        QVERIFY(!anchor.isEmpty());
        const auto menu = [&] {
            if (!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible())
                QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        };
        const auto preserved = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy(), 30000);
            // Reflow restores the canonical bookmark, rather than the new page start.
            QCOMPARE(host.session()->state().value("position").toString(), anchor);
        };
        const auto settingsToggle = [&](const char *name, const char *key) {
            menu();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            auto *option = host.window()->findChild<QCheckBox *>(name);
            QVERIFY(option);
            const bool before = option->isChecked();
            QVERIFY(click(canvas, option));
            QCOMPARE(option->isChecked(), !before);
            // Dark mode saves asynchronously. Wait before asking Settings to close.
            QTRY_VERIFY_WITH_TIMEOUT(option->isEnabled() && !host.session()->busy()
                                             && host.session()->state().value(key).toBool()
                                                     == !before,
                                     30000);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
            QTRY_VERIFY_WITH_TIMEOUT(
                    !host.window()->findChild<QWidget *>("rrGeneralSettings")->isVisible()
                            && !host.session()->busy()
                            && host.session()->state().value(key).toBool() == !before,
                    30000);
            preserved();
            QCOMPARE(host.session()->state().value(key).toBool(), !before);
        };
        // Weight controls are disabled while Bionic Reading is active.
        if (host.session()->state().value("bold_reading").toString() != "off") {
            menu();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            auto *option = host.window()->findChild<QCheckBox *>("rrBionicReading");
            QVERIFY(option && option->isChecked());
            QVERIFY(click(canvas, option));
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
            QTRY_VERIFY_WITH_TIMEOUT(
                    !host.session()->busy()
                            && host.session()->state().value("bold_reading").toString() == "off",
                    30000);
            preserved();
            QCOMPARE(host.session()->state().value("bold_reading").toString(), QString("off"));
        }
        const auto openText = [&] {
            menu();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
            QVERIFY(!host.session()->busy());
        };
        const auto apply = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(!inspect()["preview_busy"].toBool(), 30000);
            const auto expected = inspect()["preview_settings"].toObject();
            QVERIFY(!expected.isEmpty());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavStats")));
            auto *graph = host.window()->findChild<QWidget *>("rrChapterGraph");
            QVERIFY(graph);
            QTRY_VERIFY_WITH_TIMEOUT(graph->isVisible() && !host.session()->busy()
                                             && !inspect()["preview_ready"].toBool(),
                                     30000);
            for (const auto *key : { "font_face", "font_pixels", "weight_steps", "margin_pixels" })
                QCOMPARE(host.session()->state().value(key), expected.value(key));
            preserved();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavStats")));
        };
        openText();
        const auto oldFace = host.session()->state().value("font_face");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
        const QString targetFace = oldFace.toString() == "Libron"
                ? kobalt::test::alternateReadingFace()
                : QString("Libron");
        const QString targetLabel = oldFace.toString() == "Libron"
                ? kobalt::test::alternateReadingLabel()
                : QString("Libron");
        QPushButton *choice = nullptr;
        for (auto *label : host.window()->findChildren<QLabel *>())
            if (label->objectName().endsWith("Title") && label->text() == targetLabel)
                choice = qobject_cast<QPushButton *>(label->parentWidget());
        QVERIFY(choice);
        QVERIFY(choice->isVisible());
        QVERIFY(click(canvas, choice));
        QTRY_COMPARE_WITH_TIMEOUT(
                inspect()["preview_settings"].toObject().value("font_face").toString(), targetFace,
                30000);
        QTRY_VERIFY_WITH_TIMEOUT(!inspect()["preview_busy"].toBool(), 30000);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
        QTRY_VERIFY(host.window()->findChild<QSlider *>("rrTextSize")->isVisible());
        apply();
        QCOMPARE(host.session()->state().value("font_face").toString(), targetFace);
        QVERIFY(host.session()->state().value("font_face") != oldFace);
        openText();
        const auto oldPixels = host.session()->state().value("font_pixels");
        QVERIFY(click(canvas, sizeStep(host)));
        apply();
        QVERIFY(host.session()->state().value("font_pixels") != oldPixels);
        openText();
        auto *weight = host.window()->findChild<QSlider *>("rrFontWeight");
        QVERIFY(weight);
        const auto oldWeight = host.session()->state().value("weight_steps");
        QVERIFY(click(canvas,
                      host.window()->findChild<QPushButton *>(
                              weight->value() == weight->maximum() ? "rrLighter" : "rrHeavier")));
        apply();
        QVERIFY(host.session()->state().value("weight_steps") != oldWeight);
        menu();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLayout")));
        QVERIFY(!host.session()->busy());
        auto *margin = host.window()->findChild<QSlider *>("rrMargins");
        QVERIFY(margin);
        const auto oldMargin = host.session()->state().value("margin_pixels");
        QVERIFY(click(canvas,
                      host.window()->findChild<QPushButton *>(
                              margin->value() == margin->maximum() ? "rrNarrower" : "rrWider")));
        apply();
        QVERIFY(host.session()->state().value("margin_pixels") != oldMargin);
        menu();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *bionic = host.window()->findChild<QCheckBox *>("rrBionicReading");
        QVERIFY(bionic && !bionic->isChecked());
        QVERIFY(click(canvas, bionic));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy()
                                         && host.session()->state().value("bold_reading").toString()
                                                 == "bold",
                                 30000);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
        preserved();
        QCOMPARE(host.session()->state().value("bold_reading").toString(), QString("bold"));
        settingsToggle("rrDarkMode", "dark_mode");
        settingsToggle("rrDarkMode", "dark_mode");
        for (const auto &size : { QSize(800, 1200), QSize(1400, 1000) }) {
            host.window()->setFixedSize(size);
            QTRY_COMPARE_WITH_TIMEOUT(inspect().value("viewport_width").toInt(), size.width(),
                                      10000);
            preserved();
        }
    }

    void overflowClosesCurrentPanel_data()
    {
        QTest::addColumn<QString>("target");
        for (const auto *target : { "text", "text-after-fonts", "fonts", "layout", "stats" })
            QTest::newRow(target) << QString::fromLatin1(target);
    }

    void overflowClosesCurrentPanel()
    {
        QFETCH(QString, target);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        const auto action = target == "layout" ? "rrNavLayout"
                : target == "light"            ? "rrNavLight"
                : target == "stats"            ? "rrNavStats"
                                               : "rrNavText";
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(action)));
        QVERIFY(!host.session()->busy());
        if (target == "fonts" || target == "text-after-fonts") {
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrChoices")->isVisible());
            if (target == "text-after-fonts")
                QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
        }
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrGeneralSettings")->isVisible());
        for (const auto *name : { "rrMenu", "rrChoices", "rrLightPanel", "rrChapterGraph",
                                  "rrBookmarkList", "rrSearch", "rrControlsBottom" }) {
            auto *panel = host.window()->findChild<QWidget *>(name);
            QVERIFY(panel);
            QVERIFY2(!panel->isVisible(), name);
        }
        QVERIFY(!host.window()->findChild<QPushButton *>("rrNavText")->isChecked());
        QVERIFY(!host.window()->findChild<QPushButton *>("rrNavLayout")->isChecked());
    }

    void appearanceSettingsRetainsDraft()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *larger = sizeStep(host);
        QVERIFY(larger);
        QTRY_VERIFY(larger->isEnabled());
        QVERIFY(click(canvas, larger));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                    && !inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QVERIFY(click(canvas, settings));
        auto *panel = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrMenu")->isVisible());
        QCOMPARE(inspectReader(host)["preview_settings"].toObject(), draft);
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
        QVERIFY(!text->isVisible());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsSystem")));
        auto *counter = host.window()->findChild<QLabel *>("rrSettingsOptionsPage");
        QVERIFY(counter && counter->isVisible());
        const auto settingsPage = counter->text();

        struct ResumeWorker
        {
            ~ResumeWorker() { engineActivity().pause(false); }
        } resumeWorker;

        engineActivity().pause(true);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
        QVERIFY(host.session()->busy() && panel->isVisible());
        host.window()->pageButton(1);
        QCOMPARE(counter->text(), settingsPage);
        host.window()->pageButton(-1);
        QCOMPARE(counter->text(), settingsPage);
        engineActivity().pause(false);
        QTRY_VERIFY_WITH_TIMEOUT(!panel->isVisible() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, text));
        QTRY_VERIFY_WITH_TIMEOUT(
                larger->isVisible() && larger->isEnabled() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
    }

    void appearanceWriteFailureRetainsDraft_data()
    {
        QTest::addColumn<bool>("exit");
        QTest::newRow("navigation-retry") << false;
        QTest::newRow("exit-retry") << true;
    }

    void appearanceWriteFailureRetainsDraft()
    {
        QFETCH(bool, exit);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *layout = host.window()->findChild<QPushButton *>("rrNavLayout");
        QVERIFY(layout);
        QVERIFY(click(canvas, layout));
        auto *wider = host.window()->findChild<QPushButton *>("rrWider");
        QVERIFY(wider);
        QTRY_VERIFY(wider->isEnabled());
        QVERIFY(click(canvas, wider));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                    && !inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        // Rotation is the Layout preference saved when the panel closes. The
        // test host records the turn without rotating the screen.
        auto *portrait = host.window()->findChild<QPushButton *>("rrOrientation1");
        auto *landscape = host.window()->findChild<QPushButton *>("rrOrientation2");
        QVERIFY(portrait);
        QVERIFY(landscape);
        QVERIFY(click(canvas, landscape->isChecked() ? portrait : landscape));
        const QString path = testPaths().preferences();
        QFile savedFile(path);
        const bool existed = savedFile.exists();
        QByteArray savedBytes;
        if (existed) {
            QVERIFY(savedFile.open(QIODevice::ReadOnly));
            savedBytes = savedFile.readAll();
            savedFile.close();
            QVERIFY(savedFile.remove());
        }
        QVERIFY(QDir().mkpath(path));

        struct RestoreFile
        {
            QString path;
            bool existed;
            QByteArray bytes;

            ~RestoreFile()
            {
                if (QFileInfo(path).isDir())
                    QDir(path).removeRecursively();
                else
                    QFile::remove(path);
                if (existed) {
                    QFile file(path);
                    if (file.open(QIODevice::WriteOnly))
                        file.write(bytes);
                }
            }
        } restore{ path, existed, savedBytes };

        auto *search = host.window()->findChild<QPushButton *>("rrSearchOpen");
        QVERIFY(search);
        QVERIFY(click(canvas, search));
        auto *error = host.window()->findChild<QLabel *>("rrPreviewLabel");
        QVERIFY(error);
        QTRY_VERIFY(error->isVisible() && !error->text().isEmpty());
        QVERIFY(inspectReader(host)["preview_ready"].toBool());
        QCOMPARE(inspectReader(host)["preview_settings"].toObject(), draft);
        QCOMPARE(host.session()->state()["margin_pixels"], original["margin_pixels"]);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSearch")->isVisible());
        QVERIFY(click(canvas,
                      host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
        if (exit) {
            host.window()->close();
            QVERIFY(!host.idle());
            QVERIFY(host.window()->isEnabled());
            QVERIFY(inspectReader(host)["preview_ready"].toBool());
            QVERIFY(click(canvas,
                          host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
        }
        QVERIFY(QDir(path).removeRecursively());
        if (exit) {
            QVERIFY(click(canvas, layout));
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
            host.open(fixture(), nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        } else {
            QVERIFY(click(canvas, search));
            QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrSearch")->isVisible()
                                             && !host.session()->busy(),
                                     10000);
        }
        QCOMPARE(host.session()->state()["margin_pixels"], draft["margin_pixels"]);
    }

    void appearanceSleepRetainsDraft()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *larger = sizeStep(host);
        QVERIFY(larger);
        QTRY_VERIFY(larger->isEnabled());
        QVERIFY(click(canvas, larger));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                    && !inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
        QCOMPARE(inspectReader(host)["preview_settings"].toObject(), draft);
        auto *search = host.window()->findChild<QPushButton *>("rrSearchOpen");
        QVERIFY(search);
        QTRY_VERIFY(search->isVisible());
        QVERIFY(click(canvas, search));
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrSearch")->isVisible()
                                         && !host.session()->busy(),
                                 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
    }

    void appearanceFailedPreviewDismisses_data()
    {
        QTest::addColumn<bool>("exit");
        QTest::newRow("navigation") << false;
        QTest::newRow("exit") << true;
    }

    void appearanceFailedPreviewDismisses()
    {
        QFETCH(bool, exit);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        // Revoke the real source lease after opening to fail the next preview deterministically.
        host.session()->revokeSource();
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        auto *step = sizeStep(host);
        QVERIFY(step);
        QVERIFY(click(canvas, step));
        auto *error = host.window()->findChild<QLabel *>("rrPreviewLabel");
        QVERIFY(error);
        QTRY_VERIFY_WITH_TIMEOUT(error->isVisible() && !error->text().isEmpty(), 10000);
        QVERIFY(!inspectReader(host)["preview_ready"].toBool());
        if (exit) {
            host.window()->close();
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        } else {
            auto *search = host.window()->findChild<QPushButton *>("rrSearchOpen");
            QVERIFY(search);
            QVERIFY(click(canvas, search));
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrSearch")->isVisible());
            QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
            QVERIFY(!inspectReader(host)["preview_busy"].toBool());
        }
    }

    void appearanceApplyFailureCanExit()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *step = sizeStep(host);
        QVERIFY(step);
        QTRY_VERIFY(step->isEnabled());
        QVERIFY(click(canvas, step));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                    && !inspectReader(host)["preview_busy"].toBool());
        // Reject a real engine update before Reader observes apply completion.
        // Revoking a source alone cannot fail a reflow of an already cached document.
        const auto productionUpdated = host.session()->updated;
        bool rejectUpdate = true;

        struct RestoreUpdated
        {
            QPointer<ReaderSession> session;
            std::function<void()> callback;

            ~RestoreUpdated()
            {
                if (session)
                    session->updated = callback;
            }
        } restore{ host.session(), productionUpdated };

        host.session()->updated = [&, productionUpdated] {
            if (rejectUpdate) {
                rejectUpdate = false;
                auto invalid = host.session()->state();
                invalid["font_pixels"] = 0;
                host.session()->editAppearance(kobalt::appearance::Patch(invalid));
            } else if (productionUpdated)
                productionUpdated();
        };
        auto *search = host.window()->findChild<QPushButton *>("rrSearchOpen");
        QVERIFY(search);
        QVERIFY(click(canvas, search));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy() && !host.session()->error().isEmpty(),
                                 10000);
        QVERIFY(host.window()->isEnabled());
        QVERIFY(!host.window()->findChild<QWidget *>("rrSearch")->isVisible());
        host.window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
    }

    void appearanceCloseDuringPreview_data()
    {
        QTest::addColumn<bool>("edit");
        QTest::addColumn<bool>("shutdown");
        QTest::newRow("exit-unedited") << false << false;
        QTest::newRow("exit-reflow") << true << false;
        QTest::newRow("shutdown-unedited") << false << true;
        QTest::newRow("shutdown-reflow") << true << true;
    }

    void appearanceCloseDuringPreview()
    {
        QFETCH(bool, edit);
        QFETCH(bool, shutdown);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        int expectedPixels = original["font_pixels"].toInt();
        if (edit) {
            QVERIFY(click(canvas, text));
            auto *larger = sizeStep(host);
            auto *size = host.window()->findChild<QSlider *>("rrTextSize");
            QVERIFY(larger);
            QVERIFY(size);
            QTRY_VERIFY(larger->isEnabled());
            larger->click();
            if (!shutdown)
                expectedPixels = halfPointPixels(size->value());
        } else
            text->click();
        if (edit)
            QVERIFY(inspectReader(host)["preview_busy"].toBool()
                    || inspectReader(host)["preview_queued"].toBool());
        if (shutdown)
            host.shutdown();
        else
            host.window()->close();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QCOMPARE(host.session()->state()["font_pixels"].toInt(), expectedPixels);
    }

    void appearanceSleepDuringFirstEdit()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        text->click();
        QVERIFY(!host.session()->busy());
        auto *step = sizeStep(host);
        QVERIFY(step);
        step->click();
        QVERIFY(inspectReader(host)["preview_busy"].toBool()
                || inspectReader(host)["preview_queued"].toBool());
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                    && !inspectReader(host)["preview_busy"].toBool());
        host.window()->wakeReader();
        auto *font = host.window()->findChild<QPushButton *>("rrFont");
        QVERIFY(font);
        QTRY_VERIFY(font->isVisible() && font->isEnabled());
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
    }

    // Physical Clara BW reports: a second drag right after the first did
    // nothing, and Overflow ignored taps while the preview rendered.

    void appearanceDragsDuringPreview_data()
    {
        QTest::addColumn<bool>("slow");
        QTest::newRow("preview-ready") << false;
        // A held engine worker stands in for the Clara BW, where the preview
        // is still opening and rendering while the finger moves.
        QTest::newRow("preview-rendering") << true;
    }

    void appearanceDragsDuringPreview()
    {
        QFETCH(bool, slow);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
        auto *weight = host.window()->findChild<QSlider *>("rrFontWeight");
        auto *size = host.window()->findChild<QSlider *>("rrTextSize");
        auto *overflow = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(weight && size && overflow);
        QTRY_VERIFY(weight->isVisible());

        struct Release
        {
            ~Release() { engineActivity().pause(false); }
        } release;

        if (slow)
            engineActivity().pause(true);
        // Drag before the preview has opened, then drag again at once.
        QStringList disabled;
        const auto watch = [&] {
            for (auto *widget : { static_cast<QWidget *>(weight), static_cast<QWidget *>(size),
                                  static_cast<QWidget *>(overflow) })
                if (!widget->isEnabled() && !disabled.contains(widget->objectName()))
                    disabled.append(widget->objectName());
        };
        const int thickness = weight->value();
        drag(canvas, weight, thickness, thickness + 3, watch);
        watch();
        QCOMPARE(weight->value(), thickness + 3);
        drag(canvas, weight, thickness + 3, thickness + 1, watch);
        watch();
        QCOMPARE(weight->value(), thickness + 1);
        // Text size snaps to stops, so keep the value the finger lands on.
        const int halfPoints = size->value();
        // An earlier row may have saved a large size for this book.
        const bool smaller = halfPoints > (size->minimum() + size->maximum()) / 2;
        dragAcross(canvas, size, smaller ? .8 : .3, smaller ? .2 : .8, watch);
        watch();
        const int landed = size->value();
        QVERIFY(landed != halfPoints);
        QVERIFY2(disabled.isEmpty(), qPrintable(disabled.join(',')));
        // The latest values stay on the controls while the preview renders.
        QCoreApplication::processEvents();
        QCOMPARE(weight->value(), thickness + 1);
        QCOMPARE(size->value(), landed);
        // Overflow opens at once and keeps the pending edits.
        QVERIFY(click(canvas, overflow));
        auto *general = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(general);
        QTRY_VERIFY(general->isVisible());
        if (slow) {
            // The edits are still queued: closing Settings waits for the
            // preview instead of applying the unedited book settings.
            QVERIFY(!inspectReader(host)["preview_ready"].toBool());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
            QCoreApplication::processEvents();
            QVERIFY(general->isVisible());
            QVERIFY(!host.session()->busy());
            engineActivity().pause(false);
        } else
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
        QTRY_VERIFY_WITH_TIMEOUT(!general->isVisible() && !host.session()->busy()
                                         && !inspectReader(host)["appearance_panel_open"].toBool(),
                                 15000);
        QCOMPARE(host.session()->state()["weight_steps"].toInt(), thickness + 1);
        QCOMPARE(host.session()->state()["font_pixels"].toInt(), kobalt::halfPointPixels(landed));
        QVERIFY(host.session()->state()["font_pixels"] != original["font_pixels"]);
    }

    // A thickness drag repaints only its slider row while the finger moves.
    // After release, the new page and the font sample share one paint pass,
    // so the display shows them in one update.

    void pageTurnsResumeWhenReaderIsShownAfterSleep()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        // wakeReader() shows the reader full screen. Open it at the screen
        // size so the layout stays the same across wake. The offscreen
        // plugin resizes a full screen window to the screen; minimal keeps
        // the current size.
        const QSize screenSize = QGuiApplication::primaryScreen()->geometry().size();
        host.open(fixture(), screenSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->setWindowFlags(Qt::Window);
        host.window()->setWindowModality(Qt::ApplicationModal);
        host.window()->showFullScreen();
        QTRY_COMPARE(host.window()->size(), screenSize);
        QTRY_COMPARE(QApplication::activeModalWidget(), host.window());
        QTRY_VERIFY(!host.session()->busy());
        host.session()->go(0);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 0);
        const auto position = [&] { return host.session()->state().value("position").toString(); };
        const auto start = position();
        QVERIFY(!start.isEmpty());
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && !position().isEmpty() && position() != start);
        const auto beforeSleep = position();
        host.window()->sleepReader();
        host.window()->pageButton(1);
        host.window()->pageButton(-1);
        QCOMPARE(position(), beforeSleep);
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QVERIFY(host.window()->isVisible());
        QTRY_COMPARE(host.window()->size(), screenSize);
        test::skipPageTurnWindow();
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && !position().isEmpty() && position() != beforeSleep);
        test::skipPageTurnWindow();
        host.window()->pageButton(-1);
        QTRY_VERIFY(!host.session()->busy() && position() == beforeSleep);
    }

    void closeWithPendingBookmark()
    {
        // Other tests can leave bookmarks that resolve to this page after reflow.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        storage::Paths paths{ root.path() };
        paths.startup = root.path() + "/startup-session";
        paths.active = root.path() + "/active-session.json";
        paths.stateReport = root.path() + "/state.json";
        paths.sessionReport = root.path() + "/session.json";
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        // A page turn resolves the bookmark pages for this layout.
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QTRY_VERIFY(!page->bookmarked);
        const auto corner = page->mapTo(canvas.surface(), page->bookmarkCorner().center());
        // A full add and remove proves that bookmark pages are resolved, so the
        // next tap is accepted instead of refused.
        QVERIFY(clickAt(canvas, corner));
        QTRY_VERIFY(page->bookmarked);
        QVERIFY(clickAt(canvas, corner));
        QTRY_VERIFY(!page->bookmarked);

        // Hold the worker so the click cannot finish adding the bookmark before
        // shutdown. Releasing it after shutdown must drop the reply safely.
        struct Release
        {
            ~Release() { engineActivity().pause(false); }
        } release;

        engineActivity().pause(true);
        QVERIFY(clickAt(canvas, corner));
        QVERIFY(!page->bookmarked);
        host.shutdown();
        engineActivity().pause(false);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(host.idle());
        QVERIFY(!host.window());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
    }

    void pageTurnTimingIgnoresNoopNavigation()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(0));
        QTRY_VERIFY(!host.session()->busy());
        QTemporaryDir logs;
        initializeDiagnostics(logs.path());
        pauseDiagnostics(false);
        QJsonObject input, painted;
        setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event") == "page-turn-input")
                input = event;
            if (event.value("event") == "page-turn-painted")
                painted = event;
        });

        struct ResetDiagnostics
        {
            ~ResetDiagnostics()
            {
                setDiagnosticObserver({});
                pauseDiagnostics(true);
            }
        } resetDiagnostics;

        const auto start = host.session()->state().value("position").toString();
        QVERIFY(!start.isEmpty());
        host.window()->pageButton(-1);
        QVERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), start);
        // Real idle time before the input that turns: the diagnostics clocks
        // measure it and the check below must exclude it. 100 ms is well
        // above the 20 ms tolerance. The skip passes the duplicate-turn filter.
        test::skipPageTurnWindow();
        QTest::qWait(100);
        host.window()->pageButton(1);
        QTRY_VERIFY(!painted.isEmpty());
        const auto next = host.session()->state().value("position").toString();
        QVERIFY(!next.isEmpty() && next != start);
        // Both values use monotonic clocks. Rendering speed does not affect
        // this comparison; idle time before the real input must be excluded.
        const auto inputToPaint =
                painted.value("monotonic_ms").toDouble() - input.value("monotonic_ms").toDouble();
        QVERIFY(painted.value("elapsed_ms").toDouble() <= inputToPaint + 20);
    }

    void pageButtonsAfterTouchUseDeliveryTime()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        // Keep the input sequence in one chapter so its local page numbers are consecutive.
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(0));
        QTRY_VERIFY(!host.session()->busy());
        QTemporaryDir logs;
        initializeDiagnostics(logs.path());
        pauseDiagnostics(false);
        QJsonObject input;
        setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event") == "page-turn-input")
                input = event;
        });

        struct ResetDiagnostics
        {
            ~ResetDiagnostics()
            {
                setDiagnosticObserver({});
                pauseDiagnostics(true);
            }
        } resetDiagnostics;

        auto *page = host.window()->findChild<QWidget *>("rrPage");
        QVERIFY(page);
        QTest::mouseClick(page, Qt::LeftButton, Qt::NoModifier,
                          QPoint(page->width() * 9 / 10, page->height() / 2));
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
        QVERIFY(input.value("source_ms").toDouble() > 0);
        test::skipPageTurnWindow();
        // Host callbacks, including Android buttons, have no Qt source time.
        host.window()->pageButton(1);
        QCOMPARE(input.value("source_ms").toDouble(), 0.0);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 2);
        test::skipPageTurnWindow();
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 3);
    }

    // Checks both page navigation and reflow while a contact is held.
    void pageFrameDuringHold_data()
    {
        QTest::addColumn<bool>("reflow");
        QTest::addColumn<bool>("samePosition");
        QTest::addColumn<bool>("backward");
        QTest::addColumn<bool>("touch");
        for (bool touch : { false, true }) {
            const QByteArray prefix = touch ? "touch-" : "mouse-";
            QTest::newRow((prefix + "page").constData()) << false << false << false << touch;
            QTest::newRow((prefix + "layout").constData()) << true << false << false << touch;
            QTest::newRow((prefix + "same-position-forward").constData())
                    << false << true << false << touch;
            QTest::newRow((prefix + "same-position-backward").constData())
                    << false << true << true << touch;
        }
    }

    // Checks that a new page or layout cannot turn an overlapping long hold into another page turn.
    void pageFrameDuringHold()
    {
        QFETCH(bool, reflow);
        QFETCH(bool, samePosition);
        QFETCH(bool, backward);
        QFETCH(bool, touch);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto files = test::engineBook();
        const QByteArray paragraphs = samePosition
                ? "<p>" + QByteArray("alpha ").repeated(20)
                        + "</p><div style=\"padding-bottom:4000px\">t</div><p>"
                        + QByteArray("reader ").repeated(60) + "</p>"
                : QByteArray("<p>Each page must have a distinct text position.</p>").repeated(320);
        files["OEBPS/c1.xhtml"] = test::publisherHtml(paragraphs);
        QVERIFY(test::writeBook(directory.path(), "held-frame.epub", files));
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(directory.filePath("held-frame.epub"), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QVERIFY(host.session()->pages() > 4);
        const int origin = samePosition && !backward ? 1 : 2;
        QVERIFY(host.session()->go(origin));
        QTRY_VERIFY(!host.session()->busy());
        const QPoint point(page->width() * 9 / 10, page->height() / 2);
        auto *device = QTest::createTouchDevice();
        const auto send = [&](QEvent::Type type, ulong timestamp) {
            if (touch) {
                const bool down = type == QEvent::MouseButtonPress;
                QEventPoint contact(0, down ? QEventPoint::Pressed : QEventPoint::Released, point,
                                    page->mapToGlobal(point));
                QMutableEventPoint::setPosition(contact, point);
                QTouchEvent event(down ? QEvent::TouchBegin : QEvent::TouchEnd, device,
                                  Qt::NoModifier, { contact });
                event.setTimestamp(timestamp);
                QApplication::sendEvent(page, &event);
                return;
            }
            QMouseEvent event(type, point, page->mapToGlobal(point), Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            event.setTimestamp(timestamp);
            QApplication::sendEvent(page, &event);
        };
        int imageLookups = 0;
        const auto imageHeld = page->imageHeld;
        page->imageHeld = [&](QPoint point) {
            ++imageLookups;
            return imageHeld(point);
        };
        const auto position = host.session()->state().value("position");
        const auto image = page->image.cacheKey();
        const auto layout = host.session()->layoutRevision();
        const auto previousFrame = host.session()->image().cacheKey();
        const auto publish = host.session()->updated;
        const auto restore = qScopeGuard([&] { host.session()->updated = publish; });
        bool pressed = false;
        host.session()->updated = [&] {
            // Deliver the press just before the replacement frame reaches the reader.
            // Rendering can take longer than the hold timer under instrumentation.
            if (!pressed && host.session()->image().cacheKey() != previousFrame) {
                pressed = true;
                send(QEvent::MouseButtonPress, 1000);
            }
            publish();
        };
        if (reflow) {
            auto appearance = host.session()->state();
            appearance["font_pixels"] = appearance.value("font_pixels").toInt() + 4;
            host.session()->editAppearance(kobalt::appearance::Patch(appearance));
        } else {
            QVERIFY(host.session()->go(samePosition ? (backward ? 1 : 2) : 3));
        }
        QTRY_VERIFY(pressed && !host.session()->busy() && page->image.cacheKey() != image);
        QCOMPARE(host.session()->layoutRevision() != layout, reflow);
        const auto destination = host.session()->page();
        if (!reflow)
            QCOMPARE(destination, samePosition ? (backward ? 1 : 2) : 3);
        if (samePosition)
            QCOMPARE(host.session()->state().value("position"), position);
        send(QEvent::MouseButtonRelease, 1709);
        QVERIFY(!host.session()->busy());
        QCOMPARE(imageLookups, 0);
        QCOMPARE(host.session()->page(), destination);
        QVERIFY(!host.window()->findChild<QWidget *>("rrDictionary")->isVisible());
        // A fresh short tap still turns the page, with no duplicate-filter masking.
        send(QEvent::MouseButtonPress, 2000);
        send(QEvent::MouseButtonRelease, 2085);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == destination + 1);
    }

    // Checks decoded-image frames both before hold recognition and while the word-selection reply is queued.
    void pageImageDuringHold_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : { "before-threshold", "token-pending", "geometry-pending",
                                   "dictionary-open", "selection-open", "opening-estimates" })
            QTest::newRow(stage) << QString(stage);
    }

    // Checks that a deferred image keeps the first lookup on unchanged text and never navigates.
    void pageImageDuringHold()
    {
        QFETCH(QString, stage);
        const bool openingEstimates = stage == "opening-estimates";
        if (openingEstimates)
            renderingMode::setOverride(1);
        const auto restoreMode = qScopeGuard([] { renderingMode::setOverride(-1); });
        const bool pendingSelection = stage == "token-pending";
        const bool delayedPublication = stage == "geometry-pending" || stage == "dictionary-open"
                || stage == "selection-open";
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto files = test::engineBook(true);
        files["OEBPS/c1.xhtml"] = test::publisherHtml(
                "<p style=\"margin:0;text-indent:0\">" + QByteArray("reader ").repeated(35)
                + "</p><p><img src=\"colors.png\" width=\"128\" height=\"64\"/></p>");
        // Local resources defer images above 256 KiB. Noise keeps this PNG
        // above that threshold without a network service or a sleep.
        QImage image(400, 400, QImage::Format_RGB32);
        quint32 noise = 1;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                noise = noise * 1664525u + 1013904223u;
                image.setPixel(x, y, 0xff000000u | (noise & 0xffffffu));
            }
        files["OEBPS/colors.png"].clear();
        QBuffer buffer(&files["OEBPS/colors.png"]);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        QVERIFY(files["OEBPS/colors.png"].size() > 256 * 1024);
        QVERIFY(test::writeBook(directory.path(), "hold-image.epub", files));
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), { { "highlight_help_seen", true } }));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };

        struct ResumeEngine
        {
            ~ResumeEngine() { engineActivity().pause(false); }
        } resume;

        host.configureServices = [](ReaderHostServices &) { engineActivity().pause(true); };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(directory.filePath("hold-image.epub"), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QVERIFY(!host.session()->busy());
        QVERIFY(host.session()->resourceStats().value("pending_images").toInt() > 0);
        if (openingEstimates) {
            const auto position = host.session()->state().value("position");
            engineActivity().pause(false);
            QTRY_VERIFY_WITH_TIMEOUT(
                    host.session()->resourceStats().value("pending_images").toInt() == 0
                            && host.session()->resourceStats().value("ready_images").toInt() > 0
                            && host.session()->totalWords() > 0,
                    10000);
            QCOMPARE(host.session()->state().value("position"), position);
            return;
        }
        const auto imageKey = page->image.cacheKey();
        const auto sessionImageKey = host.session()->image().cacheKey();
        const auto layout = host.session()->layoutRevision();
        const auto position = host.session()->state().value("position");
        const QPoint point(page->width() / 2, page->topHeight() + 100);
        const auto send = [&](QEvent::Type type, ulong timestamp) {
            QMouseEvent event(type, point, page->mapToGlobal(point), Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            event.setTimestamp(timestamp);
            QApplication::sendEvent(page, &event);
        };
        auto *dictionary = host.window()->findChild<QWidget *>("rrDictionary");
        QVERIFY(dictionary);
        bool started = false;
        const auto drag = page->annotationDragStarted;
        page->annotationDragStarted = [&](QPoint point, int boundary) {
            started = true;
            return drag(point, boundary);
        };
        const auto publish = host.session()->updated;

        struct RestoreNotification
        {
            ReaderSession *session;
            std::function<void()> previous;

            ~RestoreNotification() { session->updated = previous; }
        } restoreNotification{ host.session(), publish };

        bool tokenBoundaryReached = false, imageBeforeToken = false;
        if (pendingSelection) {
            host.session()->updated = [&] {
                if (host.session()->image().cacheKey() != sessionImageKey && !tokenBoundaryReached)
                    imageBeforeToken = true;
                publish();
            };
            // This marker is queued before the held contact requests its token.
            // Publishing before the marker therefore proves the token is pending.
            host.session()->annotationTokenAt(
                    {}, [&](AnnotationToken) { tokenBoundaryReached = true; });
        }
        QRect sheet, firstHandle, lastHandle;
        if (delayedPublication) {
            const auto releaseFrame = [&] {
                host.session()->updated = publish;
                publish();
            };
            if (stage == "selection-open")
                page->annotationRequested();
            // Keep the decoded frame off screen until the chosen input stage.
            // An estimate update can also publish the frame already in the session.
            host.session()->updated = [] {};
            engineActivity().pause(false);
            QTRY_VERIFY(host.session()->image().cacheKey() != sessionImageKey);
            QCOMPARE(page->image.cacheKey(), imageKey);
            engineActivity().pause(true);
            send(QEvent::MouseButtonPress, 1000);
            send(QEvent::MouseButtonRelease, 1709);
            QVERIFY(started);
            if (stage == "geometry-pending") {
                // A new draft changes the markup generation before its geometry
                // reply installs handles. Probe successive worker/GUI boundaries
                // until that observable state is reached, rather than counting replies.
                const auto generation = host.session()->markupGeneration();
                bool probed = false, geometryPending = false;
                const auto probe = [&](const auto &next, int remaining) -> void {
                    host.session()->annotationTokenAt({}, [&, next, remaining](AnnotationToken) {
                        if (host.session()->markupGeneration() == generation && remaining > 0) {
                            next(next, remaining - 1);
                            return;
                        }
                        geometryPending = host.session()->markupGeneration() > generation
                                && page->annotationHandleRect(true).isEmpty()
                                && page->annotationHandleRect(false).isEmpty()
                                && !page->annotationContains(point - QPoint(0, page->topHeight()))
                                && !dictionary->isVisible();
                        if (geometryPending)
                            releaseFrame();
                        probed = true;
                    });
                };
                probe(probe, 8);
                engineActivity().pause(false);
                QTRY_VERIFY(probed);
                QVERIFY2(geometryPending,
                         "Image must arrive after draft creation and before geometry");
            } else {
                engineActivity().pause(false);
                if (stage == "dictionary-open") {
                    QTRY_VERIFY(dictionary->isVisible());
                    sheet = dictionary->geometry();
                } else {
                    QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
                    page->tapped(point);
                    auto *actions = host.window()->findChild<QWidget *>("rrAnnotationPopover");
                    QVERIFY(actions);
                    QTRY_VERIFY(actions->isVisible());
                    auto *card = actions->findChild<QWidget *>("rrModalCard");
                    QVERIFY(card);
                    sheet = card->geometry();
                }
                firstHandle = page->annotationHandleRect(true);
                lastHandle = page->annotationHandleRect(false);
                QVERIFY(!firstHandle.isEmpty() && !lastHandle.isEmpty());
                QCOMPARE(page->image.cacheKey(), imageKey);
                releaseFrame();
            }
        } else {
            send(QEvent::MouseButtonPress, 1000);
            if (pendingSelection) {
                QTRY_VERIFY(started);
                send(QEvent::MouseButtonRelease, 1709);
            }
            engineActivity().pause(false);
        }
        QTRY_VERIFY(page->image.cacheKey() != imageKey);
        if (pendingSelection) {
            QTRY_VERIFY(tokenBoundaryReached);
            QVERIFY2(imageBeforeToken, "Image must arrive before the queued token boundary");
        }
        QCOMPARE(host.session()->layoutRevision(), layout);
        QCOMPARE(host.session()->state().value("position"), position);
        if (!delayedPublication && !pendingSelection)
            send(QEvent::MouseButtonRelease, 1709);
        if (stage == "selection-open") {
            auto *actions = host.window()->findChild<QWidget *>("rrAnnotationPopover");
            QVERIFY(actions->isVisible());
            auto *card = actions->findChild<QWidget *>("rrModalCard");
            QVERIFY(card);
            QCOMPARE(card->geometry(), sheet);
        } else {
            QTRY_VERIFY(dictionary->isVisible());
            QCOMPARE(host.window()->findChild<QLabel *>("rrDictionaryWord")->text(),
                     QString("reader"));
            if (stage == "dictionary-open")
                QCOMPARE(dictionary->geometry(), sheet);
        }
        if (!firstHandle.isEmpty()) {
            QCOMPARE(page->annotationHandleRect(true), firstHandle);
            QCOMPARE(page->annotationHandleRect(false), lastHandle);
        }
        QCOMPARE(host.session()->state().value("position"), position);
    }

    void pageGestureBurstIgnoresRapidTurns()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        // Keep the input sequence in one chapter so its local page numbers are consecutive.
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QVERIFY(host.session()->pages() > 6);
        QVERIFY(host.session()->go(2));
        QTRY_VERIFY(!host.session()->busy());
        const QPoint forward(page->width() * 9 / 10, page->height() / 2);
        const QPoint center(page->width() / 2, page->height() / 2);
        // Call completed gestures in one GUI turn. The worker may finish, but
        // its callback cannot run until every input has reached the reader.
        page->tapped(forward);
        QVERIFY(host.session()->busy());
        for (int i = 0; i < 100; ++i)
            page->tapped(forward);
        QTRY_COMPARE(host.session()->page(), 3);
        QTRY_VERIFY(!host.session()->busy());
        test::skipPageTurnWindow();
        page->swiped(1);
        page->swiped(1);
        page->swiped(-1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 4);
        test::skipPageTurnWindow();
        page->tapped(forward);
        page->tapped(forward);
        page->tapped(center);
        QTRY_COMPARE(host.session()->page(), 5);
        QTRY_VERIFY(!host.session()->busy());
        QTest::qWait(30);
        QCOMPARE(host.session()->page(), 5);
        // The rapid second turn was ignored; the menu tap still works.
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QTRY_VERIFY(settings->isVisible() && settings->isEnabled());
    }

    // Checks that the return header closes the book and that reopening it keeps the saved position.
    void exitThroughReaderAndReopen()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *exit = host.window()->findChild<QPushButton *>("rrReturn");
        QVERIFY(exit);
        QTRY_VERIFY(exit->isVisible() && exit->isEnabled());
        QCOMPARE(exit->accessibleName(), strings::get("reader.exit"));
        auto *returnTitle = host.window()->findChild<QLabel *>("rrReturnTitle");
        QVERIFY(returnTitle);
        QCOMPARE(returnTitle->text(), strings::get("reader.exit"));
        QVERIFY(click(canvas, exit));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(host.idle());
        QVERIFY(!host.session());
        QVERIFY(!host.window());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QVERIFY(host.session()->savedStateMatched());
        host.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 10000);
    }

    // Closing Settings guards the Settings button, which sits under the close
    // cell, against the second half of a double tap. The guard judges a tap by
    // when it was pressed, so a tap that only waited while a slow close held
    // the GUI still opens Settings. QTest gives each event a timestamp and puts
    // a double-click interval between two clicks.
    void closeGuardUsesTapTime()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *open = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(open);
        // Settings is built when it first opens.
        const auto settingsVisible = [&] {
            auto *settings = host.window()->findChild<QWidget *>("rrGeneralSettings");
            return settings && settings->isVisible();
        };
        auto *top = host.window()->findChild<QWidget *>("rrControlsTop");
        const auto closeSettings = [&] {
            if (!top->isVisible())
                QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
            QTRY_VERIFY(open->isVisible() && open->isEnabled());
            QVERIFY(click(canvas, open));
            QTRY_VERIFY(settingsVisible());
            auto *close = host.window()->findChild<QPushButton *>("rrSettingsClose");
            QVERIFY(close);
            // The close cell covers the Settings button.
            const auto closeArea = QRect(close->mapTo(host.window(), QPoint()), close->size());
            const auto openCenter = open->mapTo(host.window(), open->rect().center());
            QVERIFY(closeArea.contains(openCenter));
            QVERIFY(click(canvas, close));
            QTRY_VERIFY(!settingsVisible() && !top->isVisible());
        };
        closeSettings();
        // Reopen the menu, then give the Settings tap the timestamp of the
        // second half of a double tap on the close cell.
        const auto firstCloseTime = QTest::lastMouseTimestamp;
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(top->isVisible());
        QTest::lastMouseTimestamp = firstCloseTime - QTest::mouseDoubleClickInterval;
        QVERIFY(click(canvas, open));
        QTest::qWait(ui::metrics::contents::closeGuardMilliseconds + 100);
        QVERIFY(!settingsVisible());
        QVERIFY(top->isVisible());
        closeSettings();
        const auto secondCloseTime = QTest::lastMouseTimestamp;
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(top->isVisible());
        // Pressed half a second after the close, before the guard timer ends.
        QTest::lastMouseTimestamp = secondCloseTime;
        QVERIFY(click(canvas, open));
        QTRY_VERIFY(settingsVisible());
    }

    void staleSearchReplyIgnored()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *search = host.window()->findChild<QPushButton *>("rrSearchOpen");
        QVERIFY(search);
        QTRY_VERIFY(search->isVisible());
        QVERIFY(click(canvas, search));
        auto *query = host.window()->findChild<QLineEdit *>("rrSearchQuery");
        QVERIFY(query);
        QTRY_VERIFY(query->isVisible());
        // A book-wide search with many matches, replaced by one with none in
        // the same GUI turn, so no reply to the first can arrive in between.
        query->setText("chapter");
        emit query->returnPressed();
        query->setText("zqxjv");
        emit query->returnPressed();
        auto *status = host.window()->findChild<QLabel *>("rrSearchStatus");
        QVERIFY(status);
        const auto none = strings::get("search_panel.no_matches_found");
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), none, 10000);
        // The engine worker runs jobs in order. Once a later request answers,
        // every reply to the first search has been delivered or dropped.
        bool drained = false;
        host.session()->pageSummary(host.session()->page(),
                                    [&](QString, QString) { drained = true; });
        QTRY_VERIFY(drained);
        QCoreApplication::processEvents();
        QCOMPARE(status->text(), none);
        auto *row = host.window()->findChild<QPushButton *>("rrSearchResult0");
        QVERIFY(!row || !row->isVisible());
    }

    // Checks that a search highlight stays when an unedited panel opens and is gone after a text-size or margin change.
    void searchHighlightClearsOnReflow_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("layout");
        QTest::newRow("text-light") << false << false;
        QTest::newRow("text-dark") << true << false;
        QTest::newRow("layout-light") << false << true;
        QTest::newRow("layout-dark") << true << true;
    }

    void searchHighlightClearsOnReflow()
    {
        QFETCH(bool, dark);
        QFETCH(bool, layout);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        host.session()->setDarkMode(dark);
        QTRY_VERIFY(!host.session()->busy());
        QVector<SearchHit> hits;
        bool searched = false;
        host.session()->search("reader", [&](SearchBatch batch) {
            hits += batch.hits;
            searched = batch.done;
        });
        QTRY_VERIFY(searched);
        QVERIFY(!hits.isEmpty());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchOpen")));
        auto *query = host.window()->findChild<QLineEdit *>("rrSearchQuery");
        auto *status = host.window()->findChild<QLabel *>("rrSearchStatus");
        QVERIFY(query && status);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchScope")));
        QVERIFY(click(canvas, host.window()->findChild<QAbstractButton *>("rrSearchScope3")));
        query->setText("reader");
        emit query->returnPressed();
        QTRY_COMPARE(status->text(), strings::get("search_panel.search_complete"));
        auto *result = host.window()->findChild<QPushButton *>("rrSearchResult0");
        QTRY_VERIFY(result && result->isVisible());
        QVERIFY(click(canvas, result));
        QTRY_VERIFY(!host.session()->busy());
        bool drained = false;
        host.session()->pageSummary(host.session()->page(),
                                    [&](QString, QString) { drained = true; });
        QTRY_VERIFY(drained);
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QVector<QRect> rectangles;
        drained = false;
        host.session()->rangeRects(hits.first().start, hits.first().end,
                                   [&](QVector<QRect> result) {
                                       rectangles = result;
                                       drained = true;
                                   });
        QTRY_VERIFY(drained);
        QVERIFY(!rectangles.isEmpty());
        // Compare page pixels against the same frame with its temporary mark
        // removed. Opening an unedited panel must keep the original highlight.
        const auto marked = page->snapshot();
        page->mark({});
        QVERIFY(page->snapshot() != marked);
        page->mark(rectangles);
        QCOMPARE(page->snapshot(), marked);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *tab = host.window()->findChild<QPushButton *>(layout ? "rrNavLayout" : "rrNavText");
        QVERIFY(click(canvas, tab));
        QCOMPARE(page->snapshot(), marked);
        auto *step = layout ? host.window()->findChild<QPushButton *>("rrWider") : sizeStep(host);
        QTRY_VERIFY(step && step->isVisible() && step->isEnabled());
        QVERIFY(click(canvas, step));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool()
                    && !inspectReader(host)["preview_busy"].toBool());
        QVERIFY(page->image.cacheKey() != host.session()->image().cacheKey());
        const auto preview = page->snapshot();
        page->mark({});
        QCOMPARE(page->snapshot(), preview);
        QVERIFY(click(canvas, tab));
        QTRY_VERIFY(!host.session()->busy() && !inspectReader(host)["preview_ready"].toBool());
        const auto applied = page->snapshot();
        page->mark({});
        QCOMPARE(page->snapshot(), applied);
    }

    // Searches beyond both old caps and switches scope through the production controls.
    void searchScopesAndPagination()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        int referenceCount = 0;
        bool referenceDone = false;
        host.session()->search("e", [&](SearchBatch batch) {
            referenceCount += batch.hits.size();
            referenceDone = batch.done;
        });
        QTRY_VERIFY_WITH_TIMEOUT(referenceDone, 15000);
        QVERIFY(referenceCount > 1000);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchOpen")));
        auto *scopeStatus = host.window()->findChild<QLabel *>("rrSearchScopeStatus");
        QVERIFY(scopeStatus && scopeStatus->isVisible());
        QCOMPARE(host.window()->findChild<QWidget *>("rrSearchQuery")->y(), 0);
        QVERIFY(host.window()->findChild<QWidget *>("rrMenuClock")->isVisible());
        QVERIFY(host.window()->findChild<QWidget *>("rrMenuBattery")->isVisible());
        const auto *searchView = host.window()->findChild<QWidget *>("rrSearch");
        QVERIFY(searchView->y() > 0);
        // Center the visible caption ink, not the font's ascent/descent box.
        const auto scopeImage = scopeStatus->grab().toImage();
        int inkTop = scopeImage.height(), inkBottom = -1;
        for (int y = 0; y < scopeImage.height(); ++y)
            for (int x = 0; x < scopeImage.width(); ++x)
                if (qGray(scopeImage.pixel(x, y)) < 128) {
                    inkTop = qMin(inkTop, y);
                    inkBottom = qMax(inkBottom, y);
                }
        QVERIFY(inkBottom >= inkTop);
        QVERIFY(qAbs(inkTop - (scopeImage.height() - 1 - inkBottom)) <= 2);
        QCOMPARE(scopeStatus->text(),
                 strings::get("search_panel.now_searching")
                         .arg(strings::get("search_panel.whole_book")));
        auto *scope = host.window()->findChild<QPushButton *>("rrSearchScope");
        QVERIFY(scope && click(canvas, scope));
        auto *defaultScope = host.window()->findChild<QAbstractButton *>("rrSearchScope3");
        QVERIFY(defaultScope && defaultScope->isChecked());
        QVERIFY(click(canvas, host.window()->findChild<QAbstractButton *>("rrSearchScope3")));
        auto *query = host.window()->findChild<QLineEdit *>("rrSearchQuery");
        auto *status = host.window()->findChild<QLabel *>("rrSearchStatus");
        QVERIFY(query && status);
        query->setText("e");
        emit query->returnPressed();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("search_panel.search_complete"),
                                  15000);
        QCOMPARE(scopeStatus->text(),
                 strings::get("search_panel.matches_in_scope")
                         .arg(strings::get("search_panel.matches").arg(referenceCount),
                              strings::get("search_panel.whole_book")));
        auto *jump = host.window()->findChild<QPushButton *>("rrSearchPageJump");
        QVERIFY(jump && click(canvas, jump));
        auto *number = host.window()->findChild<QLineEdit *>("rrSearchPageNumber");
        auto *go = host.window()->findChild<QPushButton *>("rrSearchPageGo");
        QVERIFY(number && go);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchPageKey1")));
        QCOMPARE(number->text(), QString("2"));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchPageKey11")));
        QVERIFY(number->text().isEmpty());
        QVERIFY(!go->isEnabled());
        number->setText("12");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchPageKey9")));
        QVERIFY(number->text().isEmpty());
        number->setText("0");
        QVERIFY(!go->isEnabled());
        number->setText("999999");
        QVERIFY(!go->isEnabled());
        const auto *validator = static_cast<const QIntValidator *>(number->validator());
        const int lastPage = validator->top();
        number->setText(QString::number(lastPage));
        QVERIFY(go->isEnabled() && click(canvas, go));
        auto *last = host.window()->findChild<QPushButton *>(
                QString("rrSearchResult%1").arg(referenceCount - 1));
        QVERIFY(last && last->isVisible());
        QTRY_VERIFY(last->isEnabled());
        QVERIFY(!last->accessibleName().isEmpty());
        QCOMPARE(host.window()->findChild<QLabel *>("rrSearchPage")->text(),
                 strings::get("search_panel.result_pages").arg(lastPage).arg(lastPage));
        // A delayed reply for the previous results page cannot replace the current rows.
        auto *panel = dynamic_cast<SearchPanel *>(host.window()->findChild<QWidget *>("rrSearch"));
        QVERIFY(panel);
        const auto resolve = panel->detailsRequested;
        QVector<std::function<void()>> replies;
        panel->detailsRequested = [&](QVector<SearchHit> hits,
                                      std::function<void(QVector<SearchHit>)> finished) {
            resolve(hits, [&, finished](QVector<SearchHit> resolved) {
                replies.append([finished, resolved] { finished(resolved); });
            });
        };
        const auto lastDescription = last->accessibleName();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchPrevious")));
        QTRY_COMPARE(replies.size(), 1);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchNext")));
        QTRY_COMPARE(replies.size(), 2);
        last = host.window()->findChild<QPushButton *>(
                QString("rrSearchResult%1").arg(referenceCount - 1));
        QVERIFY(last && !last->isEnabled());
        replies[0]();
        QVERIFY(!last->isEnabled() && last->accessibleName().isEmpty());
        replies[1]();
        QVERIFY(last->isEnabled());
        QCOMPARE(last->accessibleName(), lastDescription);
        panel->detailsRequested = resolve;
        QVERIFY(click(canvas, scope));
        auto *whole = host.window()->findChild<QAbstractButton *>("rrSearchScope3");
        QVERIFY(whole && whole->isChecked());
        QVERIFY(click(canvas, host.window()->findChild<QAbstractButton *>("rrSearchScope1")));
        QCOMPARE(scopeStatus->text(),
                 strings::get("search_panel.matches_so_far_in_scope")
                         .arg(strings::get("search_panel.matches").arg(0),
                              strings::get("search_panel.up_to_here")));
        QTRY_VERIFY_WITH_TIMEOUT(status->text() != strings::get("search_panel.searching"), 10000);
        query->clear();
        for (int id : { 0, 1, 2, 3 }) {
            QVERIFY(click(canvas, scope));
            auto *option =
                    host.window()->findChild<QAbstractButton *>(QString("rrSearchScope%1").arg(id));
            auto *icon =
                    host.window()->findChild<QWidget *>(QString("rrSearchScopeIcon%1").arg(id));
            QVERIFY(option && icon && icon->isVisible());
            QVERIFY(icon->geometry().center().x() > option->width() / 2);
            const QSize iconSize = icon->size();
            const auto expected = icon->grab().toImage().convertToFormat(QImage::Format_RGB32);
            QVERIFY(click(canvas, option));
            const auto actual = scope->grab()
                                        .toImage()
                                        .copy((scope->width() - iconSize.width()) / 2,
                                              (scope->height() - iconSize.height()) / 2,
                                              iconSize.width(), iconSize.height())
                                        .convertToFormat(QImage::Format_RGB32);
            QCOMPARE(actual, expected);
        }
        // Progress arrivals and incidental redraws must not churn the pagination counter.
        host.session()->cancelSearch();
        SearchBatch progress;
        progress.reset = true;
        progress.hits.append({ "first", "end", "Before", "match", "after", 0 });
        panel->append(progress, 100);
        auto *counter = panel->findChild<QLabel *>("rrSearchPage");
        QVERIFY(counter);
        const auto firstCounter = counter->text();
        const auto firstStatus = scopeStatus->text();
        QElapsedTimer progressTime;
        progressTime.start();
        progress.reset = false;
        progress.hits.clear();
        for (int i = 0; i < 40; ++i)
            progress.hits.append({ QString::number(i), "end", "Before", "match", "after", i });
        panel->append(progress, 100);
        panel->resize(panel->width() - 1, panel->height());
        QCOMPARE(counter->text(), firstCounter);
        QCOMPARE(scopeStatus->text(), firstStatus);
        QTRY_VERIFY_WITH_TIMEOUT(scopeStatus->text() != firstStatus, 3000);
        QVERIFY(progressTime.elapsed() >= 2000);
        QVERIFY(counter->text() != firstCounter);
        progress.hits.clear();
        progress.done = true;
        panel->append(progress, 100);
        QCOMPARE(status->text(), strings::get("search_panel.search_complete"));
        QVERIFY(!status->isVisible());
        const auto exactPages = counter->text();
        QVERIFY(exactPages.startsWith("Page 1 of "));
        QVERIFY(!exactPages.contains("at least"));
        // Every scope uses plain digits, including on devices with different grouping rules.
        const auto previousLocale = QLocale();

        struct RestoreLocale
        {
            QLocale locale;

            ~RestoreLocale() { QLocale::setDefault(locale); }
        } restoreLocale{ previousLocale };

        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        const char *scopeKeys[] = { "search_panel.current_chapter", "search_panel.up_to_here",
                                    "search_panel.after_here", "search_panel.whole_book" };
        for (int id : { 0, 1, 2, 3 }) {
            QVERIFY(click(canvas, scope));
            QVERIFY(click(canvas,
                          host.window()->findChild<QAbstractButton *>(
                                  QString("rrSearchScope%1").arg(id))));
            for (int count : { 0, 1, 1288 }) {
                SearchBatch sample;
                sample.reset = sample.done = true;
                for (int i = 0; i < count; ++i)
                    sample.hits.append(
                            { QString::number(i), "end", "Before", "match", "after", i });
                panel->append(sample, 100);
                QCOMPARE(scopeStatus->text(),
                         strings::get("search_panel.matches_in_scope")
                                 .arg(strings::get(count == 1 ? "search_panel.match"
                                                              : "search_panel.matches")
                                              .arg(count),
                                      strings::get(scopeKeys[id])));
                QCOMPARE(counter->isVisible(), count > 0);
                if (count == 1)
                    QCOMPARE(counter->text(), QString("Page 1 of 1"));
            }
        }
        QCOMPARE(scopeStatus->text(), QString("1288 matches in Whole book"));
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        panel->resize(panel->width() - 1, panel->height());
        QCOMPARE(scopeStatus->text(), QString("1288 matches in Whole book"));
        SearchBatch partial;
        partial.reset = true;
        partial.hits.append({ "first", "end", "Before", "match", "after", 0 });
        panel->append(partial, 100);
        QCOMPARE(counter->text(), QString("Page 1 of at least 1"));
        QVERIFY(!status->isVisible());
        QVERIFY(scopeStatus->text().contains("1 match so far"));
        panel->stop();
        QCOMPARE(status->text(), strings::get("search_panel.search_stopped"));
        panel->resize(panel->width() - 1, panel->height());
        QVERIFY(scopeStatus->text().contains("1 match so far"));
        QCOMPARE(counter->text(), QString("Page 1 of at least 1"));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSearchBack")));
    }

    void searchSurvivesReflow_data()
    {
        QTest::addColumn<QString>("interruption");
        QTest::newRow("queued-reply") << QString("queued-reply");
        QTest::newRow("partial-results") << QString("partial-results");
        QTest::newRow("reopen-partial") << QString("reopen-partial");
        QTest::newRow("completed-results") << QString("completed-results");
        QTest::newRow("busy-submit") << QString("busy-submit");
        QTest::newRow("replaced-query") << QString("replaced-query");
        QTest::newRow("closed-query") << QString("closed-query");
    }

    void searchSurvivesReflow()
    {
        QFETCH(QString, interruption);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        int referenceCount = 0;
        bool referenceDone = false;
        host.session()->search("reader", [&](SearchBatch batch) {
            referenceCount += batch.hits.size();
            referenceDone = batch.done;
        });
        QTRY_VERIFY_WITH_TIMEOUT(referenceDone, 10000);
        QVERIFY(referenceCount > 0 && referenceCount < 500);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *openSearch = host.window()->findChild<QPushButton *>("rrSearchOpen");
        QVERIFY(openSearch && click(canvas, openSearch));
        auto *panel = host.window()->findChild<QWidget *>("rrSearch");
        auto *query = host.window()->findChild<QLineEdit *>("rrSearchQuery");
        auto *status = host.window()->findChild<QLabel *>("rrSearchStatus");
        auto *back = host.window()->findChild<QPushButton *>("rrSearchBack");
        QVERIFY(panel && query && status && back);
        QTRY_VERIFY(panel->isVisible());
        auto *scope = host.window()->findChild<QPushButton *>("rrSearchScope");
        QVERIFY(scope && click(canvas, scope));
        auto *whole = host.window()->findChild<QAbstractButton *>("rrSearchScope3");
        QVERIFY(whole && click(canvas, whole));
        // Drain prior worker replies before intercepting this search's delivery.
        bool drained = false;
        host.session()->pageSummary(host.session()->page(),
                                    [&](QString, QString) { drained = true; });
        QTRY_VERIFY(drained);
        QCoreApplication::processEvents();
        auto *application = static_cast<NativeTestApplication *>(qApp);

        struct RestoreSearchDispatch
        {
            NativeTestApplication *application;

            ~RestoreSearchDispatch()
            {
                application->beforeEvent = {};
                application->afterEvent = {};
                engineActivity().pause(false);
            }
        } restoreDispatch{ application };

        const QSize resized(nativeSize.width() - 80, nativeSize.height() - 240);
        bool interrupted = false;
        if (interruption == "queued-reply") {
            application->beforeEvent = [&](QObject *object, QEvent *event) {
                if (object != qApp || event->type() != QEvent::MetaCall || interrupted) {
                    return;
                }
                application->beforeEvent = {};
                interrupted = true;
                // The worker has posted a completed batch, but its GUI reply
                // must not append after resize invalidates that layout.
                host.session()->resize(resized);
            };
        } else if (interruption == "partial-results" || interruption == "reopen-partial") {
            application->afterEvent = [&](QObject *object, QEvent *event) {
                if (object != qApp || event->type() != QEvent::MetaCall || interrupted
                    || status->text() == strings::get("search_panel.search_complete")) {
                    return;
                }
                auto *result = host.window()->findChild<QPushButton *>("rrSearchResult0");
                if (!result || !result->isVisible() || !result->isEnabled()) {
                    return;
                }
                application->afterEvent = {};
                interrupted = true;
                if (interruption == "reopen-partial")
                    result->click();
                else
                    // A restarted query must replace the already appended batch.
                    host.session()->resize(resized);
            };
        } else if (interruption != "completed-results") {
            engineActivity().pause(true);
            host.session()->resize(resized);
            QVERIFY(host.session()->busy());
            interrupted = true;
        }
        query->setText("reader");
        emit query->returnPressed();
        if (interruption == "completed-results") {
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("search_panel.search_complete"),
                                      10000);
            auto *first = panel->findChild<QPushButton *>("rrSearchResult0");
            QVERIFY(first);
            QTRY_VERIFY(first->isEnabled());
            engineActivity().pause(true);
            host.session()->resize(resized);
            QVERIFY(host.session()->busy());
            // A new visible range asks for details while rendering rejects the request.
            panel->resize(panel->width(), panel->height() - 240);
            QVERIFY(!first->isEnabled());
            // The old layout still says "Search complete" until the restarted
            // search delivers its first batch. Observe it during delivery so
            // even a scan that finishes between two test polls is counted.
            application->afterEvent = [&](QObject *object, QEvent *event) {
                if (object == qApp && event->type() == QEvent::MetaCall
                    && status->text() == strings::get("search_panel.searching")) {
                    application->afterEvent = {};
                    interrupted = true;
                }
            };
        }
        if (interruption == "replaced-query") {
            query->setText("zqxjv");
            emit query->returnPressed();
        } else if (interruption == "closed-query") {
            QVERIFY(click(canvas, back));
            QVERIFY(!panel->isVisible());
        }
        engineActivity().pause(false);
        QTRY_VERIFY(interrupted);
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy(), 10000);
        const auto none = strings::get("search_panel.no_matches_found");
        if (interruption == "reopen-partial") {
            QVERIFY(!panel->isVisible());
            QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
            QTRY_VERIFY(openSearch->isVisible());
            QVERIFY(click(canvas, openSearch));
        }
        if (interruption == "closed-query") {
            drained = false;
            host.session()->pageSummary(host.session()->page(),
                                        [&](QString, QString) { drained = true; });
            QTRY_VERIFY(drained);
            QCoreApplication::processEvents();
            QVERIFY(!panel->isVisible());
            // A later search owns a fresh epoch; the closed request cannot
            // append its matches or reopen the panel after layout settles.
            QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
            QTRY_VERIFY(openSearch->isVisible());
            QVERIFY(click(canvas, openSearch));
            query->setText("zqxjv");
            emit query->returnPressed();
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), none, 10000);
        } else {
            const auto expected = interruption == "replaced-query"
                    ? none
                    : strings::get("search_panel.search_complete");
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), expected, 10000);
        }
        if (interruption == "completed-results") {
            auto *first = panel->findChild<QPushButton *>("rrSearchResult0");
            QVERIFY(first);
            QTRY_VERIFY(first->isEnabled());
            QVERIFY(!first->accessibleName().isEmpty());
        }
        drained = false;
        host.session()->pageSummary(host.session()->page(),
                                    [&](QString, QString) { drained = true; });
        QTRY_VERIFY(drained);
        QCoreApplication::processEvents();
        QCOMPARE(status->text(),
                 interruption == "replaced-query" || interruption == "closed-query"
                         ? none
                         : strings::get("search_panel.search_complete"));
        const int expectedCount = interruption == "replaced-query" || interruption == "closed-query"
                ? 0
                : referenceCount;
        QCOMPARE(panel->findChild<QLabel *>("rrSearchScopeStatus")->text(),
                 strings::get("search_panel.matches_in_scope")
                         .arg(strings::get(expectedCount == 1 ? "search_panel.match"
                                                              : "search_panel.matches")
                                      .arg(expectedCount),
                              strings::get("search_panel.whole_book")));
    }

#ifndef KOBALT_NO_SENSORS
    // A return turn received during reflow must reach the reader and release input.
    void rotationRoundTripKeepsReadingResponsive_data()
    {
        QTest::addColumn<bool>("columns");
        QTest::newRow("one-column") << false;
        QTest::newRow("two-columns") << true;
    }

    void rotationRoundTripKeepsReadingResponsive()
    {
        QFETCH(bool, columns);
        const auto paths = testPaths();
        QVERIFY(storage::writePreferences(
                paths.preferences(),
                { { "auto_rotate", true }, { "two_columns_landscape", columns } }));
        test::Factory factory;
        test::SensorRegistration registration("kobalt-round-trip", &factory);
        QOrientationSensor sensor;
        sensor.setIdentifier("kobalt-round-trip");
        QVERIFY(sensor.start());
        factory.backend->sample(QOrientationReading::TopUp);
        QPointer<ReaderWindow> window;
        auto shown = Qt::PortraitOrientation;
        ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) {
            shown = value;
            const bool landscape =
                    value == Qt::LandscapeOrientation || value == Qt::InvertedLandscapeOrientation;
            if (window)
                window->resize(landscape ? nativeSize.transposed() : nativeSize);
        });
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.rotation = &rotation;
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        window = host.window();
        window->show();
        QTRY_VERIFY(window->readyForPresentation());
        const auto position = host.session()->state().value("position");
        auto *page = static_cast<ReadingPage *>(window->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        for (auto outward : { QOrientationReading::LeftUp, QOrientationReading::RightUp }) {
            const auto release = std::make_shared<QSemaphore>();
            qt::invoke(engineWorker(), [release] { release->acquire(); }, Qt::QueuedConnection);
            bool held = true;
            const auto unblock = qScopeGuard([&] {
                if (held)
                    release->release();
            });
            factory.backend->sample(outward);
            QTRY_COMPARE(shown,
                         outward == QOrientationReading::LeftUp ? Qt::LandscapeOrientation
                                                                : Qt::InvertedLandscapeOrientation);
            QTRY_VERIFY(host.session()->busy());
            factory.backend->sample(QOrientationReading::TopUp);
            auto *debounce = rotation.findChild<QTimer *>("rrOrientationDebounce");
            QVERIFY(debounce);
            QTRY_VERIFY(!debounce->isActive());
            QVERIFY(shown != Qt::PortraitOrientation);
            held = false;
            release->release();
            QTRY_COMPARE(shown, Qt::PortraitOrientation);
            QTRY_VERIFY(window->readyForPresentation());
            QVERIFY(!page->viewportTransition());
            QCOMPARE(host.session()->state().value("position"), position);
        }
        const auto currentPage = host.session()->page();
        test::skipPageTurnWindow();
        window->pageButton(1);
        QTRY_VERIFY(host.session()->page() > currentPage);
        QTRY_VERIFY(window->readyForPresentation());
    }

    // Settings holds rotation, then applies the last permitted sample without another movement.
    void settingsOrientationHandoff_data()
    {
        QTest::addColumn<bool>("automatic");
        QTest::addColumn<int>("initial");
        QTest::addColumn<int>("sample");
        QTest::addColumn<int>("expected");
        QTest::newRow("auto-landscape")
                << true << int(Qt::PortraitOrientation) << int(QOrientationReading::LeftUp)
                << int(Qt::LandscapeOrientation);
        QTest::newRow("auto-portrait")
                << true << int(Qt::LandscapeOrientation) << int(QOrientationReading::TopUp)
                << int(Qt::PortraitOrientation);
        QTest::newRow("portrait-lock")
                << false << int(Qt::PortraitOrientation) << int(QOrientationReading::LeftUp)
                << int(Qt::PortraitOrientation);
        QTest::newRow("landscape-lock")
                << false << int(Qt::LandscapeOrientation) << int(QOrientationReading::TopUp)
                << int(Qt::LandscapeOrientation);
        QTest::newRow("inverted-landscape-lock")
                << false << int(Qt::LandscapeOrientation) << int(QOrientationReading::RightUp)
                << int(Qt::InvertedLandscapeOrientation);
    }

    void settingsOrientationHandoff()
    {
        QFETCH(bool, automatic);
        QFETCH(int, initial);
        QFETCH(int, sample);
        QFETCH(int, expected);
        QVERIFY(storage::writeJson(testPaths().preferences(),
                                   { { "auto_rotate", automatic }, { "orientation", initial } }));
        test::Factory factory;
        test::SensorRegistration registration("kobalt-settings-test", &factory);
        QOrientationSensor sensor;
        sensor.setIdentifier("kobalt-settings-test");
        QVERIFY(sensor.start());
        factory.backend->sample(initial == int(Qt::LandscapeOrientation)
                                        ? QOrientationReading::LeftUp
                                        : QOrientationReading::TopUp);
        auto shown = Qt::ScreenOrientation(initial);
        ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) { shown = value; });
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        host.rotation = &rotation;
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        rotation.start(Qt::PortraitOrientation);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *panel = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QTRY_VERIFY(panel->isVisible());
        factory.backend->sample(QOrientationReading::Orientation(sample));
        auto *debounce = rotation.findChild<QTimer *>("rrOrientationDebounce");
        QVERIFY(debounce);
        QTRY_VERIFY(!debounce->isActive());
        QCOMPARE(shown, Qt::ScreenOrientation(initial));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
        QTRY_VERIFY(!panel->isVisible() && !host.session()->busy());
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QTRY_COMPARE(shown, Qt::ScreenOrientation(expected));
    }

    void exactRotationLock()
    {
        test::Factory factory;
        test::SensorRegistration registration("kobalt-lock-test", &factory);
        QOrientationSensor sensor;
        sensor.setIdentifier("kobalt-lock-test");
        QVERIFY(sensor.start());
        factory.backend->sample(QOrientationReading::TopDown);
        auto shown = Qt::PortraitOrientation;
        ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) { shown = value; });
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        host.rotation = &rotation;
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        rotation.start(Qt::PortraitOrientation);
        QCOMPARE(shown, Qt::InvertedPortraitOrientation);
        auto *window = host.window();
        Canvas canvas(window, nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(window->readyForPresentation());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, window->findChild<QPushButton *>("rrNavLayout")));
        auto *portrait = window->findChild<QPushButton *>("rrOrientation1");
        auto *landscape = window->findChild<QPushButton *>("rrOrientation2");
        auto *both = window->findChild<QPushButton *>("rrAutoRotate");
        QTRY_VERIFY(portrait && portrait->isVisible() && portrait->isChecked());
        QVERIFY(landscape && landscape->isVisible());
        QVERIFY(both && both->isVisible());
        auto *lock = window->findChild<QPushButton *>("rrLockRotation");
        QVERIFY(lock && lock->isVisible() && !lock->isChecked());
        const auto capture = qEnvironmentVariable("KOBALT_TEST_ROTATION_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window->grab().save(capture + "/rotation-controls.png"));
        const int lockRight = lock->mapTo(window, lock->rect().topRight()).x();
        const int portraitLeft = portrait->mapTo(window, QPoint()).x();
        QVERIFY2(lockRight + 1 < portraitLeft,
                 qPrintable(QString("Lock ends at %1 and Portrait starts at %2, lock width %3")
                                    .arg(lockRight)
                                    .arg(portraitLeft)
                                    .arg(lock->width())));
        QVERIFY(!lock->icon().isNull());
        const auto unlockedIcon = lock->icon().pixmap(lock->iconSize()).toImage();

        auto *debounce = rotation.findChild<QTimer *>("rrOrientationDebounce");
        QVERIFY(debounce);
        factory.backend->sample(QOrientationReading::TopUp);
        QVERIFY(debounce->isActive());
        QVERIFY(click(canvas, lock));
        QVERIFY(lock->isChecked() && portrait->isChecked() && !both->isChecked());
        QVERIFY(lock->isEnabled() && !portrait->isEnabled() && !landscape->isEnabled()
                && !both->isEnabled());
        QVERIFY(lock->icon().pixmap(lock->iconSize()).toImage() != unlockedIcon);
        QCOMPARE(shown, Qt::InvertedPortraitOrientation);
        QVERIFY(!debounce->isActive());
        if (!capture.isEmpty())
            QVERIFY(window->grab().save(capture + "/rotation-locked.png"));
        factory.backend->sample(QOrientationReading::LeftUp);
        factory.backend->sample(QOrientationReading::TopUp);
        QVERIFY(!debounce->isActive());
        QCOMPARE(shown, Qt::InvertedPortraitOrientation);

        QVERIFY(click(canvas, window->findChild<QPushButton *>("rrNavLayout")));
        QTRY_VERIFY(!lock->isVisible());
        const auto savedRotation = storage::readJson(testPaths().preferences());
        QCOMPARE(savedRotation.value("locked_orientation").toInt(),
                 int(Qt::InvertedPortraitOrientation));
        QCOMPARE(savedRotation.value("orientation").toInt(), int(Qt::PortraitOrientation));
        QVERIFY(!savedRotation.value("auto_rotate").toBool());

        QVERIFY(click(canvas, window->findChild<QPushButton *>("rrNavLayout")));
        QTRY_VERIFY(lock->isVisible() && lock->isChecked() && portrait->isChecked());
        QVERIFY(click(canvas, lock));
        QVERIFY(!lock->isChecked() && portrait->isChecked() && both->isEnabled());
        QVERIFY(click(canvas, both));
        factory.backend->sample(QOrientationReading::LeftUp);
        QVERIFY(debounce->isActive());
        QVERIFY(click(canvas, lock));
        QVERIFY(both->isChecked() && lock->isChecked() && !both->isEnabled());
        if (!capture.isEmpty())
            QVERIFY(window->grab().save(capture + "/rotation-both-locked.png"));
        factory.backend->sample(QOrientationReading::TopUp);
        QVERIFY(!debounce->isActive());
        QCOMPARE(shown, Qt::InvertedPortraitOrientation);

        factory.backend->sample(QOrientationReading::LeftUp);
        QVERIFY(click(canvas, lock));
        QVERIFY(both->isChecked() && !lock->isChecked() && both->isEnabled());
        QCOMPARE(lock->icon().pixmap(lock->iconSize()).toImage(), unlockedIcon);
        QVERIFY(debounce->isActive());
        QVERIFY(click(canvas, window->findChild<QPushButton *>("rrNavLayout")));
        QTRY_VERIFY(!lock->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_COMPARE(shown, Qt::LandscapeOrientation);
        const auto unlocked = storage::readJson(testPaths().preferences());
        QCOMPARE(unlocked.value("locked_orientation").toInt(-1), int(Qt::PrimaryOrientation));
        QVERIFY(unlocked.value("auto_rotate").toBool());
    }

    void sleepRequestFreezesOrientationBeforeHandoff_data()
    {
        QTest::addColumn<bool>("handoff");
        QTest::newRow("completed-sleep") << true;
        QTest::newRow("wake-before-handoff") << false;
    }

    void sleepRequestFreezesOrientationBeforeHandoff()
    {
        QFETCH(bool, handoff);
        test::Factory factory;
        test::SensorRegistration registration("kobalt-sleep-test", &factory);
        QOrientationSensor sensor;
        sensor.setIdentifier("kobalt-sleep-test");
        QVERIFY(sensor.start());
        factory.backend->sample(QOrientationReading::TopUp);
        auto shown = Qt::PortraitOrientation;
        ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) { shown = value; });
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        host.rotation = &rotation;
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        rotation.start(Qt::PortraitOrientation);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        // A new angle arms the 250 ms debounce. The sleep request cancels it,
        // and a sample while asleep does not arm it again, so no turn can
        // follow. Check the timer instead of waiting past its interval.
        auto *debounce = rotation.findChild<QTimer *>("rrOrientationDebounce");
        QVERIFY(debounce && debounce->isSingleShot());
        factory.backend->sample(QOrientationReading::TopDown);
        QVERIFY(debounce->isActive());
        QCOMPARE(debounce->interval(), 250);
        host.window()->sleepReader();
        QVERIFY(host.window()->isVisible());
        QVERIFY(!debounce->isActive());
        QCoreApplication::processEvents();
        QCOMPARE(shown, Qt::PortraitOrientation);
        factory.backend->sample(QOrientationReading::TopDown);
        QVERIFY(!debounce->isActive());
        QCoreApplication::processEvents();
        QCOMPARE(shown, Qt::PortraitOrientation);
        if (handoff) {
            host.window()->sleepViewReady();
            factory.backend->sample(QOrientationReading::TopDown);
            QCOMPARE(sensor.reading()->orientation(), QOrientationReading::TopUp);
        }
        host.window()->wakeReader();
        QTRY_VERIFY(host.window()->readyForPresentation());
        factory.backend->sample(QOrientationReading::TopDown);
        QTRY_COMPARE(shown, Qt::InvertedPortraitOrientation);
    }

    void closePublishesHeldOrientationBeforeBackgroundPaint()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        test::Factory factory;
        test::SensorRegistration registration("kobalt-close-test", &factory);
        QOrientationSensor sensor;
        sensor.setIdentifier("kobalt-close-test");
        QVERIFY(sensor.start());
        factory.backend->sample(QOrientationReading::TopUp);
        auto shown = Qt::PortraitOrientation;
        int notifications = 0;
        QObject::connect(&sensor, &QSensor::readingChanged, [&] {
            ++notifications;
            if (sensor.reading()->orientation() == QOrientationReading::TopDown) {
                shown = Qt::InvertedPortraitOrientation;
            }
        });
        ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) { shown = value; });
        QList<Qt::ScreenOrientation> painted;
        ReaderFixture host(storage::Paths{ directory.path() }, testFonts, testPlaceholder);
        host.rotation = &rotation;
        host.configureServices = [&](ReaderHostServices &services) {
            services.presentation = [&](QWidget *) {
                auto presentation = std::make_unique<test::HostPresentation>();
                presentation->repaint = [&] { painted.append(shown); };
                return presentation;
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        rotation.start(Qt::PortraitOrientation);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        factory.backend->sample(QOrientationReading::TopDown);
        QTRY_COMPARE(shown, Qt::InvertedPortraitOrientation);
        QCOMPARE(notifications, 0);
        host.window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 30000);
        QCOMPARE(notifications, 1);
        QVERIFY(!painted.isEmpty());
        QCOMPARE(painted.first(), Qt::InvertedPortraitOrientation);
    }

#endif
    void closeRestoresOrientationBeforeBackgroundPaint()
    {
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(),
                                   { { "orientation", int(Qt::InvertedPortraitOrientation) } }));
        auto shown = Qt::PortraitOrientation;
        ReaderOrientation rotation(nullptr, [&](Qt::ScreenOrientation value) { shown = value; });
        QList<Qt::ScreenOrientation> painted;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.rotation = &rotation;
        host.configureServices = [&](ReaderHostServices &services) {
            services.presentation = [&](QWidget *) {
                auto presentation = std::make_unique<test::HostPresentation>();
                presentation->repaint = [&] { painted.append(shown); };
                return presentation;
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        rotation.start(Qt::PortraitOrientation);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QCOMPARE(shown, Qt::InvertedPortraitOrientation);
        host.window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 30000);
        QVERIFY(!painted.isEmpty());
        QCOMPARE(painted.first(), Qt::PortraitOrientation);
        QVERIFY(storage::writeJson(paths.preferences(), {}));
    }

    void rotationOwnership()
    {
        QList<int> calls;
        {
            ReaderOrientation orientation(
                    nullptr, [&](Qt::ScreenOrientation value) { calls.append(int(value)); },
                    [&](bool owned) { calls.append(owned ? -1 : -2); });
            orientation.configure(QJsonObject{ { "orientation", int(Qt::LandscapeOrientation) } });
            QVERIFY(calls.isEmpty());
            orientation.start(Qt::PortraitOrientation);
            QCOMPARE(calls, QList<int>({ -1, int(Qt::LandscapeOrientation) }));
            orientation.start(Qt::PortraitOrientation);
            QCOMPARE(calls.size(), 2);
            orientation.stop();
            QCOMPARE(calls,
                     QList<int>({ -1, int(Qt::LandscapeOrientation), int(Qt::PortraitOrientation),
                                  -2 }));
            orientation.stop();
            QCOMPARE(calls.size(), 4);
            orientation.start(Qt::PortraitOrientation);
        }
        QCOMPARE(calls.mid(4),
                 QList<int>(
                         { -1, int(Qt::LandscapeOrientation), int(Qt::PortraitOrientation), -2 }));
    }

    void closedReaderDoesNotSaveSleepScreen_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("dark");
        for (const auto &mode : { QString("stock"), QString("page"), QString("cover") })
            for (bool dark : { false, true })
                QTest::newRow(qPrintable(mode + (dark ? "-dark" : "-light"))) << mode << dark;
    }

    void closedReaderDoesNotSaveSleepScreen()
    {
        QFETCH(QString, mode);
        QFETCH(bool, dark);
        const auto paths = testPaths();
        QVERIFY(storage::writePreferences(
                paths.preferences(), { { sleep::preferenceKey, mode }, { "dark_mode", dark } }));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        bool coverReady = false;
        host.session()->coverImage({ 2048, 2048 }, [&](QImage) { coverReady = true; });
        QTRY_VERIFY(coverReady);
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        const auto expected = state->image;
        host.window()->wakeReader();
        QTRY_VERIFY(host.window()->readyForPresentation());
        const auto backdrop = host.window()->powerScreenSnapshot();
        if (mode != "stock") {
            QVERIFY(!backdrop.image.isNull());
            QCOMPARE(backdrop.dark, dark);
            QCOMPARE(reader::sleepScreenImage(backdrop.image, backdrop.progress, backdrop.remaining,
                                              dark, -1, reader::SleepStatus::Sleeping,
                                              backdrop.dimPage, backdrop.tile, backdrop.percent),
                     expected);
        }
        host.window()->close();
        QTRY_VERIFY(host.idle());
        QCOMPARE(state->savedCount, 0);
    }

    void usbDoesNotSaveSleepScreen()
    {
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), { { sleep::preferenceKey, "page" } }));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        host.progress = std::make_shared<test::HostProgress>();
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        const auto expected = host.window()->powerScreenSnapshot();
        QVERIFY(!expected.image.isNull());
        host.progress->finishPending = true;
        const auto releaseNative = qScopeGuard([&] { host.progress->completeFinish(true); });
        QVERIFY(host.window()->pendingStorageSave());
        const int writes = host.progress->writeAttempts;
        bool released = false;
        int ordinaryCompletions = 0;
        host.window()->finishStorageSave([&](bool) { ++ordinaryCompletions; });
        host.window()->prepareUsb([&] { released = true; });
        QTRY_VERIFY_WITH_TIMEOUT(released, 500);
        host.progress->completeFinish(true);
        QCoreApplication::processEvents();
        QCOMPARE(ordinaryCompletions, 0);
        QCOMPARE(host.progress->writeAttempts, writes);
        QCOMPARE(state->savedCount, 0);
    }

    void persistedSleepScreenRejectsStaleAndDamagedFrames()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.path() + "/cache/sleep-screen.png";
        sleep::Snapshot snapshot;
        snapshot.image = QImage(100, 200, QImage::Format_RGB32);
        snapshot.image.fill(Qt::darkGray);
        snapshot.orientation = Qt::InvertedPortraitOrientation;
        snapshot.progress = "42% read";
        snapshot.remaining = "37 min to go";
        snapshot.dark = true;
        snapshot.mode = sleep::Mode::Page;
        QVERIFY(storage::writeSleepSnapshot(path, snapshot, "book-stamp"));
        const auto loaded = storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Page);
        QCOMPARE(loaded.image, snapshot.image);
        QCOMPARE(loaded.orientation, snapshot.orientation);
        QCOMPARE(loaded.progress, snapshot.progress);
        QCOMPARE(loaded.remaining, snapshot.remaining);
        QCOMPARE(loaded.dark, snapshot.dark);
        QCOMPARE(loaded.percent, -1);
        snapshot.percent = 42;
        QVERIFY(storage::writeSleepSnapshot(path, snapshot, "book-stamp"));
        QCOMPARE(storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Page).percent, 42);
        {
            // Images saved before the percentage was stored still load.
            QImageWriter writer(path, "png");
            writer.setText("kobalt",
                           R"({"version":1,"token":"book-stamp","mode":"page","orientation":4,)"
                           R"("progress":"42% read","remaining":"","dark":false,"dim":true})");
            QVERIFY(writer.write(snapshot.image));
        }
        const auto older = storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Page);
        QCOMPARE(older.image, snapshot.image);
        QCOMPARE(older.percent, -1);
        QVERIFY(storage::readSleepSnapshot(path, "other-book", sleep::Mode::Page).image.isNull());
        QVERIFY(storage::readSleepSnapshot(path, "", sleep::Mode::Page).image.isNull());
        QVERIFY(storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Cover).image.isNull());
        QVERIFY(storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Stock).image.isNull());
        snapshot.image = QImage(4096, 4096, QImage::Format_RGB32);
        QVERIFY(!storage::writeSleepSnapshot(path, snapshot, "book-stamp"));
        QVERIFY(!storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Page).image.isNull());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write("damaged") > 0);
        file.close();
        QVERIFY(storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Page).image.isNull());
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(21 * 1024 * 1024));
        file.close();
        QVERIFY(storage::readSleepSnapshot(path, "book-stamp", sleep::Mode::Page).image.isNull());
    }

    void homeSleepScreenRequiresUnambiguousLastBook()
    {
        QCOMPARE(nickel::nativeSleepPercent(0, 100), 0);
        QCOMPARE(nickel::nativeSleepPercent(1, 27), 27);
        QCOMPARE(nickel::nativeSleepPercent(2, 94), 100);
        QCOMPARE(nickel::nativeSleepPercent({}, {}), -1);
        const auto name = QStringLiteral("sleep-latest-book-test");
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", name);
            database.setDatabaseName(":memory:");
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(
                    "CREATE TABLE content (ContentID TEXT, ContentType INTEGER, DateLastRead TEXT)"));
            QVERIFY(nickel::lastReadBookToken(database).isEmpty());
            QVERIFY(query.exec("INSERT INTO content VALUES ('first', 6, '2026-10-24T09:00:00Z')"));
            const auto token = nickel::lastReadBookToken(database, "first");
            QVERIFY(!token.isEmpty());
            QCOMPARE(nickel::lastReadBookToken(database), token);
            QVERIFY(nickel::lastReadBookToken(database, "other").isEmpty());
            QVERIFY(query.exec("INSERT INTO content VALUES ('second', 6, '2026-10-24T09:00:01Z')"));
            QVERIFY(nickel::lastReadBookToken(database, "first").isEmpty());
            QVERIFY(nickel::lastReadBookToken(database) != token);
            QVERIFY(query.exec(
                    "UPDATE content SET DateLastRead = '2026-10-24T09:00:01Z' WHERE ContentID = 'first'"));
            QVERIFY(nickel::lastReadBookToken(database).isEmpty());
        }
        QSqlDatabase::removeDatabase(name);
    }

    void readerUsesSelectedSleepScreen_data()
    {
        QTest::addColumn<bool>("supported");
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("chargeState");
        QTest::addColumn<QString>("selectedMode");
        QTest::addColumn<bool>("hasCover");
        QTest::addColumn<QString>("hostCover");
        QTest::newRow("stock-default")
                << true << false << false << 0 << QString() << false << QString();
        QTest::newRow("custom-light")
                << true << true << false << 0 << QString() << false << QString();
        QTest::newRow("custom-dark")
                << true << true << true << 0 << QString() << false << QString();
        QTest::newRow("unsupported-host")
                << false << true << false << 0 << QString() << false << QString();
        QTest::newRow("charging-light")
                << true << true << false << 1 << QString() << false << QString();
        QTest::newRow("charging-dark")
                << true << true << true << 1 << QString() << false << QString();
        QTest::newRow("charged-light")
                << true << true << false << 2 << QString() << false << QString();
        QTest::newRow("charged-dark")
                << true << true << true << 2 << QString() << false << QString();
        QTest::newRow("cover-light")
                << true << true << false << 1 << QString("cover") << true << QString();
        QTest::newRow("cover-dark")
                << true << true << true << 2 << QString("cover") << true << QString();
        QTest::newRow("legacy-cover")
                << true << true << false << 0 << QString() << true << QString();
        QTest::newRow("page-with-cover")
                << true << true << false << 0 << QString("page") << true << QString();
        QTest::newRow("missing-cover")
                << true << true << false << 0 << QString("cover") << false << QString();
        QTest::newRow("stock-with-cover")
                << true << false << false << 0 << QString("stock") << true << QString();
        QTest::newRow("native-cover")
                << true << true << false << 0 << QString("cover") << true << QString("valid");
        QTest::newRow("native-cover-without-embedded")
                << true << true << true << 1 << QString("cover") << false << QString("valid");
        QTest::newRow("native-corrupt-fallback")
                << true << true << false << 0 << QString("cover") << true << QString("corrupt");
        QTest::newRow("native-missing-text-fallback")
                << true << true << false << 0 << QString("cover") << false << QString("missing");
        QTest::newRow("page-ignores-native-cover")
                << true << true << false << 0 << QString("page") << true << QString("valid");
        QTest::newRow("unsupported-native-cover")
                << false << true << false << 0 << QString("cover") << true << QString("valid");
    }

    void readerUsesSelectedSleepScreen()
    {
        QFETCH(bool, supported);
        QFETCH(bool, enabled);
        QFETCH(bool, dark);
        QFETCH(int, chargeState);
        QFETCH(QString, selectedMode);
        QFETCH(bool, hasCover);
        QFETCH(QString, hostCover);
        QTemporaryDir coverDirectory;
        QVERIFY(coverDirectory.isValid());
        const auto coverPath =
                hostCover.isEmpty() ? QString() : coverDirectory.path() + "/cover.parsed";
        if (hostCover == "valid") {
            QImage nativeCover(80, 160, QImage::Format_RGB32);
            nativeCover.fill(Qt::red);
            QVERIFY(nativeCover.save(coverPath, "PNG"));
        } else if (hostCover == "corrupt") {
            QFile file(coverPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("not an image") > 0);
        }
        const auto paths = testPaths();
        const auto mode = selectedMode.isEmpty() ? (enabled ? "cover" : "stock") : selectedMode;
        const QJsonObject preferences{ { sleep::preferenceKey, mode }, { "dark_mode", dark } };
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        auto sleep = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        // Exercise Kobo's missing native pace, independently of standalone history.
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {
            QUrl::fromLocalFile(QFileInfo(hasCover ? coverBook : textBook).absoluteFilePath())
                    .toString(),
            "OEBPS/c1.xhtml", 100, 1, true
        };
        host.configureServices = [sleep, supported, chargeState,
                                  coverPath](ReaderHostServices &services) {
            services.coverImagePath = coverPath;
            services.battery = [chargeState] {
                return BatteryStatus{ 67, chargeState > 0, chargeState == 2 };
            };
            services.presentation = [sleep, supported](QWidget *) {
                return std::make_unique<test::HostPresentation>(supported ? sleep : nullptr);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(hasCover ? coverBook : textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        QVERIFY(page);
        const auto original = page->grab().toImage();
        QImage cover;
        bool coverReady = false;
        host.session()->coverImage(
                { 2048, 2048 },
                [&](QImage image) {
                    cover = std::move(image);
                    coverReady = true;
                },
                coverPath);
        QTRY_VERIFY(coverReady);
        QCOMPARE(!cover.isNull(), hasCover || hostCover == "valid");
        if (hostCover == "valid") {
            QCOMPARE(cover.size(), QSize(80, 160));
            QCOMPARE(cover.pixel(40, 80), qRgb(255, 0, 0));
        } else if (hasCover) {
            QCOMPARE(cover.size(), QSize(128, 64));
            QCOMPARE(host.session()->coverThumbnail().size(), QSize(120, 60));
        }
        const bool useCover = sleep::mode(preferences) == sleep::Mode::Cover && !cover.isNull();
        const auto background = useCover ? reader::sleepCoverBackground(cover, page->size())
                                         : static_cast<ReadingPage *>(page)->snapshot(true);
        const auto status = chargeState == 2 ? reader::SleepStatus::Charged
                : chargeState == 1           ? reader::SleepStatus::Charging
                                             : reader::SleepStatus::Sleeping;
        // Hybrid can still be paging within one chapter. Use the reader's default statistics scope.
        const int percent = host.session()->statsPercent(!host.session()->hasMainTextBoundary());
        // This profile has no native reading pace and has not made forward turns.
        const auto expected = reader::sleepScreenImage(
                background,
                strings::get("sleep.percent_read")
                        .arg(strings::percentArgument(percent, QLocale::c())),
                {}, dark, 67, status, !useCover, sleep::tile(preferences), percent);
        host.window()->sleepReader();
        QVERIFY(host.window()->isVisible());
        QVERIFY(sleep->image.isNull());
        host.window()->sleepViewReady();
        QCOMPARE(sleep->presented, supported && enabled ? 1 : 0);
        QVERIFY(!host.window()->isVisible());
        if (supported && enabled) {
            QCOMPARE(sleep->image.size(), original.size());
            QVERIFY(sleep->image != original);
            QCOMPARE(sleep->image, expected);
            const auto copy = sleep->image;
            host.window()->sleepViewReady();
            QCOMPARE(sleep->presented, 1);
            QCOMPARE(sleep->image, copy);
        }
        host.window()->wakeReader();
        QVERIFY(sleep->image.isNull());
        QVERIFY(host.window()->isVisible());
        QTRY_VERIFY(!host.session()->busy());
    }

    void sleepScreenUpdatesWhilePluggedIn_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("tile");
        for (const auto &mode : { QString("stock"), QString("page"), QString("cover") }) {
            QTest::newRow(qPrintable(mode + "-light")) << mode << false << QString();
            QTest::newRow(qPrintable(mode + "-dark")) << mode << true << QString();
        }
        for (const auto &tile : { QString("card"), QString("bookmark") }) {
            QTest::newRow(qPrintable("page-" + tile + "-light"))
                    << QString("page") << false << tile;
            QTest::newRow(qPrintable("cover-" + tile + "-dark"))
                    << QString("cover") << true << tile;
        }
    }

    void sleepScreenUpdatesWhilePluggedIn()
    {
        QFETCH(QString, mode);
        QFETCH(bool, dark);
        QFETCH(QString, tile);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{ directory.path() };
        QJsonObject preferences{ { sleep::preferenceKey, mode }, { "dark_mode", dark } };
        if (!tile.isEmpty())
            preferences[sleep::tileKey] = tile;
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        auto state = std::make_shared<test::SleepPresentationState>();
        BatteryStatus battery{ 66, false, false };
        std::function<void()> changed;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.battery = [&] { return battery; };
            services.observeBattery = [&](QObject *, std::function<void()> callback) {
                changed = std::move(callback);
            };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        bool coverReady = false;
        host.session()->coverImage({ 2048, 2048 }, [&](QImage) { coverReady = true; });
        QTRY_VERIFY(coverReady);
        QVERIFY(changed);
        changed();
        QCOMPARE(state->updated, 0);
        // Sleep must sample charging even if the host has not sent a notification.
        battery = { 67, true, false };
        host.window()->sleepReader();
        changed();
        QCOMPARE(state->updated, 0);
        host.window()->sleepViewReady();
        if (mode == "stock") {
            QVERIFY(state->image.isNull());
            battery = { 100, true, true };
            changed();
            QCOMPARE(state->updated, 0);
            host.window()->wakeReader();
            return;
        }
        QCOMPARE(state->presented, 1);
        changed();
        QCOMPARE(state->updated, 0);
        const auto initial = state->image;
        QVERIFY(!initial.isNull());
        // A point on the page that the selected tile does not cover.
        const QPoint outside = sleep::tile(preferences) == sleep::Tile::Bookmark
                ? QPoint(100, 200)
                : QPoint(1000, 200);
        battery.percent = 68;
        changed();
        QCOMPARE(state->updated, 1);
        QVERIFY(state->image != initial);
        QCOMPARE(state->image.pixel(outside), initial.pixel(outside));
        QVERIFY(!state->damage.isEmpty());
        QVERIFY(!state->damage.contains(outside));
        if (sleep::tile(preferences) == sleep::Tile::Bookmark) {
            // Bookmark keeps one area whether or not a time estimate is shown.
            QCOMPARE(state->damage,
                     reader::sleepScreenCardRect(initial.size(), false, sleep::tile(preferences)));
        }
        const auto charging = state->image;
        changed();
        QCOMPARE(state->updated, 1);
        battery = { 100, true, true };
        changed();
        QCOMPARE(state->updated, 2);
        QVERIFY(state->image != charging);
        const auto charged = state->image;
        battery = { 100, false, false };
        changed();
        QCOMPARE(state->updated, 3);
        QVERIFY(state->image != charged);
        QCOMPARE(state->image.pixel(outside), initial.pixel(outside));
        const auto unplugged = state->image;
        battery.percent = 20;
        changed();
        QCOMPARE(state->updated, 3);
        QCOMPARE(state->image, unplugged);
        host.window()->wakeReader();
        battery = { 21, true, false };
        changed();
        QCOMPARE(state->updated, 3);
        QVERIFY(state->image.isNull());
    }

    // Today includes every reading session completed while this reader stays open.
    void sleepWidgetsKeepTimeAcrossSessionTimeout()
    {
        const auto paths = testPaths();
        const QJsonObject preferences{ { sleep::designKey, "widgets" },
                                       { sessionTimeout::preferenceKey, 15 } };
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        qint64 utc = 100000;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.utc = [&] { return utc; };
        auto state = std::make_shared<test::SleepPresentationState>();
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QTRY_VERIFY(host.book()->readingMilliseconds() >= 250);
        host.book()->sleepReading(preferences);
        const auto earlier = host.book()->readingMilliseconds();
        utc += 16 * 60000;
        host.book()->observeReading();
        QVERIFY(host.book()->sessionReadingMilliseconds() < earlier);
        const auto snapshot = host.window()->powerScreenSnapshot();
        QVERIFY(!snapshot.image.isNull());
        QVERIFY2(snapshot.details.todayMilliseconds >= earlier,
                 "Today dropped the completed session after the sleep timeout");
    }

    void sleepWidgetsUseSavedDay()
    {
        const auto paths = testPaths();
        QVERIFY(storage::writePreferences(paths.preferences(),
                                          { { sleep::designKey, "widgets" } }));
        qint64 utc = QDateTime(QDate(2026, 11, 10), QTime(9, 0)).toMSecsSinceEpoch();
        {
            ReadingHistory previous(paths.directory, "other-book", {}, {}, "Other", "Author",
                                    [&] { return utc; });
            previous.start(0, "start");
            utc += 8 * 60000;
            previous.stop(8 * 60000, "end");
            QVERIFY(previous.save());
        }
        qint64 active = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.utc = [&] { return utc; };
        host.readingClock = [&] { return active; };
        auto state = std::make_shared<test::SleepPresentationState>();
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.book()->readingActive());
        QTRY_COMPARE(host.window()->powerScreenSnapshot().details.todayMilliseconds,
                     qint64(8 * 60000));
        utc += 2 * 60000;
        active += 2 * 60000;
        host.window()->checkpoint();
        QCOMPARE(host.window()->powerScreenSnapshot().details.todayMilliseconds,
                 qint64(10 * 60000));
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        host.window()->show();
        QTRY_VERIFY(host.book()->readingActive());
        QTRY_COMPARE(host.window()->powerScreenSnapshot().details.todayMilliseconds,
                     qint64(10 * 60000));
    }

    void sleepWidgetsUseLocalDay_data()
    {
        QTest::addColumn<int>("timeout");
        QTest::newRow("short-sleep") << 15;
        QTest::newRow("expired-session") << 15;
        QTest::newRow("never-reset-session") << 0;
    }

    void sleepWidgetsUseLocalDay()
    {
        QFETCH(int, timeout);
        const auto paths = testPaths();
        const QJsonObject preferences{ { sleep::designKey, "widgets" },
                                       { sessionTimeout::preferenceKey, timeout } };
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        const QDate date(2026, 11, 10);
        qint64 utc = QDateTime(date, QTime(23, 50)).toMSecsSinceEpoch();
        qint64 active = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.utc = [&] { return utc; };
        host.readingClock = [&] { return active; };
        auto state = std::make_shared<test::SleepPresentationState>();
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QTRY_VERIFY(host.book()->readingActive());
        utc += 15 * 60000;
        active += 15 * 60000;
        auto shot = host.window()->powerScreenSnapshot();
        QCOMPARE(shot.details.todayMilliseconds, qint64(5 * 60000));
        // A completed checkpoint must not be added to this opening twice.
        host.book()->checkpointReadingHistory();
        shot = host.window()->powerScreenSnapshot();
        QCOMPARE(shot.details.todayMilliseconds, qint64(5 * 60000));
        host.window()->sleepReader();
        const int days = QByteArray(QTest::currentDataTag()) == "short-sleep" ? 0 : 3;
        utc = QDateTime(date.addDays(1 + days), QTime(0, 10)).toMSecsSinceEpoch();
        host.window()->wakeReader();
        QTRY_VERIFY(host.book()->readingActive());
        utc += 2 * 60000;
        active += 2 * 60000;
        shot = host.window()->powerScreenSnapshot();
        QCOMPARE(shot.details.todayMilliseconds, qint64((days ? 2 : 7) * 60000));
        host.window()->sleepReader();
        // A sleep spanning midnight contributes zero until reading resumes.
        utc = QDateTime(date.addDays(2 + days), QTime(0, 1)).toMSecsSinceEpoch();
        host.window()->wakeReader();
        QTRY_VERIFY(host.book()->readingActive());
        shot = host.window()->powerScreenSnapshot();
        QCOMPARE(shot.details.todayMilliseconds, qint64(0));
    }

    void sleepPrivatePreviewsHideBookPixels()
    {
        const QSize size(300, 400);
        QImage cover(size, QImage::Format_RGB32), pageImage(size, QImage::Format_RGB32);
        cover.fill(Qt::red);
        pageImage.fill(Qt::green);
        QImage overlay(20, 20, QImage::Format_ARGB32);
        overlay.fill(qRgba(0, 0, 255, 80));
        sleep::Snapshot source;
        source.privateBook = true;
        source.percent = 42;
        source.progress = "42% read";
        source.details.title = "Private title";
        source.details.author = "Private author";
        source.details.chapter = "Private chapter";
        source.details.greeting = "Sleep well";
        for (const auto design : sleep::designs) {
            source.design = design;
            source.tile = sleep::Tile::Bookmark;
            QJsonObject preferences;
            sleep::setDesign(preferences, design);
            preferences[sleep::greetingKey] = source.details.greeting;
            auto expected = sleep::privateSnapshot(size, preferences, source);
            if (design == sleep::Design::Overlay)
                sleep::drawOverlay(expected.image, overlay);
            QImage preview(size, QImage::Format_RGB32);
            preview.fill(Qt::yellow);
            QPainter painter(&preview);
            reader::paintSleepPreview(painter, QRectF(QPointF(), QSizeF(size)), size, cover,
                                      pageImage, overlay, source);
            painter.end();
            QCOMPARE(preview,
                     reader::sleepDesignImage(expected.image, expected, -1,
                                              reader::SleepStatus::Sleeping));
        }
    }

    void sleepPrivateBookCloseSavesAnonymousDesign()
    {
        const auto paths = testPaths();
        QJsonObject preferences;
        sleep::setDesign(preferences, sleep::Design::Ring);
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.book()->setHideBookOnSleepScreen(true);
        host.window()->close();
        QTRY_VERIFY(host.idle());
        QVERIFY(state->saved.privateBook);
        QCOMPARE(state->saved.design, sleep::Design::Ring);
        QVERIFY(state->saved.percent >= 0 && !state->saved.progress.isEmpty());
        QVERIFY(state->saved.details.title.isEmpty() && state->saved.details.author.isEmpty()
                && state->saved.details.chapter.isEmpty() && state->saved.coverPath.isEmpty());
        host.open(coverBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QVERIFY(host.book()->hideBookOnSleepScreen());
    }

    void sleepPrivateBookRetainsNeutralFrameWhenHandoffFails()
    {
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [](ReaderHostServices &services) {
            services.presentation = [](QWidget *) {
                return std::make_unique<test::HostPresentation>();
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.book()->setHideBookOnSleepScreen(true);
        const auto privateShot = host.window()->powerScreenSnapshot();
        host.window()->sleepReader();
        auto *mask = host.window()->findChild<QLabel *>("rrPrivateSleepFrame");
        QVERIFY(mask && mask->isVisible());
        QCOMPARE(mask->grab().toImage().convertToFormat(QImage::Format_RGB32),
                 reader::sleepDesignImage(privateShot.image, privateShot, -1,
                                          reader::SleepStatus::Sleeping));
        host.window()->sleepViewReady();
        QVERIFY(host.window()->isVisible() && mask->isVisible());
        host.window()->wakeReader();
        QVERIFY(!host.window()->findChild<QLabel *>("rrPrivateSleepFrame"));
        QTRY_VERIFY(host.window()->readyForPresentation());
    }

    void sleepPrivateBookOverridesEveryDesign_data()
    {
        QTest::addColumn<QString>("design");
        QTest::addColumn<bool>("dark");
        for (const auto design : sleep::designs)
            for (bool dark : { false, true })
                QTest::newRow(qPrintable(sleep::value(design)
                                         + (dark ? "-dark-coverless" : "-light-cover")))
                        << sleep::value(design) << dark;
    }

    void sleepPrivateBookOverridesEveryDesign()
    {
        QFETCH(QString, design);
        QFETCH(bool, dark);
        const auto paths = testPaths();
        QJsonObject preferences{ { "dark_mode", dark } };
        for (const auto choice : sleep::designs)
            if (sleep::value(choice) == design)
                sleep::setDesign(preferences, choice);
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(dark ? textBook : coverBook, dark ? nativeSize : nativeSize.transposed());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.book()->setHideBookOnSleepScreen(true);
        const auto backdrop = sleep::privateSnapshot(dark ? nativeSize : nativeSize.transposed(),
                                                     preferences, {}, true);
        const auto awake = host.window()->powerScreenSnapshot();
        QCOMPARE(awake.image, backdrop.image);
        QCOMPARE(awake.mode, sleep::Mode::Plain);
        // Kobo's screen becomes the card; Custom becomes the status it chose.
        QCOMPARE(awake.design, backdrop.design);
        QVERIFY(awake.design != sleep::Design::Stock && awake.design != sleep::Design::Custom);
        QVERIFY(awake.privateBook);
        QCOMPARE(awake.percent,
                 host.session()->statsPercent(!host.session()->hasMainTextBoundary()));
        QVERIFY(awake.coverPath.isEmpty() && awake.coverArea.isEmpty());
        QVERIFY(awake.details.title.isEmpty() && awake.details.author.isEmpty()
                && awake.details.chapter.isEmpty() && awake.details.greeting.isEmpty());
        QVERIFY(!awake.progress.isEmpty());
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        const auto expected =
                reader::sleepDesignImage(awake.image, awake, 67, reader::SleepStatus::Sleeping);
        QCOMPARE(state->image, expected);
        const auto sleeping = host.window()->powerScreenSnapshot();
        QCOMPARE(sleeping.image, awake.image);
        QCOMPARE(reader::sleepDesignImage(sleeping.image, sleeping, 67,
                                          reader::SleepStatus::PoweredOff),
                 reader::sleepDesignImage(awake.image, awake, 67, reader::SleepStatus::PoweredOff));
        host.window()->wakeReader();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QVERIFY(state->image.isNull());
        QCOMPARE(storage::readPreferences(paths.preferences()).object.value(sleep::designKey),
                 preferences.value(sleep::designKey));
        QVERIFY(storage::readBookState(paths, host.book()->identity())
                        .object.value("hide_book_on_sleep_screen")
                        .toBool());
    }

    void sleepRandomStatusUntilWake_data()
    {
        QTest::addColumn<bool>("privateBook");
        QTest::addColumn<bool>("randomSleep");
        QTest::addColumn<bool>("randomOff");
        for (const bool hidden : { false, true }) {
            const QByteArray prefix = hidden ? "private-" : "";
            QTest::newRow(prefix + "sleep-and-power-off") << hidden << true << true;
            QTest::newRow(prefix + "sleep-only") << hidden << true << false;
            QTest::newRow(prefix + "power-off-only") << hidden << false << true;
        }
    }

    void sleepRandomStatusUntilWake()
    {
        QFETCH(bool, privateBook);
        QFETCH(bool, randomSleep);
        QFETCH(bool, randomOff);
        const auto paths = testPaths();
        QTemporaryDir wallpapers;
        QVERIFY(wallpapers.isValid());
        QImage wallpaper(32, 32, QImage::Format_RGB32);
        wallpaper.fill(QColor(195, 65, 35));
        QVERIFY(wallpaper.save(wallpapers.filePath("background.png")));
        QJsonObject preferences;
        sleep::Custom custom;
        custom.same = false;
        custom.sleep = { sleep::Mode::Wallpaper,
                         randomSleep ? sleep::Design::Random : sleep::Design::Bookmark,
                         "background.png" };
        custom.off = { sleep::Mode::Plain,
                       randomOff ? sleep::Design::Random : sleep::Design::Seal,
                       {} };
        sleep::setCustom(preferences, custom);
        sleep::setDesign(preferences, sleep::Design::Custom);
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        const auto state = std::make_shared<test::SleepPresentationState>();
        BatteryStatus battery{ 67, false, false };
        std::function<void()> changed;
        host.configureServices = [&](ReaderHostServices &services) {
            services.sleepWallpaperDirectories = { wallpapers.path() };
            services.battery = [&] { return battery; };
            services.observeBattery = [&](QObject *, std::function<void()> notify) {
                changed = std::move(notify);
            };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QTRY_VERIFY(host.window()->property("sleepWallpaperReady").toBool());
        host.book()->setHideBookOnSleepScreen(privateBook);
        QVERIFY(changed);
        auto previousPick = sleep::Design::Stock;
        for (int cycle = 0; cycle < 2; ++cycle) {
            battery.charging = false;
            changed();
            host.window()->sleepReader();
            host.window()->sleepViewReady();
            const auto off = host.window()->powerScreenSnapshot();
            QCOMPARE(off.mode, sleep::Mode::Plain);
            QCOMPARE(off.privateBook, privateBook);
            QVERIFY(!off.image.isNull() && !state->image.isNull());
            if (randomOff)
                QVERIFY(sleep::eligibleStatus(off.design));
            else
                QCOMPARE(off.design, custom.off.status);
            if (privateBook) {
                QVERIFY(off.details.title.isEmpty() && off.details.author.isEmpty()
                        && off.details.chapter.isEmpty() && off.coverPath.isEmpty());
            }
            // Identify the sleeping status from the presented frame. The power-off
            // snapshot has the same statistics, but uses a different background.
            const auto background = privateBook
                    ? off.image
                    : reader::sleepWallpaperBackground(wallpaper, off.image.size());
            const auto sleepingFrame = [&](sleep::Design status, reader::SleepStatus power) {
                auto shot = off;
                shot.design = status;
                shot.mode = privateBook ? sleep::Mode::Plain : custom.sleep.backdrop;
                shot.tile = sleep::tileFor(status);
                shot.place = sleep::defaultPlace(status);
                return reader::sleepDesignImage(background, shot, battery.percent, power);
            };
            auto sleeping = sleep::Design::Stock;
            for (const auto candidate : sleep::customStatuses) {
                if (sleep::eligibleStatus(candidate)
                    && state->image == sleepingFrame(candidate, reader::SleepStatus::Sleeping)) {
                    sleeping = candidate;
                    break;
                }
            }
            QVERIFY(sleep::eligibleStatus(sleeping));
            if (!randomSleep)
                QCOMPARE(sleeping, custom.sleep.status);
            if (randomSleep && randomOff)
                QCOMPARE(sleeping, off.design);
            const auto pick = randomSleep ? sleeping : off.design;
            QVERIFY(pick != previousPick);
            previousPick = pick;
            host.window()->sleepReader();
            host.window()->sleepViewReady();
            battery.charging = true;
            changed();
            QCOMPARE(state->image, sleepingFrame(sleeping, reader::SleepStatus::Charging));
            QCOMPARE(host.window()->powerScreenSnapshot().design, off.design);
            QCOMPARE(state->presented, cycle + 1);
            host.window()->wakeReader();
            QTRY_VERIFY(host.window()->readyForPresentation());
        }
        const auto saved = storage::readPreferences(paths.preferences()).object;
        QCOMPARE(saved.value(sleep::customKey), preferences.value(sleep::customKey));
        // Power-off can also be the first request after waking.
        const auto direct = host.window()->powerScreenSnapshot().design;
        if (randomOff) {
            QVERIFY(sleep::eligibleStatus(direct));
            QVERIFY(direct != previousPick);
        } else {
            QCOMPARE(direct, custom.off.status);
        }
        QCOMPARE(host.window()->powerScreenSnapshot().design, direct);
    }

    void sleepPowerOffWithoutCoverKeepsDesign_data()
    {
        QTest::addColumn<QString>("design");
        QTest::addColumn<bool>("landscape");
        QTest::addColumn<bool>("dark");
        for (const auto *design : { "seal", "medallion", "pill", "caption", "widgets", "framed" })
            for (const bool landscape : { false, true })
                for (const bool dark : { false, true }) {
                    const auto name = QString::fromLatin1(design)
                            + (landscape ? "-landscape" : "-portrait")
                            + (dark ? "-dark" : "-light");
                    QTest::newRow(qPrintable(name))
                            << QString::fromLatin1(design) << landscape << dark;
                }
    }

    // A coverless book must replace Sleeping with Powered off, also after sleep.
    void sleepPowerOffWithoutCoverKeepsDesign()
    {
        QFETCH(QString, design);
        QFETCH(bool, landscape);
        QFETCH(bool, dark);
        const auto paths = testPaths();
        QVERIFY(storage::writePreferences(paths.preferences(),
                                          { { sleep::designKey, design }, { "dark_mode", dark } }));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [state](ReaderHostServices &services) {
            services.battery = [] { return BatteryStatus{ 67, false, false }; };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, landscape ? nativeSize.transposed() : nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        bool coverReady = false;
        host.session()->coverImage({ 2048, 2048 }, [&](QImage image) {
            QVERIFY(image.isNull());
            coverReady = true;
        });
        QTRY_VERIFY(coverReady);
        const auto awake = host.window()->powerScreenSnapshot();
        QVERIFY(!awake.image.isNull());
        const auto expected =
                reader::sleepDesignImage(awake.image, awake, 67, reader::SleepStatus::PoweredOff);
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        QVERIFY(!state->image.isNull());
        const auto sleeping = host.window()->powerScreenSnapshot();
        QCOMPARE(sleeping.design, awake.design);
        const auto poweredOff = reader::sleepDesignImage(sleeping.image, sleeping, 67,
                                                         reader::SleepStatus::PoweredOff);
        QCOMPARE(poweredOff, expected);
        QVERIFY(poweredOff != state->image);
    }

    // Kobalt's folder joins Nickel's scan exclusion without losing the
    // reader's own, and leaves it as it was on removal.
    void sleepKobaltFolderStaysOutOfNickelLibrary()
    {
        using namespace kobalt::nickel;
        const QRegularExpression own(QStringLiteral("^(?:") + withKobaltFolder({})
                                     + QStringLiteral(")$"));
        QVERIFY(own.match(".adds/kobalt/wallpapers").hasMatch());
        QVERIFY(own.match(".adds/kobalt").hasMatch());
        QVERIFY(!own.match(".adds/nm").hasMatch() && !own.match("Books").hasMatch());

        // A reader's calibre exclusion keeps working beside Kobalt's.
        const QString calibre = QStringLiteral("calibre");
        const auto both = withKobaltFolder(calibre);
        const QRegularExpression merged(QStringLiteral("^(?:") + both + QStringLiteral(")$"));
        QVERIFY(merged.match("calibre").hasMatch() && merged.match(".adds/kobalt/x").hasMatch());
        QCOMPARE(withKobaltFolder(both), both);
        QTemporaryDir root;
        QSettings settings(root.filePath("nickel.ini"), QSettings::IniFormat);
        const QString key = QStringLiteral("FeatureSettings/ExcludeSyncFolders");
        const QString original = QStringLiteral(R"((books)/\1)");
        settings.setValue(key, original);
        QVERIFY(excludeKobaltFolder(settings, true));
        const QRegularExpression expression(settings.value(key).toString());
        QVERIFY(expression.match("books/books").hasMatch());
        QVERIFY(excludeKobaltFolder(settings, true));
        QVERIFY(excludeKobaltFolder(settings, false));
        QCOMPARE(settings.value(key).toString(), original);
        settings.setValue(key, both);
        QVERIFY(excludeKobaltFolder(settings, true));
        QVERIFY(excludeKobaltFolder(settings, false));
        QCOMPARE(settings.value(key).toString(), both);
        settings.remove(key);
        QVERIFY(excludeKobaltFolder(settings, true));
        settings.setValue(key, "custom");
        QVERIFY(excludeKobaltFolder(settings, false));
        QCOMPARE(settings.value(key).toString(), QStringLiteral("custom"));
        // The installer's dot-folder rule already covers Kobalt's folder.
        const QString dots = QStringLiteral(R"((\.(?!kobo|adobe).+|([^.][^/]*/)+\..+))");
        // As Nickel reads it, after QSettings unescapes the file.
        QCOMPARE(withKobaltFolder(dots), dots);
        // An invalid expression stays; Nickel ignores it either way.
        QCOMPARE(withKobaltFolder(QStringLiteral("(")), QStringLiteral("("));
    }

    // A Custom screen: a wallpaper and a moved seal while sleeping, its own
    // look when powered off, and the book's own position over the global one.
    void sleepCustomPowerOffUsesItsOwnLook()
    {
        const auto paths = testPaths();
        QTemporaryDir wallpapers;
        QVERIFY(wallpapers.isValid());
        QImage wallpaper(300, 200, QImage::Format_RGB32);
        wallpaper.fill(QColor(200, 30, 30));
        QVERIFY(wallpaper.save(wallpapers.filePath("red.png")));
        QVERIFY(storage::sleepWallpapers({ wallpapers.path() }) == QStringList{ "red.png" });
        sleep::Custom custom;
        custom.same = false;
        custom.sleep = { sleep::Mode::Wallpaper, sleep::Design::Seal, QStringLiteral("red.png") };
        custom.off = { sleep::Mode::Plain, sleep::Design::Pill, {} };
        QJsonObject preferences;
        sleep::setCustom(preferences, custom);
        sleep::setDesign(preferences, sleep::Design::Custom);
        QJsonObject global;
        sleep::setPlace(global, sleep::Design::Seal, sleep::Place::TopLeft);
        preferences[sleep::placesKey] = global;
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto folder = wallpapers.path();
        host.configureServices = [state, folder](ReaderHostServices &services) {
            services.battery = [] { return BatteryStatus{ 67, false, false }; };
            services.sleepWallpaperDirectories = { folder };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto off = host.window()->powerScreenSnapshot();
        QCOMPARE(off.design, sleep::Design::Pill);
        QCOMPARE(off.mode, sleep::Mode::Plain);
        QCOMPARE(off.place, sleep::Place::Default);
        QTRY_VERIFY(host.window()->property("sleepWallpaperReady").toBool());
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        const auto image = state->image;
        QVERIFY(!image.isNull());
        // The wallpaper fills the screen; the seal sits in the top-left corner.
        const auto red = [&](QPoint point) {
            const QColor color(image.pixel(point));
            return color.red() > 150 && color.green() < 80 && color.blue() < 80;
        };
        const int width = image.width(), height = image.height();
        QVERIFY(red(QPoint(width / 2, height * 3 / 4)));
        QVERIFY(!red(QPoint(width * 24 / 100, height * 4 / 100 + width * 19 / 100)));
        QVERIFY(red(QPoint(width * 76 / 100, height * 96 / 100 - width * 19 / 100)));
        const auto asleep = host.window()->powerScreenSnapshot();
        QCOMPARE(asleep.design, sleep::Design::Pill);
        QCOMPARE(asleep.mode, sleep::Mode::Plain);
        host.window()->wakeReader();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QJsonObject own;
        own[sleep::value(sleep::Design::Pill)] = sleep::value(sleep::Place::Top);
        host.book()->setSleepPositions(own);
        off = host.window()->powerScreenSnapshot();
        QCOMPARE(off.place, sleep::Place::Top);
    }

    // Power-off after sleep draws a fresh backdrop for every Custom look: the
    // same screen with its own status over a wallpaper, and a power-off look
    // that differs only in its wallpaper.
    void sleepCustomPowerOffRedrawsItsWallpaper_data()
    {
        QTest::addColumn<bool>("same");
        QTest::newRow("same-screen") << true;
        QTest::newRow("other-wallpaper") << false;
    }

    void sleepCustomPowerOffRedrawsItsWallpaper()
    {
        QFETCH(bool, same);
        const auto paths = testPaths();
        QTemporaryDir wallpapers;
        QVERIFY(wallpapers.isValid());
        QImage red(300, 200, QImage::Format_RGB32), blue(300, 200, QImage::Format_RGB32);
        red.fill(QColor(200, 30, 30));
        blue.fill(QColor(30, 30, 200));
        QVERIFY(red.save(wallpapers.filePath("red.png")));
        QVERIFY(blue.save(wallpapers.filePath("blue.png")));
        sleep::Custom custom;
        custom.same = same;
        custom.sleep = { sleep::Mode::Wallpaper, sleep::Design::Seal, QStringLiteral("red.png") };
        custom.off = { sleep::Mode::Wallpaper, sleep::Design::Seal, QStringLiteral("blue.png") };
        QJsonObject preferences;
        sleep::setCustom(preferences, custom);
        sleep::setDesign(preferences, sleep::Design::Custom);
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto folder = wallpapers.path();
        host.configureServices = [state, folder](ReaderHostServices &services) {
            services.battery = [] { return BatteryStatus{ 67, false, false }; };
            services.sleepWallpaperDirectories = { folder };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QTRY_VERIFY(host.window()->property("sleepWallpaperReady").toBool());
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        const auto off = host.window()->powerScreenSnapshot();
        QCOMPARE(off.design, sleep::Design::Seal);
        QCOMPARE(off.mode, sleep::Mode::Wallpaper);
        // The seal's centre is plain wallpaper in a fresh backdrop.
        const QPoint seal(off.image.width() * 76 / 100,
                          off.image.height() * 96 / 100 - off.image.width() * 19 / 100);
        const QColor colour(off.image.pixel(seal));
        if (same)
            QVERIFY(colour.red() > 150 && colour.blue() < 80);
        else
            QVERIFY(colour.blue() > 150 && colour.red() < 80);
        host.window()->wakeReader();
    }

    void sleepScreenDimsOnlyItsOwnedPage_data()
    {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<bool>("dark");
        for (int ppi : { 212, 300 }) {
            QTest::newRow(qPrintable(QString::number(ppi) + "-light")) << ppi << false;
            QTest::newRow(qPrintable(QString::number(ppi) + "-dark")) << ppi << true;
        }
    }

    void sleepScreenDimsOnlyItsOwnedPage()
    {
        QFETCH(int, ppi);
        QFETCH(bool, dark);
        const int previous = ui::density();
        ui::setDensity(ppi);
        QImage page(1072, 1448, QImage::Format_RGB32);
        page.fill(dark ? QColor(48, 48, 48) : QColor(Qt::white));
        const auto original = page;
        const auto frame = reader::sleepScreenImage(page, "42% read", "37 min to go", dark);
        QCOMPARE(page, original);
        QCOMPARE(frame.size(), page.size());
        QVERIFY(qGray(frame.pixel(1000, 200)) < qGray(page.pixel(1000, 200)));
        QVERIFY(frame != reader::sleepScreenImage(page, "42% read", {}, dark));
        QVERIFY(reader::sleepScreenImage({}, "42% read", {}, dark).isNull());
        for (const int percent : { 0, 67, 100 }) {
            QCOMPARE(reader::sleepScreenImage(page, "42% read", "37 min to go", dark, percent,
                                              reader::SleepStatus::Sleeping),
                     frame);
        }
        const auto charging = reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 67,
                                                       reader::SleepStatus::Charging);
        const auto charged = reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 67,
                                                      reader::SleepStatus::Charged);
        QVERIFY(charging != charged);
        QVERIFY(charging != frame && charged != frame);
        QVERIFY(charged
                != reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 100,
                                            reader::SleepStatus::Charged));
        QVERIFY(charging
                != reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 23,
                                            reader::SleepStatus::Charging));
        QVERIFY(charging
                != reader::sleepScreenImage(page, "42% read", "37 min to go", dark, -1,
                                            reader::SleepStatus::Charging));
        QCOMPARE(reader::sleepStatusText(reader::SleepStatus::Sleeping), QString("Sleeping"));
        QCOMPARE(reader::sleepStatusText(reader::SleepStatus::Charging), QString("Charging"));
        QCOMPARE(reader::sleepStatusText(reader::SleepStatus::Charged), QString("Charged"));
        ui::setDensity(previous);
    }

    void sleepTilesRepaintOnlyTheirArea_data()
    {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("tileIndex");
        for (const int ppi : { 212, 300 }) {
            for (const auto tile : sleep::tiles) {
                const auto name = QString::number(ppi) + "-" + sleep::value(tile);
                QTest::newRow(qPrintable(name + "-light")) << ppi << false << int(tile);
                QTest::newRow(qPrintable(name + "-dark")) << ppi << true << int(tile);
            }
        }
    }

    void sleepTilesRepaintOnlyTheirArea()
    {
        QFETCH(int, ppi);
        QFETCH(bool, dark);
        QFETCH(int, tileIndex);
        const auto tile = sleep::Tile(tileIndex);
        const int previous = ui::density();
        ui::setDensity(ppi);
        for (const QSize size : { QSize(1072, 1448), QSize(1448, 1072) }) {
            QImage page(size, QImage::Format_RGB32);
            page.fill(QColor(150, 150, 150));
            const auto paint = [&](const QImage &background, reader::SleepStatus status, bool dim,
                                   int percent = 42) {
                return reader::sleepScreenImage(background, "42% read", "3 h 37 min to go", dark,
                                                67, status, dim, tile, percent);
            };
            const auto area = reader::sleepScreenCardRect(size, true, tile);
            const auto sleeping = paint(page, reader::SleepStatus::Sleeping, true);
            const auto charging = paint(page, reader::SleepStatus::Charging, true);
            const auto poweredOff = paint(page, reader::SleepStatus::PoweredOff, true);
            if (tile == sleep::Tile::None) {
                QVERIFY(area.isEmpty());
                QVERIFY(sleeping != page);
                QCOMPARE(charging, sleeping);
                QCOMPARE(poweredOff, sleeping);
                QCOMPARE(paint(page, reader::SleepStatus::Sleeping, false), page);
                continue;
            }
            QVERIFY(QRect(QPoint(), size).contains(area));
            QVERIFY(sleeping != charging);
            QVERIFY(sleeping != poweredOff);
            if (tile == sleep::Tile::Bookmark) {
                const QPoint fill = area.topLeft() + QPoint(area.width() / 2, 1);
                const QPoint outline = area.topLeft() + QPoint(0, 1);
                QCOMPARE(sleeping.pixelColor(fill), ui::themeColors(dark).sleepPaper);
                QCOMPARE(sleeping.pixelColor(outline), ui::themeColors(dark).ink);
                QCOMPARE(charging.pixelColor(fill), ui::themeColors(dark).sleepPaper);
                QCOMPARE(poweredOff.pixelColor(fill), ui::themeColors(true).paper);
            }
            // A battery update refreshes only the tile's area, so nothing outside it may change.
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    if (!area.contains(x, y) && charging.pixel(x, y) != sleeping.pixel(x, y))
                        QFAIL(qPrintable(
                                QString("tile changed %1,%2 outside its area").arg(x).arg(y)));
                }
            }
            // A battery update paints over the retained frame and must give the
            // same pixels as painting the new status once.
            QCOMPARE(paint(charging, reader::SleepStatus::Sleeping, false), sleeping);
            QCOMPARE(paint(sleeping, reader::SleepStatus::Charging, false), charging);
            if (tile == sleep::Tile::Card)
                QCOMPARE(paint(page, reader::SleepStatus::Sleeping, true, 90), sleeping);
            else
                QVERIFY(paint(page, reader::SleepStatus::Sleeping, true, 90) != sleeping);
            // Snapshots saved without a percentage still paint.
            QVERIFY(!paint(page, reader::SleepStatus::Sleeping, true, -1).isNull());
            const auto capture = qEnvironmentVariable("KOBALT_SLEEP_TILE_CAPTURE_ROOT");
            if (!capture.isEmpty()) {
                QVERIFY(QDir().mkpath(capture));
                const auto name =
                        QString("%1/%2-%3-%4-%5")
                                .arg(capture, sleep::value(tile))
                                .arg(ppi)
                                .arg(dark ? "dark" : "light")
                                .arg(size.width() > size.height() ? "landscape" : "portrait");
                QVERIFY(sleeping.save(name + "-sleeping.png"));
                QVERIFY(charging.save(name + "-charging.png"));
                QVERIFY(poweredOff.save(name + "-powered-off.png"));
            }
        }
        ui::setDensity(previous);
    }

    void sleepPillOmitsUnknownProgress()
    {
        sleep::Snapshot shot;
        shot.design = sleep::Design::Pill;
        shot.mode = sleep::Mode::Plain;
        shot.dimPage = false;
        QImage paper(480, 640, QImage::Format_RGB32);
        paper.fill(Qt::white);
        for (auto status : { reader::SleepStatus::Sleeping, reader::SleepStatus::Charging }) {
            const auto pillWidth = [&](int percent) {
                shot.percent = percent;
                const auto image = reader::sleepDesignImage(paper, shot, 67, status);
                QRect ink;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x)
                        if (image.pixel(x, y) != paper.pixel(x, y))
                            ink |= QRect(x, y, 1, 1);
                return ink.width();
            };
            const int unknown = pillWidth(-1);
            QVERIFY(unknown > 0);
            QVERIFY2(unknown < pillWidth(0),
                     "Unknown progress still occupies the percentage field");
        }
    }

    void sleepDesignsRepaintOnlyTheirStatus_data()
    {
        QTest::addColumn<int>("designIndex");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("longMetadata");
        QTest::addColumn<bool>("privateBook");
        QTest::addColumn<int>("place");
        const int standard = int(sleep::Place::Default);
        for (const auto design : sleep::designs) {
            // Custom reaches the painters as the status it chose.
            if (sleep::tiled(design) || design == sleep::Design::Stock
                || design == sleep::Design::Custom)
                continue;
            QTest::newRow(qPrintable(sleep::value(design) + "-light"))
                    << int(design) << false << false << false << standard;
            QTest::newRow(qPrintable(sleep::value(design) + "-dark"))
                    << int(design) << true << false << false << standard;
            for (bool dark : { false, true })
                QTest::newRow(
                        qPrintable(sleep::value(design) + (dark ? "-long-dark" : "-long-light")))
                        << int(design) << dark << true << false << standard;
            for (bool dark : { false, true })
                QTest::newRow(qPrintable(sleep::value(design)
                                         + (dark ? "-private-dark" : "-private-light")))
                        << int(design) << dark << false << true << standard;
            // Every other place of a status that moves, with long metadata.
            for (const auto place : sleep::places(design))
                if (place != sleep::defaultPlace(design))
                    QTest::newRow(qPrintable(sleep::value(design) + "-" + sleep::value(place)))
                            << int(design) << false << true << false << int(place);
        }
    }

    // The gallery's newer designs: a battery change may alter only their status
    // areas, and repainting those areas gives the pixels of a fresh screen.
    void sleepDesignsRepaintOnlyTheirStatus()
    {
        QFETCH(int, designIndex);
        QFETCH(bool, dark);
        QFETCH(bool, longMetadata);
        QFETCH(bool, privateBook);
        QFETCH(int, place);
        const auto design = sleep::Design(designIndex);
        const int previous = ui::density();
        ui::setDensity(300);
        // A stand-in cover: pale paper with dark title bars, in the sample book's shape.
        // Review captures can pass a real cover instead.
        QImage cover(600, 851, QImage::Format_RGB32);
        cover.fill(QColor(236, 230, 218));
        const QImage supplied(qEnvironmentVariable("KOBALT_SLEEP_CAPTURE_COVER"));
        if (!supplied.isNull())
            cover = supplied.convertToFormat(QImage::Format_RGB32);
        else {
            QPainter painter(&cover);
            painter.fillRect(QRect(70, 180, 430, 90), QColor(30, 26, 22));
            painter.fillRect(QRect(70, 290, 460, 90), QColor(30, 26, 22));
            painter.fillRect(QRect(70, 600, 400, 12), QColor(60, 54, 48));
        }
        for (const QSize size : { QSize(1072, 1448), QSize(1448, 1072) }) {
            sleep::Snapshot shot;
            shot.design = design;
            shot.privateBook = privateBook;
            shot.mode = sleep::backdrop(design);
            shot.tile = sleep::Tile::None;
            shot.place = sleep::Place(place);
            shot.dimPage = false;
            shot.dark = dark;
            shot.percent = 42;
            shot.progress = "42% read";
            shot.remaining = "3 h 37 min to go";
            shot.details.title = "Trevelyan’s Type Tester";
            shot.details.author = "Nicholas Farbridge";
            shot.details.chapter = "A Decimal Point Misplaced";
            shot.details.clock = "22:14";
            shot.details.left = "3 h 37 min";
            shot.details.todayMilliseconds = 34 * 60000;
            if (longMetadata) {
                shot.details.title = QString("A remarkably long book title ").repeated(80);
                shot.details.chapter = QString("A chapter with a long name ").repeated(80);
                shot.details.author = QString("An author with many names ").repeated(80);
                shot.details.greeting = QString::fromUtf8("Sweet dreams 🌙 ").repeated(20);
            }
            QImage backdrop;
            if (shot.mode == sleep::Mode::Cover) {
                backdrop = sleep::coverBackground(cover, size);
                shot.coverArea = sleep::coverArea(cover.size(), size);
            } else {
                backdrop = QImage(size, QImage::Format_RGB32);
                backdrop.fill(Qt::white);
            }
            if (privateBook) {
                backdrop.fill(shot.mode == sleep::Mode::Cover ? Qt::black : Qt::white);
                shot.coverArea = {};
            }
            QVector<QImage> underlay;
            const auto paint = [&](reader::SleepStatus status, QVector<QImage> *saved = nullptr) {
                return reader::sleepDesignImage(backdrop, shot, 67, status, saved);
            };
            const auto sleeping = paint(reader::SleepStatus::Sleeping, &underlay);
            const auto charging = paint(reader::SleepStatus::Charging);
            const auto poweredOff = paint(reader::SleepStatus::PoweredOff);
            QVERIFY(sleeping != backdrop);
            QVERIFY(sleeping != charging);
            QVERIFY(sleeping != poweredOff);
            const auto areas = reader::sleepStatusAreas(size, shot);
            QVERIFY(!areas.isEmpty());
            // Night's battery line is empty while asleep. Its saved background
            // includes the stars, ready for the next charging update.
            if (design == sleep::Design::Night) {
                QVERIFY(!underlay.isEmpty());
                QCOMPARE(underlay.first(), sleeping.copy(areas.first()));
                auto withoutUnderlay = sleeping;
                reader::repaintSleepStatus(withoutUnderlay, {}, shot, 67,
                                           reader::SleepStatus::Charging);
                QCOMPARE(withoutUnderlay, charging);
            }
            QRegion region;
            for (const auto &area : areas) {
                QVERIFY(QRect(QPoint(), size).contains(area));
                region += area;
            }
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x)
                    if (!region.contains(QPoint(x, y))
                        && charging.pixel(x, y) != sleeping.pixel(x, y))
                        QFAIL(qPrintable(QString("%1 changed %2,%3 outside its status")
                                                 .arg(sleep::value(design))
                                                 .arg(x)
                                                 .arg(y)));
            auto repainted = sleeping;
            const auto refreshed = reader::repaintSleepStatus(repainted, underlay, shot, 67,
                                                              reader::SleepStatus::Charging);
            QVERIFY(!refreshed.isEmpty());
            QVERIFY(region.boundingRect() == refreshed);
            QCOMPARE(repainted, charging);
            reader::repaintSleepStatus(repainted, underlay, shot, 67,
                                       reader::SleepStatus::Sleeping);
            QCOMPARE(repainted, sleeping);
            // Night shows the reader's own greeting while asleep, if one is set.
            QImage greeted;
            if (design == sleep::Design::Night) {
                auto custom = shot;
                custom.details.greeting = "Sweet dreams";
                greeted = reader::sleepDesignImage(backdrop, custom, 67,
                                                   reader::SleepStatus::Sleeping);
                QVERIFY(greeted != sleeping);
                QCOMPARE(reader::sleepDesignImage(backdrop, custom, 67,
                                                  reader::SleepStatus::Charging),
                         charging);
            }
            const auto capture = qEnvironmentVariable("KOBALT_SLEEP_TILE_CAPTURE_ROOT");
            if (!capture.isEmpty()) {
                QVERIFY(QDir().mkpath(capture));
                const auto name =
                        QString(longMetadata ? "%1/design-long-%2-%3-%4" : "%1/design-%2-%3-%4")
                                .arg(capture, sleep::value(design))
                                .arg(dark ? "dark" : "light")
                                .arg(size.width() > size.height() ? "landscape" : "portrait");
                QVERIFY(sleeping.save(name + "-sleeping.png"));
                QVERIFY(charging.save(name + "-charging.png"));
                QVERIFY(poweredOff.save(name + "-powered-off.png"));
                if (!greeted.isNull())
                    QVERIFY(greeted.save(name + "-greeting.png"));
            }
        }
        ui::setDensity(previous);
    }

    void sleepMetadataStaysInsideItsLayout_data()
    {
        QTest::addColumn<int>("designIndex");
        QTest::addColumn<QString>("metadata");
        for (const auto design : { sleep::Design::Caption, sleep::Design::Widgets,
                                   sleep::Design::Ring, sleep::Design::Letter }) {
            QTest::newRow(qPrintable(sleep::value(design) + "-words"))
                    << int(design) << QString("A very long book and chapter title ").repeated(80);
            QTest::newRow(qPrintable(sleep::value(design) + "-unbroken"))
                    << int(design) << (QString(500, QLatin1Char('W')) + " another word");
            QTest::newRow(qPrintable(sleep::value(design) + "-unicode"))
                    << int(design)
                    << QString::fromUtf8("書名 العربية é 👩🏽‍🚀 ")
                               .repeated(100);
            QTest::newRow(qPrintable(sleep::value(design) + "-whitespace"))
                    << int(design) << QString("Title\n\tSubtitle\rChapter ").repeated(100);
            QTest::newRow(qPrintable(sleep::value(design) + "-markup"))
                    << int(design) << QString("<b>Literal & title %2 %3</b> ").repeated(100);
            QTest::newRow(qPrintable(sleep::value(design) + "-huge"))
                    << int(design) << QString(100000, QLatin1Char('W'));
        }
    }

    void sleepMetadataStaysInsideItsLayout()
    {
        QFETCH(int, designIndex);
        QFETCH(QString, metadata);
        const auto design = sleep::Design(designIndex);
        for (const QSize size : { QSize(1072, 1448), QSize(1448, 1072) }) {
            const QSizeF frame = QSizeF(1072, 1448).scaled(size, Qt::KeepAspectRatio);
            const qreal x = (size.width() - frame.width()) / 2;
            const qreal y = (size.height() - frame.height()) / 2;
            QRect allowed;
            switch (design) {
            case sleep::Design::Caption:
                allowed = QRectF(x, y + frame.height() * .54, frame.width(), frame.height() * .46)
                                  .toAlignedRect();
                break;
            case sleep::Design::Widgets:
                allowed = QRectF(x, y + frame.height() * .24, frame.width(), frame.height() * .70)
                                  .toAlignedRect();
                break;
            case sleep::Design::Ring:
                allowed = QRectF(x + frame.width() * .28, y + frame.height() * .52,
                                 frame.width() * .44, frame.height() * .10)
                                  .toAlignedRect();
                break;
            default:
                allowed = QRectF(x, y + frame.height() * .20, frame.width(), frame.height() * .54)
                                  .toAlignedRect();
                break;
            }
            for (bool dark : { false, true }) {
                sleep::Snapshot shot;
                shot.design = design;
                shot.dark = dark;
                shot.dimPage = false;
                shot.percent = 42;
                // Compare two titled layouts. An absent title intentionally
                // centres Ring's percentage instead of reserving its title area.
                shot.details.title = "Title";
                shot.details.chapter = "Chapter";
                shot.details.author = "Author";
                shot.details.left = "3 h 37 min";
                QImage backdrop(size, QImage::Format_RGB32);
                backdrop.fill(Qt::black);
                const auto reference =
                        reader::sleepDesignImage(backdrop, shot, 67, reader::SleepStatus::Charging);
                shot.details.title = metadata;
                shot.details.chapter = metadata;
                shot.details.author = metadata;
                const auto actual =
                        reader::sleepDesignImage(backdrop, shot, 67, reader::SleepStatus::Charging);
                QVERIFY(actual != reference);
                const auto capture = qEnvironmentVariable("KOBALT_SLEEP_TILE_CAPTURE_ROOT");
                if (!capture.isEmpty()) {
                    QVERIFY(QDir().mkpath(capture));
                    QVERIFY(actual.save(QString("%1/long-%2-%3-%4-%5.png")
                                                .arg(capture, sleep::value(design),
                                                     QString::fromLatin1(QTest::currentDataTag()))
                                                .arg(dark ? "dark" : "light")
                                                .arg(size.width())));
                }
                for (int row = 0; row < size.height(); ++row)
                    for (int col = 0; col < size.width(); ++col)
                        if (!allowed.contains(col, row)
                            && actual.pixel(col, row) != reference.pixel(col, row))
                            QFAIL(qPrintable(QString("Metadata overwrote reserved content at %1,%2")
                                                     .arg(col)
                                                     .arg(row)));
            }
        }
    }

    void sleepLetterPreservesLiteralPlaceholders()
    {
        sleep::Snapshot shot;
        shot.design = sleep::Design::Letter;
        shot.percent = 42;
        shot.dimPage = false;
        QImage backdrop(1072, 1448, QImage::Format_RGB32);
        backdrop.fill(Qt::white);
        const auto empty =
                reader::sleepDesignImage(backdrop, shot, 67, reader::SleepStatus::Sleeping);
        shot.details.title = "%3";
        const auto literal =
                reader::sleepDesignImage(backdrop, shot, 67, reader::SleepStatus::Sleeping);
        QVERIFY2(literal != empty,
                 "A title containing %3 must not disappear as a template placeholder");
    }

    // The reader hands off its portrait direction without changing its reading layout.
    void sleepHandoffKeepsPortraitDirection_data()
    {
        QTest::addColumn<int>("readingOrientation");
        QTest::addColumn<bool>("privateBook");
        QTest::addColumn<bool>("followReading");
        for (auto orientation : { Qt::PortraitOrientation, Qt::InvertedPortraitOrientation,
                                  Qt::LandscapeOrientation, Qt::InvertedLandscapeOrientation }) {
            for (bool privateBook : { false, true }) {
                for (bool followReading : { false, true }) {
                    QTest::newRow(qPrintable(QString("%1-private-%2-follow-%3")
                                                     .arg(int(orientation))
                                                     .arg(privateBook)
                                                     .arg(followReading)))
                            << int(orientation) << privateBook << followReading;
                }
            }
        }
    }

    void sleepHandoffKeepsPortraitDirection()
    {
        QFETCH(int, readingOrientation);
        QFETCH(bool, privateBook);
        QFETCH(bool, followReading);
        auto *screen = QGuiApplication::primaryScreen();
        const auto original = screen->orientation();
        const auto turn = [screen](Qt::ScreenOrientation value) {
            QWindowSystemInterface::handleScreenOrientationChange(screen, value);
            QWindowSystemInterface::flushWindowSystemEvents();
        };
        const auto restore = qScopeGuard([&] { turn(original); });
        turn(Qt::ScreenOrientation(readingOrientation));
        const auto paths = testPaths();
        QVERIFY(storage::writePreferences(paths.preferences(),
                                          { { sleep::designKey, "letter" },
                                            { sleep::followOrientationKey, followReading } }));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        const bool landscape = readingOrientation == Qt::LandscapeOrientation
                || readingOrientation == Qt::InvertedLandscapeOrientation;
        host.open(fixture(), landscape ? QSize(1448, 1072) : QSize(1072, 1448));
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        const auto readingSize = page->size();
        QCOMPARE(readingSize.width() > readingSize.height(), landscape);
        const auto readingPage = host.session()->page();
        host.book()->setHideBookOnSleepScreen(privateBook);
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        const auto expectedOrientation = followReading ? Qt::ScreenOrientation(readingOrientation)
                : readingOrientation == Qt::InvertedPortraitOrientation
                ? Qt::InvertedPortraitOrientation
                : Qt::PortraitOrientation;
        QCOMPARE(state->orientation, expectedOrientation);
        QCOMPARE(state->image.size(),
                 landscape && !followReading ? readingSize.transposed() : readingSize);
        turn(Qt::PortraitOrientation);
        QCOMPARE(host.window()->powerScreenSnapshot().orientation, expectedOrientation);
        turn(Qt::ScreenOrientation(readingOrientation));
        host.window()->wakeReader();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QCOMPARE(page->size(), readingSize);
        QCOMPARE(host.session()->page(), readingPage);
    }

    void sleepClassifiesReaderOverlay()
    {
        QVERIFY(sleep::needsImageCleanup({}, false));
        QImage frame(240, 320, QImage::Format_RGB32);
        for (bool dark : { false, true }) {
            frame.fill(dark ? Qt::black : Qt::white);
            QPainter p(&frame);
            p.setPen(dark ? Qt::white : Qt::black);
            for (int y = 15; y < frame.height(); y += 16)
                p.drawText(5, y, QStringLiteral("An ordinary page of text."));
            p.end();
            QVERIFY(!sleep::needsImageCleanup(frame, false));
            QVERIFY(!sleep::needsImageCleanup(frame, true));
        }
        frame.fill(Qt::white);
        QPainter p(&frame);
        p.fillRect(0, 0, 60, 80, Qt::red);
        p.end();
        QVERIFY(sleep::needsImageCleanup(frame, true));
        QVERIFY(!sleep::needsImageCleanup(frame, false));
        QPainter photo(&frame);
        QLinearGradient tones(0, 0, 120, 160);
        tones.setColorAt(0, Qt::black);
        tones.setColorAt(1, Qt::white);
        photo.fillRect(0, 0, 120, 160, tones);
        photo.end();
        QVERIFY(sleep::needsImageCleanup(frame, false));
    }

    void sleepRefreshUsesDisplayedContent()
    {
        const auto paths = testPaths();
        QVERIFY(storage::writePreferences(paths.preferences(), { { sleep::designKey, "letter" } }));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [state](ReaderHostServices &services) {
            services.colorDisplay = [] { return true; };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        auto *viewer =
                static_cast<ImageViewer *>(host.window()->findChild<QWidget *>("rrImageViewer"));
        QVERIFY(page && viewer);
        auto content = PageContent{};
        content.images.known = content.colours.known = true;
        content.textOnly = true;
        setPageContent(page->image, content);
        page->refreshContent();
        QVERIFY(!page->needsImageCleanup(true));
        const auto handoff = [&](bool clean, bool externalScreenVisible = false) {
            const int before = state->presented;
            host.window()->sleepReader(externalScreenVisible);
            host.window()->sleepViewReady();
            QCOMPARE(state->presented, before + 1);
            QCOMPARE(state->clean, clean);
            QVERIFY(!host.window()->property("reader_sleep_clean_pending").toBool());
            host.window()->wakeReader();
            QTRY_VERIFY(host.window()->readyForPresentation());
        };
        handoff(false);
        handoff(true, true);
        content.textOnly = false;
        content.images.rectangles = { page->image.rect() };
        setPageContent(page->image, content);
        page->refreshContent();
        QVERIFY(page->needsImageCleanup(false));
        // A queued text frame has not replaced the illustration yet.
        content.images.rectangles.clear();
        content.textOnly = true;
        setPageContent(page->image, content);
        handoff(true);
        setPageContent(page->image, content);
        page->refreshContent();
        page->setSavedHighlights({ { { QRect(20, 20, 80, 20) }, QStringLiteral("yellow") } });
        QVERIFY(page->needsImageCleanup(true));
        QVERIFY(!page->needsImageCleanup(false));
        page->setSavedHighlights({});
        viewer->open();
        viewer->setImage(page->image);
        handoff(true);
        QVERIFY(!viewer->isVisible());
    }

    void sleepBackdropsUsePortrait()
    {
        for (const auto mode :
             { sleep::Mode::Cover, sleep::Mode::Page, sleep::Mode::Overlay, sleep::Mode::Plain }) {
            sleep::Snapshot shot;
            shot.mode = mode;
            shot.orientation = Qt::LandscapeOrientation;
            shot.image = QImage(1448, 1072, QImage::Format_RGB32);
            shot.image.fill(Qt::red);
            shot.coverArea = QRect(400, 0, 648, 1072);
            const auto original = shot.image;
            reader::portraitSleepSnapshot(shot);
            QCOMPARE(shot.orientation, Qt::PortraitOrientation);
            QCOMPARE(shot.image.size(), QSize(1072, 1448));
            QCOMPARE(original.size(), QSize(1448, 1072));
            QCOMPARE(shot.image.pixelColor(536, 724),
                     mode == sleep::Mode::Plain ? QColor(Qt::white) : QColor(Qt::red));
            if (mode == sleep::Mode::Page || mode == sleep::Mode::Overlay)
                QCOMPARE(shot.image.pixelColor(0, 0), QColor(Qt::white));
            const auto portrait = shot.image;
            reader::portraitSleepSnapshot(shot);
            QCOMPARE(shot.image, portrait);
        }
    }

    void sleepSnapshotMatchesTheDisplayedPage_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void sleepSnapshotMatchesTheDisplayedPage()
    {
        QFETCH(bool, dark);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), { { "dark_mode", dark } }));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        page->bookmarked = true;
        page->refreshContent();
        page->mark({ QRect(30, 40, 120, 25) });
        page->revealLinks({ QRect(60, 100, 180, 30) });
        const auto displayed = page->grab().toImage().convertToFormat(QImage::Format_RGB32);
        const auto snapshot = page->snapshot();
        QCOMPARE(snapshot, displayed);
        QCOMPARE(page->snapshot(), snapshot);
    }

    void sleepSnapshotReplacesEachClockWithoutChangingTheReadingPage_data()
    {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("slot");
        for (int ppi : { 212, 300 })
            for (bool dark : { false, true })
                for (int slot = 0; slot < headerfooter::slotCount; ++slot)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(ppi).arg(dark).arg(slot)))
                            << ppi << dark << slot;
    }

    void sleepSnapshotReplacesEachClockWithoutChangingTheReadingPage()
    {
        QFETCH(int, ppi);
        QFETCH(bool, dark);
        QFETCH(int, slot);

        struct RestoreDensity
        {
            int value = ui::density();

            ~RestoreDensity() { ui::setDensity(value); }
        } restore;

        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] { });
        page.setProperty("reader_dark", dark);
        page.resize(800, 1100);
        auto palette = page.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        page.setPalette(palette);
        page.setFont(ui::chromeFont(24));
        page.chrome.slotFields.fill(headerfooter::Field::Nothing);
        page.chrome.slotFields[slot] = headerfooter::Field::Time;
        const int other = headerfooter::footerSlot(slot) ? 1 : 4;
        page.chrome.slotFields[other] = headerfooter::Field::BookPage;
        page.slotContents[other].text = "3 / 12";
        page.slotContents[slot].text = "09:00";
        page.image = QImage(page.width(), page.height() - page.topHeight() - page.bottomHeight(),
                            QImage::Format_RGB32);
        page.image.fill(Qt::gray);
        page.refreshContent();
        const auto reading = page.snapshot();
        const auto sleeping = page.snapshot(true);
        QVERIFY(sleeping != reading);
        QCOMPARE(page.snapshot(), reading);
        QCOMPARE(page.slotContents[slot].text, QString("09:00"));
        const QRect body(0, page.topHeight(), page.width(), page.image.height());
        QCOMPARE(sleeping.copy(body), reading.copy(body));
        const QRect otherStrip = headerfooter::footerSlot(slot)
                ? QRect(0, 0, page.width(), page.topHeight())
                : QRect(0, page.height() - page.bottomHeight(), page.width(), page.bottomHeight());
        QCOMPARE(sleeping.copy(otherStrip), reading.copy(otherStrip));
        page.slotContents[slot].text = "23:59";
        page.refreshContent();
        QCOMPARE(page.snapshot(true), sleeping);
        QVERIFY(page.snapshot() != reading);
        page.chrome.slotFields[slot] = headerfooter::Field::BookTitle;
        page.slotContents[slot] = {};
        page.refreshContent();
        const auto blank = page.snapshot();
        QVERIFY(sleeping != blank);
        QCOMPARE(page.snapshot(true), blank);
    }

    // Checks that the chapter, book and sleep-screen time-remaining texts use the same detailed or approximate format.
    void timeRemainingFormatShared_data()
    {
        QTest::addColumn<bool>("approximate");
        QTest::newRow("detailed") << false;
        QTest::newRow("approximate") << true;
    }

    void timeRemainingFormatShared()
    {
        QFETCH(bool, approximate);
        const auto paths = testPaths();
        auto layout = headerfooter::presetLayout(headerfooter::Preset::Default);
        layout.slotFields[0] = headerfooter::Field::ChapterTimeLeft;
        layout.slotFields[5] = headerfooter::Field::BookTimeLeft;
        for (auto &uppercase : layout.uppercase)
            uppercase = false;
        QVERIFY(storage::writeJson(
                paths.preferences(),
                { { "time_remaining_format", approximate ? "approximate" : "detailed" },
                  { sleep::preferenceKey, "page" },
                  { headerfooter::presetKey, "custom" },
                  { headerfooter::customKey, headerfooter::toJson(layout) } }));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {
            QUrl::fromLocalFile(QFileInfo(coverBook).absoluteFilePath()).toString(),
            "OEBPS/c1.xhtml", 100, 1, true
        };
        host.positions = nickel::positionCodec();
        host.progress->hasEstimates = true;
        host.progress->remaining = { 18 * 60000, 83 * 60000 };
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = dynamic_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QTRY_COMPARE(page->slotContents[0].text,
                     approximate ? QString("about 30 minutes left") : QString("18 min left"));
        QTRY_COMPARE(page->slotContents[5].text,
                     approximate ? QString("about 2 hours left") : QString("1 h 23 min left"));
        const auto snapshot = host.window()->powerScreenSnapshot();
        QVERIFY(!snapshot.image.isNull());
        QCOMPARE(snapshot.remaining,
                 approximate ? QString("about 2 hours to go") : QString("83 min to go"));
    }

    void sleepTimeLeftApproximate()
    {
        QCOMPARE(reader::sleepTimeLeft(-1, true), QString());
        QCOMPARE(reader::sleepTimeLeft(0, true), QString());
        QCOMPARE(reader::sleepTimeLeft(1, true), QString("about 15 minutes to go"));
        QCOMPARE(reader::sleepTimeLeft(15 * 60000, true), QString("about 15 minutes to go"));
        QCOMPARE(reader::sleepTimeLeft(15 * 60000 + 1, true), QString("about 30 minutes to go"));
        QCOMPARE(reader::sleepTimeLeft(30 * 60000 + 1, true), QString("about 45 minutes to go"));
        QCOMPARE(reader::sleepTimeLeft(45 * 60000 + 1, true), QString("about an hour to go"));
        QCOMPARE(reader::sleepTimeLeft(60 * 60000, true), QString("about an hour to go"));
        QCOMPARE(reader::sleepTimeLeft(60 * 60000 + 1, true), QString("about 2 hours to go"));
        QCOMPARE(reader::sleepTimeLeft(120 * 60000, true), QString("about 2 hours to go"));
        QCOMPARE(reader::sleepTimeLeft(120 * 60000 + 1, true), QString("about 3 hours to go"));
    }

    void sleepTimeLeftUsesHoursAboveTwoHours()
    {
        QCOMPARE(reader::sleepTimeLeft(-1), QString());
        QCOMPARE(reader::sleepTimeLeft(0), QString("0 min to go"));
        QCOMPARE(reader::sleepTimeLeft(1), QString("1 min to go"));
        QCOMPARE(reader::sleepTimeLeft(37 * 60000), QString("37 min to go"));
        QCOMPARE(reader::sleepTimeLeft(120 * 60000 - 1), QString("120 min to go"));
        QCOMPARE(reader::sleepTimeLeft(120 * 60000), QString("120 min to go"));
        QCOMPARE(reader::sleepTimeLeft(120 * 60000 + 1), QString("2 h 1 min to go"));
        QCOMPARE(reader::sleepTimeLeft(121 * 60000), QString("2 h 1 min to go"));
        QCOMPARE(reader::sleepTimeLeft(180 * 60000), QString("3 h 0 min to go"));
        QCOMPARE(reader::sleepTimeLeft(217 * 60000), QString("3 h 37 min to go"));
    }

    void sleepScreenDefaultsMigrateOnce()
    {
        QCOMPARE(sleep::mode({}), sleep::Mode::Cover);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("reader-settings.json");
        for (const auto oldMode : { sleep::Mode::Stock, sleep::Mode::Cover, sleep::Mode::Page }) {
            for (const auto &oldTile : { QString("band"), QString("card"), QString("bookmark") }) {
                QVERIFY(storage::writeJson(path,
                                           { { sleep::preferenceKey, sleep::value(oldMode) },
                                             { sleep::tileKey, oldTile } }));
                const auto migrated = storage::readPreferences(path).object;
                QCOMPARE(sleep::mode(migrated),
                         oldMode == sleep::Mode::Stock ? sleep::Mode::Cover : oldMode);
                QCOMPARE(sleep::tile(migrated),
                         oldTile == "card" ? sleep::Tile::Card : sleep::Tile::Bookmark);
                QCOMPARE(storage::readJson(path), migrated);
            }
        }
        QVERIFY(storage::writePreferences(path, { { sleep::preferenceKey, "stock" } }));
        QCOMPARE(sleep::mode(storage::readPreferences(path).object), sleep::Mode::Stock);
        for (const auto mode : sleep::modes) {
            QCOMPARE(sleep::mode({ { sleep::preferenceKey, sleep::value(mode) },
                                   { "kobalt_sleep_screen", true } }),
                     mode);
        }
        for (const auto &invalid :
             { QJsonValue("unknown"), QJsonValue(true), QJsonValue(QJsonValue::Null) }) {
            QCOMPARE(sleep::mode({ { sleep::preferenceKey, invalid },
                                   { "kobalt_sleep_screen", true } }),
                     sleep::Mode::Cover);
        }
        QCOMPARE(sleep::tile({}), sleep::Tile::Bookmark);
        for (const auto tile : sleep::tiles) {
            QCOMPARE(sleep::tile({ { sleep::tileKey, sleep::value(tile) } }), tile);
            QCOMPARE(appearance::cleanPreferences({ { sleep::tileKey, sleep::value(tile) } })
                             .value(sleep::tileKey),
                     QJsonValue(sleep::value(tile)));
        }
        for (const auto &invalid :
             { QJsonValue("unknown"), QJsonValue(true), QJsonValue(QJsonValue::Null) }) {
            QCOMPARE(sleep::tile({ { sleep::tileKey, invalid } }), sleep::Tile::Bookmark);
            QVERIFY(!appearance::cleanPreferences({ { sleep::tileKey, invalid } })
                             .contains(sleep::tileKey));
        }
    }

    void sleepPngOverlaysUseEitherFolder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkpath("kobalt"));
        QVERIFY(QDir(directory.path()).mkpath("nickel/wallpaper/overlay"));
        const auto kobalt = directory.filePath("kobalt");
        const auto nickel = directory.filePath("nickel");
        const QStringList folders{ kobalt, nickel };
        QImage png(6, 6, QImage::Format_ARGB32);
        png.fill(Qt::transparent);
        png.setPixel(3, 3, qRgb(255, 0, 0));
        QVERIFY(png.save(nickel + "/wallpaper/overlay/ignored.png"));
        QImage opaque(6, 6, QImage::Format_RGB32);
        opaque.fill(Qt::red);
        QVERIFY(opaque.save(kobalt + "/opaque.png"));
        QVERIFY(storage::loadSleepOverlay(folders).isNull());
        for (const auto &folder : folders) {
            const auto path = folder + "/art.png";
            QVERIFY(png.save(path));
            const auto overlay = storage::loadSleepOverlay(folders);
            QVERIFY(overlay.hasAlphaChannel());
            QImage page(6, 6, QImage::Format_RGB32);
            page.fill(Qt::white);
            sleep::drawOverlay(page, overlay);
            QCOMPARE(page.pixel(0, 0), qRgb(255, 255, 255));
            QCOMPARE(page.pixel(3, 3), qRgb(255, 0, 0));
            QVERIFY(QFile::remove(path));
        }
    }

    // Checks the gallery, and that it explains how to add PNGs when that design is unavailable.
    void sleepNativePreviewFollowsSettings()
    {
        QImage cover(600, 900, QImage::Format_RGB32);
        cover.fill(Qt::gray);
        sleep::Snapshot sample;
        sample.progress = "42% read";
        const auto preview = [&](sleep::NativePreview native) {
            reader::SleepGallery gallery(nullptr, { { sleep::Design::Stock, true } },
                                         sleep::Design::Stock, sample, QSize(1072, 1448), native);
            gallery.resize(1072, 1448);
            gallery.setCover(cover);
            gallery.show();
            QCoreApplication::processEvents();
            auto *grid = gallery.findChild<QWidget *>("rrSleepGalleryGrid");
            return grid->grab().toImage();
        };
        const auto plain = preview({ false, false, false });
        QCOMPARE(plain, preview({ false, true, true }));
        const auto inset = preview({ true, false, true });
        QCOMPARE(inset, preview({ true, false, false }));
        const auto full = preview({ true, true, false });
        const auto info = preview({ true, true, true });
        QVERIFY(plain != inset);
        QVERIFY(inset != full);
        QVERIFY(full != info);
        const auto capture = qEnvironmentVariable("KOBALT_SLEEP_TILE_CAPTURE_ROOT");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(plain.save(capture + "/native-preview-plain.png"));
            QVERIFY(inset.save(capture + "/native-preview-inset.png"));
            QVERIFY(full.save(capture + "/native-preview-full.png"));
            QVERIFY(info.save(capture + "/native-preview-info.png"));
        }
    }

    void sleepSettingsShowWhereToAddPngs()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto state = std::make_shared<test::SleepPresentationState>();
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.sleepOverlayDirectories = { directory.path() };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        const QSize size(1072, 1448);
        // Review captures can open a book with a cover, so the previews show it.
        host.open(qEnvironmentVariable("KOBALT_SLEEP_CAPTURE_BOOK", textBook), size);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), size);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        QVERIFY(clickAt(canvas, QPoint(size.width() / 2, size.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsSystem")));
        auto *row = host.window()->findChild<QPushButton *>("rrSleepScreen");
        QVERIFY(row && row->isVisible());
        const auto capture = qEnvironmentVariable("KOBALT_SLEEP_TILE_CAPTURE_ROOT");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(host.window()->grab().save(capture + "/sleep-settings.png"));
        }
        QVERIFY(click(canvas, row));
        // The row opens the gallery: four sleep screens a page, today's choice first.
        auto *gallery = host.window()->findChild<QWidget *>("rrSleepGallery");
        QTRY_VERIFY(gallery && gallery->isVisible());
        auto *grid = gallery->findChild<QWidget *>("rrSleepGalleryGrid");
        QVERIFY(grid && grid->isVisible());
        auto *follow = gallery->findChild<QPushButton *>("rrSleepFollowOrientation");
        QVERIFY(follow);
        QVERIFY(click(canvas, follow));
        QTRY_COMPARE(storage::readPreferences(paths.preferences())
                             .object.value(sleep::followOrientationKey),
                     QJsonValue(false));
        auto *notice = gallery->findChild<QWidget *>("rrSleepOrientationNotice");
        QVERIFY(notice && notice->isVisible());
        auto *noticeText = notice->findChild<QLabel *>("rrButtonFeedbackText");
        QVERIFY(noticeText);
        QCOMPARE(noticeText->text(), strings::get("sleep.rotation_disabled"));
        QVERIFY(click(canvas, follow));
        QTRY_COMPARE(storage::readPreferences(paths.preferences())
                             .object.value(sleep::followOrientationKey),
                     QJsonValue(true));
        QCOMPARE(noticeText->text(), strings::get("sleep.rotation_enabled"));

        auto *counter = gallery->findChild<QLabel *>("rrSleepGalleryPage");
        QVERIFY(counter);
        const auto firstPage = counter->text();
        const auto readingPage = host.session()->state().value("page");
        host.window()->physicalPageButton(Qt::Key_F23, -1);
        QCOMPARE(counter->text(), firstPage);
        const int pages = 5;
        for (int index = 0; index < pages; ++index) {
            if (!capture.isEmpty())
                QVERIFY(host.window()->grab().save(
                        capture + QString("/sleep-gallery-%1.png").arg(index + 1)));
            if (index + 1 < pages) {
                const auto before = counter->text();
                host.window()->physicalPageButton(Qt::Key_F24, 1);
                QVERIFY(counter->text() != before);
            }
        }
        const auto lastPage = counter->text();
        host.window()->physicalPageButton(Qt::Key_F24, 1);
        QCOMPARE(counter->text(), lastPage);
        for (int index = 1; index < pages; ++index)
            host.window()->physicalPageButton(Qt::Key_F23, -1);
        QCOMPARE(counter->text(), firstPage);
        QCOMPARE(host.session()->state().value("page"), readingPage);
        for (int index = 1; index < pages; ++index)
            host.window()->physicalPageButton(Qt::Key_F24, 1);
        // PNG over text is the only entry on the last page. Without PNGs it is
        // unavailable, and a tap explains where to add them.
        const auto cell = QPoint(grid->width() / 4, grid->height() / 4);
        QVERIFY(clickAt(canvas, grid->mapTo(canvas.surface(), cell)));
        auto *help = host.window()->findChild<QWidget *>("rrSleepOverlayHelpPopup");
        QTRY_VERIFY(help && help->isVisible());
        QVERIFY(gallery->isVisible());
        const auto text = strings::get("settings.sleep_overlay_description");
        QVERIFY(text.contains(".adds/kobalt/screensaver"));
        QVERIFY(text.contains(".adds/screensaver"));
        if (!capture.isEmpty())
            QVERIFY(host.window()->grab().save(capture + "/sleep-overlay-help.png"));
        // The card has no other action, so its button closes it. A tap on the
        // card's text keeps it open.
        auto *title = help->findChild<QWidget *>("rrSleepOverlayHelpTitle");
        QVERIFY(title);
        QVERIFY(clickAt(canvas, title->mapTo(canvas.surface(), title->rect().center())));
        QVERIFY(help->isVisible());
        auto *close = help->findChild<QPushButton *>("rrSleepOverlayHelpClose");
        QVERIFY(close && close->isVisible());
        QVERIFY(click(canvas, close));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSleepOverlayHelpPopup"));
        QVERIFY(gallery->isVisible());
        // A tap on the backdrop also closes only the help, not the gallery.
        QVERIFY(clickAt(canvas, grid->mapTo(canvas.surface(), cell)));
        QTRY_VERIFY((help = host.window()->findChild<QWidget *>("rrSleepOverlayHelpPopup"))
                    && help->isVisible());
        QVERIFY(clickAt(canvas, help->mapTo(canvas.surface(), QPoint(help->width() / 2, 4))));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSleepOverlayHelpPopup"));
        QVERIFY(gallery->isVisible());
    }

    // Activity must recheck the book before returning a cached cover.
    void activityCoverReplacement()
    {
        QTemporaryDir root;
        const auto path = root.filePath("book.epub");
        QVERIFY(QFile::copy(coverBook, path));
        library::CoverThumbnails thumbnails(root.filePath("cache"));
        QImage image;
        bool done = false, missing = false;
        const auto request = [&] {
            done = false;
            thumbnails.request({ { "fixture", path } }, QSize(32, 32),
                               [&](const QString &, const QImage &result, bool gone) {
                                   image = result;
                                   missing = gone;
                                   done = true;
                               });
        };
        request();
        QTRY_VERIFY(done);
        QVERIFY(!image.isNull() && !missing);
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::copy(textBook, path));
        request();
        QTRY_VERIFY(done);
        QVERIFY(image.isNull() && !missing);
        QVERIFY(QFile::remove(path));
        request();
        QTRY_VERIFY(done);
        QVERIFY(image.isNull() && missing);
    }

    // The Custom editor shows the cover even when the chosen design does not
    // draw it, also when it opens again after Done.
    void sleepCustomEditorShowsTheCover()
    {
        const auto paths = testPaths();
        QJsonObject preferences;
        sleep::setDesign(preferences, sleep::Design::Page);
        sleep::Custom custom;
        custom.sleep = { sleep::Mode::Cover, sleep::Design::Seal, {} };
        sleep::setCustom(preferences, custom);
        QVERIFY(storage::writePreferences(paths.preferences(), preferences));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        const auto state = std::make_shared<test::SleepPresentationState>();
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        const QSize size(1072, 1448);
        host.open(coverBook, size);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), size);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        QVERIFY(clickAt(canvas, QPoint(size.width() / 2, size.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsSystem")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSleepScreen")));
        auto *gallery = host.window()->findChild<QWidget *>("rrSleepGallery");
        QVERIFY(gallery && gallery->isVisible());
        auto *grid = gallery->findChild<QWidget *>("rrSleepGalleryGrid");
        QVERIFY(grid);
        // Custom is the second entry on page one.
        const QPoint custom2(grid->width() * 3 / 4, grid->height() / 4);
        const auto openEditor = [&]() -> QWidget * {
            if (!clickAt(canvas, grid->mapTo(canvas.surface(), custom2)))
                return nullptr;
            QWidget *editor = nullptr;
            (void)QTest::qWaitFor([&] {
                editor = host.window()->findChild<QWidget *>("rrSleepEditor");
                return editor && editor->isVisible();
            });
            return editor;
        };
        // The fixture cover is green on its left half; the backdrop is black.
        const auto showsCover = [](QWidget *editor) {
            auto *preview = editor->findChild<QWidget *>("rrSleepEditorPreview");
            if (!preview)
                return false;
            const auto image = preview->grab().toImage();
            const QColor colour(image.pixel(image.width() * 35 / 100, image.height() / 2));
            return colour.green() > 100 && colour.blue() < 80;
        };
        auto *editor = openEditor();
        QVERIFY(editor);
        QTRY_VERIFY(showsCover(editor));
        // The editor covers the gallery. A hardware page button must not
        // change the hidden gallery page behind it.
        auto *counter = gallery->findChild<QLabel *>("rrSleepGalleryPage");
        auto *nextPage = gallery->findChild<QPushButton *>("rrSleepGalleryNext");
        QVERIFY(counter && nextPage && nextPage->isEnabled());
        const auto startingPage = counter->text();
        host.window()->pageButton(1);
        QCOMPARE(counter->text(), startingPage);
        QVERIFY(click(canvas, editor->findChild<QPushButton *>("rrSleepEditorDone")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSleepEditor")
                    || !host.window()->findChild<QWidget *>("rrSleepEditor")->isVisible());
        QVERIFY(gallery->isVisible());
        host.window()->pageButton(1);
        QVERIFY(counter->text() != startingPage);
        host.window()->pageButton(-1);
        QCOMPARE(counter->text(), startingPage);
        editor = openEditor();
        QVERIFY(editor);
        // Done kept the decoded cover, so it shows at once.
        QVERIFY(showsCover(editor));
        auto *customEditor = dynamic_cast<reader::SleepEditor *>(editor);
        QVERIFY(customEditor);
        auto *preview = customEditor->findChild<QWidget *>("rrSleepEditorPreview");
        auto *next = customEditor->findChild<QPushButton *>("rrSleepEditorBackdropNext");
        QVERIFY(preview && next);
        QSize bound;
        customEditor->wallpaperWanted = [&](const QString &, QSize requested) {
            bound = requested;
        };
        for (int i = 0; i < 3; ++i)
            next->click();
        preview->grab();
        QVERIFY(!bound.isEmpty());
        QVERIFY(bound.height() < preview->height() * preview->devicePixelRatioF());
        QVERIFY(bound.width() < preview->width() * preview->devicePixelRatioF());
        const auto oldBound = bound;
        preview->setFixedHeight(preview->height() / 2);
        preview->grab();
        QVERIFY(bound.height() < oldBound.height());
        QImage first(16, 16, QImage::Format_RGB32);
        customEditor->setWallpaper("first", oldBound, first);
        QVERIFY(first.isDetached());
        customEditor->setWallpaper("first", bound, first);
        QVERIFY(!first.isDetached());
        customEditor->setWallpaper("second", bound, QImage());
        customEditor->setWallpaper("third", bound, QImage());
        QVERIFY(first.isDetached());
    }

    void sleepGalleryCancelsInterruptedPresses_data()
    {
        QTest::addColumn<bool>("pageChanged");
        QTest::newRow("touch-cancelled") << false;
        QTest::newRow("page-changed") << true;
    }

    void sleepGalleryCancelsInterruptedPresses()
    {
        QFETCH(bool, pageChanged);
        using Design = sleep::Design;
        QWidget window;
        window.resize(nativeSize);
        reader::SleepGallery gallery(&window,
                                     { { Design::Stock },
                                       { Design::Custom },
                                       { Design::Page },
                                       { Design::Bookmark },
                                       { Design::Seal },
                                       { Design::Letter } },
                                     Design::Stock, {}, window.size());
        gallery.setGeometry(window.rect());
        QList<Design> choices;
        gallery.chosen = [&](Design value) { choices.append(value); };
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *grid = gallery.findChild<QWidget *>("rrSleepGalleryGrid");
        QVERIFY(grid && grid->isVisible());
        const QPoint left(grid->width() / 4, grid->height() / 4);
        QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier, left);
        if (pageChanged) {
            auto *next = gallery.findChild<QPushButton *>("rrSleepGalleryNext");
            QVERIFY(next && next->isEnabled());
            next->click();
        } else {
            QTouchEvent event(QEvent::TouchCancel, QTest::createTouchDevice());
            QCoreApplication::sendEvent(grid, &event);
        }
        QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier, left);
        QVERIFY2(choices.isEmpty(), "An interrupted press selected a sleep screen");
        QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, left);
        QCOMPARE(choices, QList<Design>{ pageChanged ? Design::Seal : Design::Stock });
    }

    void sleepGreetingEditor()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const storage::Paths paths{ directory.path() };
        ReaderFixture host(paths, testFonts, testPlaceholder);
        const auto state = std::make_shared<test::SleepPresentationState>();
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        const QSize size(1072, 1448);
        host.open(textBook, size);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), size);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        QVERIFY(clickAt(canvas, QPoint(size.width() / 2, size.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsSystem")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSleepScreen")));
        auto *gallery = host.window()->findChild<QWidget *>("rrSleepGallery");
        QVERIFY(gallery && gallery->isVisible());
        for (int page = 0; page < 2; ++page)
            QVERIFY(click(canvas, gallery->findChild<QPushButton *>("rrSleepGalleryNext")));
        auto *grid = gallery->findChild<QWidget *>("rrSleepGalleryGrid");
        QVERIFY(grid);
        const auto openEditor = [&] {
            // Night is the lower-right entry on page three. Await the real hold timer.
            QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier,
                              QPoint(grid->width() * 3 / 4, grid->height() * 3 / 4));
            const bool shown = QTest::qWaitFor([&] {
                auto *dialog = host.window()->findChild<QWidget *>("rrSleepGreeting");
                return dialog && dialog->isVisible();
            });
            QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier,
                                QPoint(grid->width() * 3 / 4, grid->height() * 3 / 4));
            return shown;
        };
        QVERIFY(openEditor());
        QVERIFY(gallery->isVisible());
        auto *editor = host.window()->findChild<QLineEdit *>("rrSleepGreetingInput");
        QVERIFY(editor);
        auto *keyboard = editor->window()
                                 ->findChild<QWidget *>("rrSleepGreeting")
                                 ->findChild<QWidget *>("widgetKeyboard");
        QVERIFY(keyboard && keyboard->isVisible());
        const auto cardBottom =
                editor->parentWidget()
                        ->mapTo(host.window(), editor->parentWidget()->rect().bottomLeft())
                        .y();
        const auto keyboardTop = keyboard->mapTo(host.window(), QPoint()).y();
        QVERIFY2(cardBottom < keyboardTop, "Greeting card overlaps the keyboard");
        const auto capture = qEnvironmentVariable("KOBALT_SLEEP_TILE_CAPTURE_ROOT");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(host.window()->grab().save(capture + "/sleep-greeting-editor.png"));
        }
        QCOMPARE(editor->maxLength(), sleep::greetingLength);
        editor->setText("  Sleep well  ");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSleepGreetingSave")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSleepGreeting"));
        QCOMPARE(storage::readJson(paths.preferences()).value(sleep::greetingKey).toString(),
                 QString("Sleep well"));
        QVERIFY(openEditor());
        editor = host.window()->findChild<QLineEdit *>("rrSleepGreetingInput");
        QVERIFY(editor);
        QCOMPARE(editor->text(), QString("Sleep well"));
        editor->clear();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSleepGreetingSave")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSleepGreeting"));
        QVERIFY(!storage::readJson(paths.preferences()).contains(sleep::greetingKey));
        QVERIFY(openEditor());
        editor = host.window()->findChild<QLineEdit *>("rrSleepGreetingInput");
        editor->setText("Do not save");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSleepGreetingCancel")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSleepGreeting"));
        QVERIFY(!storage::readJson(paths.preferences()).contains(sleep::greetingKey));
    }

    void sleepOverlayUsesTheDimmedPage_data()
    {
        QTest::addColumn<bool>("privateBook");
        QTest::newRow("reading-page") << false;
        QTest::newRow("private-opaque-page") << true;
    }

    // Checks partial alpha over either the reading page or the private opaque background.
    void sleepOverlayUsesTheDimmedPage()
    {
        QFETCH(bool, privateBook);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const storage::Paths paths{ directory.path() };
        const auto pngPath = directory.filePath("art.png");
        QImage art(1072, 1448, QImage::Format_ARGB32);
        art.fill(Qt::transparent);
        QPainter painter(&art);
        painter.fillRect(QRect(0, 0, art.width(), 160), QColor(255, 255, 255, 128));
        painter.end();
        QVERIFY(art.save(pngPath));
        QVERIFY(storage::writePreferences(paths.preferences(),
                                          { { sleep::preferenceKey, "overlay" } }));

        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.sleepOverlayDirectories = { directory.path() };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, art.size());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        QVERIFY(page);
        const auto clean = static_cast<ReadingPage *>(page)->snapshot(true);
        host.book()->setHideBookOnSleepScreen(privateBook);

        // The reader's overlay preload is ahead of this barrier on the engine worker.
        bool loaded = false;
        host.session()->sleepOverlay({ directory.path() }, [&](QImage) { loaded = true; });
        QTRY_VERIFY(loaded);
        const auto backdrop = host.window()->powerScreenSnapshot();
        QVERIFY(!backdrop.image.isNull());
        QVERIFY(!backdrop.dimPage);
        QCOMPARE(backdrop.tile, sleep::Tile::Bookmark);
        const QPoint transparent(5, 300);
        const QPoint translucent(5, 50);
        if (privateBook) {
            QCOMPARE(backdrop.image.pixel(transparent), qRgb(255, 255, 255));
            QCOMPARE(backdrop.image.pixel(translucent), qRgb(255, 255, 255));
            QVERIFY(backdrop.details.title.isEmpty() && backdrop.details.chapter.isEmpty());
        } else {
            QVERIFY(qGray(backdrop.image.pixel(transparent)) < qGray(clean.pixel(transparent)));
            const auto dimmed = reader::sleepScreenImage(clean, {}, {}, false, -1,
                                                         reader::SleepStatus::Sleeping, true,
                                                         sleep::Tile::None);
            const int before = qGray(dimmed.pixel(translucent));
            QVERIFY(qAbs(qGray(backdrop.image.pixel(translucent)) - (before + 255) / 2) <= 1);
        }
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        QCOMPARE(state->presented, 1);
        QCOMPARE(state->image.pixel(translucent), backdrop.image.pixel(translucent));
        QVERIFY(state->image != backdrop.image);
        const auto capture = qEnvironmentVariable("KOBALT_SLEEP_TILE_CAPTURE_ROOT");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(state->image.save(capture + "/translucent-overlay.png"));
        }
        host.window()->wakeReader();
    }

    void sleepCoverFitsWithoutCroppingOrDimming()
    {
        QImage cover(80, 40, QImage::Format_RGB32);
        cover.fill(Qt::green);
        cover.setPixel(0, 0, qRgb(255, 0, 0));
        const auto original = cover.copy();
        for (const bool dark : { false, true }) {
            const auto background = reader::sleepCoverBackground(cover, { 800, 1200 });
            QCOMPARE(background.size(), QSize(800, 1200));
            QCOMPARE(background.pixel(0, 0), qRgb(0, 0, 0));
            QCOMPARE(background.pixel(799, 1199), qRgb(0, 0, 0));
            QCOMPARE(background.pixel(400, 600), qRgb(0, 255, 0));
            QCOMPARE(background.pixel(0, 400), qRgb(255, 0, 0));
            QCOMPARE(background.pixel(799, 799), qRgb(0, 255, 0));
            const auto frame =
                    reader::sleepScreenImage(background, "42% read", "3 h 37 min to go", dark, 67,
                                             reader::SleepStatus::Charging, false);
            QCOMPARE(frame.pixel(400, 600), background.pixel(400, 600));
            QCOMPARE(frame.pixel(0, 0), background.pixel(0, 0));
        }
        QCOMPARE(cover, original);
        const auto sideBorders = reader::sleepCoverBackground(cover, { 120, 40 });
        QCOMPARE(sideBorders.pixel(0, 20), qRgb(0, 0, 0));
        QCOMPARE(sideBorders.pixel(119, 20), qRgb(0, 0, 0));
        QCOMPARE(sideBorders.pixel(60, 20), qRgb(0, 255, 0));
        QImage transparent(80, 40, QImage::Format_ARGB32);
        transparent.fill(Qt::transparent);
        QCOMPARE(reader::sleepCoverBackground(transparent, { 80, 40 }).pixel(40, 20),
                 qRgb(0, 0, 0));
        QVERIFY(reader::sleepCoverBackground({}, { 800, 1200 }).isNull());
        QVERIFY(reader::sleepCoverBackground(cover, {}).isNull());
        QVERIFY(reader::sleepCoverBackground(cover, { 4096, 4096 }).isNull());
    }

    void sleepCoverDecodingIsBoundedAndHonorsSourceCancellation()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QImage cover;
        int completed = 0;
        host.session()->coverImage({ 60, 90 }, [&](QImage image) {
            cover = std::move(image);
            ++completed;
        });
        QTRY_COMPARE(completed, 1);
        QCOMPARE(cover.size(), QSize(60, 30));
        QVERIFY(qGreen(cover.pixel(5, 5)) > qBlue(cover.pixel(5, 5)));
        QVERIFY(qBlue(cover.pixel(55, 5)) > qGreen(cover.pixel(55, 5)));
        QTemporaryDir nativeCover;
        QVERIFY(nativeCover.isValid());
        const auto nativePath = nativeCover.path() + "/native.png";
        QImage nativeImage(40, 60, QImage::Format_RGB32);
        nativeImage.fill(Qt::red);
        QVERIFY(nativeImage.save(nativePath));
        QFileInfo provenance;
        bool nativeDone = false;
        host.session()->coverImage(
                { 2048, 2048 },
                [&](QImage image, QFileInfo source) {
                    cover = std::move(image);
                    provenance = std::move(source);
                    nativeDone = true;
                },
                nativePath);
        QTRY_VERIFY(nativeDone);
        QCOMPARE(cover.size(), nativeImage.size());
        QCOMPARE(provenance.absoluteFilePath(), nativePath);
        QFile corrupt(nativePath);
        QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupt.write("broken PNG");
        corrupt.close();
        nativeDone = false;
        host.session()->coverImage(
                { 2048, 2048 },
                [&](QImage image, QFileInfo source) {
                    cover = std::move(image);
                    provenance = std::move(source);
                    nativeDone = true;
                },
                nativePath);
        QTRY_VERIFY(nativeDone);
        QVERIFY(!cover.isNull());
        QVERIFY(provenance.filePath().isEmpty());
        host.session()->coverImage({ 4096, 4096 }, [&](QImage image) {
            cover = std::move(image);
            ++completed;
        });
        QTRY_COMPARE(completed, 2);
        QVERIFY(cover.isNull());
        host.session()->coverImage({ 2048, 2048 }, [&](QImage image) {
            cover = std::move(image);
            ++completed;
        });
        host.session()->cancelSource();
        QTRY_COMPARE(completed, 3);
        QVERIFY(cover.isNull());
    }

    // Checks Wi-Fi indicator placement, size and visibility in the status strip, in both themes and densities.
    void wifiStatus_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        for (const bool dark : { false, true })
            for (const int ppi : { 300, 212 })
                QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(ppi)))
                        << dark << ppi;
    }

    void wifiStatus()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const int previousDensity = ui::density();

        struct Restore
        {
            int density;

            ~Restore() { ui::setDensity(density); }
        } restore{ previousDensity };

        ui::setDensity(ppi);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), { { "dark_mode", dark } }));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        std::function<void(WifiStatus)> wifiChanged;
        std::function<void(bool)> bluetoothChanged;
        host.configureServices = [&](ReaderHostServices &services) {
            services.observeWifi = [&](QObject *, std::function<void(WifiStatus)> changed) {
                wifiChanged = changed;
                changed({ true, true, 4 });
            };
            services.observeBluetooth = [&](QObject *, std::function<void(bool)> changed) {
                bluetoothChanged = changed;
                changed(true);
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *wifi = host.window()->findChild<QWidget *>("rrMenuWifi");
        auto *bluetooth = host.window()->findChild<QWidget *>("rrMenuBluetooth");
        auto *battery = host.window()->findChild<QWidget *>("rrMenuBattery");
        QVERIFY(wifi && bluetooth && battery);
        QTRY_VERIFY(wifi->isVisible() && bluetooth->isVisible());
        QVERIFY(wifi->geometry().right() < bluetooth->x());
        QVERIFY(bluetooth->geometry().right() < battery->x());
        QVERIFY(wifi->accessibleName().contains("4 of 4"));
        const auto strong = wifi->grab().toImage();
        const auto inkBounds = [dark](const QImage &image) {
            QRect bounds;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto color = image.pixelColor(x, y);
                    const int ink = dark ? qGray(color.rgb()) : 255 - qGray(color.rgb());
                    if (ink > 0 && color.alpha() > 0)
                        bounds |= QRect(x, y, 1, 1);
                }
            return bounds;
        };
        const auto wifiInk = inkBounds(strong);
        const auto bluetoothInk = inkBounds(bluetooth->grab().toImage());
        QVERIFY(qAbs(wifiInk.height() - 0.9 * bluetoothInk.height()) <= 1);
        QVERIFY(qAbs(QRectF(wifiInk).center().y() - QRectF(bluetoothInk).center().y() + ui::px(1))
                <= 0.5);
        const auto capture = qEnvironmentVariable("KOBALT_WIFI_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(host.window()->grab().save(
                    capture + QString("/wifi-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
            QVERIFY(host.window()
                            ->grab(QRect(0, 0, host.window()->width(), ui::px(60)))
                            .save(capture
                                  + QString("/wifi-statusbar-%1-%2.png")
                                            .arg(dark ? "dark" : "light")
                                            .arg(ppi)));
        }
        wifiChanged({ true, true, 1 });
        QVERIFY(wifi->accessibleName().contains("1 of 4"));
        QVERIFY(strong != wifi->grab().toImage());
        wifiChanged({ true, true, 4 });
        const int withBluetooth = wifi->x();
        const auto bluetoothSlot = QRectF(bluetooth->geometry()).center().x();
        bluetoothChanged(false);
        QTRY_VERIFY(!bluetooth->isVisible());
        QVERIFY(wifi->x() > withBluetooth);
        QCOMPARE(QRectF(wifi->geometry()).center().x(), bluetoothSlot);
        if (!capture.isEmpty())
            QVERIFY(host.window()
                            ->grab(QRect(0, 0, host.window()->width(), ui::px(60)))
                            .save(capture
                                  + QString("/wifi-only-statusbar-%1-%2.png")
                                            .arg(dark ? "dark" : "light")
                                            .arg(ppi)));
        wifiChanged({ true, false, -1 });
        QTRY_VERIFY(!wifi->isVisible());
        wifiChanged({ true, true, -1 });
        QTRY_VERIFY(!wifi->isVisible());
        wifiChanged({ true, true, 0 });
        QTRY_VERIFY(!wifi->isVisible());
        wifiChanged({ true, true, 1 });
        QTRY_VERIFY(wifi->isVisible());
        wifiChanged({ false, false, -1 });
        QTRY_VERIFY(!wifi->isVisible());
    }

    // Uses the real reading clock and Settings checklist to hide session text without stopping time collection.
    void readingTimeStatsVisibility_data()
    {
        QTest::addColumn<bool>("hidden");
        QTest::addColumn<QSize>("size");
        for (bool hidden : { false, true })
            for (const QSize size : { QSize(1072, 1448), QSize(720, 1600) })
                QTest::newRow(qPrintable(
                        QString("%1-%2").arg(hidden ? "hidden" : "visible").arg(size.width())))
                        << hidden << size;
    }

    void readingTimeStatsVisibility()
    {
        QFETCH(bool, hidden);
        QFETCH(QSize, size);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), { { "disable_stats", hidden } }));
        qint64 now = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.readingClock = [&] { return now; };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), size);
        QTRY_COMPARE(opened.count(), 1);
        Canvas canvas(host.window(), size);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QTRY_VERIFY(host.book()->readingActive());
        now += 12 * 60000;
        QVERIFY(clickAt(canvas, QPoint(size.width() / 2, size.height() / 2)));
        auto *message = host.window()->findChild<QLabel *>("rrMenuSession");
        auto *clock = host.window()->findChild<QLabel *>("rrMenuClock");
        auto *battery = host.window()->findChild<QWidget *>("rrMenuBattery");
        auto *timer = host.window()->findChild<QTimer *>("rrClockTimer");
        QVERIFY(message && clock && battery && timer);
        const auto check = [&] {
            return message->isVisible() == !hidden
                    && message->text()
                    == (hidden ? QString() : strings::get("reader.session_minutes").arg(12))
                    && clock->isVisible() && battery->isVisible()
                    && host.book()->sessionReadingMilliseconds() == 12 * 60000;
        };
        // Hybrid uses this label for the page count until its status timer refreshes it.
        QTRY_VERIFY_WITH_TIMEOUT(check(), 30000);
        const auto capture = qEnvironmentVariable("KOBALT_STATS_VISIBILITY_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QCoreApplication::processEvents();
            QVERIFY(host.window()->grab().save(
                    capture
                    + QString("/%1-menu-stats-%2.png")
                              .arg(size.width() == 720 ? "phone" : "six-inch")
                              .arg(hidden ? "hidden" : "visible")));
        }
        const auto clockBounds = clock->geometry();
        const auto batteryBounds = battery->geometry();
        for (int pass = 0; pass < 2; ++pass) {
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsSystem")));
            auto *features = host.window()->findChild<QPushButton *>("rrDisabledFeatures");
            QVERIFY(features);
            for (int page = 0; !features->isVisible() && page < 10; ++page)
                QVERIFY(click(canvas,
                              host.window()->findChild<QPushButton *>("rrSettingsOptionsNext")));
            QVERIFY(click(canvas, features));
            auto *option = host.window()->findChild<QCheckBox *>("rrDisableFeature_disable_stats");
            QVERIFY(option);
            QCOMPARE(option->isChecked(), hidden);
            QVERIFY(click(canvas, option));
            QVERIFY(click(canvas,
                          host.window()->findChild<QPushButton *>("rrDisabledFeaturesDone")));
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrDisabledFeaturesPopup"));
            auto *back = host.window()->findChild<QPushButton *>("rrSettingsBack");
            if (back->isVisible())
                QVERIFY(click(canvas, back));
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrGeneralSettings")->isVisible());
            hidden = !hidden;
            QTRY_COMPARE(storage::readJson(paths.preferences()).value("disable_stats").toBool(),
                         hidden);
            QVERIFY(clickAt(canvas, QPoint(size.width() / 2, size.height() / 2)));
            QVERIFY2(
                    check(),
                    "Session message must follow Stats visibility without changing reading time or status icons");
            QCOMPARE(clock->geometry(), clockBounds);
            QCOMPARE(battery->geometry(), batteryBounds);
            // Timer updates and a later menu presentation must keep the same visibility.
            QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
            QVERIFY2(
                    check(),
                    "Session message must follow Stats visibility without changing reading time or status icons");
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavChapters")));
            QVERIFY2(
                    check(),
                    "Session message must follow Stats visibility without changing reading time or status icons");
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavChapters")));
            QVERIFY2(
                    check(),
                    "Session message must follow Stats visibility without changing reading time or status icons");
        }
        // Hiding the feature does not disable the active reading clock.
        QVERIFY(clickAt(canvas, QPoint(size.width() / 2, size.height() / 2)));
        QTRY_VERIFY(host.book()->readingActive());
        now += 60000;
        QCOMPARE(host.book()->sessionReadingMilliseconds(), qint64(13 * 60000));
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(fixture(), size);
        QTRY_COMPARE(opened.count(), 2);
        Canvas reopened(host.window(), size);
        reopened.resize(900, 900);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        QTRY_VERIFY(host.book()->readingActive());
        now += 12 * 60000;
        QVERIFY(clickAt(reopened, QPoint(size.width() / 2, size.height() / 2)));
        message = host.window()->findChild<QLabel *>("rrMenuSession");
        QVERIFY(message);
        QTRY_COMPARE_WITH_TIMEOUT(message->text(),
                                  hidden ? QString() : strings::get("reader.session_minutes").arg(12),
                                  30000);
        QCOMPARE(message->isVisible(), !hidden);
        QCOMPARE(host.book()->sessionReadingMilliseconds(), qint64(12 * 60000));
    }

    // Checks that reading-time text fits and centers in the status strip for each minute width and radio icon combination.
    void readingTimeStatus_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        QTest::addColumn<qreal>("textScale");
        QTest::addColumn<QSize>("viewport");
        for (bool dark : { false, true })
            for (int ppi : { 300, 212 })
                for (qreal scale : { 1.0, 1.5 })
                    for (QSize viewport : { QSize(1072, 1448), QSize(1448, 600) })
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4x%5")
                                                         .arg(dark ? "dark" : "light")
                                                         .arg(ppi)
                                                         .arg(scale)
                                                         .arg(viewport.width())
                                                         .arg(viewport.height())))
                                << dark << ppi << scale << viewport;
    }

    void readingTimeStatus()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        QFETCH(qreal, textScale);
        QFETCH(QSize, viewport);
        const int previousDensity = ui::density();
        const qreal previousTextScale = ui::textScale();
        const auto restore = qScopeGuard([&] {
            ui::setDensity(previousDensity);
            ui::setLargeText(previousTextScale);
        });
        ui::setDensity(ppi);
        ui::setLargeText(textScale);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), { { "dark_mode", dark } }));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        std::function<void(WifiStatus)> wifiChanged;
        std::function<void(bool)> bluetoothChanged;
        QString clockText = "09:00";
        int timeOfDay = 9 * 3600000;
        host.configureServices = [&](ReaderHostServices &services) {
            services.clock = [&] { return clockText; };
            services.localDateTime = [&] { return testLocalTime(timeOfDay); };
            services.observeWifi = [&](QObject *, std::function<void(WifiStatus)> changed) {
                wifiChanged = changed;
                changed({ true, true, 4 });
            };
            services.observeBluetooth = [&](QObject *, std::function<void(bool)> changed) {
                bluetoothChanged = changed;
                changed(true);
            };
        };
        const QSize size(ui::px(viewport.width()), ui::px(viewport.height()));
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), size);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), size);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(size.width() / 2, size.height() / 2)));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->paginationPending(), 30000);
        auto *session = host.window()->findChild<QLabel *>("rrMenuSession");
        auto *clock = host.window()->findChild<QLabel *>("rrMenuClock");
        auto *wifi = host.window()->findChild<QWidget *>("rrMenuWifi");
        auto *bluetooth = host.window()->findChild<QWidget *>("rrMenuBluetooth");
        auto *battery = host.window()->findChild<QWidget *>("rrMenuBattery");
        QVERIFY(session && clock && wifi && bluetooth && battery);
        QCOMPARE(clock->width(), clock->sizeHint().width());
        const auto font = session->font();
        const auto batteryBounds = battery->geometry();
        const auto bluetoothBounds = bluetooth->geometry();
        const auto wifiBounds = wifi->geometry();
        for (int minutes : { 5, 15, 120 }) {
            // Use the production wording without waiting for a long reading session.
            session->setText(strings::get("reader.session_minutes").arg(minutes));
            for (int radios : { 3, 1, 2, 0, 3 }) {
                bluetoothChanged(!(radios & 2));
                wifiChanged({ bool(radios & 1), bool(radios & 1), 4 });
                bluetoothChanged(radios & 2);
                QTRY_VERIFY(session->isVisible());
                QCOMPARE(session->font(), font);
                QCOMPARE(battery->geometry(), batteryBounds);
                QWidget *right = wifi->isVisible() ? wifi
                        : bluetooth->isVisible()   ? bluetooth
                                                   : battery;
                if (session->width() > 0) {
                    QVERIFY(session->x() >= clock->geometry().right() + 1);
                    QVERIFY(session->geometry().right() < right->x());
                }
                const int available = right->x() - clock->geometry().right() - 1
                        - 2 * int(ui::metrics::chrome::inset);
                if (session->sizeHint().width() <= available)
                    QVERIFY2(session->width() >= session->sizeHint().width(),
                             qPrintable(session->text()));
                if (available >= 0) {
                    const qreal midpoint = (clock->geometry().right() + 1 + right->x()) / 2.;
                    QVERIFY(qAbs(QRectF(session->geometry()).center().x() - midpoint) <= .5);
                }
                if (radios == 3) {
                    QCOMPARE(bluetooth->geometry(), bluetoothBounds);
                    QCOMPARE(wifi->geometry(), wifiBounds);
                }
                const auto capture = qEnvironmentVariable("KOBALT_READING_TIME_CAPTURE_DIR");
                if (!capture.isEmpty()) {
                    QVERIFY(QDir().mkpath(capture));
                    QVERIFY(host.window()
                                    ->grab(QRect(0, 0, size.width(), ui::textPx(60)))
                                    .save(capture
                                          + QString("/reading-time-%1-%2-%3-%4-%5%6.png")
                                                    .arg(dark ? "dark" : "light")
                                                    .arg(ppi)
                                                    .arg(textScale)
                                                    .arg(minutes)
                                                    .arg(radios)
                                                    .arg(isShortHeight(size.height(), ppi)
                                                                 ? "-short"
                                                                 : "")));
                }
            }
        }
        // Minute changes keep the same centering while the menu stays open.
        const auto sessionBounds = session->geometry();
        auto *timer = host.window()->findChild<QTimer *>("rrClockTimer");
        timeOfDay += 60000;
        QVERIFY(timer && QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QVERIFY(session->text().isEmpty());
        QVERIFY(!session->isVisible());
        QCOMPARE(session->geometry(), sessionBounds);
        clockText = "9:01";
        timeOfDay += 60000;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(clock->text(), clockText);
        QCOMPARE(clock->width(), clock->sizeHint().width());
        const int available =
                wifi->x() - clock->geometry().right() - 1 - 2 * int(ui::metrics::chrome::inset);
        if (available >= 0) {
            const qreal midpoint = (clock->geometry().right() + 1 + wifi->x()) / 2.;
            QVERIFY(qAbs(QRectF(session->geometry()).center().x() - midpoint) <= .5);
        }
    }

    void remoteKeyActivityRequiresActiveReading()
    {
        int reports = 0;
        bool suspended = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.pageButtonDirection = [](QKeyEvent *event) {
                return event->key() == Qt::Key_Right ? 1 : 0;
            };
            services.suspended = [&] { return suspended; };
            services.remoteKeyActivity = [&](QObject *owner, QKeyEvent *event) {
                QCOMPARE(owner, static_cast<QObject *>(host.window()));
                QCOMPARE(event->type(), QEvent::KeyPress);
                ++reports;
            };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->setWindowFlags(Qt::Window);
        host.window()->setWindowModality(Qt::ApplicationModal);
        host.window()->show();
        QTRY_VERIFY(!host.session()->busy());
        QTRY_COMPARE(QApplication::activeModalWidget(), host.window());
        auto sendKey = [&](QEvent::Type type, int key, bool repeat = false) {
            QKeyEvent event(type, key, Qt::NoModifier, 106, 0, 0, {}, repeat);
            QCoreApplication::sendEvent(host.window(), &event);
        };
        const auto position = [&] { return host.session()->state().value("position").toString(); };
        const QString initial = position();
        QVERIFY(!initial.isEmpty());
        const bool nextChapter = host.session()->page() + 1 == host.session()->pages()
                && host.session()->continuesAfter();
        const int nextPage = nextChapter ? 0 : host.session()->nextPage();
        sendKey(QEvent::KeyPress, Qt::Key_Right);
        QCOMPARE(reports, 1);
        QTRY_VERIFY(!host.session()->busy() && position() != initial);
        QCOMPARE(host.session()->page(), nextPage);
        const auto turned = position();
        QVERIFY(!turned.isEmpty());
        sendKey(QEvent::KeyRelease, Qt::Key_Right);
        QCOMPARE(reports, 1);
        sendKey(QEvent::KeyPress, Qt::Key_Right, true);
        QCOMPARE(reports, 2);
        QVERIFY(!host.session()->busy());
        QCOMPARE(position(), turned);
        sendKey(QEvent::KeyPress, Qt::Key_A);
        QCOMPARE(reports, 2);
        suspended = true;
        sendKey(QEvent::KeyPress, Qt::Key_Right);
        QCOMPARE(reports, 2);
        suspended = false;
        host.window()->hide();
        sendKey(QEvent::KeyPress, Qt::Key_Right);
        QCOMPARE(reports, 2);
    }

    // Checks both finish-dialog choices, with and without "Don't ask again": ignored while suspended, kept through sleep and close requests.
    void finishChoiceSurvivesSleepAndRequiresActiveReader_data()
    {
        QTest::addColumn<bool>("markFinished");
        QTest::addColumn<bool>("exitRequested");
        QTest::addColumn<bool>("suppress");
        QTest::addColumn<bool>("closeFirst");
        QTest::newRow("suppress-still-reading") << false << false << true << false;
        QTest::newRow("suppress-mark-finished") << true << false << true << false;
        QTest::newRow("suppress-exit-still-reading") << false << true << true << false;
        QTest::newRow("suppress-exit-mark-finished") << true << true << true << false;
        QTest::newRow("still-reading") << false << false << false << false;
        QTest::newRow("mark-finished") << true << false << false << false;
        QTest::newRow("exit-still-reading") << false << true << false << false;
        QTest::newRow("exit-mark-finished") << true << true << false << false;
        QTest::newRow("exit-opens-prompt") << false << true << false << true;
    }

    void finishChoiceSurvivesSleepAndRequiresActiveReader()
    {
        QFETCH(bool, markFinished);
        QFETCH(bool, exitRequested);
        QFETCH(bool, suppress);
        QFETCH(bool, closeFirst);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        bool suspended = false;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->goToFraction(1.0));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(!host.session()->continuesAfter());
        QCOMPARE(host.session()->nextPage(), host.session()->page());
        if (closeFirst)
            QVERIFY(!host.window()->close());
        else
            host.window()->pageButton(1);
        QPointer<QWidget> dialog;
        QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook"))
                    && dialog->isVisible());
        auto *choice = dialog->findChild<QPushButton *>(markFinished ? "rrMarkFinished"
                                                                     : "rrStillReading");
        QVERIFY(choice);
        auto *suppression = dialog->findChild<QCheckBox *>("rrSuppressFinishPrompt");
        QVERIFY(suppression);
        suppression->setChecked(suppress);
        if (exitRequested) {
            // Home can request close while a choice is already open.
            // Repeated close requests must keep that choice unresolved.
            QVERIFY(!host.window()->close());
            QVERIFY(!host.window()->close());
            QVERIFY(dialog && dialog->isVisible());
            QVERIFY(!host.idle());
            QCOMPARE(host.progress->current.status, 1);
        }
        suspended = true;
        choice->click();
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(host.progress->current.status, 1);
        suspended = false;
        host.window()->sleepReader();
        QVERIFY(!dialog || dialog->isHidden());
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        if (closeFirst) {
            const int lastPage = host.session()->bookPage();
            host.window()->pageButton(-1);
            QTRY_VERIFY(!host.session()->busy() && host.session()->bookPage() < lastPage);
            QVERIFY(!dialog || !dialog->isVisible());
            return;
        }
        QTRY_VERIFY(dialog && dialog->isVisible());
        choice->click();
        if (markFinished)
            QVERIFY(host.window()->closingTiming().started.isValid());
        QTRY_VERIFY(!dialog);
        if (markFinished) {
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
            QCOMPARE(host.progress->current.status, markFinished ? 2 : 1);
        } else {
            QVERIFY(!host.idle());
            QCOMPARE(host.progress->current.status, 1);
            test::skipPageTurnWindow();
            host.window()->pageButton(1);
            if (suppress) {
                QTest::qWait(100);
                QVERIFY(!host.window()->findChild<QWidget *>("rrFinishBook"));
            } else {
                QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook"))
                            && dialog->isVisible());
            }
        }
    }

    // Checks that "Don't ask again" on the finish prompt survives reopening and a fresh process, and that other books still ask.
    void finishPromptPersistence()
    {
        // A fresh process must restore the choice without any prior Reader state.
        const auto inherited = qEnvironmentVariable("KOBALT_TEST_FINISH_PROFILE");
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = inherited.isEmpty() ? root.path() : inherited;
        const auto bookPath =
                inherited.isEmpty() ? textBook : qEnvironmentVariable("KOBALT_TEST_FINISH_BOOK");
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(bookPath).absoluteFilePath()).toString();
        if (inherited.isEmpty()) {
            ReaderFixture host(paths, testFonts, testPlaceholder);
            CloseHost cleanup{ host };
            QSignalSpy opened(&host, &ReaderFixture::opened);
            host.open(bookPath, nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
            Canvas canvas(host.window(), nativeSize);
            canvas.resize(900, 900);
            canvas.show();
            QVERIFY(QTest::qWaitForWindowExposed(&canvas));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(host.session()->goToFraction(1.0));
            QTRY_VERIFY(!host.session()->busy());
            host.window()->pageButton(1);
            QPointer<QWidget> dialog;
            QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook"))
                        && dialog->isVisible());
            auto *option = dialog->findChild<QCheckBox *>("rrSuppressFinishPrompt");
            QVERIFY(option);
            option->click();
            dialog->findChild<QPushButton *>("rrStillReading")->click();
            QTRY_VERIFY(!dialog);
            QVERIFY(!host.window()->close());
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        }
        const auto saved = storage::readBookState(paths, identity).object;
        QVERIFY(saved.value("suppress_finish_prompt").toBool());
        QVERIFY(!saved.value("position").toString().isEmpty());
        {
            ReaderFixture host(paths, testFonts, testPlaceholder);
            CloseHost cleanup{ host };
            QSignalSpy opened(&host, &ReaderFixture::opened);
            host.open(bookPath, nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
            Canvas canvas(host.window(), nativeSize);
            canvas.resize(900, 900);
            canvas.show();
            QVERIFY(QTest::qWaitForWindowExposed(&canvas));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(host.session()->goToFraction(1.0));
            QTRY_VERIFY(!host.session()->busy());
            host.window()->pageButton(1);
            QTest::qWait(100);
            QVERIFY(!host.window()->findChild<QWidget *>("rrFinishBook"));
            QVERIFY(!host.window()->close());
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
            const auto otherBook = root.path() + "/other.epub";
            QVERIFY(QFile::copy(bookPath, otherBook));
            host.open(otherBook, nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
            Canvas otherCanvas(host.window(), nativeSize);
            otherCanvas.resize(900, 900);
            otherCanvas.show();
            QVERIFY(QTest::qWaitForWindowExposed(&otherCanvas));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(host.session()->goToFraction(1.0));
            QTRY_VERIFY(!host.session()->busy());
            host.window()->pageButton(1);
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrFinishBook"));
            host.window()->findChild<QPushButton *>("rrMarkFinished")->click();
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        }
        storage::closeProfile();
        if (inherited.isEmpty()) {
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("KOBALT_TEST_FINISH_PROFILE", paths.directory);
            environment.insert("KOBALT_TEST_FINISH_BOOK", bookPath);
            child.setProcessEnvironment(environment);
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(), { "finishPromptPersistence" });
            QVERIFY(child.waitForFinished(60000));
            const auto output = child.readAll();
            QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                     output.constData());
        }
    }

    // Checks that missing, unusable and manual-only accounts never sync while reading, the toolbar Sync button and the Automatic sync sheet.
    void kosyncAccountGating_data()
    {
        QTest::addColumn<QString>("account");
        for (const auto *name :
             { "missing", "signed-out", "not-authenticated", "malformed", "future-account",
               "automatic-off", "logout", "feature-disabled", "feature-disabled-manual" })
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void kosyncAccountGating()
    {
        QFETCH(QString, account);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&server, &QTcpServer::newConnection);
        const auto paths = testPaths();
        if (account != "missing") {
            const bool authenticated = account == "automatic-off" || account == "logout"
                    || account.startsWith("feature-disabled");
            QVERIFY(storage::writeJson(
                    paths.syncAccount(),
                    { { "schema", account == "future-account" ? 2 : 1 },
                      { "server", QString("https://127.0.0.1:%1").arg(server.serverPort()) },
                      { "username",
                        account == "signed-out" ? QString() : QStringLiteral("reader") },
                      { "key",
                        authenticated                    ? QString(32, 'a')
                                : account == "malformed" ? QStringLiteral("invalid")
                                                         : QString() },
                      { "device_id", "test-device" },
                      { "automatic",
                        account != "automatic-off" && account != "feature-disabled-manual" } }));
        }
        if (account.startsWith("feature-disabled"))
            QVERIFY(storage::writeJson(paths.preferences(), { { "disable_kosync", true } }));
        const auto accountBytes = [&] {
            QFile file(paths.syncAccount());
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto original = accountBytes();
        auto network = std::make_shared<SyncNetworkState>();
        int factories = 0;
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.syncNetwork = [&] {
                ++factories;
                return std::make_unique<TestSyncNetwork>(network);
            };
            services.monotonicMilliseconds = [&] { return clock; };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        auto *manual = host.window()->findChild<QPushButton *>("rrKosyncManual");
        QVERIFY(manual);
        QCOMPARE(!manual->isHidden(), account == "automatic-off");
        if (account == "logout") {
            QVERIFY(openSettings(host, "rrSettingsKosync"));
            auto *automatic = host.window()->findChild<QPushButton *>("rrKosyncAutomatic");
            QVERIFY(automatic && automatic->isEnabled());
            automatic->click();
            auto *sheet = host.window()->findChild<QWidget *>("rrKosyncAutomaticPopup");
            QVERIFY(sheet);
            auto *off = sheet->findChild<QPushButton *>("rrKosyncAutomaticOff");
            auto *on = sheet->findChild<QPushButton *>("rrKosyncAutomaticOn");
            QVERIFY(off && on && on->isChecked());
            off->click();
            QVERIFY(!manual->isHidden());
            on->click();
            QVERIFY(manual->isHidden());
            // The interval list saves the new period for the next check.
            sheet->findChild<QPushButton *>("rrKosyncInterval")->click();
            auto *intervals = host.window()->findChild<QWidget *>("rrKosyncIntervalPopup");
            QVERIFY(intervals);
            intervals->findChild<QAbstractButton *>("rrKosyncInterval_30")->click();
            QCOMPARE(kosync::Client(paths.syncAccount()).schedule().minutes, 30);
            sheet->findChild<QPushButton *>("rrKosyncAutomaticClose")->click();
            auto *logout = host.window()->findChild<QPushButton *>("rrKosyncLogout");
            QVERIFY(logout && logout->isEnabled());
            logout->click();
            QVERIFY(!kosync::Client(paths.syncAccount()).signedIn());
            QVERIFY(manual->isHidden());
        }
        QCOMPARE(factories, account == "automatic-off" || account == "logout" ? 1 : 0);
        bool presented = false;
        host.window()->preparePresentation([&](bool ready) { presented = ready; });
        QVERIFY(presented);
        QVERIFY(!host.window()->findChild<QWidget *>("rrKosyncOpening"));
        // Inactive accounts never sync while reading, however long since the last sync.
        clock += 61 * 60000;
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy());
        QCoreApplication::processEvents();
        QCOMPARE(network->requests, 0);
        QCOMPARE(attempts.count(), 0);
        host.shutdown();
        QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        QCOMPARE(network->requests, 0);
        QCOMPARE(attempts.count(), 0);
        if (account != "logout")
            QCOMPARE(accountBytes(), original);
    }

    // Missing fields do not submit. Failed sign-in keeps the draft for an explicit retry.
    void kosyncSignInValidation_data()
    {
        QTest::addColumn<QString>("editedField");
        QTest::addColumn<QString>("replacement");
        QTest::newRow("unchanged") << QString() << QString();
        QTest::newRow("username") << QString("rrKosyncUsername") << QString("different-reader");
        QTest::newRow("server") << QString("rrKosyncServer") << QString("https://other.invalid");
    }

    void kosyncSignInValidation()
    {
        QFETCH(QString, editedField);
        QFETCH(QString, replacement);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(),
                                   { { "schema", 1 },
                                     { "server", "https://sync.invalid" },
                                     { "username", "reader" },
                                     { "key", "" },
                                     { "automatic", false },
                                     { "device_id", "sign-in-test" } }));
        auto http = std::make_shared<KosyncHttpState>();
        http->readStatus = 401;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        auto *window = host.window();
        QVERIFY(openSettings(host));
        window->findChild<QPushButton *>("rrSettingsKosync")->click();
        auto *login = window->findChild<QPushButton *>("rrKosyncLogin");
        auto *status = window->findChild<QLabel *>("rrKosyncStatus");
        auto *password =
                dynamic_cast<ui::CaptionButton *>(window->findChild<QWidget *>("rrKosyncPassword"));
        QVERIFY(login && status && password);

        const auto edit = [&](const QString &field, const QString &text) {
            auto *button = window->findChild<QPushButton *>(field);
            QVERIFY(button);
            button->click();
            QPointer<QLineEdit> input = window->findChild<QLineEdit *>("rrKosyncInput");
            QVERIFY(input);
            input->setText(text);
            window->findChild<QPushButton *>("rrKosyncEditSave")->click();
            QTRY_VERIFY(!input);
        };
        login->click();
        QCOMPARE(status->text(), strings::get("kosync.missing_password"));
        QCOMPARE(http->requests, 0);
        const QString draft = QStringLiteral("test-password");
        edit("rrKosyncPassword", draft);
        login->click();
        QTRY_COMPARE(status->text(), strings::get("kosync.auth_error"));
        QCOMPARE(http->requests, 1);
        const auto masked = QStringLiteral("••••••••");
        QCOMPARE(password->caption(), masked);
        if (!editedField.isEmpty())
            edit(editedField, replacement);
        QCOMPARE(password->caption(), masked);
        QCOMPARE(http->requests, 1);

        http->readStatus = 200;
        http->readBody = R"({"authorized":"OK"})";
        login->click();
        QTRY_COMPARE(status->text(), strings::get("kosync.signed_in"));
        QCOMPARE(http->requests, 2);
        const auto server =
                editedField == "rrKosyncServer" ? replacement : QString("https://sync.invalid");
        const auto username = editedField == "rrKosyncUsername" ? replacement : QString("reader");
        QCOMPARE(http->lastAuthentication.url(), QUrl(server + "/users/auth"));
        QCOMPARE(http->lastAuthentication.rawHeader("x-auth-user"), username.toUtf8());
        QCOMPARE(http->lastAuthentication.rawHeader("x-auth-key"),
                 QCryptographicHash::hash(draft.toUtf8(), QCryptographicHash::Md5).toHex());
        const auto saved = storage::readJsonFile(paths.syncAccount()).object;
        QCOMPARE(saved.value("server").toString(), server);
        QCOMPARE(saved.value("username").toString(), username);
        QCOMPARE(saved.value("key").toString().toLatin1(),
                 http->lastAuthentication.rawHeader("x-auth-key"));
        QCOMPARE(password->caption(), strings::get("kosync.not_set"));
        QVERIFY(login->isHidden());
        edit("rrKosyncServer", server);
        QVERIFY(login->isHidden());
        edit("rrKosyncUsername", username);
        QVERIFY(login->isHidden());

        edit("rrKosyncServer", "https://pending.invalid");
        edit("rrKosyncPassword", "pending-test-password");
        QCOMPARE(password->caption(), masked);
        auto *logout = window->findChild<QPushButton *>("rrKosyncLogout");
        QVERIFY(logout && logout->isEnabled() && !logout->isHidden());
        logout->click();
        QCOMPARE(status->text(), strings::get("kosync.signed_out"));
        QCOMPARE(password->caption(), strings::get("kosync.not_set"));
        QVERIFY(!kosync::Client(paths.syncAccount()).signedIn());
        login->click();
        QCOMPARE(status->text(), strings::get("kosync.missing_password"));
        QCOMPARE(http->requests, 2);
    }

    // Checks the opening sync against a stalled server: input blocking, Cancel sync, grace period, timeout and repaint.
    void kosyncOpening_data()
    {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("cancel-sync") << QString("cancel");
        QTest::newRow("offline-deadline") << QString("deadline");
        QTest::newRow("completed") << QString("completed");
    }

    void kosyncOpening()
    {
        QFETCH(QString, outcome);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username", "test-reader" },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", true } }));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        const auto capture = qEnvironmentVariable("KOBALT_KOSYNC_CAPTURE");
        host.open(capture.isEmpty() ? textBook : fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool prepared = false;
        QElapsedTimer openingSync;
        openingSync.start();
        qint64 presentedAt = -1;
        host.window()->preparePresentation([&](bool ready) {
            presentedAt = openingSync.elapsed();
            prepared = ready;
        });
        QVERIFY(!prepared);
        QVERIFY(!host.window()->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrKosyncOpening"));
        QTRY_VERIFY_WITH_TIMEOUT(prepared, 1500);
        QVERIFY2(presentedAt >= 450 && presentedAt < 1700,
                 qPrintable(QString("Opening presentation took %1 ms").arg(presentedAt)));
        QVERIFY(attempts.count() > 0);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrKosyncOpening"));
        QPointer<QWidget> popup = host.window()->findChild<QWidget *>("rrKosyncOpening");
        QTRY_VERIFY(popup->isVisible());
        QTRY_VERIFY(attempts.count() > 0);
        const auto before = host.session()->state().value("position").toString();
        const auto page = host.session()->page();
        host.window()->pageButton(1);
        QCOMPARE(host.session()->page(), page);
        QVERIFY(!host.session()->busy());
        auto *button = popup->findChild<QPushButton *>("rrKosyncCancel");
        QVERIFY(button && button->isVisible());
        QCOMPARE(button->text(), QString("Cancel sync"));
        auto *icon = popup->findChild<QLabel *>("rrKosyncStatusIcon");
        QVERIFY(icon && icon->isVisible() && !icon->pixmap().isNull());
        QVERIFY(icon->parentWidget() == popup->findChild<QLabel *>("rrModalTitle")->parentWidget());
        QVERIFY(!button->font().bold());
        QVERIFY(popup->findChild<QLabel *>("rrModalDescription")
                        ->text()
                        .contains("Kobalt is syncing your progress"));
        if (!capture.isEmpty())
            QVERIFY(host.window()->grab().save(capture));
        auto *paper = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(paper);
        QCoreApplication::processEvents();
        PaintRegions paints;
        paper->installEventFilter(&paints);
        host.window()->setProperty("reader_full_flash", false);
        if (outcome == "cancel") {
            QVERIFY(click(canvas, button));
            QTRY_VERIFY(!popup || !popup->isVisible());
        } else if (outcome == "completed") {
            // Finish the production notice without requiring an HTTPS server.
            // Its timer invokes the reader's normal dismissal callback.
            auto *notice = dynamic_cast<reader::KosyncNotice *>(popup.data());
            QVERIFY(notice);
            notice->complete();
            // Run the dismissal timer instead of waiting for it.
            auto *dismissal = notice->findChild<QTimer *>("rrKosyncSuccessTimer");
            QVERIFY(dismissal && dismissal->isActive());
            dismissal->stop();
            QMetaObject::invokeMethod(dismissal, "timeout", Qt::DirectConnection);
            QTRY_VERIFY(!popup || !popup->isVisible());
        } else {
            // The stalled server never answers, so the sync deadline ends the
            // opening sync. Run it instead of waiting 10 s for it.
            QVERIFY(runKosyncDeadline(host.window()));
            QTRY_VERIFY(!popup || !popup->isVisible());
        }
        QVERIFY(host.window()->property("reader_full_flash").toBool());
        QTRY_COMPARE(paints.painted, QRegion(paper->rect()));
        QCOMPARE(host.session()->state().value("position").toString(), before);
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() > page);
    }

    // Sleep dismisses a revealed editor and clears a saved draft before wake.
    void kosyncPasswordSleep()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        host.window()->findChild<QPushButton *>("rrKosyncPassword")->click();
        QPointer<QLineEdit> input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
        QVERIFY(input);
        input->setText("draft-password");
        host.window()->findChild<QPushButton *>("rrKosyncEditSave")->click();
        QTRY_VERIFY(!input);
        host.window()->findChild<QPushButton *>("rrKosyncPassword")->click();
        input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
        QVERIFY(input && input->text() == "draft-password");
        input->setText("unfinished-password");
        host.window()->findChild<QPushButton *>("rrKosyncPasswordReveal")->click();
        QCOMPARE(input->echoMode(), QLineEdit::Normal);
        host.window()->sleepReader();
        QTRY_VERIFY(!input || !input->isVisible());
        QVERIFY(!input || input->echoMode() == QLineEdit::Password);
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QTRY_VERIFY(!host.window()->findChild<QLineEdit *>("rrKosyncInput"));
        auto *settings = host.window()->findChild<QWidget *>("rrGeneralSettings");
        if (!settings->isVisible())
            host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        host.window()->findChild<QPushButton *>("rrKosyncPassword")->click();
        input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
        QVERIFY(input);
        QCOMPARE(input->echoMode(), QLineEdit::Password);
        QVERIFY(input->text().isEmpty());
    }

    void kosyncPause_data()
    {
        QTest::addColumn<bool>("automatic");
        QTest::addColumn<bool>("signedIn");
        QTest::addColumn<bool>("chapter");
        QTest::newRow("automatic") << true << true << false;
        QTest::newRow("automatic-chapter") << true << true << true;
        QTest::newRow("manual") << false << true << false;
        QTest::newRow("signed-out") << true << false << false;
    }

    // Pause hands off saved progress and reconciliation evidence only for an automatic account.
    void kosyncPause()
    {
        QFETCH(bool, automatic);
        QFETCH(bool, signedIn);
        QFETCH(bool, chapter);
        renderingMode::setOverride(chapter ? 1 : 0);
        const auto restoreMode = qScopeGuard([] { renderingMode::setOverride(-1); });
        const auto paths = testPaths();
        if (signedIn)
            QVERIFY(storage::writeJson(paths.syncAccount(),
                                       { { "schema", 1 },
                                         { "server", "https://127.0.0.1:9" },
                                         { "username", "fixture" },
                                         { "key", QString(32, 'a') },
                                         { "device_id", "fixture-device" },
                                         { "automatic", automatic },
                                         { "sync_on_open", false },
                                         { "sync_on_close", false } }));
        bool suspended = false;
        QJsonObject queued;
        QString persisted;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
            services.deferKosync = [&](const QJsonObject &job) {
                queued = job;
                persisted = storage::readBookState(paths, host.book()->identity())
                                    .object.value("position")
                                    .toString();
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.window()->checkpoint();
        QVERIFY(queued.isEmpty());
        for (int turn = 0; turn < 4; ++turn) {
            QVERIFY(host.session()->turn(1));
            QTRY_VERIFY(!host.session()->busy());
        }
        const auto frame = host.session()->state();
        QCOMPARE(frame.contains("book_fraction"), chapter);
        const double local = double(host.session()->page()) / qMax(1, host.session()->pages() - 1);
        const double percentage = chapter ? frame.value("book_fraction").toDouble() : local;
        if (chapter)
            QVERIFY(local != percentage);
        const auto position = host.session()->state().value("position").toString();
        suspended = true;
        host.window()->checkpoint();
        if (!automatic || !signedIn) {
            QVERIFY(queued.isEmpty());
            return;
        }
        QCOMPARE(queued.value("progress").toString(), position);
        QCOMPARE(persisted, position);
        QCOMPARE(queued.value("percentage").toDouble(-1), percentage);
        QCOMPARE(queued.value("account_path").toString(), paths.syncAccount());
        QCOMPARE(queued.value("account").toObject().value("key_sha256").toString().size(), 64);
        QVERIFY(!queued.contains("key"));
        QVERIFY(!queued.value("baseline").toObject().value("pending").toBool());
    }

    void kosyncSleepSavesSyncTime_data()
    {
        QTest::addColumn<bool>("sleepSync");
        QTest::newRow("reading") << false;
        QTest::newRow("sleep-sync") << true;
    }

    // Checks that sleep saves the sync time, also when the sleep sync ends later, and that a sleep without a sync writes nothing.
    void kosyncSleepSavesSyncTime()
    {
        QFETCH(bool, sleepSync);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(),
                                   { { "schema", 1 },
                                     { "server", "https://127.0.0.1:9" },
                                     { "username", "test-reader" },
                                     { "key", QString(32, 'a') },
                                     { "device_id", "test-device" },
                                     { "automatic", true },
                                     { "sync_on_open", !sleepSync },
                                     { "sync_on_sleep_wake", sleepSync } }));
        const auto savedSyncTime = [&] {
            return storage::readJsonFile(paths.syncAccount()).object.value("last_sync").toString();
        };
        auto http = std::make_shared<KosyncHttpState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [http](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
            services.kosyncSleepWake = true;
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool prepared = false;
        host.window()->preparePresentation([&](bool ready) { prepared = ready; });
        QTRY_VERIFY_WITH_TIMEOUT(prepared, 1500);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QPointer<ReaderWindow> window = host.window();
        QVERIFY(buildKosyncPage(host));
        auto *status = window->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        if (!sleepSync) {
            // The opening sync keeps its time in memory until the sleep.
            QTRY_COMPARE(status->text(), strings::get("kosync.up_to_date"));
            QCOMPARE(http->requests, 2);
            QVERIFY(savedSyncTime().isEmpty());
            window->sleepReader();
            QVERIFY(QDateTime::fromString(savedSyncTime(), Qt::ISODate).isValid());
            const auto modified = QFileInfo(paths.syncAccount()).lastModified();
            const auto contents = storage::readJsonFile(paths.syncAccount()).object;
            window->sleepViewReady();
            window->wakeReader();
            QCoreApplication::processEvents();
            window->sleepReader();
            QCOMPARE(http->requests, 2);
            QCOMPARE(QFileInfo(paths.syncAccount()).lastModified(), modified);
            QCOMPARE(storage::readJsonFile(paths.syncAccount()).object, contents);
            return;
        }
        QCOMPARE(http->requests, 0);
        http->hold = true;
        window->sleepReader();
        // The sleep sync starts after the sleep save and is still in flight.
        QCOMPARE(http->requests, 1);
        QVERIFY(window->pendingNetworkSync());
        QVERIFY(savedSyncTime().isEmpty());
        http->hold = false;
        for (const auto &reply : std::exchange(http->held, {}))
            if (reply)
                QMetaObject::invokeMethod(
                        reply, [reply] { reply->finish(); }, Qt::QueuedConnection);
        QTRY_VERIFY(!window->pendingNetworkSync());
        QCOMPARE(http->requests, 2);
        QTRY_VERIFY(QDateTime::fromString(savedSyncTime(), Qt::ISODate).isValid());
    }

    // Checks the toolbar Sync button: the first-use prompt until confirmed, an interactive request, the busy button and the error notice.
    void kosyncManualToolbar()
    {
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(),
                                   { { "schema", 1 },
                                     { "server", "https://127.0.0.1:1" },
                                     { "username", "test-reader" },
                                     { "key", QString(32, 'a') },
                                     { "device_id", "test-device" },
                                     { "automatic", false } }));
        auto network = std::make_shared<SyncNetworkState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto *window = host.window();
        auto *manual = window->findChild<QPushButton *>("rrKosyncManual");
        QVERIFY(manual && !manual->isHidden());
        manual->click();
        QPointer<QWidget> prompt = window->findChild<QWidget *>("rrKosyncManualPrompt");
        QVERIFY(prompt && prompt->isVisible());
        QCOMPARE(network->requests, 0);
        prompt->findChild<QPushButton *>("rrKosyncManualCancel")->click();
        QVERIFY(!prompt || !prompt->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(kosync::Client(paths.syncAccount()).manualExplanation());
        manual->click();
        prompt = window->findChild<QWidget *>("rrKosyncManualPrompt");
        QVERIFY(prompt && prompt->isVisible());
        prompt->findChild<QPushButton *>("rrKosyncManualConfirm")->click();
        QCOMPARE(network->requests, 1);
        QVERIFY(network->interactive);
        QVERIFY(!kosync::Client(paths.syncAccount()).manualExplanation());
        QVERIFY(!manual->isEnabled());
        auto complete = std::move(network->completed);
        complete(false);
        QVERIFY(manual->isEnabled());
        auto *page = static_cast<ReadingPage *>(window->findChild<QWidget *>("rrPage"));
        QCOMPARE(page->notice, strings::get("kosync.no_connection"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        manual->click();
        QCOMPARE(network->requests, 2);
        QVERIFY(!window->findChild<QWidget *>("rrKosyncManualPrompt"));
        complete = std::move(network->completed);
        complete(false);
        window->sleepReader();
        QCOMPARE(network->requests, 2);
        QVERIFY(!kosync::Client(paths.syncAccount()).manualExplanation());
    }

    // The reader sends the book's file name and title only while Send document metadata is checked.
    void kosyncMetadata()
    {
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(),
                                   { { "schema", 1 },
                                     { "server", "https://fixture.invalid" },
                                     { "username", "reader" },
                                     { "key", QString(32, 'a') },
                                     { "device_id", "fixture" },
                                     { "automatic", false },
                                     { "send_metadata", true } }));
        auto http = std::make_shared<KosyncHttpState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [http](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        auto *checkbox = host.window()->findChild<QCheckBox *>("rrKosyncMetadata");
        QVERIFY(checkbox && checkbox->isChecked());
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        QTRY_COMPARE(http->uploads, 1);
        const auto metadata = http->lastUpload.value("metadata").toObject();
        QCOMPARE(metadata.value("filename").toString(), QFileInfo(textBook).fileName());
        QVERIFY(!host.session()->title().isEmpty());
        QCOMPARE(metadata.value("title").toString(), host.session()->title().trimmed());
        // Unchecking saves the choice through the reader's own handler. The
        // checkbox is disabled until the sync has finished.
        QTRY_VERIFY(checkbox->isEnabled());
        checkbox->click();
        QVERIFY(!checkbox->isChecked());
        QCOMPARE(storage::readJson(paths.syncAccount()).value("send_metadata").toBool(true), false);
    }

    void kosyncMissingProgress_data()
    {
        QTest::addColumn<int>("statusCode");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("missing");
        QTest::newRow("empty-object") << 200 << QByteArray("{}") << true;
        QTest::newRow("empty-array") << 200 << QByteArray("[]") << true;
        QTest::newRow("grimmory-empty-progress")
                << 200
                << QByteArray(
                           R"({"document":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","progress":null,"percentage":null,"timestamp":null,"device":"Grimmory","device_id":"Grimmory"})")
                << true;
        QTest::newRow("missing-progress") << 404 << QByteArray(R"({"status":"not found"})") << true;
        QTest::newRow("grimmory-missing-progress")
                << 404
                << QByteArray(
                           R"({"status":404,"message":"No progress found for user and book","timestamp":"2026-11-15T12:00:00"})")
                << true;
        QTest::newRow("grimmory-missing-book")
                << 404
                << QByteArray(
                           R"({"status":404,"message":"Book not found for hash aaaa","timestamp":"2026-11-15T12:00:00"})")
                << false;
        QTest::newRow("missing-route") << 404 << QByteArray("<html>Not Found</html>") << false;
        QTest::newRow("ambiguous-404") << 404 << QByteArray("{}") << false;
        QTest::newRow("auth-failure") << 401 << QByteArray(R"({"status":"not found"})") << false;
        QTest::newRow("server-failure") << 500 << QByteArray(R"({"status":"not found"})") << false;
    }

    void kosyncMissingProgress()
    {
        QFETCH(int, statusCode);
        QFETCH(QByteArray, body);
        QFETCH(bool, missing);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(),
                                   { { "schema", 1 },
                                     { "server", "https://fixture.invalid" },
                                     { "username", "reader" },
                                     { "key", QString(32, 'a') },
                                     { "device_id", "fixture" },
                                     { "automatic", false } }));
        auto http = std::make_shared<KosyncHttpState>();
        http->readStatus = statusCode;
        http->readBody = body;
        http->uploadBody = R"({"status":"progress updated"})";
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [http](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto position = host.session()->state().value("position");
        host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        if (missing) {
            QTRY_VERIFY(showsSyncTime(host.window()));
            QCOMPARE(http->uploads, 1);
            QCOMPARE(http->requests, 2);
        } else {
            auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
            QVERIFY(status);
            QTRY_COMPARE(status->text(),
                         strings::get(statusCode == 401           ? "kosync.auth_error"
                                              : statusCode == 500 ? "kosync.server_error"
                                                                  : "kosync.route_error"));
            QCOMPARE(http->uploads, 0);
            QCOMPARE(http->requests, 1);
            QVERIFY(!showsSyncTime(host.window()));
        }
        QCOMPARE(host.session()->state().value("position"), position);
    }

    // An invalid remote position keeps the local position even if disabling automatic sync fails.
    void kosyncInvalidPosition_data()
    {
        QTest::addColumn<bool>("saveFails");
        QTest::newRow("saved") << false;
        QTest::newRow("save-failed") << true;
    }

    void kosyncInvalidPosition()
    {
        QFETCH(bool, saveFails);
        const auto paths = testPaths();
        const auto account = paths.syncAccount();
        QVERIFY(storage::writeJson(account,
                                   { { "schema", 1 },
                                     { "server", "https://fixture.invalid" },
                                     { "username", "reader" },
                                     { "key", QString(32, 'a') },
                                     { "device_id", "local" },
                                     { "automatic", true },
                                     { "sync_on_open", false } }));
        auto http = std::make_shared<KosyncHttpState>();
        http->readBody =
                R"({"progress":"/not/a/real/position","percentage":0.7,"device":"Other","device_id":"remote","timestamp":1})";
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [http](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(buildKosyncPage(host));
        const auto position = host.session()->state().value("position");
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        ui::ChoiceModal *choice = nullptr;
        QTRY_VERIFY((choice = dynamic_cast<ui::ChoiceModal *>(
                             host.window()->findChild<QWidget *>("rrKosyncChoice")))
                    && choice->isVisible() && choice->primaryButton()->isEnabled());
        if (saveFails)
            QVERIFY(storage::writeJson(account, { { "schema", 99 }, { "automatic", true } }));
        choice->primaryButton()->click();
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        QTRY_COMPARE(status->text(),
                     strings::get(saveFails ? "kosync.storage_error" : "kosync.position_error"));
        QCOMPARE(host.session()->state().value("position"), position);
        QCOMPARE(storage::readJson(account).value("automatic").toBool(), saveFails);
        QCOMPARE(http->uploads, 0);
    }

    void kosyncFailure_data()
    {
        QTest::addColumn<bool>("upload");
        QTest::newRow("read") << false;
        QTest::newRow("upload") << true;
    }

    // A failed read or upload keeps its explanation visible and leaves progress unchanged.
    void kosyncFailure()
    {
        QFETCH(bool, upload);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(),
                                   { { "schema", 1 },
                                     { "server", "https://fixture.invalid" },
                                     { "username", "reader" },
                                     { "key", QString(32, 'a') },
                                     { "device_id", "fixture" },
                                     { "automatic", false } }));
        auto http = std::make_shared<KosyncHttpState>();
        http->readStatus = upload ? 200 : 401;
        http->uploadStatus = 401;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [http](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto position = host.session()->state().value("position");
        host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        QTRY_COMPARE(status->text(), strings::get("kosync.auth_error"));
        QCOMPARE(http->requests, upload ? 2 : 1);
        QCOMPARE(host.session()->state().value("position"), position);
        QVERIFY(!showsSyncTime(host.window()));
        auto *next = host.window()->findChild<QPushButton *>("rrSettingsOptionsNext");
        for (int page = 0; page < 10 && next && next->isVisible() && next->isEnabled()
             && status->visibleRegion().boundingRect() != status->rect();
             ++page) {
            next->click();
            QCoreApplication::processEvents();
        }
        QTRY_COMPARE(status->visibleRegion().boundingRect(), status->rect());
        QVERIFY(status->height() >= status->heightForWidth(status->width()));
    }

    // A response received under a native dialog resumes when the host releases input.
    void kosyncDialogResume()
    {
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(),
                                   { { "schema", 1 },
                                     { "server", "https://fixture.invalid" },
                                     { "username", "reader" },
                                     { "key", QString(32, 'a') },
                                     { "device_id", "fixture" },
                                     { "automatic", false } }));
        auto http = std::make_shared<KosyncHttpState>();
        http->hold = true;
        bool suspended = false;
        WindowPresentation *presentation = nullptr;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
            services.suspended = [&] { return suspended; };
            services.presentation = [&](QWidget *) {
                auto result = std::make_unique<test::HostPresentation>();
                presentation = result.get();
                return result;
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(presentation && presentation->inputResumed);
        const auto position = host.session()->state().value("position");
        host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        QTRY_COMPARE(http->held.size(), 1);
        suspended = true;
        http->held.takeFirst()->finish();
        QCOMPARE(http->uploads, 0);
        auto *deadline = host.window()->findChild<QTimer *>("rrKosyncDeadline");
        QVERIFY(deadline && deadline->isActive());
        suspended = false;
        http->hold = false;
        presentation->inputResumed();
        QTRY_COMPARE(http->uploads, 1);
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        QTRY_COMPARE(status->text(), strings::get("kosync.up_to_date"));
        QCOMPARE(http->requests, 2);
        QVERIFY(!deadline->isActive());
        QCOMPARE(host.session()->state().value("position"), position);
    }

    // Checks Sync now in Settings: no deadline while a connection waits for permission, then the Syncing notice and its Cancel.
    void kosyncManualConnection_data()
    {
        QTest::addColumn<QString>("connection");
        for (const auto *connection : { "permission", "connected", "desktop" })
            QTest::newRow(connection) << QString::fromLatin1(connection);
    }

    void kosyncManualConnection()
    {
        QFETCH(QString, connection);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username", "test-reader" },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", false } }));
        auto network = std::make_shared<SyncNetworkState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        if (connection != "desktop")
            host.configureServices = [network](ReaderHostServices &services) {
                services.syncNetwork = [network] {
                    return std::make_unique<TestSyncNetwork>(network);
                };
            };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        auto *settings = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QTRY_VERIFY(settings->isVisible());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrKosyncNow")));
        if (connection != "desktop") {
            QCOMPARE(network->requests, 1);
            QVERIFY(network->interactive);
            QVERIFY(!host.window()->findChild<QWidget *>("rrKosyncOpening"));
            if (connection == "permission") {
                // An interactive connection arms the sync deadline only once
                // it connects, so a permission prompt can take as long as the
                // user needs. Check that no reader timer is left that could
                // end the wait, instead of waiting past the 10 s deadline.
                QTRY_VERIFY2_WITH_TIMEOUT(
                        activeReaderTimers(host.window()).isEmpty(),
                        qPrintable("Active reader timers: "
                                   + activeReaderTimers(host.window()).join(", ")),
                        10000);
                auto *deadline = host.window()->findChild<QTimer *>("rrKosyncDeadline");
                QVERIFY(deadline && !deadline->isActive());
            } else {
                QEventLoop permission;
                QTimer::singleShot(100, &permission, &QEventLoop::quit);
                permission.exec();
            }
            QVERIFY(bool(network->completed));
            QVERIFY(!host.window()->findChild<QPushButton *>("rrKosyncServer")->isEnabled());
            QCOMPARE(network->cancellations, 0);
            QCOMPARE(attempts.count(), 0);
            auto completed = std::move(network->completed);
            network->completed = {};
            network->connected = connection == "connected";
            completed(network->connected);
            if (!network->connected)
                QCOMPARE(host.window()->findChild<QLabel *>("rrKosyncStatus")->text(),
                         strings::get("kosync.no_connection"));
        }
        if (connection != "permission") {
            QPointer<QWidget> popup = host.window()->findChild<QWidget *>("rrKosyncOpening");
            QVERIFY(popup && popup->isVisible());
            QTRY_VERIFY(attempts.count() > 0);
            QVERIFY(kosync::networkInUse(0));
            const auto capture = qEnvironmentVariable("KOBALT_KOSYNC_MANUAL_CAPTURE");
            if (!capture.isEmpty() && connection == "connected")
                QVERIFY(host.window()->grab().save(capture));
            const auto page = host.session()->page();
            host.window()->pageButton(1);
            QCOMPARE(host.session()->page(), page);
            auto *cancel = popup->findChild<QPushButton *>("rrKosyncCancel");
            QVERIFY(cancel && click(canvas, cancel));
            QTRY_VERIFY(!popup || !popup->isVisible());
            // Includes an outstanding reply, lookup, or scheduled HTTP retry.
            QVERIFY(!kosync::networkInUse(0));
            auto *deadline = host.window()->findChild<QTimer *>("rrKosyncDeadline");
            QVERIFY(deadline && !deadline->isActive());
            QCOMPARE(host.window()->findChild<QLabel *>("rrKosyncStatus")->text(),
                     strings::get("kosync.skipped"));
        }
        QVERIFY(settings->isVisible());
        QVERIFY(host.window()->findChild<QPushButton *>("rrKosyncServer")->isEnabled());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
        QTRY_VERIFY(!settings->isVisible());
    }

    // Checks that the opening sync sends no HTTP before a connection is ready: Cancel, the deadline, sleep, and a later Sync now or Sign in.
    void kosyncNetwork_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : { "cancel", "deadline", "ready", "sleep", "login" })
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void kosyncNetwork()
    {
        QFETCH(QString, scenario);
        verifyKosyncNetwork(scenario);
    }

    // Checks that each sync moment switched off starts no connection, and how a skipped opening sync starts the interval.
    void kosyncMoments_data()
    {
        QTest::addColumn<int>("minutes");
        QTest::addColumn<bool>("automaticAfterWake");
        QTest::newRow("never-while-reading") << kosync::neverWhileReading << false;
        QTest::newRow("fifteen-minutes") << 15 << false;
        QTest::newRow("automatic-after-wake") << 15 << true;
    }

    // Each moment has its own switch. With opening, sleep and wake and
    // periodic syncs turned off, only Sync now contacts the network. A skipped
    // opening sync starts the interval, so the first page does not sync early.
    // A wake whose sync could not run does not decide the switch of the
    // opening sync that turning automatic sync on starts later.
    void kosyncMoments()
    {
        QFETCH(int, minutes);
        QFETCH(bool, automaticAfterWake);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username", "test-reader" },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", !automaticAfterWake },
                  { "sync_interval_minutes", minutes },
                  { "sync_on_open", automaticAfterWake },
                  { "sync_on_sleep_wake", false },
                  { "sync_on_close", false } }));
        auto network = std::make_shared<SyncNetworkState>();
        // Start far past every interval, as a device clock counted from boot does.
        qint64 clock = 2 * 60 * 60000;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool presented = false;
        host.window()->preparePresentation([&](bool ready) { presented = ready; });
        QVERIFY(presented);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QCoreApplication::processEvents();
        QVERIFY(!host.window()->findChild<QWidget *>("rrKosyncOpening"));
        QCOMPARE(network->requests, 0);
        QVERIFY(buildKosyncPage(host));
        QCOMPARE(network->requests, 0);
        if (automaticAfterWake) {
            // With automatic sync off, the wake sync cannot run.
            host.window()->sleepReader();
            host.window()->wakeReader();
            QCoreApplication::processEvents();
            QCOMPARE(network->requests, 0);
            host.window()->findChild<QPushButton *>("rrKosyncAutomatic")->click();
            auto *sheet = host.window()->findChild<QWidget *>("rrKosyncAutomaticPopup");
            QVERIFY(sheet);
            sheet->findChild<QPushButton *>("rrKosyncAutomaticOn")->click();
            sheet->findChild<QPushButton *>("rrKosyncAutomaticClose")->click();
            QVERIFY(kosync::Client(paths.syncAccount()).automatic());
            // The next page starts an opening sync, which follows the opening
            // switch, not the wake switch.
            QVERIFY(host.session()->turn(1));
            QTRY_VERIFY(!host.session()->busy());
            QCoreApplication::processEvents();
            QCOMPARE(network->requests, 1);
            QVERIFY(!network->interactive);
            return;
        }
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        status->setText("unchanged");
        int direction = 1;
        const auto readOn = [&](int elapsed) {
            clock += qint64(elapsed) * 60000;
            QVERIFY(host.session()->turn(direction));
            direction = -direction;
            QTRY_VERIFY(!host.session()->busy());
            QCoreApplication::processEvents();
        };
        readOn(1);
        QCOMPARE(status->text(), QString("unchanged"));
        const auto lastClock = clock;
        clock = -1;
        readOn(0);
        QCOMPARE(status->text(), QString("unchanged"));
        clock = lastClock - 5 * 60000;
        readOn(0);
        QCOMPARE(status->text(), QString("unchanged"));
        clock = lastClock;
        readOn(0);
        QCOMPARE(status->text(), QString("unchanged"));
        // Never while reading: no page syncs. Every 15 minutes: the interval
        // counts from the skipped opening sync, and Wait keeps the sync local.
        readOn(15);
        QCOMPARE(status->text(),
                 minutes < 0 ? QString("unchanged") : strings::get("kosync.no_connection"));
        QCOMPARE(network->requests, 0);
        host.window()->sleepReader();
        host.window()->wakeReader();
        QCoreApplication::processEvents();
        QCOMPARE(network->requests, 0);
        QVERIFY(!host.window()->findChild<QWidget *>("rrKosyncOpening"));
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        QCOMPARE(network->requests, 1);
        QVERIFY(network->interactive);
    }

    // The reader wakes once a minute, just after the minute changes, with a
    // precise timer. A timer that fires early only re-arms; the minute work
    // runs once.
    void minuteClock()
    {
        auto paths = testPaths();
        int timeOfDay = 10 * 3600000 + 30000;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&timeOfDay](ReaderHostServices &services) {
            services.localDateTime = [&timeOfDay] { return testLocalTime(timeOfDay); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool presented = false;
        host.window()->preparePresentation([&](bool ready) { presented = ready; });
        QVERIFY(presented);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *timer = host.window()->findChild<QTimer *>("rrClockTimer");
        QVERIFY(timer && timer->isActive() && timer->isSingleShot());
        QCOMPARE(timer->timerType(), Qt::PreciseTimer);
        QCOMPARE(timer->interval(), 30050);
        int ticks = 0;
        host.window()->minuteTicked = [&ticks] { ++ticks; };
        // An early fire at 10:00:59.640 re-arms for the rest of the minute.
        timeOfDay = 10 * 3600000 + 59640;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(ticks, 0);
        QVERIFY(timer->isActive());
        QCOMPARE(timer->interval(), 410);
        // The fire after the minute changes runs the minute work once.
        timeOfDay = 10 * 3600000 + 60050;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(ticks, 1);
        QCOMPARE(timer->interval(), 60000);
        timeOfDay = 10 * 3600000 + 60060;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(ticks, 1);
        // Midnight is a new minute too.
        timeOfDay = 23 * 3600000 + 59 * 60000 + 30000;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(ticks, 2);
        timeOfDay = 50;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(ticks, 3);
        QCOMPARE(timer->interval(), 60000);
        // The wall clock advances during standby while the Qt timer is paused.
        timeOfDay += 120000;
        host.window()->resumeReadingClock();
        QTRY_COMPARE(ticks, 4);
        host.window()->resumeReadingClock();
        QCOMPARE(timer->interval(), 60000);
        QCOMPARE(ticks, 4);
    }

    // Checks that the minute clock rests while no strip field, menu or host needs it, and that only a visible consumer asks for the reading pace.
    void clockRestsWithoutConsumers()
    {
        auto paths = testPaths();
        // The phone strips show pages left and a percentage: nothing that changes with time.
        QVERIFY(storage::writePreferences(paths.preferences(),
                                          QJsonObject{ { "header_footer", "phone" } }));
        int timeOfDay = 10 * 3600000 + 30000;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&timeOfDay](ReaderHostServices &services) {
            services.localDateTime = [&timeOfDay] { return testLocalTime(timeOfDay); };
        };
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *timer = host.window()->findChild<QTimer *>("rrClockTimer");
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        auto *menu = host.window()->findChild<QWidget *>("rrControlsTop");
        QVERIFY(timer && page && menu);
        // Nothing needs the minute: the reader does not wake for it.
        QVERIFY(!timer->isActive());
        QCOMPARE(host.progress->minuteTicks, 0);

        // The menu shows a clock, so it starts the timer and asks for the pace.
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(menu->isVisible());
        QVERIFY(timer->isActive());
        QCOMPARE(timer->interval(), 30050);
        QCOMPARE(host.progress->minuteTicks, 1);
        timeOfDay = 10 * 3600000 + 60050;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(host.progress->minuteTicks, 2);
        QVERIFY(timer->isActive());

        // With the menu closed, the next tick does not ask and does not re-arm.
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(!menu->isVisible());
        timeOfDay = 10 * 3600000 + 120050;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(host.progress->minuteTicks, 2);
        QVERIFY(!timer->isActive());

        // A host with minute work keeps the clock, but not the pace query.
        int ticks = 0;
        host.window()->minuteTicked = [&ticks] { ++ticks; };
        host.window()->hide();
        host.window()->show();
        QTRY_VERIFY(timer->isActive());
        timeOfDay = 10 * 3600000 + 180050;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(ticks, 1);
        QCOMPARE(host.progress->minuteTicks, 2);
        QVERIFY(timer->isActive());
    }

    // Checks that a strip field with time left asks for the reading pace on each minute tick.
    void clockAsksPaceForTimeLeft()
    {
        auto paths = testPaths();
        QVERIFY(storage::writePreferences(
                paths.preferences(),
                QJsonObject{ { "header_footer", "custom" },
                             { "header_footer_custom",
                               QJsonObject{ { "top_left", "book_time_left" },
                                            { "top_right", "time" } } } }));
        int timeOfDay = 10 * 3600000 + 30000;
        int clockReads = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.clock = [&] {
                ++clockReads;
                return QTime::fromMSecsSinceStartOfDay(timeOfDay).toString("HH:mm");
            };
            services.localDateTime = [&timeOfDay] { return testLocalTime(timeOfDay); };
        };
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *timer = host.window()->findChild<QTimer *>("rrClockTimer");
        QVERIFY(timer && timer->isActive());
        const int before = host.progress->minuteTicks;
        timeOfDay = 10 * 3600000 + 60050;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(host.progress->minuteTicks, before + 1);
        auto *page = dynamic_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page && page->headerText().contains("10:01"));
        const int stripReads = clockReads;
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        auto *menuClock = host.window()->findChild<QLabel *>("rrMenuClock");
        QTRY_VERIFY(menuClock && menuClock->isVisible());
        QCOMPARE(menuClock->text(), QStringLiteral("10:01"));
        QCOMPARE(clockReads, stripReads);
    }

    void liveBatteryIndicators_data()
    {
        QTest::addColumn<bool>("zen");
        QTest::newRow("normal-menu") << false;
        QTest::newRow("zen-menu") << true;
    }

    void liveBatteryIndicators()
    {
        QFETCH(bool, zen);
        const auto paths = testPaths();
        QVERIFY(storage::writePreferences(
                paths.preferences(),
                QJsonObject{ { "zen_mode", zen },
                             { "header_footer", "custom" },
                             { "header_footer_custom",
                               QJsonObject{ { "top_left", "nothing" },
                                            { "top_right", "battery_percent" } } } }));
        BatteryStatus status{ 21, false, false };
        int reads = 0;
        std::function<void()> changed;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.localDateTime = [] { return testLocalTime(7 * 3600000); };
            services.battery = [&] {
                ++reads;
                return status;
            };
            services.observeBattery = [&](QObject *, std::function<void()> notify) {
                changed = std::move(notify);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        // The shorter viewport puts the battery choices on a later chooser page.
        const QSize viewport = zen ? nativeSize : QSize(900, 1200);
        host.open(textBook, viewport);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), viewport);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = dynamic_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        auto *menu = host.window()->findChild<QWidget *>("rrControlsTop");
        auto *indicator = dynamic_cast<ui::BatteryIndicator *>(
                host.window()->findChild<QWidget *>("rrMenuBattery"));
        QVERIFY(page && menu && indicator && changed);
        QVERIFY(!menu->isVisible());
        const int startingPage = host.session()->page();
        const auto notify = [&](int percent, bool charging) {
            status = { percent, charging, false };
            const int before = reads;
            // The notification is synchronous. Opening Layout can also queue
            // an independent preview completion that refreshes the reader.
            changed();
            QCOMPARE(reads, before + 1);
        };
        const auto &field = page->slotContents[2];
        QVERIFY(field.batteryIcon && !field.charging);
        QCOMPARE(field.battery, 21);
        const auto unplugged = page->headerImage();
        notify(21, true);
        QVERIFY(field.charging);
        const auto plugged = page->headerImage();
        QVERIFY(plugged != unplugged);
        notify(21, false);
        QVERIFY(!field.charging);
        QCOMPARE(page->headerImage(), unplugged);
        if (!zen) {
            auto *viewer = dynamic_cast<ImageViewer *>(
                    host.window()->findChild<QWidget *>("rrImageViewer"));
            QVERIFY(viewer);
            viewer->setGeometry(host.window()->rect());
            viewer->open();
            QVERIFY(viewer->isVisible());
            QVERIFY(!page->visibleRegion().intersects(page->headerTextRect()));
            notify(21, true);
            viewer->closeImage();
            QVERIFY(!viewer->isVisible());
            QCOMPARE(page->headerImage(), plugged);
            notify(21, false);
            QCOMPARE(page->headerImage(), unplugged);
        }

        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(menu->isVisible());
        QCOMPARE(indicator->isVisible(), !zen);
        notify(20, true);
        QVERIFY(indicator->isVisible());
        QCOMPARE(indicator->level(), 20);
        QVERIFY(indicator->accessibleName().contains(strings::get("components.charging")));
        notify(20, false);
        QVERIFY(indicator->isVisible());
        QVERIFY(!indicator->accessibleName().contains(strings::get("components.charging")));
        notify(21, false);
        QCOMPARE(indicator->isVisible(), !zen);
        if (!zen) {
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLayout")));
            auto *preview = host.window()->findChild<QWidget *>("rrAppearancePreview");
            QVERIFY(preview && preview->isVisible());
            const auto layoutUnplugged = preview->grab().toImage();
            notify(21, true);
            QVERIFY(preview->grab().toImage() != layoutUnplugged);
            notify(21, false);
            QCOMPARE(preview->grab().toImage(), layoutUnplugged);
            QVERIFY(click(canvas, host.window()->findChild<QWidget *>("rrHeaderFooter")));
            auto *slot = host.window()->findChild<QPushButton *>("rrHeaderFooterSlot2");
            QVERIFY(slot && slot->isVisible());
            notify(46, false);
            QVERIFY(slot->accessibleName().contains(strings::percentage(46)));
            const auto slotUnplugged = slot->grab().toImage();
            notify(46, true);
            QVERIFY(slot->grab().toImage() != slotUnplugged);
            notify(46, false);
            QCOMPARE(slot->grab().toImage(), slotUnplugged);

            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrHeaderFooterSlot0")));
            auto *counter = host.window()->findChild<QLabel *>("rrHeaderFooterFieldsPage");
            auto *title = host.window()->findChild<QLabel *>("rrHeaderFooterChooserTitle");
            auto *selected =
                    host.window()->findChild<QAbstractButton *>("rrHeaderFooterField_nothing");
            auto *sample = host.window()->findChild<QAbstractButton *>(
                    "rrHeaderFooterField_battery_percent");
            QVERIFY(counter && title && selected && sample && selected->isChecked());
            const auto firstPage = counter->text();
            const auto selectedSlot = title->text();
            auto *next = host.window()->findChild<QPushButton *>("rrHeaderFooterFieldsNext");
            for (int i = 0; !sample->isVisible() && i < 10; ++i)
                QVERIFY(click(canvas, next));
            QVERIFY(sample->isVisible());
            const auto samplePage = counter->text();
            QVERIFY(samplePage != firstPage);
            const auto sampleUnplugged = sample->grab().toImage();
            notify(46, true);
            QVERIFY(sample->isVisible());
            QCOMPARE(counter->text(), samplePage);
            QCOMPARE(title->text(), selectedSlot);
            QVERIFY(selected->isChecked());
            QVERIFY(sample->grab().toImage() != sampleUnplugged);
            notify(46, false);
            QCOMPARE(sample->grab().toImage(), sampleUnplugged);
        }
        QCOMPARE(host.session()->page(), startingPage);
    }

    // Checks that a page turn with the menu hidden leaves the menu's controls alone, and that opening the menu brings them up to date.
    void turnSkipsHiddenControls()
    {
        const auto paths = testPaths();
        const auto readingFace = test::alternateReadingFace();
        QVERIFY(storage::writePreferences(
                paths.preferences(),
                QJsonObject{ { "default_font_face", readingFace },
                             { "header_footer", "custom" },
                             { "header_footer_custom",
                               QJsonObject{ { "header_size", "off" },
                                            { "top_left", "time" },
                                            { "top_right", "battery_percent_only" } } } }));
        int batteryReads = 0, percent = 73;
        int clockReads = 0;
        auto local = testLocalTime(7 * 3600000);
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.localDateTime = [&] { return local; };
            services.clock = [&] {
                ++clockReads;
                return QStringLiteral("7:01 AM");
            };
            services.battery = [&] {
                ++batteryReads;
                return BatteryStatus{ percent, false, false };
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        auto *menu = host.window()->findChild<QWidget *>("rrControlsTop");
        auto *scrubber = dynamic_cast<ui::PageScrubber *>(
                host.window()->findChild<QWidget *>("rrPageScrubber"));
        QVERIFY(page && menu && scrubber);
        auto *bookPosition = scrubber->findChild<QSlider *>("rrBookPositionIndicator");
        QVERIFY(bookPosition);
        const auto controlsPosition = [&] {
            return qMakePair(scrubber->slider()->value(), bookPosition->value());
        };
        const auto shownPosition = [&] {
            return host.session()->state().contains("book_fraction") ? bookPosition->value()
                                                                     : scrubber->slider()->value();
        };
        const auto expectedPosition = [&] {
            if (!host.session()->state().contains("book_fraction"))
                return host.session()->page();
            return host.session()->bookPages() > 0 ? host.session()->bookPage()
                                                   : host.session()->mainPercent();
        };
        // Opening syncs the controls once, hidden.
        QCoreApplication::processEvents();
        auto *marks =
                dynamic_cast<BookmarkList *>(host.window()->findChild<QWidget *>("rrBookmarkList"));
        QVERIFY(marks && !marks->isVisible());
        QCOMPARE(host.session()->fontFace(), readingFace);
        const QString hiddenFace = QStringLiteral("IBM Plex Mono");
        marks->setExcerptFont(hiddenFace);
        batteryReads = 0;
        clockReads = 0;
        local = local.addSecs(60);
        const auto start = host.session()->state().value("position");
        const auto shown = controlsPosition();
        test::skipPageTurnWindow();
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->state().value("position") != start);
        // The refresh after the painted turn runs from the event loop.
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCOMPARE(controlsPosition(), shown);
        QCOMPARE(marks->excerptFont().family(), hiddenFace);
        QVERIFY(!menu->isVisible());
        QCOMPARE(batteryReads, 0);
        QCOMPARE(clockReads, 0);
        percent = 64;
        // The menu opens with the page that is shown now.
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(menu->isVisible());
        QTRY_COMPARE(shownPosition(), expectedPosition());
        QVERIFY(controlsPosition() != shown);
        const auto *indicator = dynamic_cast<ui::BatteryIndicator *>(
                host.window()->findChild<QWidget *>("rrMenuBattery"));
        QVERIFY(indicator && indicator->isVisible());
        QCOMPARE(indicator->level(), percent);
        QCOMPARE(clockReads, 1);
        QCOMPARE(marks->excerptFont().family(), hiddenFace);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrBookmarks")));
        QTRY_VERIFY(marks->isVisible());
        QCOMPARE(marks->excerptFont().family(), readingFace);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrBookmarkListClose")));
        QTRY_VERIFY(!marks->isVisible());
        QVERIFY(menu->isVisible());
        const auto firstTurn = host.session()->state().value("position");
        const auto open = controlsPosition();
        // With the menu open, a page button turn updates the controls at once.
        test::skipPageTurnWindow();
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy()
                    && host.session()->state().value("position") != firstTurn);
        QTRY_COMPARE(shownPosition(), expectedPosition());
        QVERIFY(controlsPosition() != open);
        percent = 39;
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLayout")));
        QVERIFY(click(canvas, host.window()->findChild<QWidget *>("rrHeaderFooter")));
        auto *slot = host.window()->findChild<QPushButton *>("rrHeaderFooterSlot2");
        QVERIFY(slot && slot->isVisible());
        QVERIFY(slot->accessibleName().contains(strings::percentage(percent)));
        const auto *timeSlot = host.window()->findChild<QPushButton *>("rrHeaderFooterSlot0");
        QVERIFY(timeSlot && timeSlot->accessibleName().contains("7:01 AM"));
        QVERIFY(click(canvas, slot));
        auto *chooser = host.window()->findChild<QWidget *>("rrHeaderFooterChooser");
        QTRY_VERIFY(chooser && chooser->isVisible());
        QCOMPARE(clockReads, 1);
    }

    // A preview of the same book preempts estimates without abandoning their
    // continuation or changing the reading position.
    void appearancePreviewInterruptsEstimate()
    {
        renderingMode::setOverride(1);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QTemporaryDir logs;
        initializeDiagnostics(logs.path());
        pauseDiagnostics(false);

        struct Trace
        {
            std::atomic_bool armed{ false }, started{ false }, interrupted{ false },
                    sample{ false };
            std::atomic_int completed{ 0 };
        };

        auto trace = std::make_shared<Trace>();
        setDiagnosticObserver([trace](const QJsonObject &event) {
            if (event.value("work") != "Page estimates")
                return;
            if (event.value("event") == "engine-work-start" && !trace->armed.exchange(true)) {
                engineActivity().pause(true);
                trace->started = true;
            }
            if (event.value("event") == "engine-work-end") {
                if (event.value("completed").toBool())
                    ++trace->completed;
                else
                    trace->interrupted = true;
            }
        });
        const auto reset = qScopeGuard([] {
            engineActivity().pause(false);
            setDiagnosticObserver({});
            pauseDiagnostics(true);
            renderingMode::setOverride(-1);
        });
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(trace->started.load(), 10000);
        const auto position = host.session()->state().value("position");
        StyleSampleRequest request;
        request.settings = styleSampleSettings(host.session()->state());
        request.key = "font_pixels";
        request.values = { 28 };
        request.page = { 600, 800 };
        request.thumbnail = { 120, 160 };
        renderStyleSample(host.session()->fontInputs(), request, host.window(),
                          [trace](StyleSample sample) { trace->sample = sample.valid(); });
        engineActivity().pause(false);
        QTRY_VERIFY_WITH_TIMEOUT(trace->interrupted.load() && trace->sample.load()
                                         && trace->completed.load() > 0
                                         && host.session()->totalWords() > 0,
                                 10000);
        QCOMPARE(host.session()->state().value("position"), position);
    }

    // Checks that neither the Text and Layout controls nor Settings are built
    // with the window, and that the reader builds them by itself, one after
    // the other, once the book's background work has settled and nobody has
    // touched the screen, so that their first opening is quick.
    void panelsAfterBackgroundWork()
    {
        auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QVERIFY(!host.window()->findChild<QWidget *>("rrTextSize"));
        QVERIFY(!host.window()->findChild<QWidget *>("rrGeneralSettings"));
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrTextSize"), 15000);
        // One panel per quiet moment: Settings comes later.
        QVERIFY(!host.window()->findChild<QWidget *>("rrGeneralSettings"));
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrGeneralSettings"), 15000);
        auto *settings = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(settings->isHidden());
        QCOMPARE(settings->parentWidget(), static_cast<QWidget *>(host.window()));
        // Opening it uses the panel that is already there.
        QCOMPARE(openSettings(host), settings);
    }

    // Checks that opening a book draws no text preset tiles: a tile depends
    // only on its preset and is drawn when a preset is saved or a gallery
    // first needs it, so presentation never waits for one.
    void textPresetsWithoutOpeningWait()
    {
        auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool presented = false;
        host.window()->preparePresentation([&](bool ready) { presented = ready; });
        QTRY_VERIFY_WITH_TIMEOUT(presented, 10000);
        Canvas canvas(host.window(), nativeSize);
        const int start = host.session()->page();
        QVERIFY(host.session()->turn(1));
        QTRY_COMPARE(host.session()->page(), start + 1);
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.window()->property("text_preset_tile_draws").toInt(), 0);
    }

    // While a book sits open and nobody reads, nothing on the main thread may
    // wake more often than the reader's minute clock.
    void idleTimers()
    {
        auto paths = testPaths();
        // Ten o'clock exactly: the clock is armed for a minute and stays out of the window.
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [](ReaderHostServices &services) {
            services.localDateTime = [] { return testLocalTime(10 * 3600000); };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool presented = false;
        host.window()->preparePresentation([&](bool ready) { presented = ready; });
        QVERIFY(presented);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy());
        // The turn's follow-up work ends on its own: neighbour pages, the
        // opening record and the short delays after a turn.
        QTRY_VERIFY2_WITH_TIMEOUT(
                activeReaderTimers(host.window()).isEmpty(),
                qPrintable("Active reader timers: " + activeReaderTimers(host.window()).join(", ")),
                10000);
        // Settings is built once, after the book's background work settles
        // and the screen has been left alone.
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrGeneralSettings"), 15000);

        // No timer of any main-thread object fires for a few idle seconds:
        // this also covers unparented timers and single-shot calls.
        struct TimerEvents final : QObject
        {
            QStringList seen;

            bool eventFilter(QObject *object, QEvent *event) override
            {
                if (event->type() == QEvent::Timer) {
                    auto name = QString::fromLatin1(object->metaObject()->className());
                    if (!object->objectName().isEmpty())
                        name += ' ' + object->objectName();
                    if (auto *timer = qobject_cast<QTimer *>(object))
                        name += QString(" (%1 ms)").arg(timer->interval());
                    seen.append(name);
                }
                return false;
            }
        } events;

        qApp->installEventFilter(&events);
        QTest::qWait(3000);
        qApp->removeEventFilter(&events);
        events.seen.removeDuplicates();
        QVERIFY2(events.seen.isEmpty(),
                 qPrintable("Timer events while idle: " + events.seen.join(", ")));
    }

    // An accepted highlight must finish saving with the sleep pause still requested.
    void sleepFinishesHighlightSave_data()
    {
        QTest::addColumn<int>("stage");
        QTest::newRow("selection-only") << 0;
        QTest::newRow("prepare-range") << 1;
        QTest::newRow("save-text") << 2;
    }

    void sleepFinishesHighlightSave()
    {
        QFETCH(int, stage);
        const auto paths = testPaths();
        bool suspended = false;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        };
        CloseHost cleanup{ host };
        const auto release = std::make_shared<QSemaphore>();
        const auto entered = std::make_shared<QSemaphore>();
        const auto resume = qScopeGuard([&] {
            release->release(2);
            engineActivity().pause(false);
        });
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVector<SearchHit> hits;
        bool done = false;
        host.session()->search("reader", [&](SearchBatch batch) {
            hits += batch.hits;
            done = batch.done;
        });
        QTRY_VERIFY(done && !hits.isEmpty());
        QVector<QRect> rects;
        done = false;
        host.session()->rangeRects(hits.first().start, hits.first().end, [&](QVector<QRect> value) {
            rects = value;
            done = true;
        });
        QTRY_VERIFY(done && !rects.isEmpty());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        page->annotationRequested();
        page->held(rects.first().center());
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        page->tapped(rects.first().center() + QPoint(0, page->topHeight()));
        QPushButton *save = nullptr;
        QTRY_VERIFY((save = host.window()->findChild<QPushButton *>("rrAnnotationSave"))
                    && save->isVisible());
        QTRY_VERIFY(!host.window()->pendingStorageSave());
        auto barrier = [=] {
            qt::invoke(
                    engineWorker(),
                    [=] {
                        entered->release();
                        release->acquire();
                    },
                    Qt::QueuedConnection);
        };
        barrier();
        QTRY_VERIFY(entered->available() == 1);
        if (stage) {
            save->click();
            QVERIFY(host.window()->pendingStorageSave());
        }
        if (stage == 2) {
            // The range reply queues the text query behind this second barrier.
            barrier();
            release->release();
            QTRY_VERIFY(entered->available() == 2);
        }
        engineActivity().pause(true);
        suspended = true;
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        release->release();
        QTRY_VERIFY(!host.window()->pendingStorageSave());
        QTRY_VERIFY(engineActivity().quiescent());
        QVERIFY(!host.window()->pendingNetworkSync());
        if (stage)
            QVERIFY(storage::profileHasUnsyncedWrites());
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QVERIFY(!storage::profileHasUnsyncedWrites());
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        HighlightStore store(paths.annotations(), identity, host.session()->sourceHash(), "test",
                             paths.database());
        QVERIFY(store.load());
        QCOMPARE(store.highlights().size(), stage ? 1 : 0);
        if (stage)
            QVERIFY(store.highlights().first().text.contains("reader"));
        // With the save finished, subsequent engine work must remain parked.
        const auto replied = std::make_shared<bool>(false);
        host.session()->rangeRects(hits.first().start, hits.first().end,
                                   [replied](QVector<QRect>) { *replied = true; });
        QTest::qWait(50);
        QVERIFY(!*replied);
        engineActivity().pause(false);
        suspended = false;
        host.window()->wakeReader();
        QTRY_VERIFY(*replied);
        host.shutdown();
        QTRY_VERIFY(host.idle());
        QVERIFY(store.load());
        QCOMPARE(store.highlights().size(), stage ? 1 : 0);
    }

    void readerStandbySetting_data()
    {
        QTest::addColumn<QJsonValue>("saved");
        QTest::addColumn<bool>("supported");
        QTest::addColumn<bool>("disabled");
        QTest::newRow("default") << QJsonValue() << true << false;
        QTest::newRow("enabled") << QJsonValue(false) << true << false;
        QTest::newRow("disabled") << QJsonValue(true) << true << true;
        QTest::newRow("invalid") << QJsonValue("true") << true << false;
        QTest::newRow("unsupported") << QJsonValue(true) << false << false;
    }

    void readerStandbySetting()
    {
        QFETCH(QJsonValue, saved);
        QFETCH(bool, supported);
        QFETCH(bool, disabled);
        auto paths = testPaths();
        QJsonObject preferences{ { "schema", 1 } };
        if (!saved.isNull())
            preferences["disable_reader_standby"] = saved;
        QVERIFY(storage::writeJson(paths.preferences(), preferences));
        bool suspended = false;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.readerStandbySupported = supported;
            services.suspended = [&] { return suspended; };
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        QTRY_VERIFY(!host.book()->savesPending());
        QTRY_COMPARE(host.window()->flushBeforeStandby(), !disabled);
        // Explicit sleep must still reach the native suspend path.
        suspended = true;
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        suspended = false;
        QTRY_COMPARE(host.window()->flushBeforeStandby(), !disabled);
        host.window()->hide();
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        host.window()->show();
        QTRY_COMPARE(host.window()->flushBeforeStandby(), !disabled);
        host.shutdown();
        QTRY_VERIFY(host.idle());
    }

    // Checks when opening a book asks the host to turn Wi-Fi off: each account and schedule, the saver setting and Bluetooth.
    void wifiPowerSaver_data()
    {
        QTest::addColumn<QString>("account");
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("bluetooth");
        QTest::addColumn<bool>("expected");
        // Until beta 7 a saved setting is removed, so the saver never applies.
        QTest::newRow("no-account") << "none" << true << false << false;
        QTest::newRow("manual-sync") << "manual" << true << false << false;
        QTest::newRow("automatic") << "default" << true << false << false;
        QTest::newRow("automatic-never-while-reading") << "never" << true << false << false;
        QTest::newRow("automatic-turn-on-wifi") << "wake" << true << false << false;
        QTest::newRow("setting-default") << "none" << false << false << false;
        QTest::newRow("bluetooth-on") << "none" << true << true << false;
    }

    // The saver asks the host to turn Wi-Fi off only when automatic KOSync is
    // off or not set up, whatever the schedule, and the book is presented only
    // after the host answers.
    void wifiPowerSaver()
    {
        QFETCH(QString, account);
        QFETCH(bool, enabled);
        QFETCH(bool, bluetooth);
        QFETCH(bool, expected);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        auto paths = testPaths();
        // Until beta 7 Kobalt removes a saved setting and the saver stays off,
        // so the fixture writes the file directly.
        if (enabled)
            QVERIFY(storage::writeJson(paths.preferences(),
                                       { { "schema", 1 }, { "turn_off_wifi_on_open", true } }));
        if (account != "none") {
            QJsonObject saved{ { "schema", 1 },
                               { "server",
                                 QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                               { "username", "test-reader" },
                               { "key", QString(32, 'a') },
                               { "device_id", "test-device" },
                               { "automatic", account != "manual" } };
            if (account != "default" && account != "manual") {
                for (const auto *key : { "sync_on_open", "sync_on_sleep_wake", "sync_on_close" })
                    saved[key] = false;
                saved["sync_interval_minutes"] =
                        account == "never" ? kosync::neverWhileReading : 15;
                saved["wake_wifi"] = account == "wake";
            } else
                saved["sync_on_open"] = false;
            QVERIFY(storage::writeJson(paths.syncAccount(), saved));
        }
        auto network = std::make_shared<SyncNetworkState>();
        int requests = 0;
        QByteArray reason;
        std::function<void(bool)> answer;
        QPointer<QObject> holdOwner;
        std::function<bool()> holding;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.observeBluetooth = [bluetooth](QObject *, std::function<void(bool)> changed) {
                if (bluetooth)
                    changed(true);
            };
            services.turnOffWifi = [&](QObject *, const char *why, std::function<void(bool)> done) {
                ++requests;
                reason = why;
                answer = std::move(done);
            };
            services.holdWifiOff = [&](QObject *owner, std::function<bool()> decision) {
                holdOwner = owner;
                holding = std::move(decision);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        // The same rules keep the host's own background requests from turning
        // Wi-Fi back on while the book is open, for example after a wake.
        QCOMPARE(holdOwner.data(), static_cast<QObject *>(host.window()));
        QVERIFY(holding);
        QCOMPARE(holding(), expected);
        bool presented = false, ready = false;
        host.window()->preparePresentation([&](bool value) {
            presented = true;
            ready = value;
        });
        QCOMPARE(requests, expected ? 1 : 0);
        if (expected) {
            QVERIFY(!presented);
            QCOMPARE(reason, QByteArray("wifi-off before reader input"));
            answer(true);
        }
        QVERIFY(presented && ready);
        QCOMPARE(network->requests, 0);
    }

    // A periodic sync may turn on Wi-Fi only when the schedule allows it. Three
    // failed background connections stop further tries until Sync now. Every
    // sync that asked for Wi-Fi silently releases it when it ends, whether it
    // connected or not; Sync now does not.
    void kosyncBackgroundWifi()
    {
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username", "test-reader" },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", true },
                  { "sync_interval_minutes", 5 },
                  { "wake_wifi", true } }));
        auto network = std::make_shared<SyncNetworkState>();
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool prepared = false;
        host.window()->preparePresentation([&](bool ready) { prepared = ready; });
        QTRY_VERIFY_WITH_TIMEOUT(prepared, 1500);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QPointer<QWidget> popup;
        QTRY_VERIFY((popup = host.window()->findChild<QWidget *>("rrKosyncOpening"))
                    && popup->isVisible());
        QVERIFY(click(canvas, popup->findChild<QPushButton *>("rrKosyncCancel")));
        QCOMPARE(network->requests, 1);
        QVERIFY(buildKosyncPage(host));
        QCOMPARE(network->requests, 1);
        // A page turn syncs once five minutes have passed since the last sync.
        int direction = 1;
        const auto readOn = [&](double minutes) {
            clock += qint64(minutes * 60000);
            QVERIFY(host.session()->turn(direction));
            direction = -direction;
            QTRY_VERIFY(!host.session()->busy());
            QCoreApplication::processEvents();
        };
        readOn(4);
        QCOMPARE(network->requests, 1);
        const auto fail = [&] {
            QVERIFY(bool(network->completed));
            auto completed = std::move(network->completed);
            network->completed = {};
            completed(false);
        };
        // A connection that never answers ends at the sync deadline and
        // counts as a failure, like one that reports failure.
        readOn(5);
        QCOMPARE(network->requests, 2);
        QVERIFY(!network->interactive);
        QVERIFY(runKosyncDeadline(host.window()));
        QTRY_VERIFY(!network->completed);
        for (int attempt = 2; attempt <= kosync::backgroundConnectionLimit; ++attempt) {
            readOn(5);
            QCOMPARE(network->requests, 1 + attempt);
            QVERIFY(!network->interactive);
            fail();
        }
        readOn(5);
        QCOMPARE(network->requests, 1 + kosync::backgroundConnectionLimit);
        // The skipped opening sync and each failed periodic one.
        QCOMPARE(network->releases, 1 + kosync::backgroundConnectionLimit);
        // Sync now resets the count and the next periodic sync tries again.
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        QCOMPARE(network->requests, 2 + kosync::backgroundConnectionLimit);
        QVERIFY(network->interactive);
        fail();
        QCOMPARE(network->releases, 1 + kosync::backgroundConnectionLimit);
        readOn(5);
        QCOMPARE(network->requests, 3 + kosync::backgroundConnectionLimit);
        QVERIFY(!network->interactive);
        network->connected = true;
        auto completed = std::move(network->completed);
        network->completed = {};
        completed(true);
        // The stalled server ends the exchange at its deadline. Run it once
        // the request has reached the server.
        QTRY_VERIFY(attempts.count() > 0);
        QCOMPARE(network->releases, 1 + kosync::backgroundConnectionLimit);
        QVERIFY(runKosyncDeadline(host.window()));
        QTRY_COMPARE(network->releases, 2 + kosync::backgroundConnectionLimit);
        network->connected = false;
        // Without permission to turn on Wi-Fi, a periodic sync stays local.
        host.window()->findChild<QPushButton *>("rrKosyncAutomatic")->click();
        auto *sheet = host.window()->findChild<QWidget *>("rrKosyncAutomaticPopup");
        QVERIFY(sheet);
        sheet->findChild<QPushButton *>("rrKosyncWifiWait")->click();
        sheet->findChild<QPushButton *>("rrKosyncAutomaticClose")->click();
        QCOMPARE(kosync::Client(paths.syncAccount()).schedule().wakeWifi, false);
        const auto requests = network->requests;
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        status->setText("unchanged");
        readOn(5);
        QCOMPARE(network->requests, requests);
        QCOMPARE(status->text(), strings::get("kosync.no_connection"));
        // Time alone does not sync: a refresh long after the interval, without
        // a new position, does nothing.
        status->setText("unchanged");
        clock += 61 * 60000;
        host.window()->refreshHostPresentation();
        QCoreApplication::processEvents();
        QCOMPARE(status->text(), QString("unchanged"));
        // Every page: a turn syncs only when 30 seconds have passed since the last sync.
        host.window()->findChild<QPushButton *>("rrKosyncAutomatic")->click();
        sheet = host.window()->findChild<QWidget *>("rrKosyncAutomaticPopup");
        QVERIFY(sheet);
        sheet->findChild<QPushButton *>("rrKosyncInterval")->click();
        auto *intervals = host.window()->findChild<QWidget *>("rrKosyncIntervalPopup");
        QVERIFY(intervals);
        intervals->findChild<QAbstractButton *>("rrKosyncInterval_0")->click();
        sheet->findChild<QPushButton *>("rrKosyncAutomaticClose")->click();
        QCOMPARE(kosync::Client(paths.syncAccount()).schedule().minutes, 0);
        readOn(1);
        QCOMPARE(status->text(), strings::get("kosync.no_connection"));
        status->setText("unchanged");
        readOn(0.4);
        QCOMPARE(status->text(), QString("unchanged"));
        readOn(0.2);
        QCOMPARE(status->text(), strings::get("kosync.no_connection"));
        QCOMPARE(network->requests, requests);
    }

    // Checks that opening, wake and periodic syncs release their Wi-Fi request however they end, and that Sync now does not.
    void kosyncWifiRelease_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : { "opening-skip", "opening-failure", "wake-end", "wake-skip",
                                      "wake-close", "periodic-close" })
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    // An opening, wake or periodic sync asks for Wi-Fi silently. When it ends,
    // whether it succeeded, failed, reached its deadline, was skipped or was
    // cancelled by close, it releases the Wi-Fi it asked for. The network
    // controller turns Wi-Fi off only when that request turned it on. Sync now
    // leaves Wi-Fi as it is.
    void kosyncWifiRelease()
    {
        QFETCH(QString, scenario);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        const bool opening = scenario.startsWith("opening");
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username", "test-reader" },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", true },
                  { "sync_interval_minutes", 5 },
                  { "wake_wifi", true },
                  { "sync_on_open", opening },
                  { "sync_on_sleep_wake", true },
                  { "sync_on_close", false } }));
        auto network = std::make_shared<SyncNetworkState>();
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        const auto complete = [&](bool connected) {
            QVERIFY(bool(network->completed));
            auto completed = std::move(network->completed);
            network->completed = {};
            network->connected = connected;
            completed(connected);
        };
        bool prepared = false;
        host.window()->preparePresentation([&](bool ready) { prepared = ready; });
        QCOMPARE(network->requests, opening ? 1 : 0);
        QVERIFY(!network->interactive);
        // A failed connection ends the opening sync under the loading screen.
        if (scenario == "opening-failure") {
            complete(false);
            QCOMPARE(network->releases, 1);
        }
        QTRY_VERIFY_WITH_TIMEOUT(prepared, 1500);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const auto skip = [&] {
            QPointer<QWidget> popup;
            QTRY_VERIFY((popup = host.window()->findChild<QWidget *>("rrKosyncOpening"))
                        && popup->isVisible());
            QVERIFY(click(canvas, popup->findChild<QPushButton *>("rrKosyncCancel")));
            QTRY_VERIFY(!popup || !popup->isVisible());
        };
        const auto close = [&] {
            host.shutdown();
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        };
        if (scenario == "opening-failure") {
            QCOMPARE(network->releases, 1);
        } else if (scenario == "opening-skip") {
            skip();
            QCOMPARE(network->releases, 1);
            // Sync now leaves Wi-Fi as it is, whether it connects or not.
            QVERIFY(buildKosyncPage(host));
            auto *sync = host.window()->findChild<QPushButton *>("rrKosyncNow");
            QVERIFY(sync);
            sync->click();
            QCOMPARE(network->requests, 2);
            QVERIFY(network->interactive);
            complete(true);
            skip();
            sync->click();
            QCOMPARE(network->requests, 3);
            QVERIFY(network->interactive);
            complete(false);
            QCOMPARE(network->releases, 1);
        } else if (scenario.startsWith("wake")) {
            host.window()->sleepReader();
            host.window()->wakeReader();
            QTRY_COMPARE(network->requests, 1);
            QVERIFY(!network->interactive);
            QCOMPARE(network->releases, 0);
            if (scenario == "wake-end") {
                // The stalled server ends the exchange at the sync deadline.
                // Run it once the request has reached the server.
                complete(true);
                QTRY_VERIFY(attempts.count() > 0);
                QCOMPARE(network->releases, 0);
                QVERIFY(runKosyncDeadline(host.window()));
                QTRY_COMPARE(network->releases, 1);
            } else if (scenario == "wake-skip") {
                skip();
                QCOMPARE(network->releases, 1);
            } else {
                close();
                QCOMPARE(network->releases, 1);
            }
        } else {
            // The skipped opening sync starts the interval once the first page is shown.
            QTRY_VERIFY(!host.session()->busy());
            QCoreApplication::processEvents();
            clock += 6 * 60000;
            QVERIFY(host.session()->turn(1));
            QTRY_VERIFY(!host.session()->busy());
            QCoreApplication::processEvents();
            QCOMPARE(network->requests, 1);
            QVERIFY(!network->interactive);
            QCOMPARE(network->releases, 0);
            close();
            QCOMPARE(network->releases, 1);
        }
    }

    // Checks that the Wi-Fi power saver turns Wi-Fi off only after the sleep sync, and that wake drops the request.
    void wifiPowerSaverAtSleep_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("saver-on") << true;
        QTest::newRow("saver-default") << false;
    }

    // At sleep the power saver asks the host to turn Wi-Fi off as soon as the
    // sleep sync has ended, also with automatic KOSync. KOSync itself leaves
    // a connection it did not open alone. Wake drops an unanswered request.
    void wifiPowerSaverAtSleep()
    {
        QFETCH(bool, enabled);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        auto paths = testPaths();
        // Until beta 7 Kobalt removes a saved setting and the saver stays off,
        // so the fixture writes the file directly.
        if (enabled)
            QVERIFY(storage::writeJson(paths.preferences(),
                                       { { "schema", 1 }, { "turn_off_wifi_on_open", true } }));
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username", "test-reader" },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", true },
                  { "wake_wifi", true },
                  { "sync_on_open", false },
                  { "sync_on_sleep_wake", true },
                  { "sync_on_close", false } }));
        // Wi-Fi is already on, so the sleep sync can use it.
        auto network = std::make_shared<SyncNetworkState>();
        network->connected = true;
        int requests = 0;
        QByteArray reason;
        QPointer<QObject> context;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.turnOffWifi = [&](QObject *request, const char *why,
                                       std::function<void(bool)>) {
                ++requests;
                reason = why;
                context = request;
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool presented = false;
        host.window()->preparePresentation([&](bool ready) { presented = ready; });
        QVERIFY(presented);
        // Automatic KOSync keeps Wi-Fi on while the book is open.
        QCOMPARE(requests, 0);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(buildKosyncPage(host));
        QCOMPARE(requests, 0);
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        status->setText("unchanged");
        host.window()->sleepReader();
        // The sleep sync sends the position first; the stalled server ends it
        // at its budget.
        QVERIFY(host.window()->pendingNetworkSync());
        QCOMPARE(requests, 0);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.timeout_error"), 3000);
        QCOMPARE(requests, 0);
        QCOMPARE(network->requests, 0);
        QCOMPARE(network->releases, 0);
        host.window()->wakeReader();
        QVERIFY(!context);
    }

    // Checks the final sync at close and sleep: local position first, a bounded wait and wake during it; `https-*` rows need a real server.
    void kosyncFinal_data()
    {
        QTest::addColumn<QString>("transition");
        QTest::addColumn<bool>("inFlight");
        QTest::newRow("close") << "close" << false;
        QTest::newRow("sleep") << "sleep" << false;
        QTest::newRow("close-during-opening") << "close" << true;
        QTest::newRow("sleep-during-opening") << "sleep" << true;
        QTest::newRow("wake-during-final") << "wake" << false;
        QTest::newRow("busy-event-loop") << "blocked" << false;
        if (!qEnvironmentVariableIsEmpty("KOBALT_KOSYNC_TEST_SERVER")) {
            QTest::newRow("https-close") << "https-close" << false;
            QTest::newRow("https-sleep") << "https-sleep" << false;
            QTest::newRow("https-filename-close") << "https-filename-close" << false;
            QTest::newRow("https-filename-sleep") << "https-filename-sleep" << false;
            QTest::newRow("https-wake-recovery") << "https-wake-recovery" << false;
            for (const auto *state : { "handoff", "busy", "cancel", "deadline", "sleep" }) {
                const auto name = QByteArray("https-manual-") + state;
                QTest::newRow(name.constData()) << QString::fromLatin1(name) << false;
            }
            QTest::newRow("https-switch-close") << "https-switch-close" << false;
            for (const auto *decision : { "keep", "resume", "dismiss", "invalid" }) {
                const auto name = QByteArray("https-choice-") + decision;
                QTest::newRow(name.constData()) << QString::fromLatin1(name) << false;
            }
        }
    }

    void kosyncFinal()
    {
        QFETCH(QString, transition);
        QFETCH(bool, inFlight);
        const bool live = transition.startsWith("https-");
        if (live)
            transition.remove(0, 6);
        if (live && transition == "wake-recovery") {
            verifyKosyncWakeRecovery();
            return;
        }
        if (live && transition.startsWith("manual-")) {
            verifyKosyncManualHandoff(transition.mid(7));
            return;
        }
        if (live && transition.startsWith("choice-")) {
            verifyKosyncChoice(transition.mid(7));
            return;
        }
        bool filename = transition.startsWith("filename-");
        const bool switchMatching = transition.startsWith("switch-");
        if (switchMatching)
            transition.remove(0, 7);
        if (filename)
            transition.remove(0, 9);
        QTcpServer stalled;
        if (!live)
            QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server",
                    live ? qEnvironmentVariable("KOBALT_KOSYNC_TEST_SERVER")
                         : QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username",
                    live ? qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER")
                         : QString("test-reader") },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", true },
                  { "match_method", filename ? "filename" : "binary" } }));
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&clock](ReaderHostServices &services) {
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        if (live) {
            bool prepared = false;
            host.window()->preparePresentation([&](bool ready) { prepared = ready; });
            QTRY_VERIFY_WITH_TIMEOUT(prepared, 1500);
            QVERIFY(!host.window()->findChild<QWidget *>("rrKosyncOpening"));
        }
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        if (live) {
            QVERIFY(buildKosyncPage(host));
            auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
            QVERIFY(status);
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 12000);
            QVERIFY(showsSyncTime(host.window()));
            // Time alone does not sync; a changed position after the interval does.
            status->setText("unchanged position");
            clock += 16 * 60000;
            host.window()->refreshHostPresentation();
            QCoreApplication::processEvents();
            QCOMPARE(status->text(), QString("unchanged position"));
            QVERIFY(host.session()->go(1));
            QTRY_VERIFY(!host.session()->busy());
            QTRY_VERIFY(status->text() != QString("unchanged position"));
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 12000);
            if (switchMatching) {
                auto *matching = host.window()->findChild<QPushButton *>("rrKosyncMatching");
                QVERIFY(matching);
                matching->click();
                auto *option = host.window()->findChild<QAbstractButton *>("rrKosyncFilename");
                QVERIFY(option);
                option->click();
                QCOMPARE(status->text(), strings::get("kosync.matching_changed"));
                QVERIFY(!kosync::Client(paths.syncAccount()).lastSync().isValid());
                QVERIFY(!showsSyncTime(host.window()));
                auto *sync = host.window()->findChild<QPushButton *>("rrKosyncNow");
                QVERIFY(sync);
                sync->click();
                QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 12000);
                filename = true;
            }
        } else {
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrKosyncOpening"));
            QTRY_VERIFY(attempts.count() > 0);
        }
        if (!inFlight) {
            if (!live)
                QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrKosyncCancel")));
            QVERIFY(host.session()->go(1));
            QTRY_VERIFY(!host.session()->busy());
        }
        const auto document =
                filename ? host.session()->syncFilenameDigest() : host.session()->syncDigest();
        const auto position = host.session()->state().value("position").toString();
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        const int openingAttempts = attempts.count();
        QElapsedTimer elapsed;
        elapsed.start();
        QPointer<ReaderWindow> window = host.window();
        if (transition == "close")
            window->close();
        else
            window->sleepReader();
        if (!live)
            QTRY_VERIFY(window && window->pendingNetworkSync());
        if (!live && !inFlight)
            QTRY_VERIFY_WITH_TIMEOUT(attempts.count() > openingAttempts, 1500);
        QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(),
                 position);
        if (transition == "blocked") {
            // CPU suspension must not depend on a timer firing on the GUI thread.
            QTest::qSleep(2100);
            QVERIFY(!window->pendingNetworkSync());
            QCoreApplication::processEvents();
        }
        if (transition == "wake") {
            window->wakeReader();
            QVERIFY(!window->pendingNetworkSync());
            QTRY_VERIFY(window->findChild<QWidget *>("rrKosyncOpening"));
            QVERIFY(click(canvas, window->findChild<QPushButton *>("rrKosyncCancel")));
        } else {
            // The stalled server never answers, so the final sync ends at its
            // 2 s budget. Run that deadline instead of waiting for it. The
            // busy-event-loop row has already passed it, and a live server
            // answers.
            if (!live && transition != "blocked")
                QVERIFY(runKosyncDeadline(window, 2000));
            QTRY_VERIFY_WITH_TIMEOUT(!window || !window->pendingNetworkSync(), 3500);
            QVERIFY(elapsed.elapsed() < 3500);
            if (transition == "close")
                QTRY_VERIFY(host.idle());
            else if (!live) {
                QVERIFY(window);
                // Delete the notice that the sleep dismissed, so the check
                // after wake finds the new one.
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                window->wakeReader();
                QTRY_VERIFY(window->findChild<QWidget *>("rrKosyncOpening"));
                QVERIFY(click(canvas, window->findChild<QPushButton *>("rrKosyncCancel")));
            }
        }
        if (!live && !inFlight)
            QVERIFY(attempts.count() > openingAttempts);
        if (live) {
            // A second production client observes the final position on the server.
            kosync::Client verify(paths.syncAccount());
            // Closing and sleep write the sync time.
            QVERIFY(verify.lastSync().isValid());
            if (transition != "close")
                QVERIFY(showsSyncTime(window));
            bool received = false;
            QString failure;
            kosync::Progress remote;
            verify.pull(document, [&](kosync::Progress progress, QString error) {
                remote = progress;
                failure = error;
                received = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(received, 12000);
            QVERIFY2(failure.isEmpty(), qPrintable(failure));
            QCOMPARE(remote.position, position);
        }
        QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(),
                 position);
    }

    // Checks that revoking native reading fails its pending save once, ignores the late reply and still offers Retry for a failed local save.
    void revokedNativeDrainRetainsLocalSave()
    {
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.progress->writes, 0);
        QTRY_VERIFY(!storage::readBookState(paths, identity).object.isEmpty());
        const auto changeSql = [&](const QString &statement) {
            return storage::profileTransaction(paths.directory, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                return query.exec(statement);
            });
        };
        QVERIFY(changeSql("CREATE TRIGGER refuse_revoked_checkpoint BEFORE UPDATE ON book_state "
                          "BEGIN SELECT RAISE(ABORT,'test'); END"));

        struct RemoveFailure
        {
            std::function<void()> remove;

            ~RemoveFailure() { remove(); }
        } restore{ [&] { changeSql("DROP TRIGGER IF EXISTS refuse_revoked_checkpoint"); } };

        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        const auto position = host.session()->state().value("position").toString();
        host.progress->holdFinish = true;
        int nativeReplies = 0;
        bool nativeSaved = true;
        host.book()->finishReading([&](NativeActivityResult result) {
            ++nativeReplies;
            nativeSaved = activityCompleted(result);
            QCOMPARE(result, NativeActivityResult::Abandoned);
        });
        QVERIFY(host.progress->finishDone);
        const auto lateNativeReply = host.progress->finishDone;
        const int stops = host.progress->stopAttempts;
        host.window()->revokeNativeReading();
        host.window()->hide();
        QCOMPARE(host.progress->destroyed, 1);
        QCOMPARE(host.progress->stopAttempts, stops);
        QCOMPARE(nativeReplies, 1);
        QVERIFY(!nativeSaved);
        QVERIFY(!host.book()->hasNativeProgress());
        lateNativeReply(true);
        QCOMPARE(nativeReplies, 1);
        int localReplies = 0;
        bool localSaved = true;
        host.window()->finishStorageSave(
                [&](bool saved) {
                    ++localReplies;
                    localSaved = saved;
                },
                false);
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
        QCOMPARE(localReplies, 0);
        QCOMPARE(host.progress->finishAttempts, 1);
        QVERIFY(changeSql("DROP TRIGGER refuse_revoked_checkpoint"));
        host.window()->findChild<QPushButton *>("rrCloseSaveRetry")->click();
        QTRY_COMPARE(localReplies, 1);
        QVERIFY(localSaved);
        bool shutdownReady = false;
        host.window()->prepareShutdown([&] { shutdownReady = true; });
        QTRY_VERIFY_WITH_TIMEOUT(shutdownReady, 5000);
        QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(),
                 position);
        QCOMPARE(host.progress->finishAttempts, 1);
        QCOMPARE(host.progress->stopAttempts, stops);
    }

    // Checks that close waits for a held profile writer, keeps the reading page visible and closes once the writer is free.
    void closeWaitsForLocalSave()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QTRY_VERIFY(host.book()->readingActive());
        std::atomic_bool released{ false }, entered{ false };
        storage::RecordWriter blocker;

        struct Release
        {
            std::atomic_bool &released;

            ~Release() { released = true; }
        } release{ released };

        blocker.post(
                [&] {
                    entered = true;
                    while (!released)
                        QThread::msleep(1);
                    return true;
                },
                {});
        QTRY_VERIFY(entered.load());
        host.window()->close();
        const auto stopped = host.book()->readingMilliseconds();
        host.window()->close();
        // Hold the save past the former overlay delay while processing events.
        QElapsedTimer held;
        held.start();
        QTRY_VERIFY_WITH_TIMEOUT(held.elapsed() >= 1100, 2000);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSaving"));
        QVERIFY(host.window()->isVisible());
        QVERIFY(!host.window()->isEnabled());
        QCOMPARE(closed.count(), 0);
        QCOMPARE(host.book()->readingMilliseconds(), stopped);
        QVERIFY(!host.window()->flushBeforeStandby());
        released = true;
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
    }

    // Checks that a failed native position or reading-time save keeps the reader open with Retry, then Quit or Return after a failed retry. The reading time is finished only after the position is written.
    void closeRetriesRequiredNativeSaves_data()
    {
        QTest::addColumn<bool>("timeSave");
        QTest::addColumn<bool>("discard");
        QTest::addColumn<bool>("resume");
        QTest::newRow("position") << false << false << false;
        QTest::newRow("reading-time") << true << false << false;
        QTest::newRow("quit-position") << false << true << false;
        QTest::newRow("quit-reading-time") << true << true << false;
        QTest::newRow("resume-position") << false << true << true;
        QTest::newRow("resume-reading-time") << true << true << true;
    }

    void closeRetriesRequiredNativeSaves()
    {
        QFETCH(bool, timeSave);
        QFETCH(bool, discard);
        QFETCH(bool, resume);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.progress->writes, 0);
        host.progress->failWrite = !timeSave;
        host.progress->holdFinish = timeSave;
        host.window()->close();
        const auto stopped = host.book()->readingMilliseconds();
        if (timeSave) {
            QTRY_VERIFY(host.progress->finishDone);
            QCOMPARE(host.progress->writesAtFinish, 1);
            QVERIFY(!host.window()->findChild<QWidget *>("rrSaving"));
            QCOMPARE(closed.count(), 0);
            host.progress->completeFinish(false);
        }
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
        QCOMPARE(closed.count(), 0);
        // A failed position write does not go on to the reading time.
        QCOMPARE(host.progress->finishAttempts, timeSave ? 1 : 0);
        QCOMPARE(host.book()->readingMilliseconds(), stopped);
        auto *saving = host.window()->findChild<QWidget *>("rrSaving");
        QVERIFY(!saving || !saving->isVisible());
        QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
        host.progress->holdFinish = false;
        host.progress->failWrite = discard && !timeSave;
        host.progress->failFinish = discard && timeSave;
        auto *retry = host.window()->findChild<QPushButton *>("rrCloseSaveRetry");
        QVERIFY(click(canvas, retry));
        if (discard) {
            QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
            QCOMPARE(closed.count(), 0);
            QCOMPARE(host.progress->finishAttempts, timeSave ? 2 : 0);
            if (resume) {
                QVERIFY(click(canvas,
                              host.window()->findChild<QPushButton *>("rrCloseSaveReturn")));
                QTRY_VERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
                host.window()->close();
                QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
                QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
                QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveRetry")));
                QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
            }
            const auto attempts = host.progress->finishAttempts;
            const auto writes = host.progress->writeAttempts;
            const auto stops = host.progress->stopAttempts;
            const auto saved = host.progress->current;
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveQuit")));
            QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
            QCOMPARE(host.progress->finishAttempts, attempts);
            QCOMPARE(host.progress->writeAttempts, writes);
            QCOMPARE(host.progress->stopAttempts, stops);
            QCOMPARE(host.progress->current.location, saved.location);
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(host.progress->finishAttempts, timeSave ? 2 : 1);
        QCOMPARE(host.progress->writesAtFinish, host.progress->writes);
        QCOMPARE(host.progress->writeAttemptsAtFinish, host.progress->writeAttempts);
    }

    void quitAfterFailedLocalSaveRetry_data()
    {
        QTest::addColumn<bool>("localOnly");
        QTest::newRow("regular-exit") << false;
        QTest::newRow("exit-during-sync") << true;
    }

    void quitAfterFailedLocalSaveRetry()
    {
        QFETCH(bool, localOnly);
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        if (localOnly)
            host.configureServices = [](ReaderHostServices &services) {
                services.nickelSyncing = [] { return true; };
            };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        bool checkpointed = false;
        host.book()->checkpointForClose([&](bool saved) { checkpointed = saved; });
        QTRY_VERIFY(checkpointed);
        const auto saved = storage::readBookState(paths, identity).object;
        const auto changeSql = [&](const QString &statement) {
            return storage::profileTransaction(paths.directory, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                return query.exec(statement);
            });
        };
        QVERIFY(changeSql("CREATE TRIGGER refuse_quit_checkpoint BEFORE UPDATE ON book_state "
                          "BEGIN SELECT RAISE(ABORT,'test'); END"));

        struct RemoveFailure
        {
            std::function<void()> remove;

            ~RemoveFailure() { remove(); }
        } restore{ [&] { changeSql("DROP TRIGGER IF EXISTS refuse_quit_checkpoint"); } };

        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->state().value("position") != saved.value("position"));
        host.window()->close();
        if (localOnly)
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrExitLocalOnly")));
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
        QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveRetry")));
        if (localOnly) {
            // A failed local-only exit must keep the book open for another save.
            QTRY_VERIFY(
                    host.window()->findChild<QPushButton *>("rrCloseSaveReturn")
                    && host.window()->findChild<QPushButton *>("rrCloseSaveReturn")->isVisible());
            QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
            QCOMPARE(closed.count(), 0);
            QVERIFY(changeSql("DROP TRIGGER refuse_quit_checkpoint"));
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveRetry")));
            const auto position = host.session()->state().value("position");
            QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
            QCOMPARE(storage::readBookState(paths, identity).object.value("position"), position);
            return;
        }
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
        QCOMPARE(closed.count(), 0);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveQuit")));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(storage::readBookState(paths, identity).object.value("position"),
                 saved.value("position"));
    }

    void progressExitDuringSync_data()
    {
        QTest::addColumn<QString>("action");
        for (const auto *action : { "wait", "conflict", "cancel", "local" })
            QTest::newRow(action) << QString::fromLatin1(action);
    }

    void progressExitDuringSync()
    {
        QFETCH(QString, action);
        bool syncing = true;
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.configureServices = [&](ReaderHostServices &services) {
            services.nickelSyncing = [&] { return syncing; };
        };
        host.progress = std::make_shared<test::HostProgress>();
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        const auto position = host.session()->state().value("position");
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *exit = host.window()->findChild<QPushButton *>("rrReturn");
        QVERIFY(exit && exit->isVisible());
        QVERIFY(click(canvas, exit));
        auto *wait = host.window()->findChild<QPushButton *>("rrExitWaitForSync");
        QVERIFY(wait && wait->isVisible());
        QVERIFY(!exit->isVisible());
        QVERIFY(!host.window()->flushBeforeStandby());
        host.window()->close();
        QCOMPARE(host.window()->findChildren<QPushButton *>("rrExitWaitForSync").size(), 1);
        const int writes = host.progress->writes;
        if (action != "conflict") {
            QVERIFY(click(canvas, wait));
            QVERIFY(!wait->isEnabled());
            QCOMPARE(wait->text(), strings::get("closing.waiting_for_sync"));
        }
        if (action == "local") {
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrExitLocalOnly")));
        } else if (action == "cancel") {
            QVERIFY(clickAt(canvas, QPoint(12, 12)));
            syncing = false;
            host.window()->pageButton(1);
            QTRY_VERIFY(host.session()->state().value("position") != position);
            QCOMPARE(closed.count(), 0);
            return;
        } else {
            if (action == "conflict")
                host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true });
            syncing = false;
            if (action == "conflict") {
                QPushButton *here = nullptr;
                QTRY_VERIFY((here = host.window()->findChild<QPushButton *>("rrKeepPosition"))
                            && here->isVisible());
                QCOMPARE(host.progress->writes, writes);
                QVERIFY(click(canvas, here));
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(storage::readBookState(paths, identity).object.value("position"), position);
        QCOMPARE(host.progress->writes, writes + (action == "local" ? 0 : 1));
    }

    void slowCloseSave_data()
    {
        QTest::addColumn<bool>("keepReading");
        QTest::newRow("return-after-wake") << true;
        QTest::newRow("retry-while-writing") << false;
    }

    // A pending close save offers recovery without replaying a native reading interval.
    void slowCloseSave()
    {
        QFETCH(bool, keepReading);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.progress = std::make_shared<test::HostProgress>();
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        if (keepReading) {
            host.window()->sleepReader();
            host.window()->sleepViewReady();
            host.window()->wakeReader();
            QTRY_VERIFY(!host.session()->busy());
        }
        const auto attempts = host.progress->finishAttempts;
        host.progress->holdFinish = true;
        const auto release = qScopeGuard([&] {
            host.progress->holdFinish = false;
            if (host.progress->finishDone)
                host.progress->completeFinish(true);
        });
        host.window()->close();
        QTRY_VERIFY(host.progress->finishDone);
        QVERIFY(!host.window()->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QPushButton *>("rrCloseSaveReturn"),
                                 9500);
        QCOMPARE(closed.count(), 0);
        // Kobalt's own save is done, so only the host's records wait: the
        // notice offers to close without waiting for them.
        auto *closeNow = host.window()->findChild<QPushButton *>("rrCloseSaveQuit");
        QVERIFY(closeNow);
        QCOMPARE(closeNow->text(), strings::get("closing.close_now"));
        const int page = host.session()->page();
        const int positionWrites = host.progress->writes;
        auto *choice = host.window()->findChild<QPushButton *>(keepReading ? "rrCloseSaveReturn"
                                                                           : "rrCloseSaveRetry");
        QVERIFY(click(canvas, choice));
        if (keepReading) {
            host.window()->pageButton(1);
            QTRY_VERIFY(host.session()->page() != page);
            QTRY_VERIFY(!host.session()->busy());
        }
        host.progress->holdFinish = false;
        host.progress->completeFinish(true);
        QCOMPARE(host.progress->finishAttempts, attempts + 1);
        if (keepReading) {
            QVERIFY(host.window()->isEnabled());
            QCOMPARE(closed.count(), 0);
            host.window()->close();
        }
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(host.progress->writes, positionWrites + 1);
    }

    // Checks that a close that waits only for the host's reading time can end at once from the timeout notice. The host's position was written before the reading time, and Kobalt keeps the place it saved.
    void closeWithoutWaitingForHost()
    {
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.progress = std::make_shared<test::HostProgress>();
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy() && !host.book()->savesPending());
        const auto position = host.session()->state().value("position");
        const int writes = host.progress->writes;
        host.progress->holdFinish = true;
        const auto release = qScopeGuard([&] {
            host.progress->holdFinish = false;
            if (host.progress->finishDone)
                host.progress->completeFinish(true);
        });
        host.window()->close();
        QTRY_VERIFY(host.progress->finishDone);
        QCOMPARE(host.progress->writes, writes + 1);
        QCOMPARE(host.progress->writesAtFinish, writes + 1);
        const auto written = host.progress->current;
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QPushButton *>("rrCloseSaveQuit"), 9500);
        QCOMPARE(closed.count(), 0);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveQuit")));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(host.progress->writes, writes + 1);
        QVERIFY(host.progress->current == written);
        QCOMPARE(storage::readBookState(paths, identity).object.value("position"), position);
    }

    void sleepCancelsPendingExit_data()
    {
        QTest::addColumn<bool>("waitForCloseNotice");
        QTest::newRow("save-in-flight") << false;
        QTest::newRow("timeout-notice-open") << true;
    }

    void sleepCancelsPendingExit()
    {
        QFETCH(bool, waitForCloseNotice);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const int page = host.session()->page();

        host.progress->holdFinish = true;
        host.window()->close();
        QTRY_VERIFY(host.progress->finishDone);
        QCOMPARE(host.progress->writesAtFinish, host.progress->writes);
        QVERIFY(!host.window()->isEnabled());
        if (waitForCloseNotice)
            QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrCloseSaveError"), 9500);
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        host.window()->pageButton(1);
        QVERIFY(host.window()->isEnabled());
        auto *notice = host.window()->findChild<QWidget *>("rrCloseSaveError");
        QVERIFY(!notice || !notice->isVisible());
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == page + 1);
        QVERIFY(host.progress->finishDone);

        host.progress->holdFinish = false;
        host.progress->completeFinish(true);
        test::skipPageTurnWindow();
        host.window()->pageButton(1);
        QTRY_VERIFY(host.session() && !host.session()->busy()
                    && host.session()->page() == page + 2);
        QCOMPARE(closed.count(), 0);
        QCOMPARE(host.progress->finishAttempts, 1);
        host.window()->close();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
    }

    // An older local close can report its saved position after the reader has woken.
    void preSleepCloseAcknowledgedAfterWake()
    {
        bool closeSaved = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        host.progress = std::make_shared<test::HostProgress>();
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy() && !host.book()->savesPending());

        QSemaphore release;
        storage::RecordWriter blocker;
        blocker.post(
                [&] {
                    release.acquire();
                    return true;
                },
                {});
        const auto unblock = qScopeGuard([&] { release.release(); });
        host.book()->checkpointForClose([&](bool saved) { closeSaved = saved; });
        const int originalPage = host.session()->page();
        test::skipPageTurnWindow();
        host.window()->pageButton(1);
        QTRY_COMPARE(host.session()->page(), originalPage + 1);
        QTRY_VERIFY(!host.session()->busy());

        // Complete storage without dispatching its queued GUI acknowledgement.
        QSemaphore writerFinished;
        storage::RecordWriter marker;
        marker.post([] { return true; }, [&](bool) { writerFinished.release(); });
        release.release();
        QVERIFY(writerFinished.tryAcquire(1, 3000));
        QVERIFY(!closeSaved);
        host.progress->failFinish = true;
        const auto restore = qScopeGuard([&] { host.progress->failFinish = false; });
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QTRY_VERIFY(closeSaved);
        QVERIFY(host.progress->retryDone);
        host.progress->retryDone(false);
        QVERIFY(host.window()->findChild<QPushButton *>("rrSleepSaveRetry"));
    }

    void retryFailedSaveAfterWake_data()
    {
        QTest::addColumn<bool>("failAsleep");
        QTest::addColumn<bool>("autoSucceeds");
        QTest::addColumn<QString>("newSave");
        QTest::newRow("failure-after-page-turn") << false << false << QString();
        QTest::newRow("failure-before-wake") << true << false << QString();
        QTest::newRow("background-retry-succeeds") << true << true << QString();
        QTest::newRow("new-save-before-original-reply") << false << false << QString("original");
        QTest::newRow("new-save-before-retry-reply") << false << false << QString("retry");
    }

    void retryFailedSaveAfterWake()
    {
        QFETCH(bool, failAsleep);
        QFETCH(bool, autoSucceeds);
        QFETCH(QString, newSave);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const int initial = host.session()->page();
        host.progress->holdFinish = true;
        host.window()->close();
        QTRY_VERIFY(host.progress->finishDone);
        QCOMPARE(host.progress->writesAtFinish, host.progress->writes);
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        if (failAsleep) {
            host.progress->completeFinish(false);
            QCOMPARE(host.progress->saveRetries, 0);
        }
        host.window()->wakeReader();
        const auto advance = [&] {
            const int page = host.session()->page();
            test::skipPageTurnWindow();
            host.window()->pageButton(1);
            return QTest::qWaitFor(
                    [&] { return !host.session()->busy() && host.session()->page() == page + 1; },
                    1500);
        };
        const auto saveCurrentPage = [&] {
            const auto position = host.session()->state().value("position").toString();
            host.book()->saveReadingActivity();
            return QTest::qWaitFor([&] { return !host.book()->savesPending(); }, 3000)
                    && storage::readBookState(testPaths(), identity)
                               .object.value("position")
                               .toString()
                    == position;
        };
        if (!failAsleep) {
            QVERIFY(advance());
            if (newSave == "original")
                QVERIFY(saveCurrentPage());
            host.progress->completeFinish(false);
        }
        const auto retryButton = [&] {
            return host.window()->findChild<QPushButton *>("rrSleepSaveRetry");
        };
        if (newSave != "original") {
            QTRY_VERIFY_WITH_TIMEOUT(host.progress->retryDone, 800);
            QVERIFY(!retryButton());
            QVERIFY(advance());
            if (newSave == "retry")
                QVERIFY(saveCurrentPage());
            auto complete = std::move(host.progress->retryDone);
            complete(autoSucceeds);
        }
        const int page = host.session()->page();
        const auto position = host.session()->state().value("position").toString();
        QVERIFY(page > initial && !position.isEmpty());
        if (autoSucceeds || !newSave.isEmpty()) {
            QVERIFY(!retryButton());
            QCOMPARE(host.progress->saveRetries, newSave == "original" ? 0 : 1);
        } else {
            QPointer<QPushButton> retry;
            QTRY_VERIFY_WITH_TIMEOUT((retry = retryButton()) && retry->isVisible(), 1500);
            const int stops = host.progress->stopAttempts;
            test::skipPageTurnWindow();
            host.window()->pageButton(1);
            QVERIFY(!host.session()->busy());
            QCOMPARE(host.session()->page(), page);
            for (bool saved : { false, true }) {
                QVERIFY(click(canvas, retry));
                QVERIFY(host.progress->retryDone);
                QVERIFY(!retry->isEnabled());
                auto complete = std::move(host.progress->retryDone);
                complete(saved);
                if (!saved)
                    QTRY_VERIFY(retry && retry->isVisible() && retry->isEnabled());
            }
            QTRY_VERIFY(!retry || !retry->isVisible());
            QCOMPARE(host.progress->saveRetries, 3);
            QCOMPARE(host.progress->stopAttempts, stops);
        }
        QCOMPARE(host.session()->state().value("position").toString(), position);
        QCOMPARE(closed.count(), 0);
        QVERIFY(advance());
        const auto latest = host.session()->state().value("position").toString();
        host.progress->holdFinish = false;
        host.window()->close();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(storage::readBookState(testPaths(), identity).object.value("position").toString(),
                 latest);
    }

    // Checks that a whole-book build that is ready stops the layout clock.
    // Hybrid shows that layout from the next turn, which can come much
    // later; Book status shows how long the build took.
    void layoutTimeEndsWhenWholeBookIsReady()
    {
        renderingMode::setOverride(2);
        const int quiet = renderingMode::buildQuietMilliseconds();
        renderingMode::setBuildQuietMilliseconds(0);
        const auto restore = qScopeGuard([quiet] {
            renderingMode::setOverride(-1);
            renderingMode::setBuildQuietMilliseconds(quiet);
        });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(test::writeBook(directory.path(), "songs.epub", test::songbook(200)));
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(directory.filePath("songs.epub"), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy(), 30000);
        // Reading starts the build of the whole book. Turns keep the count
        // going, as in Book status.
        QElapsedTimer waited, sinceTurn;
        waited.start();
        sinceTurn.start();
        host.window()->pageButton(1);
        while (waited.elapsed() < 120000 && host.session()->layoutProgressPercent() < 100) {
            if (sinceTurn.elapsed() > 3000 && !host.session()->busy()) {
                host.window()->pageButton(1);
                sinceTurn.restart();
            }
            QTest::qWait(50);
        }
        QCOMPARE(host.session()->layoutProgressPercent(), 100);
        // Without a turn, the time no longer grows.
        const auto frozen = [&host] {
            const auto before = host.session()->layoutMilliseconds();
            QTest::qWait(20);
            return before >= 0 && host.session()->layoutMilliseconds() == before;
        };
        QTRY_VERIFY_WITH_TIMEOUT(frozen(), 10000);
    }

    // Checks that at close the native position is written before the native reading-time finish, and that the close waits for that finish.
    void closeWritesPositionBeforeNativeTime()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.progress->writes, 0);
        host.progress->holdFinish = true;
        const auto attempts = host.progress->writeAttempts;
        host.window()->close();
        QTRY_VERIFY(host.progress->finishDone);
        QCOMPARE(host.progress->writeAttemptsAtFinish, attempts + 1);
        QCOMPARE(host.progress->writesAtFinish, 1);
        QCOMPARE(host.progress->finishAttempts, 1);
        QCOMPARE(closed.count(), 0);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSaving"));
        QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
        host.progress->holdFinish = false;
        host.progress->completeFinish(true);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(host.progress->writeAttempts, attempts + 1);
        QCOMPARE(host.progress->finishAttempts, 1);
    }

    // Checks that at sleep the native position is written before the reading-time finish, which sleep does not wait for: immediate, held, failed and busy-database cases.
    void sleepWritesPositionBeforeNativeTime_data()
    {
        QTest::addColumn<bool>("held");
        QTest::addColumn<bool>("saved");
        QTest::addColumn<int>("contention");
        QTest::newRow("immediate") << false << true << 0;
        QTest::newRow("failed") << false << false << 0;
        QTest::newRow("held") << true << true << 0;
        QTest::newRow("held-failed") << true << false << 0;
        QTest::newRow("busy-before-write") << false << false << 150;
        QTest::newRow("persistent-busy") << false << false << 2800;
    }

    void sleepWritesPositionBeforeNativeTime()
    {
        QFETCH(bool, held);
        QFETCH(bool, saved);
        QFETCH(int, contention);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        const auto release = qScopeGuard([progress = host.progress] {
            progress->holdFinish = false;
            if (progress->finishDone)
                progress->completeFinish(true);
        });
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        const auto position = host.session()->state().value("position").toString();
        host.progress->holdFinish = held;
        host.progress->failFinish = !saved;
        if (contention) {
            host.progress->failWrite = true;
            host.progress->busyBeforeWrite = true;
            host.progress->writeError = "A different message for a typed busy result.";
            QTimer::singleShot(contention, host.window(),
                               [progress = host.progress] { progress->failWrite = false; });
        }
        QElapsedTimer elapsed;
        elapsed.start();
        host.window()->sleepReader();
        QCOMPARE(storage::readBookState(testPaths(), identity).object.value("position").toString(),
                 position);
        QCOMPARE(host.progress->finishAttempts, 1);
        // The finish began after every progress write attempt of the sleep.
        QCOMPARE(host.progress->writeAttemptsAtFinish, host.progress->writeAttempts);
        QCOMPARE(host.progress->writesAtFinish, host.progress->writes);
        if (contention) {
            QVERIFY(host.progress->writeAttempts > 1);
            QCOMPARE(host.progress->writes, contention < 2500 ? 1 : 0);
            QVERIFY(elapsed.elapsed() >= qMin(contention, 2500) - 100);
            QVERIFY(elapsed.elapsed() < 2800);
            QTRY_VERIFY(!host.progress->failWrite);
            return;
        }
        QCOMPARE(host.progress->writeAttempts, 1);
        QCOMPARE(host.progress->writes, 1);
        // Sleep does not wait for a held finish. Its result arrives later.
        QCOMPARE(host.progress->finishPending, held);
        if (held) {
            QVERIFY(elapsed.elapsed() < 2500);
            host.progress->holdFinish = false;
            host.progress->completeFinish(saved);
        }
        // A failed finish is retried once the reader wakes.
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        if (saved) {
            QTRY_VERIFY(!host.session()->busy());
            QCOMPARE(host.progress->saveRetries, 0);
        } else {
            QTRY_COMPARE(host.progress->saveRetries, 1);
        }
        QCOMPARE(host.progress->writes, 1);
    }

    // Checks that opening marks the book as reading in Nickel once, writes no position and does not retry after a failure.
    void nativeOpeningStatus_data()
    {
        QTest::addColumn<bool>("fails");
        QTest::newRow("saved") << false;
        QTest::newRow("failed") << true;
    }

    void nativeOpeningStatus()
    {
        QFETCH(bool, fails);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.progress->failWrite = fails;
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool prepared = false;
        host.window()->preparePresentation([&](bool ready) { prepared = ready; });
        QTRY_VERIFY(prepared);
        QCOMPARE(host.progress->openingAttempts, 1);
        QCOMPARE(host.progress->writeAttempts, 0);
        QCOMPARE(host.progress->current.modified, qint64(100));
        QCOMPARE(host.progress->current.location, QString("OEBPS/c1.xhtml"));
        host.progress->failWrite = false;
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        // A retry of the opening status would need an armed timer. Wait for
        // the follow-up work of the opening to end instead of for a time.
        QTRY_VERIFY2_WITH_TIMEOUT(
                activeReaderTimers(host.window()).isEmpty(),
                qPrintable("Active reader timers: " + activeReaderTimers(host.window()).join(", ")),
                10000);
        QCOMPARE(host.progress->openingAttempts, 1);
        QCOMPARE(host.progress->writeAttempts, 0);
        host.window()->preparePresentation([](bool) { });
        QTest::qWait(50);
        QCOMPARE(host.progress->openingAttempts, 1);
        QVERIFY(!host.window()->findChild<QTimer *>("rrNativeOpeningSaveTimer"));
    }

    void nativeProgressWaitsForSleep_data()
    {
        QTest::addColumn<QString>("outcome");
        for (const auto *outcome : { "saved", "newer", "failed" }) {
            QTest::newRow(outcome) << QString::fromLatin1(outcome);
        }
    }

    // A reading pause saves the local position. Sleep also writes Nickel's copy.
    void nativeProgressWaitsForSleep()
    {
        QFETCH(QString, outcome);
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.progress->writes, 0);
        const int writes = host.progress->writes;
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        const auto anchor = host.session()->state().value("position").toString();
        const auto stored = [&] {
            return storage::readBookState(paths, identity).object.value("position").toString();
        };
        QTRY_COMPARE_WITH_TIMEOUT(stored(), anchor, 5000);
        // The row can be visible before the reader receives the save acknowledgement.
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QCOMPARE(host.progress->writes, writes);
        QString error;
        const auto expected = host.session()->savedNativePosition(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!expected.isEmpty());
        // The fixture has no main-text boundary. Nickel gets progress through the book,
        // even when the renderer has page numbers for just the current chapter.
        QVERIFY(!host.session()->hasMainTextBoundary());
        const double percent = host.session()->progressPercent();
        if (outcome == "newer") {
            host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true });
        }
        host.progress->failWrite = outcome == "failed";
        // The old busy wording must not turn an unverified write into a retry.
        host.progress->writeError =
                strings::get("error.nickel_database_is_busy_local_position_was_retained");
        const auto before = host.progress->current;
        host.window()->sleepReader();
        QCOMPARE(host.progress->writes, writes + (outcome == "saved" ? 1 : 0));
        if (outcome == "failed")
            QCOMPARE(host.progress->writeAttempts, 1);
        if (outcome == "saved") {
            QCOMPARE(host.progress->current.location, expected);
            QCOMPARE(host.progress->percent, percent);
        } else {
            QVERIFY(host.progress->current == before);
        }
        QCOMPARE(stored(), anchor);
        if (outcome == "saved") {
            host.window()->wakeReader();
            QTRY_VERIFY(!host.session()->busy());
            QCOMPARE(host.session()->state().value("position").toString(), anchor);
            host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.8.1:7",
                                     host.progress->current.modified + 100, 1, true });
            host.window()->progressTick();
            QWidget *card = nullptr;
            QTRY_VERIFY((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                        && card->isVisible());
            auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
            QVERIFY(keep);
            QTest::mouseClick(keep, Qt::LeftButton);
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
            QTRY_VERIFY(!host.session()->busy());
            host.window()->sleepReader();
            QCOMPARE(host.progress->writes, writes + 2);
            QCOMPARE(host.progress->current.location, expected);
        }
    }

    // Checks that a failed appearance save stops after three attempts, page turns and background saves do not retry it, and a commit does.
    void appearanceRetryCooldown()
    {
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy() && !host.book()->savesPending());

        std::atomic<int> failures{ 0 }, attempts{ 0 };
        initializeDiagnostics(root.path() + "/diagnostics", false);
        pauseDiagnostics(false);
        setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event") == QStringLiteral("appearance-settings-unsaved")) {
                attempts = event.value("attempts").toInt();
                ++failures;
            }
        });

        struct ResetDiagnostics
        {
            ~ResetDiagnostics()
            {
                setDiagnosticObserver({});
                pauseDiagnostics(true);
            }
        } resetDiagnostics;

        const auto preferences = paths.preferences(), backup = preferences + ".retry-test";
        const auto original = storage::readPreferences(preferences).object;
        QVERIFY(QFile::rename(preferences, backup));

        struct RestorePreferences
        {
            QString preferences, backup;

            ~RestorePreferences()
            {
                QDir().rmdir(preferences);
                QFile::rename(backup, preferences);
            }
        } restore{ preferences, backup };

        QVERIFY(QDir().mkpath(preferences));
        QVERIFY(host.session()->setVerticalMargins(readingPixels(40)));
        QTRY_VERIFY(!host.session()->busy());
        host.book()->rememberAppearance();
        QTRY_VERIFY(!host.book()->savesPending());
        flushDiagnostics();
        QCOMPARE(failures.load(), 1);
        QCOMPARE(attempts.load(), 3);

        const auto turn = [&](int direction) {
            const auto before = host.session()->state().value("position");
            test::skipPageTurnWindow();
            host.window()->pageButton(direction);
            QTRY_VERIFY(!host.session()->busy()
                        && host.session()->state().value("position") != before);
            QTRY_VERIFY(!host.book()->savesPending());
        };
        for (int i = 0; i < 24; ++i)
            turn(i % 2 ? -1 : 1);
        // A new edit replaces the retained snapshot without resetting cooldown.
        QVERIFY(host.session()->setVerticalMargins(readingPixels(48)));
        QTRY_VERIFY(!host.session()->busy());
        host.book()->rememberAppearance();
        const auto expectedMargin = host.book()->checkpointState().value("top_margin_pixels");
        for (int i = 0; i < 3; ++i) {
            host.book()->saveReadingActivity();
            QTRY_VERIFY(!host.book()->savesPending());
        }
        // A pending appearance retry keeps savesPending() true, so none can
        // follow once it is false.
        QCoreApplication::processEvents();
        flushDiagnostics();
        QCOMPARE(failures.load(), 1);

        QVERIFY(QDir().rmdir(preferences));
        QVERIFY(QFile::rename(backup, preferences));
        // Background progress must not bypass the appearance retry policy.
        QJsonObject snapshot;
        QVERIFY(host.book()->checkpoint(&snapshot, true));
        QTRY_VERIFY(!host.book()->savesPending());
        for (int i = 0; i < 6; ++i)
            turn(i % 2 ? -1 : 1);
        host.book()->saveReadingActivity();
        QTRY_VERIFY(!host.book()->savesPending());
        QCOMPARE(storage::readPreferences(preferences).object, original);

        // An explicit commit can save during cooldown and clears its failure.
        host.window()->checkpoint();
        QCOMPARE(storage::readPreferences(preferences)
                         .object.value(appearance::pageMarginsKey)
                         .toObject()
                         .value("top_margin_pixels"),
                 expectedMargin);
        QVERIFY(host.session()->setVerticalMargins(readingPixels(56)));
        QTRY_VERIFY(!host.session()->busy());
        host.book()->rememberAppearance();
        QTRY_VERIFY(!host.book()->savesPending());
        QCOMPARE(storage::readPreferences(preferences)
                         .object.value(appearance::pageMarginsKey)
                         .toObject()
                         .value("top_margin_pixels"),
                 host.book()->checkpointState().value("top_margin_pixels"));
    }

    // Checks that settings that become unusable while a book is open do not fail the close and stay unchanged.
    void closeWithUnusableSettings()
    {
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy() && !host.book()->savesPending());
        // Settings that become unusable while a book is open skip the layout
        // memory. The reading position still saves, and the file is kept.
        const QByteArray future = "{\"schema\":2}";
        {
            QFile settings(paths.preferences());
            QVERIFY(settings.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    && settings.write(future) == future.size());
        }
        QVERIFY(host.session()->setVerticalMargins(readingPixels(40)));
        QTRY_VERIFY(!host.session()->busy());
        QJsonObject snapshot;
        QVERIFY(host.book()->checkpoint(&snapshot));
        bool done = false, saved = false;
        host.book()->checkpointForClose([&](bool result) {
            done = true;
            saved = result;
        });
        QTRY_VERIFY(done);
        QVERIFY(saved);
        QFile settings(paths.preferences());
        QVERIFY(settings.open(QIODevice::ReadOnly));
        QCOMPARE(settings.readAll(), future);
    }

    // Checks that the profile-reset notice shows in the open reader with the backup folder and closes on confirm.
    void recoveryNoticeAfterFirstPage()
    {
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        storage::ProfileRecovery recovery;
        recovery.settings = true;
        const auto startup = profileRecoveryNotice(recovery, ".adds/kobalt/backups");
        const auto message = startup.message;
        host.configureServices = [startup](ReaderHostServices &services) {
            services.startupNotice = startup;
        };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QWidget *notice = nullptr;
        QTRY_VERIFY((notice = host.window()->findChild<QWidget *>("rrRecoveryNotice"))
                    && notice->isVisible());
        auto *text = host.window()->findChild<QLabel *>("rrRecoveryNoticeMessage");
        QVERIFY(text && text->text() == message && message.contains(".adds/kobalt/backups"));
        auto *confirm = host.window()->findChild<QPushButton *>("rrRecoveryNoticeConfirm");
        QVERIFY(confirm);
        confirm->click();
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrRecoveryNotice"));
    }

    void benchmarkNoticeAfterRun()
    {
        const auto completed = qt::object({ { "id", "page.turn" }, { "status", "completed" } });
        const auto skipped = qt::object({ { "id", "panel.footnote" }, { "status", "skipped" } });
        const auto failed = qt::object({ { "id", "book.close" },
                                         { "status", "failed" },
                                         { "reason", "the reader did not close" } });
        const QString folder = ".adds/kobalt/diagnostics";
        const auto finished = benchmarkNotice({ completed, skipped }, true, folder);
        QCOMPARE(finished.title, strings::get("benchmark.finished"));
        QVERIFY(finished.message.contains(strings::get("benchmark.counts").arg(1).arg(1).arg(0))
                && finished.message.contains(folder));
        const auto aborted = benchmarkNotice({ completed, failed, skipped }, true, folder);
        QCOMPARE(aborted.title, strings::get("benchmark.failed"));
        QVERIFY(aborted.message.contains("book.close")
                && aborted.message.contains("the reader did not close")
                && aborted.message.contains(folder));
        QCOMPARE(benchmarkNotice({ completed }, false, folder).title,
                 strings::get("benchmark.failed"));
        QCOMPARE(benchmarkNotice({}, true, folder).title, strings::get("benchmark.failed"));

        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        // A hidden window cannot show it, so the host keeps it for later.
        QVERIFY(!host.window()->showNotice(finished));
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.window()->showNotice(aborted));
        auto *notice = host.window()->findChild<QWidget *>("rrBenchmarkNotice");
        QVERIFY(notice && notice->isVisible());
        auto *text = host.window()->findChild<QLabel *>("rrBenchmarkNoticeMessage");
        QVERIFY(text && text->text() == aborted.message);
        // One notice at a time.
        QVERIFY(!host.window()->showNotice(finished));
        auto *confirm = host.window()->findChild<QPushButton *>("rrBenchmarkNoticeConfirm");
        QVERIFY(confirm);
        confirm->click();
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrBenchmarkNotice"));
    }

    // Checks that activity saves wait for a batch without timers, save at once on low battery until charging, and cool down after a failure.
    void activityBatchesAndLowBattery_data()
    {
        QTest::addColumn<bool>("completeFailureFirst");
        QTest::newRow("immediate-standby") << false;
        QTest::newRow("completed-failure") << true;
    }

    void activityBatchesAndLowBattery()
    {
        QFETCH(bool, completeFailureFirst);
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        BatteryStatus battery{ 50, false, false };
        std::function<void()> batteryChanged;
        host.configureServices = [&](ReaderHostServices &services) {
            services.battery = [&] { return battery; };
            services.observeBattery = [&](QObject *, std::function<void()> changed) {
                batteryChanged = changed;
            };
        };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/cover.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy()
                    && host.session()->resourceStats().value("pending_images").toInt() == 0);
        const auto stored = [&] {
            return storage::readBookState(paths, identity).object.value("position").toString();
        };
        const auto current = [&] { return host.session()->state().value("position").toString(); };
        QTRY_COMPARE(stored(), current());
        const auto turn = [&] {
            const auto before = current();
            test::skipPageTurnWindow();
            host.window()->pageButton(1);
            QTRY_VERIFY(!host.session()->busy() && current() != before);
            QTRY_VERIFY(!host.book()->savesPending());
        };
        const auto initial = stored();
        turn();
        // A delayed save after the turn would need an armed timer. Wait for
        // the turn's follow-up work to end instead of for a time.
        QTRY_VERIFY2_WITH_TIMEOUT(
                activeReaderTimers(host.window()).isEmpty(),
                qPrintable("Active reader timers: " + activeReaderTimers(host.window()).join(", ")),
                10000);
        QCOMPARE(stored(), initial);
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QCOMPARE(stored(), initial);
        QVERIFY(!host.window()->findChild<QTimer *>("rrIdleSaveTimer"));
        QVERIFY(!host.window()->findChild<QTimer *>("rrReadingHistoryTimer"));
        QVERIFY(!host.book()->activitySaveDue());
        QVERIFY(host.book()->activitySaveDue(0));
        host.book()->saveReadingActivity();
        QTRY_VERIFY(!host.book()->savesPending());
        QCOMPARE(stored(), current());
        QVERIFY(storage::profileHasUnsyncedWrites());
        QCOMPARE(host.progress->writes, 0);

        battery.percent = 5;
        QVERIFY(batteryChanged);
        batteryChanged();
        turn();
        QTRY_VERIFY(!host.window()->flushBeforeStandby());
        QTRY_COMPARE(stored(), current());
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QVERIFY(!storage::profileHasUnsyncedWrites());

        int failures = 0;
        host.book()->onCheckpointFailed([&] { ++failures; });
        storage::closeProfile();
        QVERIFY(QFile::setPermissions(paths.database(), QFileDevice::ReadOwner));
        turn();
        // The engine can finish before the queued refresh marks the position
        // for saving. Keep asking until standby queues the save, or its failure
        // has already completed.
        QTRY_VERIFY(!host.window()->flushBeforeStandby() || failures == 1);
        if (completeFailureFirst) {
            QTRY_COMPARE(failures, 1);
        }
        // The failed batch can finish before Nickel asks about standby.
        // Cooldown permits standby once no write remains pending.
        if (host.window()->flushBeforeStandby()) {
            QCOMPARE(failures, 1);
            QVERIFY(!host.book()->savesPending());
            QVERIFY(host.book()->activitySaveCoolingDown());
        }
        QTRY_COMPARE(failures, 1);
        QVERIFY(host.book()->activitySaveCoolingDown());
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QVERIFY(QFile::setPermissions(paths.database(),
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        host.book()->saveReadingActivity();
        // A save and its retries keep savesPending() true until they end.
        QTRY_VERIFY(!host.book()->savesPending());
        QCOMPARE(failures, 1);
        QVERIFY(stored() != current());
        host.window()->checkpoint();
        QCOMPARE(stored(), current());
        QVERIFY(!host.book()->activitySaveCoolingDown());
        QVERIFY(!storage::profileHasUnsyncedWrites());
        battery.charging = true;
        batteryChanged();
        const auto saved = stored();
        turn();
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QCOMPARE(stored(), saved);
    }

    void annotationExportOwnerDestruction_data()
    {
        QTest::addColumn<bool>("destroyObserver");
        QTest::newRow("observer") << true;
        QTest::newRow("operation") << false;
    }

    void annotationExportOwnerDestruction()
    {
        QFETCH(bool, destroyObserver);

        struct Gate
        {
            std::atomic_bool entered{ false }, released{ false };
        };

        const auto gate = std::make_shared<Gate>();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto reader = std::make_unique<QWidget>();
        reader->resize(600, 900);
        auto exporter = reader::directoryAnnotationExporter(
                reader.get(), directory.path(), [gate](const QUrl &url, const QByteArray &bytes) {
                    gate->entered = true;
                    while (!gate->released)
                        QThread::msleep(1);
                    return platform::writeLocalExport(url, bytes);
                });

        QSemaphore profileSaved;
        storage::RecordWriter profileWriter;
        struct Release
        {
            std::shared_ptr<Gate> gate;

            ~Release() { gate->released = true; }
        } release{ gate };

        int replies = 0;
        exporter->start({ "book.zip", "Book", "HTML", [] { return QByteArray("archive"); },
                          [] { return true; }, [] { return true; } },
                        [&](AnnotationExportResult) { ++replies; });
        auto *confirm = reader->findChild<QPushButton *>("rrAnnotationExportConfirm");
        QVERIFY(confirm);
        confirm->click();
        QTRY_VERIFY(gate->entered.load());
        QVERIFY(profileWriter.tryPost([&] {
            profileSaved.release();
            return true;
        }));
        QVERIFY2(profileSaved.tryAcquire(1, 2000),
                 "A slow export destination held the profile writer");
        if (destroyObserver) {
            reader.reset();
            QVERIFY(exporter->writing());
            QCOMPARE(replies, 0);
            gate->released = true;
            QTRY_VERIFY(!exporter->writing());
        } else {
            gate->released = true;
            mainthread::AllowBlocking allow("destroying the export operation after closing input");
            exporter.reset();
        }
        QCoreApplication::processEvents();
        QCOMPARE(replies, 0);
        QFile file(directory.filePath("book.zip"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("archive"));
    }

    void annotationExportHandoff_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const auto *boundary : { "usb", "shutdown", "close", "revoke", "cancel", "failure" })
            QTest::newRow(boundary) << QString::fromLatin1(boundary);
    }

    void annotationExportHandoff()
    {
        QFETCH(QString, boundary);

        struct Gate
        {
            std::atomic_bool entered{ false }, released{ false };
        };

        const auto gate = std::make_shared<Gate>();
        auto paths = testPaths();
        const auto directory = paths.directory + "/exports";
        AnnotationExporter *exporter = nullptr;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.annotationExporter = [&](QWidget *reader) {
                auto operation = reader::directoryAnnotationExporter(
                        reader, directory,
                        [gate, fail = boundary == "failure"](const QUrl &url,
                                                             const QByteArray &bytes) {
                            gate->entered = true;
                            while (!gate->released)
                                QThread::msleep(1);
                            return !fail && platform::writeLocalExport(url, bytes);
                        });
                exporter = operation.get();
                return operation;
            };
        };
        CloseHost cleanup{ host };

        // Release the worker before any failed assertion closes the reader.
        struct Release
        {
            std::shared_ptr<Gate> gate;

            ~Release() { gate->released = true; }
        } release{ gate };

        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(exporter);
        int replies = 0;
        AnnotationExportStatus outcome = AnnotationExportStatus::Unsupported;
        exporter->start({ "book.zip", "Book", "HTML", [] { return QByteArray("archive"); },
                          [] { return true; }, [] { return true; } },
                        [&](AnnotationExportResult result) {
                            ++replies;
                            outcome = result.status;
                        });
        auto *confirm = host.window()->findChild<QPushButton *>("rrAnnotationExportConfirm");
        QVERIFY(confirm);
        confirm->click();
        QTRY_VERIFY(gate->entered.load());
        QVERIFY(exporter->writing());
        QVERIFY(host.window()->pendingStorageSave());
        QVERIFY(!host.window()->flushBeforeStandby());
        const int page = host.session()->page();
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->page() != page);
        QCOMPARE(replies, 0);
        int handoffs = 0;
        if (boundary == "usb")
            host.window()->prepareUsb([&] { ++handoffs; });
        else if (boundary == "shutdown")
            host.window()->prepareShutdown([&] { ++handoffs; });
        else if (boundary == "close")
            host.window()->close();
        else if (boundary == "revoke") {
            host.window()->revokeNativeReading();
            host.window()->finishStorageSave([&](bool) { ++handoffs; }, false);
        } else if (boundary == "cancel")
            exporter->cancel();
        QCoreApplication::processEvents();
        QCOMPARE(handoffs, 0);
        QCOMPARE(closed.count(), 0);
        gate->released = true;
        if (boundary == "close")
            QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        else {
            QTRY_VERIFY(!exporter->writing());
            if (boundary == "usb" || boundary == "shutdown" || boundary == "revoke")
                QTRY_COMPARE(handoffs, 1);
        }
        QCOMPARE(replies, boundary == "cancel" ? 0 : 1);
        if (boundary != "cancel")
            QCOMPARE(outcome,
                     boundary == "failure" ? AnnotationExportStatus::Failed
                                           : AnnotationExportStatus::Saved);
        QFile file(directory + "/book.zip");
        if (boundary == "failure")
            QVERIFY(!file.exists());
        else {
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("archive"));
        }
    }

    void sleepOpeningSaveFailure_data()
    {
        QTest::addColumn<QString>("handoff");
        for (const auto *value :
             { "ordinary-sleep", "recovered", "persistent", "sleep-during-retry", "required",
               "repaired-before-usb", "low-battery", "parked-low-battery" })
            QTest::newRow(value) << QString::fromLatin1(value);
    }

    void sleepOpeningSaveFailure()
    {
        QFETCH(QString, handoff);
        auto paths = testPaths();
        bool suspended = false, saved = true;
        int completions = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
            if (handoff == "parked-low-battery")
                services.battery = [] { return BatteryStatus{ 5, false, false }; };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto *reader = host.window();
        QTRY_VERIFY(!host.session()->busy());
        const auto identity = host.book()->identity();
        const auto position = [&] {
            return storage::readBookState(paths, identity).object.value("position").toString();
        };
        const auto before = host.session()->state().value("position").toString();
        QVERIFY(!before.isEmpty());
        bool checkpointed = false;
        host.book()->checkpointForClose([&](bool ok) { checkpointed = ok; });
        QTRY_VERIFY(checkpointed);
        QCOMPARE(position(), before);
        const auto sql = [&](const QString &statement) {
            return storage::profileTransaction(paths.directory, [&](QSqlDatabase &db) {
                return QSqlQuery(db).exec(statement);
            });
        };
        const auto repair = [&] { return sql("DROP TRIGGER IF EXISTS refuse_sleep_checkpoint"); };
        const auto restore = qScopeGuard([&] {
            repair();
            suspended = false;
        });
        QVERIFY(sql("CREATE TRIGGER refuse_sleep_checkpoint BEFORE UPDATE ON book_state "
                    "BEGIN SELECT RAISE(ABORT,'sleep checkpoint'); END"));
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        const auto after = host.session()->state().value("position").toString();
        QVERIFY(!after.isEmpty() && after != before);
        suspended = true;
        const auto retry = [&] { return reader->findChild<QPushButton *>("rrCloseSaveRetry"); };
        if (handoff == "ordinary-sleep") {
            reader->sleepReader();
            QTRY_VERIFY(reader->flushBeforeStandby() && !reader->pendingStorageSave());
            QVERIFY(!reader->property("reader_transition_save_failed").toBool());
            QVERIFY(!retry() || !retry()->isVisible());
            QCOMPARE(position(), before);
            return;
        }

        std::atomic_int failures{ 0 };
        initializeDiagnostics(paths.directory + "/diagnostics", false);
        pauseDiagnostics(false);
        setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event") == QLatin1String("local-close-save-failed"))
                ++failures;
        });
        const auto resetDiagnostics = qScopeGuard([] {
            setDiagnosticObserver({});
            pauseDiagnostics(true);
        });
        reader->hide(); // The opening view still owns the screen.
        reader->setOpeningSaveState(OpeningSaveState::Sleeping);
        reader->finishStorageSave([&](bool ok) {
            ++completions;
            saved = ok;
        });
        QVERIFY(reader->pendingStorageSave());
        QVERIFY(!reader->flushBeforeStandby());
        QTRY_VERIFY(reader->openingSaveParked());
        QVERIFY(reader->property("reader_transition_save_failed").toBool());
        QTRY_VERIFY(!reader->pendingStorageSave() && reader->flushBeforeStandby());
        QCOMPARE(failures.load(), 3);
        QCOMPARE(completions, 0);
        QCOMPARE(position(), before);
        if (handoff == "parked-low-battery") {
            // The fault stays. At 5% the parked reader must not turn each
            // standby poll into another write that refuses standby.
            const auto beforeStandby = nickel::readerStandbyCheck(reader);
            QTRY_VERIFY(beforeStandby());
            for (int poll = 0; poll < 5; ++poll) {
                QCoreApplication::processEvents();
                QVERIFY2(beforeStandby(), qPrintable(QString("standby poll %1").arg(poll)));
            }
            QCOMPARE(failures.load(), 3);
            QCOMPARE(position(), before);
            return;
        }
        if (handoff == "recovered") {
            // A later routine write can leave the profile WAL unsynced even
            // when the failed reader checkpoint has finished all its attempts.
            const QString other = "standby-wal";
            {
                storage::RoutineWrites routine;
                QVERIFY(storage::writeBookState(paths, other, { { "book", other } }));
            }
            QVERIFY(storage::profileCheckpointDue());
            QVERIFY(nickel::readerStandbyCheck(nullptr)());
            const auto beforeStandby = nickel::readerStandbyCheck(reader);
            {
                QSemaphore release;
                storage::RecordWriter blocker;
                const auto unblock = qScopeGuard([&] { release.release(); });
                // Keep the shared writer ahead of the checkpoint until both standby checks run.
                blocker.post(
                        [&] {
                            release.acquire();
                            return true;
                        },
                        {});
                QVERIFY(!beforeStandby());
                QVERIFY(storage::profileCheckpointPending());
                QVERIFY(!beforeStandby());
            }
            QTRY_VERIFY(beforeStandby());
            QVERIFY(!storage::profileHasUnsyncedWrites());
        }
        QVERIFY(!retry());
        QVERIFY(!reader->isVisible());
        reader->setOpeningSaveState(OpeningSaveState::Sleeping);
        const bool repaired = handoff == "recovered" || handoff == "repaired-before-usb";
        if (repaired)
            QVERIFY(repair());
        if (handoff == "required" || handoff == "repaired-before-usb") {
            reader->setOpeningSaveState(OpeningSaveState::Required);
            reader->setOpeningSaveState(OpeningSaveState::Required);
        } else if (handoff == "low-battery") {
            reader->setOpeningSaveState(OpeningSaveState::LowBatteryShutdown);
            QTRY_COMPARE(completions, 1);
            QVERIFY(!saved);
            QCOMPARE(position(), before);
            QVERIFY(!reader->pendingStorageSave());
            return;
        } else {
            suspended = false;
            reader->setOpeningSaveState(OpeningSaveState::Awake);
            reader->setOpeningSaveState(OpeningSaveState::Awake);
        }
        if (handoff == "sleep-during-retry") {
            QTRY_VERIFY(retry() && retry()->isVisible());
            {
                QSemaphore entered, release;
                storage::RecordWriter blocker;
                const auto unblock = qScopeGuard([&] { release.release(); });
                blocker.post(
                        [&] {
                            entered.release();
                            return release.tryAcquire(1, 5000);
                        },
                        {});
                QVERIFY(entered.tryAcquire(1, 5000));
                retry()->click();
                QVERIFY(reader->pendingStorageSave());
                suspended = true;
                reader->setOpeningSaveState(OpeningSaveState::Sleeping);
                QVERIFY(!reader->isVisible());
                QVERIFY(!reader->openingSaveParked());
                QCOMPARE(completions, 0);
            }
            QTRY_VERIFY(reader->openingSaveParked());
            QVERIFY(!reader->isVisible());
            QVERIFY(!retry() || !retry()->isVisible());
            QCOMPARE(position(), before);
            QCOMPARE(completions, 0);
            QVERIFY(repair());
            suspended = false;
            reader->setOpeningSaveState(OpeningSaveState::Awake);
        } else if (!repaired) {
            QTRY_VERIFY(retry() && retry()->isVisible());
            QCOMPARE(completions, 0);
            QCOMPARE(position(), before);
            QCOMPARE(failures.load(), 4); // Three at sleep and one on wake or required handoff.
            // This is a second sleep after wake, or USB cancellation while still asleep.
            suspended = true;
            reader->setOpeningSaveState(OpeningSaveState::Sleeping);
            QTRY_VERIFY(reader->openingSaveParked() && !reader->isVisible());
            QTRY_VERIFY(!retry() || !retry()->isVisible());
            suspended = false;
            reader->setOpeningSaveState(OpeningSaveState::Awake);
            QTRY_VERIFY(retry() && retry()->isVisible());
            QCOMPARE(failures.load(), 5);
            QVERIFY(!reader->close());
            QCoreApplication::processEvents();
            QVERIFY(host.window() == reader && reader->isVisible());
            QVERIFY(retry() && retry()->isVisible());
            QCOMPARE(completions, 0);
            QCOMPARE(position(), before);
            QVERIFY(repair());
            QTest::mouseClick(retry(), Qt::LeftButton);
        }
        QTRY_COMPARE(completions, 1);
        QVERIFY(saved);
        QCOMPARE(position(), after);
    }

    void shutdownFinalSave_data()
    {
        QTest::addColumn<QString>("outcome");
        for (const auto &outcome : { "saved", "failed", "persistent-failure", "persistent-retry",
                                     "battery-low", "battery-low-held", "newer", "timeout" }) {
            QTest::newRow(outcome) << QString::fromLatin1(outcome);
        }
    }

    void shutdownFinalSave()
    {
        QFETCH(QString, outcome);
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };

        // Ends before the cleanup closes the reader, so a failed row never
        // leaves the engine worker held.
        struct ResetDelay
        {
            ~ResetDelay()
            {
                delayShutdownEncoding = false;
                releaseShutdownEncoding = true;
            }
        } resetDelay;

        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        host.positions.encode = shutdownPosition;
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        const int writes = host.progress->writes;
        host.progress->failWrite = outcome == "failed" || outcome.startsWith("persistent-")
                || outcome == "battery-low";
        if (outcome == "newer") {
            host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true });
        }
        const auto before = host.progress->current;
        shutdownEncodingReturned = releaseShutdownEncoding = false;
        delayShutdownEncoding = outcome == "timeout";
        const int attempts = host.progress->writeAttempts;
        int completed = 0;
        QElapsedTimer elapsed;
        elapsed.start();
        host.progress->holdFinish = outcome == "battery-low-held";
        host.window()->prepareShutdown([&] { ++completed; }, outcome.startsWith("battery-low"));
        // The engine mapping has a 2 s deadline. Check the timer and run it
        // now instead of waiting for it.
        QPointer<QTimer> mappingDeadline = transitionMappingDeadline(host.window());
        QVERIFY(mappingDeadline);
        const auto runMappingDeadline = [&] {
            mappingDeadline->stop();
            QMetaObject::invokeMethod(mappingDeadline, "timeout", Qt::DirectConnection);
        };
        if (outcome == "battery-low-held") {
            QTRY_VERIFY(host.progress->finishDone);
            // The deadline bounds only the mapping, which has returned and
            // cancelled it, so nothing releases the held storage finish.
            QVERIFY(!mappingDeadline || !mappingDeadline->isActive());
            QCoreApplication::processEvents();
            QCOMPARE(completed, 0);
            // The final position was written before the held finish began.
            QCOMPARE(host.progress->writeAttempts, attempts + 1);
            QCOMPARE(host.progress->writesAtFinish, writes + 1);
            QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
            host.progress->holdFinish = false;
            host.progress->completeFinish(true);
            elapsed.restart();
        }
        const auto armedRetry = [&]() -> QTimer * {
            for (auto *timer : host.window()->findChildren<QTimer *>("rrTransitionSaveRetry")) {
                if (timer->isActive())
                    return timer;
            }
            return nullptr;
        };
        if (outcome == "failed" || outcome.startsWith("persistent-") || outcome == "battery-low") {
            // The test advances each retry itself, even if a busy host delivers its timer first.
            struct HoldRetries : QObject
            {
                bool eventFilter(QObject *object, QEvent *event) override
                {
                    return event->type() == QEvent::Timer
                            && object->objectName() == "rrTransitionSaveRetry";
                }
            } hold;

            qApp->installEventFilter(&hold);
            const int expectedAttempts = outcome == "failed" ? 1 : 3;
            for (int attempt = 1; attempt <= expectedAttempts; ++attempt) {
                QTRY_VERIFY(armedRetry());
                QCOMPARE(host.progress->writeAttempts - attempts, attempt);
                auto *retry = armedRetry();
                QVERIFY(retry->isSingleShot());
                QCOMPARE(retry->interval(), 100);
                if (outcome == "failed")
                    host.progress->failWrite = false;
                retry->stop();
                QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection);
            }
        }
        if (outcome.startsWith("persistent-")) {
            QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
            QCOMPARE(completed, 0);
            QCOMPARE(host.progress->writeAttempts - attempts, 3);
            QVERIFY(!armedRetry());
            if (outcome == "persistent-retry") {
                host.progress->failWrite = false;
                host.window()->findChild<QPushButton *>("rrCloseSaveRetry")->click();
            } else
                host.window()->findChild<QPushButton *>("rrCloseSaveQuit")->click();
        }
        if (outcome == "timeout") {
            // The held mapping cannot finish the transition; the deadline does.
            QVERIFY(mappingDeadline && mappingDeadline->isActive());
            QCoreApplication::processEvents();
            QCOMPARE(completed, 0);
            runMappingDeadline();
        }
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 3000);
        QVERIFY(elapsed.elapsed() < 2400);
        if (outcome == "timeout") {
            QVERIFY(!shutdownEncodingReturned);
            releaseShutdownEncoding = true;
            QTRY_VERIFY(shutdownEncodingReturned.load());
            // Drain the late engine callback while the window remains alive.
            QTest::qWait(100);
        }
        delayShutdownEncoding = false;
        QCOMPARE(completed, 1);
        if (outcome == "persistent-failure" || outcome == "battery-low") {
            QCOMPARE(host.progress->writeAttempts - attempts, 3);
            QVERIFY(host.window()->property("reader_transition_save_failed").toBool());
            QVERIFY(!armedRetry());
        }
        QCOMPARE(host.progress->writes,
                 writes
                         + (outcome == "saved" || outcome == "failed"
                                            || outcome == "persistent-retry"
                                            || outcome == "battery-low-held"
                                    ? 1
                                    : 0));
        if (outcome != "saved" && outcome != "failed" && outcome != "persistent-retry"
            && outcome != "battery-low-held") {
            QVERIFY(host.progress->current == before);
        }
        const auto local = storage::readBookState(paths, identity).object;
        QVERIFY(!local.value("position").toString().isEmpty());
    }

    void progressChoiceKeepAndGo_data()
    {
        QTest::addColumn<bool>("go");
        QTest::addColumn<bool>("sameTimestamp");
        QTest::addColumn<bool>("roundTrip");
        QTest::addColumn<bool>("closeBook");
        QTest::newRow("keep") << false << false << false << false;
        QTest::newRow("go") << true << false << false << false;
        QTest::newRow("equal-time-keep") << false << true << false << false;
        QTest::newRow("equal-time-go") << true << true << false << false;
        QTest::newRow("offset-round-trip") << false << true << true << false;
        QTest::newRow("offset-round-trip-close") << false << true << true << true;
        QTest::newRow("equal-time-keep-close") << false << true << false << true;
    }

    void progressChoiceKeepAndGo()
    {
        QFETCH(bool, go);
        QFETCH(bool, sameTimestamp);
        QFETCH(bool, roundTrip);
        QFETCH(bool, closeBook);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity,
                                   roundTrip ? "OEBPS/c1.xhtml#kobo.1.1:105" : "OEBPS/c1.xhtml",
                                   100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(0));
        QTRY_VERIFY(!host.session()->busy());
        host.progress->publish(host.progress->current);
        const int initialReads = host.progress->reads;
        host.window()->progressTick();
        QCOMPARE(host.progress->reads, initialReads + 1);
        QVERIFY(!host.book()->pendingProgress());
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QString mappingError;
        const auto precise = host.session()->savedNativePosition(&mappingError);
        QVERIFY2(mappingError.isEmpty(), qPrintable(mappingError));
        if (roundTrip) {
            const auto anchor = host.session()->state().value("position").toString();
            host.progress->publish({ identity, "OEBPS/c1.xhtml#kobo.1.1:0", 100, 1, true });
            if (closeBook) {
                host.window()->close();
                QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 3000);
                QCOMPARE(host.progress->writes, 1);
                QCOMPARE(host.progress->current.location, precise);
                return;
            }
            host.window()->progressTick();
            QCoreApplication::processEvents();
            QVERIFY(!host.book()->pendingProgress());
            QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
            QCOMPARE(host.session()->state().value("position").toString(), anchor);
            return;
        }
        // Another device saved a position later in the book.
        const ReadingProgress synced{ identity, "OEBPS/c3.xhtml#kobo.8.1:7",
                                      sameTimestamp ? 100 : 200, 1, true };
        host.progress->publish(synced);
        if (closeBook)
            host.window()->close();
        else
            host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                                         && card->isVisible(),
                                 10000);
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.after"));
        const int before = host.session()->page();
        auto *choice =
                card->findChild<QPushButton *>(go ? "rrGoToSyncedPosition" : "rrKeepPosition");
        QVERIFY(choice);
        QVERIFY(click(canvas, choice));
        if (closeBook) {
            QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 3000);
            QCOMPARE(host.progress->writes, 1);
            QCOMPARE(host.progress->current.location, precise);
            return;
        }
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QTRY_VERIFY(!host.session()->busy());
        if (go)
            QVERIFY(host.session()->page() > before);
        else
            QCOMPARE(host.session()->page(), before);
        // The choice is remembered: the same saved position does not ask again.
        host.progress->publish(synced);
        const int reads = host.progress->reads;
        host.window()->progressTick();
        QCOMPARE(host.progress->reads, reads + 1);
        QVERIFY(!host.book()->pendingProgress());
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    // Checks that a newer synced position waits while Settings, the menu or the image viewer covers the page.
    void progressChoiceWaitsForSettings_data()
    {
        QTest::addColumn<bool>("image");
        QTest::newRow("settings") << false;
        QTest::newRow("image") << true;
    }

    void progressChoiceWaitsForSettings()
    {
        QFETCH(bool, image);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        host.window()->progressTick();
        QPushButton *close = nullptr;
        if (image) {
            auto *viewer = static_cast<ImageViewer *>(
                    host.window()->findChild<QWidget *>("rrImageViewer"));
            QVERIFY(viewer);
            viewer->open();
            QImage picture(200, 200, QImage::Format_RGB32);
            picture.fill(Qt::white);
            viewer->setImage(picture);
            close = viewer->findChild<QPushButton *>("rrImageClose");
        } else {
            auto *page = host.window()->findChild<QWidget *>("rrPage");
            QVERIFY(page);
            QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            close = host.window()->findChild<QPushButton *>("rrSettingsClose");
        }
        QTRY_VERIFY(close && close->isVisible());
        // The platform reports a newer position while Settings is open.
        host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true });
        host.window()->progressTick();
        QVERIFY(host.progress->changed);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QVERIFY(click(canvas, close));
        // Settings closes directly to reading.
        if (!image) {
            auto *top = host.window()->findChild<QWidget *>("rrControlsTop");
            QTRY_VERIFY(!top->isVisible());
        }
        // The reader asks without another progress check.
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                                         && card->isVisible(),
                                 10000);
        auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    // Checks that a newer synced position waits while the bookmark list or the menu covers the page.
    void progressChoiceWaitsForBookmarks()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        host.window()->progressTick();
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        auto *top = host.window()->findChild<QWidget *>("rrControlsTop");
        auto *list = host.window()->findChild<QWidget *>("rrBookmarkList");
        QVERIFY(page && top && list);
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(top->isVisible());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrBookmarks")));
        QTRY_VERIFY(list->isVisible());
        // The platform reports a newer position while Bookmarks is open.
        host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true });
        host.window()->progressTick();
        QVERIFY(host.progress->changed);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrBookmarkListClose")));
        QTRY_VERIFY(!list->isVisible());
        // The menu still covers the page, so the choice keeps waiting.
        QVERIFY(top->isVisible());
        host.window()->progressTick();
        QVERIFY(host.progress->changed);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(!top->isVisible());
        // The reader asks without another progress check.
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                                         && card->isVisible(),
                                 10000);
        auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    void finishedSyncWithoutPositionKeepsLocalReading()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state().value("position");
        host.progress->publish({ identity, {}, 200, 2, true });
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                    && card->isVisible());
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.finished"));
        host.window()->sleepReader();
        QVERIFY(!card->isVisible());
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        host.window()->progressTick();
        QTRY_VERIFY(card->isVisible());
        QCOMPARE(host.session()->state().value("position"), original);
        auto *go = card->findChild<QPushButton *>("rrGoToSyncedPosition");
        QVERIFY(go);
        QVERIFY(go->isHidden());
        auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(keep->property("reader_primary_action").toBool());
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QCOMPARE(host.session()->state().value("position"), original);
        host.progress->publish(host.progress->current);
        const int reads = host.progress->reads;
        host.window()->progressTick();
        QCOMPARE(host.progress->reads, reads + 1);
        QVERIFY(!host.book()->pendingProgress());
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QCOMPARE(host.session()->state().value("position"), original);
    }

    void progressChoiceForOlderAlert()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(0));
        QTRY_VERIFY(!host.session()->busy());
        host.progress->publish({ identity, "OEBPS/c2.xhtml#kobo.5.1:3", 200, 1, true });
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                                         && card->isVisible(),
                                 10000);
        // A newer position arrives while the first alert is open. Keeping the
        // first one must not accept the newer one, so the reader asks again.
        // A tick while the engine is busy hides the alert until the next tick,
        // so tick only when it is idle.
        QPointer<QWidget> first(card);
        // Positions no other test has accepted, so neither counts as already chosen.
        host.progress->publish({ identity, "OEBPS/c3.xhtml#kobo.12.1:3", 300, 1, true });
        QTRY_VERIFY(!host.session()->busy());
        host.window()->progressTick();
        QTRY_VERIFY(first && first->isVisible());
        auto *keep = first->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!first);
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                                         && card->isVisible(),
                                 10000);
        auto *keepNewer = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keepNewer);
        QVERIFY(click(canvas, keepNewer));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    void progressChoiceUnavailablePosition()
    {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const int before = host.session()->page();
        // A saved position the book cannot place still offers the choice, and
        // going there reports that it is unavailable instead of moving.
        host.progress->publish({ identity, "OEBPS/missing.xhtml#kobo.1.1:0", 200, 1, true });
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition"))
                                         && card->isVisible(),
                                 10000);
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.unlocated"));
        auto *go = card->findChild<QPushButton *>("rrGoToSyncedPosition");
        QVERIFY(go);
        QVERIFY(click(canvas, go));
        QTRY_COMPARE_WITH_TIMEOUT(message->text(), strings::get("sync.position_unavailable"),
                                  10000);
        QCOMPARE(host.session()->page(), before);
        auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QTRY_VERIFY(keep->isEnabled());
        // Only the acknowledgement is left; the failed choice is not shown disabled.
        QVERIFY(go->isHidden());
        QCOMPARE(keep->text(), strings::get("common.ok"));
        QVERIFY(keep->property("reader_primary_action").toBool());
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    void appearanceOpeningDoesNotRepaintToolbar_data()
    {
        QTest::addColumn<QString>("buttonName");
        QTest::newRow("text") << "rrNavText";
        QTest::newRow("layout") << "rrNavLayout";
        QTest::newRow("stats") << "rrNavStats";
        QTest::newRow("font-picker-back") << "rrFontClose";
    }

    void appearanceOpeningDoesNotRepaintToolbar()
    {
        QFETCH(QString, buttonName);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QVERIFY(QTest::qWaitForWindowExposed(host.window()));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        page->tapped(page->rect().center());
        if (buttonName == "rrFontClose") {
            host.window()->findChild<QPushButton *>("rrNavText")->click();
            host.window()->findChild<QPushButton *>("rrFont")->click();
        }
        auto *toolbar = host.window()->findChild<QWidget *>("rrControlsTop");
        auto *button = host.window()->findChild<QPushButton *>(buttonName);
        QVERIFY(toolbar);
        QVERIFY(button);
        QCoreApplication::processEvents();
        PaintRegions paints;
        toolbar->installEventFilter(&paints);
        for (auto *widget : toolbar->findChildren<QWidget *>())
            widget->installEventFilter(&paints);
        // Measure the view transition without button press/release feedback.
        button->click();
        QVERIFY(!host.session()->busy());
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
    }

    // Checks that the annotation list looks up pages on demand through closing, tab switches, wake, reflow and deletion.
    void annotationListResolvesPagesOnDemand()
    {
        QTemporaryDir directory;
        storage::Paths paths{ directory.path() };
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QVector<SearchHit> hits;
        bool done = false;
        host.session()->search("reader", [&](SearchBatch batch) {
            hits += batch.hits;
            done = batch.done;
        });
        QTRY_VERIFY(done);
        QVERIFY(hits.size() > 2);
        const QString identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        HighlightStore store(paths.annotations(), identity, host.session()->sourceHash(), "test",
                             paths.database());
        QVERIFY(store.load());
        QVector<Highlight> entries;
        for (int i = 0; i < 2000; ++i) {
            const bool first = i == 1999;
            const auto &hit = first ? hits.first() : hits.last();
            entries.append({ QString("lazy-%1").arg(i), hit.start, hit.end, hit.text,
                             first ? QString("First passage") : QString("Later passage"),
                             QDateTime::currentDateTimeUtc() });
        }
        store.setHighlights(entries);
        QVERIFY(store.save());
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        done = false;
        bool prepared = false;
        host.window()->preparePresentation([&](bool ready) {
            prepared = ready;
            done = true;
        });
        QTRY_VERIFY(done && prepared);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        auto *list = host.window()->findChild<QWidget *>("rrBookmarkList");
        auto *empty = host.window()->findChild<QLabel *>("rrBookmarkListEmpty");
        QVERIFY(page && list && empty);
        auto drain = [&] {
            bool drained = false;
            ReaderSession::drain([&] { drained = true; });
            return QTest::qWaitFor([&] { return drained; }, 10000);
        };

        struct ResumeEngine
        {
            ~ResumeEngine() { engineActivity().pause(false); }
        } resume;

        for (int pass = 0; pass < 2; ++pass) {
            // Drain ordinary opening/reflow and repeated refreshes first. If
            // either eagerly resolved endpoints, opening the list below would
            // display rows immediately instead of its pending state.
            for (int i = 0; i < 3; ++i) {
                QCoreApplication::processEvents();
                QVERIFY(drain());
            }
            QCOMPARE(host.session()->markup().imageKey, host.session()->image().cacheKey());
            QCOMPARE(host.session()->markup().request.saved.size(), 2000);
            QVERIFY(!host.session()->markup().saved.isEmpty());
            engineActivity().pause(true);
            host.window()->findChild<QPushButton *>("rrBookmarks")->click();
            QVERIFY(list->isVisible() && empty->isVisible());
            QCOMPARE(empty->text(), QString::fromUtf8("Finding pages…"));
            QVERIFY(!host.window()->findChild<QWidget *>("rrBookmarkListEmptyHint")->isVisible());
            // Cancel before the queued job can run, then drain it without an
            // annotation list demanding another lookup. Reopening must still
            // wait, rather than accepting the abandoned result.
            if (pass == 0)
                host.window()->findChild<QPushButton *>("rrBookmarkListClose")->click();
            else
                host.window()->findChild<QPushButton *>("rrBookmarksTab")->click();
            if (pass == 0)
                QVERIFY(host.session()->turn(1));
            engineActivity().pause(false);
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(drain());
            if (pass == 0) {
                QVERIFY(host.session()->turn(-1));
                QTRY_VERIFY(!host.session()->busy());
                QVERIFY(drain());
            }
            engineActivity().pause(true);
            host.window()
                    ->findChild<QPushButton *>(pass == 0 ? "rrBookmarks" : "rrAnnotationsTab")
                    ->click();
            QVERIFY(list->isVisible() && empty->isVisible());
            QCOMPARE(empty->text(), QString::fromUtf8("Finding pages…"));
            if (pass == 0) {
                host.window()->sleepReader();
                host.window()->sleepViewReady();
                QVERIFY(!host.window()->isVisible());
                engineActivity().pause(false);
                QVERIFY(drain());
                engineActivity().pause(true);
                host.window()->prepareWake();
                host.window()->wakeReader();
                QVERIFY(list->isVisible() && empty->isVisible());
            }
            // Tabs remain usable while the worker has not answered.
            if (pass == 1) {
                host.window()->findChild<QPushButton *>("rrBookmarksTab")->click();
                QCOMPARE(empty->text(), strings::get("bookmarks.empty"));
                host.window()->findChild<QPushButton *>("rrAnnotationsTab")->click();
                QCOMPARE(empty->text(), QString::fromUtf8("Finding pages…"));
            }
            engineActivity().pause(false);
            QTRY_VERIFY(!empty->isVisible());
            auto *first = dynamic_cast<ui::ListRow *>(
                    host.window()->findChild<QPushButton *>("rrAnnotation0"));
            QVERIFY(first && first->isVisible() && first->isEnabled());
            QCOMPARE(first->titleLabel()->text(), QString("First passage"));
            if (pass == 0) {
                host.window()->findChild<QPushButton *>("rrBookmarkListClose")->click();
                QVERIFY(!list->isVisible());
                auto appearance = host.session()->state();
                appearance["font_pixels"] = appearance.value("font_pixels").toInt() + 4;
                host.session()->editAppearance(kobalt::appearance::Patch(appearance));
                QTRY_VERIFY(!host.session()->busy());
            }
        }
        host.window()->findChild<QPushButton *>("rrAnnotationRemove0")->click();
        auto *confirm = host.window()->findChild<QPushButton *>("rrConfirmRemoveAnnotation");
        QTRY_VERIFY(confirm && confirm->isVisible());
        confirm->click();
        QVERIFY(drain());
        host.window()->findChild<QPushButton *>("rrBookmarkListClose")->click();
        QVERIFY(drain());
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->turn(-1));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(drain());
        QCOMPARE(host.session()->markup().request.saved.size(), 1999);
        QCOMPARE(host.session()->markup().imageKey, host.session()->image().cacheKey());
        QVERIFY(host.session()->markup().saved.isEmpty());
        QVERIFY(store.load());
        QCOMPARE(store.highlights().size(), 1999);
    }

    // Checks that the footer, progress bar, Stats, sleep snapshot and saved book percent agree at the first, a middle and the last page, and that a book marked as finished sleeps at 100%.
    void progressPercentagesAgree_data()
    {
        QTest::addColumn<int>("position");
        QTest::newRow("first") << 0;
        QTest::newRow("middle") << 1;
        QTest::newRow("last") << 2;
        QTest::newRow("finished") << 3;
        QTest::newRow("scoped-native-sleep") << 4;
        QTest::newRow("back-matter-native-sleep") << 5;
    }

    void progressPercentagesAgree()
    {
        QFETCH(int, position);
        QTemporaryDir profile;
        auto paths = testPaths();
        paths.directory = profile.path();
        QVERIFY(storage::writeJson(paths.preferences(), { { sleep::preferenceKey, "page" } }));
        auto presentation = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        const auto identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        if (position >= 4) {
            host.progress = std::make_shared<test::HostProgress>();
            host.progress->current = { identity, "OEBPS/c1.xhtml", 100, 1, true };
            host.positions = nickel::positionCodec();
        }
        host.configureServices = [presentation](ReaderHostServices &services) {
            services.presentation = [presentation](QWidget *) {
                return std::make_unique<test::HostPresentation>(presentation);
            };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(nativeSize);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        if (position >= 4) {
            const bool backMatter = position == 5;
            bool applied = false;
            host.session()->chapterBoundaryActions(backMatter ? 0 : 1, [&](const auto &actions) {
                for (const auto &action : actions)
                    if (action.start == !backMatter && !action.reset)
                        applied = action.apply();
            });
            QTRY_VERIFY(applied && !host.session()->busy());
            QVERIFY(host.session()->goToChapter(1));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(host.session()->turn(1));
            QTRY_VERIFY(!host.session()->busy() && host.session()->bookPages() > 7);
            const int page = host.session()->bookPage();
            const int pages = host.session()->bookPages();
            QVERIFY(page > 0 && page < pages - 1);
            if (backMatter)
                QVERIFY(page > host.session()->lastMainPage());
            else
                QVERIFY(page > host.session()->firstMainPage());
            const int main = backMatter
                    ? 100
                    : pageProgressPercent(page - host.session()->firstMainPage(),
                                          host.session()->mainPages());
            const int volume = pageProgressPercent(page, pages);
            QVERIFY(main != volume);
            for (bool whole : { false, true }) {
                if (whole) {
                    auto *page = host.window()->findChild<QWidget *>("rrPage");
                    QVERIFY(page);
                    QTest::mouseClick(page, Qt::LeftButton, {}, page->rect().center());
                    auto *stats = host.window()->findChild<QPushButton *>("rrNavStats");
                    QVERIFY(stats);
                    stats->click();
                    auto *scope = host.window()->findChild<QPushButton *>("rrGraphWordScope");
                    QVERIFY(scope && scope->isVisible());
                    scope->click();
                    auto *option = host.window()->findChild<QAbstractButton *>("rrStatsWordVolume");
                    QVERIFY(option && option->isVisible());
                    option->click();
                    QCOMPARE(host.window()->findChild<QLabel *>("rrGraphBookPercent")->text(),
                             strings::percentage(volume));
                }
                QTRY_VERIFY(host.window()->readyForPresentation());
                const int expected = whole ? volume : main;
                QCOMPARE(host.window()->powerScreenSnapshot().percent, expected);
                QString mappingError;
                const auto anchor = host.session()->savedNativePosition(&mappingError);
                QVERIFY2(mappingError.isEmpty(), qPrintable(mappingError));
                const int writes = host.progress->writes;
                host.window()->sleepReader();
                QCOMPARE(host.progress->writes, writes + 1);
                QCOMPARE(host.progress->current.status, 1);
                QCOMPARE(host.progress->percent, double(expected));
                QCOMPARE(host.progress->current.location, anchor);
                host.window()->wakeReader();
                QTRY_VERIFY(!host.session()->busy());
            }
            return;
        }
        const double target = position == 0 ? 0.0
                : position == 2             ? 1.0
                : position == 3             ? 0.95
                                            : 0.15;
        QVERIFY(host.session()->goToFraction(target));
        QTRY_VERIFY(!host.session()->busy() && host.session()->bookPages() > 7);
        const int pageIndex = host.session()->bookPage();
        const int pages = host.session()->bookPages();
        if (position == 0)
            QCOMPARE(pageIndex, 0);
        else if (position == 2)
            QCOMPARE(pageIndex, pages - 1);
        else
            QVERIFY(pageIndex > 0 && pageIndex < pages - 1);
        // Nickel's count: the page on screen is read, and the result rounds down.
        const int percent = int(100 * (pageIndex + 1) / pages);
        auto *page = dynamic_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QTRY_COMPARE(page->slotContents[5].text, strings::percentage(percent));
        QCOMPARE(page->progressFraction, double(pageIndex + 1) / pages);
        page->tapped(page->rect().center());
        auto *stats = host.window()->findChild<QPushButton *>("rrNavStats");
        QVERIFY(stats);
        stats->click();
        auto *graph = host.window()->findChild<QWidget *>("rrChapterGraph");
        QTRY_VERIFY(graph && graph->isVisible());
        auto *bookPercent = graph->findChild<QLabel *>("rrGraphBookPercent");
        QVERIFY(bookPercent);
        QCOMPARE(bookPercent->text(), strings::percentage(percent));
        stats->click();
        QTRY_VERIFY(!graph->isVisible());
        QTRY_VERIFY(host.window()->readyForPresentation());
        // A book marked as finished is done; the reading views keep the page.
        if (position == 3) {
            host.book()->markFinished();
            QVERIFY(host.book()->finishedHere());
        }
        const int sleepPercent = position == 3 ? 100 : percent;
        QVERIFY(position != 3 || percent < 100);
        const auto snapshot = host.window()->powerScreenSnapshot();
        QVERIFY(!snapshot.image.isNull());
        QCOMPARE(snapshot.percent, sleepPercent);
        QCOMPARE(snapshot.progress,
                 strings::get("sleep.percent_read")
                         .arg(strings::percentArgument(sleepPercent, QLocale::c())));
        host.window()->checkpoint();
        QCOMPARE(storage::readBookState(paths, identity).object.value("percent").toInt(-1),
                 percent);
    }

    void annotationFramesKeepFirstPaintComplete_data()
    {
        QTest::addColumn<QString>("markup");
        for (const auto *name :
             { "none", "saved", "draft", "elsewhere", "elsewhere-256", "grayscale" }) {
            QTest::newRow(name) << QString(name);
        }
    }

    void annotationFramesKeepFirstPaintComplete()
    {
        QFETCH(QString, markup);
        QTemporaryDir directory;
        storage::Paths paths{ directory.path() };
        QVERIFY(storage::writeJson(paths.preferences(), { { "highlight_help_seen", true } }));
        int colorQueries = 0;
        bool inColor = true;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        if (markup == "grayscale") {
            host.configureServices = [](ReaderHostServices &services) {
                services.colorDisplay = [] { return false; };
            };
        } else if (markup == "elsewhere-256") {
            host.configureServices = [&](ReaderHostServices &services) {
                services.colorDisplay = [&] {
                    ++colorQueries;
                    return inColor;
                };
            };
        }
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QVector<SearchHit> hits;
        bool done = false;
        host.session()->search("reader", [&](SearchBatch batch) {
            hits += batch.hits;
            done = batch.done;
        });
        QTRY_VERIFY(done);
        QVERIFY(hits.size() > 2);
        TextRange passage;
        done = false;
        host.session()->textRange(hits.first().start, hits.last().end, [&](TextRange value) {
            passage = value;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(!passage.text.isEmpty());
        const QString identity =
                QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        HighlightStore store(paths.annotations(), identity, host.session()->sourceHash(), "test",
                             paths.database());
        if (markup != "none") {
            Highlight entry{ "frame-one", passage.start,
                             passage.end, passage.text,
                             {},          QDateTime::currentDateTimeUtc() };
            entry.color = "blue";
            if (markup.startsWith("elsewhere")) {
                entry.start = hits.last().start;
                entry.end = hits.last().end;
                entry.text = "reader";
            }
            QVector<Highlight> entries{ entry };
            if (markup == "elsewhere-256") {
                for (int i = 1; i < 256; ++i) {
                    entry.id = QString("frame-offpage-%1").arg(i);
                    entries.append(entry);
                }
            }
            if (markup == "saved") {
                entry.id = "frame-two";
                entry.color = "pink";
                entries.append(entry);
            }
            store.setHighlights(entries);
            QVERIFY(store.save());
        }
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        engineActivity().pause(true);

        struct ResumePreparation
        {
            ~ResumePreparation() { engineActivity().pause(false); }
        } resumePreparation;

        bool prepared = false, preparationSucceeded = false;
        host.window()->preparePresentation([&](bool ready) {
            preparationSucceeded = ready;
            prepared = true;
        });
        if (markup == "none") {
            QVERIFY(prepared && preparationSucceeded);
        } else {
            QVERIFY(!prepared);
            QCoreApplication::processEvents();
            QVERIFY(!prepared && !host.window()->isVisible());
        }
        engineActivity().pause(false);
        QTRY_VERIFY(prepared && preparationSucceeded);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        auto barrier = [&] {
            for (int i = 0; i < 3; ++i) {
                bool finished = false;
                host.session()->rangeRects(hits.first().start, hits.first().end,
                                           [&](QVector<QRect>) { finished = true; });
                QElapsedTimer timeout;
                timeout.start();
                while (!finished && timeout.elapsed() < 10000) {
                    QTest::qWait(1);
                }
                if (!finished) {
                    return false;
                }
                QCoreApplication::processEvents();
            }
            return true;
        };
        QVERIFY(barrier());
        if (markup == "elsewhere-256") {
            for (bool color : { true, false }) {
                inColor = color;
                colorQueries = 0;
                prepared = false;
                host.window()->preparePresentation([&](bool ready) {
                    preparationSucceeded = ready;
                    prepared = true;
                });
                QTRY_VERIFY(prepared && preparationSucceeded);
                const auto &saved = host.session()->markup().request.saved;
                QCOMPARE(saved.size(), 256);
                QCOMPARE(saved.first().color, color ? QString("blue") : QString("gray"));
                QVERIFY2(colorQueries < saved.size(),
                         "Refreshing the same book checked display capability for every highlight");
            }
        }
        if (markup == "grayscale") {
            QVector<QRect> rects;
            done = false;
            host.session()->rangeRects(hits.first().start, hits.first().end,
                                       [&](QVector<QRect> value) {
                                           rects = value;
                                           done = true;
                                       });
            QTRY_VERIFY(done && !rects.isEmpty());
            page->annotationRequested();
            const QPoint point = rects.first().center() + QPoint(0, page->topHeight());
            for (const auto &style : { "underline", "dotted" }) {
                page->held(point - QPoint(0, page->topHeight()));
                const QString name = "rrAnnotationStyle_" + QString(style);
                QTRY_VERIFY(host.window()->findChild<QAbstractButton *>(name)
                            && host.window()->findChild<QAbstractButton *>(name)->isVisible());
                QVERIFY(!host.window()->findChild<QWidget *>("rrAnnotationPalette"));
                QVERIFY(!host.window()->findChild<QWidget *>("rrAnnotationSave"));
                host.window()->findChild<QAbstractButton *>(name)->click();
                QVERIFY(host.window()->findChild<QWidget *>("rrAnnotationPopover")->isVisible());
                host.window()->findChild<QPushButton *>("rrAnnotationApply")->click();
                QTRY_VERIFY(
                        !host.window()->findChild<QWidget *>("rrAnnotationPopover")->isVisible());
                // The save follows on the highlight session's writer thread.
                QTRY_VERIFY(store.load() && store.highlights().first().style == QString(style));
                QCOMPARE(store.highlights().first().color, QString("blue"));
                QVERIFY(barrier());
            }
            page->tapped(page->rect().center());
            auto *exit = host.window()->findChild<QPushButton *>("rrAnnotate");
            QTRY_VERIFY(exit && exit->isVisible());
            QVERIFY(click(canvas, exit));
            page->tapped(page->rect().center());
            QVERIFY(barrier());
        }
        if (markup == "draft") {
            QVector<QRect> rects;
            done = false;
            host.session()->rangeRects(hits.first().start, hits.first().end,
                                       [&](QVector<QRect> value) {
                                           rects = value;
                                           done = true;
                                       });
            QTRY_VERIFY(done && !rects.isEmpty());
            page->annotationRequested();
            page->held(rects.first().center());
            auto edit = [&] { return host.window()->findChild<QPushButton *>("rrAnnotationEdit"); };
            QTRY_VERIFY(edit() && edit()->isVisible());
            edit()->click();
            QVERIFY(barrier());
        }

        class FramePaints final : public QObject
        {
        public:
            ReaderFixture &host;
            ReadingPage &page;
            int destination = -1;
            QVector<QImage> images;
            QVector<qint64> milliseconds;
            QElapsedTimer elapsed;

            FramePaints(ReaderFixture &host, ReadingPage &page) : host(host), page(page) { }

            bool eventFilter(QObject *, QEvent *event) override
            {
                if (event->type() == QEvent::Paint && !host.session()->busy()
                    && host.session()->page() == destination) {
                    images.append(page.snapshot());
                    milliseconds.append(elapsed.elapsed());
                }
                return false;
            }
        } paints(host, *page);

        page->installEventFilter(&paints);
        bool complete = true;
        for (int i = 0; i < 8; ++i) {
            paints.images.clear();
            paints.milliseconds.clear();
            paints.destination = i % 2 ? 0 : 1;
            paints.elapsed.start();
            QVERIFY(host.session()->turn(i % 2 ? -1 : 1));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(barrier());
            QTRY_VERIFY(!paints.images.isEmpty());
            const QImage finished = page->snapshot();
            const auto &frameMarkup = host.session()->markup();
            QCOMPARE(frameMarkup.imageKey, host.session()->image().cacheKey());
            QCOMPARE(frameMarkup.generation, host.session()->markupGeneration());
            if (markup == "none" || markup.startsWith("elsewhere")) {
                QCOMPARE(frameMarkup.resolvedRanges, 0);
            } else {
                QCOMPARE(frameMarkup.resolvedRanges, markup == "saved" ? 2 : 1);
            }
            int firstComplete = -1;
            for (int j = 0; j < paints.images.size(); ++j) {
                if (paints.images[j] == finished) {
                    firstComplete = j;
                    break;
                }
            }
            complete = complete && firstComplete == 0;
            QCOMPARE(paints.images.size(), 1);
            if (qEnvironmentVariableIsSet("KOBALT_ANNOTATION_METRICS")) {
                qInfo("annotation-frame scenario=%s turn=%d paints=%d first_ms=%lld complete_ms=%lld first_complete=%d",
                      qPrintable(markup), i, int(paints.images.size()), paints.milliseconds.first(),
                      firstComplete < 0 ? -1LL : paints.milliseconds[firstComplete],
                      firstComplete == 0);
            }
        }
        QVERIFY2(complete,
                 "The first painted page must already contain its complete saved/draft markup");
        if (markup == "saved" || markup == "draft") {
            paints.images.clear();
            paints.milliseconds.clear();
            paints.destination = host.session()->page();
            paints.elapsed.start();
            auto appearance = host.session()->state();
            appearance["font_pixels"] = appearance.value("font_pixels").toInt() + 4;
            host.session()->editAppearance(kobalt::appearance::Patch(appearance));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(barrier());
            QTRY_VERIFY(!paints.images.isEmpty());
            QCOMPARE(paints.images.first(), page->snapshot());
            QCOMPARE(paints.images.size(), 1);
            const int count = paints.images.size();
            QVERIFY(barrier());
            QCoreApplication::processEvents();
            QCOMPARE(paints.images.size(), count);
            QCOMPARE(host.session()->markup().imageKey, host.session()->image().cacheKey());
        }
        if (markup == "draft") {
            QVERIFY(annotationDatabaseEnabled());
            QVERIFY(store.load());
            QJsonArray persisted;
            for (const auto &highlight : store.highlights())
                persisted.append(highlightJson(highlight));
            QCOMPARE(persisted.size(), 1);
            paints.images.clear();
            paints.milliseconds.clear();
            paints.destination = 1;
            paints.elapsed.start();
            engineActivity().pause(true);
            host.window()->pageButton(1);
            QVERIFY(host.session()->busy());
            page->tapped(QPoint(page->width() / 2, page->topHeight() / 2));
            auto *exit = host.window()->findChild<QPushButton *>("rrAnnotate");
            QVERIFY(exit && exit->isVisible());
            QVERIFY(click(canvas, exit));
            page->tapped(page->rect().center());
            engineActivity().pause(false);
            QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
            QVERIFY(barrier());
            QTRY_VERIFY(!paints.images.isEmpty());
            QCOMPARE(paints.images.first(), page->snapshot());
            QVERIFY(host.session()->markup().request.draftStart.isEmpty());
            QCOMPARE(host.session()->markup().request.saved.size(), 1);
            HighlightStore reopened(paths.annotations(), identity, host.session()->sourceHash(),
                                    "test", paths.database());
            QVERIFY(reopened.load());
            QJsonArray actual;
            for (const auto &highlight : reopened.highlights())
                actual.append(highlightJson(highlight));
            QCOMPARE(actual, persisted);
            QVERIFY(!host.window()->findChild<QWidget *>("rrAnnotationPopover")->isVisible());
        }
    }

private:
    // A successful sync sets the sync time only in memory. The reader writes
    // it to the account file when the book closes or the account changes, so
    // read it from the reader's KOSync page.
    static bool showsSyncTime(QWidget *window)
    {
        auto *label = window ? window->findChild<QLabel *>("rrKosyncLastSync") : nullptr;
        return label
                && label->text()
                != strings::get("kosync.last_synced").arg(strings::get("kosync.never"));
    }

    void verifyKosyncManualHandoff(const QString &state)
    {
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", qEnvironmentVariable("KOBALT_KOSYNC_TEST_SERVER") },
                  { "username", qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER") },
                  { "key", QString(32, 'a') },
                  { "device_id", "manual-handoff" },
                  { "automatic", false } }));
        auto network = std::make_shared<SyncNetworkState>();
        bool suspended = false;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.suspended = [&] { return suspended; };
        };
        CloseHost cleanup{ host };

        struct ResumeEngine
        {
            ~ResumeEngine() { engineActivity().pause(false); }
        } resume;

        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        kosync::Client seed(paths.syncAccount());
        bool seeded = false;
        QString failure;
        seed.push(host.session()->syncDigest(),
                  { host.session()->state().value("position").toString(), {}, {}, 0 }, {},
                  [&](QString error) {
                      failure = error;
                      seeded = true;
                  });
        QTRY_VERIFY_WITH_TIMEOUT(seeded, 12000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        initializeDiagnostics(paths.directory + "/diagnostics");
        pauseDiagnostics(false);

        struct PauseDiagnostics
        {
            ~PauseDiagnostics() { pauseDiagnostics(true); }
        } diagnostics;

        const auto events = [&] {
            flushDiagnostics();
            QFile file(paths.directory + "/diagnostics/events.jsonl");
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray();
            return file.readAll();
        };
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(buildKosyncPage(host));
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        if (state == "busy") {
            engineActivity().pause(true);
            QVERIFY(host.session()->go(host.session()->page() + 1));
            QVERIFY(host.session()->busy());
        }
        host.window()->findChild<QPushButton *>("rrKosyncNow")->click();
        QCOMPARE(network->requests, 1);
        QVERIFY(!host.window()->findChild<QWidget *>("rrKosyncOpening"));
        // Native connection completion can precede restoring the reader.
        suspended = state != "busy";
        if (suspended)
            host.window()->hide();
        network->connected = true;
        auto completed = std::move(network->completed);
        network->completed = {};
        completed(true);
        QTRY_VERIFY_WITH_TIMEOUT(events().contains("\"event\":\"kosync-reader-wait\""), 3000);
        QCOMPARE(status->text(), strings::get("kosync.syncing"));
        QVERIFY(!host.window()->findChild<QPushButton *>("rrKosyncServer")->isEnabled());
        QVERIFY(!showsSyncTime(host.window()));
        QPointer<QWidget> popup = host.window()->findChild<QWidget *>("rrKosyncOpening");
        QVERIFY(popup);
        if (state == "deadline") {
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.reader_timeout"), 11000);
        } else if (state == "cancel") {
            host.window()->show();
            QVERIFY(click(canvas, popup->findChild<QPushButton *>("rrKosyncCancel")));
        } else if (state == "sleep") {
            QElapsedTimer elapsed;
            elapsed.start();
            host.window()->sleepReader();
            QTRY_VERIFY_WITH_TIMEOUT(!host.window()->pendingNetworkSync(), 2100);
            QVERIFY(elapsed.elapsed() < 2100);
        }
        suspended = false;
        engineActivity().pause(false);
        host.window()->show();
        if (state == "cancel" || state == "deadline" || state == "sleep") {
            QTest::qWait(400);
            QVERIFY(!showsSyncTime(host.window()));
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 3000);
            QVERIFY(showsSyncTime(host.window()));
            QVERIFY(popup && popup->isVisible());
            QCOMPARE(popup->findChild<QLabel *>("rrModalTitle")->text(),
                     strings::get("kosync.completed"));
            QVERIFY(!popup->findChild<QPushButton *>("rrKosyncCancel")->isVisible());
            QVERIFY(!host.window()->flushBeforeStandby());
            // The confirmation stays for its dismissal timer's interval. Check
            // the timer and run it now instead of waiting for it.
            auto *dismissal = popup->findChild<QTimer *>("rrKosyncSuccessTimer");
            QVERIFY(dismissal && dismissal->isActive() && dismissal->interval() >= 1500);
            QCoreApplication::processEvents();
            QVERIFY(popup && popup->isVisible());
            dismissal->stop();
            QMetaObject::invokeMethod(dismissal, "timeout", Qt::DirectConnection);
        }
        QTRY_VERIFY(!popup || !popup->isVisible());
        QCOMPARE(network->requests, 1);
        int reads = 0;
        for (const auto &line : events().split('\n')) {
            const auto event = QJsonDocument::fromJson(line).object();
            if (event.value("event").toString() == "kosync-request-started"
                && event.value("method").toString() == "GET")
                ++reads;
        }
        QCOMPARE(reads, 1);
    }

    void verifyKosyncNetwork(const QString &scenario)
    {
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://127.0.0.1:%1").arg(stalled.serverPort()) },
                  { "username", "test-reader" },
                  { "key", QString(32, 'a') },
                  { "device_id", "test-device" },
                  { "automatic", true } }));
        auto network = std::make_shared<SyncNetworkState>();
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        bool prepared = false;
        host.window()->preparePresentation([&](bool ready) { prepared = ready; });
        QCOMPARE(network->requests, 1);
        QVERIFY(!network->interactive);
        QCOMPARE(attempts.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(prepared, 1500);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QPointer<QWidget> popup;
        QTRY_VERIFY((popup = host.window()->findChild<QWidget *>("rrKosyncOpening"))
                    && popup->isVisible());
        const auto lateReady = network->completed;
        QVERIFY(bool(lateReady));
        if (scenario == "ready") {
            network->connected = true;
            auto completed = std::move(network->completed);
            network->completed = {};
            completed(true);
            QTRY_VERIFY(attempts.count() > 0);
            QVERIFY(click(canvas, popup->findChild<QPushButton *>("rrKosyncCancel")));
        } else if (scenario == "sleep") {
            const auto position = host.session()->state().value("position").toString();
            host.window()->setProperty("reader_full_flash", false);
            host.window()->sleepReader();
            QVERIFY(!host.window()->property("reader_full_flash").toBool());
            QTRY_COMPARE(network->cancellations, 1);
            QTRY_VERIFY(!host.window()->pendingNetworkSync());
            QCOMPARE(network->requests, 1);
            QCOMPARE(attempts.count(), 0);
            const auto identity =
                    QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
            QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(),
                     position);
        } else if (scenario == "deadline") {
            // The connection never answers, so the sync deadline cancels it.
            QVERIFY(runKosyncDeadline(host.window()));
            QTRY_VERIFY(!popup || !popup->isVisible());
            QCOMPARE(network->cancellations, 1);
            QCOMPARE(attempts.count(), 0);
            QVERIFY(buildKosyncPage(host));
            QCOMPARE(host.window()->findChild<QLabel *>("rrKosyncStatus")->text(),
                     strings::get("kosync.connection_timeout"));
        } else {
            QVERIFY(click(canvas, popup->findChild<QPushButton *>("rrKosyncCancel")));
            QCOMPARE(network->cancellations, 1);
            network->connected = true;
            lateReady(true);
            QCoreApplication::processEvents();
            QCOMPARE(attempts.count(), 0);
            network->connected = false;
            QVERIFY(buildKosyncPage(host));
            // A later periodic sync without Wi-Fi permission stays local.
            auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
            QVERIFY(status);
            status->setText("unchanged");
            clock += 16 * 60000;
            QVERIFY(host.session()->turn(1));
            QTRY_VERIFY(!host.session()->busy());
            QCoreApplication::processEvents();
            QCOMPARE(network->requests, 1);
            QCOMPARE(status->text(), strings::get("kosync.no_connection"));
            QCOMPARE(attempts.count(), 0);
            if (scenario == "login") {
                // Editing credentials exposes Sign in even for the saved account.
                auto *username = host.window()->findChild<QPushButton *>("rrKosyncUsername");
                QVERIFY(username);
                username->click();
                auto *input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
                QVERIFY(input);
                const auto usernameStyle = input->styleSheet();
                QVERIFY(usernameStyle.contains("solid"));
                QTRY_VERIFY(input->isVisible());
                const int usernameLeft = input->mapTo(host.window(), QPoint()).x();
                input->setText("another-reader");
                host.window()->findChild<QPushButton *>("rrKosyncEditSave")->click();
                QTRY_VERIFY(!host.window()->findChild<QLineEdit *>("rrKosyncInput"));
                host.window()->findChild<QPushButton *>("rrKosyncPassword")->click();
                input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
                QVERIFY(input);
                // Both credentials use the same bordered input at the same inset.
                QCOMPARE(input->styleSheet(), usernameStyle);
                QTRY_VERIFY(input->isVisible());
                QCOMPARE(input->mapTo(host.window(), QPoint()).x(), usernameLeft);
                input->setText("fixture-password");
                host.window()->findChild<QPushButton *>("rrKosyncEditSave")->click();
                QTRY_VERIFY(!host.window()->findChild<QLineEdit *>("rrKosyncInput"));
                host.window()->findChild<QPushButton *>("rrKosyncLogin")->click();
            } else {
                auto *sync = host.window()->findChild<QPushButton *>("rrKosyncNow");
                QVERIFY(sync);
                sync->click();
            }
            QCOMPARE(network->requests, 2);
            QVERIFY(network->interactive);
            QVERIFY(!host.window()->findChild<QPushButton *>("rrKosyncServer")->isEnabled());
            const auto failed = std::move(network->completed);
            network->completed = {};
            failed(false);
            QCOMPARE(attempts.count(), 0);
            QVERIFY(host.window()->findChild<QPushButton *>("rrKosyncServer")->isEnabled());
            if (scenario == "login") {
                // A failed connection keeps the draft. Only an explicit retry
                // opens another connection dialog.
                QCOMPARE(network->requests, 2);
                host.window()->findChild<QPushButton *>("rrKosyncLogin")->click();
                QCOMPARE(network->requests, 3);
                QVERIFY(network->interactive && network->completed);
                const auto retried = std::exchange(network->completed, {});
                retried(false);
                QCOMPARE(attempts.count(), 0);
            }
        }
        QVERIFY(!kosync::Client(paths.syncAccount()).lastSync().isValid());
    }

    void verifyKosyncWakeRecovery()
    {
        const QUrl destination(qEnvironmentVariable("KOBALT_KOSYNC_TEST_SERVER"));
        QTcpServer relay;
        QVector<QPointer<QTcpSocket>> connections;
        int accepted = 0;
        connect(&relay, &QTcpServer::newConnection, &relay, [&] {
            while (auto *incoming = relay.nextPendingConnection()) {
                ++accepted;
                connections.append(incoming);
                auto *upstream = new QTcpSocket(incoming);
                connect(incoming, &QTcpSocket::readyRead, upstream, [incoming, upstream] {
                    if (upstream->state() == QAbstractSocket::ConnectedState)
                        upstream->write(incoming->readAll());
                });
                connect(upstream, &QTcpSocket::connected, incoming,
                        [incoming, upstream] { upstream->write(incoming->readAll()); });
                connect(upstream, &QTcpSocket::readyRead, incoming,
                        [incoming, upstream] { incoming->write(upstream->readAll()); });
                connect(incoming, &QTcpSocket::disconnected, incoming, &QObject::deleteLater);
                connect(upstream, &QTcpSocket::disconnected, incoming, &QObject::deleteLater);
                upstream->connectToHost(destination.host(), quint16(destination.port()));
            }
        });
        QVERIFY(relay.listen(QHostAddress::LocalHost));
        const auto port = relay.serverPort();
        auto paths = testPaths();
        QVERIFY(storage::writeJson(
                paths.syncAccount(),
                { { "schema", 1 },
                  { "server", QString("https://localhost:%1").arg(port) },
                  { "username", qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER") },
                  { "key", QString(32, 'a') },
                  { "device_id", "wake-recovery-device" },
                  { "automatic", true } }));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        // Isolate the initial state from earlier rows using the same server account.
        kosync::Client seed(paths.syncAccount());
        bool seeded = false;
        QString seedError;
        seed.push(host.session()->syncDigest(),
                  { host.session()->state().value("position").toString(), {}, {}, 0 }, {},
                  [&](QString error) {
                      seedError = error;
                      seeded = true;
                  });
        QTRY_VERIFY_WITH_TIMEOUT(seeded, 12000);
        QVERIFY2(seedError.isEmpty(), qPrintable(seedError));
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(buildKosyncPage(host));
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 12000);
        host.window()->sleepReader();
        QTRY_VERIFY_WITH_TIMEOUT(!host.window()->pendingNetworkSync(), 3500);
        const auto position = host.session()->state().value("position").toString();
        relay.close();
        for (const auto &incoming : connections)
            if (incoming)
                incoming->abort();
        QCoreApplication::processEvents();
        const int before = accepted;
        QTimer::singleShot(1000, &relay,
                           [&] { QVERIFY(relay.listen(QHostAddress::LocalHost, port)); });
        host.window()->wakeReader();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 8000);
        QVERIFY(accepted > before);
        QCOMPARE(host.session()->state().value("position").toString(), position);
    }

    void verifyKosyncChoice(const QString &decision)
    {
        const bool forward = decision == "resume";
        const bool invalid = decision == "invalid";
        const QSize size(800, 1136);
        auto paths = testPaths();
        QJsonObject account{ { "schema", 1 },
                             { "server", qEnvironmentVariable("KOBALT_KOSYNC_TEST_SERVER") },
                             { "username", qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER") },
                             { "key", QString(32, 'a') },
                             { "device_id", "choice-local" },
                             { "automatic", false } };
        QVERIFY(storage::writeJson(paths.syncAccount(), account));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{ host };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), size);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), size);
        canvas.resize(size);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(buildKosyncPage(host));
        const int count = host.session()->pages();
        QVERIFY(count > 10);
        QVERIFY(host.session()->go(int(count * (forward ? 0.8 : 0.2))));
        QTRY_VERIFY(!host.session()->busy());
        const auto remotePosition = invalid ? QString("/not/a/real/position")
                                            : host.session()->state().value("position").toString();
        const auto remoteChapter = host.session()->chapterTitle();
        // A contradictory percentage must not reverse the source-order warning.
        const double remotePercent = decision == "dismiss"
                ? 0.99
                : double(host.session()->page()) / (host.session()->pages() - 1);
        QVERIFY(host.session()->go(int(count * 0.5)));
        QTRY_VERIFY(!host.session()->busy());
        const auto localPosition = host.session()->state().value("position").toString();
        const auto localChapter = host.session()->chapterTitle();
        // The page total can still grow after the book opens. Read it with the page.
        const double localPercent = 100.0 * host.session()->page() / (host.session()->pages() - 1);
        QVERIFY(!localChapter.isEmpty() && !remoteChapter.isEmpty());
        account["device_id"] = "choice-remote";
        account["device_name"] = "Crosspoint X4";
        const auto otherPath = paths.directory + "/remote-account.json";
        QVERIFY(storage::writeJson(otherPath, account));
        kosync::Client other(otherPath);
        const auto document = host.session()->syncDigest();
        bool completed = false;
        QString failure;
        other.push(document, { remotePosition, {}, {}, remotePercent }, {}, [&](QString error) {
            failure = error;
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 12000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        auto *sync = host.window()->findChild<QPushButton *>("rrKosyncNow");
        QVERIFY(sync);
        sync->click();
        QPointer<QWidget> widget;
        QTRY_VERIFY((widget = host.window()->findChild<QWidget *>("rrKosyncChoice"))
                    && widget->isVisible());
        auto *dialog = dynamic_cast<ui::ChoiceModal *>(widget.data());
        QVERIFY(dialog);
        reader::SyncChoiceView *view = nullptr;
        QTRY_VERIFY((view = dynamic_cast<reader::SyncChoiceView *>(
                             dialog->findChild<QWidget *>("rrKosyncChoiceView")))
                    && view->isVisible());
        const auto &jump = view->jump();
        // Source order decides the direction, not the contradictory percentage.
        QCOMPARE(jump.direction(), invalid ? 0 : forward ? 1 : -1);
        QCOMPARE(jump.drawable(), !invalid);
        QCOMPARE(jump.fromChapter, localChapter);
        QCOMPARE(jump.toChapter, invalid ? QString() : remoteChapter);
        QCOMPARE(jump.here, host.session()->page());
        QCOMPARE(qRound(jump.fromPercent), qRound(localPercent));
        if (!invalid)
            QCOMPARE(qRound(jump.toPercent),
                     qRound(100.0 * jump.synced / (host.session()->pages() - 1)));
        QCOMPARE(jump.device, QString("Crosspoint X4"));
        // The server stamps the sync, so a run across midnight reads Yesterday.
        QVERIFY(jump.time.startsWith(strings::get("kosync.today_at").arg(QString()))
                || jump.time.startsWith(strings::get("kosync.yesterday_at").arg(QString())));
        // A long jump leaves the page boxes for a track.
        QVERIFY(invalid || view->scale() != reader::SyncChoiceView::Scale::Pages);
        QVERIFY(host.window()->rect().contains(dialog->card()->geometry()));
        QVERIFY(view->height() >= view->heightForWidth(view->width()));
        QCOMPARE(view->visibleRegion().boundingRect(), view->rect());
        QCOMPARE(host.session()->state().value("position").toString(), localPosition);
        const auto title = invalid ? "kosync.resume_title"
                : forward          ? "kosync.ahead_title"
                                   : "kosync.behind_title";
        QCOMPARE(dialog->findChild<QLabel *>("rrModalTitle")->text(), strings::get(title));
        if (!invalid && !forward)
            QCOMPARE(dialog->focusWidget(), dialog->secondaryButton());
        const auto capture = qEnvironmentVariable("KOBALT_KOSYNC_CHOICE_CAPTURE");
        if (!capture.isEmpty() && (decision == "keep" || forward))
            QVERIFY(host.window()->grab().save(capture
                                               + (forward ? "-forward.png" : "-backward.png")));
        if (decision == "dismiss")
            dialog->dismissed();
        else if (decision == "keep")
            dialog->secondaryButton()->click();
        else
            dialog->primaryButton()->click();
        QTRY_VERIFY(!widget || !widget->isVisible());
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        if (decision == "keep") {
            sync->click();
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 12000);
        } else if (forward) {
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.resumed"), 12000);
            QCOMPARE(host.session()->state().value("position").toString(), remotePosition);
        } else if (invalid) {
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.position_error"), 12000);
            QCOMPARE(host.session()->state().value("position").toString(), localPosition);
        }
        host.window()->close();
        QTRY_VERIFY(host.idle());
        completed = false;
        kosync::Progress received;
        other.pull(document, [&](kosync::Progress value, QString error) {
            received = value;
            failure = error;
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 12000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(received.position, decision == "keep" ? localPosition : remotePosition);
    }

    std::unique_ptr<QTemporaryDir> directory;
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    NativeTestApplication app(argc, argv);
    if (QGuiApplication::platformName() != "minimal")
        return 2;
    app.setStyle("Fusion");
    if (!qEnvironmentVariableIsEmpty("KOBALT_KOSYNC_TEST_CA")) {
        auto ssl = QSslConfiguration::defaultConfiguration();
        auto authorities = ssl.caCertificates();
        const auto certificates =
                QSslCertificate::fromPath(qEnvironmentVariable("KOBALT_KOSYNC_TEST_CA"));
        if (certificates.isEmpty())
            return 2;
        authorities.append(certificates);
        ssl.setCaCertificates(authorities);
        QSslConfiguration::setDefaultConfiguration(ssl);
    }
    app.setQuitOnLastWindowClosed(false);
    auto &assets = test::readerAssets();
    if (!assets.available() || !ui::initializeInterfaceFonts(assets))
        return 2;
    for (const auto &path : bundledFontFiles(assets))
        if (QFontDatabase::addApplicationFont(path) < 0)
            return 2;
    testFonts = test::readerFonts();
    testPlaceholder = test::readerPlaceholder();
    ui::setDensity(300);
    readingPpi.store(300);
    app.setFont(ui::interfaceFont());
    QTemporaryDir books(QDir::tempPath() + "/kobalt-sleep-books-XXXXXX");
    if (!books.isValid() || !prepareSleepBooks(books.path()))
        return 2;
    ReaderLifecycleTests tests;
    QTimer::singleShot(0, &app, [&] {
        // The test inspects saved data between events; the reader's own work
        // runs inside event delivery.
        mainthread::checkEventsOnly(true);
        app.exit(QTest::qExec(&tests, argc, argv));
    });
    return app.exec();
}

#include "reader_lifecycle.moc"
