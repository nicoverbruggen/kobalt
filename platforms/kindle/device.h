#pragma once
#include "properties.h"
#include "platform/api/reader_host_services.h"
#include <QMap>
#include <QImage>
#include <QElapsedTimer>
#include <memory>

namespace kobalt::kindle {
struct DeviceState {
    BatteryStatus battery;
    WifiStatus wifi;
    bool bluetoothConnected = false;
    int brightness = -1, maximumBrightness = 24, warmth = -1;
    bool sleeping = false;
    bool hasLight() const { return brightness >= 0 && brightness <= maximumBrightness; }
    bool hasWarmth() const { return warmth >= 0 && warmth <= 24; }
};

class Device final : public QObject {
    Q_OBJECT
public:
    explicit Device(Properties::Commands commands, QObject *parent = nullptr);
    ~Device() override;
    const DeviceState &state() const { return state_; }
    bool initialized() const { return initialized_ && (!lifecycle || powerKnown); }
    void refresh();
    void stop();
    int setBrightness(int percent);
    int setWarmth(int level);
    bool warmthVerified() const { return warmthConfirmed; }
    void startLifecycle(const QString &program);
    void activity();
    void setSleepFrame(QImage frame);
    const QImage &sleepFrame() const { return sleepFrame_; }
#ifdef KOBALT_KINDLE_NATIVE_TESTING
    void simulatePowerEvent(const QByteArray &event) { powerEvent(event); }
#endif
    static DeviceState decode(const QMap<QString, PropertyReply> &values);
    static int percentToLevel(int percent, int maximum);
    static int levelToPercent(int level, int maximum);
signals:
    void changed();
    void batteryChanged();
    void wifiChanged();
    void bluetoothChanged();
    void initializedChanged();
    void sleepingChanged(bool sleeping);
    void sleepFrameChanged();
    void usbRequested();
    void lifecycleFailed(QString error);
private:
    Properties properties;
    QTimer poll;
    DeviceState state_;
    QMap<QString, PropertyReply> sample;
    bool reading = false, stopped = false, initialized_ = false;
    bool brightnessConfirmed = false, warmthConfirmed = false;
    quint64 lightRevision = 0;
    quint64 powerRevision = 0;
    QProcess bridge;
    QTimer bridgeDeadline;
    QByteArray bridgeOutput;
    QElapsedTimer lastActivity;
    QImage sleepFrame_;
    bool lifecycle = false, bridgeReady = false, powerKnown = false;
    void powerEvent(const QByteArray &event);
    void setSleeping(bool sleeping);
    int setLight(const QString &name, int value, int maximum);
};
std::shared_ptr<Device> makeDevice();
std::unique_ptr<LightControls> makeLight(std::shared_ptr<Device> device);
}
