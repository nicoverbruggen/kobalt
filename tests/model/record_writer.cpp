#include "storage/record_writer.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include <QFile>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <time.h>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include "check.h"

using kobalt::storage::RecordWriter;

namespace {
int rejectedStarts = 0;
std::atomic_bool shiftNextWallTime{ false };
} // namespace

namespace kobalt::storage::test {
// Simulate a wall-clock correction between reading the time and starting the wait.
void adjustWriterTime(clockid_t clock, timespec &now)
{
    if (clock == CLOCK_REALTIME && shiftNextWallTime.exchange(false))
        now.tv_sec -= 60;
}

void beforeWriterStart()
{
    if (rejectedStarts > 0) {
        --rejectedStarts;
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
}
} // namespace kobalt::storage::test

namespace {
// Holds the writer thread inside a write until the test opens it.
class Gate
{
public:
    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this] { return open; });
    }

    void awaitEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, open = false;
};

// Records the order in which writes run on the writer thread.
class Log
{
public:
    void add(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries += name + ' ';
    }

    std::string text()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return entries;
    }

private:
    std::mutex mutex;
    std::string entries;
};

RecordWriter::Write record(Log &log, const std::string &name, bool result = true)
{
    return [&log, name, result] {
        log.add(name);
        return result;
    };
}
void checkLaunchFailure()
{
    using namespace kobalt::storage;
    rejectedStarts = 5;
    int calls = 0;
    require(!tryRunOnWriterThread([&] { ++calls; }), "Rejected call ran");
    {
        RecordWriter writer;
        require(!writer.tryPost([&] {
                    ++calls;
                    return true;
                }),
                "Rejected write was queued");
        require(writer.idle(), "Unstarted writer kept a pending write");
    }

    QTemporaryDir profile;
    require(profile.isValid(), "Cannot create startup profile fixture");
    const Paths paths{ profile.path() };
    const auto original = QByteArrayLiteral(R"({"dark_mode":true})");
    {
        QFile preferences(paths.preferences());
        require(preferences.open(QIODevice::WriteOnly)
                        && preferences.write(original) == original.size(),
                "Cannot seed legacy preferences");
    }
    const auto normalized = readPreferences(paths.preferences());
    require(normalized.status == ReadStatus::Ok && normalized.object.value("dark_mode").toBool()
                    && normalized.object.value("schema").toInt() == 1,
            "Rejected cleanup lost normalized preferences");
    {
        QFile preferences(paths.preferences());
        require(preferences.open(QIODevice::ReadOnly) && preferences.readAll() == original,
                "Rejected cleanup changed the legacy file");
    }
    {
        QFile database(paths.database());
        require(database.open(QIODevice::WriteOnly), "Cannot create database placeholder");
    }
    QString error;
    require(inspectProfileDatabase(paths.directory, &error) == ProfileDatabaseState::Unavailable
                    && error == "profile-writer-unavailable",
            "Rejected inspection did not report unavailable storage");

    try {
        runOnWriterThread([&] { ++calls; });
        require(false, "Required call hid its launch error");
    } catch (const std::system_error &error) {
        require(error.code() == std::errc::resource_unavailable_try_again,
                "Required call changed its launch error");
    }
    require(rejectedStarts == 0 && calls == 0, "A rejected operation ran");

    RecordWriter writer;
    Gate firstRequest;
    std::atomic_bool blocked{ false };
    // A first submission races the USB gate. The worker must see the gate
    // set by the unstarted drain before it executes the accepted operation.
    std::thread submit([&] {
        firstRequest.awaitEntered();
        firstRequest.release();
        require(writer.tryPost([&] {
                    require(onWriterThread(), "Retried write used the submitting thread");
                    require(blocked.load(), "First operation ran before the export gate was set");
                    ++calls;
                    return true;
                }),
                "Rejected startup could not retry");
    });
    require(!drainWriterThread([&] {
                firstRequest.wait();
                blocked = true;
            }),
            "Unstarted drain reported a worker");
    submit.join();
    require(writer.flush() && calls == 1, "Retried startup ran lost or rejected work");
    try {
        tryRunOnWriterThread([] { throw std::system_error(std::make_error_code(std::errc::io_error)); });
        require(false, "Accepted operation hid its own exception");
    } catch (const std::system_error &error) {
        require(error.code() == std::errc::io_error, "Accepted operation changed its exception");
    }
    readPreferences(paths.preferences());
    QFile preferences(paths.preferences());
    require(preferences.open(QIODevice::ReadOnly) && preferences.readAll() != original,
            "Accepted cleanup did not persist normalized preferences");
}
} // namespace

