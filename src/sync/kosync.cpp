#include "kosync.h"
#include "support/main_thread.h"
#include <fcntl.h>
#include <unistd.h>
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "support/strings.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSslError>
#include <QHostAddress>
#include <QJsonArray>
#include "support/diagnostics.h"
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <cmath>
#include <memory>

namespace kobalt::kosync {
namespace {
std::atomic_bool paused{ false };
}

void pauseAutomatic(bool value)
{
    paused = value;
}

bool automaticPaused()
{
    return paused;
}

namespace {
QJsonObject &accountOverride()
{
    static QJsonObject account;
    return account;
}
} // namespace

void overrideAccount(const QJsonObject &account)
{
    accountOverride() = account;
}

int readingIntervalMilliseconds(const Schedule &schedule)
{
    const int interval = accountOverride().value("interval_ms").toInt();
    return interval > 0 ? interval : scheduleIntervalMilliseconds(schedule);
}

namespace {
QString message(const char *key)
{
    return strings::get(key);
}

// Every client writes the account through this one writer, so a client that
// opens later reads the account only after earlier writes finished. Never
// destroyed: a queued write can outlive its client.
storage::RecordWriter &accountWriter()
{
    static auto *writer = new storage::RecordWriter;
    return *writer;
}

bool transientNetworkError(QNetworkReply::NetworkError error)
{
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        return true;
    default:
        return false;
    }
}

bool validDeviceName(const QString &name)
{
    if (name.size() > 128)
        return false;
    for (const auto ch : name)
        if (ch.category() == QChar::Other_Control)
            return false;
    return true;
}

bool validMatching(const QJsonObject &object)
{
    if (!object.contains("match_method"))
        return true;
    const auto method = object.value("match_method").toString();
    return method == "binary" || method == "filename";
}

bool hexDigest(const QByteArray &value)
{
    if (value.size() != 32)
        return false;
    for (const char ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}
} // namespace

QByteArray documentDigest(QIODevice &file)
{
    if (!file.isOpen() || file.isSequential())
        return {};
    const auto original = file.pos();
    QCryptographicHash hash(QCryptographicHash::Md5);
    bool ok = true;
    for (int sample = 0; sample < 12; ++sample) {
        // Lua's negative bit shift for the first sample places it at byte zero.
        const qint64 offset = sample == 0 ? 0 : qint64(1024) << (2 * (sample - 1));
        if (offset >= file.size())
            break;
        if (!file.seek(offset)) {
            ok = false;
            break;
        }
        const auto bytes = file.read(1024);
        if (bytes.isEmpty()) {
            ok = false;
            break;
        }
        hash.addData(bytes);
    }
    if (!file.seek(original))
        ok = false;
    return ok ? hash.result().toHex() : QByteArray();
}

QByteArray filenameDigest(const QString &path)
{
    const auto name = QFileInfo(path).fileName();
    return name.isEmpty()
            ? QByteArray()
            : QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Md5).toHex();
}

QUrl serverUrl(const QString &text)
{
    QUrl url(text.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return {};
    auto path = url.path();
    while (path.endsWith('/'))
        path.chop(1);
    url.setPath(path);
    return url;
}

bool parseProgress(const QJsonObject &object, Progress *progress)
{
    *progress = {};
    if (object.isEmpty())
        return true;
    const auto position = object.value("progress"), percent = object.value("percentage");
    const auto timestamp = object.value("timestamp");
    if (!position.isString() || position.toString().isEmpty() || position.toString().size() > 4096
        || !percent.isDouble() || !std::isfinite(percent.toDouble()) || percent.toDouble() < 0
        || percent.toDouble() > 1 || !object.value("device").isString()
        || (!object.value("device_id").isUndefined() && !object.value("device_id").isString())
        || (!timestamp.isUndefined()
            && (!timestamp.isDouble() || !std::isfinite(timestamp.toDouble())
                || timestamp.toDouble() < 0 || timestamp.toDouble() > 1e12)))
        return false;
    progress->position = position.toString();
    progress->percentage = percent.toDouble();
    progress->device = object.value("device").toString();
    progress->deviceId = object.value("device_id").toString();
    progress->timestamp = qint64(timestamp.toDouble());
    progress->found = true;
    return true;
}

Reconciler::Action Reconciler::observe(const Progress &remote, const QString &position,
                                       const QString &deviceId)
{
    if (pending())
        return Action::Choose;
    const bool changed = remote.found
            && (!observed_.found || remote.position != observed_.position
                || remote.deviceId != observed_.deviceId
                || remote.timestamp != observed_.timestamp);
    observed_ = remote;
    if (changed && remote.deviceId != deviceId && remote.position != position) {
        pending_ = remote;
        return Action::Choose;
    }
    if (!remote.found)
        uploaded_.clear();
    else if (remote.position == position)
        uploaded_ = position;
    return position.isEmpty() || position == uploaded_ ? Action::Current : Action::Upload;
}

Client::Client(QString accountPath, QObject *parent, QNetworkAccessManager *network,
               HostLookup *lookup)
    : QObject(parent),
      accountPath_(std::move(accountPath)),
      network_(network ? network : &ownedNetwork_),
      lookup_(lookup)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!lookup_ && !network)
        lookup_ = &ownedLookup_;
