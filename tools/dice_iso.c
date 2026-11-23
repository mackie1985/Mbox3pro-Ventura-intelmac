/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "dice_iso.h"
#include "dice_io.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define RING_PACKETS 512
#define PKT_PAYLOAD 512            /* max payload bytes per packet */
#define PKT_STRIDE (4 + PKT_PAYLOAD) /* 4-byte iso header + payload */
#define FILL_PATTERN 0xdeadbeefu

static uint32_t g_tx_iso_off;      /* DICE offset of the TX stream's iso channel quadlet */
static uint32_t g_rx_iso_off;      /* ... of the RX stream's */
static uint32_t g_enable_off;      /* global enable register */
static uint32_t g_status_off;      /* global status register */
static uint32_t g_clock_off;       /* global clock select register */
static int g_chan = -1;

/* Read the register offsets used by every streaming test. */
static int iso_init(void)
{
    uint8_t t[8];
    if (dice_read(0, t, 8) != kIOReturnSuccess) return -1;
    g_enable_off = dice_be32(t) * 4 + 0x50;
    g_status_off = dice_be32(t) * 4 + 0x54;
    g_clock_off = dice_be32(t) * 4 + 0x4c;
    if (dice_read(8, t, 8) != kIOReturnSuccess) return -1;
    g_tx_iso_off = dice_be32(t) * 4 + 8;
    if (dice_read(0x10, t, 8) != kIOReturnSuccess) return -1;
    g_rx_iso_off = dice_be32(t) * 4 + 8;
    return 0;
}

static IOReturn on_release(IOFireWireLibIsochPortRef p)
{
    (void)p;
    return dice_write_q(g_tx_iso_off, 0xffffffffu);
}

static IOReturn on_start(IOFireWireLibIsochPortRef p)
{
    (void)p;
    IOReturn r = dice_write_q(g_enable_off, 1);
    printf("[iso] enable on: 0x%08x\n", r);
    return r;
}

static IOReturn on_stop(IOFireWireLibIsochPortRef p)
{
    (void)p;
    IOReturn r = dice_write_q(g_enable_off, 0);
    printf("[iso] enable off: 0x%08x\n", r);
    return r;
}

static uint32_t be32p(const uint8_t *p) { return dice_be32(p); }

static void analyze(const uint8_t *ring)
{
    unsigned data_packets = 0, empty = 0, unwritten = 0, dbs_seen = 0;
    int32_t peak[8] = {0};
    int shown = 0;
    for (unsigned i = 0; i < RING_PACKETS; i++) {
        const uint8_t *b = ring + (size_t)i * PKT_STRIDE;
        uint32_t first;
        memcpy(&first, b, 4);
        if (first == FILL_PATTERN) { unwritten++; continue; }
        const uint8_t *cip = b + 4;
        uint32_t q0 = be32p(cip), q1 = be32p(cip + 4);
        unsigned dbs = (q0 >> 16) & 0xff, syt = q1 & 0xffff;
        if (shown < 3) {
            printf("[iso] pkt %u: hdr %02x%02x%02x%02x  cip %08x %08x  d0 %08x d1 %08x\n",
                   i, b[0], b[1], b[2], b[3], q0, q1, be32p(cip + 8), be32p(cip + 12));
            shown++;
        }
        if (syt == 0xffff || dbs == 0 || dbs > 32) { empty++; continue; }
        data_packets++;
        dbs_seen = dbs;
        for (unsigned blk = 0; blk < 8; blk++) {
            for (unsigned ch = 0; ch < 8 && ch < dbs; ch++) {
                size_t off = 8 + ((size_t)blk * dbs + ch) * 4;
                if (off + 4 > PKT_PAYLOAD) break;
                uint32_t q = be32p(cip + off);
                int32_t s = (int32_t)(q << 8) >> 8;
                if (s < 0) s = -s;
                if (s > peak[ch]) peak[ch] = s;
            }
        }
    }
    printf("[iso] ring: %u data packets, %u empty (no-data), %u never written, dbs=%u\n",
           data_packets, empty, unwritten, dbs_seen);
    for (int ch = 0; ch < 8; ch++)
        printf("[iso]   ch %d peak 0x%06x\n", ch, peak[ch]);
}


/* Continuity check on the ring, in ring order: data-block-counter gaps between
 * consecutive data packets and, if expect_freq != 0, jumps in channel 0 that a
 * clean sine of that frequency cannot make. The ring wraps once, so one break of
 * each kind is normal (see the seam note at the end of the function). */
