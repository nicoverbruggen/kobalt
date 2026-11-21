#include "record_writer.h"
#include "support/main_thread.h"
#include "support/save_wait.h"
#include <algorithm>
#include <cerrno>
#include <cassert>
#include <chrono>
#include <deque>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <system_error>
#include <thread>
#include <time.h>
#include <utility>

namespace kobalt::storage {
#ifdef KOBALT_TEST_RECORD_WRITER
namespace test {
void beforeWriterStart();
void adjustWriterTime(clockid_t clock, timespec &now);
} // namespace test
#endif
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
#ifndef __APPLE__
    Condition()
    {
        pthread_condattr_t attributes;
        int error = pthread_condattr_init(&attributes);
        if (error)
            throw std::system_error(error, std::generic_category());
        error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
        if (!error)
            error = pthread_cond_init(&condition, &attributes);
        pthread_condattr_destroy(&attributes);
        if (error)
            throw std::system_error(error, std::generic_category());
    }
#endif

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
        const auto deadline = monotonicTime() + std::chrono::milliseconds(milliseconds);
        while (!ready()) {
#ifdef __APPLE__
            // Darwin takes a relative timeout. Keep the same deadline across wakeups.
            const auto remaining = deadline - monotonicTime();
            if (remaining <= std::chrono::nanoseconds::zero())
                return false;
            const auto timeout = timespecOf(remaining);
            const int error = pthread_cond_timedwait_relative_np(
                    &condition, lock.mutex()->native_handle(), &timeout);
#else
            const auto timeout = timespecOf(deadline);
            const int error =
                    pthread_cond_timedwait(&condition, lock.mutex()->native_handle(), &timeout);
#endif
            if (error == ETIMEDOUT)
                return ready();
            if (error)
                throw std::system_error(error, std::generic_category());
        }
        return true;
    }

    void notifyAll() { pthread_cond_broadcast(&condition); }

private:
    static std::chrono::nanoseconds monotonicTime()
    {
        timespec now;
        constexpr auto clock = CLOCK_MONOTONIC;
        clock_gettime(clock, &now);
#ifdef KOBALT_TEST_RECORD_WRITER
        test::adjustWriterTime(clock, now);
#endif
        return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
    }

    static timespec timespecOf(std::chrono::nanoseconds duration)
    {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        return { static_cast<time_t>(seconds.count()),
                 static_cast<long>((duration - seconds).count()) };
    }

#ifdef __APPLE__
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
#else
    pthread_cond_t condition;
#endif
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
    enum class LaunchFailure { Throw, Report };
    // Never destroyed, so no writer outlives its lock at process exit.
    static WriterThread &instance()
    {
        static auto *thread = new WriterThread;
        return *thread;
    }

    bool current()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::this_thread::get_id() == worker.get_id();
    }

    bool call(const std::function<void()> &operation, LaunchFailure launchFailure)
    {
        if (current()) {
            operation();
            return true;
        }
        mainthread::BlockingOperation blockingOperation("profile-database");
        Call request{ operation };
        std::unique_lock<std::mutex> lock(mutex);
        if (!start(launchFailure))
            return false;
        calls.push_back(&request);
        wake.notifyAll();
        finished.wait(lock, [&request] { return request.done; });
        lock.unlock();
        if (request.error)
            std::rethrow_exception(request.error);
        return true;
    }

    bool drain(const std::function<void()> &whenUnstarted)
    {
        mainthread::BlockingOperation blockingOperation("profile-database-drain");
        std::unique_lock<std::mutex> lock(mutex);
        assertNotRunner();
        if (!worker.joinable()) {
            if (whenUnstarted)
                whenUnstarted();
            return false;
        }
        finished.wait(lock, [this] { return queue.empty() && calls.empty() && !busy; });
        return true;
    }

    void remove(RecordWriter *writer)
    {
        std::unique_lock<std::mutex> lock(mutex);
        assertNotRunner();
        finished.wait(lock, [writer] { return !writer->running && !writer->pending; });
    }

    bool post(RecordWriter *writer, RecordWriter::Write write, RecordWriter::Done done,
              LaunchFailure launchFailure)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (writer->stopped || !start(launchFailure))
                return false;
            if (writer->pending && writer->delivery == RecordWriter::Delivery::Latest) {
                auto pending = std::find_if(queue.begin(), queue.end(), [writer](const Job &job) {
                    return job.owner == writer;
                });
                pending->write = std::move(write);
                pending->done = std::move(done);
            } else {
                queue.push_back({ writer, std::move(write), std::move(done),
                                  std::chrono::steady_clock::now() });
                ++writer->pending;
            }
        }
        wake.notifyAll();
        return true;
    }

    void cancel(RecordWriter *writer)
    {
        std::unique_lock<std::mutex> lock(mutex);
        assertNotRunner();
        discardQueuedLocked(writer);
        finished.wait(lock, [writer] { return !writer->running; });
    }

    bool discardQueued(RecordWriter *writer)
    {
        std::lock_guard<std::mutex> lock(mutex);
        writer->stopped = true;
        discardQueuedLocked(writer);
        return !writer->running;
    }

