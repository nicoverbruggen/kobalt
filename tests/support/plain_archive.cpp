#include "plain_archive.h"
#include "publication/publication.h"
#include "publication/resource_cache.h"
#include "publication/zip_resource.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QFile>
#include <QNetworkReply>
#include <QThread>
#include <cstring>

namespace kobalt::native {
namespace {
bool guiThread()
{
    return qApp && QThread::currentThread() == qApp->thread();
}

class Reply : public QNetworkReply
{
public:
    Reply(QByteArray bytes, bool ok) : bytes_(std::move(bytes))
    {
        open(QIODevice::ReadOnly);
        setHeader(QNetworkRequest::ContentLengthHeader, bytes_.size());
        if (!ok)
            setError(ContentAccessDenied, QStringLiteral("Plain resource unavailable"));
        qt::later(0, this, [this] {
            emit readyRead();
            emit finished();
        });
    }

    void abort() override { close(); }

    qint64 bytesAvailable() const override
    {
        return bytes_.size() - offset_ + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *out, qint64 count) override
    {
        count = qMin(count, qint64(bytes_.size()) - offset_);
        std::memcpy(out, bytes_.constData() + offset_, size_t(count));
        offset_ += count;
        return count;
    }

private:
    QByteArray bytes_;
    qint64 offset_ = 0;
};
} // namespace

namespace {
class PlainArchive final : public ResourceArchive
{
public:
    PlainArchive();
    ~PlainArchive() override;
    static bool available();
    bool open(const QString &path);
    QNetworkReply *request(const QString &name) override;
    bool canStreamPlainImage(const QString &name) const override;
    void close();

private:
    struct Data;
    std::unique_ptr<Data> data_;
};

struct PlainArchive::Data
{
    QString path;
    QMap<QString, qint64> sizes;
    // Requests are restricted to the GUI thread.
    ZipResourceIndex index;
};

PlainArchive::PlainArchive() = default;
PlainArchive::~PlainArchive() = default;

bool PlainArchive::available()
{
    return guiThread();
}

void PlainArchive::close()
{
    data_.reset();
}

bool PlainArchive::open(const QString &path)
{
    close();
    if (!available())
        return false;
    QFile file(path);
    Publication publication;
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        reportDescriptorExhaustion("book-open");
        return false;
    }
    if (!publication.inspect(file, &error))
        return false;
    auto data = std::make_unique<Data>();
    data->path = path;
    data->sizes = publication.sizes();
    data_ = std::move(data);
    return true;
}

QNetworkReply *PlainArchive::request(const QString &name)
{
    if (!guiThread() || !data_)
        return nullptr;
    if (!data_->sizes.contains(name))
        return nullptr;
    // Never ask Nickel to decode a resource. CREngine reads the original
    // bytes even when the advisory formatting inspection is incomplete.
    ZipResource stream;
    if (data_->sizes.value(name) > ResourceCache::budgetBytes
        || !stream.open(data_->path, name, data_->index))
        return new Reply({}, false);
    QByteArray bytes;
    while (stream.valid() && !stream.finished())
        bytes += stream.next();
    const bool ok = stream.valid() && stream.finished();
    return new Reply(ok ? bytes : QByteArray(), ok);
}

bool PlainArchive::canStreamPlainImage(const QString &name) const
{
    return guiThread() && data_ && data_->sizes.contains(name);
}
} // namespace

std::unique_ptr<ResourceArchive> openPlainArchive(const QString &path)
{
    auto archive = std::make_unique<PlainArchive>();
    if (!archive->open(path))
        return {};
    return archive;
}

} // namespace kobalt::native
