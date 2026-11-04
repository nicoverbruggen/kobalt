#pragma once

namespace kobalt::nickel {
// Nickel clears pending analytics after attempting their save. Acquire the
// write transaction first, as ReadingController does around its native saves.
// Nickel's nested ScopedTransaction does not commit a transaction it did not
// start. A busy begin must leave both the buffer and someone else's work alone.
template <class Begin, class Flush, class End>
bool saveNativeEvents(Begin begin, Flush flush, End end) {
    if (!begin())
        return false;
    const bool saved = flush();
    const bool committed = end(saved);
    return saved && committed;
}
}
