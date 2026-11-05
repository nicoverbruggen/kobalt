#include <QVariant>
#include <QSqlQuery>
#include "storage/annotation_database.h"
#include "storage/record_writer.h"
#include "storage/highlight_store.h"
#include "storage/sleep_snapshot.h"
#include "platforms/kobo/common/last_read_book.h"
#include "platforms/kobo/common/browser_handoff.h"
#include "support/paint_regions.h"
#include <QElapsedTimer>
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "app/reader_interface.h"
#include "app/reader_window.h"
#include "ui/controls/page_scrubber.h"
#include "ui/dialogs/modals.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/scale.h"
#include "app/reader/views/reading_page.h"
#include "app/reader/views/kosync_notice.h"
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
#include "sync/kosync.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include <QLocale>
#include "engine/crengine/engine_activity.h"
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
#include "support/strings.h"

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
#ifndef KOBALT_NO_SENSORS
#include <QOrientationSensor>
#endif
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#ifndef KOBALT_NO_SENSORS
#include <QSensorBackend>
#include <QSensorManager>
#endif
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSlider>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTime>
#include <QUrl>
#include <QtEndian>
#include <atomic>
#include <zlib.h>

#ifndef KOBALT_NO_SENSORS
namespace kobalt::test {
class Backend : public QSensorBackend {
public:
    QOrientationReading reading;
    quint64 timestamp = 0;
    explicit Backend(QSensor *sensor) : QSensorBackend(sensor) {
        setReading(&reading);
    }
    void start() override {}
    void stop() override {}
    void sample(QOrientationReading::Orientation value) {
        reading.setOrientation(value);
        reading.setTimestamp(++timestamp);
        newReadingAvailable();
    }
};
class Factory : public QSensorBackendFactory {
public:
    Backend *backend = nullptr;
    QSensorBackend *createBackend(QSensor *sensor) override {
        return backend = new Backend(sensor);
    }
};
struct SensorRegistration {
    explicit SensorRegistration(const QByteArray &id, Factory *factory) : id(id) {
        QSensorManager::registerBackend(QOrientationSensor::sensorType, id, factory);
    }
    ~SensorRegistration() {
        QSensorManager::unregisterBackend(QOrientationSensor::sensorType, id);
    }
    SensorRegistration(const SensorRegistration &) = delete;
    SensorRegistration &operator=(const SensorRegistration &) = delete;
    const QByteArray id;
};
}

#endif

using namespace kobalt;
using test::PaintRegions;

