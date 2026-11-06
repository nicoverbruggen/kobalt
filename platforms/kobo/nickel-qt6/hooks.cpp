#include "hooks.h"
#include "platforms/kobo/common/book_menu.h"
#include "refresh.h"
#include "platforms/kobo/common/native_lifecycle.h"
#include "platforms/kobo/common/standby_diagnostics.h"
#include "platforms/kobo/common/standby_input.h"
#include "platforms/kobo/common/native_authorization.h"
#include "platforms/kobo/common/native_dictionary.h"
#include "platforms/kobo/common/native_keyboard.h"
#include "platforms/kobo/common/native_light.h"
#include "platforms/kobo/common/native_wifi_power.h"
#include "platforms/kobo/display/page_animation.h"
#include <NickelHook.h>
#include <QMetaType>
#include <dlfcn.h>

namespace kobalt::nickel_qt6 {
void (*nativeHandleOpener)(void *, const void *, const void *);
void (*nativeFinishInitialization)(void *);
void (*nativeContinueUsing)(void *), (*nativeUnplugged)(void *);
void (*settingsConstructor)(QWidget *, QWidget *);
bool (*nativePrerenderAllowed)(const void *);

// No firmware 5 build is named here. The adapters check every exported symbol
// they call. Volume's registered size stands for the object layouts they rely
// on; a release that changes it leaves Nickel's reader in place.
bool firmwareSupported()
{
    const auto *volume = static_cast<const QtPrivate::QMetaTypeInterface *>(
            dlsym(RTLD_DEFAULT, "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI6VolumeE8metaTypeE"));
    return volume && QMetaType(volume).sizeOf() == 12;
}
} // namespace kobalt::nickel_qt6

using namespace kobalt::nickel_qt6;
static nh_hook readerHooks[] = {
    { .sym = "_ZN10VirtualKey11repaintFastEb",
      .sym_new = "kobalt_keyboard_repaint",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeKeyboardRepaint),
      .desc = "Theme Kobalt keyboard key repaints",
      .optional = true },
    { .sym = "read",
      .sym_new = "kobalt_touch_read",
      .lib = "/usr/lib/plugins/generic/libqevdevtouchplugin.so",
      .out = nh_symoutptr(kobalt::nativeTouchRead),
      .desc = "Observe touch acquisition independently of coordinate mapping",
      .optional = true },
    { .sym = "write",
      .sym_new = "kobalt_power_write",
      .lib = "libQt6Core.so.6",
      .out = nh_symoutptr(kobalt::nativePowerWrite),
      .desc = "Observe native power writes when diagnostics are enabled",
      .optional = true },
    { .sym = "_ZN6IOUtil12stringToFileERK7QStringS2_",
      .sym_new = "kobalt_observe_power_string_to_file",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeDiagnosticPowerStringToFile),
      .desc = "Observe native power helpers without an input guard",
      .optional = true },
    { .sym = "_ZN16ActionProxyMixin17setupBookListMenuEP28AbstractNickelMenuControllerRK6VolumeP15NickelTouchMenuRK7QStringS9_bRK3Tab",
      .sym_new = "kobalt_book_menu",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeBookMenu),
      .desc = "Track the library book menu",
      .optional = true },
    { .sym = "_ZN22N3PowerWorkflowManager6rebootEv",
      .sym_new = "kobalt_reboot",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeReboot),
      .desc = "Save and release Kobalt before Nickel reboots",
      .optional = false },
    { .sym = "_ZN22N3PowerWorkflowManager8powerOffEb",
      .sym_new = "kobalt_power_off",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativePowerOff),
      .desc = "Save and release Kobalt before Nickel powers off",
      .optional = false },
    { .sym = "_ZN22N3PowerWorkflowManager13hideSleepViewEv",
      .sym_new = "kobalt_hide_sleep_view",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeHideSleepView),
      .desc = "Keep the sleep frame until Kobalt completes authorized resume",
      .optional = false },
    { .sym = "_ZN27SubscriptionWorkflowManager13maybeExitBookERK6Volume",
      .sym_new = "kobalt_authorization_exit_book",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeMaybeExitBook),
      .desc = "Apply Nickel subscription closure to the active Kobalt book",
      .optional = false },
    { .sym = "_ZN13VolumeManager13volumeRemovedERK6Volume",
      .sym_new = "kobalt_authorization_volume_removed",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeVolumeRemoved),
      .desc = "Release Kobalt content when Nickel removes its book",
      .optional = false },
    { .sym = "_ZN19AccountSignOutMixin7signOutEv",
      .sym_new = "kobalt_authorization_sign_out",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeAccountSignOut),
      .desc = "Release Kobalt content before confirmed native sign-out",
      .optional = false },
    { .sym = "_ZN18Nickel3Application20finishInitializationEv",
      .sym_new = "kobalt_finish_initialization",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(nativeFinishInitialization),
      .desc = "Wait for native initialization before creating Kobalt controllers",
      .optional = false },
    { .sym = "_ZN22N3PowerWorkflowManager13showSleepViewEv",
      .sym_new = "kobalt_show_sleep_view",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeShowSleepView),
      .desc = "Keep Kobalt visible until Nickel installs its sleep view",
      .optional = false },
    { .sym = "_ZN12PowerManager13suspendDeviceERK7QString",
      .sym_new = "kobalt_suspend_device",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeSuspendDevice),
      .desc = "Trace native standby calls when diagnostics are enabled",
      .optional = true },
    { .sym = "_ZN12PowerManager22suspendDeviceWithAlarmERK7QString",
      .sym_new = "kobalt_suspend_device_with_alarm",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeSuspendDeviceWithAlarm),
      .desc = "Trace native alarm standby calls when diagnostics are enabled",
      .optional = true },
    { .sym = "_ZN12PowerManager10canSuspendEv",
      .sym_new = "kobalt_can_suspend",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeCanSuspend),
      .desc = "Wait for Kobalt work to park before suspension",
      .optional = false },
    { .sym = "_ZN19PlugWorkflowManager18onCancelAndConnectEv",
      .sym_new = "kobalt_usb_workflow",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeUsbWorkflow),
      .desc = "Save Kobalt progress before the USB workflow starts",
      .optional = false },
    { .sym = "_ZN19PlugWorkflowManager14aboutToConnectEv",
      .sym_new = "kobalt_usb_about_to_connect",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeAboutUsbConnect),
      .desc = "Prepare Kobalt for confirmed USB connection",
      .optional = false },
    { .sym = "_ZN11PlugManager16goGoGadgetDeviceEv",
      .sym_new = "kobalt_usb_connect",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeUsbConnect),
      .desc = "Release Kobalt storage before USB connection",
      .optional = false },
