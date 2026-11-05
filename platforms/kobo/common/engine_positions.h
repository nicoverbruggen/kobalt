#pragma once
#include "platform/api/position_codec.h"

namespace kobalt::nickel {
// Shared across Qt5/Qt6. This codec reads the engine document on its worker;
// it does not call Nickel or retain firmware objects.
PositionCodec positionCodec();
} // namespace kobalt::nickel
