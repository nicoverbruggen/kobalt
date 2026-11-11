#include "sync_coordinator.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include <utility>

namespace kobalt::kosync {
namespace {
QString unavailableConnection(const char *stage)
{
    const auto message = strings::get("kosync.no_connection");
    diagnostic("kosync-network-unavailable",
               qt::object({ { "stage", stage },
                            { "code", "kosync.no_connection" },
                            { "guidance", message } }));
    return message;
}
} // namespace

SyncCoordinator::SyncCoordinator(Client &client, SyncNetworkFactory network, ContextSource context,
                                 Observer observer, bool prepareNetwork, QObject *parent)
    : QObject(parent),
      client_(client),
      networkFactory_(std::move(network)),
      context_(std::move(context)),
      observer_(std::move(observer))
{
    if (prepareNetwork && networkFactory_)
        network_ = networkFactory_();
    finishingElapsed_.invalidate();
    deadline_.setParent(this);
    readerRetry_.setParent(this);
    deadline_.setObjectName("rrKosyncDeadline");
    deadline_.setSingleShot(true);
    readerRetry_.setInterval(100);
    connect(&readerRetry_, &QTimer::timeout, this, [this] {
        if (readerReady_)
            whenReady(std::move(readerReady_));
    });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        const auto code = connecting_ ? "kosync.connection_timeout"
                : readerReady_        ? "kosync.reader_timeout"
                                      : "kosync.timeout_error";
        diagnostic("kosync-deadline",
                   qt::object({ { "code", code },
                                { "guidance", strings::get(code) },
                                { "connecting", connecting_ },
                                { "request_busy", client_.busy() },
                                { "suspended", this->context().suspended },
                                { "reader_waiting", bool(readerReady_) } }));
        ++epoch_;
        if (network_)
            network_->cancel();
        if (connecting_)
            ++backgroundFailures_;
        connecting_ = authenticating_ = false;
        client_.cancel();
        flushWaiting_ = false;
        complete(strings::get(code));
    });
}

SyncCoordinator::~SyncCoordinator()
{
    observer_ = {};
    context_ = {};
    cancel();
}

SyncCoordinator::Context SyncCoordinator::context() const
{
    return context_ ? context_() : Context();
}

void SyncCoordinator::notify(EventKind kind, const QString &message, bool finalUpload,
                             bool interactiveConnectionFailed)
{
    if (observer_) {
        const auto observer = observer_;
        Event event;
        event.kind = kind;
        event.message = message;
        event.finalUpload = finalUpload;
        event.interactiveConnectionFailed = interactiveConnectionFailed;
        observer(event);
    }
}

bool SyncCoordinator::busy() const
{
    return client_.busy() || connecting_ || authenticating_ || bool(readerReady_);
}

bool SyncCoordinator::connected() const
{
    return !network_ || network_->connected();
}

bool SyncCoordinator::finalPending() const
{
    return finishing_ && finishingElapsed_.elapsed() < finishBudget;
}

bool SyncCoordinator::start(Connection connection, bool retryRead)
{
    if (busy() || finishing_)
        return false;
    ++epoch_;
    retryRead_ = retryRead;
    if (connection != Connection::Interactive)
        deadline_.start(10000);
    if (connection == Connection::Unavailable) {
        complete(unavailableConnection("periodic"));
        return true;
    }
    if (connection == Connection::Existing) {
        backgroundFailures_ = 0;
        exchange(false);
        return true;
    }
    const bool interactive = connection == Connection::Interactive;
    if (interactive)
        backgroundFailures_ = 0;
    prepareNetwork(interactive, [this, interactive] {
        if (interactive) {
            deadline_.start(10000);
            QPointer<SyncCoordinator> guard(this);
            const auto epoch = epoch_;
            notify(EventKind::InteractiveReady);
            if (!guard || epoch != epoch_)
                return;
        }
        exchange(false);
    });
    return true;
}

bool SyncCoordinator::login(const QString &server, const QString &username, const QString &password,
                            Client::Done done)
{
    if (busy() || finishing_)
        return false;
    ++epoch_;
    authenticating_ = true;
    prepareNetwork(true, [this, server, username, password, done] {
        deadline_.start(10000);
        const auto epoch = epoch_;
        QPointer<SyncCoordinator> guard(this);
        client_.login(server, username, password, [this, guard, epoch, done](const QString &error) {
            if (!guard || epoch != epoch_)
                return;
            deadline_.stop();
            authenticating_ = false;
            if (error.isEmpty()) {
                ++epoch_;
                progress_ = {};
            }
            if (done)
                done(error);
        });
        if (guard && epoch == epoch_ && client_.busy())
            notify(EventKind::Status, strings::get("kosync.signing_in"));
    });
    return true;
}

void SyncCoordinator::prepareNetwork(bool interactive, Done ready)
{
    if (!network_ && networkFactory_)
        network_ = networkFactory_();
    if (!network_) {
        ready();
        return;
    }
    const auto epoch = epoch_;
    QPointer<SyncCoordinator> guard(this);
    connecting_ = true;
    if (!interactive)
        releaseWifi_ = true;
    notify(EventKind::Status, strings::get("kosync.connecting"));
    if (!guard || epoch != epoch_)
        return;
    network_->prepare(interactive, [this, guard, epoch, ready, interactive](bool connected) {
        if (!guard || epoch != epoch_ || !connecting_)
            return;
        connecting_ = false;
        if (!interactive)
            backgroundFailures_ = connected ? 0 : backgroundFailures_ + 1;
        if (!connected) {
            complete(unavailableConnection("preparation"), interactive);
            return;
        }
        ready();
    });
}

void SyncCoordinator::cancel()
{
    cancel(false);
}

