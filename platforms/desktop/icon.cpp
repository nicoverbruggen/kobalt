#include <lunasvg.h>
#include <QImage>
#include <QDir>
#include <QString>

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    const auto svg = lunasvg::Document::loadFromFile(argv[1]);
    if (!svg || !QDir().mkpath(QString::fromLocal8Bit(argv[2])))
        return 1;
    for (const int size : { 16, 32, 128, 256, 512 }) {
        for (const int scale : { 1, 2 }) {
            auto bitmap = svg->renderToBitmap(size * scale, size * scale);
            QImage image(bitmap.data(), bitmap.width(), bitmap.height(), bitmap.stride(),
                         QImage::Format_ARGB32_Premultiplied);
            const auto name = QString("%1/icon_%2x%2%3.png")
                                      .arg(QString::fromLocal8Bit(argv[2]))
                                      .arg(size)
                                      .arg(scale == 2 ? "@2x" : "");
            if (!image.save(name))
                return 1;
        }
    }
}
