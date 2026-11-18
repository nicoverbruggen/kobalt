#pragma once
#include <QPointer>
#include <QObject>
#include <cstring>

namespace nfx {
// The inline slot has no separate eligibility function. Lend it an expired
// timestamp only until it either declines shutdown or enters attemptToKill.
// Permission observers must see the original value. A real heartbeat arriving
// during the retry wins, and deleting the watchdog makes restoration a no-op.
class HeartbeatScope {
public:
    HeartbeatScope(QObject *owner, char *field, qint64 expired)
        : owner_(owner), field_(field), expired_(expired), previous_(current_) {
        std::memcpy(&saved_, field_, sizeof(saved_));
        std::memcpy(field_, &expired_, sizeof(expired_));
        current_ = this;
    }
    ~HeartbeatScope() { restore(); current_ = previous_; }
    HeartbeatScope(const HeartbeatScope &) = delete;
    HeartbeatScope &operator=(const HeartbeatScope &) = delete;
    static void beforeAttempt(QObject *owner) {
        if (current_ && current_->owner_ == owner) current_->restore();
    }
private:
    void restore() {
        if (owner_ && !std::memcmp(field_, &expired_, sizeof(expired_)))
            std::memcpy(field_, &saved_, sizeof(saved_));
        owner_.clear();
    }
    QPointer<QObject> owner_;
    char *field_;
    qint64 saved_, expired_;
    HeartbeatScope *previous_;
    static thread_local HeartbeatScope *current_;
};
}
