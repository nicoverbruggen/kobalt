#pragma once
#ifndef KOBALT_READER_HARDWARE
#  include <QJsonObject>

namespace kobalt::nickel {
// Firmware-specific ReadingSettings storage stays outside shared lifecycle code.
void sendTestPageButton(const QJsonObject &command, bool blocked);
} // namespace kobalt::nickel
#endif
