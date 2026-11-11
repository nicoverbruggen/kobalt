#include "sync/sync_coordinator.h"
#include "support/strings.h"
#include "check.h"
#include "kosync_reply.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

using namespace kobalt;
using Coordinator = kosync::SyncCoordinator;

namespace {
void await(const std::function<bool()> &condition)
{
    if (condition())
        return;
    QEventLoop loop;
    QTimer poll, deadline;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (condition())
            loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop,
                     [] { require(false, "Coordinator did not reach the expected state"); });
    poll.start(1);
    deadline.start(3000);
    loop.exec();
}

class Http final : public QNetworkAccessManager
{
public:
    QVector<QPointer<test::KosyncReply>> replies;
    QVector<QJsonObject> uploads;

    void finish(int request)
    {
        await([&] { return replies.size() > request; });
        require(bool(replies[request]), "Held reply was destroyed before completion");
        replies[request]->finish();
    }

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *data) override
    {
        QByteArray response = "{}";
        if (operation == PutOperation) {
            const auto upload = QJsonDocument::fromJson(data->readAll()).object();
            uploads.append(upload);
            response = QJsonDocument(QJsonObject{ { "document", upload.value("document") } })
                               .toJson(QJsonDocument::Compact);
        }
        auto *reply =
                new test::KosyncReply(request, response, 200, QNetworkReply::NoError, this, true);
        replies.append(reply);
        return reply;
    }
};

struct RadioState
{
    std::function<void(bool)> answer;
    bool connected = false;
    int prepared = 0, cancelled = 0, released = 0;
};

class Radio final : public SyncNetwork
{
public:
    explicit Radio(std::shared_ptr<RadioState> state) : state(std::move(state)) { }

    bool connected() const override { return state->connected; }

    void prepare(bool, std::function<void(bool)> done) override
    {
        ++state->prepared;
        state->answer = std::move(done);
    }

    // Keep the answer to exercise a host completion already queued at cancellation.
    void cancel() override { ++state->cancelled; }

    void release() override { ++state->released; }

private:
    std::shared_ptr<RadioState> state;
};

Coordinator::Context context()
{
    Coordinator::Context result;
    result.document = QByteArray(32, 'b');
    result.local.position = "chapter-one";
    result.local.percentage = .2;
    return result;
}

void lateConnection(const QString &path)
{
    Http http;
    kosync::Client client(path, nullptr, &http);
    const auto radio = std::make_shared<RadioState>();
    int events = 0;
    auto owner = std::make_unique<Coordinator>(
            client, [radio] { return std::make_unique<Radio>(radio); }, context,
            [&](const Coordinator::Event &) { ++events; }, true);
    owner->start(Coordinator::Connection::Background, true);
    require(radio->prepared == 1 && owner->busy(), "Connection did not belong to the attempt");
    const auto first = radio->answer;
    require(!owner->start(Coordinator::Connection::Existing, false)
                    && !owner->login("https://127.0.0.1:9", "fixture", "fixture", {})
                    && radio->prepared == 1 && http.replies.isEmpty(),
            "A second operation replaced an attempt waiting for its connection");
    owner->cancel();
    const int cancelledEvents = events;
    first(true);
    require(!owner->busy() && http.replies.isEmpty() && events == cancelledEvents
                    && radio->released == 1,
            "A connection answer after cancellation resumed the old attempt");
    owner->start(Coordinator::Connection::Background, true);
    const auto second = radio->answer;
    owner.reset();
    const int destroyedEvents = events;
    second(true);
    require(http.replies.isEmpty() && events == destroyedEvents && radio->released == 2,
            "A connection answer outlived its coordinator");
}

void waitingRead(const QString &path)
{
    Http http;
    kosync::Client client(path, nullptr, &http);
    auto current = context();
    int completed = 0;
    Coordinator owner(
            client, {}, [&] { return current; },
            [&](const Coordinator::Event &event) {
                if (event.kind == Coordinator::EventKind::Completed)
                    ++completed;
            },
            false);
    owner.start(Coordinator::Connection::Existing, false);
    current.readiness = Coordinator::Readiness::Hidden;
    http.finish(0);
    require(owner.busy() && http.uploads.isEmpty(),
            "Hidden reading state accepted a fetched position");
    for (const auto *timer : owner.findChildren<QTimer *>())
        require(!timer->isActive() || timer->isSingleShot(), "Hidden reading state kept polling");
    current.local.position = "chapter-two";
    current.local.percentage = .6;
    current.readiness = Coordinator::Readiness::Ready;
    owner.resume();
    await([&] { return http.uploads.size() == 1; });
    require(http.uploads[0].value("progress") == "chapter-two" && http.replies.size() == 2,
            "A retained read repeated HTTP or uploaded the old local position");
    http.finish(1);
    require(completed == 1 && !owner.busy() && !owner.needsSync("chapter-two"),
            "Settled reading state did not complete its retained exchange");
}

