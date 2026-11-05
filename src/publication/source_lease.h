#pragma once
#include <QtGlobal>
#include <atomic>

namespace kobalt {
// One opening's access generation. Revocation is permanent. Hosts keep their
// endpoint owner alive until engine and resource work has drained; this token
// never owns a GUI object or changes the thread on which it is destroyed.
class SourceLease
{
public:
    SourceLease() : generation_(next.fetch_add(1)) { }

    quint64 generation() const { return generation_; }
    enum class End { Active, Cancelled, Revoked };

    bool revoked() const { return end_.load() != End::Active; }

    End end() const { return end_.load(); }

    void cancel() { endAccess(End::Cancelled); }

    void revoke() { endAccess(End::Revoked); }

private:
    void endAccess(End reason)
    {
        // Record the first cause. Cleanup must not relabel an access revocation
        // as an ordinary cancellation when it disposes the same opening.
        auto expected = End::Active;
        end_.compare_exchange_strong(expected, reason);
    }

    inline static std::atomic<quint64> next{ 1 };
    const quint64 generation_;
    std::atomic<End> end_{ End::Active };
};
} // namespace kobalt
