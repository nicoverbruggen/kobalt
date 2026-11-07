#include "sync/kosync.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include "check.h"
#include "kosync_reply.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkReply>
#include <QHostAddress>
#include <QSemaphore>
#include <QSslCertificate>
#include <QSslError>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cstring>
#include <limits>
#include <atomic>

using namespace kobalt;

namespace {
using Reply = test::KosyncReply;

class Network final : public QNetworkAccessManager
{
public:
    QByteArray response = "{}", payload;
    int status = 200, calls = 0, failures = 0;
    bool held = false, hostile = false;
    QNetworkReply::NetworkError transportError = QNetworkReply::ConnectionRefusedError;
    QNetworkRequest last;
    QPointer<QNetworkReply> lastReply;
    Operation operation = GetOperation;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                                 QIODevice *data) override
    {
        ++calls;
        last = request;
        operation = op;
        payload = data ? data->readAll() : QByteArray();
        const auto error = failures > 0 ? (--failures, transportError) : QNetworkReply::NoError;
        auto *reply = new Reply(request, response, status, error, this, held);
        if (hostile)
            reply->hostileMetadata();
        lastReply = reply;
        return lastReply;
    }
};

void wait(kosync::Client &client, int timeout = 1000)
{
    QEventLoop loop;
    QTimer tick, deadline;
    QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
        if (!client.busy())
            loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop,
                     [] { require(false, "KOsync request timed out"); });
    tick.start(1);
    deadline.start(timeout);
    if (client.busy())
        loop.exec();
}

void save(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "write account fixture");
}

void drainEvents(int milliseconds = 60)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QHostInfo resolvedHost(const QString &host)
{
    QHostInfo result;
    result.setHostName(host);
    result.setAddresses({ QHostAddress::LocalHost });
    return result;
}

void lookupChecks(const QString &path)
{
    {
        QSemaphore started, release, finished;
        std::atomic<bool> stallNext{ true };
        std::atomic<int> lookups{ 0 };
        kosync::HostLookup lookup([&](const QString &host) {
            ++lookups;
            if (stallNext.exchange(false)) {
                started.release();
                release.acquire();
                finished.release();
            }
            return resolvedHost(host);
        });
        Network network;
        network.response = R"({"authorized":"OK"})";
        kosync::Client client(path, nullptr, &network, &lookup);
        int callbacks = 0;
        QString error;
        const auto done = [&](QString result) {
            error = result;
            ++callbacks;
        };
        const auto recover = [&](int previousHttpCalls, int previousLookups, bool expectError) {
            require(started.tryAcquire(1, 1000), "first DNS lookup is blocked");
            wait(client, 2500);
            require(network.calls == previousHttpCalls + 1 && lookups == previousLookups + 2,
                    "one sync call recovers through a fresh DNS lookup and sends HTTP once");
            require(error.isEmpty() != expectError, "DNS recovery preserves the HTTP result");
            release.release();
            require(finished.tryAcquire(1, 1000), "release the abandoned first lookup");
            drainEvents();
            require(network.calls == previousHttpCalls + 1,
                    "late DNS completion cannot replay HTTP");
        };
        client.login("https://fixture.invalid", "reader", "secret", done);
        recover(0, 0, false);
        require(callbacks == 1, "sign-in completes once after automatic DNS recovery");
        const QByteArray document(32, 'a');
        network.response = "{}";
        stallNext.store(true);
        client.pull(document, [&](kosync::Progress, QString result) { done(result); }, false);
        recover(1, 2, false);
        require(callbacks == 2, "manual pull recovers even when HTTP retries are disabled");
        network.response = "{\"document\":\"" + document + "\"}";
        network.failures = 1;
        stallNext.store(true);
        client.push(document, { "local", {}, {}, 0.5 }, done);
        recover(2, 4, true);
        require(callbacks == 3 && network.operation == QNetworkAccessManager::PutOperation,
                "upload DNS recovery does not enable replay after an HTTP transport failure");
    }
    {
        QSemaphore started, release, finished;
        kosync::HostLookup lookup([&](const QString &host) {
            started.release();
            release.acquire();
            const auto result = resolvedHost(host);
            finished.release();
            return result;
        });
        Network network;
        kosync::Client client(path, nullptr, &network, &lookup);
        int callbacks = 0;
        client.pull(QByteArray(32, 'a'), [&](kosync::Progress, QString) { ++callbacks; }, false);
        require(started.tryAcquire(1, 1000), "first lookup started before cancellation test");
        drainEvents(1150);
        require(started.tryAcquire(1, 1000) && client.busy() && network.calls == 0,
                "second lookup starts within the same busy request before HTTP");
        client.cancel();
        require(!client.busy(), "cancellation stops the second DNS subattempt immediately");
        release.release(2);
        require(finished.tryAcquire(2, 1000), "release both cancelled DNS subattempts");
        drainEvents();
        require(callbacks == 0 && network.calls == 0,
                "neither lookup can dispatch HTTP after cancellation");
    }
    {
        QSemaphore started, release, finished;
        std::atomic<int> calls{ 0 };
        int callbacks = 0;
        const auto blocked = [&](const QString &host) {
            ++calls;
            started.release();
            release.acquire();
            const auto result = resolvedHost(host);
            finished.release();
            return result;
        };
        auto first = std::make_unique<kosync::HostLookup>(blocked);
        auto second = std::make_unique<kosync::HostLookup>(blocked);
        first->start("fixture.invalid", 1000, [&](QHostInfo) { ++callbacks; });
        second->start("fixture.invalid", 1000, [&](QHostInfo) { ++callbacks; });
        require(started.tryAcquire(2, 1000), "two independent DNS lookups can run");
        QElapsedTimer closing;
        closing.start();
        first.reset();
        second.reset();
        require(closing.elapsed() < 100, "destroying readers does not wait for blocked DNS");
        kosync::HostLookup saturated(blocked);
        bool rejected = false;
        saturated.start("fixture.invalid", 1000,
                        [&](QHostInfo result) { rejected = result.error() != QHostInfo::NoError; });
        require(!rejected && saturated.busy(), "pool saturation completes asynchronously");
        drainEvents();
        require(rejected && calls == 2,
                "cancelled running lookups still count toward the process limit");
        release.release(2);
        require(finished.tryAcquire(2, 1000), "release both cancelled resolver calls");
        drainEvents();
        require(callbacks == 0, "destroyed lookups never deliver late callbacks");
    }
    {
        QSemaphore started, release;
        std::atomic<int> calls{ 0 };
        kosync::HostLookup lookup([&](const QString &host) {
            if (++calls == 1) {
                started.release();
                release.acquire();
            }
            return resolvedHost(host);
        });
        Network network;
        network.response = R"({"authorized":"OK"})";
        auto client = std::make_unique<kosync::Client>(path, nullptr, &network, &lookup);
        int callbacks = 0;
        client->login("https://fixture.invalid/prefix", "reader", "secret",
                      [&](QString) { ++callbacks; });
        require(client->busy() && network.calls == 0, "DNS is busy before HTTP starts");
        require(started.tryAcquire(1, 1000), "first client lookup started");
        client->cancel();
        require(!client->busy(), "cancellation immediately clears the lookup phase");
        client->login("https://fixture.invalid/prefix", "reader", "secret", [&](QString error) {
            require(error.isEmpty(), "a fresh lookup can bypass the cancelled lookup");
            ++callbacks;
        });
        wait(*client);
        require(callbacks == 1 && network.calls == 1
                        && network.last.url() == QUrl("https://fixture.invalid/prefix/users/auth"),
                "fresh DNS preserves the HTTPS authority and URL prefix");
        release.release();
        drainEvents();
        require(callbacks == 1 && network.calls == 1,
                "cancelled DNS cannot start a stale HTTP request");
        client->login("https://127.0.0.1/prefix", "reader", "secret",
                      [&](QString) { ++callbacks; });
        wait(*client);
        require(calls == 2, "numeric addresses bypass DNS workers");
    }
    {
        QSemaphore started, release, finished;
        kosync::HostLookup lookup([&](const QString &host) {
            started.release();
            release.acquire();
            const auto result = resolvedHost(host);
            finished.release();
            return result;
        });
        Network network;
        auto client = std::make_unique<kosync::Client>(path, nullptr, &network, &lookup);
        int callbacks = 0;
        client->login("https://fixture.invalid", "reader", "secret", [&](QString) { ++callbacks; });
        require(started.tryAcquire(1, 1000), "client destruction test has a running resolver");
        client.reset();
        release.release();
        require(finished.tryAcquire(1, 1000), "resolver returned after client destruction");
        drainEvents();
        require(callbacks == 0 && network.calls == 0 && !lookup.busy(),
                "client destruction suppresses DNS and HTTP completion");
    }
    {
        QSemaphore started, release, finished;
        kosync::HostLookup lookup([&](const QString &host) {
            started.release();
            release.acquire();
            const auto result = resolvedHost(host);
            finished.release();
            return result;
        });
        bool timedOut = false;
        lookup.start("fixture.invalid", 40,
                     [&](QHostInfo result) { timedOut = result.error() != QHostInfo::NoError; });
        require(started.tryAcquire(1, 1000), "deadline test started resolver");
        drainEvents(100);
        require(timedOut && !lookup.busy(), "lookup deadline returns without waiting for libc");
        release.release();
        require(finished.tryAcquire(1, 1000), "release timed out resolver");
        drainEvents();
    }
}