void observerLifetime(const QString &path)
{
    Http http;
    kosync::Client client(path, nullptr, &http);
    const auto radio = std::make_shared<RadioState>();
    std::unique_ptr<Coordinator> owner;
    owner = std::make_unique<Coordinator>(
            client, [radio] { return std::make_unique<Radio>(radio); }, context,
            [&](const Coordinator::Event &event) {
                if (event.kind == Coordinator::EventKind::Status)
                    owner->cancel();
            },
            true);
    require(owner->start(Coordinator::Connection::Background, true), "Attempt was not accepted");
    require(radio->prepared == 0 && http.replies.isEmpty() && !owner->busy(),
            "Cancellation from a status observer still prepared the connection");
    owner.reset();

    int events = 0;
    owner = std::make_unique<Coordinator>(
            client, SyncNetworkFactory{}, context, [&](const Coordinator::Event &) { ++events; },
            false);
    owner->start(Coordinator::Connection::Existing, false);
    await([&] { return http.replies.size() == 1; });
    const auto pending = http.replies[0];
    const int before = events;
    owner.reset();
    require(!client.busy() && pending->isFinished() && events == before,
            "Destroying a coordinator left its HTTP request or observer active");

    for (const bool destroy : { false, true }) {
        owner = std::make_unique<Coordinator>(
                client, SyncNetworkFactory{}, context,
                [&](const Coordinator::Event &event) {
                    if (event.kind == Coordinator::EventKind::Completed) {
                        if (destroy)
                            owner.reset();
                        else
                            owner->cancel();
                    }
                },
                false);
        const int first = http.replies.size();
        int finished = 0;
        owner->flush(true, [&] { ++finished; });
        http.finish(first);
        await([&] { return http.replies.size() == first + 2; });
        http.finish(first + 1);
        require((destroy ? !owner : !owner->busy()) && finished == 0 && !client.busy(),
                "Final completion called back after its observer cancelled or destroyed it");
        owner.reset();
    }
}

void finalUpload(const QString &path, bool timeout)
{
    Http http;
    kosync::Client client(path, nullptr, &http);
    const auto radio = std::make_shared<RadioState>();
    auto current = context();
    QVector<Coordinator::Event> completions;
    Coordinator owner(
            client, [radio] { return std::make_unique<Radio>(radio); }, [&] { return current; },
            [&](const Coordinator::Event &event) {
                if (event.kind == Coordinator::EventKind::Completed)
                    completions.append(event);
            },
            true);
    owner.start(Coordinator::Connection::Background, true);
    radio->connected = true;
    radio->answer(true);
    await([&] { return http.replies.size() == 1; });
    const auto originalRead = http.replies[0];
    radio->answer(true);
    require(http.replies.size() == 1 && !originalRead->isFinished(),
            "A repeated connection answer replaced the active read");
    http.finish(0);
    await([&] { return http.uploads.size() == 1; });
    const auto oldUpload = http.replies[1];
    current.local.position = "last-position";
    current.local.percentage = .9;
    current.readiness = Coordinator::Readiness::Hidden;
    current.queueSyncTime = true;
    int done = 0;
    owner.flush(true, [&] { ++done; });
    auto *deadline = owner.findChild<QTimer *>("rrKosyncDeadline");
    require(deadline && deadline->isActive() && deadline->interval() == 2000 && owner.finalPending()
                    && !oldUpload->isFinished() && http.replies.size() == 2 && done == 0
                    && radio->released == 0,
            "Final handoff aborted or overlapped the older upload");
    if (timeout) {
        deadline->stop();
        QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
        require(done == 1 && !owner.finalPending() && !owner.busy() && http.replies.size() == 2
                        && oldUpload->isFinished() && radio->released == 1,
                "Final deadline failed to cancel the old upload and release its connection");
        require(completions.size() == 1 && completions[0].finalUpload
                        && completions[0].message == strings::get("kosync.timeout_error"),
                "Final deadline lost its outcome");
        return;
    }
    http.finish(1);
    await([&] { return http.replies.size() == 3; });
    require(done == 0 && completions.isEmpty() && radio->released == 0,
            "Older upload completed the final handoff too early");
    http.finish(2);
    await([&] { return http.uploads.size() == 2; });
    require(http.uploads[1].value("progress") == "last-position",
            "Final upload did not use the handoff position");
    http.finish(3);
    require(done == 1 && !owner.busy() && !owner.finalPending() && radio->released == 1
                    && completions.size() == 1 && completions[0].finalUpload
                    && !owner.needsSync("last-position"),
            "Final handoff did not complete exactly once");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "Coordinator fixture directory unavailable");
    kosync::overrideAccount(QJsonObject{ { "schema", 1 },
                                         { "server", "https://127.0.0.1:9" },
                                         { "username", "fixture" },
                                         { "key", QString(32, 'a') },
                                         { "device_id", "fixture-device" },
                                         { "automatic", true } });
    const auto path = directory.filePath("account.json");
    lateConnection(path);
    waitingRead(path);
    observerLifetime(path);
    finalUpload(path, false);
    finalUpload(path, true);
    kosync::overrideAccount({});
    std::puts(
            "PASS: book-scoped sync attempts own late replies, readiness and final upload handoffs");
}
