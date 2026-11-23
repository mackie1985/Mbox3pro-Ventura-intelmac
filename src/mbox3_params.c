/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "mbox3_params.h"
#include <string.h>

/* DICE quadlets are big-endian on the wire. */
static uint32_t get_q(const uint8_t *r)
{
    return ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
           ((uint32_t)r[2] << 8) | (uint32_t)r[3];
}

static void put_q(uint8_t *r, uint32_t v)
{
    r[0] = v >> 24; r[1] = v >> 16; r[2] = v >> 8; r[3] = v;
}

/* Offsets (bytes) inside the application section. 0x04 is unknown. */
enum {
    OFF_USE_CASE = 0x00,
    OFF_KNOB_VALUE = 0x08,
    OFF_KNOB_ASSIGN = 0x0c,
    OFF_MUTE_LED = 0x10,
    OFF_MONO_SPKR_LED = 0x14, /* mono and spkr share one quadlet */
    OFF_DIM_LED = 0x1c,
    OFF_HOLD = 0x20,
    OFF_PHANTOM_HPF = 0x24,   /* phantom: bit0, HPF: bits 4..7 */
    OFF_TRIM0 = 0x28,         /* 6 quadlets */
    OFF_REVERB_TYPE = 0x40,
    OFF_REVERB_VOL = 0x44,
    OFF_REVERB_DUR = 0x48,
    OFF_REVERB_FB = 0x4c,
};

#define USE_CASE_MASK 0x03u
#define KNOB_ASSIGN_MASK 0x3fu
#define MUTE_MASK 0x03u
#define MUTE_BLINK_BIT 0x01u
#define MONO_MASK 0x0cu
#define SPKR_COLOR_MASK 0x60u
#define SPKR_COLOR_SHIFT 5
#define SPKR_BLINK_MASK 0x10u
#define PHANTOM_MASK 0x01u
#define HPF_MASK 0xf0u
#define HPF_SHIFT 4

enum { COL_NONE = 0, COL_GREEN = 1, COL_RED = 2, COL_ORANGE = 3 };

static int valid_reverb(uint32_t v)
{
    switch (v) {
    case MBOX3_REVERB_ROOM1: case MBOX3_REVERB_ROOM2: case MBOX3_REVERB_ROOM3:
    case MBOX3_REVERB_HALL1: case MBOX3_REVERB_HALL2: case MBOX3_REVERB_PLATE:
    case MBOX3_REVERB_DELAY: case MBOX3_REVERB_ECHO:
        return 1;
    }
    return 0;
}

