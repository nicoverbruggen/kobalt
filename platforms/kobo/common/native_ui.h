#pragma once
#include <QTime>
#include <QString>
#include <QStringList>
#include <functional>

class QUrl;
class QFont;
class QWidget;
class QKeyEvent;
class QObject;

namespace kobalt {
// Firmware 5.18 owns these widgets and preferences. A firmware 4 adapter can
// replace this implementation without changing the reader's control layouts.
bool nativeReaderBrowserAvailable();
bool nativeReaderOpenBrowser(const QUrl &url, QWidget *reader);
QString nativeReaderClock();
QString nativeReaderTime(QTime time);
// GUI thread only, after firmwareSupported() accepts Nickel. No native widgets are created.
QStringList nativeReaderFontFamilies();

struct NativeBatteryStatus
{
    int percent = -1;
    bool charging = false;
    bool fullyCharged = false;
};

// Uses Nickel's full-screen cover path, excluding its generic cover IDs.
// The volume is borrowed only for this GUI-thread call.
QString nativeReaderCoverPath(const void *volume);
bool nativeReaderSleepScreenAvailable();
// GUI thread only, after firmwareSupported() accepts Nickel.
NativeBatteryStatus nativeReaderBattery();
void observeNativeBattery(QObject *context, std::function<void()> changed);
// Observe power state without synchronous native D-Bus getters.
void observeNativeBluetooth(QObject *context, std::function<void(bool)> changed);
enum class BluetoothPower;
// Asks the Bluetooth stack once whether the adapter is powered. The answer
// arrives on the GUI thread. No Bluetooth stack on the bus counts as off.
void queryNativeBluetoothPower(QObject *context, std::function<void(BluetoothPower)> answer);
struct WifiStatus;
void observeNativeWifi(QObject *context, std::function<void(WifiStatus)> changed);
// Report accepted remote page keys; retain Nickel's idle timeout for other input.
void reportNativeBluetoothActivity(QObject *context, QKeyEvent *event);
bool nativeReaderHasPageButtons();
bool nativeReaderReverseLandscapeButtons();
int nativePageButtonDirection(QKeyEvent *event);
void installNativeReaderSetting(QWidget *view, std::function<bool()> enabled,
                                std::function<bool(bool)> save);
// GUI thread only. Use the firmware font alias used by native navigation and clock controls.
QFont nativeReaderUiFont();
} // namespace kobalt
