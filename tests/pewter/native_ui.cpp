#include "nickel.h"
#include "platforms/kobo/common/native_light.h"
#include "platforms/kobo/common/native_ui.h"
#include "platform/api/kobo_refresh.h"
#include <QApplication>
#include <QWidget>
#include <cstdio>

extern "C" void nh_log(const char *, ...) { }

extern "C" void *nh_dlhook(void *, const char *, void *)
{
    require(false, "native UI test attempted to install a loader hook");
    return nullptr;
}

// Checks native cover lookup, sleep capability and local partial cover refresh flags.
int main(int argc, char **argv)
{
    QApplication application(argc, argv);
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
    std::puts("PASS native UI sleep and covers");
    return 0;
}
