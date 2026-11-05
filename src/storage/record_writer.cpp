#include "record_writer.h"
#include "support/main_thread.h"
#include <algorithm>
#include <cerrno>
#include <cassert>
#include <chrono>
#include <deque>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <time.h>
#include <utility>

namespace kobalt::storage {
namespace {
// A background write that has waited this long runs before further calls, so
// a run of reads cannot hold it back.
constexpr auto backgroundLimit = std::chrono::seconds(2);

// The Qt 6 toolchain links std::condition_variable::wait against
// GLIBCXX_3.4.30, which firmware 6 does not provide, so Nickel could not load
// the library. Wait on a pthread condition with the std::mutex's handle, as
// engine_activity.h does.
class Condition
{
public:
    ~Condition() { pthread_cond_destroy(&condition); }

    template <class Ready>
    void wait(std::unique_lock<std::mutex> &lock, Ready ready)
    {
        while (!ready())
            pthread_cond_wait(&condition, lock.mutex()->native_handle());
    }

    // False when `milliseconds` pass first.
    template <class Ready>
    bool waitFor(std::unique_lock<std::mutex> &lock, int milliseconds, Ready ready)
    {
        timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += milliseconds / 1000;
        deadline.tv_nsec += long(milliseconds % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        while (!ready())
            if (pthread_cond_timedwait(&condition, lock.mutex()->native_handle(), &deadline)
                == ETIMEDOUT)
                return ready();
        return true;
    }

    void notifyAll() { pthread_cond_broadcast(&condition); }

private:
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
};
} // namespace

// The one thread that reads and writes the profile. It starts with the first
// request and stays for the life of the process; it sleeps while idle.
// Calls that a caller waits for run first. A writer with a new background
// write joins the back of its queue; the thread takes each writer's newest
// write in turn. The fields of writers and calls are guarded by this mutex.
class WriterThread
{
public:
    // Never destroyed, so no writer outlives its lock at process exit.
    static WriterThread &instance()
    {
        static auto *thread = new WriterThread;
        return *thread;
    }

    bool current()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return started && std::this_thread::get_id() == runner;
    }

    void call(const std::function<void()> &operation)
    {
        if (current()) {
            operation();
            return;
        }
        mainthread::BlockingOperation blockingOperation("profile-database");
        Call request{ operation };
        std::unique_lock<std::mutex> lock(mutex);
        calls.push_back(&request);
        start();
        wake.notifyAll();
        finished.wait(lock, [&request] { return request.done; });
        lock.unlock();
        if (request.error)
            std::rethrow_exception(request.error);
    }

    void drain()
    {
        mainthread::BlockingOperation blockingOperation("profile-database-drain");
        std::unique_lock<std::mutex> lock(mutex);
        assertNotRunner();
        finished.wait(lock, [this] { return queue.empty() && calls.empty() && !busy; });
    }

    void remove(RecordWriter *writer)
    {
        std::unique_lock<std::mutex> lock(mutex);
        assertNotRunner();
        finished.wait(lock, [writer] { return !writer->running && !writer->pending; });
        // A cancelled writer can still be queued without a write.
        if (writer->queued)
            queue.erase(std::find(queue.begin(), queue.end(), writer));
    }

    void post(RecordWriter *writer, RecordWriter::Write write, RecordWriter::Done done)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            writer->pending = std::move(write);
            writer->pendingDone = std::move(done);
            if (!writer->queued) {
                writer->queued = true;
                writer->queuedAt = std::chrono::steady_clock::now();
                queue.push_back(writer);
            }
            start();
        }
        wake.notifyAll();
    }

    void cancel(RecordWriter *writer)
    {
        std::unique_lock<std::mutex> lock(mutex);
        assertNotRunner();
        writer->pending = {};
        writer->pendingDone = {};
        finished.wait(lock, [writer] { return !writer->running; });
    }

    bool flush(RecordWriter *writer, int milliseconds)
    {
        std::unique_lock<std::mutex> lock(mutex);
        assertNotRunner();
        const auto drained = [writer] { return !writer->running && !writer->pending; };
        if (milliseconds < 0) {
            finished.wait(lock, drained);
            return true;
        }
        return finished.waitFor(lock, milliseconds, drained);
    }

    bool idle(const RecordWriter *writer)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return !writer->running && !writer->pending;
    }

