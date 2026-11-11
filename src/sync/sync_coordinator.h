#pragma once
#include "kosync.h"
#include "platform/api/sync_network.h"
#include <memory>

namespace kobalt::kosync {
// One opened book owns one coordinator. The client must outlive it. Account
// editing stays with the owner; all request and connection lifetimes live here.
class SyncCoordinator final : public QObject
{
public:
    enum class Readiness { Ready, Waiting, Hidden };

    struct Context
    {
        QByteArray document;
        Progress local;
        // Sent only when the account has Send document metadata on.
        Metadata metadata;
        Readiness readiness = Readiness::Ready;
        bool finalWriteAllowed = true;
        bool queueSyncTime = false;
        bool suspended = false;
        // Diagnostic evidence only; no control flow depends on its fields.
        QJsonObject waitEvidence;
        // Non-null only at an owner-declared boundary where input cannot arrive.
        const char *releaseBlockingReason = nullptr;
    };
    enum class Connection { Existing, Background, Interactive, Unavailable };
    enum class EventKind { Status, InteractiveReady, Cancelled, FinalStarted, Completed };

    struct Event
    {
        EventKind kind;
        QString message;
        bool finalUpload = false, interactiveConnectionFailed = false;
    };

    using Done = std::function<void()>;
    // A synchronous snapshot. It must not reenter or destroy the coordinator.
    using ContextSource = std::function<Context()>;
    using Observer = std::function<void(const Event &)>;

    SyncCoordinator(Client &client, SyncNetworkFactory network, ContextSource context,
                    Observer observer, bool prepareNetwork, QObject *parent = nullptr);
    ~SyncCoordinator() override;

    bool busy() const;
    bool connected() const;
    bool finalPending() const;

    int backgroundFailures() const { return backgroundFailures_; }

    void resetBackgroundFailures() { backgroundFailures_ = 0; }

    // False means another attempt still owns the request or connection.
    bool start(Connection connection, bool retryRead);
    bool login(const QString &server, const QString &username, const QString &password,
               Client::Done done);
    // The owner decides eligibility. Only an existing connection may be used.
    // An older HTTP request finishes before the final read/write exchange starts.
    // The same deadline covers that wait and the exchange. Cancel drops done.
    void flush(bool enabled, Done done);
    void cancel();
    // Resume a response waiting for visible, settled reading state. Hidden
    // owners call this on return; there is no readiness polling while hidden.
    void resume();

    bool pendingProgress() const { return progress_.pending(); }

    bool needsSync(const QString &position) const { return progress_.needsSync(position); }

    Progress candidate() const { return progress_.candidate(); }

    void keepLocal() { progress_.keepLocal(); }

    void acceptRemote(const QString &position) { progress_.acceptRemote(position); }

    void resetProgress() { progress_ = {}; }

private:
    static constexpr int finishBudget = 2000;
    Client &client_;
    SyncNetworkFactory networkFactory_;
    std::unique_ptr<SyncNetwork> network_;
    ContextSource context_;
    Observer observer_;
    QTimer deadline_, readerRetry_;
    QElapsedTimer finishingElapsed_;
    Reconciler progress_;
    Done readerReady_, finished_;
    quint64 epoch_ = 0;
    bool connecting_ = false, authenticating_ = false, finishing_ = false;
    bool flushWaiting_ = false, releaseWifi_ = false, retryRead_ = false;
    int backgroundFailures_ = 0;

    Context context() const;
    void notify(EventKind kind, const QString &message = {}, bool finalUpload = false,
                bool interactiveConnectionFailed = false);
    void prepareNetwork(bool interactive, Done ready);
    void exchange(bool finalUpload);
    void whenReady(Done ready);
    void complete(const QString &message, bool interactiveConnectionFailed = false);
    void cancel(bool keepConnection);
    void releaseConnection();
};
} // namespace kobalt::kosync
