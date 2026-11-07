#pragma once
#include "page_animation_support.h"
#include "page_buttons.h"
#include "dictionary_service.h"
#include "keyboard_controller.h"
#include "sync_network.h"
#include "light_controls.h"
#include "safe_area.h"
#include "window_presentation.h"
#include <QFont>
#include <QJsonObject>
#include <QSize>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>
class QKeyEvent;
class QNetworkAccessManager;
class QLineEdit;
class QWidget;
class QObject;

namespace kobalt {
struct BatteryStatus
{
    int percent = -1;
    bool charging = false;
    bool fullyCharged = false;
};

struct WifiStatus
{
    bool enabled = false;
    bool connected = false;
    // Nickel reports four signal levels. -1 means no quality report yet.
    int strength = -1;
};

// Constructed by the host and moved into one reader window. Queries stay live
// across sleep and capability changes. Factories return independently owned
// non-null controllers; they never return a borrowed native handle. Every
// callback except automaticBookOpening, setAutomaticBookOpening,
// debugProgressAction, observeBattery, observeBluetooth, observeWifi, remoteKeyActivity and syncNetwork is required,
// including unsupported hosts.
// A notice in the reader's usual notice card. `name` is the card's object
// name; its message and button add `Message` and `Confirm`.
struct ReaderNotice
{
    QString title, message, name;
};

struct ReaderHostServices
{
    // Human-readable model used for the default sync device name.
    QString deviceName;
    // Standalone hosts return to their Library; Kobo uses the default exit label.
    QString backButtonLabel;
    // Desktop-only presentation preferences; fixtures retain device chrome.
    bool desktopUi = false;
    // Narrow phone form factor, resolved from native bounds and reader density.
    bool phoneLayout = false;
    // Optional: the cutout, system-bar and corner bands of a reader of this
    // size. The reader asks again on resize or refreshHostPresentation().
    std::function<SafeArea(QSize)> safeArea;
    // Kobo supplies its default export folder. Other hosts open a save-location picker.
    std::function<QString()> annotationExportDirectory;
    // Optional native dialog parent for hosts that embed the reader in a canvas.
    std::function<QWidget *()> nativeDialogParent;
    // Missing capability means monochrome; independent of theme and animation preferences.
    std::function<bool()> colorDisplay;
    // Optional desktop query. Device previews retain simulated status chrome.
    std::function<bool()> devicePreview;
    // The host asks flushBeforeStandby before suspending an active reader,
    // or previews a device that does.
    bool readerStandbySupported = false;
    bool systemRotation = false;
    bool downloadableDictionaries = true;
    // Optional host-resolved high-resolution cover. The reader decodes it on
    // its worker and falls back to the publication cover if it is unusable.
    QString coverImagePath;
    // Optional host paths for user-supplied sleep PNGs.
    QStringList sleepOverlayDirectories;
    // Optional host reader-selection preference. Both callbacks are supplied
    // together; a successful write affects future book openings.
    std::function<bool()> automaticBookOpening;
    std::function<bool(bool)> setAutomaticBookOpening;
    std::function<QString()> clock;
    // Optional monotonic milliseconds for interval checks; tests replace it.
    // Kobo supplies a clock that includes suspended time, so the intervals
    // count real time while Nickel keeps the device in standby. -1 means the
    // clock failed.
    std::function<qint64()> monotonicMilliseconds;
    // Optional monotonic milliseconds at which page-turn input arrives, for the
    // filter that drops duplicate turns. Tests replace it to move past the
    // filter window without waiting. Hosts leave it empty: the reader then
    // uses its own monotonic timer.
    std::function<qint64()> inputMilliseconds;
    // Optional local time of day in milliseconds for the reader's minute tick;
    // tests replace it.
    std::function<int()> millisecondsOfDay;
    std::function<BatteryStatus()> battery;
    // Optional native notifications. The subscription ends with context.
    std::function<void(QObject *context, std::function<void()> changed)> observeBattery;
    // Optional on unsupported hosts. The host owns the subscription under
    // context and reports cached state without blocking the reader thread.
    std::function<void(QObject *context, std::function<void(bool)> changed)> observeBluetooth;
    std::function<void(QObject *context, std::function<void(WifiStatus)> changed)> observeWifi;
    // Report consumed key activity to the host. The host identifies remote
    // input and retains its own radio inactivity policy.
    std::function<void(QObject *context, QKeyEvent *event)> remoteKeyActivity;
    // Optional physical controls. Empty means there are no mappable buttons.
    std::function<pageButtons::Buttons()> buttonControls;
    std::function<bool()> pageButtons, reverseLandscapeButtons;
    std::function<int(QKeyEvent *)> pageButtonDirection;
    std::function<QStringList()> fontFamilies;
    std::function<QFont()> uiFont;
    std::function<bool()> suspended, lightAvailable;
    std::function<PageAnimationSupport()> animationSupport;
    // Software page transitions on ordinary Android screens, separate from e-paper sweeps.
    bool softwarePageAnimations = false;
    // Optional: the platform's text size setting, where 1 is standard. On
    // Android and desktop, the reader applies it when a book opens and the
    // Library when it is shown. Kobo applies Nickel's Large Print scale in its
    // coordinator instead.
    std::function<qreal()> textScale;
    std::function<void()> cancelAnimation;
    std::function<std::unique_ptr<LightControls>(QWidget *)> light;
    std::function<std::unique_ptr<DictionaryService>()> dictionary;
    KeyboardFactory keyboard;
    // Optional on hosts whose operating system already supplies connectivity.
    SyncNetworkFactory syncNetwork;
    // Optional HTTP access for KOSync requests; tests replace it. Hosts leave
    // it empty, and the KOSync client then uses its own.
    std::function<std::unique_ptr<QNetworkAccessManager>()> kosyncHttp;
    // Automatic KOsync moments the host supports: a device sleep and wake
    // lifecycle, and a sync while reading that may turn on Wi-Fi.
    bool kosyncSleepWake = false, kosyncWifiControl = false;
    // Optional: turns Wi-Fi off for the Wi-Fi power saver, while an opened
    // book waits to be presented or while the device goes to sleep. Both come
    // before the reader takes input. The host leaves Wi-Fi alone unless it is
    // on, Bluetooth is known to be off and the host's own sync is idle, then
    // calls done with whether it turned Wi-Fi off. Deleting the context drops
    // the request. Turning Wi-Fi off can wait for child processes on the main
    // thread; the host declares that wait with `reason`, a string literal.
    std::function<void(QObject *context, const char *reason, std::function<void(bool)> done)>
            turnOffWifi;
    // Optional: lets the reader keep the host's own background Wi-Fi requests,
    // such as the connection Nickel starts after every wake, from turning
    // Wi-Fi on. The host calls `holding` on the GUI thread before such a
    // request and skips the request when it answers true. Connections that
    // the reader asks for itself are not affected. Deleting the owner ends
    // the hold.
    std::function<void(QObject *owner, std::function<bool()> holding)> holdWifiOff;
    PresentationFactory presentation;
    // Empty in hardware builds. Keep the value layout independent of the
    // diagnostic macro used by the constructing and consuming targets.
    std::function<QJsonObject(const QJsonObject &)> debugProgressAction;
    std::function<bool()> browserAvailable;
    std::function<bool(const QUrl &, QWidget *)> openBrowser;
    // Optional notice the reader shows once its first page has painted, for
    // example after profile preparation reset unusable saved data.
    ReaderNotice startupNotice;

    bool complete() const
    {
        return bool(automaticBookOpening) == bool(setAutomaticBookOpening) && clock && battery
                && pageButtons && reverseLandscapeButtons && pageButtonDirection && fontFamilies
                && uiFont && suspended && lightAvailable && animationSupport && cancelAnimation
                && light && dictionary && keyboard && presentation && browserAvailable
                && openBrowser;
    }
};
} // namespace kobalt