#endif
    retryTimer_.setSingleShot(true);
    observeNetworkState();
    deviceId_ = QUuid::createUuid().toString();
    // Read after any account write that another client queued.
    accountWriter().flush();
    const bool overridden = !accountOverride().isEmpty();
    const auto read = overridden ? storage::JsonRead{ accountOverride(), storage::ReadStatus::Ok }
                                 : storage::readJsonFile(accountPath_);
    if (read.status == storage::ReadStatus::Missing)
        return;
    // A scenario account lives in memory only.
    if (overridden)
        writable_ = false;
    const auto &object = read.object;
    const auto url = serverUrl(object.value("server").toString());
    const auto key = object.value("key").toString().toLatin1();
    if (read.status != storage::ReadStatus::Ok || object.value("schema").toDouble() != 1
        || (!object.value("server").toString().isEmpty() && url.isEmpty())
        || !object.value("server").isString() || !object.value("username").isString()
        || !object.value("key").isString()
        || (!key.isEmpty()
            && (!hexDigest(key) || url.isEmpty()
                || object.value("username").toString().trimmed().isEmpty()))
        || object.value("device_id").toString().isEmpty() || !object.value("automatic").isBool()
        || (object.contains("device_name")
            && (!object.value("device_name").isString()
                || !validDeviceName(object.value("device_name").toString())))
        || !validMatching(object)) {
        writable_ = false;
        storageError_ = message("kosync.storage_error");
        return;
    }
    server_ = url;
    username_ = object.value("username").toString();
    key_ = key;
    deviceId_ = object.value("device_id").toString();
    automatic_ = object.value("automatic").toBool();
    manualExplanation_ = object.value("manual_explanation").toBool(true);
    lastSync_ = QDateTime::fromString(object.value("last_sync").toString(), Qt::ISODate);
    deviceName_ = object.value("device_name").toString().trimmed();
    filenameMatching_ = object.value("match_method").toString() == "filename";
    // The schedule fields are optional. Invalid values use the defaults and
    // do not block writes, because they only tune background syncs.
    const auto minutes = object.value("sync_interval_minutes");
    if (minutes.isDouble() && minutes.toDouble() == double(minutes.toInt())
        && validScheduleMinutes(minutes.toInt()))
        schedule_.minutes = minutes.toInt();
    schedule_.wakeWifi = object.value("wake_wifi").toBool(false);
    schedule_.onOpen = object.value("sync_on_open").toBool(true);
    schedule_.onSleepWake = object.value("sync_on_sleep_wake").toBool(true);
    schedule_.onClose = object.value("sync_on_close").toBool(true);
}

void Client::observeNetworkState()
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && !defined(QT_NO_BEARERMANAGEMENT)
    connect(network_, &QNetworkAccessManager::networkAccessibleChanged, this,
            [](QNetworkAccessManager::NetworkAccessibility value) {
                diagnostic("kosync-qt-network-state", qt::object({ { "accessible", int(value) } }));
            });
#endif
}

Client::~Client()
{
    cancel();
}

