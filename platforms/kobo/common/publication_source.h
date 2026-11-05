#pragma once
#include "publication/publication_endpoint.h"
#include <QString>
#include <memory>

namespace kobalt::native {
// Nickel composition only. Native arguments are borrowed for the synchronous
// factory call; each implementation retains the native objects it needs.
struct PublicationSource
{
    bool (*available)();
    std::unique_ptr<PublicationEndpoint> (*open)(const void *volume, const void *keys,
                                                 const QString &path, const QString &identity,
                                                 QString *error);
};

const PublicationSource &plainPublicationSource();
const PublicationSource &publicationSource();
} // namespace kobalt::native
