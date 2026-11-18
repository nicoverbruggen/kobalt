#include "detour.h"
#include "detour_prologue.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

static const int NTF_DETOUR_BYTES = 8;

struct NtfCodeMapping {
    uintptr_t lo, hi;
    int prot;
};

// Use the current permissions, not the ELF flags: another startup patch may have changed them.
// Refuse a range crossing mappings so one mprotect call cannot partially change several VMAs.
static bool ntf_code_mapping(uintptr_t addr, size_t size, NtfCodeMapping *out)
{
    if (!size || size > UINTPTR_MAX - addr) return false;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[1024], flags[5];
    unsigned long lo, hi;
    bool found = false;
    while (fgets(line, sizeof line, maps)) {
        if (!strchr(line, '\n')) {
            int c;
            while ((c = fgetc(maps)) != '\n' && c != EOF) {}
        }
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, flags) != 3) continue;
        if (addr < lo || addr >= hi || size > hi - addr) continue;
        out->lo = lo;
        out->hi = hi;
        out->prot = (flags[0] == 'r' ? PROT_READ : 0) |
                    (flags[1] == 'w' ? PROT_WRITE : 0) |
                    (flags[2] == 'x' ? PROT_EXEC : 0);
        found = true;
        break;
    }
    fclose(maps);
    return found;
}

static bool ntf_permissions(void *addr, size_t size, int prot)
{
    NtfCodeMapping mapping;
    return ntf_code_mapping((uintptr_t)addr, size, &mapping) && mapping.prot == prot;
}

// ldr.w pc, [pc, #0]; .word target|1. The caller checks alignment and 32-bit addresses.
static void ntf_absolute_jump(unsigned char *out, uintptr_t target)
{
    out[0] = 0xdf; out[1] = 0xf8; out[2] = 0x00; out[3] = 0xf0;
    uint32_t t = (uint32_t)target | 1U;
    memcpy(out + 4, &t, 4);
}

// Each word is aligned, but the whole jump is NOT atomic. Installation is restricted to init.
static void ntf_write_entry(unsigned char *fn, const unsigned char *bytes)
{
    uint32_t words[2];
    memcpy(words, bytes, sizeof words);
    __atomic_store_n((uint32_t *)(fn + 4), words[1], __ATOMIC_RELAXED);
    __atomic_store_n((uint32_t *)fn, words[0], __ATOMIC_RELAXED);
    __builtin___clear_cache((char *)fn, (char *)fn + NTF_DETOUR_BYTES);
}

extern "C" int ntf_detour_at(void *addr, void *replacement, void **original, int *relocated_out)
{
    if (!addr || !replacement || !original || *original) return -1;
    uintptr_t start = (uintptr_t)addr & ~uintptr_t(1);
    uintptr_t dest = (uintptr_t)replacement & ~uintptr_t(1);
    if (start & 3) return -5;
    if (start > UINT32_MAX - 12 || dest > UINT32_MAX) return -1;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 32 || (page_size & (page_size - 1))) return -2;
    size_t page = (size_t)page_size;
    NtfCodeMapping target, replacement_map;
    const int rx = PROT_READ | PROT_EXEC;
    if (!ntf_code_mapping(start, NTF_DETOUR_BYTES, &target) || target.prot != rx ||
        !ntf_code_mapping(dest, 2, &replacement_map) || replacement_map.prot != rx) return -2;
    unsigned char *fn = (unsigned char *)start;
    if (!ntf_known_detour_prologue(fn, NTF_DETOUR_BYTES)) return -6;
    const int n = NTF_DETOUR_BYTES;
    uintptr_t lo = start & ~(uintptr_t)(page - 1);
    if (start + NTF_DETOUR_BYTES > UINTPTR_MAX - (page - 1)) return -2;
    uintptr_t hi = (start + NTF_DETOUR_BYTES + page - 1) & ~(uintptr_t)(page - 1);
    if (lo < target.lo || hi > target.hi) return -2;
    void *pages = (void *)lo;
    size_t span = hi - lo;
    unsigned char saved[10];
    memcpy(saved, fn, n);

    unsigned char *tramp = (unsigned char *)mmap(0, page, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return -3;
    if ((uintptr_t)tramp > UINT32_MAX - page) {
        munmap(tramp, page);
        return -3;
    }
    unsigned char expected[20] = {};
    memcpy(expected, saved, n);
    int jump_at = n;
    if (jump_at & 3) {
        expected[jump_at] = 0x00; expected[jump_at + 1] = 0xbf; // nop to align the literal load
        jump_at += 2;
    }
    ntf_absolute_jump(expected + jump_at, start + n);
    size_t trampoline_size = jump_at + NTF_DETOUR_BYTES;
    memcpy(tramp, expected, trampoline_size);
    __builtin___clear_cache((char *)tramp, (char *)tramp + trampoline_size);
    if (mprotect(tramp, page, rx) != 0 || !ntf_permissions(tramp, page, rx) ||
        memcmp(tramp, expected, trampoline_size) != 0) {
        munmap(tramp, page);
        return -3;
    }
    // Preparation must not overwrite changes made by another installer.
    if (!ntf_permissions(pages, span, rx) || memcmp(fn, saved, n) != 0) {
        munmap(tramp, page);
        return -2;
    }
    if (mprotect(pages, span, rx | PROT_WRITE) != 0) {
        // A failed mprotect is not evidence that the permissions stayed unchanged.
        mprotect(pages, span, rx);
        if (!ntf_permissions(pages, span, rx)) ntf_patch_unsafe("detour recovery failed");
        munmap(tramp, page);
        return -4;
    }

    unsigned char detour[NTF_DETOUR_BYTES];
    ntf_absolute_jump(detour, (uintptr_t)replacement);
    *original = (void *)((uintptr_t)tramp | 1);
    ntf_write_entry(fn, detour);
    bool written = memcmp(fn, detour, sizeof detour) == 0;
    bool protected_code = mprotect(pages, span, rx) == 0;
    if (written && protected_code && ntf_permissions(pages, span, rx) &&
        memcmp(fn, detour, sizeof detour) == 0) {
        if (relocated_out) *relocated_out = n;
        return 0;
    }

    // Do not unmap the trampoline until the target no longer reaches it. If either bytes or
    // permissions cannot be recovered, startup must never return and disarm the boot failsafe.
    mprotect(pages, span, rx | PROT_WRITE);
    if (!ntf_permissions(pages, span, rx | PROT_WRITE)) ntf_patch_unsafe("detour recovery failed");
    ntf_write_entry(fn, saved);
    mprotect(pages, span, rx);
    if (!ntf_permissions(pages, span, rx) || memcmp(fn, saved, n) != 0) ntf_patch_unsafe("detour recovery failed");
    *original = 0;
    munmap(tramp, page);
    return -4;
}
