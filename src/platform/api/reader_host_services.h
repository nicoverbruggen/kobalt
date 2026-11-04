#pragma once
#include "page_animation_support.h"
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
class QLineEdit;
class QWidget;
class QObject;

namespace kobalt {
struct BatteryStatus {
    int percent = -1;
    bool charging = false;
    bool fullyCharged = false;
};
struct WifiStatus {
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
struct ReaderNotice {
    QString title, message, name;
};
struct ReaderHostServices {
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
    bool systemRotation = false;
    bool downloadableDictionaries = true;
    // Optional host-resolved high-resolution cover. The reader decodes it on
    // its worker and falls back to the publication cover if it is unusable.
    QString coverImagePath;
    // Optional host reader-selection preference. Both callbacks are supplied
    // together; a successful write affects future book openings.
    std::function<bool()> automaticBookOpening;
    std::function<bool(bool)> setAutomaticBookOpening;
    std::function<QString()> clock;
    // Optional monotonic milliseconds for interval checks; tests replace it.
    std::function<qint64()> monotonicMilliseconds;
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
    // Automatic KOsync moments the host supports: a device sleep and wake
    // lifecycle, and a sync while reading that may turn on Wi-Fi.
    bool kosyncSleepWake = false, kosyncWifiControl = false;
    // Optional: turns Wi-Fi off for the Wi-Fi power saver while an opened book
    // waits to be presented, before it takes input. The host leaves Wi-Fi
    // alone unless it is on, Bluetooth is known to be off and the host's own
    // sync is idle, then calls done with whether it turned Wi-Fi off. Turning
    // Wi-Fi off can wait for child processes on the main thread.
    std::function<void(QObject *context, std::function<void(bool)> done)> turnOffWifi;
    PresentationFactory presentation;
    // Empty in hardware builds. Keep the value layout independent of the
    // diagnostic macro used by the constructing and consuming targets.
    std::function<QJsonObject(const QJsonObject &)> debugProgressAction;
    std::function<bool()> browserAvailable;
    std::function<bool(const QUrl &, QWidget *)> openBrowser;
    // Optional notice the reader shows once its first page has painted, for
    // example after profile preparation reset unusable saved data.
    ReaderNotice startupNotice;
    bool complete() const {
        return bool(automaticBookOpening) == bool(setAutomaticBookOpening) && clock && battery && pageButtons &&
               reverseLandscapeButtons && pageButtonDirection && fontFamilies && uiFont && suspended &&
               lightAvailable && animationSupport && cancelAnimation && light && dictionary && keyboard &&
               presentation && browserAvailable && openBrowser;
    }
};
}
