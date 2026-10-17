#include "platform/nickel/common/native_ui.h"
#include <QCoreApplication>
#include <QTest>
#include <QThread>

class HardwareInterface : public QObject {
    Q_OBJECT
signals:
    void battery_level(int);
    void battery_changed();
};
class HardwareFactory {
public:
    static QObject *sharedInstance();
};
class PlugManager : public QObject {
    Q_OBJECT
public:
    static PlugManager *sharedInstance();
signals:
    void plugged();
    void unplugged();
    void ac_plugged();
    void ac_unplugged();
};
QObject *HardwareFactory::sharedInstance() {
    static HardwareInterface hardware;
    return &hardware;
}
PlugManager *PlugManager::sharedInstance() {
    static PlugManager manager;
    return &manager;
}

class NativeBatteryTest : public QObject {
    Q_OBJECT
private slots:
    void nativeNotificationsWaitForStateAndEndWithTheirContext() {
        auto *hardware = static_cast<HardwareInterface *>(HardwareFactory::sharedInstance());
        auto *plug = PlugManager::sharedInstance();
        auto *context = new QObject;
        int notifications = 0;
        kobalt::observeNativeBattery(context, [&] {
            QCOMPARE(QThread::currentThread(), context->thread());
            ++notifications;
        });
        emit hardware->battery_level(68);
        emit hardware->battery_changed();
        emit plug->plugged();
        emit plug->unplugged();
        emit plug->ac_plugged();
        emit plug->ac_unplugged();
        QCOMPARE(notifications, 0);
        QTRY_COMPARE(notifications, 6);
        emit hardware->battery_level(100);
        delete context;
        QCoreApplication::processEvents();
        QCOMPARE(notifications, 6);
        emit plug->plugged();
        QCoreApplication::processEvents();
        QCOMPARE(notifications, 6);
    }
};
QTEST_GUILESS_MAIN(NativeBatteryTest)
#include "native_battery_test.moc"