void Client::cancel()
{
    if (lookup_)
        lookup_->cancel();
    retryTimer_.stop();
    disconnect(&retryTimer_, nullptr, this, nullptr);
    if (!reply_)
        return;
    auto *reply = reply_.data();
    auto fields = qt::object(
            { { "elapsed_ms", double(requestElapsed_.elapsed()) },
              { "network_error", int(reply->error()) },
              { "finished", reply->isFinished() },
              { "http_status", reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() },
              { "encrypted",
                reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute).toBool() } });
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && !defined(QT_NO_BEARERMANAGEMENT)
    fields["qt_network_accessible"] = int(network_->networkAccessible());
#endif
    diagnostic("kosync-request-cancelled", fields);
    reply_ = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    // A cancelled TLS handshake can hold later requests in Qt's connection
    // pool. This client owns its transport, so drop that pool when cancelling.
    network_->clearAccessCache();
    reply->deleteLater();
}

namespace {
struct Account
{
    QUrl server;
    QString username, deviceId, deviceName;
    QByteArray key;
    bool automatic, filenameMatching, manualExplanation;
    QDateTime lastSync;
    Schedule schedule;
};

// Runs on the writer thread. Fields the client does not know are kept.
bool writeAccount(const QString &path, const Account &account)
{
    const auto previous = storage::readJsonFile(path);
    if (previous.status != storage::ReadStatus::Missing
        && (previous.status != storage::ReadStatus::Ok
            || previous.object.value("schema").toDouble() != 1 || !validMatching(previous.object)))
        return false;
    auto object = previous.object;
    object["schema"] = 1;
    object["server"] = account.server.toString();
    object["username"] = account.username;
    object["key"] = QString::fromLatin1(account.key);
    object["device_id"] = account.deviceId;
    object["automatic"] = account.automatic;
    object["manual_explanation"] = account.manualExplanation;
    object["match_method"] =
            account.filenameMatching ? QStringLiteral("filename") : QStringLiteral("binary");
    if (account.deviceName.isEmpty())
        object.remove("device_name");
    else
        object["device_name"] = account.deviceName;
    object["sync_interval_minutes"] = account.schedule.minutes;
    object["wake_wifi"] = account.schedule.wakeWifi;
    object["sync_on_open"] = account.schedule.onOpen;
    object["sync_on_sleep_wake"] = account.schedule.onSleepWake;
    object["sync_on_close"] = account.schedule.onClose;
    if (account.lastSync.isValid())
        object["last_sync"] = account.lastSync.toUTC().toString(Qt::ISODate);
    else
        object.remove("last_sync");
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    // Kobo profiles live on FAT, which cannot enforce Unix owner permissions.
    // Private desktop and Android profiles apply them when supported.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return file.write(bytes) == bytes.size() && file.commit();
}
} // namespace

bool Client::save(const QUrl &server, const QString &username, const QByteArray &key,
                  bool automatic, const QDateTime &lastSync, const QString &deviceName,
                  bool filenameMatching, bool manualExplanation, const Schedule &schedule)
{
    if (!writable_)
        return false;
    // Known exception: sign-in and account settings wait for their write, so
    // the page can show a storage error and keep the previous values.
    mainthread::AllowBlocking allow("kosync account settings");
    const Account account{ server,    username,         deviceId_,         deviceName, key,
                           automatic, filenameMatching, manualExplanation, lastSync,   schedule };
    if (!accountWriter().writeNow(
                [path = accountPath_, account] { return writeAccount(path, account); })) {
        storageError_ = message("kosync.storage_error");
        return false;
    }
    server_ = server;
    username_ = username;
    key_ = key;
    automatic_ = automatic;
    manualExplanation_ = manualExplanation;
    lastSync_ = lastSync;
    syncTimeDirty_ = false;
    deviceName_ = deviceName;
    filenameMatching_ = filenameMatching;
    schedule_ = schedule;
    storageError_.clear();
    return true;
}

void Client::setDefaultDeviceName(const QString &name)
{
    defaultDeviceName_ = validDeviceName(name) && !name.trimmed().isEmpty()
            ? name.trimmed()
            : QStringLiteral("Kobalt");
}

bool Client::setDeviceName(const QString &name)
{
    if (!validDeviceName(name))
        return false;
    return save(server_, username_, key_, automatic_, lastSync_, name.trimmed(), filenameMatching_,
                manualExplanation_, schedule_);
}

bool Client::setFilenameMatching(bool enabled)
{
    if (busy())
        return false;
    if (enabled == filenameMatching_)
        return true;
    return save(server_, username_, key_, automatic_, {}, deviceName_, enabled, manualExplanation_,
                schedule_);
}