void SyncCoordinator::cancel(bool keepConnection)
{
    ++epoch_;
    if (network_)
        network_->cancel();
    if (!keepConnection)
        releaseConnection();
    connecting_ = authenticating_ = false;
    client_.cancel();
    deadline_.stop();
    readerRetry_.stop();
    readerReady_ = {};
    finished_ = {};
    finishing_ = flushWaiting_ = false;
    notify(EventKind::Cancelled);
}

void SyncCoordinator::releaseConnection()
{
    if (!std::exchange(releaseWifi_, false) || !network_)
        return;
    const auto reason = context().releaseBlockingReason;
    if (!reason) {
        network_->release();
        return;
    }
    mainthread::AllowBlocking allow(reason);
    network_->release();
}

void SyncCoordinator::complete(const QString &message, bool interactiveConnectionFailed)
{
    readerRetry_.stop();
    readerReady_ = {};
    if (context().queueSyncTime)
        client_.queueSyncTime();
    if (flushWaiting_) {
        flushWaiting_ = false;
        exchange(true);
        return;
    }
    ++epoch_;
    releaseConnection();
    if (network_)
        network_->cancel();
    connecting_ = authenticating_ = false;
    const bool finishing = finishing_;
    deadline_.stop();
    finishing_ = false;
    auto finished = std::move(finished_);
    finished_ = {};
    QPointer<SyncCoordinator> guard(this);
    const auto epoch = epoch_;
    notify(EventKind::Completed, message, finishing, interactiveConnectionFailed);
    if (guard && epoch == epoch_ && finished)
        finished();
}

void SyncCoordinator::flush(bool enabled, Done done)
{
    QPointer<SyncCoordinator> guard(this);
    if (connecting_ || authenticating_ || readerReady_) {
        cancel(true);
        if (!guard)
            return;
    }
    if (!enabled || progress_.pending()) {
        cancel();
        if (guard && done)
            done();
        return;
    }
    if (!connected()) {
        cancel();
        if (!guard)
            return;
        notify(EventKind::Status, unavailableConnection("final"));
        if (guard && done)
            done();
        return;
    }
    const auto epoch = epoch_;
    notify(EventKind::FinalStarted);
    if (!guard || epoch != epoch_)
        return;
    finishingElapsed_.start();
    finishing_ = true;
    finished_ = std::move(done);
    deadline_.start(finishBudget);
    // Aborting an older PUT cannot prove that the server stopped its write.
    // Let it finish before the final read/write exchange, under one budget.
    flushWaiting_ = client_.busy();
    if (!flushWaiting_)
        exchange(true);
}

void SyncCoordinator::resume()
{
    if (readerReady_ && !readerRetry_.isActive())
        whenReady(std::move(readerReady_));
}

void SyncCoordinator::whenReady(Done ready)
{
    const auto current = context();
    if (current.readiness != Readiness::Ready) {
        readerReady_ = std::move(ready);
        if (current.readiness == Readiness::Hidden)
            readerRetry_.stop();
        if (!readerRetry_.isActive()) {
            diagnostic("kosync-reader-wait", current.waitEvidence);
            if (current.readiness != Readiness::Hidden)
                readerRetry_.start();
            notify(EventKind::Status, strings::get("kosync.syncing"));
        }
        return;
    }
    readerRetry_.stop();
    readerReady_ = {};
    ready();
}

void SyncCoordinator::exchange(bool finalUpload)
{
    const auto initial = context();
    const auto document = initial.document;
    if (document.isEmpty()) {
        complete(strings::get(client_.filenameMatching() ? "kosync.filename_unavailable"
                                                         : "kosync.unavailable"));
        return;
    }
    const auto epoch = epoch_;
    const auto snapshot = initial.local;
    QPointer<SyncCoordinator> guard(this);
    client_.pull(
            document,
            [this, guard, document, epoch, finalUpload, snapshot](Progress remote,
                                                                  const QString &error) {
                if (!guard || epoch != epoch_)
                    return;
                if (!error.isEmpty()) {
                    complete(error);
                    return;
                }
                if (finalUpload && !context().finalWriteAllowed) {
                    complete(strings::get("kosync.retry_later"));
                    return;
                }
                auto apply = [this, guard, document, epoch, finalUpload, snapshot, remote] {
                    if (!guard || epoch != epoch_)
                        return;
                    const auto local = finalUpload ? snapshot : context().local;
                    const auto action =
                            progress_.observe(remote, local.position, client_.deviceId());
                    if (action == Reconciler::Action::Choose) {
                        complete(strings::get("kosync.pending"));
                        return;
                    }
                    const bool renamed = remote.deviceId == client_.deviceId()
                            && remote.device != client_.deviceName() && !local.position.isEmpty();
                    if (action == Reconciler::Action::Current && !renamed) {
                        complete(strings::get(client_.recordSyncSuccess()
                                                      ? "kosync.up_to_date"
                                                      : "kosync.storage_error"));
                        return;
                    }
                    client_.push(
                            document, local, [this, guard, epoch, local](const QString &error) {
                                if (!guard || epoch != epoch_)
                                    return;
                                if (error.isEmpty())
                                    progress_.uploaded(local.position);
                                complete(!error.isEmpty()
                                                 ? error
                                                 : strings::get(client_.recordSyncSuccess()
                                                                        ? "kosync.up_to_date"
                                                                        : "kosync.storage_error"));
                            });
                    if (guard && epoch == epoch_ && client_.busy())
                        notify(EventKind::Status, strings::get("kosync.syncing"));
                };
                if (finalUpload)
                    apply();
                else
                    whenReady(std::move(apply));
            },
            !finalUpload && retryRead_);
    if (guard && epoch == epoch_ && client_.busy())
        notify(EventKind::Status, strings::get("kosync.syncing"));
}
} // namespace kobalt::kosync
