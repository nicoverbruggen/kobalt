#pragma once

#include "resource_source.h"
#include "source_lease.h"

namespace kobalt::native {
// Owned on the GUI thread. The host keeps this endpoint alive until engine
// and chapter work drains and resource connections have been released.
class PublicationEndpoint
{
public:
    virtual ~PublicationEndpoint() = default;
    virtual int port() const = 0;
    virtual bool active() const = 0;

    // A direct backend supplies worker-side resources. Other adapters retain
    // their existing network transport when this factory is empty.
    virtual ResourceFactory resources(std::shared_ptr<SourceLease>) const { return {}; }
};
} // namespace kobalt::native
