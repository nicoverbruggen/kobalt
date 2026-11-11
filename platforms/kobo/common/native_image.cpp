#include "platform/api/native_image.h"
#include <QBuffer>
#include <QImageReader>

namespace kobalt::platform {
namespace {
// kimg holds a Nickel image reference. Keep its encoded bytes, device and
// decoder together; the resource server keeps the reply and archive alive.
class Image final : public NativeImage
{
public:
    explicit Image(QByteArray reference)
        : reference_(std::move(reference)), source_(&reference_), reader_(&source_)
    {
        source_.open(QIODevice::ReadOnly);
    }

    QSize size() override { return reader_.size(); }

    QImage read() override { return reader_.read(); }

    bool canUsePlainRaster() const override
    {
        return !(reference_.size() == 20 && reference_.startsWith(QByteArray("GT\0", 3))
                 && reference_.at(11) != 0);
    }

private:
    QByteArray reference_;
    QBuffer source_;
    QImageReader reader_;
};
} // namespace

std::unique_ptr<NativeImage> nativeImage(const QByteArray &type, const QByteArray &body)
{
    if (type != "image/x-kobo")
        return {};
    return std::make_unique<Image>(body);
}
} // namespace kobalt::platform
