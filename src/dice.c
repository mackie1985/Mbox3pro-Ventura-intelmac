/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "dice.h"
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* The device's max_rec is 512 bytes and the mixer section only answers
 * quadlet transactions, so try 256-byte blocks first, then single quadlets. */
int dice_read(fw_transport *t, uint32_t off, void *buf, size_t len)
{
    static const size_t sizes[] = {256, 4};
    int rc = 0;
    if (!t || !buf || (len & 3))
        return -1;
    for (unsigned s = 0; s < 2; s++) {
        size_t done = 0;
        rc = 0;
        while (done < len) {
            size_t n = len - done < sizes[s] ? len - done : sizes[s];
            rc = t->ops->read_block(t, DICE_BASE + off + done,
                                    (uint8_t *)buf + done, n);
            if (rc)
                break;
            done += n;
        }
        if (!rc)
            return 0;
    }
    return rc;
}

int dice_read_quadlet(fw_transport *t, uint32_t off, uint32_t *val)
{
    uint8_t b[4];
    int rc = dice_read(t, off, b, 4);
    if (!rc)
        *val = be32(b);
    return rc;
}

int dice_write_quadlet(fw_transport *t, uint32_t off, uint32_t val)
{
    uint8_t b[4];
    put_be32(b, val);
    return t->ops->write_block(t, DICE_BASE + off, b, 4);
}

int dice_discover(fw_transport *t, dice_layout *l)
{
    uint8_t tbl[0x20], ext[EXT_COUNT * 8];
    int rc;

    memset(l, 0, sizeof(*l));
    rc = dice_read(t, 0, tbl, sizeof(tbl));
    if (rc)
        return rc;
    /* Offsets and sizes are stored in quadlets. */
    l->global_off = be32(tbl + 0x00) * 4; l->global_size = be32(tbl + 0x04) * 4;
    l->tx_off = be32(tbl + 0x08) * 4;     l->tx_size = be32(tbl + 0x0c) * 4;
    l->rx_off = be32(tbl + 0x10) * 4;     l->rx_size = be32(tbl + 0x14) * 4;
    l->sync_off = be32(tbl + 0x18) * 4;   l->sync_size = be32(tbl + 0x1c) * 4;
    if (l->global_size == 0 || l->global_off == 0)
        return -2; /* not a DICE device */

    rc = dice_read(t, DICE_EXT_BASE, ext, sizeof(ext));
    if (rc)
        return rc;
    for (int i = 0; i < EXT_COUNT; i++) {
        l->ext_off[i] = DICE_EXT_BASE + be32(ext + i * 8) * 4;
        l->ext_size[i] = be32(ext + i * 8 + 4) * 4;
    }
    if (l->ext_size[EXT_APPLICATION] < MBOX3_APP_SECTION_MIN_SIZE)
        return -3; /* not the Mbox 3 Pro application layout */
    return 0;
}

int dice_app_read(fw_transport *t, const dice_layout *l, mbox3_params *p)
{
    uint8_t raw[MBOX3_APP_SECTION_MIN_SIZE];
    int rc = dice_read(t, l->ext_off[EXT_APPLICATION], raw, sizeof(raw));
    if (rc)
        return rc;
    return mbox3_params_deserialize(p, raw, sizeof(raw)) ? -4 : 0;
}

int dice_app_update(fw_transport *t, const dice_layout *l,
                    const mbox3_params *newp, int *written)
{
    uint8_t old[MBOX3_APP_SECTION_MIN_SIZE], nw[MBOX3_APP_SECTION_MIN_SIZE];
    int rc, n = 0;

    if (written)
        *written = 0;
    rc = dice_read(t, l->ext_off[EXT_APPLICATION], old, sizeof(old));
    if (rc)
        return rc;
    memcpy(nw, old, sizeof(nw)); /* unknown bits are preserved */
    if (mbox3_params_serialize(newp, nw, sizeof(nw)))
        return -4;
    for (size_t pos = 0; pos < sizeof(nw); pos += 4) {
        if (memcmp(old + pos, nw + pos, 4) == 0)
            continue;
        rc = dice_write_quadlet(t, l->ext_off[EXT_APPLICATION] + pos, be32(nw + pos));
        if (rc)
            return rc;
        n++;
    }
    if (written)
        *written = n;
    return 0;
}

static int mixer_cell(const dice_layout *l, unsigned out, unsigned in, uint32_t *off)
{
    if (out >= DICE_MIXER_OUTPUTS || in >= DICE_MIXER_INPUTS)
        return -1;
    uint32_t rel = DICE_MIXER_HEAD_BYTES + (out * DICE_MIXER_INPUTS + in) * 4;
    if (rel + 4 > l->ext_size[EXT_MIXER])
        return -1;
    *off = l->ext_off[EXT_MIXER] + rel;
    return 0;
}

int dice_mixer_get(fw_transport *t, const dice_layout *l,
                   unsigned out, unsigned in, uint32_t *val)
{
    uint32_t off;
    if (mixer_cell(l, out, in, &off))
        return -1;
    return dice_read_quadlet(t, off, val);
}

int dice_mixer_set(fw_transport *t, const dice_layout *l,
                   unsigned out, unsigned in, uint32_t val)
{
    uint32_t off;
    if (mixer_cell(l, out, in, &off))
        return -1;
    return dice_write_quadlet(t, off, val);
}

int dice_peaks_read(fw_transport *t, const dice_layout *l,
                    uint32_t *entries, size_t max, size_t *count)
{
    uint8_t buf[0x200];
    size_t n = l->ext_size[EXT_PEAK];
    if (n > sizeof(buf))
        n = sizeof(buf);
    int rc = dice_read(t, l->ext_off[EXT_PEAK], buf, n);
    if (rc)
        return rc;
    size_t c = n / 4 < max ? n / 4 : max;
    for (size_t i = 0; i < c; i++)
        entries[i] = be32(buf + i * 4);
    *count = c;
    return 0;
}

const char *dice_src_block_name(unsigned id)
{
    switch (id) {
    case 0x0: return "AES"; case 0x1: return "ADAT"; case 0x2: return "Mixer";
    case 0x4: return "Ins0"; case 0x5: return "Ins1"; case 0x6: return "ARM";
    case 0xb: return "Avs0"; case 0xc: return "Avs1"; case 0xf: return "Mute";
    }
    return "?";
}

const char *dice_dst_block_name(unsigned id)
{
    switch (id) {
    case 0x0: return "AES"; case 0x1: return "ADAT"; case 0x2: return "MixerTx0";
    case 0x3: return "MixerTx1"; case 0x4: return "Ins0"; case 0x5: return "Ins1";
    case 0x6: return "ARM"; case 0xb: return "Avs0"; case 0xc: return "Avs1";
    }
    return "?";
}
