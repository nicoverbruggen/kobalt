#pragma once
#include <QByteArray>
#ifdef __linux__
#  include <linux/fb.h>
#  include <fcntl.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
#  include <cstring>
#endif

namespace kobalt {
// MediaTek panels (Clara BW and Colour, Libra Colour, Elipsa 2E) report
// "hwtcon". i.MX panels report a name that starts with "mxc_epdc".
inline bool mtkFramebuffer(const QByteArray &driver)
{
    return driver == "hwtcon";
}

inline bool imxFramebuffer(const QByteArray &driver)
{
    return driver.startsWith("mxc_epdc");
}

#ifdef __linux__
// The driver id of /dev/fb0, read once. Empty when the device has none.
inline const QByteArray &framebufferDriver()
{
    static const QByteArray driver = [] {
        QByteArray result;
        const int fd = ::open("/dev/fb0", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            fb_fix_screeninfo info{};
            if (::ioctl(fd, FBIOGET_FSCREENINFO, &info) == 0)
                result = QByteArray(info.id, int(strnlen(info.id, sizeof(info.id))));
            ::close(fd);
        }
        return result;
    }();
    return driver;
}
#endif
} // namespace kobalt