#ifndef KOBALT_READER_HARDWARE
    { .sym = "ioctl",
      .sym_new = "kobalt_light_test_ioctl",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::lightTestIoctl),
      .desc = "Test native brightness against simulated light hardware",
      .optional = true },
    { .sym = "_ZN11ReadingViewC1EP7QWidget",
      .sym_new = "kobalt_dictionary_reading_view",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::dictionaryTestNextReadingView),
      .desc = "Count native reading views during dictionary tests",
      .optional = true },
    { .sym = "_ZN13WebEngineViewC2EP7QWidget",
      .sym_new = "kobalt_dictionary_web_view",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::dictionaryTestNextWebView),
      .desc = "Count WebEngine views during dictionary tests",
      .optional = true },
#endif
    { .sym = "_ZN13QBackingStore5flushERK7QRegionP7QWindowRK6QPointRK5QListISt4pairI5QRectyEE",
      .sym_new = "kobalt_reader_flush",
      .lib = "libQt6Widgets.so.6",
      .out = nh_symoutptr(kobalt::platform::nativeFlush),
      .desc = "Submit completed Kobalt paints together",
      .optional = true },
    { .sym = "_ZN13KoboScreenMTK15getFlagsForRectERyRK5QRect",
      .sym_new = "kobalt_refresh_flags",
      .lib = "/usr/local/Kobo/platforms/libkobo.so",
      .out = nh_symoutptr(kobalt::platform::nativeRefreshFlags),
      .desc = "Preserve Kobalt refresh policies for merged dirty regions",
      .optional = true },
    { .sym = "_ZN19PlugWorkflowManager15onContinueUsingEv",
      .sym_new = "kobalt_continue_using",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(nativeContinueUsing),
      .desc = "Track the end of a declined USB connection",
      .optional = false },
    { .sym = "_ZN19PlugWorkflowManager9unpluggedEv",
      .sym_new = "kobalt_unplugged",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(nativeUnplugged),
      .desc = "Track the end of USB connection and processing",
      .optional = false },
    { .sym = "_ZN23WirelessWorkflowManager23connectWirelessSilentlyEv",
      .sym_new = "kobalt_connect_wireless_silently",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(kobalt::nativeConnectWirelessSilently),
      .desc = "Keep Nickel's background connections off while the Wi-Fi power saver applies",
      .optional = true },
    { .sym = "_ZNK25PrerenderToPersistManager25isAllowedToDoPrerenderingEv",
      .sym_new = "kobalt_prerender_allowed",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(nativePrerenderAllowed),
      .desc = "Suspend native background page rendering while Kobalt is selected",
      .optional = false },
    { .sym = "_ZN24AbstractReadingViewMixin12handleOpenerERK6VolumeRKN20AbstractReaderOpener7ContextE",
      .sym_new = "kobalt_handle_opener",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(nativeHandleOpener),
      .desc = "Open supported publications without creating the native reading view",
      .optional = false },
    { .sym = "_ZN21N3SettingsReadingViewC1EP7QWidget",
      .sym_new = "kobalt_settings_constructor",
      .lib = "libnickel.so.1.0.0",
      .out = nh_symoutptr(settingsConstructor),
      .desc = "Add the Kobalt preference to Reading Settings",
      .optional = false },
    { 0 },
};
#ifdef KOBALT_READER_HARDWARE
static nh_info info = { .name = "Kobalt",
                        .desc = "Native CREngine reader",
                        .uninstall_flag = KOBALT_CONFIG_DIR "/uninstall-now",
                        .uninstall_xflag = KOBALT_CONFIG_DIR "/uninstall",
                        .failsafe_delay = 3 };
NickelHook(.init = initializeReader, .info = &info, .hook = readerHooks,
           .uninstall = uninstallReader);
#else
static nh_info info = { .name = "Kobalt",
                        .desc = "Native CREngine reader experiment",
                        .failsafe_delay = 3 };
NickelHook(.init = initializeReader, .info = &info, .hook = readerHooks);
#endif
