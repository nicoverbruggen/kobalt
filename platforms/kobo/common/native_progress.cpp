#include "native_progress.h"
#include <dlfcn.h>
#include <utility>

namespace kobalt {
namespace {
template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

quint64 nextConnectionId = 0;
} // namespace

QPointer<NativeProgress> NativeProgress::active;
#ifndef KOBALT_READER_HARDWARE
bool NativeProgress::failNextWrite = false;
#endif
NativeProgress::NativeProgress(QString identity)
    : book(std::move(identity)), connectionId(++nextConnectionId)
{
    active = this;
#ifndef KOBALT_READER_HARDWARE
    failNextWrite = false;
#endif
    bool usbSignalled = false, syncSignalled = false;
    const auto manager = symbol<QObject *(*)()>("_ZN19PlugWorkflowManager14sharedInstanceEv");
    if (manager) {
        auto *usb = manager();
        connect(usb, SIGNAL(aboutToConnect()), this, SLOT(beginTransition()));
        connect(usb, SIGNAL(aboutToUnplug()), this, SLOT(beginTransition()));
        connect(usb, SIGNAL(startedProcessing()), this, SLOT(beginProcessing()));
        usbSignalled = bool(connect(usb, SIGNAL(doneProcessing()), this, SLOT(endTransition())));
    }
    if (const auto plug = symbol<QObject *(*)()>("_ZN11PlugManager14sharedInstanceEv")) {
        // Nickel can call its unplug slot without passing through the PLT
        // hook. Wait for that slot to finish before clearing a charge-only
        // transition; an exported session still waits for doneProcessing.
        connect(plug(), SIGNAL(unplugged()), this, SLOT(finishUnplug()), Qt::QueuedConnection);
    }
    if (const auto sync = symbol<QObject *(*)()>("_ZN13N3SyncManager14sharedInstanceEv")) {
        const bool finished = bool(connect(sync(), SIGNAL(finished()), this, SLOT(markChanged())));
        const bool profile =
                bool(connect(sync(), SIGNAL(userProfileUpdated()), this, SLOT(markChanged())));
        syncSignalled = finished && profile;
    }
    // Without these signals the reader falls back to reading Nickel's saved
    // position once a minute.
    signalled = usbSignalled && syncSignalled;
}

NativeProgress::~NativeProgress()
{
    if (active == this)
        active.clear();
}

void NativeProgress::disconnected()
{
    if (active)
        active->finishUnplug();
}
} // namespace kobalt
