#pragma once
#include "support/qt_compat.h"
#include <QByteArray>
#include <QSize>
#include <QString>

namespace kobalt {
// This is a per-image bitmap limit, separate from the publication source cache.
// A 3000 x 4000 cover fits; an 8000 x 8000 scan does not.
constexpr quint64 imagePixelLimit = 16 * 1024 * 1024;
constexpr qsizetype imageHeaderLimit = 1024 * 1024;

struct ImageHeader
{
    enum Action { More, Keep, Replace } action = More;

    QSize size;
};

ImageHeader inspectImageHeader(const QByteArray &prefix);
bool imageFileName(const QString &name);
QByteArray imagePlaceholder(QSize original = {}, const QString &systemFont = "sans-serif");
} // namespace kobalt
