#pragma once
#include <QByteArray>
#include <QImage>
#include <functional>
#include <memory>

namespace kobalt {
// A host-owned decoder for an image reference returned by a native archive.
// The caller retains the archive and its reply until the decoder is destroyed.
class NativeImage
{
public:
    virtual ~NativeImage() = default;
    virtual QSize size() = 0;
    virtual QImage read() = 0;
    // Native effects can require decoding even when matching plaintext exists.
    virtual bool canUsePlainRaster() const = 0;
};

using NativeImageFactory =
        std::function<std::unique_ptr<NativeImage>(const QByteArray &type, const QByteArray &body)>;

namespace platform {
// Null means ordinary resource bytes, with no host-specific conversion.
std::unique_ptr<NativeImage> nativeImage(const QByteArray &type, const QByteArray &body);
} // namespace platform
} // namespace kobalt