private:
    struct Call
    {
        const std::function<void()> &operation;
        std::exception_ptr error = nullptr;
        bool done = false;
    };

    // Called with the mutex held.
    void start()
    {
        if (started)
            return;
        started = true;
        std::thread thread([this] { run(); });
        runner = thread.get_id();
        thread.detach();
    }

    // A write or its callback that waits for the thread would wait for itself.
    void assertNotRunner() const { assert(!started || std::this_thread::get_id() != runner); }

    bool backgroundFirst() const
    {
        if (queue.empty())
            return false;
        return calls.empty()
                || std::chrono::steady_clock::now() - queue.front()->queuedAt >= backgroundLimit;
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return !queue.empty() || !calls.empty(); });
            if (!backgroundFirst()) {
                auto *request = calls.front();
                calls.pop_front();
                busy = true;
                lock.unlock();
                try {
                    request->operation();
                } catch (...) {
                    request->error = std::current_exception();
                }
                lock.lock();
                busy = false;
                request->done = true;
                finished.notifyAll();
                continue;
            }
            auto *writer = queue.front();
            queue.pop_front();
            writer->queued = false;
            // A cancelled writer stays queued without a write.
            if (!writer->pending) {
                finished.notifyAll();
                continue;
            }
            auto write = std::move(writer->pending);
            auto done = std::move(writer->pendingDone);
            writer->pending = {};
            writer->pendingDone = {};
            writer->running = busy = true;
            lock.unlock();
            // The writer cannot be destroyed while `running` is set. A write
            // that throws counts as failed, so `running` is always cleared.
            bool saved = false;
            {
                DiagnosticOperation operation("background-profile-write");
                try {
                    saved = write();
                } catch (...) {
                }
            }
            try {
                if (done)
                    done(saved);
            } catch (...) {
            }
            lock.lock();
            writer->running = busy = false;
            finished.notifyAll();
        }
    }

    std::mutex mutex;
    Condition wake, finished;
    std::deque<RecordWriter *> queue;
    std::deque<Call *> calls;
    bool started = false, busy = false;
    std::thread::id runner;
};

RecordWriter::RecordWriter() = default;

RecordWriter::~RecordWriter()
{
    mainthread::BlockingOperation operation("record-writer-close", !idle());
    WriterThread::instance().remove(this);
}

void RecordWriter::post(Write write, Done done)
{
    WriterThread::instance().post(this, std::move(write), std::move(done));
}

void RecordWriter::cancel()
{
    mainthread::BlockingOperation operation("record-writer-cancel", !idle());
    WriterThread::instance().cancel(this);
}

bool RecordWriter::writeNow(Write write)
{
    mainthread::BlockingOperation blockingOperation("record-writer-write");
    bool saved = false;
    WriterThread::instance().post(this, std::move(write),
                                  [&saved](bool result) { saved = result; });
    WriterThread::instance().flush(this, -1);
    return saved;
}

bool RecordWriter::flush(int milliseconds)
{
    mainthread::BlockingOperation operation("record-writer-flush", milliseconds != 0 && !idle());
    return WriterThread::instance().flush(this, milliseconds);
}

bool RecordWriter::idle() const
{
    return WriterThread::instance().idle(this);
}

void runOnWriterThread(const std::function<void()> &operation)
{
    WriterThread::instance().call(operation);
}

bool onWriterThread()
{
    return WriterThread::instance().current();
}

void drainWriterThread()
{
    WriterThread::instance().drain();
}
} // namespace kobalt::storage
