#pragma once

namespace kobalt::platform {
// Brings lo up and adds 127.0.0.1/8 only when it has no IPv4 address.
// Preserves existing addresses and other interfaces. False reports errno,
// or zero for no change.
bool configureLoopback(int *error);
} // namespace kobalt::platform
