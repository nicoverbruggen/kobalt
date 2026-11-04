#pragma once
#include <functional>
#include <memory>

namespace kobalt {
// The reader owns one controller. Preparing a connection is asynchronous;
// cancel stops this caller's wait without changing the user's Wi-Fi preference.
class SyncNetwork {
public:
    virtual ~SyncNetwork() = default;
    virtual bool connected() const = 0;
    virtual void prepare(bool interactive, std::function<void(bool)> finished) = 0;
    virtual void cancel() = 0;
    // Turns Wi-Fi off again only when this controller turned it on silently
    // for its last preparation. A connection the user made stays on.
    virtual void release() {}
};
using SyncNetworkFactory = std::function<std::unique_ptr<SyncNetwork>()>;
}
