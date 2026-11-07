#include "nickel.h"
#include "platforms/kobo/common/native_ui.h"
#include <QCoreApplication>
#include <cstdio>

extern "C" void nh_log(const char *, ...) { }

extern "C" void *nh_dlhook(void *, const char *, void *)
{
    require(false, "native UI test attempted to install a loader hook");
    return nullptr;
}

// Checks sleep capability and full-size cover lookup for both Kobo and Tolino.
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
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