static unsigned ring_continuity(const uint8_t *ring, double expect_freq, const char *label, int verbose)
{
    unsigned data_packets = 0, dbc_breaks = 0, glitches = 0, printed = 0;
    int have = 0;
    unsigned hist = 0;          /* valid samples in s1/s2 since the last break */
    unsigned quiet = 0;         /* samples to skip after a glitch (its echo in the recurrence) */
    uint32_t last_dbc = 0;
    double s1 = 0, s2 = 0;
    int32_t peak = 0;
    double c2 = expect_freq > 0 ? 2.0 * cos(2.0 * M_PI * expect_freq / 44100.0) : 0;
    for (unsigned i = 0; i < RING_PACKETS; i++) {
        const uint8_t *b = ring + (size_t)i * PKT_STRIDE;
        uint32_t first;
        memcpy(&first, b, 4);
        if (first == FILL_PATTERN) continue;
        const uint8_t *cip = b + 4;
        uint32_t q0 = dice_be32(cip), q1 = dice_be32(cip + 4);
        unsigned dbs = (q0 >> 16) & 0xff, syt = q1 & 0xffff;
        if (syt == 0xffff || dbs != 9) continue;
        data_packets++;
        uint32_t dbc = q0 & 0xff;
        if (have && dbc != ((last_dbc + 8) & 0xff)) { dbc_breaks++; hist = 0; quiet = 0; }
        last_dbc = dbc;
        have = 1;
        for (unsigned blk = 0; blk < 8; blk++) {
            uint32_t q = dice_be32(cip + 8 + (size_t)blk * 36);
            double smp = (double)((int32_t)(q << 8) >> 8);
            if (fabs(smp) > peak) peak = (int32_t)fabs(smp);
            if (c2 > 0 && hist >= 2 && quiet == 0) {
                double pred = c2 * s1 - s2;
                if (fabs(smp - pred) > 3000.0) {
                    glitches++;
                    quiet = 2;
                    if (verbose && printed < 4) {
                        printf("[cap]   glitch at pkt %u blk %u: prev2 %.0f prev1 %.0f got %.0f predicted %.0f\n",
                               i, blk, s2, s1, smp, pred);
                        printed++;
                    }
                }
            } else if (quiet > 0) quiet--;
            s2 = s1; s1 = smp;
            if (hist < 2) hist++;
        }
    }
    /* The snapshot is read while the DMA writes, so it holds one seam (old data
     * joined to new data at the write pointer). If the data-block counter happens
     * to be continuous across it (352 data packets in 512 slots instead of 353),
     * the seam shows up as one sine jump with no dbc break. Count that one as the
     * seam, not as a glitch. */
    if (dbc_breaks == 0 && glitches > 0) glitches--;
    if (verbose)
        printf("[cap] %s: %u data packets, dbc breaks %u, glitches %u, ch0 peak 0x%06x\n",
               label, data_packets, dbc_breaks, glitches, peak);
    return glitches;
}

typedef struct {
    IOFireWireLibIsochChannelRef ch;
    uint8_t *ring;
} cap_t;

static IOReturn tx_on_allocate(IOFireWireLibIsochPortRef p, IOFWSpeed speed, UInt32 chan)
{
    (void)p;
    printf("[iso] TX channel %u allocated (speed %d)\n", chan, (int)speed);
    g_chan = (int)chan;
    return dice_write_q(g_tx_iso_off, chan);
}