int liveServer(const QString &server, const QString &certificate)
{
    const auto url = kosync::serverUrl(server);
    require(!url.isEmpty() && (url.host() == "localhost" || url.host() == "127.0.0.1"),
            "integration accounts may only be created on localhost");
    const auto certificates = QSslCertificate::fromPath(certificate);
    require(certificates.size() == 1, "load the local server certificate");
    auto ssl = QSslConfiguration::defaultConfiguration();
    auto authorities = ssl.caCertificates();
    authorities.append(certificates);
    ssl.setCaCertificates(authorities);
    QSslConfiguration::setDefaultConfiguration(ssl);

    QTemporaryDir profile;
    require(profile.isValid(), "create integration profiles");
    const auto username = "fixture-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString password = "local-fixture-password";
    QJsonObject registration;
    registration["username"] = username;
    registration["password"] = QString::fromLatin1(
            QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex());
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(server + "/users/create"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/vnd.koreader.v1+json");
    auto *reply = network.post(request, QJsonDocument(registration).toJson(QJsonDocument::Compact));
    QEventLoop registrationLoop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &registrationLoop, &QEventLoop::quit);
    deadline.start(10000);
    registrationLoop.exec();
    require(reply->error() == QNetworkReply::NoError
                    && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 201,
            qPrintable("register a local fixture account: " + reply->errorString()));
    reply->deleteLater();

    kosync::Client first(profile.path() + "/first.json"), second(profile.path() + "/second.json");
    first.setDefaultDeviceName("Kobo Clara BW (Kobalt)");
    require(first.setDeviceName("Kitchen Kobo"), "save local fixture device name");
    QString error;
    const auto done = [&](QString value) { error = value; };
    first.login(server, username, password, done);
    wait(first, 12000);
    require(error.isEmpty() && first.signedIn(), qPrintable("real HTTPS login: " + error));
    second.login(server, username, password, done);
    wait(second, 12000);
    require(error.isEmpty() && second.deviceId() != first.deviceId(),
            "second reader has its own device identity");
    const QByteArray document("900150983cd24fb0d6963f7d28e17f72");
    kosync::Progress received;
    const auto pulled = [&](kosync::Progress value, QString failure) {
        received = value;
        error = failure;
    };
    first.pull(document, pulled);
    wait(first, 12000);
    require(error.isEmpty() && !received.found,
            qPrintable("new account has no reading progress: " + error));
    kosync::Progress progress{ "/body/DocFragment[2]/body/p[3]/text().0", {}, {}, 0.42 };
    first.push(document, progress, done);
    wait(first, 12000);
    require(error.isEmpty(), qPrintable("send reading progress: " + error));
    second.pull(document, pulled);
    wait(second, 12000);
    require(error.isEmpty() && received.found && received.position == progress.position
                    && received.percentage == progress.percentage
                    && received.deviceId == first.deviceId() && received.timestamp > 0
                    && received.device == "Kitchen Kobo",
            "second reader receives the exact position and server timestamp");
    kosync::Reconciler reconciliation;
    require(reconciliation.observe(received, progress.position, first.deviceId())
                    == kosync::Reconciler::Action::Current,
            "first reader recognizes its saved position");
    progress.position = "/body/DocFragment[5]/body/p[7]/text().0";
    progress.percentage = 0.73;
    second.push(document, progress, done);
    wait(second, 12000);
    require(error.isEmpty(), "second reader advances the publication");
    first.pull(document, pulled);
    wait(first, 12000);
    require(error.isEmpty() && received.position == progress.position
                    && received.deviceId == second.deviceId(),
            "first reader observes progress from the second reader");
    require(reconciliation.observe(received, "/body/DocFragment[2]/body/p[3]/text().0",
                                   first.deviceId())
                    == kosync::Reconciler::Action::Choose,
            "real remote progress requires a choice before local overwrite");
    kosync::Client rejected(profile.path() + "/rejected.json");
    rejected.login(server, username, "wrong-password", done);
    wait(rejected, 12000);
    require(!error.isEmpty() && !rejected.signedIn(), "real server rejects invalid credentials");
    return 0;
}
} // namespace

