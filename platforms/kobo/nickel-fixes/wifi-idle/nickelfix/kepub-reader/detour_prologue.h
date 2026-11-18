#pragma once
#include <cstddef>
#include <cstring>

// Only these inspected eight-byte prologues may be copied verbatim. Each ends
// on an instruction boundary and contains no PC-relative instruction, branch
// or IT block. This is deliberately not a general Thumb relocator: a future
// prologue, including ADD/MOV from PC or either ADR form, must sit out.
inline bool ntf_known_detour_prologue(const unsigned char *code, size_t size)
{
    static const unsigned char known[][8] = {
        {0x2d,0xe9,0xf0,0x43,0x89,0xb0,0x04,0x46}, // inline WirelessWatchdog::onBatteryLevel
        {0x2d,0xe9,0xf0,0x4f,0x8f,0xb0,0x06,0xaf}, // PriceReceiver::getPrice
        {0x2d,0xe9,0xf0,0x4f,0x89,0xb0,0x00,0xaf}, // shapeLine
        {0x2d,0xe9,0xf0,0x4f,0xb9,0xb0,0x02,0xaf}, // HarfBuzz NG
        {0x2d,0xe9,0xf0,0x4f,0xdb,0xb0,0x00,0xaf}, // HarfBuzz
        {0x2d,0xe9,0xf0,0x4f,0x8b,0xb0,0x00,0xaf}, // fontEngine
        {0xf8,0xb5,0x04,0x46,0xd0,0xf8,0x24,0x01}, // scheduleRelayout
    };
    if (!code || size < 8) return false;
    for (const auto &bytes : known)
        if (std::memcmp(code, bytes, sizeof bytes) == 0) return true;
    return false;
}
