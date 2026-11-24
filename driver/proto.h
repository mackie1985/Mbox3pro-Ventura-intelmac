/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Shared-memory protocol between mbox3d (the daemon that owns the FireWire
 * device) and the Audio Server plug-in running inside coreaudiod.
 *
 * The plug-in connects to the mach service M3_SERVICE with XPC, sends any
 * dictionary, and gets back a reply holding the shared memory region
 * ("shm", an xpc_shmem object). Everything after that is plain memory.
 *
 * Time base: CoreAudio sample time == capture frame index. The daemon counts
 * the frames it has received from the device since the stream started (cap_w)
 * and publishes base_ns, the (smoothed) mach_absolute_time-based nanosecond
 * time at which frame 0 was captured, so
 *     sample time T happens at base_ns + T * 1e9 / rate (rate = cur_rate).
 * The plug-in hands CoreAudio exactly that as its zero timestamp.
 *
 * Capture:  frame T is at cap[T & mask]; valid while T < cap_w and
 *           T >= cap_w - ring_frames.
 * Playback: the plug-in writes frame T at play[T & mask]. The daemon takes
 *           frames in order starting at index 0 (play_pos = next one) as the
 *           device clock advances, then zeroes them, so stale audio is never
 *           replayed. Frames the plug-in writes after the daemon has taken
 *           them are lost. The daemon keeps M3_PREFILL_FRAMES of silence
 *           queued, so a frame written at sample time T is audible about
 *           M3_OUT_LATENCY frames after T.
 */
#ifndef MBOX3_PROTO_H
#define MBOX3_PROTO_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define M3_SERVICE "com.mbox3pro.driver.daemon"
#define M3_MAGIC 0x4d334452u            /* 'M3DR' */
#define M3_VERSION 5u

#define M3_CHANNELS 8
#define M3_SAMPLE_RATE 44100            /* the default rate; 48000, 88200 and 96000 are the others (cur_rate / req_rate) */
#define M3_RATE_OK(r) ((r) == 44100u || (r) == 48000u || (r) == 88200u || (r) == 96000u)
#define M3_RATE_MULT(r) ((r) > 48000u ? 2u : 1u)   /* frame-count constants below are for 44.1 / 48 kHz; at 88.2 / 96 kHz they double (same time) */
#define M3_RING_FRAMES 32768u           /* power of two */
#define M3_HEADER_SIZE 4096u

/* Output pipeline defaults (v0.2.0, low latency; verified on hardware at 44.1 kHz, see DESIGN section 32).
 * The older conservative values were prefill 768, queue 512, send look-ahead 224 slots:
 * scripts/tune.sh 768 512 224 brings them back (the reported latency below is then too small by about 1250 frames). */
#define M3_PREFILL_FRAMES 384u          /* silence queued ahead of the audio */
#define M3_QUEUE_FRAMES 320u            /* what the pump keeps queued in the library; the daemon takes frame T when the device
                                           clock is at T + (prefill - queue), the time CoreAudio has to deliver it */
#define M3_TX_AHEAD_SLOTS 64u           /* send look-ahead in slots of 125 us (5.5 frames each) */
#define M3_OUT_LATENCY_FOR(rate) (M3_PREFILL_FRAMES * M3_RATE_MULT(rate) + (rate) * 384u / 44100u)  /* 768 at 44100; the look-ahead part is 64 x 5.5 + about 32 frames of converters */
#define M3_OUT_LATENCY M3_OUT_LATENCY_FOR(44100u)
#define M3_IN_LATENCY_FOR(rate) (128u * M3_RATE_MULT(rate))
#define M3_IN_LATENCY 128u
/* Safety offsets (v0.2.0): output 192, input 192 (were 1024 / 512). An input safety of 96 gave noise on recording. */
#define M3_OUT_SAFETY_FOR(rate) (192u * M3_RATE_MULT(rate))
#define M3_OUT_SAFETY 192u             /* CoreAudio may not write closer than this to "now" */
#define M3_IN_SAFETY_FOR(rate) (192u * M3_RATE_MULT(rate))
#define M3_IN_SAFETY 192u
#define M3_NOM_LEAD_FOR(rate) (1024 * (int)M3_RATE_MULT(rate))
#define M3_NOM_LEAD 1024               /* where the plug-in puts its write cursor relative to the daemon's read position when it has lost it */
#define M3_MAX_LEAD 16384              /* a lead beyond this is not a lead (half the ring) */
#define M3_JUMP_TOLERANCE_FOR(rate) (2048 * (int)M3_RATE_MULT(rate))
#define M3_JUMP_TOLERANCE 2048          /* CoreAudio's cycle start wobbles by up to a few ms; the plug-in
                                           keeps writing/reading contiguously. Beyond this it resyncs. */

enum { M3_STATE_NO_DEVICE = 0, M3_STATE_STARTING = 1, M3_STATE_STREAMING = 2 };

