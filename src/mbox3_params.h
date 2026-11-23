/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Avid Mbox 3 Pro: application-section parameters (DICE / TCAT extension).
 *
 * Register layout is based on the reverse-engineering work in
 * firewire-dice-protocols (snd-firewire-ctl-services, (c) 2020 Takashi Sakamoto,
 * LGPL-3.0-or-later). This is an independent C re-implementation.
 *
 * Pure logic only: no OS dependencies, so it can be unit-tested anywhere.
 */
#ifndef MBOX3_PARAMS_H
#define MBOX3_PARAMS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Size of the application section region covered by the known layout. */
#define MBOX3_APP_SECTION_MIN_SIZE 0x50

typedef enum {
    MBOX3_USE_MIXER = 0x00,
    MBOX3_USE_AD_DA = 0x01,
    MBOX3_USE_PREAMP = 0x02,
    MBOX3_USE_UNDEFINED = 0x03,
} mbox3_standalone_use_case;

typedef enum { MBOX3_MUTE_OFF, MBOX3_MUTE_BLINK, MBOX3_MUTE_ON } mbox3_mute_led;
typedef enum { MBOX3_MONO_OFF, MBOX3_MONO_ON } mbox3_mono_led;
typedef enum {
    MBOX3_SPKR_OFF,
    MBOX3_SPKR_GREEN, MBOX3_SPKR_GREEN_BLINK,
    MBOX3_SPKR_RED, MBOX3_SPKR_RED_BLINK,
    MBOX3_SPKR_ORANGE, MBOX3_SPKR_ORANGE_BLINK,
} mbox3_spkr_led;

typedef enum {
    MBOX3_REVERB_ROOM1 = 0x01,
    MBOX3_REVERB_ROOM2 = 0x04,
    MBOX3_REVERB_ROOM3 = 0x05,
    MBOX3_REVERB_HALL1 = 0x06,
    MBOX3_REVERB_HALL2 = 0x08,
    MBOX3_REVERB_PLATE = 0x0b,
    MBOX3_REVERB_DELAY = 0x13,
    MBOX3_REVERB_ECHO = 0x14,
} mbox3_reverb_type;

typedef struct {
    mbox3_standalone_use_case standalone_use_case;
    uint8_t master_knob_value;
    bool master_knob_assigns[6];
    mbox3_mute_led mute_led;
    mbox3_mono_led mono_led;
    mbox3_spkr_led spkr_led;
    bool dim_led;
    uint8_t duration_hold;
    bool phantom_powering;
    bool hpf_enables[4];
    uint8_t output_trims[6]; /* user-facing value; wire value is 255 - trim */
    mbox3_reverb_type reverb_type;
    uint8_t reverb_volume;
    uint8_t reverb_duration;
    uint8_t reverb_feedback;
} mbox3_params;

/* Notification bits (quadlet notification from the device). */
#define MBOX3_NOTIFY_PHANTOM_CHANGED 0x10000000u
#define MBOX3_NOTIFY_MASTER_KNOB_CHANGED 0x08000000u
#define MBOX3_NOTIFY_SPKR_PUSHED 0x04000000u
#define MBOX3_NOTIFY_SPKR_HELD 0x02000000u
#define MBOX3_NOTIFY_MONO_PUSHED 0x00800000u
#define MBOX3_NOTIFY_MUTE_PUSHED 0x00400000u
#define MBOX3_NOTIFY_MUTE_HELD 0x00200000u

/* Return 0 on success, -1 on invalid argument / unknown enum value. */
int mbox3_params_serialize(const mbox3_params *p, uint8_t *raw, size_t len);
int mbox3_params_deserialize(mbox3_params *p, const uint8_t *raw, size_t len);

/*
 * The device does not toggle LED state itself on button events; the host is
 * expected to. Apply the notification message to `p` (LED state machine).
 * Returns true if anything changed (caller should then write the diff).
 */
bool mbox3_apply_notification(mbox3_params *p, uint32_t msg);

#endif /* MBOX3_PARAMS_H */
