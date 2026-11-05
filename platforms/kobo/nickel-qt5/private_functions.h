#pragma once
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <link.h>

// Firmware 4 adapters use this matcher for unexported libnickel functions. A firmware release moves them, so an address holds for one build only.
// Each function is found by its first 24 Thumb halfwords instead. Branch
// targets and literal-pool offsets are masked, because they move too. A
// signature counts only if it matches exactly one place. The signatures were
// taken from 4.46.23836 and each matches once in 4.38.23697 and 4.42.23303.
namespace kobalt::nickel_qt5 {
struct Halfword
{
    std::uint16_t value, mask;
};

namespace detail {
struct Segment
{
    const void *base;
    const std::uint16_t *code = nullptr;
    std::size_t halfwords = 0;
};

inline int executableSegment(dl_phdr_info *info, std::size_t, void *data)
{
    auto *segment = static_cast<Segment *>(data);
    if (reinterpret_cast<const void *>(info->dlpi_addr) != segment->base)
        return 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto &header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || !(header.p_flags & PF_X))
            continue;
        segment->code = reinterpret_cast<const std::uint16_t *>(info->dlpi_addr + header.p_vaddr);
        segment->halfwords = header.p_memsz / 2;
    }
    return 1;
}

template <std::size_t N>
void *find(const Halfword (&signature)[N])
{
    // Any exported libnickel function names the library to search.
    void *known = dlsym(RTLD_DEFAULT, "_ZN6Device16getCurrentDeviceEv");
    Dl_info library{};
    if (!known || !dladdr(known, &library) || !library.dli_fbase)
        return nullptr;
    Segment segment{ library.dli_fbase };
    dl_iterate_phdr(executableSegment, &segment);
    if (!segment.code || segment.halfwords < N)
        return nullptr;
    const std::uint16_t *match = nullptr;
    for (std::size_t at = 0; at + N <= segment.halfwords; ++at) {
        if ((segment.code[at] & signature[0].mask) != signature[0].value)
            continue;
        std::size_t i = 1;
        while (i < N && (segment.code[at + i] & signature[i].mask) == signature[i].value)
            ++i;
        if (i != N)
            continue;
        if (match)
            return nullptr;
        match = segment.code + at;
    }
    // Bit 0 selects Thumb state for the call.
    return match ? reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(match) | 1) : nullptr;
}
} // namespace detail

// The singleton getter of the 24-byte object that owns Nickel's orientation
// sensor at offset 8. The allocation size is part of the signature.
inline void *orientationService()
{
    static constexpr Halfword signature[] = {
        { 0xb598, 0xffff }, { 0xaf00, 0xffff }, { 0x4c00, 0xff00 }, { 0x447c, 0xffff },
        { 0x4620, 0xffff }, { 0xf000, 0xf800 }, { 0xc000, 0xd000 }, { 0x6863, 0xffff },
        { 0xb103, 0xfd07 }, { 0x4c00, 0xff00 }, { 0x447c, 0xffff }, { 0x4620, 0xffff },
        { 0xf000, 0xf800 }, { 0xc000, 0xd000 }, { 0x6860, 0xffff }, { 0xbd98, 0xffff },
        { 0x2018, 0xffff }, { 0xf000, 0xf800 }, { 0xc000, 0xd000 }, { 0x4604, 0xffff },
        { 0xf000, 0xf800 }, { 0xd000, 0xd000 }, { 0x4b00, 0xff00 }, { 0x447b, 0xffff }
    };
    static void *const found = detail::find(signature);
    return found;
}
} // namespace kobalt::nickel_qt5
