#pragma once
class QWidget;
class QObject;
class QKeyEvent;

// Internal contract between Nickel's hook entry point and its reader controller.
// Other application targets do not include this header.
namespace kobalt::nickel_qt5 {
bool firmwareSupported();
int initializeReader();
#ifdef KOBALT_READER_HARDWARE
bool uninstallReader();
#endif
extern void (*nativeShowReader)(void *, const void *, const void *, void *, const void *);
extern void (*nativeFinishInitialization)(void *);
extern bool (*nativeHomeKeyFilter)(void *, QObject *, QKeyEvent *);
extern void (*nativeContinueUsing)(void *), (*nativeUnplugged)(void *);
} // namespace kobalt::nickel_qt5
