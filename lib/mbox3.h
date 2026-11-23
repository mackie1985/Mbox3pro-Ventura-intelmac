/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * libmbox3: Avid Mbox 3 Pro (FireWire, TCAT DICE) streaming and control for
 * macOS, on top of IOFireWireLib.
 *
 * Lifecycle:
 *     mbox3 *m;
 *     mbox3_open(&m);        // opens the device, snapshots its stream config,
 *                            // takes ownership (the device then stops its
 *                            // standalone routing until streaming starts)
 *     mbox3_start(m, NULL);  // clock, router, stream config, isochronous
 *                            // streams, clock slaved to the host stream
 *     ... mbox3_write_frames() / mbox3_read_frames() ...
 *     mbox3_stop(m);
 *     mbox3_close(m);        // releases ownership
 *
 * Audio format: 44.1, 48, 88.2 or 96 kHz (cfg.sample_rate), 8 channels in each direction, interleaved frames of
 * MBOX3_CHANNELS int32 values. Each value is a signed 24-bit sample held in
 * the low 24 bits (range -8388608..8388607), sign-extended.
 *   channels 0-5: analog 1-6, channels 6-7: S/PDIF 1-2 (default routing)
 * MIDI is not carried yet.
 *
 * Threads: the library runs its own IO thread. mbox3_write_frames() and
 * mbox3_read_frames() are lock-free and meant for one producer and one
 * consumer thread each (the audio IO thread of the caller). Everything else
 * may be called from any thread, one call at a time.
 *
 * Limits: one instance per process, the first FireWire device found, 44.1 and
 * 48 kHz only. After a session the device keeps muted outputs until it is power
 * cycled (its standalone routing is lost while the host owned it).
 */
#ifndef MBOX3_H
#define MBOX3_H

#include <stddef.h>
#include "../src/mbox3_params.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBOX3_CHANNELS 8
#define MBOX3_SAMPLE_RATE 44100
#define MBOX3_MIXER_INPUTS 18
#define MBOX3_MIXER_OUTPUTS 16
#define MBOX3_MIXER_UNITY 0x4000

enum {
    MBOX3_OK = 0,
    MBOX3_ERR_NODEV = -1,    /* no FireWire device found / cannot open it */
    MBOX3_ERR_IO = -2,       /* register access or device command failed */
    MBOX3_ERR_BUSY = -3,     /* wrong state, e.g. already started */
    MBOX3_ERR_OWNED = -4,    /* another node owns the device */
    MBOX3_ERR_LOCK = -5,     /* clock did not lock to the host stream */
    MBOX3_ERR_ARG = -6,      /* unsupported or invalid argument */
    MBOX3_ERR_NOSNAP = -7,   /* stream config snapshot is unavailable */
    MBOX3_ERR_ISOCH = -8     /* isochronous setup failed */
};

enum {
    MBOX3_NO_MIXER_DEFAULTS = 1u << 0  /* do not write the default mixer (out n <- in n) */
};

typedef struct {
    unsigned sample_rate;      /* 0 = 44100; 44100, 48000, 88200 and 96000 are supported, others are rejected */
    unsigned syt_latency;      /* presentation offset in bus cycles (125 us), 0 = 6; valid 2..10 */
    unsigned ring_frames;      /* audio ring size per direction, power of two, 0 = 16384 */
    const uint32_t *routes;    /* router entries (src<<8 | dst), NULL = built-in default */
    unsigned n_routes;
    unsigned flags;            /* MBOX3_* flags */
    unsigned prefill_frames;   /* frames of silence queued when audio starts (adds that much output latency) */
    unsigned tx_ahead_slots;   /* audio is put into a send slot this many slots (1 slot = 125 us) before it goes out;
                                  0 = 224 (28 ms). Valid 48..480. More survives longer IO thread stalls, and adds
                                  5.5 frames of output latency per slot. */
} mbox3_config;

typedef struct {
    int running;               /* streaming and audio is flowing */
    int clock_locked;          /* device reports the clock source as locked */
    uint64_t capture_frames;   /* frames received from the device since start */
    uint64_t playback_frames;  /* frames taken from the playback ring since start */
    unsigned playback_queued;  /* frames waiting in the playback ring */
    unsigned capture_queued;   /* frames waiting in the capture ring */
    unsigned playback_underruns; /* packets sent as silence because the ring was empty */
    unsigned capture_overruns;   /* frames dropped because the capture ring was full */
    unsigned capture_dbc_errors; /* gaps in the device's data block counter (lost packets) */
    unsigned late_slots;       /* send slots refilled late (should stay 0) */
    double max_service_gap_ms; /* longest pause of the IO thread */
    uint64_t capture_host_ns;  /* mach_absolute_time-based ns of the latest received packet batch */
} mbox3_stats;

/* Give the calling thread real-time (time-constraint) scheduling with the
 * given period. The library does this for its own IO thread; a program that
 * pumps audio in and out (mbox3d) should do it for that thread too. */
void mbox3_make_thread_realtime(unsigned period_us);

typedef void (*mbox3_log_fn)(const char *msg);
/* Optional diagnostics sink (called from any thread). NULL = silent. */
void mbox3_set_log(mbox3_log_fn fn);

typedef struct mbox3 mbox3;

const char *mbox3_strerror(int err);

int mbox3_open(mbox3 **out);
void mbox3_close(mbox3 *m);

/* cfg may be NULL for the defaults. Returns after the clock is locked to the
 * host stream (about 2 s) and audio is flowing, or with an error. */
int mbox3_start(mbox3 *m, const mbox3_config *cfg);
int mbox3_stop(mbox3 *m);

/* Playback: queue up to n frames; returns how many were accepted. Silence is
 * sent whenever the ring runs dry. The ring is consumed about 80 ms before
 * the audio is heard, so that is the minimum output latency for now. */
size_t mbox3_write_frames(mbox3 *m, const int32_t *frames, size_t n);

/* Capture: copy up to n frames; returns how many were available. */
size_t mbox3_read_frames(mbox3 *m, int32_t *frames, size_t n);

void mbox3_get_stats(mbox3 *m, mbox3_stats *st);

/* Mixer (device ext section). value 0..0x7fff, MBOX3_MIXER_UNITY = 0 dB.
 * Cell (out, in): in 0-7 = host playback, 8-15 = hardware inputs, 16-17 =
 * reverb returns (layout inferred from device dumps). */
int mbox3_mixer_set(mbox3 *m, unsigned out, unsigned in, unsigned value);
int mbox3_mixer_get(mbox3 *m, unsigned out, unsigned in, unsigned *value);

/* Raw peak-meter entries, (level << 16) | (src << 8) | dst in router order.
 * Returns the number of entries read, or a negative error. */
int mbox3_read_peaks(mbox3 *m, uint32_t *entries, unsigned max_entries);

/* Application section (phantom power, HPF, output trims, reverb, LEDs). The set
 * call writes only the quadlets that differ from what the device holds. */
int mbox3_params_get(mbox3 *m, mbox3_params *p);
int mbox3_params_set(mbox3 *m, const mbox3_params *p);

/* The whole mixer matrix: out-major, MBOX3_MIXER_OUTPUTS x MBOX3_MIXER_INPUTS
 * values. Also returns the saturation flags quadlet (may be NULL). */
int mbox3_mixer_read_all(mbox3 *m, uint16_t *cells, uint32_t *saturation);

#ifdef __cplusplus
}
#endif

#endif /* MBOX3_H */
