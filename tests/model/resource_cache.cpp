#include "support/qt_compat.h"
#include "support/strings.h"
#include "publication/resource_cache.h"
#include "publication/source_lease.h"
#include <QCoreApplication>
#include <cstdio>
#include <cstring>
#include "check.h"
using kobalt::ResourceCache;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    {
        ResourceCache bounded(
                { "text" },
                [](const QString &, const ResourceCache::Sink &sink, QString *) {
                    return sink("readable", 8);
                },
                cancelled);
        require(bounded.readBounded("text", 4).isNull()
                        && bounded.failure() == ResourceCache::Failure::Limit,
                "Inspection did not enforce its smaller budget");
        require(bounded.resumeAfterInspection() && bounded.readBounded("text", 8) == "readable",
                "Inspection budget prevented a normal engine read");
        ResourceCache failed(
                { "text" },
                [](const QString &, const ResourceCache::Sink &, QString *error) {
                    *error = "Synthetic transfer failure";
                    return false;
                },
                cancelled);
        require(failed.readBounded("text", 8).isNull() && !failed.resumeAfterInspection()
                        && failed.failure() == ResourceCache::Failure::Transfer,
                "Advisory inspection cleared a transfer failure");
    }
    {
        int failedLoads = 0, goodLoads = 0;
        ResourceCache images(
                { "bad", "good" },
                [&](const QString &name, const ResourceCache::Sink &sink, QString *error) {
                    if (name == "bad") {
                        ++failedLoads;
                        *error = "Synthetic image timeout";
                        return false;
                    }
                    ++goodLoads;
                    return sink("ok", 2);
                },
                cancelled);
        images.imageReady("bad");
        images.imageReady("good");
        require(!images.loadNextImage(), "Failed image was reported as successfully loaded");
        require(images.error().isEmpty() && images.statistics()["failed_images"].toInt() == 1
                        && images.statistics()["image_error"].toString()
                                == "Synthetic image timeout",
                "Optional image failure stopped the session or lost its warning");
        require(images.loadNextImage() && goodLoads == 1 && images.readBounded("good", 2) == "ok",
                "One failed image prevented later images from loading");
        require(images.imageReady("bad") && images.size("bad") == -1 && failedLoads == 1,
                "Failed image was repeatedly queued or retried while painting");
        images.releaseBytes();
        require(images.statistics()["failed_images"].toInt() == 0
                        && images.statistics()["image_error"].toString().isEmpty(),
                "Released source retained image failures");
        ResourceCache renewed(
                { "bad", "good" },
                [](const QString &, const ResourceCache::Sink &sink, QString *) {
                    return sink("ok", 2);
                },
                cancelled);
        require(images.rebind(renewed) && !images.imageReady("bad") && images.loadNextImage()
                        && images.readBounded("bad", 2) == "ok",
                "Renewed source did not retry its failed image");
    }
    {
        auto revoked = std::make_shared<kobalt::SourceLease>();
        int goodLoads = 0;
        ResourceCache images(
                { "bad", "good" },
                ResourceCache::Access{
                        [&](const QString &name, const ResourceCache::Sink &sink, QString *) {
                            if (name == "bad") {
                                revoked->revoke();
                                return false;
                            }
                            ++goodLoads;
                            return sink("ok", 2);
                        },
                        [revoked] { return revoked->revoked(); },
                        {},
                        {} });
        images.imageReady("bad");
        images.imageReady("good");
        require(!images.loadNextImage() && images.failure() == ResourceCache::Failure::Cancelled
                        && !images.error().isEmpty() && !images.loadNextImage() && goodLoads == 0,
                "Revoked access was treated as an optional image failure");
    }
    {
        bool changedImage = false;
        ResourceCache image(
                { "image" },
                [&](const QString &, const ResourceCache::Sink &sink, QString *) {
                    return sink(changedImage ? "new" : "old", 3);
                },
                cancelled);
        require(image.readBounded("image", 3) == "old", "Initial image failed");
        image.releaseBytes();
        changedImage = true;
        image.imageReady("image");
        require(!image.loadNextImage() && image.failure() == ResourceCache::Failure::Changed
                        && !image.error().isEmpty()
                        && image.statistics()["failed_images"].toInt() == 0,
                "Changed bytes were treated as an optional image failure");
    }
    int calls = 0;
    bool changed = false;
    auto load = [&](const QString &name, const ResourceCache::Sink &sink, QString *) {
        ++calls;
        const qint64 size = name == "empty" ? 0 : 20 * 1024 * 1024;
        std::array<char, ResourceCache::blockBytes> block;
        for (qint64 at = 0; at < size; at += block.size()) {
            block.fill(char(at / block.size() + (name == "a" ? 1 : 7) + (changed ? 1 : 0)));
            if (!sink(block.data(), block.size()))
                return false;
        }
        return true;
    };
    ResourceCache cache({ "a", "b", "empty" }, load, cancelled, [] {
        return kobalt::qt::object({ { "requests", 17 }, { "request_first_byte_ms", 12.5 } });
    });
    require(cache.size("absent") == -1 && calls == 0 && cache.error().isEmpty(),
            "Missing optional resource triggered a request");
    require(cache.size("a") == 20 * 1024 * 1024 && calls == 1, "Resource did not load");
    char bytes[4];
    require(cache.read("a", ResourceCache::blockBytes - 2, bytes, 4) == 4 && bytes[0] == 1
                    && bytes[1] == 1 && bytes[2] == 2 && bytes[3] == 2,
            "Read crossing a block lost data");
    require(cache.size("b") > 0 && cache.size("a") > 0 && calls == 3,
            "Evicted resource did not reload");
    require(cache.read("a", 20 * 1024 * 1024 - 2, bytes, 4) == 2, "Read crossed resource end");
    require(cache.read("a", -1, bytes, 1) == -1 && cache.read("a", 0, nullptr, 1) == -1,
            "Invalid read accepted");
    require(cache.size("empty") == 0 && cache.read("empty", 0, nullptr, 0) == 0,
            "Empty resource did not work");
    auto stats = cache.statistics();
    require(stats["load_ms"].toDouble() >= 0 && stats["requests"].toInt() == 17
                    && stats["request_first_byte_ms"].toDouble() == 12.5,
            "Resource timings were not included");
    require(stats["peak_bytes"].toDouble() <= ResourceCache::budgetBytes
                    && stats["evictions"].toDouble() >= 2,
            "Cache exceeded its allocated block budget");
    require(cache.size("b") > 0, "Cannot evict before changed-resource check");
    const auto revision = cache.imageRevision();
    require(!cache.imageReady("a"), "Uncached image was unexpectedly ready");
    cache.releaseBytes();
    require(cache.statistics()["allocated_bytes"].toDouble() == 0
                    && cache.statistics()["pending_images"].toInt() == 0
                    && cache.imageRevision() != revision && !cache.loadNextImage(),
            "Idle cache kept encoded bytes or pending image work");
    changed = true;
    require(cache.size("a") == -1
                    && cache.error()
                            == kobalt::strings::get("error.a_book_resource_changed_during_reading"),
            "Changed resource was mixed with existing document");
    require(!cache.resumeAfterInspection(), "Advisory inspection cleared changed resource bytes");
    ResourceCache oversized(
            { "large" },
            [](const QString &, const ResourceCache::Sink &sink, QString *) {
                std::array<char, ResourceCache::blockBytes> block{};
                for (qint64 at = 0; at <= ResourceCache::budgetBytes; at += block.size())
                    if (!sink(block.data(), block.size()))
                        return false;
                return true;
            },
            cancelled);
    require(oversized.size("large") == -1
                    && oversized.error()
                            == kobalt::strings::get(
                                    "error.a_book_resource_exceeds_the_32_mib_limit"),
            "Oversized resource accepted");
    require(oversized.statistics()["peak_bytes"].toDouble() == ResourceCache::budgetBytes
                    && oversized.statistics()["allocated_bytes"].toDouble() == 0,
            "Failed transfer retained blocks or exceeded budget");
    ResourceCache largeImage(
            { "large", "small" },
            [](const QString &name, const ResourceCache::Sink &sink, QString *) {
                if (name == "small")
                    return sink("ok", 2);
                std::array<char, ResourceCache::blockBytes> block{};
                for (qint64 at = 0; at <= ResourceCache::budgetBytes; at += block.size())
                    if (!sink(block.data(), block.size()))
                        return false;
                return true;
            },
            cancelled);
    largeImage.imageReady("large");
    largeImage.imageReady("small");
    require(!largeImage.loadNextImage() && largeImage.error().isEmpty()
                    && largeImage.statistics()["failed_images"].toInt() == 1
                    && largeImage.statistics()["allocated_bytes"].toDouble() == 0
                    && largeImage.loadNextImage() && largeImage.readBounded("small", 2) == "ok",
            "Oversized optional image stopped later images or retained partial content");
    ResourceCache interrupted(
            { "a" },
            [&](const QString &, const ResourceCache::Sink &sink, QString *) {
                char byte = 'a';
                if (!sink(&byte, 1))
                    return false;
                cancelled->store(true);
                return sink(&byte, 1);
            },
            cancelled);
    require(interrupted.size("a") == -1
                    && interrupted.statistics()["allocated_bytes"].toDouble() == 0,
            "Cancelled transfer retained partial content");
    {
        auto oldLease = std::make_shared<kobalt::SourceLease>();
        auto newLease = std::make_shared<kobalt::SourceLease>();
        int oldLoads = 0, newLoads = 0, probes = 0;
        ResourceCache old({ "text", "image" },
                          ResourceCache::Access{
                                  [&](const QString &, const ResourceCache::Sink &sink, QString *) {
                                      ++oldLoads;
                                      return sink("abc", 3);
                                  },
                                  [oldLease] { return oldLease->revoked(); },
                                  [] { return kobalt::qt::object({ { "generation", 1 } }); },
                                  [](const QString &) { return QSize(10, 20); } });
        require(old.readBounded("text", 3) == "abc"
                        && old.deferredImageSize("image") == QSize(10, 20),
                "Initial source failed");
        old.imageReady("image");
        oldLease->revoke();
        require(old.readBounded("text", 3).isEmpty() && !old.deferredImageSize("image").isValid(),
                "Revoked source returned cached content");
        ResourceCache fresh(
                { "text", "image" },
                ResourceCache::Access{
                        [&](const QString &, const ResourceCache::Sink &sink, QString *) {
                            ++newLoads;
                            return sink("abc", 3);
                        },
                        [newLease] { return newLease->revoked(); },
                        [] { return kobalt::qt::object({ { "generation", 2 } }); },
                        [&](const QString &) {
                            ++probes;
                            return QSize(30, 40);
                        } });
        require(old.rebind(fresh), "Fresh source did not replace revoked access");
        require(old.statistics()["allocated_bytes"].toDouble() == 0
                        && old.statistics()["pending_images"].toInt() == 0
                        && old.statistics()["generation"].toInt() == 2,
                "Rebind retained bytes, pending work or old statistics");
        require(old.readBounded("text", 3) == "abc" && oldLoads == 1 && newLoads == 1
                        && old.deferredImageSize("image") == QSize(30, 40) && probes == 1,
                "Rebind used old loader or image metadata");
        ResourceCache changedSource(
                { "text", "image" },
                ResourceCache::Access{ [](const QString &, const ResourceCache::Sink &sink,
                                          QString *) { return sink("xyz", 3); },
                                       {},
                                       {},
                                       {} });
        require(old.rebind(changedSource) && old.readBounded("text", 3).isEmpty()
                        && old.error()
                                == kobalt::strings::get(
                                        "error.a_book_resource_changed_during_reading"),
                "Rebind forgot content integrity checks");
        auto lateLease = std::make_shared<kobalt::SourceLease>();
        ResourceCache late(
                { "text" },
                ResourceCache::Access{
                        [lateLease](const QString &, const ResourceCache::Sink &sink, QString *) {
                            const bool delivered = sink("abc", 3);
                            lateLease->revoke();
                            return delivered;
                        },
                        [lateLease] { return lateLease->revoked(); },
                        {},
                        {} });
        require(late.readBounded("text", 3).isEmpty()
                        && late.statistics()["allocated_bytes"].toDouble() == 0,
                "Completion after revocation committed resource bytes");
    }
    std::puts(
            "PASS: bounded blocks, eviction, reload, boundary reads, changed resources, oversize and cancellation");
}
