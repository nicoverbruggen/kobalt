#pragma once
#include "publication/resource_source.h"
#include "publication/source_lease.h"
#include <QPointer>
#include <QString>
#include <memory>

namespace kobalt {
namespace native {
class PublicationEndpoint;
}
// Keeps the native request alive while Kobalt loads asynchronously.
class NativeOpening {
public:
    NativeOpening();
    ~NativeOpening();
    NativeOpening(const NativeOpening &) = delete;
    NativeOpening &operator=(const NativeOpening &) = delete;
    static bool available();
    bool retain(const void *volume, const void *context);
    const void *volume() const;
    const void *bookmark() const;
    const void *keys() const;
    void openStock();

private:
    struct Data;
    std::unique_ptr<Data> data_;
};

// Called only after Nickel has accepted the opening request, on its GUI thread.
// Use only after firmwareSupported() accepts Nickel.
class NativePublication {
public:
    NativePublication();
    ~NativePublication();
    NativePublication(const NativePublication &) = delete;
    NativePublication &operator=(const NativePublication &) = delete;

    static bool available();
    bool open(const void *volume, const void *bookmark, QString *error, const void *keys = nullptr);
    QString identity() const {
        return identity_;
    }
    QString path() const {
        return path_;
    }
    QString location() const {
        return location_;
    }
    QString coverPath() const {
        return coverPath_;
    }
    int port() const {
        return port_;
    }
    bool active() const;
    ResourceFactory resources(std::shared_ptr<SourceLease> lease) const;

private:
    std::unique_ptr<native::PublicationEndpoint> source_;
    QString identity_, path_, location_, coverPath_;
    int port_ = 0;
    void close();
};
}
