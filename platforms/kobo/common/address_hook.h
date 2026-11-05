#pragma once

namespace kobalt::nickel {
// NickelHook redirects calls through a library's PLT. Code that takes a
// function's address, such as a Qt connection to a member function, reads it
// from a GLOB_DAT entry instead and still reaches the original. This points
// those entries at `target` too, so a dialog button reaches the same hook as a
// direct call. An entry that no longer holds the library's own address, for
// example because another mod changed it, is left alone. Returns the number of
// entries redirected.
int redirectAddressReferences(void *library, const char *symbol, void *target);
// Nickel's USB confirmation dialog connects its Connect and Cancel buttons by
// member function address. Sends both through Kobalt's USB hooks, so Connect
// holds the workflow and saves before Nickel starts the connection, as a
// direct connection does. Call after NickelHook has installed its hooks.
void redirectUsbDialog();
} // namespace kobalt::nickel
