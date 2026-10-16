#pragma once

#include <QImage>
#include <QString>

namespace kobalt::reader {
// Takes ownership of one reading-page capture and paints in place. The caller
// releases the completed frame to the host before the device suspends.
QImage sleepScreenImage(QImage page, const QString &progress, const QString &remaining, bool dark);
}
