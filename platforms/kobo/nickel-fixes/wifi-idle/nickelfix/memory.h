#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nfx {
enum MemoryAccess { Read = 1, Write = 2, Execute = 4 };
struct MemoryRange { uintptr_t begin, end; unsigned access; };
// Snapshot actual mapped permissions, bounded by this loaded ELF's segments.
// Never infer safety from proximity to a library base. Capture during startup,
// before changing code, and require every read/write to fit one checked range.
struct ModuleMemory {
    std::vector<MemoryRange> ranges;
    uintptr_t got = 0;
    static ModuleMemory containing(const void *symbol);
    bool contains(uintptr_t address, size_t size, unsigned access = Read) const {
        if (!address || !size || size > UINTPTR_MAX - address) return false;
        for (const auto &r : ranges)
            if (address >= r.begin && address + size <= r.end && (r.access & access) == access) return true;
        return false;
    }
    template<typename T> bool read(uintptr_t address, T &value) const {
        if (!contains(address, sizeof value)) return false;
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof value);
        return true;
    }
};
inline bool addOffset(uintptr_t base, int64_t offset, uintptr_t &result) {
    if (offset < 0) {
        const uint64_t distance = uint64_t(-(offset + 1)) + 1;
        if (distance > base) return false;
        result = base - uintptr_t(distance);
    } else {
        if (uint64_t(offset) > UINTPTR_MAX - base) return false;
        result = base + uintptr_t(offset);
    }
    return true;
}
}
