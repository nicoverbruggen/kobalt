#pragma once
#include <cstdio>
#include <cstdlib>

// Stops the test at the first failed requirement and names its source line.
#define require(condition, message) \
    ::kobalt::test::requireAt(static_cast<bool>(condition), (message), __FILE__, __LINE__)

namespace kobalt::test {
inline void requireAt(bool condition, const char *message, const char *file, int line)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message);
    std::exit(1);
}
} // namespace kobalt::test
