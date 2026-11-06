#include "platforms/kobo/common/home_sleep_screen.h"
#include "platforms/kobo/common/native_ui.h"
#include "platforms/kobo/common/window_presentation.h"
#include "app/reader/views/sleep_screen.h"
#include "ui/style/fonts.h"
#include "assets/assets.h"
#include "storage/sleep_snapshot.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QApplication>
#include <QTemporaryDir>
#include <QPointer>
#include <QPainter>
#include <QTest>

// These substitute the native battery service and window types only. The
// overlay, composition and power-screen owner are the production Kobo code.
namespace kobalt {
bool nativeReaderSleepScreenAvailable()
{
    return true;
}

NativeBatteryStatus battery{ 67, true, false };
std::function<void()> batteryChanged;

NativeBatteryStatus nativeReaderBattery()
{
    return battery;
}

void observeNativeBattery(QObject *context, std::function<void()> changed)
{
    QPointer<QObject> guard(context);
    batteryChanged = [guard, changed] {
        if (guard)
            changed();
    };
}
} // namespace kobalt

// Supply the same exported database entry points that the firmware adapters
// resolve. The production owner still opens its own read-only connection.
class Device
{
public:
    static Device *getCurrentDevice();
    const QString &getDbName() const;
};

Device *Device::getCurrentDevice()
{
    static Device device;
    return &device;
}

const QString &Device::getDbName() const
{
    static const QString name("power-screen-native");
    return name;
}

class DatabaseAdapter
{
public:
    static QSqlDatabase dbConnection(const QString &, bool);
};

QSqlDatabase DatabaseAdapter::dbConnection(const QString &name, bool)
{
    return QSqlDatabase::database(name, false);
}

class MainWindowView : public QWidget
{
    Q_OBJECT
};

class AbstractPowerView : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;
};

class PaintProbe : public QWidget
{
public:
    explicit PaintProbe(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

    int paints = 0;
    int deferredUpdates = 0;
    bool repaintAgain = false;
    QColor color = Qt::white;
    QColor painted;

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::UpdateLater)
            ++deferredUpdates;
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        ++paints;
        painted = color;
        QPainter(this).fillRect(rect(), color);
        if (repaintAgain) {
            repaintAgain = false;
            color = Qt::gray;
            update();
        }
    }
};

// Checks the Kobo sleep and power-off frames: saved and prepared backdrops, battery status, invalidation by a later book and handoff repaints.
class PowerScreenTests : public QObject
{
    Q_OBJECT
private slots:

    void saveReadsCommittedTokenOffTheGuiThread()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const kobalt::storage::Paths paths{ directory.path() };
        const auto connection = Device::getCurrentDevice()->getDbName();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(directory.path() + "/native.sqlite");
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(
                    "CREATE TABLE content (ContentID TEXT, ContentType INTEGER, DateLastRead TEXT)"));
            QVERIFY(query.exec("INSERT INTO content VALUES ('book', 6, '2026-10-24T09:00:00Z')"));
            kobalt::sleep::Snapshot snapshot;
            snapshot.image = QImage(100, 200, QImage::Format_RGB32);
            snapshot.image.fill(Qt::gray);
            snapshot.orientation = Qt::PortraitOrientation;
            snapshot.mode = kobalt::sleep::Mode::Page;
            kobalt::nickel::HomeSleepScreen screen(paths, nullptr);
            // A synchronous separate reader times out while this lock is held.
            // Returning first lets the GUI commit before the worker's read.
            QVERIFY(query.exec("BEGIN EXCLUSIVE"));
            screen.remember(snapshot, "book");
            QVERIFY(query.exec("COMMIT"));
            screen.finishSave();
            QVERIFY(QFile::exists(paths.sleepScreen()));
            screen.remember(snapshot, "book");
            screen.forget();
            QVERIFY(!QFile::exists(paths.sleepScreen()));
            {
                kobalt::nickel::HomeSleepScreen retiring(paths, nullptr);
                retiring.remember(snapshot, "book");
            }
            QVERIFY(QFile::exists(paths.sleepScreen()));
            screen.forget();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void init() { kobalt::battery = { 67, true, false }; }

    void initTestCase()
    {
        QVERIFY(kobalt::ui::initializeInterfaceFonts(kobalt::AssetBundle(
                QStringLiteral(KOBALT_TEST_ASSET_ROOT), QStringLiteral(KOBALT_VERSION))));
    }

    void homeSleepUsesSavedBackdropAndInvalidatesOnOpening_data()
    {
        QTest::addColumn<bool>("card");
        QTest::newRow("default-bookmark") << false;
        QTest::newRow("explicit-card") << true;
    }

    void homeSleepUsesSavedBackdropAndInvalidatesOnOpening()
    {
        QFETCH(bool, card);
        const auto tile = card ? kobalt::sleep::Tile::Card : kobalt::sleep::Tile::Bookmark;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const kobalt::storage::Paths paths{ directory.path() };
        const auto connection = Device::getCurrentDevice()->getDbName();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(directory.path() + "/native.sqlite");
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(
                    "CREATE TABLE content (ContentID TEXT, ContentType INTEGER, DateLastRead TEXT)"));
            QVERIFY(query.exec("INSERT INTO content VALUES ('book', 6, '2026-10-24T09:00:00Z')"));
            MainWindowView window;
            window.resize(800, 1200);
            AbstractPowerView power(&window);
            power.setGeometry(window.rect());
            window.show();
            power.show();
            kobalt::sleep::Snapshot snapshot;
            snapshot.image = QImage(window.size(), QImage::Format_RGB32);
            snapshot.image.fill(Qt::gray);
            snapshot.orientation = Qt::PortraitOrientation;
            snapshot.progress = "42% read";
            snapshot.mode = kobalt::sleep::Mode::Page;
            QJsonObject preferences{ { "schema", 1 }, { "sleep_screen", "page" } };
            if (card)
                preferences["sleep_tile"] = "card";
            QVERIFY(kobalt::storage::writeJson(paths.preferences(), preferences));
            const auto expected = [&](kobalt::reader::SleepStatus status, int battery) {
                return kobalt::reader::sleepScreenImage(snapshot.image, snapshot.progress, {},
                                                       false, battery, status, true, tile);
            };
            {
                kobalt::nickel::HomeSleepScreen screen(paths, nullptr);
                screen.remember(snapshot, "book");
                // The file is written on another thread. Every later use of it
                // waits for that write first.
                screen.finishSave();
                QVERIFY(QFile::exists(paths.sleepScreen()));
            }
            // A new owner simulates Nickel starting with only the saved file.
            kobalt::nickel::HomeSleepScreen screen(paths, nullptr);
            screen.prepare(true);
            screen.present();
            auto *frame = power.findChild<QWidget *>("rrSleepFrame");
            QVERIFY(frame);
            QCOMPARE(frame->grab().toImage().convertToFormat(QImage::Format_RGB32),
                     expected(kobalt::reader::SleepStatus::Charging, 67));
            kobalt::battery = { 88, false, false };
            kobalt::batteryChanged();
            QCOMPARE(frame->grab().toImage().convertToFormat(QImage::Format_RGB32),
                     expected(kobalt::reader::SleepStatus::Sleeping, 88));
            kobalt::battery = { 100, true, true };
            kobalt::batteryChanged();
            QCOMPARE(frame->grab().toImage().convertToFormat(QImage::Format_RGB32),
                     expected(kobalt::reader::SleepStatus::Charged, 100));
            screen.clear();
            // A later book invalidates persisted data even without a hook,
            // for example after a database change while Nickel was stopped.
            QVERIFY(query.exec("INSERT INTO content VALUES ('other', 6, '2026-10-24T09:00:01Z')"));
            screen.prepare(true);
            screen.present();
            QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
            QVERIFY(query.exec("DELETE FROM content WHERE ContentID = 'other'"));
            screen.prepare(false);
            screen.present();
            QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
            screen.forget();
            QVERIFY(!QFile::exists(paths.sleepScreen()));
            screen.prepare(true);
            screen.present();
            QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
            screen.remember(snapshot, "book");
            // Preparing waits for the background write before it reads the file.
            screen.prepare(true, true);
            QVERIFY(QFile::exists(paths.sleepScreen()));
            power.hide();
            power.show();
            frame = power.findChild<QWidget *>("rrSleepFrame");
            QVERIFY(frame);
            QCOMPARE(frame->grab().toImage().convertToFormat(QImage::Format_RGB32),
                     expected(kobalt::reader::SleepStatus::PoweredOff, 100));
            screen.clear();
            QVERIFY(QFile::remove(paths.sleepScreen()));
            screen.prepare(true);
            screen.present();
            QVERIFY(power.findChild<QWidget *>("rrSleepFrame"));
            QVERIFY(query.exec("INSERT INTO content VALUES ('later', 6, '2026-10-24T10:00:00Z')"));
            screen.clear();
            screen.prepare(true);
            screen.present();
            QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void powerOffUsesPreparedBackdrop_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("card");
        QTest::newRow("light-default-bookmark") << false << false;
        QTest::newRow("dark-default-bookmark") << true << false;
        QTest::newRow("light-explicit-card") << false << true;
        QTest::newRow("dark-explicit-card") << true << true;
    }

    void powerOffUsesPreparedBackdrop()
    {
        QFETCH(bool, dark);
        QFETCH(bool, card);
        const auto tile = card ? kobalt::sleep::Tile::Card : kobalt::sleep::Tile::Bookmark;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MainWindowView window;
        window.resize(800, 1200);
        AbstractPowerView power(&window);
        power.setGeometry(window.rect());
        window.show();
        power.show();
        kobalt::nickel::HomeSleepScreen screen({ directory.path() }, nullptr);
        kobalt::sleep::Snapshot snapshot;
        snapshot.image = QImage(window.size(), QImage::Format_RGB32);
        snapshot.image.fill(Qt::gray);
        snapshot.orientation = Qt::PortraitOrientation;
        snapshot.progress = "42% read";
        snapshot.remaining = "37 min to go";
        snapshot.dark = dark;
        snapshot.mode = kobalt::sleep::Mode::Page;
        snapshot.percent = 42;
        if (card)
            snapshot.tile = kobalt::sleep::Tile::Card;
        screen.preparePowerOff(snapshot);
        // No storage is needed after preparation, including during shutdown.
        QVERIFY(directory.remove());
        power.hide();
        power.show();
        auto *frame = power.findChild<QWidget *>("rrSleepFrame");
        QVERIFY(frame);
        const auto expected = kobalt::reader::sleepScreenImage(
                snapshot.image, snapshot.progress, snapshot.remaining, dark, 67,
                kobalt::reader::SleepStatus::PoweredOff, true, tile, 42);
        const auto rendered = frame->grab().toImage().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(rendered, expected);
        QCOMPARE(rendered,
                 kobalt::reader::sleepScreenImage(snapshot.image, snapshot.progress,
                                                  snapshot.remaining, !dark, 67,
                                                  kobalt::reader::SleepStatus::PoweredOff, true,
                                                  tile, 42));
        const auto area = kobalt::reader::sleepScreenCardRect(rendered.size(), true, tile);
        QCOMPARE(rendered.pixel(area.center().x(), area.top() + 4), qRgb(0, 0, 0));
        QCOMPARE(rendered.pixel(area.left(), area.top() + 10), qRgb(255, 255, 255));
        const auto capture = qEnvironmentVariable("KOBALT_POWER_CAPTURE_ROOT");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(rendered.save(capture + "/power-off-"
                                  + QString::fromLatin1(QTest::currentDataTag()) + ".png"));
        }
        QCOMPARE(kobalt::reader::sleepStatusText(kobalt::reader::SleepStatus::PoweredOff),
                 QString("Powered off"));
        screen.clear();
        QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
        power.hide();
        power.show();
        QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
    }

    void nativeViewOwnsTheFrameAndInput()
    {
        MainWindowView window;
        window.resize(800, 1200);
        auto *power = new AbstractPowerView(&window);
        power->setGeometry(window.rect());
        window.show();
        power->show();
        auto presentation = kobalt::nickel::makeWindowPresentation(nullptr, true);
        QImage image(window.size(), QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(presentation->presentSleepScreen(image, Qt::PortraitOrientation));
        auto *frame = power->findChild<QWidget *>("rrSleepFrame");
        QVERIFY(frame);
        QVERIFY(frame->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(frame->focusPolicy(), Qt::NoFocus);
        power->resize(600, 900);
        QCOMPARE(frame->size(), power->size());
        delete power;
        QVERIFY(!presentation->updateSleepScreen(image, image.rect()));
        QVERIFY(!presentation->presentSleepScreen(image, Qt::PortraitOrientation));
        presentation->clearSleepScreen();
    }

    void sleepHandoffPaintsAfterPinUnlock()
    {
        MainWindowView window;
        window.resize(400, 600);
        PaintProbe pin(&window);
        pin.setGeometry(window.rect());
        pin.color = Qt::black;
        window.show();
        pin.show();
        QCoreApplication::processEvents();
        QVERIFY(pin.paints > 0);

        QWidget reader;
        reader.resize(window.size());
        reader.show();
        QCoreApplication::processEvents();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        presentation->pauseBackground(true);
        pin.hide();
        AbstractPowerView power(&window);
        power.setGeometry(window.rect());
        PaintProbe sleep(&power);
        sleep.setGeometry(power.rect());
        sleep.color = Qt::gray;
        power.show();
        sleep.show();
        QCOMPARE(sleep.paints, 0);

        presentation->resumeAndRepaintBackground();
        QVERIFY(window.updatesEnabled());
        QVERIFY(sleep.paints > 0);
        QCOMPARE(sleep.painted, sleep.color);
    }

    void handoffCompletesDeferredChildPaints()
    {
        MainWindowView window;
        window.resize(400, 600);
        PaintProbe background(&window);
        background.setGeometry(window.rect());
        background.color = Qt::black;
        window.show();
        background.show();
        QWidget reader;
        reader.show();
        QCoreApplication::processEvents();

        const int deferred = background.deferredUpdates;
        background.repaintAgain = true;
        background.repaint();
        QCOMPARE(background.painted, QColor(Qt::black));
        QCOMPARE(background.color, QColor(Qt::gray));
        QCOMPARE(background.deferredUpdates, deferred);
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        presentation->pauseBackground(true);
        presentation->resumeAndRepaintBackground();
        QCOMPARE(background.deferredUpdates, deferred + 1);
        QCOMPARE(background.painted, background.color);
    }
};
QTEST_MAIN(PowerScreenTests)
#include "kobo_power_screen.moc"
