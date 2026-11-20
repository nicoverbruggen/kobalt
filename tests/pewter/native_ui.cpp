#include "nickel.h"
#include "platforms/kobo/common/native_light.h"
#include "platforms/kobo/common/native_ui.h"
#include "platform/api/kobo_refresh.h"
#include <QApplication>
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

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
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
