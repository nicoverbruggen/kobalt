/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/display/mt8113_hwtcon_frame.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    MT8113HWTCONFrame frames[64] = {0};
    uint8_t source[64], panel[68], original[68];
    const unsigned pitch = 16;

    memset(panel, 0xee, sizeof(panel));
    memcpy(original, panel, sizeof(panel));
    /* Stage twelve independent stripes before any waveform starts. Use
     * nonconsecutive LUTs and mutate the DMA source after each capture. */
    for (unsigned stripe = 0; stripe < 12; stripe++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 16; x++) {
                source[y * pitch + x] = x + y * 16;
            }
        }
        unsigned lut = (5 + stripe * 7) % 64;
        assert(mt8113_hwtcon_frame_capture(&frames[lut], source,
                   12, 4, pitch, 1, 7, 0x100, stripe, 1, 1, 2));
        memset(source, 0, sizeof(source));
    }
    assert(memcmp(panel, original, sizeof(panel)) == 0);
    for (int stripe = 11; stripe >= 0; stripe--) {
        unsigned lut = (5 + stripe * 7) % 64;
        assert(frames[lut].minimum == stripe + 16);
        assert(frames[lut].maximum == stripe + 32);
        assert(frames[lut].flags == 0x100);
        assert(mt8113_hwtcon_frame_publish(&frames[lut], panel, 64, pitch));
        assert(!mt8113_hwtcon_frame_publish(&frames[lut], panel, 64, pitch));
    }
    for (unsigned i = 0; i < sizeof(panel); i++) {
        unsigned y = i / pitch, x = i % pitch;
        assert(panel[i] == ((y == 1 || y == 2) && x < 12 ? x + y * 16 : 0xee));
    }
    /* A reused LUT owns only its latest snapshot. Invalid captures and
     * failed publication discard it rather than replaying obsolete pixels. */
    memset(source, 11, sizeof(source));
    assert(mt8113_hwtcon_frame_capture(&frames[0], source, 12, 4, pitch,
                                     1, 7, 0, 1, 1, 2, 2));
    memset(source, 22, sizeof(source));
    assert(mt8113_hwtcon_frame_capture(&frames[0], source, 12, 4, pitch,
                                     1, 7, 0, 1, 1, 2, 2));
    assert(mt8113_hwtcon_frame_publish(&frames[0], panel, 64, pitch));
    assert(panel[17] == 22);
    assert(mt8113_hwtcon_frame_capture(&frames[0], source, 12, 4, pitch,
                                     1, 7, 0, 1, 1, 2, 2));
    assert(!mt8113_hwtcon_frame_capture(&frames[0], source, 12, 4, pitch,
                                      1, 7, 0, 12, 0, 1, 1));
    assert(!frames[0].pixels);
    assert(!mt8113_hwtcon_frame_capture(&frames[0], source, 4097, 4, pitch,
                                      1, 7, 0, 0, 0, 1, 1));
    assert(!mt8113_hwtcon_frame_capture(&frames[0], source, 12, 4, 11,
                                      1, 7, 0, 0, 0, 1, 1));
    assert(mt8113_hwtcon_frame_capture(&frames[0], source, 12, 4, pitch,
                                     1, 7, 0, 1, 1, 2, 2));
    memcpy(original, panel, sizeof(panel));
    assert(!mt8113_hwtcon_frame_publish(&frames[0], panel, 63, pitch));
    assert(!frames[0].pixels && !memcmp(panel, original, sizeof(panel)));
    /* RGBA snapshots retain all channels and preserve destination padding. */
    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = i;
    }
    memset(panel, 0xee, sizeof(panel));
    assert(mt8113_hwtcon_frame_capture(&frames[63], source, 3, 4, pitch,
                                     4, 2, 0, 1, 1, 2, 2));
    assert(mt8113_hwtcon_frame_publish(&frames[63], panel, 64, pitch));
    for (unsigned i = 0; i < sizeof(panel); i++) {
        unsigned y = i / pitch, x = i % pitch;
        assert(panel[i] == ((y == 1 || y == 2) && x >= 4 && x < 12 ? i : 0xee));
    }
    for (unsigned lut = 0; lut < 64; lut++) {
        mt8113_hwtcon_frame_clear(&frames[lut]);
    }
    puts("HWTCON per-LUT snapshots, deferred stripe commits, ownership and bounds passed.");
}
