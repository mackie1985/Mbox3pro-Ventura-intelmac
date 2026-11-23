/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * DICE / TCAT access layer on top of fw_transport. OS independent, so it can
 * be unit-tested with a fake transport (tests/test_dice.c).
 */
#ifndef DICE_H
#define DICE_H

#include "fw_transport.h"
#include "mbox3_params.h"

#define DICE_BASE 0xffffe0000000ull   /* confirmed on hardware */
#define DICE_EXT_BASE 0x00200000u     /* TCAT extension, relative to DICE_BASE */

enum {
    EXT_CAPS, EXT_COMMAND, EXT_MIXER, EXT_PEAK, EXT_ROUTER,
    EXT_STREAM_FORMAT, EXT_CURRENT_CONFIG, EXT_STANDALONE, EXT_APPLICATION,
    EXT_COUNT
};

#define DICE_MIXER_INPUTS 18
#define DICE_MIXER_OUTPUTS 16
/* ASSUMPTION (to be verified on hardware): one quadlet precedes the matrix,
 * and the matrix is output-major: cell = (out * 18 + in). */
#define DICE_MIXER_HEAD_BYTES 4

typedef struct {
    uint32_t global_off, global_size;
    uint32_t tx_off, tx_size;
    uint32_t rx_off, rx_size;
    uint32_t sync_off, sync_size;
    /* Absolute offsets from DICE_BASE (extension base already added). */
    uint32_t ext_off[EXT_COUNT];
    uint32_t ext_size[EXT_COUNT];
} dice_layout;

/* All functions return 0 on success, nonzero on failure. Offsets are relative
 * to DICE_BASE. Data buffers hold big-endian wire bytes. */
int dice_read(fw_transport *t, uint32_t off, void *buf, size_t len);
int dice_write_quadlet(fw_transport *t, uint32_t off, uint32_t val);
int dice_read_quadlet(fw_transport *t, uint32_t off, uint32_t *val);

int dice_discover(fw_transport *t, dice_layout *l);

int dice_app_read(fw_transport *t, const dice_layout *l, mbox3_params *p);
/* Writes only the quadlets that differ from the device's current state.
 * `written` (optional) receives the number of quadlets written. */
int dice_app_update(fw_transport *t, const dice_layout *l,
                    const mbox3_params *newp, int *written);

int dice_mixer_get(fw_transport *t, const dice_layout *l,
                   unsigned out, unsigned in, uint32_t *val);
int dice_mixer_set(fw_transport *t, const dice_layout *l,
                   unsigned out, unsigned in, uint32_t val);

/* Peak section: each entry = (level << 16) | (src << 8) | dst. */
int dice_peaks_read(fw_transport *t, const dice_layout *l,
                    uint32_t *entries, size_t max, size_t *count);

const char *dice_src_block_name(unsigned id);
const char *dice_dst_block_name(unsigned id);

#endif /* DICE_H */
