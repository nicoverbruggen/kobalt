#pragma once
#include "platform/api/reader_host_services.h"

namespace kobalt::test {
// Deterministic desktop defaults preserve the accepted capture profile. The
// opt-in test fixture adds in-memory light controls, never hardware commands.
ReaderHostServices hostReaderServices(bool ambientLightSensor = false);
}