bool Client::setAutomatic(bool enabled)
{
    return save(server_, username_, key_, enabled, lastSync_, deviceName_, filenameMatching_,
                manualExplanation_, schedule_);
}

bool Client::setSchedule(const Schedule &schedule)
{
    if (!validScheduleMinutes(schedule.minutes))
        return false;
    return save(server_, username_, key_, automatic_, lastSync_, deviceName_, filenameMatching_,
                manualExplanation_, schedule);
}

bool Client::setManualExplanation(bool enabled)
{
    return save(server_, username_, key_, automatic_, lastSync_, deviceName_, filenameMatching_,
                enabled, schedule_);
}

bool Client::logout()
{
    cancel();
    return save(server_, username_, {}, false, lastSync_, deviceName_, filenameMatching_,
                manualExplanation_, schedule_);
}

bool Client::recordSyncSuccess()
{
    if (!signedIn() || !writable_)
        return false;
    lastSync_ = QDateTime::currentDateTimeUtc();
    syncTimeDirty_ = true;
    return true;
}

std::function<bool()> Client::syncTimeWrite() const
{
    return [path = accountPath_, server = server_, username = username_, key = key_,
            device = deviceId_, stamp = lastSync_, filenameMatching = filenameMatching_] {
        const auto read = storage::readJsonFile(path);
        if (read.status != storage::ReadStatus::Ok)
            return false;
        auto object = read.object;
        // Another client may have changed or removed the account. Never
        // restore old credentials or overwrite its settings just for a date.
        if (object.value("schema").toInt() != 1
            || object.value("server").toString() != server.toString()
            || object.value("username").toString() != username
            || object.value("key").toString().toLatin1() != key
            || object.value("device_id").toString() != device
            || object.value("match_method").toString("binary")
                    != (filenameMatching ? "filename" : "binary"))
            return false;
        const auto existing =
                QDateTime::fromString(object.value("last_sync").toString(), Qt::ISODate);
        if (existing.isValid() && existing >= stamp)
            return true;
        object["last_sync"] = stamp.toUTC().toString(Qt::ISODate);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        const auto bytes = QJsonDocument(object).toJson();
        return file.write(bytes) == bytes.size() && file.commit();
    };
}

bool Client::persistSyncTime()
{
    if (!syncTimeDirty_)
        return true;
    const bool saved = accountWriter().writeNow(syncTimeWrite());
    if (saved)
        syncTimeDirty_ = false;
    else
        diagnostic("kosync-sync-time-unsaved", {}, DiagnosticSeverity::Warning);
    return saved;
}

void Client::queueSyncTime()
{
    if (!syncTimeDirty_)
        return;
    accountWriter().post(syncTimeWrite(), [](bool saved) {
        if (!saved)
            diagnostic("kosync-sync-time-unsaved", {}, DiagnosticSeverity::Warning);
    });
}

void Client::request(const QByteArray &method, const QString &path, const QUrl &server,
                     const QString &username, const QByteArray &key, const QJsonObject &body,
                     Response done, bool retryTransient)
{
    if (busy()) {
        done(0, {}, message("kosync.busy"));
        return;
    }
    requestElapsed_.start();
    requestAttempt(method, path, server, username, key, body, std::move(done),
                   retryTransient ? 0 : 3);
}

