#include "publication_source.h"
#include "publication/local_resources.h"
#include <QCoreApplication>
#include <QThread>
#include "support/strings.h"

namespace kobalt::native {
namespace {
bool available()
{
    return qApp && QThread::currentThread() == qApp->thread();
}

std::unique_ptr<PublicationEndpoint> open(const void *, const void *, const QString &path,
                                          const QString &, QString *error)
{
    auto publication = openLocalPublication(path, error);
    if (!publication) {
        if (error)
            *error = strings::get("error.cannot_start_the_native_publication_service");
        return {};
    }
    return publication;
}
} // namespace

const PublicationSource &plainPublicationSource()
{
    static const PublicationSource source{ available, open };
    return source;
}
} // namespace kobalt::native
