#pragma once
#include <QObject>
#include <memory>
#include "native_archive.h"
#include "publication_endpoint.h"

namespace kobalt::native {
// Serves the selected provider's resources through the reader's loopback transport.
class ResourceServer : public QObject, public PublicationEndpoint
{
public:
    explicit ResourceServer(QObject *parent = nullptr);
    ~ResourceServer();
    bool open(const QString &path, const OpenArchive &openArchive);
    int port() const override;

    bool active() const override { return port() != 0; }

private:
    struct Data;
    std::unique_ptr<Data> data_;
};
} // namespace kobalt::native
