#include "storage/record_writer.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "check.h"

using kobalt::storage::RecordWriter;

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
} // namespace

// Checks record writers: queue order, replaced waiting writes, synchronous writes, cancellation, destruction and time limits.
int main()
{
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
        require(!first.flush(50), "A flush finished while its write was held");
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
