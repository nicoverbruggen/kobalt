#include "nickel.h"
#include "platforms/kobo/common/native_light.h"
#include "platforms/kobo/common/native_ui.h"
#include "platform/api/kobo_refresh.h"
#include "platforms/kobo/display/sleep_refresh.h"
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#  include "platforms/kobo/nickel-qt5/refresh.h"
#else
#  include "platforms/kobo/nickel-qt6/refresh.h"
#endif
#include <QApplication>
#include <QBackingStore>
#include <QWidget>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

extern "C" void nh_log(const char *, ...) { }

extern "C" void *nh_dlhook(void *, const char *, void *)
{
    require(false, "native UI test attempted to install a loader hook");
    return nullptr;
}

// These stand-ins put the exported level method in a firmware-specific slot.
struct HardwareInterface
{
    void **vtable = nullptr;
    int percent = 0, state = 0;
    bool isCharging();
};

bool HardwareInterface::isCharging()
{
    return state != 0;
}

struct MTK : HardwareInterface
{
    int getBatteryLevel() const;
    int chargingState();
};

int MTK::getBatteryLevel() const
{
    return percent;
}

int MTK::chargingState()
{
    return state;
}

struct iMX508 : HardwareInterface
{
    int getBatteryLevel() const; // This model exercises the missing chargingState fallback.
};

int iMX508::getBatteryLevel() const
{
    return percent;
}

static HardwareInterface *currentHardware;

struct HardwareFactory
{
    static HardwareInterface *sharedInstance();
};

HardwareInterface *HardwareFactory::sharedInstance()
{
    return currentHardware;
}

static int batteryLookups;
extern "C" void *__real_dlsym(void *, const char *);

extern "C" void *__wrap_dlsym(void *handle, const char *name)
{
    if (std::strstr(name, "getBatteryLevel") || std::strstr(name, "chargingState"))
        ++batteryLookups;
    return __real_dlsym(handle, name);
}

static void checkBattery()
{
    auto check = [](int percent, bool charging, bool full) {
        const auto status = kobalt::nativeReaderBattery();
        require(status.percent == percent && status.charging == charging
                        && status.fullyCharged == full,
                "native battery must read the current device and charge state");
    };
    check(-1, false, false);
    MTK mtk;
    iMX508 imx;
    void *mtkTable[0xa0 / sizeof(void *)]{}, *imxTable[0xa0 / sizeof(void *)]{};
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const int mtkSlot = 0x84, imxSlot = 0x90;
#else
    const int mtkSlot = 0x9c, imxSlot = 0x9c;
#endif
    auto setLevel = [](void **table, int offset, const char *name) {
        table[offset / sizeof(void *)] = __real_dlsym(RTLD_DEFAULT, name);
        require(table[offset / sizeof(void *)], "Pewter battery implementation must be exported");
    };
    setLevel(mtkTable, mtkSlot, "_ZNK3MTK15getBatteryLevelEv");
    setLevel(imxTable, imxSlot, "_ZNK6iMX50815getBatteryLevelEv");
    mtk.vtable = mtkTable;
    currentHardware = &mtk;
    mtk.percent = 34;
    check(34, false, false);
    const auto firstLookups = batteryLookups;
    require(firstLookups > 0, "first battery reading must look up a hardware implementation");
    mtk.percent = 100;
    mtk.state = 2;
    check(100, true, true);
    currentHardware = &imx;
    imx.vtable = imxTable;
    imx.percent = 86;
    imx.state = 1;
    check(86, true, false);
    imx.percent = 101;
    check(-1, true, false);
    imxTable[imxSlot / sizeof(void *)] =
            __real_dlsym(RTLD_DEFAULT, "_ZN17HardwareInterface10isChargingEv");
    check(-1, false, false);
    currentHardware = nullptr;
    check(-1, false, false);
    require(batteryLookups == firstLookups,
            "repeated battery readings resolved hardware symbols again");
}

