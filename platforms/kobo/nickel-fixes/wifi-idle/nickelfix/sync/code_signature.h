#pragma once
#include "../memory.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <functional>

namespace nfx {
struct CodeReference { size_t offset; const char *symbol; };
struct CodeSignature {
    size_t size, gotLiteral, gotPc;
    const char *sha256;
    const CodeReference *calls; size_t callCount;
    const CodeReference *data; size_t dataCount;
};
inline uint32_t armImmediate(uint32_t word) {
    const unsigned rotate = ((word >> 8) & 15) * 2;
    const uint32_t value = word & 255;
    return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}
// Accept a direct Thumb call or the standard three-instruction ARM PLT entry.
// Follow only checked reads, and require the resulting callee to be the symbol
// specified by the local signature. Merely masking a BL displacement is unsafe.
inline bool callsSymbol(const ModuleMemory &memory, uintptr_t at, uintptr_t expected)
{
    uint16_t first, second;
    if (!expected || !memory.read(at, first) || !memory.read(at + 2, second)
        || (first & 0xf800) != 0xf000) return false;
    const bool blx = (second & 0xd001) == 0xc000;
    if (!blx && (second & 0xd000) != 0xd000) return false;
    const unsigned sign = (first >> 10) & 1;
    const unsigned i1 = (~((second >> 13) ^ sign)) & 1;
    const unsigned i2 = (~((second >> 11) ^ sign)) & 1;
    const uint32_t immediate = (sign << 24) | (i1 << 23) | (i2 << 22)
        | ((first & 1023) << 12) | ((second & (blx ? 0x7fe : 0x7ff)) << 1);
    const int32_t displacement = int32_t(immediate << 7) >> 7;
    uintptr_t target;
    if (!addOffset(blx ? ((at + 4) & ~uintptr_t(3)) : at + 4, displacement, target)) return false;
    if (target == (expected & ~uintptr_t(1))) return bool(expected & 1) != blx;
    if (!blx || !memory.contains(target, 12, Read | Execute)) return false;
    uint32_t a, b, c;
    if (!memory.read(target, a) || !memory.read(target + 4, b) || !memory.read(target + 8, c)
        || (a & 0xfffff000) != 0xe28fc000 || (b & 0xfffff000) != 0xe28cc000
        || (c & 0xfffff000) != 0xe5bcf000) return false;
    uintptr_t slot;
    const uint64_t offset = uint64_t(armImmediate(a)) + armImmediate(b) + (c & 4095) + 8;
    if (offset > INT64_MAX || !addOffset(target, int64_t(offset), slot)) return false;
    uint32_t callee;
    return memory.read(slot, callee) && uintptr_t(callee) == expected;
}
inline bool matchesCode(const ModuleMemory &memory, uintptr_t fn, const CodeSignature &signature,
                        const std::function<uintptr_t(const char *)> &resolve)
{
    if (signature.size > INT32_MAX || !memory.got
        || !memory.contains(fn, signature.size, Read | Execute)
        || signature.gotLiteral > signature.size || signature.size - signature.gotLiteral < 4
        || signature.gotPc >= signature.size) return false;
    QByteArray code(reinterpret_cast<const char *>(fn), int(signature.size));
    uint32_t value;
    uintptr_t got;
    if (!memory.read(fn + signature.gotLiteral, value)
        || !addOffset(fn + signature.gotPc, int32_t(value), got) || got != memory.got) return false;
    std::memset(code.data() + signature.gotLiteral, 0, 4);
    for (size_t i = 0; i < signature.callCount; ++i) {
        const auto &reference = signature.calls[i];
        if (reference.offset > signature.size || signature.size - reference.offset < 4
            || !callsSymbol(memory, fn + reference.offset, resolve(reference.symbol))) return false;
        std::memset(code.data() + reference.offset, 0, 4);
    }
    for (size_t i = 0; i < signature.dataCount; ++i) {
        const auto &reference = signature.data[i];
        uintptr_t slot;
        uint32_t pointer;
        const auto expected = resolve(reference.symbol);
        if (!expected || reference.offset > signature.size || signature.size - reference.offset < 4
            || !memory.read(fn + reference.offset, value)
            || !addOffset(got, int32_t(value), slot) || !memory.read(slot, pointer)
            || uintptr_t(pointer) != expected) return false;
        std::memset(code.data() + reference.offset, 0, 4);
    }
    return QCryptographicHash::hash(code, QCryptographicHash::Sha256).toHex() == signature.sha256;
}
}