/* Create the TX channel (device -> host) and start it. */
static int cap_begin(cap_t *c)
{
    IOFireWireLibDeviceRef dev = dice_device();
    (*dev)->AddCallbackDispatcherToRunLoop(dev, CFRunLoopGetCurrent());
    (*dev)->AddIsochCallbackDispatcherToRunLoop(dev, CFRunLoopGetCurrent());

    c->ch = (*dev)->CreateIsochChannel(dev, true, PKT_PAYLOAD,
        kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!c->ch) { fprintf(stderr, "CreateIsochChannel failed\n"); return 1; }

    IOFireWireLibRemoteIsochPortRef remote = (*dev)->CreateRemoteIsochPort(dev, true,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!remote) { fprintf(stderr, "CreateRemoteIsochPort failed\n"); return 1; }
    (*remote)->SetAllocatePortHandler(remote, tx_on_allocate);
    (*remote)->SetReleasePortHandler(remote, on_release);
    (*remote)->SetStartHandler(remote, on_start);
    (*remote)->SetStopHandler(remote, on_stop);

    c->ring = valloc((size_t)RING_PACKETS * PKT_STRIDE);
    if (!c->ring) return 1;
    for (size_t i = 0; i < (size_t)RING_PACKETS * PKT_STRIDE; i += 4)
        memcpy(c->ring + i, &(uint32_t){FILL_PATTERN}, 4);

    IOFireWireLibNuDCLPoolRef pool = (*dev)->CreateNuDCLPool(dev, RING_PACKETS,
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!pool) { fprintf(stderr, "CreateNuDCLPool failed\n"); return 1; }
    CFMutableSetRef bag = CFSetCreateMutable(kCFAllocatorDefault, 0, NULL);
    NuDCLRef first = NULL, last = NULL;
    for (unsigned i = 0; i < RING_PACKETS; i++) {
        IOVirtualRange range = { (IOVirtualAddress)(c->ring + (size_t)i * PKT_STRIDE), PKT_STRIDE };
        NuDCLRef d = (NuDCLRef)(*pool)->AllocateReceivePacket(pool, bag, 4, 1, &range);
        if (!d) { fprintf(stderr, "AllocateReceivePacket %u failed\n", i); return 1; }
        if (!first) first = d;
        last = d;
    }
    (*pool)->SetDCLBranch(last, first);

    IOFireWireLibLocalIsochPortRef local = (*dev)->CreateLocalIsochPort(dev, false,
        (*pool)->GetProgram(pool), 0, 0, 0, NULL, 0, NULL, 0,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!local) { fprintf(stderr, "CreateLocalIsochPort failed\n"); return 1; }

    IOReturn r;
    if ((r = (*c->ch)->SetTalker(c->ch, (IOFireWireLibIsochPortRef)remote)) != kIOReturnSuccess ||
        (r = (*c->ch)->AddListener(c->ch, (IOFireWireLibIsochPortRef)local)) != kIOReturnSuccess) {
        fprintf(stderr, "SetTalker/AddListener failed: 0x%08x\n", r);
        return 1;
    }
    if ((r = (*c->ch)->AllocateChannel(c->ch)) != kIOReturnSuccess) {
        fprintf(stderr, "AllocateChannel failed: 0x%08x\n", r);
        return 1;
    }
    if ((r = (*c->ch)->Start(c->ch)) != kIOReturnSuccess) {
        fprintf(stderr, "channel Start failed: 0x%08x\n", r);
        (*c->ch)->ReleaseChannel(c->ch);
        return 1;
    }
    return 0;
}

static void cap_end(cap_t *c)
{
    (*c->ch)->Stop(c->ch);
    (*c->ch)->ReleaseChannel(c->ch);
}

int dice_iso_capture(unsigned secs)
{
    if (!dice_device() || iso_init() != 0) return 1;
    printf("[iso] enable at +0x%x, TX iso channel at +0x%x\n", g_enable_off, g_tx_iso_off);
    cap_t c;
    if (cap_begin(&c) != 0) return 1;
    printf("[iso] streaming on channel %d for %u s\n", g_chan, secs);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs, false);
    analyze(c.ring);
    ring_continuity(c.ring, 0, "final", 1);
    cap_end(&c);
    return 0;
}

/* ===================== playback (host -> device) ===================== */

#define PLAY_SLOTS 640        /* one cycle of the 44.1 kHz packet pattern: 441 data packets */
#define PLAY_DATA_BYTES 296   /* CIP header (8) + 8 blocks x 9 quadlets */
#define PLAY_NODATA_BYTES 8
#define PLAY_STRIDE 304
#define PLAY_RATE 44100u
#define PLAY_DATA_PER_LAP 441u

static volatile unsigned g_cb_count;

static void on_slot_cb(void *refcon, NuDCLRef dcl)
{
    (void)refcon; (void)dcl;
    g_cb_count++;
}

static IOReturn rx_on_allocate(IOFireWireLibIsochPortRef p, IOFWSpeed speed, UInt32 chan)
{
    (void)p;
    g_chan = (int)chan;
    printf("[play] channel %u allocated (speed %d); writing RX iso channel\n", chan, (int)speed);
    return dice_write_q(g_rx_iso_off, chan);
}

static IOReturn rx_on_release(IOFireWireLibIsochPortRef p)
{
    (void)p;
    return dice_write_q(g_rx_iso_off, 0xffffffffu);
}

/* Slot s carries data iff the Bresenham counter crosses 8 samples there. */
static int slot_is_data(unsigned s)
{
    return ((uint64_t)(s + 1) * PLAY_RATE / 64000u) > ((uint64_t)s * PLAY_RATE / 64000u);
}
static uint64_t data_before(unsigned s) { return (uint64_t)s * PLAY_RATE / 64000u; }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

