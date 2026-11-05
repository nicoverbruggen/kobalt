#pragma once

#include <QCoreApplication>
#include <QThread>
#include <private/qobject_p.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <link.h>

namespace kobalt::nickel::startup_detail {
struct Region
{
    std::uintptr_t begin = 0, end = 0;

    bool contains(std::uintptr_t address, std::size_t bytes) const
    {
        return address >= begin && address <= end && bytes <= end - address;
    }
};

struct Mapping
{
    std::uintptr_t base = 0;
    Region code, data;
};

inline int mapped(dl_phdr_info *info, std::size_t, void *argument)
{
    auto &mapping = *static_cast<Mapping *>(argument);
    if (info->dlpi_addr != mapping.base)
        return 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto &p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD || !(p.p_flags & PF_R))
            continue;
        Region region{ info->dlpi_addr + p.p_vaddr, info->dlpi_addr + p.p_vaddr + p.p_memsz };
        if (p.p_flags & PF_X)
            mapping.code = region;
        else if (p.p_flags & PF_W)
            mapping.data = region;
    }
    return 1;
}

struct Instruction
{
    std::uint16_t value, mask;
};

template <std::size_t N>
bool matches(const std::uint16_t *code, const Instruction (&expected)[N])
{
    for (std::size_t i = 0; i < N; ++i)
        if ((code[i] & expected[i].mask) != expected[i].value)
            return false;
    return true;
}

inline const std::uint32_t *constructionGuard(void **existingObject = nullptr)
{
    void *function = dlsym(RTLD_DEFAULT, "_ZN22N3PowerWorkflowManager14sharedInstanceEv");
    Dl_info info{};
    if (!function || !(reinterpret_cast<std::uintptr_t>(function) & 1) || !dladdr(function, &info))
        return nullptr;
    Mapping mapping;
    mapping.base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    dl_iterate_phdr(mapped, &mapping);
    const auto address = reinterpret_cast<std::uintptr_t>(function) & ~std::uintptr_t(1);
    if (!mapping.code.contains(address, 128))
        return nullptr;
    const auto *code = reinterpret_cast<const std::uint16_t *>(address);
    // Both known forms test bit zero of the C++ construction guard before
    // entering __cxa_guard_acquire. The getter itself is never called.
    static constexpr Instruction qt5[] = {
        { 0xb5b0, 0xffff }, { 0xaf00, 0xffff }, { 0x4c00, 0xff00 }, { 0x4d00, 0xff00 },
        { 0x447c, 0xffff }, { 0x6823, 0xf83f }, { 0x447d, 0xffff }, { 0x07db, 0xffff },
        { 0xd503, 0xffff }, { 0x4800, 0xff00 }, { 0x4478, 0xffff }, { 0x3000, 0xff00 },
        { 0xbdb0, 0xffff }, { 0x1c20, 0xfe3f }, { 0xf000, 0xf800 }, { 0xc000, 0xd000 },
        { 0x2800, 0xffff }, { 0xd0f6, 0xffff }, { 0xf104, 0xffff }, { 0x0000, 0xff00 },
        { 0xf000, 0xf800 }, { 0xc000, 0xd000 },
    };
    static constexpr Instruction qt6[] = {
        { 0xb538, 0xffff }, { 0x4c00, 0xff00 }, { 0x4d00, 0xff00 }, { 0x447c, 0xffff },
        { 0x6823, 0xf83f }, { 0xf3bf, 0xffff }, { 0x8f5b, 0xffff }, { 0x447d, 0xffff },
        { 0x07db, 0xffff }, { 0xd503, 0xffff }, { 0x4800, 0xff00 }, { 0x4478, 0xffff },
        { 0x3000, 0xff00 }, { 0xbd38, 0xffff }, { 0xf104, 0xffff }, { 0x0000, 0xff00 },
        { 0xf000, 0xf800 }, { 0xc000, 0xd000 }, { 0x2800, 0xffff }, { 0xd0f5, 0xffff },
        { 0xf104, 0xffff }, { 0x0000, 0xff00 }, { 0xf000, 0xf800 }, { 0xc000, 0xd000 },
    };
    const bool old = matches(code, qt5), newer = matches(code, qt6);
    if (!old && !newer)
        return nullptr;
    const std::size_t load = old ? 2 : 1;
    const std::size_t add = old ? 4 : 3;
    const std::size_t guardLoad = old ? 5 : 4;
    const auto literal = ((address + load * 2 + 4) & ~std::uintptr_t(3)) + (code[load] & 255) * 4;
    if (!mapping.code.contains(literal, sizeof(std::int32_t)))
        return nullptr;
    std::int32_t displacement;
    std::memcpy(&displacement, reinterpret_cast<const void *>(literal), sizeof(displacement));
    const auto singletonBase = address + add * 2 + 4 + displacement;
    const auto guardOffset = ((code[guardLoad] >> 6) & 31) * 4;
    // Check that guard acquisition uses the same address and that the fast
    // return and constructor both use the object immediately after the guard.
    const auto acquireOffset = old ? ((code[13] >> 6) & 7) : (code[15] & 255);
    const auto objectOffset = code[old ? 11 : 12] & 255;
    const auto constructorOffset = code[old ? 19 : 21] & 255;
    if (acquireOffset != guardOffset || objectOffset != constructorOffset
        || objectOffset <= guardOffset)
        return nullptr;
    const std::size_t returnLoad = old ? 9 : 10;
    const std::size_t returnAdd = old ? 10 : 11;
    const auto returnLiteral =
            ((address + returnLoad * 2 + 4) & ~std::uintptr_t(3)) + (code[returnLoad] & 255) * 4;
    if (!mapping.code.contains(returnLiteral, sizeof(std::int32_t))) {
        return nullptr;
    }
    std::memcpy(&displacement, reinterpret_cast<const void *>(returnLiteral), sizeof(displacement));
    if (address + returnAdd * 2 + 4 + displacement != singletonBase) {
        return nullptr;
    }
    const auto guardAddress = singletonBase + guardOffset;
    if (guardAddress % alignof(std::uint32_t)
        || !mapping.data.contains(guardAddress, sizeof(std::uint32_t))
        || !mapping.data.contains(singletonBase + objectOffset, 8))
        return nullptr;
    if (existingObject)
        *existingObject = reinterpret_cast<void *>(singletonBase + objectOffset);
    return reinterpret_cast<const std::uint32_t *>(guardAddress);
}

inline void *existingPower()
{
    void *object = nullptr;
    const auto *guard = constructionGuard(&object);
    return guard && (__atomic_load_n(guard, __ATOMIC_ACQUIRE) & 1) ? object : nullptr;
}
} // namespace kobalt::nickel::startup_detail

namespace kobalt::nickel {
inline bool nativeStartupReady()
{
    if (!qApp || QThread::currentThread() != qApp->thread()) {
        return false;
    }
    auto *power = static_cast<QObject *>(startup_detail::existingPower());
    if (!power || std::strcmp(power->metaObject()->className(), "N3PowerWorkflowManager")) {
        return false;
    }
    using Query = QObjectList (*)(const QObjectPrivate *, const char *);
    static const auto receivers =
            reinterpret_cast<Query>(dlsym(RTLD_DEFAULT, "_ZNK14QObjectPrivate12receiverListEPKc"));
    // Nickel connects this signal to its application at the end of initialization.
    // The manager can exist earlier, so its construction guard alone is insufficient.
    return receivers && receivers(QObjectPrivate::get(power), "resumedFromSleep()").contains(qApp);
}
} // namespace kobalt::nickel