// Checks record writers: queue order, replaced waiting writes, synchronous writes, cancellation, destruction and time limits.
int main()
{
    checkLaunchFailure();
    {
        // Writers share one thread. Writes run in the order their writers
        // were queued; a writer posting again while it runs goes to the back.
        Log log;
        Gate gate;
        RecordWriter first, second;
        first.post(
                [&] {
                    gate.wait();
                    log.add("a1");
                    return true;
                },
                {});
        gate.awaitEntered();
        second.post(record(log, "b1"), {});
        first.post(record(log, "a2"), {});
        require(!first.idle() && !second.idle(), "Queued writers reported idle");
        shiftNextWallTime = true;
        const auto started = std::chrono::steady_clock::now();
        require(!first.flush(50), "A flush finished while its write was held");
        require(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(50),
                "A wall-clock adjustment shortened the writer flush timeout");
        shiftNextWallTime = false;
        gate.release();
        require(first.flush() && second.flush(), "Writers did not drain");
        require(log.text() == "a1 b1 a2 ", "Writes ran out of queue order");
    }
    {
        // A new write replaces one that has not started. Only the newest
        // write runs and reports its result.
        Log log;
        Gate gate;
        RecordWriter busy, saver;
        busy.post(
                [&] {
                    gate.wait();
                    return true;
                },
                {});
        gate.awaitEntered();
        std::atomic_int reports{ 0 }, newestResult{ -1 };
        saver.post(record(log, "old"), [&](bool) { ++reports; });
        saver.post(record(log, "new", false), [&](bool saved) {
            ++reports;
            newestResult = saved ? 1 : 0;
        });
        gate.release();
        saver.flush();
        require(log.text() == "new ", "A replaced write still ran");
        require(reports == 1 && newestResult == 0,
                "The newest write did not report its own result");
    }
    {
        // An ordered writer keeps independent edits and their outcomes.
        Log log;
        Gate gate;
        RecordWriter blocker, edits(RecordWriter::Delivery::All);
        blocker.post(
                [&] {
                    gate.wait();
                    return true;
                },
                {});
        gate.awaitEntered();
        edits.post(record(log, "zen", false),
                   [&](bool ok) { log.add(ok ? "unexpected" : "failed"); });
        edits.post(record(log, "dark"), [&](bool ok) { log.add(ok ? "saved" : "unexpected"); });
        gate.release();
        require(edits.writeNow(record(log, "apply")), "Cannot apply settings");
        require(log.text() == "zen failed dark saved apply ",
                "Applying settings lost a queued edit or its completion");
    }
    {
        // writeNow replaces a waiting write, runs on the writer thread and
        // returns that write's result.
        Log log;
        Gate gate;
        RecordWriter busy, saver;
        busy.post(
                [&] {
                    gate.wait();
                    return true;
                },
                {});
        gate.awaitEntered();
        saver.post(record(log, "queued"), {});
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.release();
        });
        std::thread::id ran;
        const bool saved = saver.writeNow([&] {
            ran = std::this_thread::get_id();
            log.add("now");
            return false;
        });
        release.join();
        require(!saved, "writeNow did not return the write's result");
        require(ran != std::this_thread::get_id(), "writeNow ran on the calling thread");
        require(log.text() == "now ", "writeNow did not replace the waiting write");
    }
    {
        // cancel drops a waiting write and waits for a running one.
        Log log;
        Gate gate;
        RecordWriter saver;
        std::atomic_bool finished{ false };
        saver.post(
                [&] {
                    gate.wait();
                    finished = true;
                    return true;
                },
                {});
        gate.awaitEntered();
        saver.post(record(log, "dropped"), {});
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.release();
        });
        saver.cancel();
        require(finished, "cancel returned before the running write finished");
        release.join();
        saver.flush();
        require(log.text().empty(), "A cancelled write ran");
    }
    {
        // Destruction finishes a waiting write. A writer cancelled while
        // still queued leaves no dangling entry for the thread.
        Log log;
        Gate gate;
        auto busy = std::make_unique<RecordWriter>();
        busy->post(
                [&] {
                    gate.wait();
                    return true;
                },
                {});
        gate.awaitEntered();
        auto cancelled = std::make_unique<RecordWriter>();
        cancelled->post(record(log, "cancelled"), {});
        auto pending = std::make_unique<RecordWriter>();
        pending->post(record(log, "kept"), {});
        std::thread([&] { cancelled->cancel(); }).join();
        cancelled.reset();
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.release();
        });
        pending.reset();
        release.join();
        busy.reset();
        require(log.text() == "kept ", "Destruction lost or ran the wrong writes");
    }
    {
        // A write that throws counts as failed and leaves the writer usable.
        Log log;
        RecordWriter saver;
        std::atomic_int result{ -1 };
        saver.post([]() -> bool { throw 1; }, [&](bool saved) { result = saved ? 1 : 0; });
        saver.flush();
        require(result == 0, "A throwing write did not report a failure");
        require(saver.writeNow(record(log, "after")) && log.text() == "after ",
                "A throwing write stopped the writer");
    }
    {
        // A call runs before waiting background writes, on the writer thread,
        // and a call made on that thread runs at once.
        Log log;
        Gate gate;
        RecordWriter busy, saver;
        busy.post(
                [&] {
                    gate.wait();
                    return true;
                },
                {});
        gate.awaitEntered();
        saver.post(record(log, "background"), {});
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.release();
        });
        bool nested = false;
        kobalt::storage::runOnWriterThread([&] {
            require(kobalt::storage::onWriterThread(), "A call did not run on the writer thread");
            log.add("call");
            kobalt::storage::runOnWriterThread([&] { nested = true; });
        });
        release.join();
        require(nested, "A call made on the writer thread did not run at once");
        kobalt::storage::drainWriterThread();
        require(log.text() == "call background ", "A call did not run before the waiting write");
        require(!kobalt::storage::onWriterThread(),
                "The test thread reported itself as the writer thread");
    }
    {
        // An exception from a call reaches its caller.
        bool caught = false;
        try {
            kobalt::storage::runOnWriterThread([] { throw 7; });
        } catch (int value) {
            caught = value == 7;
        }
        require(caught, "A call's exception did not reach its caller");
    }
    {
        // A background write that has waited two seconds runs before more calls.
        Log log;
        Gate gate;
        RecordWriter busy, saver;
        busy.post(
                [&] {
                    gate.wait();
                    return true;
                },
                {});
        gate.awaitEntered();
        saver.post(record(log, "aged"), {});
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.release();
        });
        kobalt::storage::runOnWriterThread([&] { log.add("call"); });
        release.join();
        kobalt::storage::drainWriterThread();
        require(log.text() == "aged call ", "A long-waiting write did not run before a new call");
    }
    std::puts(
            "PASS: record writers share one thread in queue order, keep the newest write, and drain on destruction");
}
