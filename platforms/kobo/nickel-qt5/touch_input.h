#pragma once
class QObject;

namespace kobalt::nickel_qt5 {
// Firmware 4 delivers touch events only. An unhandled touch reaches the Qt
// window and produces no mouse event, so plain Qt buttons never click. Stock
// Nickel uses its own touch widgets. This adapter gives Kobalt's windows the
// mouse press, move and release that Qt6 synthesizes on firmware 5.
void installTouchInput(QObject *owner);
} // namespace kobalt::nickel_qt5
