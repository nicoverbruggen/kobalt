#pragma once
#include "platform/api/window_presentation.h"
#include <memory>
class QWidget;

namespace kobalt::nickel {
std::unique_ptr<WindowPresentation> makeWindowPresentation(QWidget *reader, bool sleepScreenSupported);
}