namespace {
struct SyncNetworkState {
    bool connected = false, interactive = false;
    int requests = 0, cancellations = 0, releases = 0;
    std::function<void(bool)> completed;
};
class TestSyncNetwork final : public SyncNetwork {
public:
    explicit TestSyncNetwork(std::shared_ptr<SyncNetworkState> state) : state(std::move(state)) {}
    bool connected() const override { return state->connected; }
    void prepare(bool interactive, std::function<void(bool)> done) override {
        ++state->requests;
        state->interactive = interactive;
        state->completed = std::move(done);
    }
    void cancel() override {
        if (state->completed) ++state->cancellations;
        state->completed = {};
    }
    void release() override { ++state->releases; }
private:
    std::shared_ptr<SyncNetworkState> state;
};
// Answers the reader's KOSync requests without a server: no remote progress
// for a GET, and the uploaded document for a PUT. While `hold` is set, new
// replies wait in `held` until the test finishes them.
struct KosyncHttpState {
    int requests = 0;
    bool hold = false;
    QList<QPointer<test::KosyncReply>> held;
};
class KosyncHttp final : public QNetworkAccessManager {
public:
    explicit KosyncHttp(std::shared_ptr<KosyncHttpState> state) : state(std::move(state)) {}
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *data) override {
        ++state->requests;
        QByteArray body = "{}";
        if (operation == PutOperation) {
            const auto upload = QJsonDocument::fromJson(data ? data->readAll() : QByteArray()).object();
            body = QJsonDocument(QJsonObject{{"document", upload.value("document")}, {"timestamp", 1}})
                       .toJson(QJsonDocument::Compact);
        }
        auto *reply = new test::KosyncReply(request, body, 200, QNetworkReply::NoError, this, state->hold);
        if (state->hold) state->held.append(reply);
        return reply;
    }
private:
    std::shared_ptr<KosyncHttpState> state;
};
// Observe queued reply delivery without delaying the worker or sleeping.
class NativeTestApplication final : public QApplication {
public:
    using QApplication::QApplication;
    std::function<void(QObject *, QEvent *)> beforeEvent, afterEvent;
    bool notify(QObject *object, QEvent *event) override {
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
std::atomic_bool delayUsbEncoding{false}, releaseUsbEncoding{false}, usbEncodingReturned{false};
// A delayed encoding holds the engine worker until the test releases it, so
// the mapping outlasts the reader's deadline without a fixed sleep. The bound
// keeps a failed test from holding the worker for good.
QString usbPosition(const EngineDocument &model, QString *error) {
    if (delayUsbEncoding) {
        QElapsedTimer held;
        held.start();
        while (!releaseUsbEncoding && held.elapsed() < 10000)
            QThread::msleep(1);
        usbEncodingReturned = true;
    }
    return nickel::positionCodec().encode(model, error);
}

bool prepareSleepBooks(const QString &directory) {
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
    const QByteArray text = QByteArray("Comment\0", 8) + QByteArray("cover fixture ").repeated(12000);
    const QByteArray payload = "tEXt" + text;
    QByteArray chunk(4, '\0');
    qToBigEndian<quint32>(text.size(), chunk.data());
    chunk += payload;
    char checksum[4];
    qToBigEndian<quint32>(crc32(0, reinterpret_cast<const Bytef *>(payload.constData()), payload.size()), checksum);
    chunk.append(checksum, 4);
    png.insert(33, chunk);
    files["OEBPS/colors.png"] = png;
    files["OEBPS/book.opf"].replace("</metadata>", "<meta name=\"cover\" content=\"colors\"/></metadata>");
    files["OEBPS/book.opf"].replace("</manifest>", "<item id=\"colors\" href=\"colors.png\" media-type=\"image/png\"/></manifest>");
    return test::writeBook(directory, "deferred-cover.epub", files);
}

storage::Paths testPaths() {
    storage::Paths paths{profile};
    paths.stateReport = profile + "/state.json";
    return paths;
}
QString fixture() {
    return QStringLiteral(KOBALT_TEST_BOOK);
}

// Compose real publication, session, book and window objects. Keeping the session
// handle here permits deterministic worker cancellation and failed-apply tests
// without exposing the reader's private implementation or copying its controller.
class ReaderFixture final : public QObject {
    Q_OBJECT
public:
    ReaderFixture(storage::Paths paths, EngineFontInputs fonts, QString placeholder)
        : paths(std::move(paths)), fonts(std::move(fonts)), placeholder(std::move(placeholder)),
          orientation(nullptr, [](Qt::ScreenOrientation) {}) {}
    ~ReaderFixture() override {
        shutdown();
        if (!QTest::qWaitFor([this] { return idle(); }, 10000))
            qFatal("Reader lifecycle fixture did not drain");
    }
    ReaderWindow *window() const {
        return window_;
    }
    ReaderSession *session() const {
        return session_;
    }
    ReadingBook *book() const {
        return book_;
    }
    bool idle() const {
        return !active;
    }
    ReaderOrientation *rotation = nullptr;
    std::shared_ptr<test::HostProgress> progress;
    PositionCodec positions;
    ReadingHistory::UtcClock utc;
    std::function<void(ReaderHostServices &)> configureServices;
    void open(const QString &path, QSize size = nativeSize) {
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
        const auto preferences = appearance::readerPreferences(storage::readJson(paths.preferences()), saved);
        saved["dark_mode"] = preferences.value("dark_mode").toBool();
        saved["bold_reading"] = preferences.value("bold_reading").toString("off");
        for (const auto *key : appearance::normalizationKeys)
            saved[key] = appearance::normalizationSettings(preferences).value(key);
        (rotation ? rotation : &orientation)->configure(preferences);
        opening = std::make_unique<ReaderSession>(fonts, placeholder);
        session_ = opening.get();
        OpenRequest request{identity, path, readerInterface().readingViewport(size, preferences), saved,
                            {}, paths.layoutCache(), lease, source->resources(lease)};
        request.appearanceDefaults = appearance::bookDefaults(preferences);
        request.positions = positions;
        session_->open(std::move(request), [this, identity, statePath, size](OpeningResult outcome) {
            if (closing)
                return;
            if (!outcome.ready()) {
                emit failed("Cannot prepare publication");
                shutdown();
                return;
            }
            const auto hash = session_->sourceHash();
            const auto baseline = progress ? progress->current : ReadingProgress{};
            auto book = std::make_unique<ReadingBook>(std::move(opening), statePath, paths, identity, hash,
                                                      progress ? test::hostReadingBridge(progress) : nullptr,
                                                      baseline, baseline, QString(), utc);
            book_ = book.get();
            auto services = desktop::readerServices();
            services.desktopUi = false;
            services.suspended = [] { return false; };
            services.clock = [] { return QStringLiteral("09:00"); };
            services.openBrowser = [](const QUrl &, QWidget *) { return false; };
            services.inputMilliseconds = test::inputMilliseconds;
            if (configureServices)
                configureServices(services);
            window_ = createReaderWindow(std::move(book), rotation ? rotation : &orientation, std::move(services));
            window_->setAttribute(Qt::WA_DeleteOnClose, false);
            window_->setWindowFlags(Qt::Widget);
            window_->setWindowModality(Qt::NonModal);
            window_->resize(size);
            window_->finished = [this](std::unique_ptr<ReaderSession>) {
                QTimer::singleShot(0, this, [this] { shutdown(); });
            };
            emit opened();
        });
    }
    void shutdown() {
        if (!active || closing)
            return;
        closing = true;
        // Release injected save failures before fixture cleanup. The real USB
        // barrier deliberately refuses to dispose a reader with failed saves.
        if (progress) {
            progress->failWrite = progress->writeBlocked = progress->holdFinish = progress->failFinish = false;
            progress->completeFinish(true);
        }
        if (window_)
            window_->prepareShutdown([this] { retire(); });
        else
            retire();
    }
signals:
    void opened();
    void closed();
    void failed(QString error);
private:
    void retire() {
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
struct CloseHost {
    ReaderFixture &host;
    ~CloseHost() {
        host.shutdown();
        if (!QTest::qWaitFor([this] { return host.idle(); }, 10000))
            qFatal("Reader lifecycle test did not drain");
    }
};

// The reader's armed timers apart from its minute clock: repeating timers and
// single-shot timers due within a minute. The follow-up work after a turn or
// an opening uses such timers and ends on its own. A test that proves that
// nothing more happens waits for this list to empty instead of for a time.
QStringList activeReaderTimers(ReaderWindow *window) {
    QStringList active;
    for (auto *timer : window->findChildren<QTimer *>())
        if (timer->isActive() && timer->objectName() != "rrClockTimer" &&
            (timer->interval() < 60000 || !timer->isSingleShot()))
            active.append(QString("%1 (%2 ms%3)")
                              .arg(timer->objectName().isEmpty() ? QString("unnamed") : timer->objectName())
                              .arg(timer->interval())
                              .arg(timer->isSingleShot() ? ", single shot" : ""));
    return active;
}

// Runs the reader's armed sync deadline now instead of waiting for it.
// Returns false when no single-shot deadline with this interval is armed: 10 s
// for a sync, 2 s for the final sync at close or sleep.
bool runKosyncDeadline(ReaderWindow *window, int interval = 10000) {
    auto *deadline = window->findChild<QTimer *>("rrKosyncDeadline");
    if (!deadline || !deadline->isActive() || !deadline->isSingleShot() || deadline->interval() != interval)
        return false;
    deadline->stop();
    QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
    return true;
}

// The position-mapping deadline of the reader's only shutdown or USB request:
// a single-shot 2 s timer. Returns nullptr when there is not exactly one.
QTimer *transitionMappingDeadline(ReaderWindow *window) {
    const auto deadlines = window->findChildren<QTimer *>("rrTransitionMappingDeadline");
    if (deadlines.size() != 1 || !deadlines.last()->isSingleShot() || deadlines.last()->interval() != 2000)
        return nullptr;
    return deadlines.last();
}

class Canvas final : public standalone::WidgetCanvas {
public:
    Canvas(ReaderWindow *reader, QSize size) : WidgetCanvas(nullptr) {
        reader->setFixedSize(size);
        setContent(reader);
        setSceneRect(QRectF(QPointF(), size));
    }
    QWidget *surface() const { return content; }
protected:
    void resizeEvent(QResizeEvent *event) override {
        WidgetCanvas::resizeEvent(event);
        fitInView(sceneRect(), Qt::KeepAspectRatio);
    }
};
bool clickAt(Canvas &canvas, QPoint point) {
    QCoreApplication::processEvents();
    const auto local = canvas.mapFromScene(point);
    if (!canvas.viewport()->rect().contains(local))
        return false;
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, local);
    return true;
}
bool click(Canvas &canvas, QWidget *widget) {
    QCoreApplication::processEvents();
    if (!widget || !widget->isVisible() || !widget->isEnabled())
        return false;
    const auto point = widget->mapTo(canvas.surface(), widget->rect().center());
    const auto *receiver = canvas.surface()->childAt(point);
    if (receiver != widget && (!receiver || !widget->isAncestorOf(receiver)))
        return false;
    return clickAt(canvas, point);
}

QPushButton *sizeStep(ReaderFixture &host) {
    auto *slider = host.window()->findChild<QSlider *>("rrTextSize");
    if (!slider)
        return nullptr;
    return host.window()->findChild<QPushButton *>(slider->value() == slider->maximum() ? "rrSmaller" : "rrLarger");
}
QJsonObject inspectReader(ReaderFixture &host) {
    host.window()->checkpoint();
    return storage::readJson(testPaths().stateReport);
}
int sliderX(const QSlider *slider, int value) {
    const int inset = ui::metrics::control::sliderInset;
    return inset + QStyle::sliderPositionFromValue(slider->minimum(), slider->maximum(), value,
                                                   qMax(1, slider->width() - 2 * inset));
}
// One continuous press, move and release through the window, as a finger
// drag arrives on the device. Events go through Qt's grab and enabled checks.
void drag(Canvas &canvas, QSlider *slider, int from, int to, const std::function<void()> &moved = {}) {
    QCoreApplication::processEvents();
    const auto at = [&](int value) {
        return canvas.mapFromScene(slider->mapTo(canvas.surface(), QPoint(sliderX(slider, value), slider->height() / 2)));
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
                const std::function<void()> &moved = {}) {
    const int inset = ui::metrics::control::sliderInset;
    const auto at = [&](qreal fraction) {
        const QPoint local(inset + qRound(fraction * (slider->width() - 2 * inset)), slider->height() / 2);
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

}

class ReaderLifecycleTests final : public QObject {
    Q_OBJECT
private slots:
    void init() {
        directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/kobalt-lifecycle-XXXXXX");
        QVERIFY(directory->isValid());
        profile = directory->path();
    }
    void cleanup() {
        engineActivity().pause(false);
        directory.reset();
    }
    // Checks that an event delivered during reader destruction no longer reaches the reader's application filter.
    void teardownRemovesApplicationFilter() {
        class Probe : public QWidget {
        public:
            using QWidget::QWidget;
            bool received = false;
            bool event(QEvent *event) override {
                if (event->type() == QEvent::HoverEnter) {
                    received = true;
                    return true;
                }
                return QWidget::event(event);
            }
        };
        bool delivered = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QVERIFY2(delivered, "The reader's application filter still consumed events during teardown");
    }
    // Checks that shutdown ends at the position-conversion deadline with a stalled engine, keeps the local position and writes no native one.
    void shutdownFinalSaveTimesOut() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.progress = std::make_shared<test::HostProgress>();
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        QVERIFY(!storage::readBookState(testPaths(), identity).object.value("position").toString().isEmpty());
    }
    // Checks that a clock tick repaints only the changed reading strips and does not count as a page turn.
    void clockUpdatesOnlyReadingStrips_data() {
        QTest::addColumn<bool>("custom");
        QTest::addColumn<bool>("header");
        QTest::newRow("default") << false << true;
        QTest::newRow("custom-right") << true << true;
        QTest::newRow("header-off") << false << false;
    }
    void clockUpdatesOnlyReadingStrips() {
        QFETCH(bool, custom);
        QFETCH(bool, header);
        QJsonObject preferences{{"header_footer", custom ? "custom" : "default"}};
        if (custom)
            preferences["header_footer_custom"] = QJsonObject{{"top_left", "nothing"}, {"top_right", "time"}};
        if (!header)
            preferences["header_footer_text"] = QJsonObject{{"header_size", "off"}};
        QVERIFY(storage::writePreferences(testPaths().preferences(), preferences));
        QString time = "09:00";
        int timeOfDay = 9 * 3600000 + 30000;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [&](ReaderHostServices &services) {
            services.clock = [&] { return time; };
            services.millisecondsOfDay = [&] { return timeOfDay; };
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
        timeOfDay = 9 * 3600000 + 60050;
        QVERIFY(QMetaObject::invokeMethod(clock, "timeout", Qt::DirectConnection));
        QVERIFY(!host.window()->property("reader_page_turn").toBool());
        QVERIFY(page->headerText().contains(time));
        if (header)
            QTRY_VERIFY(!paints.painted.isEmpty());
        else
            QCoreApplication::processEvents();
        QVERIFY(!paints.painted.intersects(
            QRect(0, page->topHeight(), page->width(), page->height() - page->topHeight() - page->bottomHeight())));
        QCOMPARE(page->image.cacheKey(), body);
        QCOMPARE(host.session()->state().value("position"), position);
        paints.painted = {};
        QVERIFY(QMetaObject::invokeMethod(clock, "timeout", Qt::DirectConnection));
        QCoreApplication::processEvents();
        QVERIFY(paints.painted.isEmpty());
    }
    // Checks that hiding the reader stops its clock and that showing it restarts the clock without changing the page.
    void clockStopsWhileReaderIsHidden_data() {
        QTest::addColumn<bool>("browser");
        QTest::newRow("ordinary-hide") << false;
        QTest::newRow("browser-handoff") << true;
    }
    void clockStopsWhileReaderIsHidden() {
        QFETCH(bool, browser);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QTRY_VERIFY(host.window()->isVisible());
        QTRY_VERIFY(!host.session()->busy() && host.session()->resourceStats().value("pending_images").toInt() == 0);
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
    void backgroundCheckpointStopsBeforeOtherCallbacks() {
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
        bool suspended = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        };
        // Android registers its checkpoint callback before constructing the
        // reader. It can be the last callback to finish before process death.
        bool checkpointSawPausedSession = false;
        connect(qApp, &QGuiApplication::applicationStateChanged, &host, [&](Qt::ApplicationState state) {
            if (state != Qt::ApplicationInactive || !host.window()) return;
            host.window()->checkpoint();
            storage::profileTransaction(profile, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                if (!query.exec("SELECT COUNT(*) FROM reading_sessions WHERE status='active'") || !query.next()) return false;
                checkpointSawPausedSession = query.value(0).toInt() == 1 && !host.book()->readingActive();
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
            if (!query.exec("SELECT COUNT(*) FROM reading_sessions WHERE status='active'") || !query.next()) return false;
            active = query.value(0).toInt() == 1;
            return true;
        }));
        QVERIFY(active);
        suspended = true;
        qApp->applicationStateChanged(Qt::ApplicationInactive);
        QVERIFY2(checkpointSawPausedSession, "Host checkpoint left the session running until a later callback");
    }
    // Checks that after a failed background save the paused reading time stops growing and is saved into the same session row on resume.
    void backgroundWriteFailureRetriesWithoutCountingPause() {
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
        bool suspended = false;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [&](ReaderHostServices &services) { services.suspended = [&] { return suspended; }; };
        const auto value = [&](const QString &statement) {
            QVariant result;
            const bool ok = storage::profileTransaction(profile, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                if (!query.exec(statement)) return false;
                if (query.next()) result = query.value(0);
                return true;
            });
            if (!ok) QTest::qFail("Background retry fixture SQL", __FILE__, __LINE__);
            return result;
        };
        connect(qApp, &QGuiApplication::applicationStateChanged, &host, [&](Qt::ApplicationState state) {
            if (state == Qt::ApplicationInactive && host.window()) host.window()->checkpoint();
        });
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(host.window()->readyForPresentation());
        host.window()->checkpoint();
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 1);
        value("CREATE TRIGGER fail_background BEFORE UPDATE ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
        suspended = true;
        qApp->applicationStateChanged(Qt::ApplicationInactive);
        const auto paused = inspectReader(host).value("session_reading_ms").toVariant().toLongLong();
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
    void standaloneReadingHistory_data() {
        QTest::addColumn<bool>("native");
        QTest::newRow("standalone") << false;
        QTest::newRow("native") << true;
    }
    void standaloneReadingHistory() {
        QFETCH(bool, native);
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
        bool suspended = false;
        qint64 utc = 100000;
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.utc = [&] { return utc; };
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        };
        const auto value = [&](const QString &statement) {
            QVariant result;
            const bool ok = storage::profileTransaction(profile, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                if (!query.exec(statement) || !query.next()) return false;
                result = query.value(0);
                return true;
            });
            if (!ok) QTest::qFail("Cannot inspect history", __FILE__, __LINE__);
            return result;
        };
        if (native) {
            host.progress = std::make_shared<test::HostProgress>();
            const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
            host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 1);
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
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 1);
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
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='completed'").toInt(), 2);
        host.window()->show();
        host.window()->checkpoint();
        QTRY_COMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 1);
        host.shutdown();
        QTRY_VERIFY(host.idle());
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='completed'").toInt(), 3);
    }
    void uneditedPanelTransitions_data() {
        QTest::addColumn<QString>("start");
        QTest::newRow("text") << "rrNavText";
        QTest::newRow("layout") << "rrNavLayout";
        QTest::newRow("stats") << "rrNavStats";
        QTest::newRow("chapters") << "rrNavChapters";
    }
    void uneditedPanelTransitions() {
        QFETCH(QString, start);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
            const int previousPage = host.session()->page();
            const bool rtl = host.session()->state().value("page_progression_rtl").toBool();
            QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(),
                QPoint(rtl ? page->width() / 6 : page->width() * 5 / 6, page->height() / 2))));
            QTRY_VERIFY(!host.session()->busy() && host.session()->page() != previousPage);
            QCoreApplication::processEvents();
            QCOMPARE(position->text(), hiddenText);
        }
        const auto image = page->image.cacheKey();
        auto state = host.session()->state();
        state.remove("cached_pages");
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(toolbar->isVisible());

        const QStringList tabs = {"rrNavText", "rrNavLayout", "rrNavLight", "rrNavStats", "rrNavChapters"};
        const QStringList panels = {"rrMenu", "rrMenu", "rrLightPanel", "rrChapterGraph", "rrChoices"};
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
            auto current = host.session()->state();
            current.remove("cached_pages");
            QCOMPARE(current, state);
            QCOMPARE(page->image.cacheKey(), image);
            QVERIFY(toolbar->isVisible());
            QVERIFY2(startButton->isVisible() && startButton->isEnabled(), qPrintable(start));
            QVERIFY(click(canvas, startButton));
            QVERIFY2(!host.session()->busy(), qPrintable(start));
            QTRY_VERIFY(startPanel->isVisible());
            QVERIFY(!host.session()->busy());
            if (start == "rrNavStats") {
                QCOMPARE(host.window()->findChild<QLabel *>("rrGraphChapterPosition")->text(),
                         strings::get("stats.page_of").arg(host.session()->chapterPage())
                             .arg(host.session()->chapterPages()));
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

    // Checks that Light replaces the default menu view, closing Light restores it, and other tabs or dismissal do not reopen it.
    void lightReplacesDefaultMenu_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("menuView");
        QTest::addColumn<QString>("action");
        for (bool dark : {false, true})
            for (const auto *view : {"none", "seek", "stats"})
                for (const auto *action : {"tab", "outside", "Text", "Layout", "Chapters", "Settings", "Seek", "Stats", "dismiss"}) {
                    if ((QString(action) == "Seek" && QString(view) == "seek") ||
                        (QString(action) == "Stats" && QString(view) == "stats"))
                        continue;
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(view, dark ? "dark" : "light", action)))
                        << dark << QString::fromLatin1(view) << QString::fromLatin1(action);
                }
    }
    void lightReplacesDefaultMenu() {
        QFETCH(bool, dark);
        QFETCH(QString, menuView);
        QFETCH(QString, action);
        class Light final : public LightControls {
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
            void finishGesture(bool) override {}
            int brightness() const override { return 50; }
            bool gesturing() const override { return false; }
        };
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), {{"dark_mode", dark}, {"default_menu_view", menuView}}));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        auto *seek = dynamic_cast<ui::PageScrubber *>(host.window()->findChild<QWidget *>("rrPageScrubber"));
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
        class DefaultShows final : public QObject {
        public:
            int count = 0;
            bool eventFilter(QObject *, QEvent *event) override {
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
        else if (menuView == "seek")
            // The slider tracks Navigate; the parent also hosts floating image actions.
            seek->slider()->installEventFilter(&defaultShows);
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
            const auto name = action == "Settings" ? QString("rrSettings") : QString("rrNav%1").arg(action);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(name)));
        }
        QTRY_VERIFY(!light->isVisible());
        QVERIFY(!tab->isChecked());
        const bool restoring = action == "tab" || action == "outside";
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
            if (action == "dismiss") {
                QVERIFY(!toolbar->isVisible());
                QVERIFY(!host.window()->findChild<QWidget *>("rrBottomNavigation")->isVisible());
                QVERIFY(clickAt(canvas, paper));
                QTRY_VERIFY(toolbar->isVisible());
                QTRY_COMPARE(stats->isVisible(), menuView == "stats");
                QTRY_COMPARE(seek->isVisible() && seek->barBounds().height() > 0, menuView == "seek");
                QCOMPARE(defaultShows.count, menuView == "none" ? 0 : 1);
            } else {
                const auto panel = action == "Chapters" ? "rrChoices" : action == "Settings" ? "rrGeneralSettings" : "rrMenu";
                if (action == "Text" || action == "Layout" || action == "Chapters" || action == "Settings")
                    QTRY_VERIFY(host.window()->findChild<QWidget *>(panel)->isVisible());
                QVERIFY(toolbar->isVisible());
            }
        }
        QCOMPARE(host.session()->state().value("position"), position);
        QCOMPARE(page->image.cacheKey(), image);
        QCOMPARE(storage::readJson(paths.preferences()).value("default_menu_view").toString(), menuView);
    }

    void appearanceApplyOnNavigation_data() {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("panel");
        QTest::newRow("stats") << "rrNavStats" << "rrChapterGraph";
        QTest::newRow("chapters") << "rrNavChapters" << "rrChoices";
        QTest::newRow("search") << "rrSearchOpen" << "rrSearch";
        QTest::newRow("bookmarks") << "rrBookmarks" << "rrBookmarkList";
        QTest::newRow("seek") << "rrNavSeek" << "rrPageScrubber";
    }
    void appearanceApplyOnNavigation() {
        QFETCH(QString, action);
        QFETCH(QString, panel);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        const auto textDraft = inspectReader(host)["preview_settings"].toObject();
        QVERIFY(textDraft["font_pixels"] != original["font_pixels"]);
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
        auto *layout = host.window()->findChild<QPushButton *>("rrNavLayout");
        QVERIFY(layout);
        QVERIFY(click(canvas, layout));
        auto *wider = host.window()->findChild<QPushButton *>("rrWider");
        QVERIFY(wider);
        QTRY_VERIFY(wider->isVisible() && wider->isEnabled());
        for (const auto *name : {"rrMargins", "rrVerticalMargins"}) {
            auto *slider = host.window()->findChild<QSlider *>(name);
            QVERIFY(slider);
            QCOMPARE(slider->minimum(), 0);
            QCOMPARE(slider->maximum(), 120);
            QCOMPARE(slider->singleStep(), 3);
        }
        QCOMPARE(inspectReader(host)["preview_settings"].toObject()["font_pixels"], textDraft["font_pixels"]);
        QVERIFY(click(canvas, wider));
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        auto *verticalMarginsMore = host.window()->findChild<QPushButton *>("rrVerticalMarginsMore");
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

    void appearanceRelayoutPreservesPassage() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto paths = testPaths();
        paths.directory = directory.path();
        paths.startup = paths.directory + "/startup-session";
        paths.active = paths.directory + "/active-session.json";
        paths.stateReport = paths.directory + "/state.json";
        paths.sessionReport = paths.directory + "/session.json";
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
            widget->render(&image, QPoint(), QRegion(QRect(0, 0, 32, 32)), QWidget::DrawWindowBackground);
            return image;
        };
        QCOMPARE(backdrop(notice), backdrop(&reference));
        reference.setDimmed(false);
        QVERIFY(backdrop(notice) != backdrop(&reference));
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        const auto first = host.session()->state().value("position").toString();
        const bool rtl = host.session()->state().value("page_progression_rtl").toBool();
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), QPoint(rtl ? page->width() / 6 : page->width() * 5 / 6,
                                                                     page->height() / 2))));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("position").toString() != first, 10000);
        QVERIFY(host.session()->page() > 0);
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
            QTRY_VERIFY_WITH_TIMEOUT(option->isEnabled() && !host.session()->busy() &&
                                         host.session()->state().value(key).toBool() == !before,
                                     30000);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
            QTRY_VERIFY_WITH_TIMEOUT(!host.window()->findChild<QWidget *>("rrGeneralSettings")->isVisible() &&
                                         !host.session()->busy() &&
                                         host.session()->state().value(key).toBool() == !before,
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
                !host.session()->busy() && host.session()->state().value("bold_reading").toString() == "off", 30000);
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
            QTRY_VERIFY_WITH_TIMEOUT(
                graph->isVisible() && !host.session()->busy() && !inspect()["preview_ready"].toBool(), 30000);
            for (const auto *key : {"font_face", "font_pixels", "weight_steps", "margin_pixels"})
                QCOMPARE(host.session()->state().value(key), expected.value(key));
            preserved();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavStats")));
        };
        openText();
        const auto oldFace = host.session()->state().value("font_face");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
        const QString targetFace =
            oldFace.toString() == "Libron" ? kobalt::test::alternateReadingFace() : QString("Libron");
        const QString targetLabel =
            oldFace.toString() == "Libron" ? kobalt::test::alternateReadingLabel() : QString("Libron");
        QPushButton *choice = nullptr;
        for (auto *label : host.window()->findChildren<QLabel *>())
            if (label->objectName().endsWith("Title") && label->text() == targetLabel)
                choice = qobject_cast<QPushButton *>(label->parentWidget());
        QVERIFY(choice);
        QVERIFY(choice->isVisible());
        QVERIFY(click(canvas, choice));
        QTRY_COMPARE_WITH_TIMEOUT(inspect()["preview_settings"].toObject().value("font_face").toString(), targetFace,
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
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(
                                  weight->value() == weight->maximum() ? "rrLighter" : "rrHeavier")));
        apply();
        QVERIFY(host.session()->state().value("weight_steps") != oldWeight);
        menu();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLayout")));
        QVERIFY(!host.session()->busy());
        auto *margin = host.window()->findChild<QSlider *>("rrMargins");
        QVERIFY(margin);
        const auto oldMargin = host.session()->state().value("margin_pixels");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(
                                  margin->value() == margin->maximum() ? "rrNarrower" : "rrWider")));
        apply();
        QVERIFY(host.session()->state().value("margin_pixels") != oldMargin);
        menu();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *bionic = host.window()->findChild<QCheckBox *>("rrBionicReading");
        QVERIFY(bionic && !bionic->isChecked());
        QVERIFY(click(canvas, bionic));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("bold_reading").toString() == "bold", 30000);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
        preserved();
        QCOMPARE(host.session()->state().value("bold_reading").toString(), QString("bold"));
        settingsToggle("rrDarkMode", "dark_mode");
        settingsToggle("rrDarkMode", "dark_mode");
        for (const auto &size : {QSize(800, 1200), QSize(1400, 1000)}) {
            host.window()->setFixedSize(size);
            QTRY_COMPARE_WITH_TIMEOUT(inspect().value("viewport_width").toInt(), size.width(), 10000);
            preserved();
        }
    }

    void overflowClosesCurrentPanel_data() {
        QTest::addColumn<QString>("target");
        for (const auto *target : {"text", "text-after-fonts", "fonts", "layout", "stats"})
            QTest::newRow(target) << QString::fromLatin1(target);
    }
    void overflowClosesCurrentPanel() {
        QFETCH(QString, target);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        const auto action = target == "layout"  ? "rrNavLayout"
                            : target == "light" ? "rrNavLight"
                            : target == "stats" ? "rrNavStats"
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
        for (const auto *name : {"rrMenu", "rrChoices", "rrLightPanel", "rrChapterGraph", "rrBookmarkList", "rrSearch",
                                 "rrControlsBottom"}) {
            auto *panel = host.window()->findChild<QWidget *>(name);
            QVERIFY(panel);
            QVERIFY2(!panel->isVisible(), name);
        }
        QVERIFY(!host.window()->findChild<QPushButton *>("rrNavText")->isChecked());
        QVERIFY(!host.window()->findChild<QPushButton *>("rrNavLayout")->isChecked());
    }

    void appearanceSettingsRetainsDraft() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
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
        struct ResumeWorker {
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
        QVERIFY(click(canvas, text));
        QTRY_VERIFY_WITH_TIMEOUT(larger->isVisible() && larger->isEnabled() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
    }

    void appearanceWriteFailureRetainsDraft_data() {
        QTest::addColumn<bool>("exit");
        QTest::newRow("navigation-retry") << false;
        QTest::newRow("exit-retry") << true;
    }
    void appearanceWriteFailureRetainsDraft() {
        QFETCH(bool, exit);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
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
        struct RestoreFile {
            QString path;
            bool existed;
            QByteArray bytes;
            ~RestoreFile() {
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
        } restore{path, existed, savedBytes};
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
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
        if (exit) {
            host.window()->close();
            QVERIFY(!host.idle());
            QVERIFY(host.window()->isEnabled());
            QVERIFY(inspectReader(host)["preview_ready"].toBool());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
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
            QTRY_VERIFY_WITH_TIMEOUT(
                host.window()->findChild<QWidget *>("rrSearch")->isVisible() && !host.session()->busy(), 10000);
        }
        QCOMPARE(host.session()->state()["margin_pixels"], draft["margin_pixels"]);
    }

    void appearanceSleepRetainsDraft() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
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
        QTRY_VERIFY_WITH_TIMEOUT(
            host.window()->findChild<QWidget *>("rrSearch")->isVisible() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
    }

    void appearanceFailedPreviewDismisses_data() {
        QTest::addColumn<bool>("exit");
        QTest::newRow("navigation") << false;
        QTest::newRow("exit") << true;
    }
    void appearanceFailedPreviewDismisses() {
        QFETCH(bool, exit);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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

    void appearanceApplyFailureCanExit() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        // Reject a real engine update before Reader observes apply completion.
        // Revoking a source alone cannot fail a reflow of an already cached document.
        const auto productionUpdated = host.session()->updated;
        bool rejectUpdate = true;
        struct RestoreUpdated {
            QPointer<ReaderSession> session;
            std::function<void()> callback;
            ~RestoreUpdated() {
                if (session)
                    session->updated = callback;
            }
        } restore{host.session(), productionUpdated};
        host.session()->updated = [&, productionUpdated] {
            if (rejectUpdate) {
                rejectUpdate = false;
                auto invalid = host.session()->state();
                invalid["font_pixels"] = 0;
                host.session()->apply(invalid);
            } else if (productionUpdated)
                productionUpdated();
        };
        auto *search = host.window()->findChild<QPushButton *>("rrSearchOpen");
        QVERIFY(search);
        QVERIFY(click(canvas, search));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy() && !host.session()->error().isEmpty(), 10000);
        QVERIFY(host.window()->isEnabled());
        QVERIFY(!host.window()->findChild<QWidget *>("rrSearch")->isVisible());
        host.window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
    }

    void appearanceCloseDuringPreview_data() {
        QTest::addColumn<bool>("edit");
        QTest::addColumn<bool>("shutdown");
        QTest::newRow("exit-unedited") << false << false;
        QTest::newRow("exit-reflow") << true << false;
        QTest::newRow("shutdown-unedited") << false << true;
        QTest::newRow("shutdown-reflow") << true << true;
    }
    void appearanceCloseDuringPreview() {
        QFETCH(bool, edit);
        QFETCH(bool, shutdown);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
            QVERIFY(inspectReader(host)["preview_busy"].toBool() || inspectReader(host)["preview_queued"].toBool());
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

    void appearanceSleepDuringFirstEdit() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QVERIFY(inspectReader(host)["preview_busy"].toBool() || inspectReader(host)["preview_queued"].toBool());
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        host.window()->wakeReader();
        auto *font = host.window()->findChild<QPushButton *>("rrFont");
        QVERIFY(font);
        QTRY_VERIFY(font->isVisible() && font->isEnabled());
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
    }
    // Physical Clara BW reports: a second drag right after the first did
    // nothing, and Overflow ignored taps while the preview rendered.

    void appearanceDragsDuringPreview_data() {
        QTest::addColumn<bool>("slow");
        QTest::newRow("preview-ready") << false;
        // A held engine worker stands in for the Clara BW, where the preview
        // is still opening and rendering while the finger moves.
        QTest::newRow("preview-rendering") << true;
    }
    void appearanceDragsDuringPreview() {
        QFETCH(bool, slow);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        struct Release {
            ~Release() {
                engineActivity().pause(false);
            }
        } release;
        if (slow)
            engineActivity().pause(true);
        // Drag before the preview has opened, then drag again at once.
        QStringList disabled;
        const auto watch = [&] {
            for (auto *widget :
                 {static_cast<QWidget *>(weight), static_cast<QWidget *>(size), static_cast<QWidget *>(overflow)})
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
        QTRY_VERIFY_WITH_TIMEOUT(!general->isVisible() && !host.session()->busy() &&
                                     !inspectReader(host)["appearance_panel_open"].toBool(),
                                 15000);
        QCOMPARE(host.session()->state()["weight_steps"].toInt(), thickness + 1);
        QCOMPARE(host.session()->state()["font_pixels"].toInt(), kobalt::halfPointPixels(landed));
        QVERIFY(host.session()->state()["font_pixels"] != original["font_pixels"]);
    }
    // A thickness drag repaints only its slider row while the finger moves.
    // After release, the new page and the font sample share one paint pass,
    // so the display shows them in one update.

    void pageTurnsResumeWhenReaderIsShownAfterSleep() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        // wakeReader() shows the reader full screen. Open it at the screen
        // size so page numbers stay comparable across wake. The offscreen
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
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
        host.window()->sleepReader();
        host.window()->pageButton(1);
        host.window()->pageButton(-1);
        QCOMPARE(host.session()->page(), 1);
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QVERIFY(host.window()->isVisible());
        QTRY_COMPARE(host.window()->size(), screenSize);
        test::skipPageTurnWindow();
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 2);
        test::skipPageTurnWindow();
        host.window()->pageButton(-1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
    }

    void closeWithPendingBookmark() {
        // Other tests can leave bookmarks that resolve to this page after reflow.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        storage::Paths paths{root.path()};
        paths.startup = root.path() + "/startup-session";
        paths.active = root.path() + "/active-session.json";
        paths.stateReport = root.path() + "/state.json";
        paths.sessionReport = root.path() + "/session.json";
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QVERIFY(host.session()->go(1));
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
        struct Release {
            ~Release() {
                engineActivity().pause(false);
            }
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

    void pageTurnTimingIgnoresNoopNavigation() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
            if (event.value("event") == "page-turn-input") input = event;
            if (event.value("event") == "page-turn-painted") painted = event;
        });
        struct ResetDiagnostics {
            ~ResetDiagnostics() { setDiagnosticObserver({}); pauseDiagnostics(true); }
        } resetDiagnostics;
        host.window()->pageButton(-1);
        QVERIFY(!host.session()->busy());
        // Real idle time before the input that turns: the diagnostics clocks
        // measure it and the check below must exclude it. 100 ms is well
        // above the 20 ms tolerance. The skip passes the duplicate-turn filter.
        test::skipPageTurnWindow();
        QTest::qWait(100);
        host.window()->pageButton(1);
        QTRY_VERIFY(!painted.isEmpty());
        QCOMPARE(host.session()->page(), 1);
        // Both values use monotonic clocks. Rendering speed does not affect
        // this comparison; idle time before the real input must be excluded.
        const auto inputToPaint = painted.value("monotonic_ms").toDouble() - input.value("monotonic_ms").toDouble();
        QVERIFY(painted.value("elapsed_ms").toDouble() <= inputToPaint + 20);
    }

    void pageButtonsAfterTouchUseDeliveryTime() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QJsonObject input;
        setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event") == "page-turn-input") input = event;
        });
        struct ResetDiagnostics {
            ~ResetDiagnostics() { setDiagnosticObserver({}); pauseDiagnostics(true); }
        } resetDiagnostics;
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        QVERIFY(page);
        QTest::mouseClick(page, Qt::LeftButton, Qt::NoModifier, QPoint(page->width() * 9 / 10, page->height() / 2));
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

    void pageGestureBurstIgnoresRapidTurns() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
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
    void exitThroughReaderAndReopen() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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

    // Closing Settings guards Exit against the second half of a double tap.
    // The guard judges a tap by when it was pressed, so a tap that only waited
    // while a slow close held the GUI still exits. QTest gives each event a
    // timestamp and puts a double-click interval between two clicks.
    void exitGuardUsesTapTime() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        auto *settings = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(exit && settings);
        const auto closeSettings = [&] {
            QTRY_VERIFY(exit->isVisible() && exit->isEnabled());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            QTRY_VERIFY(settings->isVisible());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
            QTRY_VERIFY(!settings->isVisible() && exit->isVisible() && exit->isEnabled());
        };
        closeSettings();
        // Pressed a few milliseconds after the close: part of a double tap.
        QTest::lastMouseTimestamp -= QTest::mouseDoubleClickInterval;
        QVERIFY(click(canvas, exit));
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 100);
        QCOMPARE(closed.count(), 0);
        QVERIFY(host.window());
        closeSettings();
        // Pressed half a second after the close, before the guard timer ends.
        QVERIFY(click(canvas, exit));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
    }

    void staleSearchReplyIgnored() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        host.session()->pageSummary(host.session()->page(), [&](QString, QString) {
            drained = true;
        });
        QTRY_VERIFY(drained);
        QCoreApplication::processEvents();
        QCOMPARE(status->text(), none);
        auto *row = host.window()->findChild<QPushButton *>("rrSearchResult0");
        QVERIFY(!row || !row->isVisible());
    }

    // Checks that a search highlight stays when an unedited panel opens and is gone after a text-size or margin change.
    void searchHighlightClearsOnReflow_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("layout");
        QTest::newRow("text-light") << false << false;
        QTest::newRow("text-dark") << true << false;
        QTest::newRow("layout-light") << false << true;
        QTest::newRow("layout-dark") << true << true;
    }
    void searchHighlightClearsOnReflow() {
        QFETCH(bool, dark);
        QFETCH(bool, layout);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        query->setText("reader");
        emit query->returnPressed();
        QTRY_COMPARE(status->text(), strings::get("search_panel.matches").arg(hits.size()));
        auto *result = host.window()->findChild<QPushButton *>("rrSearchResult0");
        QTRY_VERIFY(result && result->isVisible());
        QVERIFY(click(canvas, result));
        QTRY_VERIFY(!host.session()->busy());
        bool drained = false;
        host.session()->pageSummary(host.session()->page(), [&](QString, QString) { drained = true; });
        QTRY_VERIFY(drained);
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QVector<QRect> rectangles;
        drained = false;
        host.session()->rangeRects(hits.first().start, hits.first().end, [&](QVector<QRect> result) {
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
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() &&
                    !inspectReader(host)["preview_busy"].toBool());
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

    void searchSurvivesReflow_data() {
        QTest::addColumn<QString>("interruption");
        QTest::newRow("queued-reply") << QString("queued-reply");
        QTest::newRow("partial-results") << QString("partial-results");
        QTest::newRow("busy-submit") << QString("busy-submit");
        QTest::newRow("replaced-query") << QString("replaced-query");
        QTest::newRow("closed-query") << QString("closed-query");
    }
    void searchSurvivesReflow() {
        QFETCH(QString, interruption);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        // Drain prior worker replies before intercepting this search's delivery.
        bool drained = false;
        host.session()->pageSummary(host.session()->page(), [&](QString, QString) {
            drained = true;
        });
        QTRY_VERIFY(drained);
        QCoreApplication::processEvents();
        auto *application = static_cast<NativeTestApplication *>(qApp);
        struct RestoreSearchDispatch {
            NativeTestApplication *application;
            ~RestoreSearchDispatch() {
                application->beforeEvent = {};
                application->afterEvent = {};
                engineActivity().pause(false);
            }
        } restoreDispatch{application};
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
        } else if (interruption == "partial-results") {
            application->afterEvent = [&](QObject *object, QEvent *event) {
                if (object != qApp || event->type() != QEvent::MetaCall || interrupted ||
                    status->text() == strings::get("search_panel.searching") ||
                    status->text() == strings::get("search_panel.matches").arg(referenceCount)) {
                    return;
                }
                auto *result = host.window()->findChild<QPushButton *>("rrSearchResult0");
                if (!result || !result->isVisible()) {
                    return;
                }
                application->afterEvent = {};
                interrupted = true;
                // A restarted query must replace the already appended batch.
                host.session()->resize(resized);
            };
        } else {
            engineActivity().pause(true);
            host.session()->resize(resized);
            QVERIFY(host.session()->busy());
            interrupted = true;
        }
        query->setText("reader");
        emit query->returnPressed();
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
        if (interruption == "closed-query") {
            drained = false;
            host.session()->pageSummary(host.session()->page(), [&](QString, QString) {
                drained = true;
            });
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
            const auto expected =
                interruption == "replaced-query" ? none : strings::get("search_panel.matches").arg(referenceCount);
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), expected, 10000);
        }
        drained = false;
        host.session()->pageSummary(host.session()->page(), [&](QString, QString) {
            drained = true;
        });
        QTRY_VERIFY(drained);
        QCoreApplication::processEvents();
        QCOMPARE(status->text(), interruption == "replaced-query" || interruption == "closed-query"
                                     ? none
                                     : strings::get("search_panel.matches").arg(referenceCount));
    }