void Client::requestAttempt(const QByteArray &method, const QString &path, const QUrl &server,
                            const QString &username, const QByteArray &key, const QJsonObject &body,
                            Response done, int attempt, int lookupAttempt)
{
    if (requestElapsed_.elapsed() >= 10000) {
        done(0, {}, message("kosync.network_error"));
        return;
    }
    if (!lookup_ || !QHostAddress(server.host()).isNull()) {
        sendRequest(method, path, server, username, key, body, std::move(done), attempt);
        return;
    }
    const int remaining = qMax(1, 10000 - int(requestElapsed_.elapsed()));
    // A lookup begun during Wi-Fi reconnection can outlive the connection
    // change. Leave time for one fresh lookup before any HTTP is sent.
    const int lookupBudget = lookupAttempt == 0 ? qMin(1000, remaining) : remaining;
    diagnostic("kosync-lookup-started",
               qt::object({ { "attempt", attempt }, { "lookup_attempt", lookupAttempt } }));
    lookup_->start(
            server.host(), lookupBudget,
            [this, method, path, server, username, key, body, done, attempt,
             lookupAttempt](QHostInfo result) {
                diagnostic("kosync-lookup-finished",
                           qt::object({ { "error", int(result.error()) },
                                        { "elapsed_ms", double(requestElapsed_.elapsed()) },
                                        { "attempt", attempt },
                                        { "lookup_attempt", lookupAttempt } }));
                if (result.error() != QHostInfo::NoError || result.addresses().isEmpty()) {
                    // This is safe for manual reads and uploads too: no HTTP
                    // request has started, and the overall deadline is unchanged.
                    if (lookupAttempt == 0 && requestElapsed_.elapsed() < 10000) {
                        requestAttempt(method, path, server, username, key, body, done, attempt, 1);
                        return;
                    }
                    if (!retryRequest(method, path, server, username, key, body, done, attempt))
                        done(0, {}, message("kosync.network_error"));
                    return;
                }
                // fromName fills Qt's ordinary hostname cache. Keep the URL intact
                // so HTTP authority, TLS SNI and certificate checks remain intact.
                sendRequest(method, path, server, username, key, body, done, attempt);
            });
}

bool Client::retryRequest(const QByteArray &method, const QString &path, const QUrl &server,
                          const QString &username, const QByteArray &key, const QJsonObject &body,
                          Response done, int attempt)
{
    static const int delays[] = { 500, 1500, 3000 };
    if (method != "GET" || attempt >= 3 || requestElapsed_.elapsed() + delays[attempt] >= 10000)
        return false;
    network_->clearAccessCache();
    disconnect(&retryTimer_, nullptr, this, nullptr);
    connect(&retryTimer_, &QTimer::timeout, this,
            [this, method, path, server, username, key, body, done, attempt] {
                disconnect(&retryTimer_, nullptr, this, nullptr);
                requestAttempt(method, path, server, username, key, body, done, attempt + 1);
            });
    retryTimer_.start(delays[attempt]);
    return true;
}

void Client::sendRequest(const QByteArray &method, const QString &path, const QUrl &server,
                         const QString &username, const QByteArray &key, const QJsonObject &body,
                         Response done, int attempt)
{
    if (requestElapsed_.elapsed() >= 10000) {
        done(0, {}, message("kosync.network_error"));
        return;
    }
    QUrl url = server;
    url.setPath(server.path() + path);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.koreader.v1+json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-auth-user", username.toUtf8());
    request.setRawHeader("x-auth-key", key);
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
#endif
    auto *reply = method == "GET"
            ? network_->get(request)
            : network_->put(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    reply_ = reply;
    diagnostic("kosync-request-started",
               qt::object({ { "method", QString::fromLatin1(method) }, { "attempt", attempt } }));
    connect(reply, &QNetworkReply::encrypted, this, [] { diagnostic("kosync-tls-connected"); });
    connect(reply, &QNetworkReply::sslErrors, this, [](const QList<QSslError> &errors) {
        QJsonArray codes;
        for (const auto &error : errors)
            codes.append(int(error.error()));
        diagnostic("kosync-tls-errors", qt::object({ { "codes", codes } }),
                   kobalt::DiagnosticSeverity::Error);
    });
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(qMax(1, 10000 - int(requestElapsed_.elapsed())));
    reply->setReadBufferSize(storage::maxJsonBytes + 1);
    auto bytes = std::make_shared<QByteArray>();
    auto drain = [reply, bytes] {
        bytes->append(reply->read(storage::maxJsonBytes + 1 - bytes->size()));
        if (bytes->size() > storage::maxJsonBytes)
            reply->abort();
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, bytes, drain, done, method, path, server, username, key, body, attempt] {
                if (reply_ != reply)
                    return;
                drain();
                reply_ = nullptr;
                reply->deleteLater();
                const int status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                {
                    auto fields =
                            qt::object({ { "network_error", int(reply->error()) },
                                         { "http_status", status },
                                         { "method", QString::fromLatin1(method) },
                                         { "attempt", attempt },
                                         { "elapsed_ms", double(requestElapsed_.elapsed()) } });
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && !defined(QT_NO_BEARERMANAGEMENT)
                    fields["qt_network_accessible"] = int(network_->networkAccessible());
#endif
                    // Keep URLs, authentication, response bodies and book IDs out of diagnostics.
                    diagnostic("kosync-request-finished", fields);
                }
                if (status == 401) {
                    done(status, {}, message("kosync.auth_error"));
                    return;
                }
                // Retry only reads whose transport failed. A PUT may already have reached
                // the server; authentication, TLS and HTTP failures need an explicit fix.
                if (status == 0 && transientNetworkError(reply->error())
                    && retryRequest(method, path, server, username, key, body, done, attempt))
                    return;
                if (reply->error() != QNetworkReply::NoError || status != 200
                    || bytes->size() > storage::maxJsonBytes) {
                    // Qt opens sockets on its own thread, so errno here says nothing.
                    // A transport failure checks whether descriptors ran out now.
                    if (status == 0) {
                        const int probe = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                        if (probe >= 0)
                            ::close(probe);
                        else
                            reportDescriptorExhaustion("kosync-request");
                    }
                    done(status, {}, message("kosync.network_error"));
                    return;
                }
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(*bytes, &error);
                // Some servers encode absent progress as [] instead of {}.
                if (method == "GET" && path.startsWith("/syncs/progress/") && document.isArray()
                    && document.array().isEmpty()) {
                    done(status, {}, {});
                    return;
                }
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    diagnostic("kosync-response-invalid",
                               qt::object({ { "stage", "json" },
                                            { "parse_error", int(error.error) } }));
                    done(status, {}, message("kosync.invalid_response"));
                    return;
                }
                done(status, document.object(), {});
            });
}

