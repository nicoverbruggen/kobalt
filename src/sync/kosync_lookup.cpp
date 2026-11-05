#include "kosync_lookup.h"
#include <QCoreApplication>
#include <QRunnable>
#include <QThreadPool>
#include <atomic>
#include <utility>

namespace kobalt::kosync {
namespace {
QThreadPool &lookupPool() {
    struct Pool {
        QThreadPool threads;
        Pool() {
            // Cancellation cannot interrupt libc's resolver. Bound outstanding
            // work across retries and readers, and never queue more lookups.
            threads.setMaxThreadCount(2);
            threads.setExpiryTimeout(0);
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &threads, [this] {
                threads.waitForDone();
            }, Qt::DirectConnection);
            QObject::connect(QCoreApplication::instance(), &QObject::destroyed, &threads, [this] {
                // Reader destruction never waits. Application destruction must
                // also join workers when the main event loop was not run.
                // Qt5's own lookup manager uses this signal; its cache lives
                // until the Q_GLOBAL_STATIC manager is destroyed at exit.
                threads.waitForDone();
            }, Qt::DirectConnection);
        }
    };
    static Pool pool;
    return pool.threads;
}
QHostInfo failedLookup() {
    QHostInfo result;
    result.setError(QHostInfo::UnknownError);
    return result;
}
}
struct HostLookup::State {
    std::atomic<bool> cancelled{false}, finished{false};
    QHostInfo result;
};
struct HostLookup::Job final : QRunnable {
    std::shared_ptr<State> state;
    QString host;
    Resolver resolve;
    void run() override {
        if (!state->cancelled.load()) {
            state->result = resolve(host);
            state->finished.store(true, std::memory_order_release);
        }
    }
};
HostLookup::HostLookup(Resolver resolver)
    : resolver_(resolver ? std::move(resolver) : Resolver(QHostInfo::fromName)) {
    poll_.setInterval(25);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!state_) return;
        const bool finished = state_->finished.load(std::memory_order_acquire);
        if (!finished && elapsed_.elapsed() < timeout_) return;
        const auto result = finished ? state_->result : failedLookup();
        auto done = std::move(done_);
        cancel();
        done(result);
    });
}
HostLookup::~HostLookup() { cancel(); }
void HostLookup::start(const QString &host, int timeout, Done done) {
    cancel();
    state_ = std::make_shared<State>();
    done_ = std::move(done);
    timeout_ = timeout;
    elapsed_.start();
    auto *job = new Job;
    job->state = state_;
    job->host = host;
    job->resolve = resolver_;
    if (!lookupPool().tryStart(job)) {
        delete job;
        state_->result = failedLookup();
        state_->finished.store(true, std::memory_order_release);
    }
    // Even saturation and cached results complete asynchronously.
    poll_.start();
}
void HostLookup::cancel() {
    poll_.stop();
    done_ = {};
    if (state_) state_->cancelled.store(true);
    state_.reset();
}
}