namespace {
using namespace kobalt::platform;
QRegion submitted;
quint64 submittedFlags;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool lookupDuringFlush = true;
#endif

template <typename Flags>
void captureSleepFlush(QBackingStore *, const QRegion &region, QWindow *, const QPoint &,
                       const Flags &flags)
{
    require(flags.size() == 1, "Power screen must have one native update policy");
    submitted = region;
    submittedFlags = flags.first().second;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (lookupDuringFlush)
        lookupReaderRefreshFlags(nullptr, submittedFlags, region.boundingRect());
#endif
}

void checkSleepRefresh()
{
    QWidget home, unrelated;
    unrelated.show();
    home.resize(300, 400);
    QWidget frame(&home);
    frame.setGeometry(home.rect());
    home.show();
    QApplication::processEvents();
    QBackingStore native(home.windowHandle()), other(unrelated.windowHandle());
    native.resize(home.size());
    other.resize(unrelated.size());
    nativeFlush = captureSleepFlush<FlushFlags>;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    nativeFlush32 = captureSleepFlush<FlushFlags32>;
    const int firstAttribute = 128;
    const int abis = 2;
#else
    // The compositor finds Nickel's old partial policy even when flush carries
    // different flags. The matching lookup must retain the requested cleanup.
    nativeRefreshFlags = [](void *, quint64 &flags, const QRect &) { flags = 4; };
    const int firstAttribute = Qt::WA_KoboEpdFlagsStart;
    const int abis = 1;
#endif
    const quint64 clean = (quint64(1) << (Qt::WA_KoboEpdWfModeGC16 - firstAttribute))
            | (quint64(1) << (Qt::WA_KoboEpdUpdateModeFull - firstAttribute));
    const QRect status(3, 4, 24, 20);
    SleepRefresh request;
    for (int abi = 0; abi < abis; ++abi) {
        auto send = [&](QBackingStore &store) {
            submitted = {};
            submittedFlags = 0;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            if (abi == 1)
                flushReader32(&store, status, store.window(), {}, { { status, 4 } });
            else
#endif
                flushReader(&store, status, store.window(), {}, { { status, 4 } });
        };
        request.begin(true);
        send(native);
        require(submittedFlags == 4, "Unpainted sleep frame consumed the flash");
        request.painted(&frame);
        send(other);
        require(submittedFlags == 4, "Unrelated window consumed the sleep flash");
        send(native);
        require(submittedFlags == clean && submitted == QRegion(home.rect()),
                "First power-frame flush did not clear the whole screen with full GC16");
        request.painted(&frame);
        send(native);
        require(submittedFlags == 4 && submitted == QRegion(status),
                "Status update repeated the sleep flash");
        request.begin(false);
        request.painted(&frame);
        send(native);
        require(submittedFlags == 4, "Clean text requested a flash");
        request.begin(true);
        request.painted(&frame);
        request.reset();
        send(native);
        require(submittedFlags == 4, "Wake left a pending sleep flash");
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    lookupDuringFlush = false;
    request.begin(true);
    request.painted(&frame);
    flushReader(&native, status, native.window(), {}, { { status, 4 } });
    require(submitted == QRegion(home.rect()) && submittedFlags == clean,
            "Deferred MTK lookup had no full-frame submission");
    flushReader(&native, status, native.window(), {}, { { status, 4 } });
    require(submitted == QRegion(status) && submittedFlags == 4,
            "Second backing-store flush resubmitted the full screen");
    quint64 flags = 0;
    lookupReaderRefreshFlags(nullptr, flags, home.rect());
    require(flags == clean, "Delayed MTK lookup replaced GC16 with Nickel's stale policy");
    lookupReaderRefreshFlags(nullptr, flags, home.rect());
    require(flags == 4, "Delayed MTK lookup repeated the cleanup");
    request.begin(true);
    request.painted(&frame);
    lookupReaderRefreshFlags(nullptr, flags, status);
    require(flags == 4, "An unrelated compositor rectangle consumed the sleep flash");
    lookupReaderRefreshFlags(nullptr, flags, home.rect());
    require(flags == clean, "Direct native composition missed the sleep flash without a reader");
    lookupReaderRefreshFlags(nullptr, flags, home.rect());
    require(flags == 4, "Direct native composition repeated the sleep flash");
    nativeRefreshFlags = nullptr;
#else
    nativeFlush32 = nullptr;
#endif
    nativeFlush = nullptr;
}
} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    checkSleepRefresh();
    checkBattery();
    require(!kobalt::nativeLandscapeBezelOnLeft(), "other panels keep the standard landscape turn");
    pewter::options().claraColour = true;
    require(kobalt::nativeLandscapeBezelOnLeft(), "Clara Colour reverses landscape");
    pewter::options().claraColour = false;
    pewter::options().claraBW = true;
    require(kobalt::nativeLandscapeBezelOnLeft(), "Clara BW reverses landscape");
    pewter::options().claraBW = false;
    QWidget opening;
    QWidget cover(&opening);
    for (bool dark : { false, true }) {
        opening.setProperty("reader_dark", dark);
        kobalt::platform::pageRefresh(&cover, true);
        kobalt::platform::coverRefresh(&cover);
        require(cover.testAttribute(Qt::WA_KoboEpdUpdateModePartial),
                "Cover replacement did not request partial mode");
        require(!cover.testAttribute(Qt::WA_KoboEpdUpdateModeFull)
                        && !cover.testAttribute(Qt::WA_KoboEpdUpdateModeHeuristic)
                        && !cover.testAttribute(Qt::WA_KoboEpdWfModeGC16),
                "Cover replacement retained a full refresh or cleanup policy");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        require(cover.testAttribute(static_cast<Qt::WidgetAttribute>(dark ? 141 : 137))
                        && !cover.testAttribute(static_cast<Qt::WidgetAttribute>(140)),
                "Qt5 cover replacement did not request its non-cleanup waveform");
#else
        require(cover.testAttribute(Qt::WA_KoboEpdWfModeAutoText)
                        && !cover.testAttribute(Qt::WA_KoboEpdWfModeAutoImage)
                        && !cover.testAttribute(Qt::WA_KoboEpdWfModeGCK16)
                        && cover.testAttribute(Qt::WA_KoboEpdFlagDarkMode) == dark,
                "Qt6 cover replacement lost its text waveform or dark flag");
#endif
    }
    for (bool cover : { false, true }) {
        for (bool full : { false, true }) {
            for (bool info : { false, true }) {
                pewter::options().sleepShowCover = cover;
                pewter::options().sleepFullScreen = full;
                pewter::options().sleepInfo = info;
                pewter::clearCalls();
                const auto preview = kobalt::nativeSleepPreview();
                require(preview.showCover == cover && preview.fullScreen == full
                                && preview.showInfo == info,
                        "preview must read current native power settings");
                requireNames(pewter::callNames(),
                             QList<QByteArray>({ "Settings::construct", "Settings::destroy" }),
                             "native settings are released after each query");
            }
        }
    }
    Volume volume(new VolumeData);
    for (bool tolino : { false, true }) {
        pewter::options().tolino = tolino;
        require(kobalt::nativeReaderSleepScreenAvailable(), "sleep screens support both brands");
        require(kobalt::nativeReaderCoverPath(nullptr).isEmpty(), "missing volume has no cover");
        require(kobalt::nativeReaderCoverPath(&volume).isEmpty(), "empty image ID has no cover");
        volume.data->values["ImageID"] = QStringLiteral("default");
        require(kobalt::nativeReaderCoverPath(&volume).isEmpty(), "default image is not a cover");
        volume.data->values["ImageID"] = QStringLiteral("book");
        pewter::clearCalls();
        requireSame(kobalt::nativeReaderCoverPath(&volume), QStringLiteral("/covers/book-N3_FULL"),
                    "both brands use the full-size native cover");
        requireSame(pewter::find("Image::getFileName")->arguments.value("type"),
                    QStringLiteral("N3_FULL"), "cover lookup requests the full-size image");
        volume.data->values.clear();
    }
    std::puts("PASS native UI sleep, covers and battery");
    return 0;
}
