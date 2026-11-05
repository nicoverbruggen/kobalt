#include "engine_document.h"
#include "column_view.h"
#include "publication/image_policy.h"
#include <lvdrawbuf.h>
#include <lvimg.h>
#include <QCryptographicHash>

namespace kobalt {
QImage EngineDocument::imageAt(QPoint point)
{
    if (!engine_ || !QRect(QPoint(), size_).contains(point))
        return {};
    const auto pointer = engine_->getNodeByPoint(lvPoint(point.x(), point.y()), true);
    auto *node = pointer.isNull() ? nullptr : pointer.getNode();
    auto source = node ? node->getObjectImageSource() : LVImageSourceRef();
    // CREngine's synthetic cover has no DOM node.
    const int current = engine_->getCurPage(true);
    const auto *pages = engine_->getPageList();
    if (source.isNull() && current >= 0 && current < pages->length()
        && (pages->get(current)->flags & RN_PAGE_TYPE_COVER))
        source = engine_->getCoverPageImage();
    if (source.isNull() || source->GetWidth() <= 0 || source->GetHeight() <= 0
        || quint64(source->GetWidth()) * source->GetHeight() > imagePixelLimit)
        return {};
    QImage image(source->GetWidth(), source->GetHeight(), QImage::Format_RGB32);
    if (image.isNull())
        return {};
    // Keep illustrations in their original colours, also in the dark theme.
    image.fill(Qt::white);
    LVColorDrawBuf buffer(image.width(), image.height(), image.bits(), 32);
    buffer.setSmoothScalingImages(true);
    buffer.Draw(source, 0, 0, image.width(), image.height(), false);
    // Content identity survives reflow, repeated references and DOM boxing.
    QCryptographicHash identity(QCryptographicHash::Sha256);
    identity.addData(QByteArray("image-v1:") + QByteArray::number(image.width()) + ':'
                     + QByteArray::number(image.height()) + ':');
    for (int y = 0; y < image.height(); ++y) {
        auto *pixels = reinterpret_cast<quint32 *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            pixels[x] |= 0xff000000U;
        identity.addData(
                QByteArray::fromRawData(reinterpret_cast<const char *>(pixels), image.width() * 4));
    }
    image.setText("kobalt.image_id", QString::fromLatin1(identity.result().toHex()));
    return image;
}
} // namespace kobalt
