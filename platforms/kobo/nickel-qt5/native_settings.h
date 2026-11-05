#pragma once
#include <functional>
class QObject;
class QWidget;

namespace kobalt::nickel_qt5 {
// Find existing Reading settings pages and observe new pages through Qt. This
// also covers pages whose constructor was called before Kobalt was ready.
void watchReadingSettings(QObject *owner, std::function<void(QWidget *)> install);
} // namespace kobalt::nickel_qt5