private:
    void discardQueuedLocked(RecordWriter *writer)
    {
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [writer](const Job &job) { return job.owner == writer; }),
                    queue.end());
        writer->pending = 0;
        finished.notifyAll();
    }

public:
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
    struct Job
    {
        RecordWriter *owner;
        RecordWriter::Write write;
        RecordWriter::Done done;
        std::chrono::steady_clock::time_point queuedAt;
    };

    struct Call
    {
        const std::function<void()> &operation;
        std::exception_ptr error = nullptr;
        bool done = false;
    };

    // Called with the mutex held.
    bool start(LaunchFailure launchFailure)
    {
        if (worker.joinable())
            return true;
        try {
#ifdef KOBALT_TEST_RECORD_WRITER
            test::beforeWriterStart();
#endif
            worker = std::thread([this] { run(); });
        } catch (const std::system_error &) {
            if (launchFailure == LaunchFailure::Throw)
                throw;
            return false;
        }
        return true;
    }

    // A write or its callback that waits for the thread would wait for itself.
    void assertNotRunner() const { assert(std::this_thread::get_id() != worker.get_id()); }

    bool backgroundFirst() const
    {
        if (queue.empty())
            return false;
        return calls.empty()
                || std::chrono::steady_clock::now() - queue.front().queuedAt >= backgroundLimit;
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
            auto job = std::move(queue.front());
            queue.pop_front();
            auto *writer = job.owner;
            --writer->pending;
            writer->running = busy = true;
            lock.unlock();
            // The writer cannot be destroyed while `running` is set. A write
            // that throws counts as failed, so `running` is always cleared.
            bool saved = false;
            {
                DiagnosticOperation operation("background-profile-write");
                try {
                    saved = job.write();
                } catch (...) {
                }
            }
            try {
                if (job.done)
                    job.done(saved);
            } catch (...) {
            }
            lock.lock();
            writer->running = busy = false;
            finished.notifyAll();
            // `done` has queued its GUI-thread work. This report follows it.
            saves::settled();
        }
    }

    std::mutex mutex;
    Condition wake, finished;
    std::deque<Job> queue;
    std::deque<Call *> calls;
    bool busy = false;
    // The process-lifetime singleton keeps the worker joinable.
    std::thread worker;
};

RecordWriter::RecordWriter(Delivery delivery) : delivery(delivery) { }

RecordWriter::~RecordWriter()
{
    mainthread::BlockingOperation operation("record-writer-close", !idle());
    WriterThread::instance().remove(this);
}

void RecordWriter::post(Write write, Done done)
{
    WriterThread::instance().post(this, std::move(write), std::move(done),
                                  WriterThread::LaunchFailure::Throw);
}

bool RecordWriter::tryPost(Write write)
{
    return WriterThread::instance().post(this, std::move(write), {},
                                         WriterThread::LaunchFailure::Report);
}

void RecordWriter::cancel()
{
    mainthread::BlockingOperation operation("record-writer-cancel", !idle());
    WriterThread::instance().cancel(this);
}

bool RecordWriter::discardQueued()
{
    return WriterThread::instance().discardQueued(this);
}

bool RecordWriter::writeNow(Write write)
{
    mainthread::BlockingOperation blockingOperation("record-writer-write");
    bool saved = false;
    WriterThread::instance().post(this, std::move(write),
                                  [&saved](bool result) { saved = result; },
                                  WriterThread::LaunchFailure::Throw);
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
    WriterThread::instance().call(operation, WriterThread::LaunchFailure::Throw);
}

bool tryRunOnWriterThread(const std::function<void()> &operation)
{
    return WriterThread::instance().call(operation, WriterThread::LaunchFailure::Report);
}

bool onWriterThread()
{
    return WriterThread::instance().current();
}

bool drainWriterThread(const std::function<void()> &whenUnstarted)
{
    return WriterThread::instance().drain(whenUnstarted);
}
} // namespace kobalt::storage
