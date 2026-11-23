/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "../src/mbox3_params.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    mbox3_params p;
    memset(&p, 0, sizeof(p));
    p.standalone_use_case = MBOX3_USE_AD_DA;
    p.master_knob_value = 0xf9;
    bool assigns[6] = {true, false, true, true, false, true};
    memcpy(p.master_knob_assigns, assigns, sizeof(assigns));
    p.mute_led = MBOX3_MUTE_BLINK;
    p.mono_led = MBOX3_MONO_ON;
    p.spkr_led = MBOX3_SPKR_RED_BLINK;
    p.duration_hold = 10;
    p.phantom_powering = true;
    p.hpf_enables[2] = true;
    for (int i = 0; i < 6; i++) p.output_trims[i] = i;
    p.reverb_type = MBOX3_REVERB_HALL2;
    p.reverb_volume = 0xb3;
    p.reverb_duration = 0xa5;
    p.reverb_feedback = 0x17;

    uint8_t raw[MBOX3_APP_SECTION_MIN_SIZE] = {0};
    assert(mbox3_params_serialize(&p, raw, sizeof(raw)) == 0);

    mbox3_params q;
    assert(mbox3_params_deserialize(&q, raw, sizeof(raw)) == 0);
    assert(memcmp(&p, &q, sizeof(p)) == 0);

    /* Wire-format spot checks: big-endian quadlets. */
    assert(raw[0x00 + 3] == MBOX3_USE_AD_DA);
    assert(raw[0x08 + 3] == 0xf9);
    assert(raw[0x24 + 3] == (0x01 | (1 << (2 + 4))));
    assert(raw[0x28 + 3] == 0xff);          /* trim 0 -> 255 */
    assert(raw[0x40 + 3] == 0x08);          /* hall 2 */

    /* Too-short buffers and bad reverb are rejected. */
    assert(mbox3_params_serialize(&p, raw, 0x4c) == -1);
    memset(raw, 0, sizeof(raw));
    assert(mbox3_params_deserialize(&q, raw, sizeof(raw)) == -1); /* type 0 */

    /* Notification state machine. */
    memset(&p, 0, sizeof(p));
    p.reverb_type = MBOX3_REVERB_ROOM1;
    assert(mbox3_apply_notification(&p, MBOX3_NOTIFY_SPKR_PUSHED));
    assert(p.spkr_led == MBOX3_SPKR_GREEN);
    assert(mbox3_apply_notification(&p, MBOX3_NOTIFY_MUTE_PUSHED));
    assert(p.mute_led == MBOX3_MUTE_ON);
    assert(!mbox3_apply_notification(&p, 0));

    /* Real dump from an Mbox 3 Pro (application section, 0x50 bytes). The
     * decode -> encode round trip must reproduce it bit for bit, otherwise
     * dice_ctl would rewrite quadlets it was not asked to change. */
    static const uint32_t dump[20] = {
        0x00000001, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x00000121, 0x0000003f, 0x00000001,
        0x00000032, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000004, 0x0000003f, 0x0000003f, 0x0000003f,
    };
    uint8_t real[MBOX3_APP_SECTION_MIN_SIZE], again[MBOX3_APP_SECTION_MIN_SIZE];
    for (int i = 0; i < 20; i++) {
        real[i * 4] = dump[i] >> 24; real[i * 4 + 1] = dump[i] >> 16;
        real[i * 4 + 2] = dump[i] >> 8; real[i * 4 + 3] = dump[i];
    }
    memcpy(again, real, sizeof(real));
    mbox3_params rp;
    assert(mbox3_params_deserialize(&rp, real, sizeof(real)) == 0);
    assert(rp.standalone_use_case == MBOX3_USE_AD_DA);
    assert(rp.spkr_led == MBOX3_SPKR_GREEN && rp.dim_led && rp.duration_hold == 50);
    assert(rp.reverb_type == MBOX3_REVERB_ROOM2 && rp.master_knob_assigns[1]);
    assert(mbox3_params_serialize(&rp, again, sizeof(again)) == 0);
    assert(memcmp(real, again, sizeof(real)) == 0);

    puts("mbox3_params: all tests passed");
    return 0;
}