typedef struct {
    uint8_t *buf;
    uint32_t sid;
    unsigned latency;
    double freq;
} play_ctx;

/* Fill slot s for ring lap `lap`; `cycle` is the bus cycle (0..7999) the slot
 * will be transmitted in. */
static void fill_slot(const play_ctx *c, unsigned s, unsigned lap, unsigned cycle)
{
    uint8_t *b = c->buf + (size_t)s * PLAY_STRIDE;
    uint64_t n = data_before(s) + (uint64_t)PLAY_DATA_PER_LAP * lap; /* data packets before this slot */
    uint32_t dbc = (uint32_t)((n * 8) & 0xff);
    uint32_t q0 = (c->sid << 24) | (9u << 16) | dbc;
    uint32_t syt = 0xffff;
    if (slot_is_data(s)) {
        /* ideal presentation time of this packet minus the slot's own cycle position, in 24.576 MHz ticks */
        int64_t ideal = (int64_t)(data_before(s) * 196608000ull / PLAY_RATE);
        int64_t off = ideal - (int64_t)s * 3072;
        int64_t tick = (int64_t)((cycle + c->latency) % 16) * 3072 + off;
        tick %= 49152;
        if (tick < 0) tick += 49152;
        syt = (uint32_t)((((tick / 3072) & 0xf) << 12) | (tick % 3072));
    }
    put32(b, q0);
    put32(b + 4, 0x90010000u | syt);
    if (!slot_is_data(s)) return;
    uint64_t first_sample = n * 8;
    for (unsigned blk = 0; blk < 8; blk++) {
        double v = sin(2.0 * M_PI * c->freq * (double)(first_sample + blk) / PLAY_RATE) * 0x180000;
        int32_t smp = (int32_t)v;
        uint8_t *q = b + 8 + blk * 36;
        put32(q, 0x40000000u | ((uint32_t)smp & 0xffffff));
        put32(q + 4, 0x40000000u | ((uint32_t)smp & 0xffffff));
        for (unsigned ch = 2; ch < 8; ch++) put32(q + ch * 4, 0x40000000u);
        put32(q + 32, 0x80000000u);
    }
}


typedef struct {
    IOFireWireLibIsochChannelRef ch;
    play_ctx ctx;
    unsigned base;
    unsigned refills, shown;
    unsigned late;          /* slots whose send interval was not exactly one lap */
    double max_gap_ms;      /* longest time between two service calls */
    double last_ms;
} play_t;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint16_t g_last_cycle[PLAY_SLOTS];

static volatile uint32_t g_ts[PLAY_SLOTS];
static unsigned g_lap[PLAY_SLOTS];

