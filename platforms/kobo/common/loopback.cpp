#include "platform/api/loopback.h"
#include <cerrno>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kobalt::platform {
bool configureLoopback(int *error)
{
    *error = 0;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = errno;
        return false;
    }

    struct Close
    {
        int fd;

        ~Close() { close(fd); }
    } closeSocket{ fd };

    ifreq interface{};
    std::strcpy(interface.ifr_name, "lo");
    if (ioctl(fd, SIOCGIFFLAGS, &interface) < 0) {
        *error = errno;
        return false;
    }
    const short flags = interface.ifr_flags;
    if (!(flags & IFF_LOOPBACK)) {
        *error = ENODEV;
        return false;
    }
    if (ioctl(fd, SIOCGIFADDR, &interface) == 0) {
        if (flags & IFF_UP)
            return false;
        interface.ifr_flags = flags | IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &interface) == 0)
            return true;
        *error = errno;
        return false;
    }
    if (errno != EADDRNOTAVAIL) {
        *error = errno;
        return false;
    }
    const auto address = [&](unsigned value) {
        sockaddr_in ip{};
        ip.sin_family = AF_INET;
        ip.sin_addr.s_addr = htonl(value);
        std::memcpy(&interface.ifr_addr, &ip, sizeof(ip));
    };
    address(INADDR_LOOPBACK);
    if (ioctl(fd, SIOCSIFADDR, &interface) < 0) {
        *error = errno;
        return false;
    }
    address(0xff000000U);
    if (ioctl(fd, SIOCSIFNETMASK, &interface) == 0) {
        interface.ifr_flags = flags | IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &interface) == 0)
            return true;
    }
    *error = errno;
    // Undo our address if setup cannot finish. Preserve the original failure.
    address(INADDR_ANY);
    ioctl(fd, SIOCSIFADDR, &interface);
    interface.ifr_flags = flags;
    ioctl(fd, SIOCSIFFLAGS, &interface);
    return false;
}
} // namespace kobalt::platform