// Failure categories and guidance must survive hostile network text without disclosing it.
void failureChecks(const QString &path)
{
    Network network;
    network.response = R"({"authorized":"OK"})";
    kosync::Client client(path, nullptr, &network);
    QString error;
    auto done = [&](QString value) { error = value; };
    client.login("https://fixture.invalid/private-prefix", "PRIVATE-SENTINEL", "PRIVATE-SENTINEL",
                 done);
    wait(client);
    require(error.isEmpty(), "diagnostic fixture signs in");
    const QByteArray document(32, 'b');
    const kosync::Progress progress{ "PRIVATE-SENTINEL", "PRIVATE-SENTINEL", "PRIVATE-SENTINEL",
                                     0.5 };
    QByteArray records;
    QJsonObject lastFailure, tlsFailure;
    setDiagnosticObserver([&](const QJsonObject &entry) {
        records += QJsonDocument(entry).toJson(QJsonDocument::Compact);
        if (entry.value("event") == "kosync-failure")
            lastFailure = entry;
        if (entry.value("event") == "kosync-tls-errors")
            tlsFailure = entry;
    });
    network.hostile = true;
    const auto check = [&](const char *code, const char *phase) {
        require(error == strings::get(code),
                "failure gives the appropriate troubleshooting message");
        require(lastFailure.value("code").toString() == code
                        && lastFailure.value("phase").toString() == phase,
                "failure logs its stable category and safe request phase");
        require(lastFailure.value("guidance").toString() == error,
                "failure log includes actionable fixed guidance");
        require(!records.contains("PRIVATE-SENTINEL") && !records.contains("private-prefix")
                        && !records.contains(document)
                        && !records.contains(client.deviceId().toUtf8())
                        && !records.contains("fixture.invalid")
                        && !records.contains("elsewhere.invalid")
                        && !records.contains(QCryptographicHash::hash("PRIVATE-SENTINEL",
                                                                      QCryptographicHash::Md5)
                                                     .toHex()),
                "diagnostics omit credentials, hashes, paths, redirects, bodies and identities");
    };
    for (const auto &pair :
         { std::make_pair(QNetworkReply::HostNotFoundError, "kosync.dns_error"),
           std::make_pair(QNetworkReply::ConnectionRefusedError, "kosync.connection_refused"),
           std::make_pair(QNetworkReply::RemoteHostClosedError, "kosync.connection_closed"),
           std::make_pair(QNetworkReply::SslHandshakeFailedError, "kosync.tls_error"),
           std::make_pair(QNetworkReply::TimeoutError, "kosync.timeout_error"),
           std::make_pair(QNetworkReply::OperationCanceledError, "kosync.interrupted"),
           std::make_pair(QNetworkReply::UnknownNetworkError, "kosync.network_error") }) {
        network.transportError = pair.first;
        network.failures = 1;
        client.pull(
                document,
                [&](kosync::Progress value, QString failure) {
                    require(!value.found, "transport failure cannot supply progress");
                    done(failure);
                },
                false);
        wait(client);
        check(pair.second, "read-position");
    }
    for (const auto &pair :
         { std::make_pair(401, "kosync.auth_error"), std::make_pair(403, "kosync.forbidden"),
           std::make_pair(404, "kosync.route_error"), std::make_pair(405, "kosync.route_error"),
           std::make_pair(302, "kosync.redirect_error"), std::make_pair(429, "kosync.rate_limited"),
           std::make_pair(503, "kosync.server_error"), std::make_pair(204, "kosync.http_error") }) {
        network.status = pair.first;
        network.response = "PRIVATE-SENTINEL";
        // Qt reports both HTTP status and a network error for server rejection.
        network.transportError = QNetworkReply::ContentAccessDenied;
        network.failures = pair.first >= 400 ? 1 : 0;
        const int before = network.calls;
        client.push(document, progress, done);
        wait(client);
        require(network.calls == before + 1,
                "HTTP failures never replay an upload or redirect credentials");
        check(pair.second, "upload-position");
    }
    network.status = 404;
    network.response = R"({"status":"not found","secret":"PRIVATE-SENTINEL"})";
    client.pull(document, [&](kosync::Progress, QString value) { done(value); });
    wait(client);
    check("kosync.route_error", "read-position");
    network.status = 200;
    for (const auto &body : { QByteArray("<html>PRIVATE-SENTINEL</html>"),
                              QByteArray(R"({"authorized":123,"secret":"PRIVATE-SENTINEL"})") }) {
        network.response = body;
        client.login("https://fixture.invalid/private-prefix", "PRIVATE-SENTINEL",
                     "PRIVATE-SENTINEL", done);
        wait(client);
        check("kosync.invalid_response", "sign-in");
    }
    network.response = R"({"authorized":"PRIVATE-SENTINEL"})";
    client.login("https://fixture.invalid/private-prefix", "PRIVATE-SENTINEL", "PRIVATE-SENTINEL",
                 done);
    wait(client);
    check("kosync.invalid_response", "sign-in");
    network.status = 401;
    network.response = QByteArray(17000, 'x') + "PRIVATE-SENTINEL";
    client.login("https://fixture.invalid/private-prefix", "PRIVATE-SENTINEL", "PRIVATE-SENTINEL",
                 done);
    wait(client);
    check("kosync.auth_error", "sign-in");
    network.status = 200;
    network.response = R"({"document":"PRIVATE-SENTINEL"})";
    client.push(document, progress, done);
    wait(client);
    check("kosync.invalid_response", "upload-position");
    {
        kosync::HostLookup lookup([](const QString &) {
            QHostInfo result;
            result.setError(QHostInfo::HostNotFound);
            result.setErrorString("PRIVATE-SENTINEL");
            return result;
        });
        Network resolverNetwork;
        kosync::Client resolving(path, nullptr, &resolverNetwork, &lookup);
        resolving.pull(document, [&](kosync::Progress, QString value) { done(value); }, false);
        wait(resolving);
        check("kosync.dns_error", "read-position");
        require(resolverNetwork.calls == 0 && !lookup.timedOut(),
                "a resolver error stays distinct from a lookup deadline and sends no credentials");
    }
    network.response = QByteArray(17000, 'x') + "PRIVATE-SENTINEL";
    client.push(document, progress, done);
    wait(client);
    check("kosync.response_too_large", "upload-position");
    network.response = "{}";
    network.held = true;
    client.push(document, progress, done);
    QList<QSslError> certificateErrors;
    for (int i = 0; i < 40; ++i)
        certificateErrors.append(QSslError(QSslError::HostNameMismatch));
    network.lastReply->sslErrors(certificateErrors);
    require(tlsFailure.value("codes").toArray().size() == 16
                    && tlsFailure.value("omitted").toInt() == 24
                    && tlsFailure.value("phase").toString() == "upload-position",
            "TLS diagnostics retain bounded numeric codes and the safe request phase");
    const auto timers = network.lastReply->findChildren<QTimer *>();
    require(timers.size() == 1, "request has one bounded deadline timer");
    QMetaObject::invokeMethod(timers.first(), "timeout", Qt::DirectConnection);
    check("kosync.timeout_error", "upload-position");
    error = "unchanged";
    lastFailure = {};
    client.push(document, progress, done);
    client.cancel();
    drainEvents();
    require(error == "unchanged" && lastFailure.isEmpty() && !client.busy(),
            "intentional cancellation has no failure callback or failure event");
    setDiagnosticObserver({});
}

