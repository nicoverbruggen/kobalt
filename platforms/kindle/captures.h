#pragma once
#include <QString>

namespace kobalt::kindle {
class Window;
void captureScreens(Window &window, const QString &book, const QString &directory);
}
