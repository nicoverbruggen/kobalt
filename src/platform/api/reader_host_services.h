#pragma once
#include "dictionary_service.h"
#include "keyboard_controller.h"
#include "light_controls.h"
#include "window_presentation.h"
#include <QFont>
#include <QJsonObject>
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
// Constructed by the host and moved into one reader window. Queries stay live
// across sleep and capability changes. Factories return independently owned
// non-null controllers; they never return a borrowed native handle. Every
// callback except automaticBookOpening, setAutomaticBookOpening,
// debugProgressAction, observeBattery, observeBluetooth and remoteKeyActivity is required,
// including unsupported hosts.
struct ReaderHostServices {
    // Desktop-only presentation preferences; fixtures retain device chrome.
    bool desktopUi = false;
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
    std::function<BatteryStatus()> battery;
    // Optional native notifications. The subscription ends with context.
    std::function<void(QObject *context, std::function<void()> changed)> observeBattery;
    // Optional on unsupported hosts. The host owns the subscription under
    // context and reports cached state without blocking the reader thread.
    std::function<void(QObject *context, std::function<void(bool)> changed)> observeBluetooth;
    // Report consumed key activity to the host. The host identifies remote
    // input and retains its own radio inactivity policy.
    std::function<void(QObject *context, QKeyEvent *event)> remoteKeyActivity;
    std::function<bool()> pageButtons, reverseLandscapeButtons;
    std::function<int(QKeyEvent *)> pageButtonDirection;
    std::function<QStringList()> fontFamilies;
    std::function<QFont()> uiFont;
    std::function<bool()> suspended, lightAvailable, animationsAvailable;
    std::function<void()> cancelAnimation;
    std::function<std::unique_ptr<LightControls>(QWidget *)> light;
    std::function<std::unique_ptr<DictionaryService>()> dictionary;
    KeyboardFactory keyboard;
    PresentationFactory presentation;
    // Empty in hardware builds. Keep the value layout independent of the
    // diagnostic macro used by the constructing and consuming targets.
    std::function<QJsonObject(const QJsonObject &)> debugProgressAction;
    std::function<bool()> browserAvailable;
    std::function<bool(const QUrl &, QWidget *)> openBrowser;
    bool complete() const {
        return bool(automaticBookOpening) == bool(setAutomaticBookOpening) && clock && battery && pageButtons &&
               reverseLandscapeButtons && pageButtonDirection && fontFamilies && uiFont && suspended &&
               lightAvailable && animationsAvailable && cancelAnimation && light && dictionary && keyboard &&
               presentation && browserAvailable && openBrowser;
    }
};
}