// Checks progress reconciliation, document identity, matching modes, accounts, the schedule, responses, read retries and hostname lookups.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 4 && QByteArray(argv[1]) == "--server")
        return liveServer(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    {
        using Action = kosync::Reconciler::Action;
        kosync::Reconciler sync;
        require(sync.observe({}, "local", "this-device") == Action::Upload,
                "first progress may be uploaded");
        require(sync.needsSync("local"), "an unsuccessful attempt leaves progress dirty for retry");
        require(!sync.needsSync({}), "an empty position cannot trigger a reading sync");
        sync.uploaded("local");
        require(!sync.needsSync("local"), "a confirmed upload suppresses unchanged reading checks");
        require(sync.needsSync("later-local"), "a later page turn needs another exchange");
        require(sync.observe({}, "local", "this-device") == Action::Upload,
                "a server that lost its data receives progress again");
        kosync::Progress remote{ "local", "Kobalt", "this-device", 0.3, 100, true };
        require(sync.observe(remote, "local", "this-device") == Action::Current,
                "unchanged position is not uploaded");
        require(sync.observe(remote, "new-local", "this-device") == Action::Upload,
                "a page turn makes progress dirty");
        remote = { "remote", "KOReader", "other-device", 0.4, 101, true };
        require(sync.observe(remote, "new-local", "this-device") == Action::Choose
                        && sync.pending(),
                "a different device cannot silently replace local progress");
        require(sync.observe({}, "new-local", "this-device") == Action::Choose,
                "unresolved remote progress blocks all uploads");
        sync.keepLocal();
        require(sync.observe(remote, "new-local", "this-device") == Action::Upload,
                "keeping local explicitly permits replacing the observed remote position");
        remote.timestamp = 102;
        require(sync.observe(remote, "new-local", "this-device") == Action::Choose,
                "a later remote write requires a new choice even at the same remote position");
        sync.acceptRemote("remote");
        require(!sync.needsSync("remote"),
                "accepting remote progress does not trigger an automatic echo");
        require(sync.observe(remote, "remote", "this-device") == Action::Current && !sync.pending(),
                "accepted remote position does not echo back as a new write");
        sync.uploaded("remote");
        require(sync.observe(remote, "later-local", "this-device") == Action::Upload,
                "a completed older upload does not hide a later page turn");
    }
    QTemporaryDir directory;
    require(directory.isValid(), "create profile");
    initializeDiagnostics(directory.path() + "/diagnostics");
    failureChecks(directory.path() + "/failures.json");
    lookupChecks(directory.path() + "/lookup.json");
    const auto path = directory.path() + "/kosync.json";
    QBuffer small;
    small.setData("abc");
    require(small.open(QIODevice::ReadOnly) && small.seek(1), "open digest input");
    require(kosync::documentDigest(small) == "900150983cd24fb0d6963f7d28e17f72" && small.pos() == 1,
            "small-file digest matches KOReader and restores stream position");
    require(kosync::filenameDigest("/books/book.epub") == "03053ffc045564439ff7f2cabb3b58c5",
            "filename identity matches the MD5 of the exact UTF-8 basename");
    require(kosync::filenameDigest("/other/book.epub") == kosync::filenameDigest("book.epub")
                    && kosync::filenameDigest("Book.epub") != kosync::filenameDigest("book.epub")
                    && kosync::filenameDigest("book.kepub") != kosync::filenameDigest("book.epub")
                    && kosync::filenameDigest({}).isEmpty(),
            "filename matching ignores directories but preserves case and extension");
    QByteArray archive(17000, 'x');
    archive.replace(0, 1024, QByteArray(1024, 'a'));
    archive.replace(1024, 1024, QByteArray(1024, 'b'));
    archive.replace(4096, 1024, QByteArray(1024, 'c'));
    archive.replace(16384, 616, QByteArray(616, 'd'));
    QBuffer sampled(&archive);
    require(sampled.open(QIODevice::ReadOnly), "open sampled input");
    const auto expected =
            QCryptographicHash::hash(QByteArray(1024, 'a') + QByteArray(1024, 'b')
                                             + QByteArray(1024, 'c') + QByteArray(616, 'd'),
                                     QCryptographicHash::Md5)
                    .toHex();
    require(kosync::documentDigest(sampled) == expected,
            "sample offsets and partial final block match KOReader");
    archive[3000] = 'z';
    require(kosync::documentDigest(sampled) == expected,
            "unsampled bytes do not alter KOReader identity");
    for (const auto &url :
         { "http://example.com", "https://u:p@example.com", "https://example.com?a=b",
           "https://example.com/#fragment", "https:///missing", "file:///book" })
        require(kosync::serverUrl(url).isEmpty(), "reject unsafe server URL");
    require(kosync::serverUrl(" https://example.com/kosync/// ").toString()
                    == "https://example.com/kosync",
            "custom path prefix is normalized");
    {
        kosync::Progress parsed;
        QJsonObject invalid;
        invalid["progress"] = "/body/p[1]";
        invalid["percentage"] = 1.1;
        invalid["device"] = "reader";
        require(!kosync::parseProgress(invalid, &parsed), "out-of-range percentage is rejected");
        invalid["percentage"] = 0.5;
        require(kosync::parseProgress(invalid, &parsed),
                "older servers may omit device ID and timestamp");
        invalid["timestamp"] = -1;
        require(!kosync::parseProgress(invalid, &parsed), "negative timestamps are rejected");
    }
    Network network;
    kosync::Client client(path, nullptr, &network);
    require(!client.signedIn() && !client.busy() && client.server().isEmpty(),
            "new profile has no server and makes no request");
    {
        const auto blankPath = path + ".blank";
        kosync::Client blank(blankPath);
        require(blank.logout(), "unconfigured sign-out can be saved");
        kosync::Client reloaded(blankPath);
        require(reloaded.server().isEmpty() && !reloaded.signedIn()
                        && reloaded.storageError().isEmpty(),
                "blank signed-out account reloads without a default server");
    }
    QString error;
    int completed = 0;
    auto done = [&](QString value) {
        error = value;
        ++completed;
    };
    network.response = R"({"authorized":"OK"})";
    client.login("https://sync.example/prefix/", "reader", "pässword", done);
    wait(client);
    require(completed == 1 && error.isEmpty() && client.signedIn(),
            "successful authentication persists account");
    const auto key = QCryptographicHash::hash(QString::fromUtf8("pässword").toUtf8(),
                                              QCryptographicHash::Md5)
                             .toHex();
    require(network.last.url().toString() == "https://sync.example/prefix/users/auth"
                    && network.last.rawHeader("x-auth-user") == "reader"
                    && network.last.rawHeader("x-auth-key") == key
                    && network.last.rawHeader("Accept") == "application/vnd.koreader.v1+json",
            "auth uses KOReader headers and UTF-8 MD5");
    QFile account(path);
    require(account.open(QIODevice::ReadOnly), "read saved account");
    const auto accountBytes = account.readAll();
    account.close();
    require(!accountBytes.contains(QString::fromUtf8("pässword").toUtf8()),
            "plaintext password is not stored");
    {
        // A scenario account replaces the saved one in memory and is never written.
        kosync::overrideAccount(QJsonObject{ { "schema", 1 },
                                             { "server", "https://127.0.0.1:9" },
                                             { "username", "eval" },
                                             { "key", QString(32, 'a') },
                                             { "device_id", "eval-device" },
                                             { "automatic", true },
                                             { "sync_interval_minutes", 0 },
                                             { "wake_wifi", true } });
        kosync::Client scenario(path, nullptr, &network);
        require(scenario.signedIn() && scenario.username() == "eval"
                        && scenario.schedule().minutes == 0 && scenario.schedule().wakeWifi,
                "a scenario account is used instead of the saved one");
        require(!scenario.setAutomatic(false), "a scenario account is never saved");
        require(kosync::readingIntervalMilliseconds(scenario.schedule()) == 30000,
                "a scenario account without interval_ms keeps the 30-second check for every page");
        kosync::overrideAccount(QJsonObject{ { "schema", 1 },
                                             { "server", "https://127.0.0.1:9" },
                                             { "username", "eval" },
                                             { "key", QString(32, 'a') },
                                             { "device_id", "eval-device" },
                                             { "automatic", true },
                                             { "sync_interval_minutes", 15 },
                                             { "interval_ms", 1000 } });
        kosync::Client shortened(path, nullptr, &network);
        require(kosync::readingIntervalMilliseconds(shortened.schedule()) == 1000,
                "a scenario account's interval_ms replaces the schedule's interval");
        kosync::overrideAccount({});
        require(kosync::readingIntervalMilliseconds(shortened.schedule()) == 15 * 60000,
                "without a scenario account the schedule decides the interval");
        QFile unchanged(path);
        require(unchanged.open(QIODevice::ReadOnly) && unchanged.readAll() == accountBytes,
                "the saved account is unchanged by a scenario account");
        kosync::Client saved(path, nullptr, &network);
        require(saved.username() == "reader",
                "clearing the scenario account restores the saved one");
    }
    require(!(account.permissions()
              & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)),
            "account permissions exclude group and other users");
    kosync::Client reopened(path, nullptr, &network);
    require(reopened.signedIn() && reopened.deviceId() == client.deviceId(),
            "account and device ID survive restart");
    require(reopened.setAutomatic(false) && !reopened.automatic(),
            "automatic sync can be disabled");
    require(reopened.setAutomatic(true) && !kosync::automaticPaused(),
            "automatic sync is not paused by default");
    kosync::pauseAutomatic(true);
    require(kosync::automaticPaused() && reopened.automatic(),
            "a paused run keeps the saved automatic setting");
    kosync::pauseAutomatic(false);
    require(!kosync::automaticPaused(), "automatic sync resumes after a paused run");
    require(reopened.setAutomatic(false) && !reopened.automatic(),
            "automatic sync can be disabled again");
    require(reopened.manualExplanation(), "older accounts explain manual sync on first use");
    require(reopened.setManualExplanation(false), "manual sync explanation can be acknowledged");
    kosync::Client explained(path);
    require(!explained.manualExplanation() && !explained.automatic() && explained.signedIn(),
            "manual sync acknowledgment survives restart without changing sync mode or account");
    require(explained.setDeviceName("Manual reader") && explained.setAutomatic(true),
            "other account edits succeed");
    require(!kosync::Client(path).manualExplanation(),
            "account edits preserve the manual sync preference");
    {
        // The schedule is optional. Accounts without one sync every 15
        // minutes and never turn on Wi-Fi for it; invalid values use the
        // same defaults and do not block writes.
        const auto schedule = kosync::Client(path).schedule();
        require(schedule.minutes == 15 && !schedule.wakeWifi,
                "accounts without a schedule use every 15 minutes");
        require(!explained.setSchedule({ 7, true }) && explained.schedule() == schedule,
                "an unsupported interval is refused");
        require(explained.setSchedule({ 0, true }), "a schedule can be saved");
        const auto saved = kosync::Client(path).schedule();
        require(saved.minutes == 0 && saved.wakeWifi, "the schedule survives restart");
        require(explained.setDeviceName("Manual reader")
                        && kosync::Client(path).schedule() == saved,
                "account edits preserve the schedule");
        QFile stored(path);
        require(stored.open(QIODevice::ReadOnly), "fixture reads the account");
        auto object = QJsonDocument::fromJson(stored.readAll()).object();
        stored.close();
        object["sync_interval_minutes"] = 7;
        object["wake_wifi"] = "yes";
        require(stored.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && stored.write(QJsonDocument(object).toJson()) > 0,
                "fixture writes invalid schedule values");
        stored.close();
        kosync::Client invalid(path);
        require(invalid.schedule() == kosync::Schedule{} && invalid.storageError().isEmpty()
                        && invalid.setAutomatic(true),
                "invalid schedule values use the defaults and keep the account writable");
        require(invalid.setSchedule({ 15, false })
                        && kosync::Client(path).schedule() == kosync::Schedule{},
                "the default schedule can be saved again");
    }
    require(!client.lastSync().isValid(), "login alone does not count as a progress sync");
    require(client.recordSyncSuccess(), "successful progress exchange records a timestamp");
    const auto synced = client.lastSync();
    require(!kosync::Client(path).lastSync().isValid(), "sync time stays in memory while reading");
    require(client.persistSyncTime(), "closing persists the informational timestamp");
    require(kosync::Client(path).customDeviceName() == "Manual reader"
                    && !kosync::Client(path).manualExplanation(),
            "timestamp persistence preserves settings written by another client");
    kosync::Client syncedReload(path);
    require(synced.isValid()
                    && syncedReload.lastSync().toSecsSinceEpoch() == synced.toSecsSinceEpoch(),
            "last successful sync survives restart");
    require(client.setAutomatic(true) && client.lastSync() == synced,
            "preference changes retain last sync");
    client.setDefaultDeviceName("Kobo Clara BW (Kobalt)");
    require(client.deviceName() == "Kobo Clara BW (Kobalt)",
            "accounts without an override use the detected default");
    const auto originalDeviceId = client.deviceId();
    require(client.setDeviceName(QString::fromUtf8("  Nico’s Kobo  "))
                    && client.deviceName() == QString::fromUtf8("Nico’s Kobo"),
            "custom device names preserve Unicode and trim surrounding space");
    kosync::Client renamed(path);
    require(renamed.deviceName() == QString::fromUtf8("Nico’s Kobo")
                    && renamed.deviceId() == originalDeviceId
                    && renamed.lastSync().toSecsSinceEpoch() == synced.toSecsSinceEpoch(),
            "renaming persists without changing identity or last sync");
    require(!client.setDeviceName(QString(129, 'x')) && !client.setDeviceName("bad\nname")
                    && client.deviceName() == QString::fromUtf8("Nico’s Kobo"),
            "invalid names do not replace the saved name");
    {
        const auto separate = directory.path() + "/deferred-timestamp.json";
        QFile source(path);
        require(source.open(QIODevice::ReadOnly), "read deferred account fixture");
        save(separate, source.readAll());
        kosync::Client stale(separate), current(separate);
        require(stale.recordSyncSuccess() && current.setFilenameMatching(true),
                "change matching before deferred save");
        require(!stale.persistSyncTime() && !kosync::Client(separate).lastSync().isValid(),
                "a stale timestamp cannot restore a date cleared by a matching change");
        current.logout();
        require(!stale.persistSyncTime() && !kosync::Client(separate).signedIn(),
                "a deferred date cannot recreate a removed account");
    }
    const auto digest = QByteArray("900150983cd24fb0d6963f7d28e17f72");
    kosync::Progress received;
    auto pulled = [&](kosync::Progress value, QString failure) {
        received = value;
        error = failure;
        ++completed;
    };
    const kosync::Progress local{ "/body/DocFragment[2]/body/p[3]/text().0", {}, {}, 0.42 };
    for (const auto &response :
         { std::make_pair(200, QByteArray("{}")), std::make_pair(200, QByteArray("[]")),
           std::make_pair(200, QByteArray(" \n[ \n ]\t")),
           std::make_pair(404, QByteArray(R"({"status":"not found"})")) }) {
        network.status = response.first;
        network.response = response.second;
        network.failures = network.status == 404 ? 1 : 0;
        network.transportError = QNetworkReply::ContentNotFoundError;
        const int before = network.calls;
        bool uploaded = false;
        client.pull(digest, [&](kosync::Progress remote, QString failure) {
            pulled(remote, failure);
            if (!failure.isEmpty())
                return;
            kosync::Reconciler sync;
            if (sync.observe(remote, local.position, client.deviceId())
                != kosync::Reconciler::Action::Upload)
                return;
            network.status = 200;
            network.response = "{\"document\":\"" + digest + "\",\"timestamp\":124}";
            client.push(digest, local, [&](QString failure) {
                uploaded = failure.isEmpty();
                done(failure);
            });
        });
        wait(client);
        require(error.isEmpty() && !received.found && uploaded && network.calls == before + 2
                        && network.operation == QNetworkAccessManager::PutOperation,
                "empty progress or the structured missing-progress 404 permits the first upload");
    }
    const auto uploadedProgress = network.payload;
    {
        Network peerNetwork;
        peerNetwork.response = R"({"authorized":"OK"})";
        kosync::Client peer(path + ".peer", nullptr, &peerNetwork);
        peer.login("https://sync.example/prefix", "reader", "secret", done);
        wait(peer);
        require(error.isEmpty() && peer.deviceId() != client.deviceId(),
                "a second reader has its own device identity");
        peerNetwork.response = uploadedProgress;
        peer.pull(digest, pulled);
        wait(peer);
        require(error.isEmpty() && received.found && received.position == local.position
                        && received.deviceId == client.deviceId(),
                "a second reader retrieves the first uploaded position unchanged");
        kosync::Reconciler sync;
        require(sync.observe(received, "different-local", peer.deviceId())
                        == kosync::Reconciler::Action::Choose,
                "progress created after a 404 still requires a remote conflict choice");
        peerNetwork.status = 404;
        peerNetwork.response = R"({"status":"not found"})";
        peer.pull(digest, pulled);
        wait(peer);
        require(error.isEmpty() && !received.found
                        && sync.observe(received, "different-local", peer.deviceId())
                                == kosync::Reconciler::Action::Choose,
                "a missing-progress response without a Qt error cannot clear a pending conflict");
    }
    network.status = 404;
    for (const auto &body :
         { QByteArray("{}"), QByteArray("[]"), QByteArray("not found"),
           QByteArray("<html>Not Found</html>"), QByteArray(R"({"error":"not found"})"),
           QByteArray(R"({"status":"Not Found"})"),
           QByteArray(R"({"status":"not found","progress":"private"})"),
           QByteArray(R"({"status":"not found"})") + QByteArray(17000, ' '),
           QByteArray(R"({"status":"not found"} trailing)") }) {
        network.response = body;
        const int before = network.calls;
        client.pull(digest, pulled);
        wait(client);
        require(!error.isEmpty() && !received.found && network.calls == before + 1,
                "an unrecognized or oversized 404 remains a failed read without retries");
    }
    network.response = R"({"status":"not found"})";
    client.login("https://another.example", "reader", "secret", done);
    wait(client);
    require(!error.isEmpty(), "missing-progress JSON cannot authenticate an account");
    client.push(digest, local, done);
    wait(client);
    require(!error.isEmpty(), "missing-progress JSON cannot acknowledge an upload");
    network.failures = 1;
    network.transportError = QNetworkReply::SslHandshakeFailedError;
    client.pull(digest, pulled);
    wait(client);
    require(!error.isEmpty(), "a transport failure cannot become missing progress");
    network.status = 200;
    network.response =
            R"({"progress":"/body/DocFragment[2]/body/p[3]/text().0","percentage":0.42,"device":"KOReader","device_id":"other","timestamp":123})";
    client.pull(digest, pulled);
    wait(client);
    require(error.isEmpty() && received.found && received.percentage == 0.42
                    && received.deviceId == "other",
            "KOReader progress parses without changing its XPointer");
    network.response = "{\"document\":\"" + digest + "\",\"timestamp\":124}";
    client.push(digest, received, done);
    wait(client);
    const auto payload = QJsonDocument::fromJson(network.payload).object();
    require(error.isEmpty() && network.operation == QNetworkAccessManager::PutOperation
                    && payload.value("progress").toString() == received.position
                    && payload.value("document").toString() == digest
                    && payload.value("device_id").toString() == client.deviceId()
                    && payload.value("percentage").toDouble() == 0.42
                    && payload.value("device").toString() == QString::fromUtf8("Nico’s Kobo"),
            "upload uses stable document and device identity with precise position");
    for (const auto &body :
         { QByteArray("[{}]"), QByteArray("[null]"), QByteArray("null"), QByteArray("oops"),
           QByteArray(R"({"percentage":0.5})"), QByteArray(17000, 'x') }) {
        network.response = body;
        client.pull(digest, pulled);
        wait(client);
        require(!error.isEmpty() && !received.found,
                "malformed or oversized responses cannot supply progress");
    }
    network.response = "[]";
    client.login("https://another.example", "reader", "secret", done);
    wait(client);
    require(error == strings::get("kosync.invalid_response"),
            "an empty array cannot authenticate an account");
    client.push(digest, local, done);
    wait(client);
    require(error == strings::get("kosync.invalid_response"),
            "an empty array cannot acknowledge an upload");
    for (const int status : { 204, 301, 401, 404, 500, 502 }) {
        network.status = status;
        const int before = network.calls;
        client.pull(digest, pulled);
        wait(client);
        require(!error.isEmpty() && network.calls == before + 1,
                "errors do not redirect or retry credentials");
    }
    network.status = 401;
    client.login("https://another.example", "wrong", "wrong", done);
    wait(client);
    require(!error.isEmpty() && client.server() == "https://sync.example/prefix"
                    && client.username() == "reader",
            "failed replacement login retains the existing account");
    network.status = 200;
    const int beforeCancel = completed;
    client.pull(digest, pulled);
    client.cancel();
    QCoreApplication::processEvents();
    require(!client.busy() && completed == beforeCancel, "cancellation suppresses stale callbacks");
    require(client.lastSync() == synced, "failed requests and cancellation retain last sync");
    require(client.setDeviceName({}) && client.deviceName() == "Kobo Clara BW (Kobalt)",
            "clearing a name restores the platform default");
    kosync::Client resetName(path);
    resetName.setDefaultDeviceName("Kobo Clara BW (Kobalt)");
    require(resetName.customDeviceName().isEmpty() && resetName.deviceName() == client.deviceName(),
            "automatic naming survives restart");
    network.response = R"({"authorized":"OK"})";
    client.login("https://another.example", "other-reader", "test-password", done);
    wait(client);
    require(error.isEmpty() && !client.lastSync().isValid(),
            "a different account clears the old last sync");
    require(client.logout() && !client.signedIn(), "sign out clears the saved key");
    kosync::Client signedOut(path, nullptr, &network);
    require(!signedOut.signedIn(), "sign out survives restart");
    {
        const auto matchingPath = path + ".matching";
        auto legacy = QJsonDocument::fromJson(accountBytes).object();
        legacy.remove("match_method");
        legacy["extra"] = "preserve";
        save(matchingPath, QJsonDocument(legacy).toJson());
        kosync::Client matching(matchingPath, nullptr, &network);
        require(!matching.filenameMatching(), "existing profiles default to binary matching");
        const auto deviceId = matching.deviceId();
        require(matching.recordSyncSuccess() && matching.setFilenameMatching(true)
                        && matching.filenameMatching() && !matching.lastSync().isValid(),
                "switching matching method clears the old document's last-sync timestamp");
        require(matching.setDeviceName("Matching reader") && matching.setAutomatic(false),
                "edit filename-mode preferences");
        kosync::Client restored(matchingPath, nullptr, &network);
        require(restored.filenameMatching() && restored.signedIn()
                        && restored.deviceId() == deviceId && restored.username() == "reader"
                        && restored.server() == "https://sync.example/prefix"
                        && restored.customDeviceName() == "Matching reader" && !restored.automatic()
                        && !restored.lastSync().isValid(),
                "matching preference survives restart and other edits without changing account identity");
        network.response = "{}";
        matching.pull(digest, pulled);
        require(!matching.setFilenameMatching(false) && matching.filenameMatching(),
                "in-flight exchanges block matching changes");
        wait(matching);
        network.response = R"({"authorized":"OK"})";
        matching.login("https://sync.example/prefix", "reader", "pässword", done);
        wait(matching);
        require(error.isEmpty() && matching.filenameMatching(),
                "authentication preserves matching preference");
        require(matching.setFilenameMatching(false) && !matching.filenameMatching(),
                "binary matching can be restored");
        QFile stored(matchingPath);
        require(stored.open(QIODevice::ReadOnly), "read matching preference");
        const auto bytes = stored.readAll();
        stored.close();
        auto unsupported = QJsonDocument::fromJson(bytes).object();
        require(unsupported.value("extra").toString() == "preserve",
                "matching edits preserve unknown optional fields");
        unsupported["match_method"] = "future";
        const auto futureBytes = QJsonDocument(unsupported).toJson();
        save(matchingPath, futureBytes);
        require(!matching.setAutomatic(true),
                "a matching method changed externally also blocks existing clients from overwriting it");
        kosync::Client invalid(matchingPath);
        require(!invalid.storageError().isEmpty() && !invalid.setFilenameMatching(true),
                "unsupported matching method blocks writes");
        require(stored.open(QIODevice::ReadOnly) && stored.readAll() == futureBytes,
                "unsupported matching preference remains untouched");
    }
    {
        Network retryNetwork;
        retryNetwork.response = R"({"authorized":"OK"})";
        retryNetwork.failures = 1;
        kosync::Client retrying(path + ".retry", nullptr, &retryNetwork);
        retrying.login("https://retry.example", "reader", "test-password", done);
        wait(retrying, 3000);
        require(error.isEmpty() && retrying.signedIn() && retryNetwork.calls == 2,
                "login recovers from a transient connection failure without re-entering credentials");
        require(retrying.recordSyncSuccess(), "record a baseline before retry checks");
        const auto lastSuccess = retrying.lastSync();
        retryNetwork.response = "{}";
        retryNetwork.failures = 10;
        int before = retryNetwork.calls;
        retrying.pull(digest, pulled);
        wait(retrying, 8000);
        require(!error.isEmpty() && retryNetwork.calls == before + 4
                        && retrying.lastSync() == lastSuccess,
                "persistent offline failures stop after bounded read retries without advancing last sync");
        retryNetwork.failures = 1;
        retryNetwork.transportError = QNetworkReply::SslHandshakeFailedError;
        before = retryNetwork.calls;
        retrying.pull(digest, pulled);
        wait(retrying);
        require(!error.isEmpty() && retryNetwork.calls == before + 1,
                "TLS failures are not retried");
        retryNetwork.failures = 0;
        for (const int status : { 401, 404, 500 }) {
            retryNetwork.status = status;
            before = retryNetwork.calls;
            retrying.pull(digest, pulled);
            wait(retrying);
            require(!error.isEmpty() && retryNetwork.calls == before + 1,
                    "HTTP failures are not retried");
        }
        retryNetwork.status = 200;
        retryNetwork.transportError = QNetworkReply::ConnectionRefusedError;
        retryNetwork.failures = 1;
        before = retryNetwork.calls;
        retrying.push(digest, { "local", {}, {}, 0.5 }, done);
        wait(retrying);
        require(!error.isEmpty() && retryNetwork.calls == before + 1,
                "uploads are never retried after an ambiguous transport failure");
        retryNetwork.failures = 1;
        before = retryNetwork.calls;
        retrying.pull(digest, pulled, false);
        wait(retrying);
        require(!error.isEmpty() && retryNetwork.calls == before + 1,
                "background and final reads can opt out of retries");
        retryNetwork.failures = 1;
        before = retryNetwork.calls;
        int replies = 0;
        retrying.pull(digest, [&](kosync::Progress, QString) { ++replies; });
        QEventLoop firstFailure;
        QTimer::singleShot(10, &firstFailure, &QEventLoop::quit);
        firstFailure.exec();
        require(!retryNetwork.lastReply || retryNetwork.lastReply->isFinished(),
                "the first transport failure was delivered before cancellation");
        require(retrying.busy() && !retrying.setFilenameMatching(true),
                "pending retry remains busy and blocks identity changes");
        retrying.cancel();
        require(!retrying.busy(), "cancel clears a scheduled retry immediately");
        QEventLoop cancelled;
        QTimer::singleShot(700, &cancelled, &QEventLoop::quit);
        cancelled.exec();
        require(replies == 0 && retryNetwork.calls == before + 1,
                "cancel suppresses the delayed request and its callback");
        retryNetwork.failures = 0;
        retrying.pull(digest, [&](kosync::Progress, QString failure) {
            require(failure.isEmpty(), "retry cancellation permits a fresh sync");
            ++replies;
        });
        wait(retrying);
        require(replies == 1, "a later sync finishes exactly once");
    }
    save(path, R"({"schema":99,"private":"preserve"})");
    require(!reopened.setAutomatic(true),
            "newer account schema blocks writes even after client construction");
    const bool explanation = reopened.manualExplanation();
    require(!reopened.setManualExplanation(!explanation)
                    && reopened.manualExplanation() == explanation,
            "a rejected preference write preserves the explanation in memory");
    kosync::Client future(path, nullptr, &network);
    require(!future.signedIn() && !future.logout(), "unsupported saved account is not overwritten");
    return 0;
}
