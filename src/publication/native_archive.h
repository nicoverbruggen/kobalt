#pragma once
#include <QString>
#include <functional>
#include <memory>

class QNetworkReply;

namespace kobalt::native {
// Construct, request and destroy on the GUI thread. Each request transfers reply
// ownership to its caller, which must abort/delete it before destroying this
// provider. Completion stays asynchronous, including native replies that emit
// finished() without setting isFinished(). No nested event loop is required.
class ResourceArchive
{
public:
    virtual ~ResourceArchive() = default;
    virtual QNetworkReply *request(const QString &name) = 0;
    virtual bool canStreamPlainImage(const QString &name) const = 0;
};

// Invoked synchronously, at most once, after publication inspection. The server
// does not retain this callback or any borrowed native arguments it captures.
using OpenArchive = std::function<std::unique_ptr<ResourceArchive>()>;
} // namespace kobalt::native
