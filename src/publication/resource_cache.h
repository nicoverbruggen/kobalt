#pragma once
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QSize>
#include <QSet>
#include <array>
#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <vector>

namespace kobalt {
// Streams keep names and offsets, never pointers into cache storage. Evicting a
// resource therefore releases its bytes even while the engine retains a stream.
class ResourceCache
{
public:
    enum class Failure { None, Transfer, Limit, Cancelled, Changed };
    static constexpr qint64 blockBytes = 64 * 1024;
    static constexpr qint64 budgetBytes = 32 * 1024 * 1024;
    using Sink = std::function<bool(const char *, qint64)>;
    using Loader = std::function<bool(const QString &, const Sink &, QString *)>;
    using ImageProbe = std::function<QSize(const QString &)>;
    using Statistics = std::function<QJsonObject()>;

    struct Access
    {
        Loader loader;
        std::function<bool()> cancelled;
        Statistics statistics;
        ImageProbe imageProbe;
    };

    ResourceCache(QStringList names, Access access);
    // Worker-only, after the previous opening has drained. Existing streams
    // retain this cache object; all access callbacks must change together.
    bool rebind(const ResourceCache &fresh);
    ResourceCache(QStringList names, Loader loader, std::shared_ptr<std::atomic_bool> cancelled,
                  Statistics transferStatistics = {}, ImageProbe imageProbe = {});
    QSize deferredImageSize(const QString &name);
    bool imageReady(const QString &name);
    bool loadNextImage();
    // Keep dimensions and integrity checks, but no encoded resource bytes.
    void releaseBytes();

    quint64 imageRevision() const { return imageRevision_; }

    bool contains(const QString &name) const { return names_.contains(name); }

    const QStringList &names() const { return names_; }

    QByteArray readBounded(const QString &name, qint64 limit);
    // After advisory inspection, let the engine retry with its normal resource
    // budget. Transfer, cancellation and changed-byte failures remain fatal.
    bool resumeAfterInspection();
    qint64 size(const QString &name);
    qint64 read(const QString &name, qint64 offset, void *buffer, qint64 count);

    QString error() const { return error_; }

    Failure failure() const { return failure_; }

    QJsonObject statistics() const;

private:
    using Block = std::array<char, blockBytes>;

    struct Entry
    {
        qint64 size = 0;
        std::vector<std::unique_ptr<Block>> blocks;
    };

    QStringList names_;
    Loader loader_;
    Statistics transferStatistics_;
    ImageProbe imageProbe_;
    QMap<QString, QSize> imageSizes_;
    QSet<QString> readyImages_;
    QStringList pendingImages_;
    quint64 imageRevision_ = 0;
    std::function<bool()> cancelled_;
    std::map<QString, Entry> entries_;
    std::map<QString, QByteArray> fingerprints_;
    std::list<QString> recent_;
    QString error_;
    Failure failure_ = Failure::None;
    QSet<QString> failedImages_;
    QString imageError_;
    qint64 loadNs_ = 0;
    qint64 allocated_ = 0, peak_ = 0, transferred_ = 0, loads_ = 0, evictions_ = 0;
    Entry *ensure(const QString &name, qint64 limit = budgetBytes);
    void evict();
};
} // namespace kobalt