typedef struct {
    uint32_t magic, version;
    uint32_t channels, ring_frames, sample_rate, header_size;
    _Atomic uint32_t heartbeat;         /* incremented by the daemon every loop */
    _Atomic uint32_t state;             /* M3_STATE_* */
    _Atomic uint32_t seed;              /* changes whenever the timeline restarts */
    uint32_t reserved0;
    _Atomic uint64_t cap_w;             /* frames captured since the stream started */
    _Atomic uint64_t play_pos;          /* next playback frame index the daemon takes */
    _Atomic uint64_t base_ns;           /* ns (mach_absolute_time base) of capture frame 0 */
    _Atomic uint32_t underruns, overruns, dbc_errors, late_slots;
    _Atomic uint32_t clock_locked;
    /* fitted timeline: capture frame T happens at anchor_ns + (T - anchor_frame) * nsf.
     * Seqlock: tl_seq is odd while the daemon writes. nsf is a double stored as bits. */
    _Atomic uint32_t max_pump_gap_us;   /* longest time between two passes of the daemon's pump loop */
    _Atomic uint32_t max_gap_us;        /* longest pause of the library's IO thread since streaming started */
    _Atomic uint32_t tl_seq;
    _Atomic uint32_t tl_valid;
    _Atomic uint64_t tl_anchor_frame;
    _Atomic uint64_t tl_anchor_ns;
    _Atomic uint64_t tl_nsf_bits;
    /* written by the plug-in; the watcher resets the minimums */
    _Atomic uint32_t io_cycles;           /* output cycles seen */
    _Atomic uint32_t late_write_frames;   /* output frames dropped: the daemon had already taken them */
    _Atomic int32_t write_margin_min;     /* min of (first frame written - play_pos), frames */
    _Atomic uint32_t late_read_frames;    /* input frames requested before they were captured */
    _Atomic int32_t read_margin_min;      /* min of (cap_w - end of the requested input), frames */
    _Atomic uint32_t io_jumps;            /* output cycles that began more than M3_JUMP_TOLERANCE from where the last one ended */
    _Atomic int32_t io_jump_max;          /* the largest such jump, frames (signed) */
    _Atomic uint32_t io_wobble;           /* output cycles that began a few frames off (absorbed) */
    _Atomic int32_t io_wobble_max;        /* the largest of those, frames */
    _Atomic uint32_t in_jumps;            /* the same for input cycles */
    _Atomic uint64_t io_wobble_abs_sum;   /* sum of |cycle start - cursor| over all output cycles, frames */
    _Atomic uint32_t io_wobble_gt128;     /* output cycles off by more than 128 frames */
    /* sample rate switching */
    _Atomic uint32_t req_rate;            /* written by the plug-in: the rate CoreAudio wants (44100, 48000, 88200 or 96000) */
    _Atomic uint32_t plug_out_peak;       /* plug-in: peak |sample| (24 bit) CoreAudio handed over for output; the watcher resets it */
    _Atomic uint32_t ahead_write_frames;  /* output frames dropped: written more than a ring ahead of the daemon */
    _Atomic uint32_t io_frames_last, io_frames_max;   /* frames per output cycle, last and largest */
    _Atomic uint32_t play_peak;           /* daemon: peak |sample| (24 bit) of the playback audio handed to the device in the last 250 ms */
    _Atomic uint32_t cur_rate;            /* written by the daemon: the rate the running session streams at, 0 = none */
    /* plug-in: wall-clock time between two output cycles (what CoreAudio's IO thread really does) */
    _Atomic uint32_t io_drift_fixes;      /* times the write cursor was moved to follow CoreAudio's time (dropped cycles) */
    _Atomic int64_t io_drift_frames;      /* the sum of those moves, frames (positive = CoreAudio's time ran ahead of the cursor) */
    _Atomic uint32_t wake_gap_max_us;     /* the largest gap since the watcher last reset it */
    _Atomic uint32_t wake_over5, wake_over10, wake_over20;   /* gaps longer than 5, 10, 20 ms (a 128 frame cycle is 2.9 ms) */
} m3_shm_header;

/* header, then cap[ring][channels], then play[ring][channels] (int32, 24 bit in the low bits) */
#define M3_RING_BYTES ((size_t)M3_RING_FRAMES * M3_CHANNELS * sizeof(int32_t))
#define M3_SHM_SIZE ((size_t)M3_HEADER_SIZE + 2 * M3_RING_BYTES)

static inline int32_t *m3_cap_ring(m3_shm_header *h)
{
    return (int32_t *)((char *)h + M3_HEADER_SIZE);
}
static inline int32_t *m3_play_ring(m3_shm_header *h)
{
    return (int32_t *)((char *)h + M3_HEADER_SIZE + M3_RING_BYTES);
}

#endif
