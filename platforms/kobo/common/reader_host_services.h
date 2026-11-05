#pragma once
#include "platform/api/reader_host_services.h"

namespace kobalt::nickel {
// Construct only after the firmware adapter has accepted the running Nickel.
ReaderHostServices readerHostServices();
} // namespace kobalt::nickel