bool validLogin(const QString &server, const QString &username, const QString &password)
{
    return !serverUrl(server).isEmpty() && !username.trimmed().isEmpty() && !username.contains('\r')
            && !username.contains('\n') && !username.contains(QChar::Null) && username.size() <= 256
            && !password.isEmpty();
}

void Client::login(const QString &server, const QString &username, const QString &password,
                   Done done)
{
    if (!validLogin(server, username, password)) {
        done(message("kosync.invalid_account"));
        return;
    }
    const auto url = serverUrl(server);
    const auto key = QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex();
    request("GET", "/users/auth", url, username, key, {},
            [this, url, username, key, done](int status, QJsonObject object, QString error) {
                if (error.isEmpty()
                    && (status != 200 || object.value("authorized").toString() != "OK"))
                    error = message("kosync.auth_error");
                if (error.isEmpty()
                    && !save(url, username, key, true,
                             url == server_ && username == username_ ? lastSync_ : QDateTime{},
                             deviceName_, filenameMatching_, manualExplanation_, schedule_))
                    error = message("kosync.storage_error");
                done(error);
            });
}

void Client::pull(const QByteArray &document, std::function<void(Progress, QString)> done,
                  bool retryTransient)
{
    if (!signedIn() || !hexDigest(document)) {
        done({}, message("kosync.unavailable"));
        return;
    }
    request(
            "GET", "/syncs/progress/" + QString::fromLatin1(document), server_, username_, key_, {},
            [done](int, QJsonObject object, QString error) {
                Progress progress;
                if (error.isEmpty() && !parseProgress(object, &progress)) {
                    diagnostic("kosync-response-invalid", qt::object({ { "stage", "progress" } }));
                    error = message("kosync.invalid_response");
                }
                done(progress, error);
            },
            retryTransient);
}

void Client::push(const QByteArray &document, const Progress &progress, Done done)
{
    if (!signedIn() || !hexDigest(document) || progress.position.isEmpty()
        || progress.position.size() > 4096 || !std::isfinite(progress.percentage)
        || progress.percentage < 0 || progress.percentage > 1) {
        done(message("kosync.unavailable"));
        return;
    }
    QJsonObject object;
    object["document"] = QString::fromLatin1(document);
    object["progress"] = progress.position;
    object["percentage"] = progress.percentage;
    object["device"] = deviceName();
    object["device_id"] = deviceId_;
    request("PUT", "/syncs/progress", server_, username_, key_, object,
            [done, document](int status, QJsonObject response, QString error) {
                if (error.isEmpty()
                    && (status != 200
                        || response.value("document").toString().toLatin1() != document))
                    error = message("kosync.invalid_response");
                done(error);
            });
}
} // namespace kobalt::kosync
