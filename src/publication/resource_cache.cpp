#include "support/qt_compat.h"
#include "support/strings.h"
#include "resource_cache.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <cstring>

namespace kobalt {
ResourceCache::ResourceCache(QStringList names, Loader loader,
                             std::shared_ptr<std::atomic_bool> cancelled,
                             Statistics transferStatistics, ImageProbe imageProbe)
    : ResourceCache(std::move(names),
                    Access{ std::move(loader),
                            [cancelled] { return cancelled && cancelled->load(); },
                            std::move(transferStatistics), std::move(imageProbe) })
{
}

ResourceCache::ResourceCache(QStringList names, Access access)
    : names_(std::move(names)),
      loader_(std::move(access.loader)),
      transferStatistics_(std::move(access.statistics)),
      imageProbe_(std::move(access.imageProbe)),
      cancelled_(std::move(access.cancelled))
{
}

bool ResourceCache::rebind(const ResourceCache &fresh)
{
    if (this == &fresh || names_ != fresh.names_ || !fresh.error_.isEmpty()
        || (fresh.cancelled_ && fresh.cancelled_()))
        return false;
    // A fresh readability probe must agree with any previously read member.
    for (const auto &entry : fresh.fingerprints_) {
        const auto previous = fingerprints_.find(entry.first);
        if (previous != fingerprints_.end() && previous->second != entry.second)
            return false;
    }
    releaseBytes();
    imageSizes_.clear();
    loader_ = fresh.loader_;
    cancelled_ = fresh.cancelled_;
    transferStatistics_ = fresh.transferStatistics_;
    imageProbe_ = fresh.imageProbe_;
    error_.clear();
    failure_ = Failure::None;
    loadNs_ = 0;
    peak_ = transferred_ = loads_ = evictions_ = 0;
    fingerprints_.insert(fresh.fingerprints_.begin(), fresh.fingerprints_.end());
    return true;
}

QSize ResourceCache::deferredImageSize(const QString &name)
{
    if ((cancelled_ && cancelled_()) || !imageProbe_ || !contains(name))
        return {};
    auto found = imageSizes_.constFind(name);
    if (found != imageSizes_.constEnd())
        return found.value();
    const auto size = imageProbe_(name);
    if (cancelled_ && cancelled_())
        return {};
    imageSizes_.insert(name, size);
    return size;
}

bool ResourceCache::imageReady(const QString &name)
{
    if (cancelled_ && cancelled_())
        return false;
    if (readyImages_.contains(name) || failedImages_.contains(name)
        || entries_.find(name) != entries_.end())
        return true;
    if (!pendingImages_.contains(name))
        pendingImages_.append(name);
    return false;
}

bool ResourceCache::loadNextImage()
{
    if (pendingImages_.isEmpty() || !error_.isEmpty() || (cancelled_ && cancelled_()))
        return false;
    const auto name = pendingImages_.takeFirst();
    const bool loaded = ensure(name) != nullptr;
    if (loaded)
        readyImages_.insert(name);
    else if (failure_ == Failure::Transfer || failure_ == Failure::Limit) {
        // A missing optional image must not block unrelated content. Keep its
        // warning and stop repeated paint requests until the source is renewed.
        failedImages_.insert(name);
        imageError_ = error_;
        error_.clear();
        failure_ = Failure::None;
    }
    ++imageRevision_;
    return loaded;
}

void ResourceCache::releaseBytes()
{
    entries_.clear();
    recent_.clear();
    allocated_ = 0;
    readyImages_.clear();
    pendingImages_.clear();
    failedImages_.clear();
    imageError_.clear();
    ++imageRevision_;
}

void ResourceCache::evict()
{
    const auto name = recent_.back();
    recent_.pop_back();
    auto entry = entries_.find(name);
    allocated_ -= qint64(entry->second.blocks.size()) * blockBytes;
    entries_.erase(entry);
    ++evictions_;
}

bool ResourceCache::resumeAfterInspection()
{
    if (failure_ == Failure::Limit) {
        error_.clear();
        failure_ = Failure::None;
    }
    return error_.isEmpty();
}

ResourceCache::Entry *ResourceCache::ensure(const QString &name, qint64 limit)
{
    if (cancelled_ && cancelled_()) {
        error_ = strings::get("error.reading_session_closed");
        failure_ = Failure::Cancelled;
        return nullptr;
    }
    if (!error_.isEmpty())
        return nullptr;
    if (failedImages_.contains(name))
        return nullptr;
    limit = qBound(qint64(0), limit, budgetBytes);
    auto found = entries_.find(name);
    if (found != entries_.end()) {
        if (found->second.size > limit) {
            error_ = strings::get("error.resource_exceeds_inspection_limit");
            failure_ = Failure::Limit;
            return nullptr;
        }
        recent_.remove(name);
        recent_.push_front(name);
        return &found->second;
    }
    if (!contains(name))
        return nullptr;
    Entry pending;
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    QString failure;
    Failure cause = Failure::None;
    ++loads_;
    QElapsedTimer transfer;
    transfer.start();
    const bool loaded = loader_(
            name,
            [&](const char *data, qint64 count) {
                if (!failure.isEmpty())
                    return false;
                if (count < 0 || count > limit - pending.size) {
                    failure = limit == budgetBytes
                            ? strings::get("error.a_book_resource_exceeds_the_32_mib_limit")
                            : strings::get("error.resource_exceeds_inspection_limit");
                    cause = Failure::Limit;
                    return false;
                }
                if (cancelled_ && cancelled_()) {
                    failure = strings::get("error.reading_session_closed");
                    cause = Failure::Cancelled;
                    return false;
                }
                fingerprint.addData(QByteArray::fromRawData(data, count));
                transferred_ += count;
                while (count) {
                    const auto offset = pending.size % blockBytes;
                    if (!offset) {
                        while (allocated_ + blockBytes > budgetBytes && !recent_.empty())
                            evict();
                        pending.blocks.push_back(std::make_unique<Block>());
                        allocated_ += blockBytes;
                        peak_ = qMax(peak_, allocated_);
                    }
                    const auto length = qMin(count, blockBytes - offset);
                    std::memcpy(pending.blocks.back()->data() + offset, data, size_t(length));
                    data += length;
                    count -= length;
                    pending.size += length;
                }
                return true;
            },
            &error_);
    loadNs_ += transfer.nsecsElapsed();
    if (cancelled_ && cancelled_()) {
        failure = strings::get("error.reading_session_closed");
        cause = Failure::Cancelled;
    }
    const auto digest = fingerprint.result();
    const auto previous = fingerprints_.find(name);
    if (loaded && failure.isEmpty() && previous != fingerprints_.end()
        && previous->second != digest) {
        failure = strings::get("error.a_book_resource_changed_during_reading");
        cause = Failure::Changed;
    }
    if (!loaded || !failure.isEmpty() || !error_.isEmpty()) {
        allocated_ -= qint64(pending.blocks.size()) * blockBytes;
        if (!failure.isEmpty())
            error_ = failure;
        if (error_.isEmpty())
            error_ = strings::get("error.could_not_load_a_book_resource");
        failure_ = cause == Failure::None ? Failure::Transfer : cause;
        return nullptr;
    }
    fingerprints_[name] = digest;
    recent_.push_front(name);
    return &entries_.emplace(name, std::move(pending)).first->second;
}

QByteArray ResourceCache::readBounded(const QString &name, qint64 limit)
{
    auto *entry = ensure(name, limit);
    if (!entry)
        return {};
    QByteArray bytes(entry->size, Qt::Uninitialized);
    if (read(name, 0, bytes.data(), bytes.size()) != bytes.size())
        return {};
    return bytes;
}

qint64 ResourceCache::size(const QString &name)
{
    auto *entry = ensure(name);
    return entry ? entry->size : -1;
}

qint64 ResourceCache::read(const QString &name, qint64 offset, void *buffer, qint64 count)
{
    if (offset < 0 || count < 0 || (count && !buffer))
        return -1;
    auto *entry = ensure(name);
    if (!entry || offset > entry->size)
        return -1;
    const auto length = qMin(count, entry->size - offset);
    auto *output = static_cast<char *>(buffer);
    for (qint64 copied = 0; copied < length;) {
        if (cancelled_ && cancelled_())
            return -1;
        const auto at = offset + copied;
        const auto take = qMin(length - copied, blockBytes - at % blockBytes);
        std::memcpy(output + copied,
                    entry->blocks[size_t(at / blockBytes)]->data() + at % blockBytes, size_t(take));
        copied += take;
    }
    return cancelled_ && cancelled_() ? -1 : length;
}

QJsonObject ResourceCache::statistics() const
{
    QJsonObject result = kobalt::qt::object({ { "budget_bytes", budgetBytes },
                                              { "allocated_bytes", allocated_ },
                                              { "peak_bytes", peak_ },
                                              { "transferred_bytes", transferred_ },
                                              { "loads", loads_ },
                                              { "evictions", evictions_ },
                                              { "pending_images", pendingImages_.size() },
                                              { "ready_images", readyImages_.size() },
                                              { "failed_images", failedImages_.size() },
                                              { "image_error", imageError_ },
                                              { "resource_count", names_.size() },
                                              { "loaded_resources", qint64(fingerprints_.size()) },
                                              { "load_ms", double(loadNs_) / 1000000.0 } });
    if (transferStatistics_) {
        const auto transfer = transferStatistics_();
        for (auto at = transfer.begin(); at != transfer.end(); ++at)
            result.insert(at.key(), at.value());
    }
    return result;
}
} // namespace kobalt