#ifndef KOBALT_NO_SENSORS
    void sleepRequestFreezesOrientationBeforeHandoff_data() {
        QTest::addColumn<bool>("handoff");
        QTest::newRow("completed-sleep") << true;
        QTest::newRow("wake-before-handoff") << false;
    }
    void sleepRequestFreezesOrientationBeforeHandoff() {
        QFETCH(bool, handoff);
        test::Factory factory;
        test::SensorRegistration registration("kobalt-sleep-test", &factory);
        QOrientationSensor sensor;
        sensor.setIdentifier("kobalt-sleep-test");
        QVERIFY(sensor.start());
        factory.backend->sample(QOrientationReading::TopUp);
        auto shown = Qt::PortraitOrientation;
        ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) {
            shown = value;
        });
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        host.rotation = &rotation;
        CloseHost cleanup{host};
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

    void closePublishesHeldOrientationBeforeBackgroundPaint() {
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
        ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) {
            shown = value;
        });
        QList<Qt::ScreenOrientation> painted;
        ReaderFixture host(storage::Paths{directory.path()}, testFonts, testPlaceholder);
        host.rotation = &rotation;
        host.configureServices = [&](ReaderHostServices &services) {
            services.presentation = [&](QWidget *) {
                auto presentation = std::make_unique<test::HostPresentation>();
                presentation->repaint = [&] {
                    painted.append(shown);
                };
                return presentation;
            };
        };
        CloseHost cleanup{host};
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
    void closeRestoresOrientationBeforeBackgroundPaint() {
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), {{"orientation", int(Qt::InvertedPortraitOrientation)}}));
        auto shown = Qt::PortraitOrientation;
        ReaderOrientation rotation(nullptr, [&](Qt::ScreenOrientation value) {
            shown = value;
        });
        QList<Qt::ScreenOrientation> painted;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.rotation = &rotation;
        host.configureServices = [&](ReaderHostServices &services) {
            services.presentation = [&](QWidget *) {
                auto presentation = std::make_unique<test::HostPresentation>();
                presentation->repaint = [&] {
                    painted.append(shown);
                };
                return presentation;
            };
        };
        CloseHost cleanup{host};
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

    void rotationOwnership() {
        QList<int> calls;
        {
            ReaderOrientation orientation(
                nullptr,
                [&](Qt::ScreenOrientation value) {
                    calls.append(int(value));
                },
                [&](bool owned) {
                    calls.append(owned ? -1 : -2);
                });
            orientation.configure(QJsonObject{{"orientation", int(Qt::LandscapeOrientation)}});
            QVERIFY(calls.isEmpty());
            orientation.start(Qt::PortraitOrientation);
            QCOMPARE(calls, QList<int>({-1, int(Qt::LandscapeOrientation)}));
            orientation.start(Qt::PortraitOrientation);
            QCOMPARE(calls.size(), 2);
            orientation.stop();
            QCOMPARE(calls, QList<int>({-1, int(Qt::LandscapeOrientation), int(Qt::PortraitOrientation), -2}));
            orientation.stop();
            QCOMPARE(calls.size(), 4);
            orientation.start(Qt::PortraitOrientation);
        }
        QCOMPARE(calls.mid(4), QList<int>({-1, int(Qt::LandscapeOrientation), int(Qt::PortraitOrientation), -2}));
    }

    void closedReaderRetainsSleepScreen_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("dark");
        for (const auto &mode : {QString("stock"), QString("page"), QString("cover")})
            for (bool dark : {false, true})
                QTest::newRow(qPrintable(mode + (dark ? "-dark" : "-light"))) << mode << dark;
    }
    void closedReaderRetainsSleepScreen() {
        QFETCH(QString, mode);
        QFETCH(bool, dark);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), {{sleep::preferenceKey, mode}, {"dark_mode", dark}}));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) { return std::make_unique<test::HostPresentation>(state); };
        };
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        bool coverReady = false;
        host.session()->coverImage({2048, 2048}, [&](QImage) { coverReady = true; });
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
            QCOMPARE(reader::sleepScreenImage(backdrop.image, backdrop.progress, backdrop.remaining, dark,
                                               -1, reader::SleepStatus::Sleeping, backdrop.dimPage), expected);
        }
        host.window()->close();
        QTRY_COMPARE(state->savedCount, 1);
        QCOMPARE(state->saved.image, backdrop.image);
        QCOMPARE(state->saved.dark, backdrop.dark);
        QCOMPARE(state->saved.mode, sleep::mode({{sleep::preferenceKey, mode}}));
        QTRY_VERIFY(host.idle());
        // Destroying the old presentation clears its active view, not the
        // separate snapshot handed to the host.
        QCOMPARE(state->saved.image, backdrop.image);
    }
    void usbRetainsBackdropBeforeReleasingReader() {
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), {{sleep::preferenceKey, "page"}}));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) { return std::make_unique<test::HostPresentation>(state); };
        };
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        const auto expected = host.window()->powerScreenSnapshot();
        QVERIFY(!expected.image.isNull());
        bool released = false;
        host.window()->prepareUsb([&] { released = true; });
        QTRY_VERIFY(released);
        QCOMPARE(state->savedCount, 1);
        QCOMPARE(state->saved.image, expected.image);
        QCOMPARE(state->saved.progress, expected.progress);
    }
    void persistedSleepScreenRejectsStaleAndDamagedFrames() {
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
            writer.setText("kobalt", R"({"version":1,"token":"book-stamp","mode":"page","orientation":4,)"
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
    void homeSleepScreenRequiresUnambiguousLastBook() {
        const auto name = QStringLiteral("sleep-latest-book-test");
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", name);
            database.setDatabaseName(":memory:");
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("CREATE TABLE content (ContentID TEXT, ContentType INTEGER, DateLastRead TEXT)"));
            QVERIFY(nickel::lastReadBookToken(database).isEmpty());
            QVERIFY(query.exec("INSERT INTO content VALUES ('first', 6, '2026-10-24T09:00:00Z')"));
            const auto token = nickel::lastReadBookToken(database, "first");
            QVERIFY(!token.isEmpty());
            QCOMPARE(nickel::lastReadBookToken(database), token);
            QVERIFY(nickel::lastReadBookToken(database, "other").isEmpty());
            QVERIFY(query.exec("INSERT INTO content VALUES ('second', 6, '2026-10-24T09:00:01Z')"));
            QVERIFY(nickel::lastReadBookToken(database, "first").isEmpty());
            QVERIFY(nickel::lastReadBookToken(database) != token);
            QVERIFY(query.exec("UPDATE content SET DateLastRead = '2026-10-24T09:00:01Z' WHERE ContentID = 'first'"));
            QVERIFY(nickel::lastReadBookToken(database).isEmpty());
        }
        QSqlDatabase::removeDatabase(name);
    }

    void customSleepScreenIsOptIn_data() {
        QTest::addColumn<bool>("supported");
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("chargeState");
        QTest::addColumn<QString>("selectedMode");
        QTest::addColumn<bool>("hasCover");
        QTest::addColumn<QString>("hostCover");
        QTest::newRow("stock-default") << true << false << false << 0 << QString() << false << QString();
        QTest::newRow("custom-light") << true << true << false << 0 << QString() << false << QString();
        QTest::newRow("custom-dark") << true << true << true << 0 << QString() << false << QString();
        QTest::newRow("unsupported-host") << false << true << false << 0 << QString() << false << QString();
        QTest::newRow("charging-light") << true << true << false << 1 << QString() << false << QString();
        QTest::newRow("charging-dark") << true << true << true << 1 << QString() << false << QString();
        QTest::newRow("charged-light") << true << true << false << 2 << QString() << false << QString();
        QTest::newRow("charged-dark") << true << true << true << 2 << QString() << false << QString();
        QTest::newRow("cover-light") << true << true << false << 1 << QString("cover") << true << QString();
        QTest::newRow("cover-dark") << true << true << true << 2 << QString("cover") << true << QString();
        QTest::newRow("legacy-cover") << true << true << false << 0 << QString() << true << QString();
        QTest::newRow("page-with-cover") << true << true << false << 0 << QString("page") << true << QString();
        QTest::newRow("missing-cover") << true << true << false << 0 << QString("cover") << false << QString();
        QTest::newRow("stock-with-cover") << true << false << false << 0 << QString("stock") << true << QString();
        QTest::newRow("native-cover") << true << true << false << 0 << QString("cover") << true << QString("valid");
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
    void customSleepScreenIsOptIn() {
        QFETCH(bool, supported);
        QFETCH(bool, enabled);
        QFETCH(bool, dark);
        QFETCH(int, chargeState);
        QFETCH(QString, selectedMode);
        QFETCH(bool, hasCover);
        QFETCH(QString, hostCover);
        QTemporaryDir coverDirectory;
        QVERIFY(coverDirectory.isValid());
        const auto coverPath = hostCover.isEmpty() ? QString() : coverDirectory.path() + "/cover.parsed";
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
        QJsonObject preferences{{"kobalt_sleep_screen", enabled}, {"dark_mode", dark}};
        if (!selectedMode.isEmpty()) {
            preferences[sleep::preferenceKey] = selectedMode;
        }
        QVERIFY(storage::writeJson(paths.preferences(), preferences));
        auto sleep = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        // Exercise Kobo's missing native pace, independently of standalone history.
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {
            QUrl::fromLocalFile(QFileInfo(hasCover ? coverBook : textBook).absoluteFilePath()).toString(),
            "OEBPS/c1.xhtml", 100, 1, true};
        host.configureServices = [sleep, supported, chargeState, coverPath](ReaderHostServices &services) {
            services.coverImagePath = coverPath;
            services.battery = [chargeState] {
                return BatteryStatus{67, chargeState > 0, chargeState == 2};
            };
            services.presentation = [sleep, supported](QWidget *) {
                return std::make_unique<test::HostPresentation>(supported ? sleep : nullptr);
            };
        };
        CloseHost cleanup{host};
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
            {2048, 2048},
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
        const auto status = chargeState == 2   ? reader::SleepStatus::Charged
                            : chargeState == 1 ? reader::SleepStatus::Charging
                                               : reader::SleepStatus::Sleeping;
        const int percent = pageProgressPercent(host.session()->page(), host.session()->pages());
        // This profile has no native reading pace and has not made forward turns.
        const auto expected = reader::sleepScreenImage(background, strings::get("sleep.percent_read").arg(percent), {},
                                                       dark, 67, status, !useCover);
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

    void sleepScreenUpdatesWhilePluggedIn_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("tile");
        for (const auto &mode : {QString("stock"), QString("page"), QString("cover")}) {
            QTest::newRow(qPrintable(mode + "-light")) << mode << false << QString();
            QTest::newRow(qPrintable(mode + "-dark")) << mode << true << QString();
        }
        for (const auto &tile : {QString("band"), QString("bookmark")}) {
            QTest::newRow(qPrintable("page-" + tile + "-light")) << QString("page") << false << tile;
            QTest::newRow(qPrintable("cover-" + tile + "-dark")) << QString("cover") << true << tile;
        }
    }
    void sleepScreenUpdatesWhilePluggedIn() {
        QFETCH(QString, mode);
        QFETCH(bool, dark);
        QFETCH(QString, tile);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        QJsonObject preferences{{sleep::preferenceKey, mode}, {"dark_mode", dark}};
        if (!tile.isEmpty())
            preferences[sleep::tileKey] = tile;
        QVERIFY(storage::writeJson(paths.preferences(), preferences));
        auto state = std::make_shared<test::SleepPresentationState>();
        BatteryStatus battery{66, false, false};
        std::function<void()> changed;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.battery = [&] {
                return battery;
            };
            services.observeBattery = [&](QObject *, std::function<void()> callback) {
                changed = std::move(callback);
            };
            services.presentation = [state](QWidget *) {
                return std::make_unique<test::HostPresentation>(state);
            };
        };
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        bool coverReady = false;
        host.session()->coverImage({2048, 2048}, [&](QImage) {
            coverReady = true;
        });
        QTRY_VERIFY(coverReady);
        QVERIFY(changed);
        changed();
        QCOMPARE(state->updated, 0);
        host.window()->sleepReader();
        battery = {67, true, false};
        changed();
        QCOMPARE(state->updated, 0);
        host.window()->sleepViewReady();
        if (mode == "stock") {
            QVERIFY(state->image.isNull());
            battery = {100, true, true};
            changed();
            QCOMPARE(state->updated, 0);
            host.window()->wakeReader();
            return;
        }
        QCOMPARE(state->presented, 1);
        const auto initial = state->image;
        QVERIFY(!initial.isNull());
        // A point on the page that the selected tile does not cover.
        const QPoint outside = tile == "bookmark" ? QPoint(100, 200) : QPoint(1000, 200);
        battery.percent = 68;
        changed();
        QCOMPARE(state->updated, 1);
        QVERIFY(state->image != initial);
        QCOMPARE(state->image.pixel(outside), initial.pixel(outside));
        QVERIFY(!state->damage.isEmpty());
        QVERIFY(!state->damage.contains(outside));
        if (!tile.isEmpty()) {
            // These tiles keep one area whether or not a time estimate is shown.
            QCOMPARE(state->damage, reader::sleepScreenCardRect(initial.size(), false, sleep::tile(preferences)));
        }
        const auto charging = state->image;
        changed();
        QCOMPARE(state->updated, 1);
        battery = {100, true, true};
        changed();
        QCOMPARE(state->updated, 2);
        QVERIFY(state->image != charging);
        const auto charged = state->image;
        battery = {100, false, false};
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
        battery = {21, true, false};
        changed();
        QCOMPARE(state->updated, 3);
        QVERIFY(state->image.isNull());
    }

    void sleepScreenDimsOnlyItsOwnedPage_data() {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<bool>("dark");
        for (int ppi : {212, 300}) {
            QTest::newRow(qPrintable(QString::number(ppi) + "-light")) << ppi << false;
            QTest::newRow(qPrintable(QString::number(ppi) + "-dark")) << ppi << true;
        }
    }
    void sleepScreenDimsOnlyItsOwnedPage() {
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
        for (const int percent : {0, 67, 100}) {
            QCOMPARE(reader::sleepScreenImage(page, "42% read", "37 min to go", dark, percent,
                                              reader::SleepStatus::Sleeping),
                     frame);
        }
        const auto charging =
            reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 67, reader::SleepStatus::Charging);
        const auto charged =
            reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 67, reader::SleepStatus::Charged);
        QVERIFY(charging != charged);
        QVERIFY(charging != frame && charged != frame);
        QVERIFY(charged !=
                reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 100, reader::SleepStatus::Charged));
        QVERIFY(charging !=
                reader::sleepScreenImage(page, "42% read", "37 min to go", dark, 23, reader::SleepStatus::Charging));
        QVERIFY(charging !=
                reader::sleepScreenImage(page, "42% read", "37 min to go", dark, -1, reader::SleepStatus::Charging));
        QCOMPARE(reader::sleepStatusText(reader::SleepStatus::Sleeping), QString("Sleeping"));
        QCOMPARE(reader::sleepStatusText(reader::SleepStatus::Charging), QString("Charging"));
        QCOMPARE(reader::sleepStatusText(reader::SleepStatus::Charged), QString("Charged"));
        ui::setDensity(previous);
    }

    void sleepTilesRepaintOnlyTheirArea_data() {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("tileIndex");
        for (const int ppi : {212, 300}) {
            for (const auto tile : sleep::tiles) {
                const auto name = QString::number(ppi) + "-" + sleep::value(tile);
                QTest::newRow(qPrintable(name + "-light")) << ppi << false << int(tile);
                QTest::newRow(qPrintable(name + "-dark")) << ppi << true << int(tile);
            }
        }
    }
    void sleepTilesRepaintOnlyTheirArea() {
        QFETCH(int, ppi);
        QFETCH(bool, dark);
        QFETCH(int, tileIndex);
        const auto tile = sleep::Tile(tileIndex);
        const int previous = ui::density();
        ui::setDensity(ppi);
        for (const QSize size : {QSize(1072, 1448), QSize(1448, 1072)}) {
            QImage page(size, QImage::Format_RGB32);
            page.fill(QColor(150, 150, 150));
            const auto paint = [&](const QImage &background, reader::SleepStatus status, bool dim, int percent = 42) {
                return reader::sleepScreenImage(background, "42% read", "3 h 37 min to go", dark, 67, status, dim, tile,
                                                percent);
            };
            const auto area = reader::sleepScreenCardRect(size, true, tile);
            QVERIFY(QRect(QPoint(), size).contains(area));
            const auto sleeping = paint(page, reader::SleepStatus::Sleeping, true);
            const auto charging = paint(page, reader::SleepStatus::Charging, true);
            const auto poweredOff = paint(page, reader::SleepStatus::PoweredOff, true);
            QVERIFY(sleeping != charging);
            QVERIFY(sleeping != poweredOff);
            // A battery update refreshes only the tile's area, so nothing outside it may change.
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    if (!area.contains(x, y) && charging.pixel(x, y) != sleeping.pixel(x, y))
                        QFAIL(qPrintable(QString("tile changed %1,%2 outside its area").arg(x).arg(y)));
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
                const auto name = QString("%1/%2-%3-%4-%5").arg(capture, sleep::value(tile)).arg(ppi)
                                      .arg(dark ? "dark" : "light").arg(size.width() > size.height() ? "landscape" : "portrait");
                QVERIFY(sleeping.save(name + "-sleeping.png"));
                QVERIFY(charging.save(name + "-charging.png"));
                QVERIFY(poweredOff.save(name + "-powered-off.png"));
            }
        }
        ui::setDensity(previous);
    }

    void sleepSnapshotMatchesTheDisplayedPage_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void sleepSnapshotMatchesTheDisplayedPage() {
        QFETCH(bool, dark);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), {{"dark_mode", dark}}));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = static_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        page->bookmarked = true;
        page->refreshContent();
        page->mark({QRect(30, 40, 120, 25)});
        page->revealLinks({QRect(60, 100, 180, 30)});
        const auto displayed = page->grab().toImage().convertToFormat(QImage::Format_RGB32);
        const auto snapshot = page->snapshot();
        QCOMPARE(snapshot, displayed);
        QCOMPARE(page->snapshot(), snapshot);
    }

    void sleepSnapshotReplacesEachClockWithoutChangingTheReadingPage_data() {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("slot");
        for (int ppi : {212, 300})
            for (bool dark : {false, true})
                for (int slot = 0; slot < headerfooter::slotCount; ++slot)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(ppi).arg(dark).arg(slot))) << ppi << dark << slot;
    }
    void sleepSnapshotReplacesEachClockWithoutChangingTheReadingPage() {
        QFETCH(int, ppi);
        QFETCH(bool, dark);
        QFETCH(int, slot);
        struct RestoreDensity {
            int value = ui::density();
            ~RestoreDensity() {
                ui::setDensity(value);
            }
        } restore;
        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] {});
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
        page.image = QImage(page.width(), page.height() - page.topHeight() - page.bottomHeight(), QImage::Format_RGB32);
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
    void timeRemainingFormatShared_data() {
        QTest::addColumn<bool>("approximate");
        QTest::newRow("detailed") << false;
        QTest::newRow("approximate") << true;
    }
    void timeRemainingFormatShared() {
        QFETCH(bool, approximate);
        const auto paths = testPaths();
        auto layout = headerfooter::presetLayout(headerfooter::Preset::Default);
        layout.slotFields[0] = headerfooter::Field::ChapterTimeLeft;
        layout.slotFields[5] = headerfooter::Field::BookTimeLeft;
        for (auto &uppercase : layout.uppercase) uppercase = false;
        QVERIFY(storage::writeJson(paths.preferences(), {{"time_remaining_format", approximate ? "approximate" : "detailed"},
            {sleep::preferenceKey, "page"}, {headerfooter::presetKey, "custom"},
            {headerfooter::customKey, headerfooter::toJson(layout)}}));
        auto state = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {QUrl::fromLocalFile(QFileInfo(coverBook).absoluteFilePath()).toString(),
                                  "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        host.progress->hasEstimates = true;
        host.progress->remaining = {18 * 60000, 83 * 60000};
        host.configureServices = [state](ReaderHostServices &services) {
            services.presentation = [state](QWidget *) { return std::make_unique<test::HostPresentation>(state); };
        };
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        auto *page = dynamic_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QTRY_COMPARE(page->slotContents[0].text, approximate ? QString("about 30 minutes left") : QString("18 min left"));
        QTRY_COMPARE(page->slotContents[5].text, approximate ? QString("about 2 hours left") : QString("1 h 23 min left"));
        const auto snapshot = host.window()->powerScreenSnapshot();
        QVERIFY(!snapshot.image.isNull());
        QCOMPARE(snapshot.remaining, approximate ? QString("about 2 hours to go") : QString("83 min to go"));
    }
    void sleepTimeLeftApproximate() {
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
    void sleepTimeLeftUsesHoursAboveTwoHours() {
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

    void sleepScreenModesKeepTheStockDefaultAndMigrateTheOptIn() {
        QCOMPARE(sleep::mode({}), sleep::Mode::Stock);
        QCOMPARE(sleep::mode({{"kobalt_sleep_screen", false}}), sleep::Mode::Stock);
        QCOMPARE(sleep::mode({{"kobalt_sleep_screen", true}}), sleep::Mode::Cover);
        for (const auto mode : sleep::modes) {
            QCOMPARE(sleep::mode({{sleep::preferenceKey, sleep::value(mode)}, {"kobalt_sleep_screen", true}}), mode);
        }
        for (const auto &invalid : {QJsonValue("unknown"), QJsonValue(true), QJsonValue(QJsonValue::Null)}) {
            QCOMPARE(sleep::mode({{sleep::preferenceKey, invalid}, {"kobalt_sleep_screen", true}}), sleep::Mode::Stock);
        }
        QCOMPARE(sleep::tile({}), sleep::Tile::Card);
        for (const auto tile : sleep::tiles) {
            QCOMPARE(sleep::tile({{sleep::tileKey, sleep::value(tile)}}), tile);
            QCOMPARE(appearance::cleanPreferences({{sleep::tileKey, sleep::value(tile)}}).value(sleep::tileKey),
                     QJsonValue(sleep::value(tile)));
        }
        for (const auto &invalid : {QJsonValue("unknown"), QJsonValue(true), QJsonValue(QJsonValue::Null)}) {
            QCOMPARE(sleep::tile({{sleep::tileKey, invalid}}), sleep::Tile::Card);
            QVERIFY(!appearance::cleanPreferences({{sleep::tileKey, invalid}}).contains(sleep::tileKey));
        }
    }

    void sleepCoverFitsWithoutCroppingOrDimming() {
        QImage cover(80, 40, QImage::Format_RGB32);
        cover.fill(Qt::green);
        cover.setPixel(0, 0, qRgb(255, 0, 0));
        const auto original = cover.copy();
        for (const bool dark : {false, true}) {
            const auto background = reader::sleepCoverBackground(cover, {800, 1200});
            QCOMPARE(background.size(), QSize(800, 1200));
            QCOMPARE(background.pixel(0, 0), qRgb(0, 0, 0));
            QCOMPARE(background.pixel(799, 1199), qRgb(0, 0, 0));
            QCOMPARE(background.pixel(400, 600), qRgb(0, 255, 0));
            QCOMPARE(background.pixel(0, 400), qRgb(255, 0, 0));
            QCOMPARE(background.pixel(799, 799), qRgb(0, 255, 0));
            const auto frame = reader::sleepScreenImage(background, "42% read", "3 h 37 min to go", dark, 67,
                                                        reader::SleepStatus::Charging, false);
            QCOMPARE(frame.pixel(400, 600), background.pixel(400, 600));
            QCOMPARE(frame.pixel(0, 0), background.pixel(0, 0));
        }
        QCOMPARE(cover, original);
        const auto sideBorders = reader::sleepCoverBackground(cover, {120, 40});
        QCOMPARE(sideBorders.pixel(0, 20), qRgb(0, 0, 0));
        QCOMPARE(sideBorders.pixel(119, 20), qRgb(0, 0, 0));
        QCOMPARE(sideBorders.pixel(60, 20), qRgb(0, 255, 0));
        QImage transparent(80, 40, QImage::Format_ARGB32);
        transparent.fill(Qt::transparent);
        QCOMPARE(reader::sleepCoverBackground(transparent, {80, 40}).pixel(40, 20), qRgb(0, 0, 0));
        QVERIFY(reader::sleepCoverBackground({}, {800, 1200}).isNull());
        QVERIFY(reader::sleepCoverBackground(cover, {}).isNull());
        QVERIFY(reader::sleepCoverBackground(cover, {4096, 4096}).isNull());
    }

    void sleepCoverDecodingIsBoundedAndHonorsSourceCancellation() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(coverBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QImage cover;
        int completed = 0;
        host.session()->coverImage({60, 90}, [&](QImage image) {
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
        host.session()->coverImage({2048, 2048}, [&](QImage image, QFileInfo source) {
            cover = std::move(image);
            provenance = std::move(source);
            nativeDone = true;
        }, nativePath);
        QTRY_VERIFY(nativeDone);
        QCOMPARE(cover.size(), nativeImage.size());
        QCOMPARE(provenance.absoluteFilePath(), nativePath);
        QFile corrupt(nativePath);
        QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupt.write("broken PNG");
        corrupt.close();
        nativeDone = false;
        host.session()->coverImage({2048, 2048}, [&](QImage image, QFileInfo source) {
            cover = std::move(image);
            provenance = std::move(source);
            nativeDone = true;
        }, nativePath);
        QTRY_VERIFY(nativeDone);
        QVERIFY(!cover.isNull());
        QVERIFY(provenance.filePath().isEmpty());
        host.session()->coverImage({4096, 4096}, [&](QImage image) {
            cover = std::move(image);
            ++completed;
        });
        QTRY_COMPARE(completed, 2);
        QVERIFY(cover.isNull());
        host.session()->coverImage({2048, 2048}, [&](QImage image) {
            cover = std::move(image);
            ++completed;
        });
        host.session()->cancelSource();
        QTRY_COMPARE(completed, 3);
        QVERIFY(cover.isNull());
    }

    // Checks Wi-Fi indicator placement, size and visibility in the status strip, in both themes and densities.
    void wifiStatus_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        for (const bool dark : {false, true})
            for (const int ppi : {300, 212})
                QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(ppi))) << dark << ppi;
    }
    void wifiStatus() {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const int previousDensity = ui::density();
        struct Restore { int density; ~Restore() { ui::setDensity(density); } } restore{previousDensity};
        ui::setDensity(ppi);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), {{"dark_mode", dark}}));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        std::function<void(WifiStatus)> wifiChanged;
        std::function<void(bool)> bluetoothChanged;
        host.configureServices = [&](ReaderHostServices &services) {
            services.observeWifi = [&](QObject *, std::function<void(WifiStatus)> changed) {
                wifiChanged = changed;
                changed({true, true, 4});
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
                    if (ink > 0 && color.alpha() > 0) bounds |= QRect(x, y, 1, 1);
                }
            return bounds;
        };
        const auto wifiInk = inkBounds(strong);
        const auto bluetoothInk = inkBounds(bluetooth->grab().toImage());
        QVERIFY(qAbs(wifiInk.height() - 0.9 * bluetoothInk.height()) <= 1);
        QVERIFY(qAbs(QRectF(wifiInk).center().y() - QRectF(bluetoothInk).center().y() + ui::px(1)) <= 0.5);
        const auto capture = qEnvironmentVariable("KOBALT_WIFI_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(host.window()->grab().save(capture + QString("/wifi-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
            QVERIFY(host.window()->grab(QRect(0, 0, host.window()->width(), ui::px(60))).save(
                capture + QString("/wifi-statusbar-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        }
        wifiChanged({true, true, 1});
        QVERIFY(wifi->accessibleName().contains("1 of 4"));
        QVERIFY(strong != wifi->grab().toImage());
        wifiChanged({true, true, 4});
        const int withBluetooth = wifi->x();
        const auto bluetoothSlot = QRectF(bluetooth->geometry()).center().x();
        bluetoothChanged(false);
        QTRY_VERIFY(!bluetooth->isVisible());
        QVERIFY(wifi->x() > withBluetooth);
        QCOMPARE(QRectF(wifi->geometry()).center().x(), bluetoothSlot);
        if (!capture.isEmpty())
            QVERIFY(host.window()->grab(QRect(0, 0, host.window()->width(), ui::px(60))).save(
                capture + QString("/wifi-only-statusbar-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        wifiChanged({true, false, -1});
        QTRY_VERIFY(!wifi->isVisible());
        wifiChanged({true, true, -1});
        QTRY_VERIFY(!wifi->isVisible());
        wifiChanged({true, true, 0});
        QTRY_VERIFY(!wifi->isVisible());
        wifiChanged({true, true, 1});
        QTRY_VERIFY(wifi->isVisible());
        wifiChanged({false, false, -1});
        QTRY_VERIFY(!wifi->isVisible());
    }
    // Checks that reading-time text fits and centers in the status strip for each minute width and radio icon combination.
    void readingTimeStatus_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        QTest::addColumn<qreal>("textScale");
        QTest::addColumn<QSize>("viewport");
        for (bool dark : {false, true})
            for (int ppi : {300, 212})
                for (qreal scale : {1.0, 1.5})
                    for (QSize viewport : {QSize(1072, 1448), QSize(1448, 600)})
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4x%5")
                            .arg(dark ? "dark" : "light").arg(ppi).arg(scale).arg(viewport.width()).arg(viewport.height())))
                            << dark << ppi << scale << viewport;
    }
    void readingTimeStatus() {
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
        QVERIFY(storage::writeJson(paths.preferences(), {{"dark_mode", dark}}));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        std::function<void(WifiStatus)> wifiChanged;
        std::function<void(bool)> bluetoothChanged;
        QString clockText = "09:00";
        int timeOfDay = 9 * 3600000;
        host.configureServices = [&](ReaderHostServices &services) {
            services.clock = [&] { return clockText; };
            services.millisecondsOfDay = [&] { return timeOfDay; };
            services.observeWifi = [&](QObject *, std::function<void(WifiStatus)> changed) {
                wifiChanged = changed;
                changed({true, true, 4});
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
        for (int minutes : {5, 15, 120}) {
            // Use the production wording without waiting for a long reading session.
            session->setText(strings::get("reader.session_minutes").arg(minutes));
            for (int radios : {3, 1, 2, 0, 3}) {
                bluetoothChanged(!(radios & 2));
                wifiChanged({bool(radios & 1), bool(radios & 1), 4});
                bluetoothChanged(radios & 2);
                QTRY_VERIFY(session->isVisible());
                QCOMPARE(session->font(), font);
                QCOMPARE(battery->geometry(), batteryBounds);
                QWidget *right = wifi->isVisible() ? wifi : bluetooth->isVisible() ? bluetooth : battery;
                if (session->width() > 0) {
                    QVERIFY(session->x() >= clock->geometry().right() + 1);
                    QVERIFY(session->geometry().right() < right->x());
                }
                const int available = right->x() - clock->geometry().right() - 1 - 2 * int(ui::metrics::chrome::inset);
                if (session->sizeHint().width() <= available)
                    QVERIFY2(session->width() >= session->sizeHint().width(), qPrintable(session->text()));
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
                    QVERIFY(host.window()->grab(QRect(0, 0, size.width(), ui::textPx(60))).save(
                        capture + QString("/reading-time-%1-%2-%3-%4-%5%6.png")
                            .arg(dark ? "dark" : "light").arg(ppi).arg(textScale).arg(minutes).arg(radios)
                            .arg(isShortHeight(size.height(), ppi) ? "-short" : "")));
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
        const int available = wifi->x() - clock->geometry().right() - 1 - 2 * int(ui::metrics::chrome::inset);
        if (available >= 0) {
            const qreal midpoint = (clock->geometry().right() + 1 + wifi->x()) / 2.;
            QVERIFY(qAbs(QRectF(session->geometry()).center().x() - midpoint) <= .5);
        }
    }
    void remoteKeyActivityRequiresActiveReading() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        int reports = 0;
        bool suspended = false;
        host.configureServices = [&](ReaderHostServices &services) {
            services.pageButtonDirection = [](QKeyEvent *event) {
                return event->key() == Qt::Key_Right ? 1 : 0;
            };
            services.suspended = [&] {
                return suspended;
            };
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
        const int initial = host.session()->page();
        sendKey(QEvent::KeyPress, Qt::Key_Right);
        QCOMPARE(reports, 1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == initial + 1);
        sendKey(QEvent::KeyRelease, Qt::Key_Right);
        QCOMPARE(reports, 1);
        sendKey(QEvent::KeyPress, Qt::Key_Right, true);
        QCOMPARE(reports, 2);
        QCOMPARE(host.session()->page(), initial + 1);
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
    void finishChoiceSurvivesSleepAndRequiresActiveReader_data() {
        QTest::addColumn<bool>("markFinished");
        QTest::addColumn<bool>("exitRequested");
        QTest::addColumn<bool>("suppress");
        QTest::newRow("suppress-still-reading") << false << false << true;
        QTest::newRow("suppress-mark-finished") << true << false << true;
        QTest::newRow("suppress-exit-still-reading") << false << true << true;
        QTest::newRow("suppress-exit-mark-finished") << true << true << true;
        QTest::newRow("still-reading") << false << false << false;
        QTest::newRow("mark-finished") << true << false << false;
        QTest::newRow("exit-still-reading") << false << true << false;
        QTest::newRow("exit-mark-finished") << true << true << false;
    }
    void finishChoiceSurvivesSleepAndRequiresActiveReader() {
        QFETCH(bool, markFinished);
        QFETCH(bool, exitRequested);
        QFETCH(bool, suppress);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        bool suspended = false;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] {
                return suspended;
            };
        };
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(host.session()->pages() - 1));
        QTRY_VERIFY(!host.session()->busy());
        host.window()->pageButton(1);
        QPointer<QWidget> dialog;
        QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook")) && dialog->isVisible());
        auto *choice = dialog->findChild<QPushButton *>(markFinished ? "rrMarkFinished" : "rrStillReading");
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
        QVERIFY(dialog->isHidden());
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QTRY_VERIFY(dialog && dialog->isVisible());
        choice->click();
        QTRY_VERIFY(!dialog);
        if (markFinished || exitRequested) {
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
                QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook")) && dialog->isVisible());
            }
        }
    }

    // Checks that "Don't ask again" on the finish prompt survives reopening and a fresh process, and that other books still ask.
    void finishPromptPersistence() {
        // A fresh process must restore the choice without any prior Reader state.
        const auto inherited = qEnvironmentVariable("KOBALT_TEST_FINISH_PROFILE");
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = inherited.isEmpty() ? root.path() : inherited;
        const auto bookPath = inherited.isEmpty() ? textBook : qEnvironmentVariable("KOBALT_TEST_FINISH_BOOK");
        const auto identity = QUrl::fromLocalFile(QFileInfo(bookPath).absoluteFilePath()).toString();
        if (inherited.isEmpty()) {
            ReaderFixture host(paths, testFonts, testPlaceholder);
            CloseHost cleanup{host};
            QSignalSpy opened(&host, &ReaderFixture::opened);
            host.open(bookPath, nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
            Canvas canvas(host.window(), nativeSize);
            canvas.resize(900, 900);
            canvas.show();
            QVERIFY(QTest::qWaitForWindowExposed(&canvas));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(host.session()->go(host.session()->pages() - 1));
            QTRY_VERIFY(!host.session()->busy());
            host.window()->pageButton(1);
            QPointer<QWidget> dialog;
            QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook")) && dialog->isVisible());
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
            CloseHost cleanup{host};
            QSignalSpy opened(&host, &ReaderFixture::opened);
            host.open(bookPath, nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
            Canvas canvas(host.window(), nativeSize);
            canvas.resize(900, 900);
            canvas.show();
            QVERIFY(QTest::qWaitForWindowExposed(&canvas));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(host.session()->go(host.session()->pages() - 1));
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
            QVERIFY(host.session()->go(host.session()->pages() - 1));
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
            child.start(QCoreApplication::applicationFilePath(), {"finishPromptPersistence"});
            QVERIFY(child.waitForFinished(60000));
            const auto output = child.readAll();
            QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
        }
    }

    // Checks that missing, unusable and manual-only accounts never sync while reading, the toolbar Sync button and the Automatic sync sheet.
    void kosyncAccountGating_data() {
        QTest::addColumn<QString>("account");
        for (const auto *name : {"missing", "signed-out", "not-authenticated", "malformed", "future-account",
                                 "automatic-off", "logout"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void kosyncAccountGating() {
        QFETCH(QString, account);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&server, &QTcpServer::newConnection);
        const auto paths = testPaths();
        if (account != "missing") {
            const bool authenticated = account == "automatic-off" || account == "logout";
            QVERIFY(storage::writeJson(paths.syncAccount(), {
                {"schema", account == "future-account" ? 2 : 1},
                {"server", QString("https://127.0.0.1:%1").arg(server.serverPort())},
                {"username", account == "signed-out" ? QString() : QStringLiteral("reader")},
                {"key", authenticated ? QString(32, 'a') : account == "malformed" ? QStringLiteral("invalid") : QString()},
                {"device_id", "test-device"}, {"automatic", account != "automatic-off"}}));
        }
        const auto accountBytes = [&] {
            QFile file(paths.syncAccount());
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto original = accountBytes();
        auto network = std::make_shared<SyncNetworkState>();
        int factories = 0;
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        if (account != "logout") QCOMPARE(accountBytes(), original);
    }
    // Checks the opening sync against a stalled server: input blocking, Cancel sync, grace period, timeout and repaint.
    void kosyncOpening_data() {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("cancel-sync") << QString("cancel");
        QTest::newRow("offline-deadline") << QString("deadline");
        QTest::newRow("completed") << QString("completed");
    }
    void kosyncOpening() {
        QFETCH(QString, outcome);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", true}}));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        QVERIFY(popup->findChild<QLabel *>("rrModalDescription")->text().contains("Kobalt is syncing your progress"));
        if (!capture.isEmpty()) QVERIFY(host.window()->grab().save(capture));
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
    // Checks that sleep dismisses a revealed password editor and that it reopens masked and empty after wake.
    void kosyncPasswordSleep() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->findChild<QPushButton *>("rrSettings")->click();
        host.window()->findChild<QPushButton *>("rrSettingsKosync")->click();
        host.window()->findChild<QPushButton *>("rrKosyncPassword")->click();
        QPointer<QLineEdit> input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
        QVERIFY(input);
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
    void kosyncSleepSavesSyncTime_data() {
        QTest::addColumn<bool>("sleepSync");
        QTest::newRow("reading") << false;
        QTest::newRow("sleep-sync") << true;
    }
    // Checks that sleep saves the sync time, also when the sleep sync ends later, and that a sleep without a sync writes nothing.
    void kosyncSleepSavesSyncTime() {
        QFETCH(bool, sleepSync);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", "https://127.0.0.1:9"}, {"username", "test-reader"},
            {"key", QString(32, 'a')}, {"device_id", "test-device"}, {"automatic", true},
            {"sync_on_open", !sleepSync}, {"sync_on_sleep_wake", sleepSync}}));
        const auto savedSyncTime = [&] {
            return storage::readJsonFile(paths.syncAccount()).object.value("last_sync").toString();
        };
        auto http = std::make_shared<KosyncHttpState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [http](ReaderHostServices &services) {
            services.kosyncHttp = [http] { return std::make_unique<KosyncHttp>(http); };
            services.kosyncSleepWake = true;
        };
        CloseHost cleanup{host};
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
            if (reply) QMetaObject::invokeMethod(reply, [reply] { reply->finish(); }, Qt::QueuedConnection);
        QTRY_VERIFY(!window->pendingNetworkSync());
        QCOMPARE(http->requests, 2);
        QTRY_VERIFY(QDateTime::fromString(savedSyncTime(), Qt::ISODate).isValid());
    }
    // Checks the toolbar Sync button: the first-use prompt until confirmed, an interactive request, the busy button and the error notice.
    void kosyncManualToolbar() {
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", "https://127.0.0.1:1"},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", false}}));
        auto network = std::make_shared<SyncNetworkState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
        };
        CloseHost cleanup{host};
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
        QCOMPARE(page->notice, strings::get("kosync.network_error"));
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
    // Checks Sync now in Settings: no deadline while a connection waits for permission, then the Syncing notice and its Cancel.
    void kosyncManualConnection_data() {
        QTest::addColumn<QString>("connection");
        for (const auto *connection : {"permission", "connected", "desktop"})
            QTest::newRow(connection) << QString::fromLatin1(connection);
    }
    void kosyncManualConnection() {
        QFETCH(QString, connection);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", false}}));
        auto network = std::make_shared<SyncNetworkState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        if (connection != "desktop")
            host.configureServices = [network](ReaderHostServices &services) {
                services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            };
        CloseHost cleanup{host};
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
                QTRY_VERIFY2_WITH_TIMEOUT(activeReaderTimers(host.window()).isEmpty(),
                                          qPrintable("Active reader timers: " +
                                                     activeReaderTimers(host.window()).join(", ")), 10000);
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
        }
        if (connection != "permission") {
            QPointer<QWidget> popup = host.window()->findChild<QWidget *>("rrKosyncOpening");
            QVERIFY(popup && popup->isVisible());
            QTRY_VERIFY(attempts.count() > 0);
            const auto capture = qEnvironmentVariable("KOBALT_KOSYNC_MANUAL_CAPTURE");
            if (!capture.isEmpty() && connection == "connected") QVERIFY(host.window()->grab().save(capture));
            const auto page = host.session()->page();
            host.window()->pageButton(1);
            QCOMPARE(host.session()->page(), page);
            auto *cancel = popup->findChild<QPushButton *>("rrKosyncCancel");
            QVERIFY(cancel && click(canvas, cancel));
            QTRY_VERIFY(!popup || !popup->isVisible());
            const auto count = attempts.count();
            QTest::qWait(600);
            QCOMPARE(attempts.count(), count);
            QCOMPARE(host.window()->findChild<QLabel *>("rrKosyncStatus")->text(), strings::get("kosync.skipped"));
        }
        QVERIFY(settings->isVisible());
        QVERIFY(host.window()->findChild<QPushButton *>("rrKosyncServer")->isEnabled());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsClose")));
        QTRY_VERIFY(!settings->isVisible());
    }
    // Checks that the opening sync sends no HTTP before a connection is ready: Cancel, the deadline, sleep, and a later Sync now or Sign in.
    void kosyncNetwork_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"cancel", "deadline", "ready", "sleep", "login"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void kosyncNetwork() {
        QFETCH(QString, scenario);
        verifyKosyncNetwork(scenario);
    }
    // Checks that each sync moment switched off starts no connection, and how a skipped opening sync starts the interval.
    void kosyncMoments_data() {
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
    void kosyncMoments() {
        QFETCH(int, minutes);
        QFETCH(bool, automaticAfterWake);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", !automaticAfterWake}, {"sync_interval_minutes", minutes},
            {"sync_on_open", automaticAfterWake}, {"sync_on_sleep_wake", false}, {"sync_on_close", false}}));
        auto network = std::make_shared<SyncNetworkState>();
        // Start far past every interval, as a device clock counted from boot does.
        qint64 clock = 2 * 60 * 60000;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{host};
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
        // Never while reading: no page syncs. Every 15 minutes: the interval
        // counts from the skipped opening sync, and Wait keeps the sync local.
        readOn(15);
        QCOMPARE(status->text(), minutes < 0 ? QString("unchanged") : strings::get("kosync.saved_locally"));
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
    void minuteClock() {
        auto paths = testPaths();
        int timeOfDay = 10 * 3600000 + 30000;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&timeOfDay](ReaderHostServices &services) {
            services.millisecondsOfDay = [&timeOfDay] { return timeOfDay; };
        };
        CloseHost cleanup{host};
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
    }
    // While a book sits open and nobody reads, nothing on the main thread may
    // wake more often than the reader's minute clock.
    void idleTimers() {
        auto paths = testPaths();
        // Ten o'clock exactly: the clock is armed for a minute and stays out of the window.
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [](ReaderHostServices &services) {
            services.millisecondsOfDay = [] { return 10 * 3600000; };
        };
        CloseHost cleanup{host};
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
        QTRY_VERIFY2_WITH_TIMEOUT(activeReaderTimers(host.window()).isEmpty(),
                                  qPrintable("Active reader timers: " + activeReaderTimers(host.window()).join(", ")),
                                  10000);
        // No timer of any main-thread object fires for a few idle seconds:
        // this also covers unparented timers and single-shot calls.
        struct TimerEvents final : QObject {
            QStringList seen;
            bool eventFilter(QObject *object, QEvent *event) override {
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
        QVERIFY2(events.seen.isEmpty(), qPrintable("Timer events while idle: " + events.seen.join(", ")));
    }
    // Checks when opening a book asks the host to turn Wi-Fi off: each account and schedule, the saver setting and Bluetooth.
    void wifiPowerSaver_data() {
        QTest::addColumn<QString>("account");
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("bluetooth");
        QTest::addColumn<bool>("expected");
        QTest::newRow("no-account") << "none" << true << false << true;
        QTest::newRow("manual-sync") << "manual" << true << false << true;
        QTest::newRow("automatic") << "default" << true << false << false;
        QTest::newRow("automatic-never-while-reading") << "never" << true << false << false;
        QTest::newRow("automatic-turn-on-wifi") << "wake" << true << false << false;
        QTest::newRow("setting-off") << "none" << false << false << false;
        QTest::newRow("bluetooth-on") << "none" << true << true << false;
    }
    // The saver asks the host to turn Wi-Fi off only when automatic KOSync is
    // off or not set up, whatever the schedule, and the book is presented only
    // after the host answers.
    void wifiPowerSaver() {
        QFETCH(QString, account);
        QFETCH(bool, enabled);
        QFETCH(bool, bluetooth);
        QFETCH(bool, expected);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        auto paths = testPaths();
        if (!enabled) QVERIFY(storage::writePreferences(paths.preferences(), {{"turn_off_wifi_on_open", false}}));
        if (account != "none") {
            QJsonObject saved{{"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
                              {"username", "test-reader"}, {"key", QString(32, 'a')},
                              {"device_id", "test-device"}, {"automatic", account != "manual"}};
            if (account != "default" && account != "manual") {
                for (const auto *key : {"sync_on_open", "sync_on_sleep_wake", "sync_on_close"})
                    saved[key] = false;
                saved["sync_interval_minutes"] = account == "never" ? kosync::neverWhileReading : 15;
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
                if (bluetooth) changed(true);
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
        CloseHost cleanup{host};
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
    void kosyncBackgroundWifi() {
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", true},
            {"sync_interval_minutes", 5}, {"wake_wifi", true}}));
        auto network = std::make_shared<SyncNetworkState>();
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{host};
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
        QTRY_VERIFY((popup = host.window()->findChild<QWidget *>("rrKosyncOpening")) && popup->isVisible());
        QVERIFY(click(canvas, popup->findChild<QPushButton *>("rrKosyncCancel")));
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
        QCOMPARE(status->text(), strings::get("kosync.saved_locally"));
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
        QCOMPARE(status->text(), strings::get("kosync.saved_locally"));
        status->setText("unchanged");
        readOn(0.4);
        QCOMPARE(status->text(), QString("unchanged"));
        readOn(0.2);
        QCOMPARE(status->text(), strings::get("kosync.saved_locally"));
        QCOMPARE(network->requests, requests);
    }
    // Checks that opening, wake and periodic syncs release their Wi-Fi request however they end, and that Sync now does not.
    void kosyncWifiRelease_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"opening-skip", "opening-failure", "wake-end", "wake-skip", "wake-close", "periodic-close"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    // An opening, wake or periodic sync asks for Wi-Fi silently. When it ends,
    // whether it succeeded, failed, reached its deadline, was skipped or was
    // cancelled by close, it releases the Wi-Fi it asked for. The network
    // controller turns Wi-Fi off only when that request turned it on. Sync now
    // leaves Wi-Fi as it is.
    void kosyncWifiRelease() {
        QFETCH(QString, scenario);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        const bool opening = scenario.startsWith("opening");
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", true}, {"sync_interval_minutes", 5}, {"wake_wifi", true},
            {"sync_on_open", opening}, {"sync_on_sleep_wake", true}, {"sync_on_close", false}}));
        auto network = std::make_shared<SyncNetworkState>();
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.kosyncSleepWake = services.kosyncWifiControl = true;
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{host};
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
            QTRY_VERIFY((popup = host.window()->findChild<QWidget *>("rrKosyncOpening")) && popup->isVisible());
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
    void wifiPowerSaverAtSleep_data() {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("saver-on") << true;
        QTest::newRow("saver-off") << false;
    }
    // At sleep the power saver asks the host to turn Wi-Fi off as soon as the
    // sleep sync has ended, also with automatic KOSync. KOSync itself leaves
    // a connection it did not open alone. Wake drops an unanswered request.
    void wifiPowerSaverAtSleep() {
        QFETCH(bool, enabled);
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        auto paths = testPaths();
        if (!enabled) QVERIFY(storage::writePreferences(paths.preferences(), {{"turn_off_wifi_on_open", false}}));
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", true}, {"wake_wifi", true},
            {"sync_on_open", false}, {"sync_on_sleep_wake", true}, {"sync_on_close", false}}));
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
            services.turnOffWifi = [&](QObject *request, const char *why, std::function<void(bool)>) {
                ++requests;
                reason = why;
                context = request;
            };
        };
        CloseHost cleanup{host};
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
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        status->setText("unchanged");
        host.window()->sleepReader();
        // The sleep sync sends the position first; the stalled server ends it
        // at its budget.
        QVERIFY(host.window()->pendingNetworkSync());
        QCOMPARE(requests, 0);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.saved_locally"), 3000);
        QCOMPARE(requests, enabled ? 1 : 0);
        QCOMPARE(network->requests, 0);
        QCOMPARE(network->releases, 0);
        if (enabled) {
            QCOMPARE(reason, QByteArray("wifi-off at sleep"));
            QVERIFY(context);
        }
        host.window()->wakeReader();
        QVERIFY(!context);
    }
    // Checks the final sync at close and sleep: local position first, a bounded wait and wake during it; `https-*` rows need a real server.
    void kosyncFinal_data() {
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
            for (const auto *state : {"handoff", "busy", "cancel", "deadline", "sleep"}) {
                const auto name = QByteArray("https-manual-") + state;
                QTest::newRow(name.constData()) << QString::fromLatin1(name) << false;
            }
            QTest::newRow("https-switch-close") << "https-switch-close" << false;
            for (const auto *decision : {"keep", "resume", "dismiss", "invalid"}) {
                const auto name = QByteArray("https-choice-") + decision;
                QTest::newRow(name.constData()) << QString::fromLatin1(name) << false;
            }
        }
    }
    void kosyncFinal() {
        QFETCH(QString, transition);
        QFETCH(bool, inFlight);
        const bool live = transition.startsWith("https-");
        if (live) transition.remove(0, 6);
        if (live && transition == "wake-recovery") { verifyKosyncWakeRecovery(); return; }
        if (live && transition.startsWith("manual-")) { verifyKosyncManualHandoff(transition.mid(7)); return; }
        if (live && transition.startsWith("choice-")) {
            verifyKosyncChoice(transition.mid(7));
            return;
        }
        bool filename = transition.startsWith("filename-");
        const bool switchMatching = transition.startsWith("switch-");
        if (switchMatching) transition.remove(0, 7);
        if (filename) transition.remove(0, 9);
        QTcpServer stalled;
        if (!live) QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", live ? qEnvironmentVariable("KOBALT_KOSYNC_TEST_SERVER")
                                          : QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", live ? qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER") : QString("test-reader")},
            {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", true},
            {"match_method", filename ? "filename" : "binary"}}));
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&clock](ReaderHostServices &services) {
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{host};
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
            if (!live) QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrKosyncCancel")));
            QVERIFY(host.session()->go(1));
            QTRY_VERIFY(!host.session()->busy());
        }
        const auto document = filename ? host.session()->syncFilenameDigest() : host.session()->syncDigest();
        const auto position = host.session()->state().value("position").toString();
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        const int openingAttempts = attempts.count();
        QElapsedTimer elapsed;
        elapsed.start();
        QPointer<ReaderWindow> window = host.window();
        if (transition == "close") window->close();
        else window->sleepReader();
        if (!live) QTRY_VERIFY(window && window->pendingNetworkSync());
        if (!live && !inFlight) QTRY_VERIFY_WITH_TIMEOUT(attempts.count() > openingAttempts, 1500);
        QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(), position);
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
            if (!live && transition != "blocked") QVERIFY(runKosyncDeadline(window, 2000));
            QTRY_VERIFY_WITH_TIMEOUT(!window || !window->pendingNetworkSync(), 3500);
            QVERIFY(elapsed.elapsed() < 3500);
            if (transition == "close") QTRY_VERIFY(host.idle());
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
        if (!live && !inFlight) QVERIFY(attempts.count() > openingAttempts);
        if (live) {
            // A second production client observes the final position on the server.
            kosync::Client verify(paths.syncAccount());
            // Closing and sleep write the sync time.
            QVERIFY(verify.lastSync().isValid());
            if (transition != "close") QVERIFY(showsSyncTime(window));
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
        QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(), position);
    }

    // Checks that revoking native reading fails its pending save once, ignores the late reply and still offers Retry for a failed local save.
    void revokedNativeDrainRetainsLocalSave() {
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        struct RemoveFailure {
            std::function<void()> remove;
            ~RemoveFailure() { remove(); }
        } restore{[&] { changeSql("DROP TRIGGER IF EXISTS refuse_revoked_checkpoint"); }};
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        const auto position = host.session()->state().value("position").toString();
        host.progress->holdFinish = true;
        int nativeReplies = 0;
        bool nativeSaved = true;
        host.book()->finishReading([&](bool saved) { ++nativeReplies; nativeSaved = saved; });
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
        host.window()->finishStorageSave([&](bool saved) { ++localReplies; localSaved = saved; }, false);
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
        QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(), position);
        QCOMPARE(host.progress->finishAttempts, 1);
        QCOMPARE(host.progress->stopAttempts, stops);
    }

    // Checks that close waits for a held profile writer, shows the saving notice only after a delay and closes once the writer is free.
    void closeWaitsForLocalSave() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened), closed(&host, &ReaderFixture::closed);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QTRY_VERIFY(host.book()->readingActive());
        std::atomic_bool released{false}, entered{false};
        storage::RecordWriter blocker;
        struct Release {
            std::atomic_bool &released;
            ~Release() { released = true; }
        } release{released};
        blocker.post([&] {
            entered = true;
            while (!released) QThread::msleep(1);
            return true;
        }, {});
        QTRY_VERIFY(entered.load());
        host.window()->close();
        const auto stopped = host.book()->readingMilliseconds();
        host.window()->close();
        QTest::qWait(250);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSaving"));
        QCOMPARE(closed.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrSaving"), 2000);
        auto *saving = host.window()->findChild<QWidget *>("rrSaving");
        QVERIFY(saving->isVisible());
        QCOMPARE(saving->findChild<QLabel *>("rrSavingTitle")->text(), strings::get("closing.saving"));
        QVERIFY(saving->findChild<QLabel *>("rrSavingHint")->isVisible());
        auto *modal = dynamic_cast<ui::ModalOverlay *>(saving);
        QVERIFY(modal && modal->inputBarrier());
        QVERIFY(modal->card()->height() < host.window()->height() / 2);
        QCOMPARE(host.book()->readingMilliseconds(), stopped);
        QVERIFY(!host.window()->flushBeforeStandby());
        released = true;
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
    }

    // Checks that a failed native position or reading-time save keeps the reader open with Retry, then Quit or Return after a failed retry.
    void closeRetriesRequiredNativeSaves_data() {
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
    void closeRetriesRequiredNativeSaves() {
        QFETCH(bool, timeSave);
        QFETCH(bool, discard);
        QFETCH(bool, resume);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
            QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrSaving"), 2000);
            QCOMPARE(closed.count(), 0);
            host.progress->completeFinish(false);
        }
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
        QCOMPARE(closed.count(), 0);
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
            QCOMPARE(host.progress->finishAttempts, 2);
            if (resume) {
                QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveReturn")));
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
        QCOMPARE(host.progress->finishAttempts, 2);
    }

    void quitAfterFailedLocalSaveRetry() {
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
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
        struct RemoveFailure {
            std::function<void()> remove;
            ~RemoveFailure() { remove(); }
        } restore{[&] { changeSql("DROP TRIGGER IF EXISTS refuse_quit_checkpoint"); }};
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->state().value("position") != saved.value("position"));
        host.window()->close();
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
        QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveRetry")));
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
        QCOMPARE(closed.count(), 0);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrCloseSaveQuit")));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(storage::readBookState(paths, identity).object.value("position"), saved.value("position"));
    }

    // Checks that at close the native position write waits for the native reading-time write.
    void closeWaitsForNativeTimeBeforePosition() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        host.progress->holdFinish = host.progress->rejectWriteWhileFinishing = true;
        const auto attempts = host.progress->writeAttempts;
        host.window()->close();
        QTRY_VERIFY(host.progress->finishDone);
        QCOMPARE(host.progress->writeAttempts, attempts);
        QCOMPARE(closed.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(host.window()->findChild<QWidget *>("rrSaving"), 2000);
        QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
        host.progress->holdFinish = false;
        host.progress->completeFinish(true);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(host.progress->writeAttempts, attempts + 1);
        QCOMPARE(host.progress->finishAttempts, 1);
    }

    // Checks that at sleep the native position waits for the reading-time write: immediate, delayed, failed, timed-out and busy-database cases.
    void sleepWaitsForNativeTimeBeforePosition_data() {
        QTest::addColumn<int>("delay");
        QTest::addColumn<bool>("saved");
        QTest::addColumn<int>("contention");
        QTest::newRow("immediate") << 0 << true << 0;
        QTest::newRow("delayed") << 100 << true << 0;
        QTest::newRow("failed") << 100 << false << 0;
        QTest::newRow("timeout") << 2800 << true << 0;
        QTest::newRow("busy-after-drain") << 0 << false << 150;
        QTest::newRow("persistent-busy") << 0 << false << 2800;
    }
    void sleepWaitsForNativeTimeBeforePosition() {
        QFETCH(int, delay);
        QFETCH(bool, saved);
        QFETCH(int, contention);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
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
        host.progress->holdFinish = delay != 0;
        host.progress->failFinish = !saved;
        host.progress->rejectWriteWhileFinishing = true;
        if (contention) {
            host.progress->failWrite = true;
            host.progress->writeError = strings::get("error.nickel_database_is_busy_local_position_was_retained");
            QTimer::singleShot(contention, host.window(), [progress = host.progress] { progress->failWrite = false; });
        }
        const auto overlap = std::make_shared<bool>(false);
        const auto progress = host.progress;
        if (delay) {
            QTimer::singleShot(delay, host.window(), [progress, overlap, saved] {
                *overlap = progress->writeAttempts != 0;
                progress->holdFinish = false;
                progress->completeFinish(saved);
            });
        }
        QElapsedTimer elapsed;
        elapsed.start();
        host.window()->sleepReader();
        QCOMPARE(storage::readBookState(testPaths(), identity).object.value("position").toString(), position);
        QCOMPARE(host.progress->finishAttempts, 1);
        if (contention) {
            QVERIFY(host.progress->writeAttempts > 1);
            QCOMPARE(host.progress->writes, contention < 2500 ? 1 : 0);
            QVERIFY(elapsed.elapsed() >= qMin(contention, 2500) - 100);
            QVERIFY(elapsed.elapsed() < 2800);
            QTRY_VERIFY(!host.progress->failWrite);
            return;
        }
        QCOMPARE(host.progress->writeAttempts, 1);
        if (delay > 2500) {
            QVERIFY(elapsed.elapsed() < delay);
            QCOMPARE(host.progress->writes, 0);
            QVERIFY(host.progress->finishPending);
            QTRY_VERIFY(!host.progress->finishPending);
            QVERIFY(*overlap);
        } else {
            QVERIFY(!*overlap);
            QCOMPARE(host.progress->writes, 1);
            QVERIFY(!host.progress->finishPending);
        }
    }

    // Checks that opening marks the book as reading in Nickel once, writes no position and does not retry after a failure.
    void nativeOpeningStatus_data() {
        QTest::addColumn<bool>("fails");
        QTest::newRow("saved") << false;
        QTest::newRow("failed") << true;
    }
    void nativeOpeningStatus() {
        QFETCH(bool, fails);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        QTRY_VERIFY2_WITH_TIMEOUT(activeReaderTimers(host.window()).isEmpty(),
                                  qPrintable("Active reader timers: " + activeReaderTimers(host.window()).join(", ")),
                                  10000);
        QCOMPARE(host.progress->openingAttempts, 1);
        QCOMPARE(host.progress->writeAttempts, 0);
        host.window()->preparePresentation([](bool) {});
        QTest::qWait(50);
        QCOMPARE(host.progress->openingAttempts, 1);
        QVERIFY(!host.window()->findChild<QTimer *>("rrNativeOpeningSaveTimer"));
    }

    void nativeProgressWaitsForSleep_data() {
        QTest::addColumn<QString>("outcome");
        for (const auto *outcome : {"saved", "newer", "failed"}) {
            QTest::newRow(outcome) << QString::fromLatin1(outcome);
        }
    }
    // Nickel's progress is written on the GUI thread, so reading pauses save
    // only the local position. Sleep writes Nickel's copy.
    void nativeProgressWaitsForSleep() {
        QFETCH(QString, outcome);
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        QVERIFY(host.window()->flushBeforeStandby());
        QCOMPARE(host.progress->writes, writes);
        QString error;
        const auto expected = host.session()->savedNativePosition(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!expected.isEmpty());
        const double percent = 100.0 * host.session()->page() / host.session()->pages();
        if (outcome == "newer") {
            host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true});
        }
        host.progress->failWrite = outcome == "failed";
        const auto before = host.progress->current;
        host.window()->sleepReader();
        QCOMPARE(host.progress->writes, writes + (outcome == "saved" ? 1 : 0));
        if (outcome == "saved") {
            QCOMPARE(host.progress->current.location, expected);
            QCOMPARE(host.progress->percent, percent);
        } else {
            QVERIFY(host.progress->current == before);
        }
        QCOMPARE(stored(), anchor);
    }

    // Checks that a failed appearance save stops after three attempts, page turns and background saves do not retry it, and a commit does.
    void appearanceRetryCooldown() {
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy() && !host.book()->savesPending());

        std::atomic<int> failures{0}, attempts{0};
        initializeDiagnostics(root.path() + "/diagnostics", false);
        pauseDiagnostics(false);
        setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event") == QStringLiteral("appearance-settings-unsaved")) {
                attempts = event.value("attempts").toInt();
                ++failures;
            }
        });
        struct ResetDiagnostics {
            ~ResetDiagnostics() { setDiagnosticObserver({}); pauseDiagnostics(true); }
        } resetDiagnostics;
        const auto preferences = paths.preferences(), backup = preferences + ".retry-test";
        const auto original = storage::readPreferences(preferences).object;
        QVERIFY(QFile::rename(preferences, backup));
        struct RestorePreferences {
            QString preferences, backup;
            ~RestorePreferences() { QDir().rmdir(preferences); QFile::rename(backup, preferences); }
        } restore{preferences, backup};
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
            QTRY_VERIFY(!host.session()->busy() && host.session()->state().value("position") != before);
            QTRY_VERIFY(!host.book()->savesPending());
        };
        for (int i = 0; i < 24; ++i) turn(i % 2 ? -1 : 1);
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
        for (int i = 0; i < 6; ++i) turn(i % 2 ? -1 : 1);
        host.book()->saveReadingActivity();
        QTRY_VERIFY(!host.book()->savesPending());
        QCOMPARE(storage::readPreferences(preferences).object, original);

        // An explicit commit can save during cooldown and clears its failure.
        host.window()->checkpoint();
        QCOMPARE(storage::readPreferences(preferences).object.value(appearance::pageMarginsKey).toObject()
                     .value("top_margin_pixels"), expectedMargin);
        QVERIFY(host.session()->setVerticalMargins(readingPixels(56)));
        QTRY_VERIFY(!host.session()->busy());
        host.book()->rememberAppearance();
        QTRY_VERIFY(!host.book()->savesPending());
        QCOMPARE(storage::readPreferences(preferences).object.value(appearance::pageMarginsKey).toObject()
                     .value("top_margin_pixels"), host.book()->checkpointState().value("top_margin_pixels"));
    }

    // Checks that settings that become unusable while a book is open do not fail the close and stay unchanged.
    void closeWithUnusableSettings() {
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
            QVERIFY(settings.open(QIODevice::WriteOnly | QIODevice::Truncate) && settings.write(future) == future.size());
        }
        QVERIFY(host.session()->setVerticalMargins(readingPixels(40)));
        QTRY_VERIFY(!host.session()->busy());
        QJsonObject snapshot;
        QVERIFY(host.book()->checkpoint(&snapshot));
        bool done = false, saved = false;
        host.book()->checkpointForClose([&](bool result) { done = true; saved = result; });
        QTRY_VERIFY(done);
        QVERIFY(saved);
        QFile settings(paths.preferences());
        QVERIFY(settings.open(QIODevice::ReadOnly));
        QCOMPARE(settings.readAll(), future);
    }

    // Checks that the profile-reset notice shows in the open reader with the backup folder and closes on confirm.
    void recoveryNoticeAfterFirstPage() {
        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        storage::ProfileRecovery recovery;
        recovery.settings = true;
        const auto startup = profileRecoveryNotice(recovery, ".adds/kobalt/backups");
        const auto message = startup.message;
        host.configureServices = [startup](ReaderHostServices &services) { services.startupNotice = startup; };
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QWidget *notice = nullptr;
        QTRY_VERIFY((notice = host.window()->findChild<QWidget *>("rrRecoveryNotice")) && notice->isVisible());
        auto *text = host.window()->findChild<QLabel *>("rrRecoveryNoticeMessage");
        QVERIFY(text && text->text() == message && message.contains(".adds/kobalt/backups"));
        auto *confirm = host.window()->findChild<QPushButton *>("rrRecoveryNoticeConfirm");
        QVERIFY(confirm);
        confirm->click();
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrRecoveryNotice"));
    }

    void benchmarkNoticeAfterRun() {
        const auto completed = qt::object({{"id", "page.turn"}, {"status", "completed"}});
        const auto skipped = qt::object({{"id", "panel.footnote"}, {"status", "skipped"}});
        const auto failed =
            qt::object({{"id", "book.close"}, {"status", "failed"}, {"reason", "the reader did not close"}});
        const QString folder = ".adds/kobalt/diagnostics";
        const auto finished = benchmarkNotice({completed, skipped}, true, folder);
        QCOMPARE(finished.title, strings::get("benchmark.finished"));
        QVERIFY(finished.message.contains(strings::get("benchmark.counts").arg(1).arg(1).arg(0)) &&
                finished.message.contains(folder));
        const auto aborted = benchmarkNotice({completed, failed, skipped}, true, folder);
        QCOMPARE(aborted.title, strings::get("benchmark.failed"));
        QVERIFY(aborted.message.contains("book.close") && aborted.message.contains("the reader did not close") &&
                aborted.message.contains(folder));
        QCOMPARE(benchmarkNotice({completed}, false, folder).title, strings::get("benchmark.failed"));
        QCOMPARE(benchmarkNotice({}, true, folder).title, strings::get("benchmark.failed"));

        const auto paths = testPaths();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
    void activityBatchesAndLowBattery() {
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        BatteryStatus battery{50, false, false};
        std::function<void()> batteryChanged;
        host.configureServices = [&](ReaderHostServices &services) {
            services.battery = [&] { return battery; };
            services.observeBattery = [&](QObject *, std::function<void()> changed) { batteryChanged = changed; };
        };
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/cover.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy() && host.session()->resourceStats().value("pending_images").toInt() == 0);
        const auto stored = [&] { return storage::readBookState(paths, identity).object.value("position").toString(); };
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
        QTRY_VERIFY2_WITH_TIMEOUT(activeReaderTimers(host.window()).isEmpty(),
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
        QVERIFY(!host.window()->flushBeforeStandby());
        QTRY_COMPARE(stored(), current());
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QVERIFY(!storage::profileHasUnsyncedWrites());

        int failures = 0;
        host.book()->onCheckpointFailed([&] { ++failures; });
        storage::closeProfile();
        QVERIFY(QFile::setPermissions(paths.database(), QFileDevice::ReadOwner));
        turn();
        QVERIFY(!host.window()->flushBeforeStandby());
        QTRY_COMPARE(failures, 1);
        QVERIFY(host.book()->activitySaveCoolingDown());
        QTRY_VERIFY(host.window()->flushBeforeStandby());
        QVERIFY(QFile::setPermissions(paths.database(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
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

    void usbFinalSave_data() {
        QTest::addColumn<QString>("outcome");
        for (const auto &outcome : {"saved", "failed", "persistent-failure", "persistent-retry", "battery-low", "battery-low-held", "newer", "timeout"}) {
            QTest::newRow(outcome) << QString::fromLatin1(outcome);
        }
    }
    void usbFinalSave() {
        QFETCH(QString, outcome);
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        // Ends before the cleanup closes the reader, so a failed row never
        // leaves the engine worker held.
        struct ResetDelay {
            ~ResetDelay() {
                delayUsbEncoding = false;
                releaseUsbEncoding = true;
            }
        } resetDelay;
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        host.positions.encode = usbPosition;
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        const int writes = host.progress->writes;
        host.progress->failWrite = outcome == "failed" || outcome.startsWith("persistent-") || outcome == "battery-low";
        if (outcome == "newer") {
            host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true});
        }
        const auto before = host.progress->current;
        usbEncodingReturned = releaseUsbEncoding = false;
        delayUsbEncoding = outcome == "timeout";
        const int attempts = host.progress->writeAttempts;
        int completed = 0;
        QElapsedTimer elapsed;
        elapsed.start();
        host.progress->holdFinish = outcome == "battery-low-held";
        if (outcome.startsWith("battery-low"))
            host.window()->prepareShutdown([&] { ++completed; }, true);
        else
            host.window()->prepareUsb([&] { ++completed; });
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
            QCOMPARE(host.progress->writeAttempts, attempts);
            QVERIFY(!host.window()->findChild<QPushButton *>("rrCloseSaveRetry"));
            host.progress->holdFinish = false;
            host.progress->completeFinish(true);
            elapsed.restart();
        }
        if (outcome == "failed") {
            QTRY_VERIFY(host.progress->writeAttempts > attempts);
            QCOMPARE(completed, 0);
            host.progress->failWrite = false;
        }
        if (outcome.startsWith("persistent-")) {
            QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrCloseSaveQuit"));
            QCOMPARE(completed, 0);
            QCOMPARE(host.progress->writeAttempts - attempts, 3);
            QTest::qWait(300);
            QCOMPARE(host.progress->writeAttempts - attempts, 3);
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
            QVERIFY(!usbEncodingReturned);
            releaseUsbEncoding = true;
            QTRY_VERIFY(usbEncodingReturned.load());
            // Drain the late engine callback while the window remains alive.
            QTest::qWait(100);
        }
        delayUsbEncoding = false;
        QCOMPARE(completed, 1);
        if (outcome == "persistent-failure" || outcome == "battery-low") {
            QCOMPARE(host.progress->writeAttempts - attempts, 3);
            QVERIFY(host.window()->property("reader_transition_save_failed").toBool());
            const int terminalAttempts = host.progress->writeAttempts;
            QTest::qWait(400);
            QCOMPARE(host.progress->writeAttempts, terminalAttempts);
        }
        QCOMPARE(host.progress->writes, writes + (outcome == "saved" || outcome == "failed" || outcome == "persistent-retry" || outcome == "battery-low-held" ? 1 : 0));
        if (outcome != "saved" && outcome != "failed" && outcome != "persistent-retry" && outcome != "battery-low-held") {
            QVERIFY(host.progress->current == before);
        }
        const auto local = storage::readBookState(paths, identity).object;
        QVERIFY(!local.value("position").toString().isEmpty());
    }

    void progressChoiceKeepAndGo_data() {
        QTest::addColumn<bool>("go");
        QTest::newRow("keep") << false;
        QTest::newRow("go") << true;
    }
    void progressChoiceKeepAndGo() {
        QFETCH(bool, go);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        host.window()->progressTick();
        QTest::qWait(200);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        // Another device saved a position later in the book.
        host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.after"));
        const int before = host.session()->page();
        auto *choice = card->findChild<QPushButton *>(go ? "rrGoToSyncedPosition" : "rrKeepPosition");
        QVERIFY(choice);
        QVERIFY(click(canvas, choice));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QTRY_VERIFY(!host.session()->busy());
        if (go)
            QVERIFY(host.session()->page() > before);
        else
            QCOMPARE(host.session()->page(), before);
        // The choice is remembered: the same saved position does not ask again.
        host.window()->progressTick();
        QTest::qWait(200);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    // Checks that a newer synced position waits while Settings covers the page, and asks once Settings closes.
    void progressChoiceWaitsForSettings() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        QVERIFY(page);
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *close = host.window()->findChild<QPushButton *>("rrSettingsClose");
        QTRY_VERIFY(close && close->isVisible());
        // The platform reports a newer position while Settings is open.
        host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true});
        host.window()->progressTick();
        QTest::qWait(200);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        // Closing Settings asks without another progress check.
        QVERIFY(click(canvas, close));
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    void finishedSyncWithoutPositionKeepsLocalReading() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        host.progress->publish({identity, {}, 200, 2, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible());
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
        host.window()->progressTick();
        QTest::qWait(200);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QCOMPARE(host.session()->state().value("position"), original);
    }

    void progressChoiceForOlderAlert() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        host.progress->publish({identity, "OEBPS/c2.xhtml#kobo.5.1:3", 200, 1, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        // A newer position arrives while the first alert is open. Keeping the
        // first one must not accept the newer one, so the reader asks again.
        // A tick while the engine is busy hides the alert until the next tick,
        // so tick only when it is idle.
        QPointer<QWidget> first(card);
        // Positions no other test has accepted, so neither counts as already chosen.
        host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.12.1:3", 300, 1, true});
        QTRY_VERIFY(!host.session()->busy());
        host.window()->progressTick();
        QTRY_VERIFY(first && first->isVisible());
        auto *keep = first->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!first);
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        auto *keepNewer = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keepNewer);
        QVERIFY(click(canvas, keepNewer));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }

    void progressChoiceUnavailablePosition() {
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
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
        host.progress->publish({identity, "OEBPS/missing.xhtml#kobo.1.1:0", 200, 1, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.unlocated"));
        auto *go = card->findChild<QPushButton *>("rrGoToSyncedPosition");
        QVERIFY(go);
        QVERIFY(click(canvas, go));
        QTRY_COMPARE_WITH_TIMEOUT(message->text(), strings::get("sync.position_unavailable"), 10000);
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

    void appearanceOpeningDoesNotRepaintToolbar_data() {
        QTest::addColumn<QString>("buttonName");
        QTest::newRow("text") << "rrNavText";
        QTest::newRow("layout") << "rrNavLayout";
        QTest::newRow("stats") << "rrNavStats";
        QTest::newRow("font-picker-back") << "rrFontClose";
    }
    void appearanceOpeningDoesNotRepaintToolbar() {
        QFETCH(QString, buttonName);
        ReaderFixture host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
    void annotationListResolvesPagesOnDemand() {
        QTemporaryDir directory;
        storage::Paths paths{directory.path()};
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
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
        const QString identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        HighlightStore store(paths.annotations(), identity, host.session()->sourceHash(), "test", paths.database());
        QVERIFY(store.load());
        QVector<Highlight> entries;
        for (int i = 0; i < 2000; ++i) {
            const bool first = i == 1999;
            const auto &hit = first ? hits.first() : hits.last();
            entries.append({QString("lazy-%1").arg(i), hit.start, hit.end, hit.text,
                            first ? QString("First passage") : QString("Later passage"),
                            QDateTime::currentDateTimeUtc()});
        }
        store.setHighlights(entries);
        QVERIFY(store.save());
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        done = false;
        bool prepared = false;
        host.window()->preparePresentation([&](bool ready) { prepared = ready; done = true; });
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
        struct ResumeEngine {
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
            host.window()->findChild<QPushButton *>(pass == 0 ? "rrBookmarks" : "rrAnnotationsTab")->click();
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
            auto *first = host.window()->findChild<QPushButton *>("rrAnnotation0");
            QVERIFY(first && first->isVisible() && first->isEnabled());
            QCOMPARE(first->accessibleName(), QString("First passage"));
            if (pass == 0) {
                host.window()->findChild<QPushButton *>("rrBookmarkListClose")->click();
                QVERIFY(!list->isVisible());
                auto appearance = host.session()->state();
                appearance["font_pixels"] = appearance.value("font_pixels").toInt() + 4;
                host.session()->apply(appearance);
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

    // Checks that the footer, progress bar, Stats, sleep snapshot and saved book percent agree at the first, a middle and the last page.
    void progressPercentagesAgree_data() {
        QTest::addColumn<int>("position");
        QTest::newRow("first") << 0;
        QTest::newRow("middle") << 1;
        QTest::newRow("last") << 2;
    }
    void progressPercentagesAgree() {
        QFETCH(int, position);
        const auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.preferences(), {{sleep::preferenceKey, "page"}}));
        auto presentation = std::make_shared<test::SleepPresentationState>();
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [presentation](ReaderHostServices &services) {
            services.presentation = [presentation](QWidget *) {
                return std::make_unique<test::HostPresentation>(presentation);
            };
        };
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(nativeSize);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(host.session()->pages() > 7);
        const int target = position == 0 ? 0 : position == 2 ? host.session()->pages() - 1 : host.session()->pages() / 7;
        if (host.session()->page() != target) QVERIFY(host.session()->go(target));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->page(), target);
        const int percent = qRound(100.0 * target / (host.session()->pages() - 1));
        auto *page = dynamic_cast<ReadingPage *>(host.window()->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QTRY_COMPARE(page->slotContents[5].text, strings::get("common.percent").arg(percent));
        QCOMPARE(qRound(page->progressFraction * 100), percent);
        page->tapped(page->rect().center());
        auto *stats = host.window()->findChild<QPushButton *>("rrNavStats");
        QVERIFY(stats);
        stats->click();
        auto *graph = host.window()->findChild<QWidget *>("rrChapterGraph");
        QTRY_VERIFY(graph && graph->isVisible());
        auto *bookPercent = graph->findChild<QLabel *>("rrGraphBookPercent");
        QVERIFY(bookPercent);
        QCOMPARE(bookPercent->text(), strings::get("common.percent").arg(percent));
        stats->click();
        QTRY_VERIFY(!graph->isVisible());
        QTRY_VERIFY(host.window()->readyForPresentation());
        const auto snapshot = host.window()->powerScreenSnapshot();
        QVERIFY(!snapshot.image.isNull());
        QCOMPARE(snapshot.percent, percent);
        QCOMPARE(snapshot.progress, strings::get("sleep.percent_read").arg(percent));
        host.window()->checkpoint();
        const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        QCOMPARE(storage::readBookState(paths, identity).object.value("percent").toInt(-1), percent);
    }
    void annotationFramesKeepFirstPaintComplete_data() {
        QTest::addColumn<QString>("markup");
        for (const auto *name : {"none", "saved", "draft", "elsewhere", "elsewhere-256", "grayscale"}) {
            QTest::newRow(name) << QString(name);
        }
    }
    void annotationFramesKeepFirstPaintComplete() {
        QFETCH(QString, markup);
        QTemporaryDir directory;
        storage::Paths paths{directory.path()};
        QVERIFY(storage::writeJson(paths.preferences(), {{"highlight_help_seen", true}}));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        if (markup == "grayscale") {
            host.configureServices = [](ReaderHostServices &services) {
                services.colorDisplay = [] { return false; };
            };
        }
        CloseHost cleanup{host};
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
        const QString identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
        HighlightStore store(paths.annotations(), identity, host.session()->sourceHash(), "test", paths.database());
        if (markup != "none") {
            Highlight entry{"frame-one", passage.start, passage.end, passage.text, {}, QDateTime::currentDateTimeUtc()};
            entry.color = "blue";
            if (markup.startsWith("elsewhere")) {
                entry.start = hits.last().start;
                entry.end = hits.last().end;
                entry.text = "reader";
            }
            QVector<Highlight> entries{entry};
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
        struct ResumePreparation {
            ~ResumePreparation() {
                engineActivity().pause(false);
            }
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
                host.session()->rangeRects(hits.first().start, hits.first().end, [&](QVector<QRect>) {
                    finished = true;
                });
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
        if (markup == "grayscale") {
            QVector<QRect> rects;
            done = false;
            host.session()->rangeRects(hits.first().start, hits.first().end, [&](QVector<QRect> value) {
                rects = value;
                done = true;
            });
            QTRY_VERIFY(done && !rects.isEmpty());
            page->annotationRequested();
            const QPoint point = rects.first().center() + QPoint(0, page->topHeight());
            for (const auto &style : {"underline", "dotted"}) {
                page->held(point - QPoint(0, page->topHeight()));
                const QString name = "rrAnnotationStyle_" + QString(style);
                QTRY_VERIFY(host.window()->findChild<QAbstractButton *>(name) &&
                            host.window()->findChild<QAbstractButton *>(name)->isVisible());
                QVERIFY(!host.window()->findChild<QWidget *>("rrAnnotationPalette"));
                QVERIFY(!host.window()->findChild<QWidget *>("rrAnnotationSave"));
                host.window()->findChild<QAbstractButton *>(name)->click();
                QVERIFY(host.window()->findChild<QWidget *>("rrAnnotationPopover")->isVisible());
                host.window()->findChild<QPushButton *>("rrAnnotationApply")->click();
                QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrAnnotationPopover")->isVisible());
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
            host.session()->rangeRects(hits.first().start, hits.first().end, [&](QVector<QRect> value) {
                rects = value;
                done = true;
            });
            QTRY_VERIFY(done && !rects.isEmpty());
            page->annotationRequested();
            page->held(rects.first().center());
            auto edit = [&] {
                return host.window()->findChild<QPushButton *>("rrAnnotationEdit");
            };
            QTRY_VERIFY(edit() && edit()->isVisible());
            edit()->click();
            QVERIFY(barrier());
        }
        class FramePaints final : public QObject {
        public:
            ReaderFixture &host;
            ReadingPage &page;
            int destination = -1;
            QVector<QImage> images;
            QVector<qint64> milliseconds;
            QElapsedTimer elapsed;
            FramePaints(ReaderFixture &host, ReadingPage &page) : host(host), page(page) {}
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::Paint && !host.session()->busy() &&
                    host.session()->page() == destination) {
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
                      firstComplete < 0 ? -1LL : paints.milliseconds[firstComplete], firstComplete == 0);
            }
        }
        QVERIFY2(complete, "The first painted page must already contain its complete saved/draft markup");
        if (markup == "saved" || markup == "draft") {
            paints.images.clear();
            paints.milliseconds.clear();
            paints.destination = host.session()->page();
            paints.elapsed.start();
            auto appearance = host.session()->state();
            appearance["font_pixels"] = appearance.value("font_pixels").toInt() + 4;
            host.session()->apply(appearance);
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
            for (const auto &highlight : store.highlights()) persisted.append(highlightJson(highlight));
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
            HighlightStore reopened(paths.annotations(), identity, host.session()->sourceHash(), "test", paths.database());
            QVERIFY(reopened.load());
            QJsonArray actual;
            for (const auto &highlight : reopened.highlights()) actual.append(highlightJson(highlight));
            QCOMPARE(actual, persisted);
            QVERIFY(!host.window()->findChild<QWidget *>("rrAnnotationPopover")->isVisible());
        }
    }

private:
    // A successful sync sets the sync time only in memory. The reader writes
    // it to the account file when the book closes or the account changes, so
    // read it from the reader's KOSync page.
    static bool showsSyncTime(QWidget *window) {
        auto *label = window ? window->findChild<QLabel *>("rrKosyncLastSync") : nullptr;
        return label && label->text() != strings::get("kosync.last_synced").arg(strings::get("kosync.never"));
    }
    void verifyKosyncManualHandoff(const QString &state) {
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", qEnvironmentVariable("KOBALT_KOSYNC_TEST_SERVER")},
            {"username", qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER")}, {"key", QString(32, 'a')},
            {"device_id", "manual-handoff"}, {"automatic", false}}));
        auto network = std::make_shared<SyncNetworkState>();
        bool suspended = false;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.suspended = [&] { return suspended; };
        };
        CloseHost cleanup{host};
        struct ResumeEngine { ~ResumeEngine() { engineActivity().pause(false); } } resume;
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        kosync::Client seed(paths.syncAccount());
        bool seeded = false;
        QString failure;
        seed.push(host.session()->syncDigest(), {host.session()->state().value("position").toString(), {}, {}, 0},
                  [&](QString error) { failure = error; seeded = true; });
        QTRY_VERIFY_WITH_TIMEOUT(seeded, 12000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        initializeDiagnostics(paths.directory + "/diagnostics");
        pauseDiagnostics(false);
        struct PauseDiagnostics { ~PauseDiagnostics() { pauseDiagnostics(true); } } diagnostics;
        const auto events = [&] {
            flushDiagnostics();
            QFile file(paths.directory + "/diagnostics/events.jsonl");
            if (!file.open(QIODevice::ReadOnly)) return QByteArray();
            return file.readAll();
        };
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
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
        if (suspended) host.window()->hide();
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
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.saved_locally"), 11000);
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
            QCOMPARE(popup->findChild<QLabel *>("rrModalTitle")->text(), strings::get("kosync.completed"));
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
            if (event.value("event").toString() == "kosync-request-started" && event.value("method").toString() == "GET")
                ++reads;
        }
        QCOMPARE(reads, 1);
    }
    void verifyKosyncNetwork(const QString &scenario) {
        QTcpServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QSignalSpy attempts(&stalled, &QTcpServer::newConnection);
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://127.0.0.1:%1").arg(stalled.serverPort())},
            {"username", "test-reader"}, {"key", QString(32, 'a')},
            {"device_id", "test-device"}, {"automatic", true}}));
        auto network = std::make_shared<SyncNetworkState>();
        qint64 clock = 0;
        ReaderFixture host(paths, testFonts, testPlaceholder);
        host.configureServices = [network, &clock](ReaderHostServices &services) {
            services.syncNetwork = [network] { return std::make_unique<TestSyncNetwork>(network); };
            services.monotonicMilliseconds = [&clock] { return clock; };
        };
        CloseHost cleanup{host};
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
        QTRY_VERIFY((popup = host.window()->findChild<QWidget *>("rrKosyncOpening")) && popup->isVisible());
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
            const auto identity = QUrl::fromLocalFile(QFileInfo(textBook).absoluteFilePath()).toString();
            QCOMPARE(storage::readBookState(paths, identity).object.value("position").toString(), position);
        } else if (scenario == "deadline") {
            // The connection never answers, so the sync deadline cancels it.
            QVERIFY(runKosyncDeadline(host.window()));
            QTRY_VERIFY(!popup || !popup->isVisible());
            QCOMPARE(network->cancellations, 1);
            QCOMPARE(attempts.count(), 0);
        } else {
            QVERIFY(click(canvas, popup->findChild<QPushButton *>("rrKosyncCancel")));
            QCOMPARE(network->cancellations, 1);
            network->connected = true;
            lateReady(true);
            QCoreApplication::processEvents();
            QCOMPARE(attempts.count(), 0);
            network->connected = false;
            // A later periodic sync without Wi-Fi permission stays local.
            auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
            QVERIFY(status);
            status->setText("unchanged");
            clock += 16 * 60000;
            QVERIFY(host.session()->turn(1));
            QTRY_VERIFY(!host.session()->busy());
            QCoreApplication::processEvents();
            QCOMPARE(network->requests, 1);
            QCOMPARE(status->text(), strings::get("kosync.saved_locally"));
            QCOMPARE(attempts.count(), 0);
            if (scenario == "login") {
                // Editing credentials exposes Sign in even for the saved account.
                auto *username = host.window()->findChild<QPushButton *>("rrKosyncUsername");
                QVERIFY(username);
                username->click();
                auto *input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
                QVERIFY(input);
                input->setText("another-reader");
                host.window()->findChild<QPushButton *>("rrKosyncEditSave")->click();
                QTRY_VERIFY(!host.window()->findChild<QLineEdit *>("rrKosyncInput"));
                host.window()->findChild<QPushButton *>("rrKosyncPassword")->click();
                input = host.window()->findChild<QLineEdit *>("rrKosyncInput");
                QVERIFY(input);
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
                // Password consumption makes the next sign-in invalid. It must
                // report that locally without opening another networking dialog.
                host.window()->findChild<QPushButton *>("rrKosyncLogin")->click();
                QCOMPARE(network->requests, 2);
            }
        }
        QVERIFY(!kosync::Client(paths.syncAccount()).lastSync().isValid());
    }
    void verifyKosyncWakeRecovery() {
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
                    if (upstream->state() == QAbstractSocket::ConnectedState) upstream->write(incoming->readAll());
                });
                connect(upstream, &QTcpSocket::connected, incoming, [incoming, upstream] { upstream->write(incoming->readAll()); });
                connect(upstream, &QTcpSocket::readyRead, incoming, [incoming, upstream] { incoming->write(upstream->readAll()); });
                connect(incoming, &QTcpSocket::disconnected, incoming, &QObject::deleteLater);
                connect(upstream, &QTcpSocket::disconnected, incoming, &QObject::deleteLater);
                upstream->connectToHost(destination.host(), quint16(destination.port()));
            }
        });
        QVERIFY(relay.listen(QHostAddress::LocalHost));
        const auto port = relay.serverPort();
        auto paths = testPaths();
        QVERIFY(storage::writeJson(paths.syncAccount(), {
            {"schema", 1}, {"server", QString("https://localhost:%1").arg(port)},
            {"username", qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER")}, {"key", QString(32, 'a')},
            {"device_id", "wake-recovery-device"}, {"automatic", true}}));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(textBook, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        // Isolate the initial state from earlier rows using the same server account.
        kosync::Client seed(paths.syncAccount());
        bool seeded = false;
        QString seedError;
        seed.push(host.session()->syncDigest(), {host.session()->state().value("position").toString(), {}, {}, 0},
                  [&](QString error) { seedError = error; seeded = true; });
        QTRY_VERIFY_WITH_TIMEOUT(seeded, 12000);
        QVERIFY2(seedError.isEmpty(), qPrintable(seedError));
        Canvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto *status = host.window()->findChild<QLabel *>("rrKosyncStatus");
        QVERIFY(status);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 12000);
        host.window()->sleepReader();
        QTRY_VERIFY_WITH_TIMEOUT(!host.window()->pendingNetworkSync(), 3500);
        const auto position = host.session()->state().value("position").toString();
        relay.close();
        for (const auto &incoming : connections) if (incoming) incoming->abort();
        QCoreApplication::processEvents();
        const int before = accepted;
        QTimer::singleShot(1000, &relay, [&] { QVERIFY(relay.listen(QHostAddress::LocalHost, port)); });
        host.window()->wakeReader();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), strings::get("kosync.up_to_date"), 8000);
        QVERIFY(accepted > before);
        QCOMPARE(host.session()->state().value("position").toString(), position);
    }
    void verifyKosyncChoice(const QString &decision) {
        const bool forward = decision == "resume";
        const bool invalid = decision == "invalid";
        const QSize size(800, 1136);
        auto paths = testPaths();
        QJsonObject account{{"schema", 1}, {"server", qEnvironmentVariable("KOBALT_KOSYNC_TEST_SERVER")},
            {"username", qEnvironmentVariable("KOBALT_KOSYNC_TEST_USER")}, {"key", QString(32, 'a')},
            {"device_id", "choice-local"}, {"automatic", false}};
        QVERIFY(storage::writeJson(paths.syncAccount(), account));
        ReaderFixture host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &ReaderFixture::opened);
        host.open(fixture(), size);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        Canvas canvas(host.window(), size);
        canvas.resize(size);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const int count = host.session()->pages();
        QVERIFY(count > 10);
        QVERIFY(host.session()->go(int(count * (forward ? 0.8 : 0.2))));
        QTRY_VERIFY(!host.session()->busy());
        const auto remotePosition = invalid ? QString("/not/a/real/position") : host.session()->state().value("position").toString();
        const auto remoteChapter = host.session()->chapterTitle();
        // A contradictory percentage must not reverse the source-order warning.
        const double remotePercent = decision == "dismiss" ? 0.99 : double(host.session()->page()) / (host.session()->pages() - 1);
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
        other.push(document, {remotePosition, {}, {}, remotePercent}, [&](QString error) { failure = error; completed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 12000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        auto *sync = host.window()->findChild<QPushButton *>("rrKosyncNow");
        QVERIFY(sync);
        sync->click();
        QPointer<QWidget> widget;
        QTRY_VERIFY((widget = host.window()->findChild<QWidget *>("rrKosyncChoice")) && widget->isVisible());
        auto *dialog = dynamic_cast<ui::ChoiceModal *>(widget.data());
        QVERIFY(dialog);
        auto *description = dialog->descriptionLabel();
        const auto direction = strings::get(invalid ? "kosync.direction_unknown" : forward ? "kosync.direction_forward" : "kosync.direction_backward");
        QTRY_VERIFY(description->text().contains(direction));
        QVERIFY(description->text().contains(localChapter));
        QVERIFY(description->text().contains(invalid ? strings::get("kosync.chapter_unavailable") : remoteChapter));
        QVERIFY(description->text().contains("Crosspoint X4"));
        QVERIFY(description->text().contains("Synced: "));
        QVERIFY(description->text().contains("On this device · " + QLocale().toString(localPercent, 'f', 1) + "%"));
        QVERIFY(description->text().contains("Synced position · " + QLocale().toString(remotePercent * 100, 'f', 1) + "%"));
        QCOMPARE(description->textFormat(), Qt::PlainText);
        QVERIFY(host.window()->rect().contains(dialog->card()->geometry()));
        QCOMPARE(description->visibleRegion().boundingRect(), description->rect());
        QCOMPARE(host.session()->state().value("position").toString(), localPosition);
        if (!invalid && !forward) {
            QCOMPARE(dialog->findChild<QLabel *>("rrModalTitle")->text(), strings::get("kosync.resume_backward_title"));
            QCOMPARE(dialog->focusWidget(), dialog->secondaryButton());
        }
        const auto capture = qEnvironmentVariable("KOBALT_KOSYNC_CHOICE_CAPTURE");
        if (!capture.isEmpty() && (decision == "keep" || forward))
            QVERIFY(host.window()->grab().save(capture + (forward ? "-forward.png" : "-backward.png")));
        if (decision == "dismiss") dialog->dismissed();
        else if (decision == "keep") dialog->secondaryButton()->click();
        else dialog->primaryButton()->click();
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
        other.pull(document, [&](kosync::Progress value, QString error) { received = value; failure = error; completed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 12000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(received.position, decision == "keep" ? localPosition : remotePosition);
    }
    std::unique_ptr<QTemporaryDir> directory;
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    NativeTestApplication app(argc, argv);
    if (QGuiApplication::platformName() != "minimal")
        return 2;
    app.setStyle("Fusion");
    if (!qEnvironmentVariableIsEmpty("KOBALT_KOSYNC_TEST_CA")) {
        auto ssl = QSslConfiguration::defaultConfiguration();
        auto authorities = ssl.caCertificates();
        const auto certificates = QSslCertificate::fromPath(qEnvironmentVariable("KOBALT_KOSYNC_TEST_CA"));
        if (certificates.isEmpty()) return 2;
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
