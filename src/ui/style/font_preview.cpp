#include "font_preview.h"
#include "metrics.h"
#include "private_text.h"
#include "support/diagnostics.h"
#include <QCache>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>

namespace kobalt::ui {
namespace {
QString identity(const QStringList &files)
{
    QString result;
    for (const auto &path : files) {
        const QFileInfo file(path);
        result += path + '\n' + QString::number(file.size()) + ':'
                + QString::number(file.lastModified().toMSecsSinceEpoch()) + '\n';
    }
    return result;
}

QCache<QString, QImage> &cache()
{
    static QCache<QString, QImage> value(fontPreviewBitmapBudget);
    return value;
}
} // namespace

FontPreviewSource fontPreviewSource(const QString &family, const QStringList &files)
{
    if (family.isEmpty() || files.isEmpty() || files.size() > 64)
        return {};
    return { family, files, identity(files) };
}

bool drawFontPreview(QPainter &painter, QRect area, const QString &text,
                     const FontPreviewSource &source, int inkHeight, int weight, bool boldReading,
                     bool centered, bool wrap)
{
    // The same size limits as the interface sample font.
    const FontPreviewScale scale{ inkHeight, metrics::choices::sampleBase,
                                  metrics::choices::sampleMinimum, metrics::choices::sampleMaximum,
                                  px(5) };
    if (source.files.isEmpty() || source.identity.size() > 16384 || text.isEmpty()
        || text.size() > 256 || area.width() <= 0 || area.height() <= 0 || area.width() > 2048
        || area.height() > 512 || qint64(area.width()) * area.height() > 256 * 1024)
        return false;
    const auto keyData = source.identity + source.family + '\n' + text + '\n'
            + QString::number(area.width()) + ':' + QString::number(area.height()) + ':'
            + QString::number(scale.inkHeight) + ':' + QString::number(scale.base) + ':'
            + QString::number(scale.minimum) + ':' + QString::number(scale.maximum) + ':'
            + QString::number(scale.lineGap) + ':' + QString::number(weight) + ':'
            + QString::number(boldReading) + ':' + QString::number(centered) + ':'
            + QString::number(wrap);
    const auto key = QString::fromLatin1(
            QCryptographicHash::hash(keyData.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QImage *bitmap = cache().object(key);
    if (!bitmap) {
        // Render while painting, so a page shows all its samples in one screen
        // update. Each face is opened from its file and closed afterwards.
        QElapsedTimer timer;
        timer.start();
        QImage image;
        try {
            if (identity(source.files) == source.identity)
                image = renderFontPreviewBitmap(source.files, source.family, text, area.size(),
                                                scale, weight, boldReading, centered, wrap,
                                                privateInterfaceFontFiles());
        } catch (const std::bad_alloc &) {
            image = {};
        }
        // A failed sample is cached too, so its label does not reopen the files on every paint.
        const int bytes = image.bytesPerLine() * image.height()
                + image.colorCount() * int(sizeof(QRgb)) + key.size() * 2 + 256;
        cache().insert(key, new QImage(image), qMax(1, bytes));
        setDiagnosticPreviewBytes(cache().totalCost());
        diagnostic("font-preview-bitmap",
                   { { "elapsed_ms", double(timer.elapsed()) },
                     { "bitmap_bytes", image.bytesPerLine() * image.height() },
                     { "cache_bytes", cache().totalCost() },
                     { "ok", !image.isNull() } },
                   false);
        bitmap = cache().object(key);
    }
    if (!bitmap || bitmap->isNull())
        return false;
    auto colored = *bitmap;
    const auto ink = painter.pen().color();
    QVector<QRgb> colors(256);
    for (int i = 0; i < 256; ++i)
        colors[i] = qRgba(ink.red(), ink.green(), ink.blue(), i * ink.alpha() / 255);
    colored.setColorTable(colors);
    painter.drawImage(area.topLeft(), colored);
    return true;
}

int fontPreviewCacheBytes()
{
    return cache().totalCost();
}
} // namespace kobalt::ui
