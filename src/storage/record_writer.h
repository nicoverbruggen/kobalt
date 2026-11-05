#pragma once
#include <chrono>
#include <functional>

namespace kobalt::storage {
// One thread owns the profile database. Every read and write of it runs there,
// one at a time, so no two operations ever wait for each other's lock and the
// GUI thread does no database work of its own.
//
// A RecordWriter sends background writes to that thread, so reading never
// waits for a commit and its storage syncs. Each writer holds at most one
// waiting write: posting replaces a write that has not started. Background
// writes run in the order they were first posted. Calls that a caller waits
// for, from runOnWriterThread(), run before them, but a background write that
// has waited two seconds runs next.
class RecordWriter
{
public:
    using Write = std::function<bool()>;
    using Done = std::function<void(bool)>;
    RecordWriter();
    RecordWriter(const RecordWriter &) = delete;
    RecordWriter &operator=(const RecordWriter &) = delete;
    // Finishes this writer's running and pending write.
    ~RecordWriter();
    // `done` runs on the writer thread with the result of `write`.
    void post(Write write, Done done);
    // Drops a pending write and waits for a running one. A synchronous write
    // that follows cannot then be overwritten by an older one.
    void cancel();
    // Replaces this writer's pending write with `write`, runs it on the writer
    // thread and waits for its result.
    bool writeNow(Write write);
    // Waits for this writer's pending and running write. False when
    // `milliseconds` pass first; a negative value waits without a limit.
    bool flush(int milliseconds = -1);
    bool idle() const;

private:
    friend class WriterThread;
    Write pending;
    Done pendingDone;
    std::chrono::steady_clock::time_point queuedAt;
    bool queued = false, running = false;
};

// Runs `operation` on the writer thread and waits for it. On the writer
// thread itself it runs at once. An exception from `operation` reaches the
// caller.
void runOnWriterThread(const std::function<void()> &operation);
bool onWriterThread();
// Waits until no write or call is waiting or running on the writer thread.
void drainWriterThread();
} // namespace kobalt::storage
