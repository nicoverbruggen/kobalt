#pragma once
class QWidget;

// Internal contract between Nickel's hook entry point and its reader controller.
// Other application targets do not include this header.
namespace kobalt::nickel_qt6 {
bool firmwareSupported();
int initializeReader();
#ifdef KOBALT_READER_HARDWARE
bool uninstallReader();
#endif
extern void (*nativeHandleOpener)(void *, const void *, const void *);
extern void (*nativeFinishInitialization)(void *);
extern void (*nativeContinueUsing)(void *), (*nativeUnplugged)(void *);
extern void (*settingsConstructor)(QWidget *, QWidget *);
extern bool (*nativePrerenderAllowed)(const void *);
} // namespace kobalt::nickel_qt6
