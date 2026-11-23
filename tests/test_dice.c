/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Unit test of the DICE layer against a memory-backed fake device that mimics
 * the quirks seen on real hardware (mixer answers quadlet reads only). */
#include "../src/dice.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPACE_SIZE 0x208000u

static uint8_t *space;
static int nwrites;
static uint32_t mixer_lo, mixer_hi;

static void put(uint32_t off, uint32_t v)
{
    space[off] = v >> 24; space[off + 1] = v >> 16;
    space[off + 2] = v >> 8; space[off + 3] = v;
}

static int fake_read(fw_transport *t, uint64_t addr, void *buf, size_t len)
{
    (void)t;
    uint64_t off = addr - DICE_BASE;
    if (off + len > SPACE_SIZE) return -1;
    if (len > 4 && off < mixer_hi && off + len > mixer_lo)
        return (int)0xe0008017; /* FireWire "address error" */
    memcpy(buf, space + off, len);
    return 0;
}

static int fake_write(fw_transport *t, uint64_t addr, const void *buf, size_t len)
{
    (void)t;
    uint64_t off = addr - DICE_BASE;
    if (off + len > SPACE_SIZE) return -1;
    memcpy(space + off, buf, len);
    nwrites++;
    return 0;
}

static const fw_transport_ops ops = { .read_block = fake_read, .write_block = fake_write };

int main(void)
{
    space = calloc(1, SPACE_SIZE);
    fw_transport t = { .ops = &ops, .priv = NULL };

    /* Section table (values in quadlets), as seen on the real device. */
    put(0x00, 0x28 / 4); put(0x04, 0x168 / 4);
    put(0x08, 0x190 / 4); put(0x0c, 0x238 / 4);
    put(0x10, 0x3c8 / 4); put(0x14, 0x468 / 4);
    put(0x18, 0x830 / 4); put(0x1c, 0x10 / 4);
    static const uint32_t ext[EXT_COUNT][2] = {
        {0x4c, 0x10}, {0x5c, 0x8}, {0x64, 0x484}, {0x4e8, 0x200}, {0x6e8, 0x204},
        {0x8ec, 0x438}, {0xd24, 0x6000}, {0x6d24, 0x40}, {0x6d64, 0x50},
    };
    for (int i = 0; i < EXT_COUNT; i++) {
        put(DICE_EXT_BASE + i * 8, ext[i][0] / 4);
        put(DICE_EXT_BASE + i * 8 + 4, ext[i][1] / 4);
    }

    dice_layout l;
    assert(dice_discover(&t, &l) == 0);
    assert(l.global_off == 0x28 && l.tx_off == 0x190 && l.sync_size == 0x10);
    assert(l.ext_off[EXT_MIXER] == DICE_EXT_BASE + 0x64);
    assert(l.ext_size[EXT_APPLICATION] == 0x50);
    mixer_lo = l.ext_off[EXT_MIXER];
    mixer_hi = mixer_lo + l.ext_size[EXT_MIXER];

    /* Application section: values observed on hardware. */
    static const uint32_t app[] = {
        0x1, 0x0, 0x0, 0x3, 0x0, 0x121, 0x3f, 0x1, 0x32, 0, 0, 0, 0, 0, 0, 0,
        0x4, 0x3f, 0x3f, 0x3f };
    for (unsigned i = 0; i < sizeof(app) / 4; i++)
        put(l.ext_off[EXT_APPLICATION] + i * 4, app[i]);

    mbox3_params p;
    assert(dice_app_read(&t, &l, &p) == 0);
    assert(p.standalone_use_case == MBOX3_USE_AD_DA);
    assert(p.reverb_type == MBOX3_REVERB_ROOM2 && p.reverb_volume == 0x3f);
    assert(p.spkr_led == MBOX3_SPKR_GREEN && p.dim_led && p.duration_hold == 0x32);
    assert(!p.phantom_powering);

    /* Unchanged params must not write anything (unknown bits preserved). */
    int w = -1;
    nwrites = 0;
    assert(dice_app_update(&t, &l, &p, &w) == 0);
    assert(w == 0 && nwrites == 0);

    /* One change = exactly one quadlet written, others untouched. */
    p.phantom_powering = true;
    assert(dice_app_update(&t, &l, &p, &w) == 0);
    assert(w == 1 && nwrites == 1);
    uint32_t q;
    assert(dice_read_quadlet(&t, l.ext_off[EXT_APPLICATION] + 0x24, &q) == 0 && q == 1);
    assert(dice_read_quadlet(&t, l.ext_off[EXT_APPLICATION] + 0x14, &q) == 0 && q == 0x121);

    /* Mixer: whole-section read falls back to quadlets; cell round-trip. */
    uint8_t whole[0x484];
    assert(dice_read(&t, l.ext_off[EXT_MIXER], whole, sizeof(whole)) == 0);
    assert(dice_mixer_set(&t, &l, 6, 0, 0x4000) == 0);
    assert(dice_mixer_get(&t, &l, 6, 0, &q) == 0 && q == 0x4000);
    assert(dice_read_quadlet(&t, l.ext_off[EXT_MIXER] + 4 + 6 * 18 * 4, &q) == 0 && q == 0x4000);
    assert(dice_mixer_set(&t, &l, 16, 0, 1) != 0);  /* out of range */
    assert(dice_mixer_set(&t, &l, 0, 18, 1) != 0);

    /* Peaks. */
    put(l.ext_off[EXT_PEAK], 0x00194020);
    uint32_t pk[128]; size_t n = 0;
    assert(dice_peaks_read(&t, &l, pk, 128, &n) == 0 && n == 128 && pk[0] == 0x00194020);

    free(space);
    puts("dice layer: all tests passed");
    return 0;
}