/* Create the RX channel (host -> device) with the send ring and start it. */
static int play_begin(play_t *p, unsigned latency, unsigned freq)
{
    IOFireWireLibDeviceRef dev = dice_device();
    uint16_t node = 0;
    if (dice_local_node_id(&node) != 0) { fprintf(stderr, "cannot read local node id\n"); return 1; }

    (*dev)->AddCallbackDispatcherToRunLoop(dev, CFRunLoopGetCurrent());
    (*dev)->AddIsochCallbackDispatcherToRunLoop(dev, CFRunLoopGetCurrent());

    p->ch = (*dev)->CreateIsochChannel(dev, true, PLAY_STRIDE,
        kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!p->ch) { fprintf(stderr, "CreateIsochChannel failed\n"); return 1; }

    IOFireWireLibRemoteIsochPortRef remote = (*dev)->CreateRemoteIsochPort(dev, false,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!remote) { fprintf(stderr, "CreateRemoteIsochPort failed\n"); return 1; }
    (*remote)->SetAllocatePortHandler(remote, rx_on_allocate);
    (*remote)->SetReleasePortHandler(remote, rx_on_release);
    (*remote)->SetStartHandler(remote, on_start);
    (*remote)->SetStopHandler(remote, on_stop);

    p->ctx = (play_ctx){ valloc((size_t)PLAY_SLOTS * PLAY_STRIDE), node & 0x3f, latency, freq };
    if (!p->ctx.buf) return 1;
    memset(p->ctx.buf, 0, (size_t)PLAY_SLOTS * PLAY_STRIDE);
    for (unsigned s = 0; s < PLAY_SLOTS; s++) { g_ts[s] = 0xffffffffu; g_lap[s] = 0; }

    IOFireWireLibNuDCLPoolRef pool = (*dev)->CreateNuDCLPool(dev, PLAY_SLOTS,
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!pool) { fprintf(stderr, "CreateNuDCLPool failed\n"); return 1; }
    (*pool)->SetCurrentTagAndSync(pool, 1, 0);

    CFMutableSetRef bag = CFSetCreateMutable(kCFAllocatorDefault, 0, NULL);
    NuDCLRef dcls[PLAY_SLOTS];
    for (unsigned s = 0; s < PLAY_SLOTS; s++) {
        IOVirtualRange range = { (IOVirtualAddress)(p->ctx.buf + (size_t)s * PLAY_STRIDE),
                                 slot_is_data(s) ? PLAY_DATA_BYTES : PLAY_NODATA_BYTES };
        NuDCLRef d = (NuDCLRef)(*pool)->AllocateSendPacket(pool, bag, 1, &range);
        if (!d) { fprintf(stderr, "AllocateSendPacket %u failed\n", s); return 1; }
        (*pool)->SetDCLTimeStampPtr(d, (UInt32 *)&g_ts[s]);
        (*pool)->SetDCLFlags(d, kNuDCLDynamic | kNuDCLUpdateBeforeCallback);
        dcls[s] = d;
    }
    (*pool)->SetDCLBranch(dcls[PLAY_SLOTS - 1], dcls[0]);
    for (unsigned s = 15; s < PLAY_SLOTS; s += 16) {
        (*pool)->SetDCLCallback(dcls[s], on_slot_cb);
        for (unsigned j = s - 15; j <= s; j++) (*pool)->AppendDCLUpdateList(dcls[s], dcls[j]);
    }

    uint32_t ct = 0;
    (*dev)->GetCycleTime(dev, &ct);
    p->base = (((ct >> 12) & 0x1fff) + 1600) % 8000;
    for (unsigned s = 0; s < PLAY_SLOTS; s++) fill_slot(&p->ctx, s, 0, (p->base + s) % 8000);

    IOFireWireLibLocalIsochPortRef local = (*dev)->CreateLocalIsochPort(dev, true,
        (*pool)->GetProgram(pool), 0, 0, 0, NULL, 0, NULL, 0,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!local) { fprintf(stderr, "CreateLocalIsochPort failed\n"); return 1; }

    IOReturn r;
    if ((r = (*p->ch)->SetTalker(p->ch, (IOFireWireLibIsochPortRef)local)) != kIOReturnSuccess ||
        (r = (*p->ch)->AddListener(p->ch, (IOFireWireLibIsochPortRef)remote)) != kIOReturnSuccess) {
        fprintf(stderr, "SetTalker/AddListener failed: 0x%08x\n", r);
        return 1;
    }
    if ((r = (*p->ch)->AllocateChannel(p->ch)) != kIOReturnSuccess) {
        fprintf(stderr, "AllocateChannel failed: 0x%08x\n", r);
        return 1;
    }
    if ((r = (*p->ch)->Start(p->ch)) != kIOReturnSuccess) {
        fprintf(stderr, "channel Start failed: 0x%08x\n", r);
        (*p->ch)->ReleaseChannel(p->ch);
        return 1;
    }
    /* The kernel zeroes the timestamp words when the program is created;
     * clear them again so only real completions are seen. */
    for (unsigned s = 0; s < PLAY_SLOTS; s++) g_ts[s] = 0xffffffffu;
    p->refills = 0;
    p->shown = 0;
    p->late = 0;
    p->max_gap_ms = 0;
    p->last_ms = now_ms();
    for (unsigned s = 0; s < PLAY_SLOTS; s++) g_last_cycle[s] = 0xffff;
    return 0;
}

/* Refill every slot the hardware has sent since the last call. */
static void play_service(play_t *p)
{
    double t = now_ms();
    if (t - p->last_ms > p->max_gap_ms) p->max_gap_ms = t - p->last_ms;
    p->last_ms = t;
    for (unsigned s = 0; s < PLAY_SLOTS; s++) {
        uint32_t v = g_ts[s];
        if (v == 0xffffffffu) continue;
        g_ts[s] = 0xffffffffu;
        if (p->shown < 3) {
            printf("[play] slot %u sent, timestamp 0x%08x (cycle %u; primed for %u)\n",
                   s, v, (v >> 12) & 0x1fff, (p->base + s) % 8000);
            p->shown++;
        }
        unsigned cyc = (v >> 12) & 0x1fff;
        if (g_last_cycle[s] != 0xffff && (cyc + 8000 - g_last_cycle[s]) % 8000 != PLAY_SLOTS) p->late++;
        g_last_cycle[s] = (uint16_t)cyc;
        g_lap[s]++;
        fill_slot(&p->ctx, s, g_lap[s], (cyc + PLAY_SLOTS) % 8000);
        p->refills++;
    }
}

