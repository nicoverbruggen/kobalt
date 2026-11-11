#include "platform/api/native_image.h"

namespace kobalt::platform {
std::unique_ptr<NativeImage> nativeImage(const QByteArray &, const QByteArray &)
{
    return {};
}
} // namespace kobalt::platform
