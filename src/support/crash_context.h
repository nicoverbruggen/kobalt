#pragma once
#include <atomic>
#include <cstddef>
#include <cerrno>
#include <unistd.h>

namespace kobalt {
// Writers publish bounded text during normal execution. Copying this context
// in the fatal handler never waits for a writer, allocates, formats JSON or
// enters the diagnostic queue.
namespace crashContext {
struct Slot
{
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    char data[2048]{};
    std::size_t size = 0;

    void store(const char *text, std::size_t count)
    {
        if (busy.test_and_set(std::memory_order_acquire))
            return;
        size = count < sizeof(data) ? count : sizeof(data);
        for (std::size_t i = 0; i < size; ++i)
            data[i] = text[i];
        busy.clear(std::memory_order_release);
    }

    void writeTo(int fd)
    {
        if (busy.test_and_set(std::memory_order_acquire))
            return;
        for (std::size_t offset = 0; offset < size;) {
            const auto n = ::write(fd, data + offset, size - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            offset += std::size_t(n);
        }
        busy.clear(std::memory_order_release);
    }
};

inline Slot resources;
inline Slot events[16];
inline std::atomic<unsigned> next{ 0 };
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Crash sequence must be lock-free");

inline void event(const char *text, std::size_t size)
{
    events[next.fetch_add(1, std::memory_order_relaxed) % 16].store(text, size);
}

inline void writeTo(int fd)
{
    const char heading[] =
            "\nlast resource sample and recent events (monotonic_ms identifies sample age):\n";
    for (std::size_t offset = 0; offset < sizeof(heading) - 1;) {
        const auto n = ::write(fd, heading + offset, sizeof(heading) - 1 - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        offset += std::size_t(n);
    }
    resources.writeTo(fd);
    const auto end = next.load(std::memory_order_relaxed);
    const auto count = end < 16 ? end : 16;
    for (unsigned i = count; i > 0; --i)
        events[(end - i) % 16].writeTo(fd);
}
} // namespace crashContext
} // namespace kobalt