static void play_end(play_t *p)
{
    (*p->ch)->Stop(p->ch);
    (*p->ch)->ReleaseChannel(p->ch);
}

static void print_peaks(const char *tag, unsigned sec)
{
    uint8_t sb[4];
    if (dice_read(g_status_off, sb, 4) == kIOReturnSuccess)
        printf("[%s] device status after %u s: 0x%08x\n", tag, sec, dice_be32(sb));
    uint32_t poff, psize;
    if (dice_ext_section(3, &poff, &psize) == 0) {
        uint8_t pk[16];
        if (dice_read(poff, pk, sizeof(pk)) == kIOReturnSuccess)
            for (int e = 0; e < 4; e++) {
                uint32_t v = dice_be32(pk + e * 4);
                printf("[%s]   peak[%d] route %04x level 0x%04x\n", tag, e, v & 0xffff, v >> 16);
            }
    }
}

int dice_iso_play(unsigned secs, unsigned latency, unsigned freq)
{
    if (!dice_device() || iso_init() != 0) return 1;
    printf("[play] enable at +0x%x, RX iso channel at +0x%x, latency %u cycles, tone %u Hz\n",
           g_enable_off, g_rx_iso_off, latency, freq);
    play_t p;
    if (play_begin(&p, latency, freq) != 0) return 1;
    printf("[play] streaming for %u s\n", secs);
    for (unsigned i = 0; i < secs * 500; i++) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
        play_service(&p);
        if (i == 500 || i == 1000) print_peaks("play", i / 500);
    }
    printf("[play] done: %u slot refills (%.1f laps), %u callbacks, late slots %u, longest gap %.1f ms\n",
           p.refills, (double)p.refills / PLAY_SLOTS, g_cb_count, p.late, p.max_gap_ms);
    play_end(&p);
    return 0;
}

/* Capture and playback at once. Route Avs0:0 -> Avs0:0 (0xb0b0) beforehand to
 * loop the played tone back to the host and verify it sample by sample. */
int dice_iso_duplex(unsigned secs, unsigned latency, unsigned freq, int clock_src)
{
    if (!dice_device() || iso_init() != 0) return 1;
    printf("[duplex] latency %u cycles, tone %u Hz, %u s, clock source after 1 s: %s\n", latency, freq, secs,
           clock_src < 0 ? "unchanged" : "switch");
    cap_t c;
    play_t p;
    if (cap_begin(&c) != 0) return 1;
    if (play_begin(&p, latency, freq) != 0) { cap_end(&c); return 1; }
    unsigned snaps = 0, bad_snaps = 0, total_glitches = 0;
    int verbose_cap = getenv("DICE_QUIET") ? 0 : 1;
    for (unsigned i = 0; i < secs * 500; i++) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
        play_service(&p);
        if (clock_src >= 0 && i == 500) {
            IOReturn r = dice_write_q(g_clock_off, (1u << 8) | (unsigned)clock_src);
            printf("[duplex] clock select -> source 0x%02x, 44100 Hz: 0x%08x\n", clock_src, r);
        }
        if (clock_src >= 0 && (i == 550 || i == 1500)) {
            uint8_t sb[4];
            if (dice_read(g_status_off, sb, 4) == kIOReturnSuccess)
                printf("[duplex] device status: 0x%08x (bit0 = locked)\n", dice_be32(sb));
        }
        if (i % 500 == 499) {
            char label[32];
            snprintf(label, sizeof(label), "t=%us", (i + 1) / 500);
            unsigned g = ring_continuity(c.ring, freq, label, verbose_cap);
            snaps++;
            if (g) { bad_snaps++; total_glitches += g; }
        }
    }
    analyze(c.ring);
    printf("[duplex] latency %u: %u of %u snapshots had glitches, %u glitches in total\n",
           latency, bad_snaps, snaps, total_glitches);
    printf("[duplex] play: %u slot refills (%.1f laps), late slots %u, longest gap between service calls %.1f ms\n",
           p.refills, (double)p.refills / PLAY_SLOTS, p.late, p.max_gap_ms);
    play_end(&p);
    cap_end(&c);
    return 0;
}