int mbox3_params_serialize(const mbox3_params *p, uint8_t *raw, size_t len)
{
    if (!p || !raw || len < MBOX3_APP_SECTION_MIN_SIZE)
        return -1;
    if (!valid_reverb(p->reverb_type))
        return -1;

    /* Read-modify-write semantics: unknown bits in `raw` are preserved. */
    uint32_t v;

    v = get_q(raw + OFF_USE_CASE) & ~USE_CASE_MASK;
    put_q(raw + OFF_USE_CASE, v | (uint32_t)p->standalone_use_case);

    put_q(raw + OFF_KNOB_VALUE, p->master_knob_value);

    v = get_q(raw + OFF_KNOB_ASSIGN) & ~KNOB_ASSIGN_MASK;
    for (int i = 0; i < 6; i++)
        if (p->master_knob_assigns[i])
            v |= 1u << i;
    put_q(raw + OFF_KNOB_ASSIGN, v);

    v = get_q(raw + OFF_MUTE_LED) & ~MUTE_MASK;
    if (p->mute_led == MBOX3_MUTE_BLINK)
        v |= MUTE_BLINK_BIT;
    else if (p->mute_led == MBOX3_MUTE_ON)
        v |= MUTE_MASK & ~MUTE_BLINK_BIT;
    put_q(raw + OFF_MUTE_LED, v);

    v = get_q(raw + OFF_MONO_SPKR_LED) &
        ~(MONO_MASK | SPKR_COLOR_MASK | SPKR_BLINK_MASK);
    if (p->mono_led == MBOX3_MONO_ON)
        v |= MONO_MASK;
    uint32_t color = COL_NONE;
    bool blink = false;
    switch (p->spkr_led) {
    case MBOX3_SPKR_OFF: break;
    case MBOX3_SPKR_GREEN: color = COL_GREEN; break;
    case MBOX3_SPKR_GREEN_BLINK: color = COL_GREEN; blink = true; break;
    case MBOX3_SPKR_RED: color = COL_RED; break;
    case MBOX3_SPKR_RED_BLINK: color = COL_RED; blink = true; break;
    case MBOX3_SPKR_ORANGE: color = COL_ORANGE; break;
    /* NOTE: the reference implementation maps orange-blink to red-blink,
     * which looks like an upstream typo. We encode orange here; verify on
     * real hardware. */
    case MBOX3_SPKR_ORANGE_BLINK: color = COL_ORANGE; blink = true; break;
    default: return -1;
    }
    v |= color << SPKR_COLOR_SHIFT;
    if (blink)
        v |= SPKR_BLINK_MASK;
    put_q(raw + OFF_MONO_SPKR_LED, v);

    put_q(raw + OFF_DIM_LED, p->dim_led ? 1 : 0);
    put_q(raw + OFF_HOLD, p->duration_hold);

    v = get_q(raw + OFF_PHANTOM_HPF) & ~(PHANTOM_MASK | HPF_MASK);
    if (p->phantom_powering)
        v |= PHANTOM_MASK;
    for (int i = 0; i < 4; i++)
        if (p->hpf_enables[i])
            v |= 1u << (i + HPF_SHIFT);
    put_q(raw + OFF_PHANTOM_HPF, v);

    for (int i = 0; i < 6; i++)
        put_q(raw + OFF_TRIM0 + 4 * i, (uint8_t)(UINT8_MAX - p->output_trims[i]));

    put_q(raw + OFF_REVERB_TYPE, (uint32_t)p->reverb_type);
    put_q(raw + OFF_REVERB_VOL, p->reverb_volume);
    put_q(raw + OFF_REVERB_DUR, p->reverb_duration);
    put_q(raw + OFF_REVERB_FB, p->reverb_feedback);
    return 0;
}

int mbox3_params_deserialize(mbox3_params *p, const uint8_t *raw, size_t len)
{
    if (!p || !raw || len < MBOX3_APP_SECTION_MIN_SIZE)
        return -1;

    uint32_t v;
    memset(p, 0, sizeof(*p));

    p->standalone_use_case =
        (mbox3_standalone_use_case)(get_q(raw + OFF_USE_CASE) & USE_CASE_MASK);
    p->master_knob_value = get_q(raw + OFF_KNOB_VALUE) & 0xff;

    v = get_q(raw + OFF_KNOB_ASSIGN);
    for (int i = 0; i < 6; i++)
        p->master_knob_assigns[i] = (v >> i) & 1;

    v = get_q(raw + OFF_MUTE_LED);
    if ((v & MUTE_MASK) == 0)
        p->mute_led = MBOX3_MUTE_OFF;
    else if (v & MUTE_BLINK_BIT)
        p->mute_led = MBOX3_MUTE_BLINK;
    else
        p->mute_led = MBOX3_MUTE_ON;

    v = get_q(raw + OFF_MONO_SPKR_LED);
    p->mono_led = (v & MONO_MASK) ? MBOX3_MONO_ON : MBOX3_MONO_OFF;
    uint32_t color = (v & SPKR_COLOR_MASK) >> SPKR_COLOR_SHIFT;
    bool blink = (v & SPKR_BLINK_MASK) != 0;
    switch (color) {
    case COL_NONE: p->spkr_led = MBOX3_SPKR_OFF; break;
    case COL_GREEN: p->spkr_led = blink ? MBOX3_SPKR_GREEN_BLINK : MBOX3_SPKR_GREEN; break;
    case COL_RED: p->spkr_led = blink ? MBOX3_SPKR_RED_BLINK : MBOX3_SPKR_RED; break;
    default: p->spkr_led = blink ? MBOX3_SPKR_ORANGE_BLINK : MBOX3_SPKR_ORANGE; break;
    }

    p->dim_led = (get_q(raw + OFF_DIM_LED) & 0xff) != 0;
    p->duration_hold = get_q(raw + OFF_HOLD) & 0xff;

    v = get_q(raw + OFF_PHANTOM_HPF);
    p->phantom_powering = (v & PHANTOM_MASK) != 0;
    for (int i = 0; i < 4; i++)
        p->hpf_enables[i] = (v >> (i + HPF_SHIFT)) & 1;

    for (int i = 0; i < 6; i++)
        p->output_trims[i] =
            (uint8_t)(UINT8_MAX - (get_q(raw + OFF_TRIM0 + 4 * i) & 0xff));

    uint32_t rt = get_q(raw + OFF_REVERB_TYPE);
    if (!valid_reverb(rt))
        return -1;
    p->reverb_type = (mbox3_reverb_type)rt;
    p->reverb_volume = get_q(raw + OFF_REVERB_VOL) & 0xff;
    p->reverb_duration = get_q(raw + OFF_REVERB_DUR) & 0xff;
    p->reverb_feedback = get_q(raw + OFF_REVERB_FB) & 0xff;
    return 0;
}

