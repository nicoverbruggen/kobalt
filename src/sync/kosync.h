#pragma once
#include "kosync_lookup.h"
#include "kosync_schedule.h"
#include <QJsonObject>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>
#include <functional>

class QIODevice;
class QNetworkReply;

namespace kobalt::kosync {
// KOReader hashes 1 KiB samples at exponentially spaced offsets, not the whole archive.
QByteArray documentDigest(QIODevice &file);
QByteArray filenameDigest(const QString &path);
QUrl serverUrl(const QString &text);
bool validLogin(const QString &server, const QString &username, const QString &password);
// Pauses automatic syncs for the whole process, so a performance run never
// reaches the network. The saved setting and manual syncs are unchanged.
void pauseAutomatic(bool paused);
bool automaticPaused();
// For the scenario player: clients created afterwards use this account
// instead of the saved one and never write it. An empty object restores the
// saved account. Call on the GUI thread.
void overrideAccount(const QJsonObject &account);
// The time between two automatic syncs while reading. A scenario account can
// shorten it with `interval_ms`, so a check does not wait 30 seconds for each
// sync; a saved account always uses its schedule.
int readingIntervalMilliseconds(const Schedule &schedule);

struct Progress
{
    QString position, device, deviceId;
    double percentage = 0;
    qint64 timestamp = 0;
    bool found = false;
};

bool parseProgress(const QJsonObject &object, Progress *progress);

// A failed fetch never reaches this reconciler. A remote choice must be
// acknowledged before any local position can be uploaded.
class Reconciler
{
public:
    enum class Action { Current, Upload, Choose };
    Action observe(const Progress &remote, const QString &position, const QString &deviceId);

    bool pending() const { return pending_.found; }

    bool needsSync(const QString &position) const
    {
        return !position.isEmpty() && position != uploaded_;
    }

    Progress candidate() const { return pending_; }

    void keepLocal()
    {
        pending_ = {};
        uploaded_.clear();
    }

    void acceptRemote(const QString &position)
    {
        pending_ = {};
        uploaded_ = position;
    }

    void uploaded(const QString &position) { uploaded_ = position; }

private:
    Progress observed_, pending_;
    QString uploaded_;
};

// One reader owns one client. Requests are bounded, cancellable and never redirect credentials.
class Client : public QObject
{
public:
    explicit Client(QString accountPath, QObject *parent = nullptr,
                    QNetworkAccessManager *network = nullptr, HostLookup *lookup = nullptr);
    ~Client() override;

    bool signedIn() const { return !key_.isEmpty(); }

    bool busy() const
    {
        return !reply_.isNull() || retryTimer_.isActive() || (lookup_ && lookup_->busy());
    }

    bool automatic() const { return automatic_; }

    QString server() const { return server_.toString(); }

    QString username() const { return username_; }

    QString deviceId() const { return deviceId_; }

    QString deviceName() const { return deviceName_.isEmpty() ? defaultDeviceName_ : deviceName_; }

    QString customDeviceName() const { return deviceName_; }

    QString defaultDeviceName() const { return defaultDeviceName_; }

    void setDefaultDeviceName(const QString &name);
    bool setDeviceName(const QString &name);

    bool filenameMatching() const { return filenameMatching_; }

    bool setFilenameMatching(bool enabled);

    QString storageError() const { return storageError_; }

    using Done = std::function<void(QString error)>;
    void login(const QString &server, const QString &username, const QString &password, Done done);
    bool setAutomatic(bool enabled);

    Schedule schedule() const { return schedule_; }

    bool setSchedule(const Schedule &schedule);

    bool manualExplanation() const { return manualExplanation_; }

    bool setManualExplanation(bool enabled);
    bool logout();

    QDateTime lastSync() const { return lastSync_; }

    bool recordSyncSuccess();
    // Sleep and closing checkpoint the informational timestamp without
    // rewriting credentials. Writes nothing when no sync succeeded since.
    bool persistSyncTime();
    // Queues the same write on the account writer thread and does not wait,
    // for a sync that ends while the reader sleeps. The time stays unconfirmed
    // until the next persistSyncTime(), which then only reads the file.
    void queueSyncTime();
    void cancel();
    void pull(const QByteArray &document, std::function<void(Progress, QString)> done,
              bool retryTransient = true);
    void push(const QByteArray &document, const Progress &progress, Done done);

private:
    using Response = std::function<void(int, QJsonObject, QString)>;
    void observeNetworkState();
    std::function<bool()> syncTimeWrite() const;
    void request(const QByteArray &method, const QString &path, const QUrl &server,
                 const QString &username, const QByteArray &key, const QJsonObject &body,
                 Response done, bool retryTransient = true);
    void requestAttempt(const QByteArray &method, const QString &path, const QUrl &server,
                        const QString &username, const QByteArray &key, const QJsonObject &body,
                        Response done, int attempt, int lookupAttempt = 0);
    void sendRequest(const QByteArray &method, const QString &path, const QUrl &server,
                     const QString &username, const QByteArray &key, const QJsonObject &body,
                     Response done, int attempt);
    bool retryRequest(const QByteArray &method, const QString &path, const QUrl &server,
                      const QString &username, const QByteArray &key, const QJsonObject &body,
                      Response done, int attempt);
    QTimer retryTimer_;
    QElapsedTimer requestElapsed_;
    bool save(const QUrl &server, const QString &username, const QByteArray &key, bool automatic,
              const QDateTime &lastSync, const QString &deviceName, bool filenameMatching,
              bool manualExplanation, const Schedule &schedule);
    QString accountPath_, username_, deviceId_, storageError_;
    QString deviceName_, defaultDeviceName_ = QStringLiteral("Kobalt");
    QUrl server_;
    QDateTime lastSync_;
    bool syncTimeDirty_ = false;
    QByteArray key_;
    bool automatic_ = true, writable_ = true;
    bool filenameMatching_ = false;
    bool manualExplanation_ = true;
    Schedule schedule_;
    QNetworkAccessManager ownedNetwork_;
    QNetworkAccessManager *network_;
    QPointer<QNetworkReply> reply_;
    HostLookup ownedLookup_;
    HostLookup *lookup_;
};
} // namespace kobalt::kosync
