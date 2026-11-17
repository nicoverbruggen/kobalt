#pragma once
#include "platform/api/position_codec.h"

namespace kobalt::nickel {
// Shared across Qt5/Qt6. This codec reads the engine document on its worker;
// it does not call Nickel or retain firmware objects.
PositionCodec positionCodec();
// The UTF-16 units of chapter source text that positionCodec() keeps for
// `engine` until it closes. For tests.
int keptNativeText(const Engine &engine);
} // namespace kobalt::nickel