bool mbox3_apply_notification(mbox3_params *p, uint32_t msg)
{
    mbox3_params old = *p;

    if (msg & MBOX3_NOTIFY_SPKR_PUSHED) {
        switch (old.spkr_led) { /* cycle off -> green -> red -> orange -> off */
        case MBOX3_SPKR_OFF: p->spkr_led = MBOX3_SPKR_GREEN; break;
        case MBOX3_SPKR_GREEN_BLINK:
        case MBOX3_SPKR_GREEN: p->spkr_led = MBOX3_SPKR_RED; break;
        case MBOX3_SPKR_RED_BLINK:
        case MBOX3_SPKR_RED: p->spkr_led = MBOX3_SPKR_ORANGE; break;
        case MBOX3_SPKR_ORANGE_BLINK:
        case MBOX3_SPKR_ORANGE: p->spkr_led = MBOX3_SPKR_OFF; break;
        }
    }
    if (msg & MBOX3_NOTIFY_SPKR_HELD) { /* toggle blink on current colour */
        switch (old.spkr_led) {
        case MBOX3_SPKR_OFF: break;
        case MBOX3_SPKR_GREEN: p->spkr_led = MBOX3_SPKR_GREEN_BLINK; break;
        case MBOX3_SPKR_GREEN_BLINK: p->spkr_led = MBOX3_SPKR_GREEN; break;
        case MBOX3_SPKR_RED: p->spkr_led = MBOX3_SPKR_RED_BLINK; break;
        case MBOX3_SPKR_RED_BLINK: p->spkr_led = MBOX3_SPKR_RED; break;
        case MBOX3_SPKR_ORANGE: p->spkr_led = MBOX3_SPKR_ORANGE_BLINK; break;
        case MBOX3_SPKR_ORANGE_BLINK: p->spkr_led = MBOX3_SPKR_ORANGE; break;
        }
    }
    if (msg & MBOX3_NOTIFY_MONO_PUSHED)
        p->mono_led = old.mono_led == MBOX3_MONO_ON ? MBOX3_MONO_OFF : MBOX3_MONO_ON;
    if (msg & MBOX3_NOTIFY_MUTE_PUSHED)
        p->mute_led = old.mute_led == MBOX3_MUTE_ON ? MBOX3_MUTE_OFF : MBOX3_MUTE_ON;
    if (msg & MBOX3_NOTIFY_MUTE_HELD) {
        if (old.mute_led == MBOX3_MUTE_BLINK) p->mute_led = MBOX3_MUTE_ON;
        else if (old.mute_led == MBOX3_MUTE_ON) p->mute_led = MBOX3_MUTE_BLINK;
    }
    return memcmp(&old, p, sizeof(old)) != 0;
}
