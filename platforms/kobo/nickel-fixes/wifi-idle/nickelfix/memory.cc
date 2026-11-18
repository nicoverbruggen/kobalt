#include "memory.h"
#include <cstdio>
#include <algorithm>
#ifdef __linux__
#include <link.h>
#endif

namespace nfx {
ModuleMemory ModuleMemory::containing(const void *symbol)
{
    ModuleMemory result;
#ifdef __linux__
    if (!symbol) return result;
    struct Search { uintptr_t address; ModuleMemory *result; } search{
        reinterpret_cast<uintptr_t>(symbol) & ~uintptr_t(1), &result};
    dl_iterate_phdr([](dl_phdr_info *info, size_t, void *opaque) -> int {
        auto &s = *static_cast<Search *>(opaque);
        bool owner = false;
        for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
            const auto &p = info->dlpi_phdr[i];
            if (p.p_type != PT_LOAD || p.p_vaddr > UINTPTR_MAX - info->dlpi_addr) continue;
            const uintptr_t begin = info->dlpi_addr + p.p_vaddr;
            if (s.address >= begin && s.address - begin < p.p_memsz) owner = true;
        }
        if (!owner) return 0;
        FILE *maps = fopen("/proc/self/maps", "r");
        if (!maps) return 1;
        char line[1024], flags[5];
        unsigned long lo, hi;
        while (fgets(line, sizeof line, maps)) {
            if (sscanf(line, "%lx-%lx %4s", &lo, &hi, flags) != 3) continue;
            const unsigned access = (flags[0] == 'r' ? Read : 0)
                | (flags[1] == 'w' ? Write : 0) | (flags[2] == 'x' ? Execute : 0);
            for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
                const auto &p = info->dlpi_phdr[i];
                if (p.p_type != PT_LOAD || p.p_vaddr > UINTPTR_MAX - info->dlpi_addr) continue;
                const uintptr_t begin = info->dlpi_addr + p.p_vaddr;
                if (p.p_memsz > UINTPTR_MAX - begin) continue;
                const uintptr_t first = std::max(begin, uintptr_t(lo));
                const uintptr_t last = std::min(begin + p.p_memsz, uintptr_t(hi));
                if (first < last) s.result->ranges.push_back({first, last, access});
            }
        }
        fclose(maps);
        for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
            const auto &p = info->dlpi_phdr[i];
            if (p.p_type != PT_DYNAMIC || p.p_vaddr > UINTPTR_MAX - info->dlpi_addr) continue;
            const uintptr_t begin = info->dlpi_addr + p.p_vaddr;
            if (!s.result->contains(begin, p.p_memsz)) continue;
            for (size_t off = 0; off + sizeof(ElfW(Dyn)) <= p.p_memsz; off += sizeof(ElfW(Dyn))) {
                ElfW(Dyn) entry;
                if (!s.result->read(begin + off, entry) || entry.d_tag == DT_NULL) break;
                if (entry.d_tag != DT_PLTGOT) continue;
                uintptr_t got = entry.d_un.d_ptr;
                if (!s.result->contains(got, 4)) {
                    if (got > UINTPTR_MAX - info->dlpi_addr) break;
                    got += info->dlpi_addr;
                }
                if (s.result->contains(got, 4)) s.result->got = got;
                break;
            }
        }
        return 1;
    }, &search);
#else
    (void)symbol;
#endif
    return result;
}
}
