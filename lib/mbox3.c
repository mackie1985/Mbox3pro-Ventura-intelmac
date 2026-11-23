/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "mbox3.h"
#include "dice_io.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

/* ---------------------------------------------------------------- constants */

#define OWNER_NONE 0xffff000000000000ull
#define NOTIFY_PLACEHOLDER 0x000100000000ull

#define EXECUTE_FLAG 0x80000000u
#define OP_LOAD_ROUTER 1u
#define OP_LOAD_STREAM 2u
#define RATE_FLAG_LOW 0x00010000u   /* 44.1 / 48 kHz mode */
#define RATE_FLAG_MID 0x00020000u   /* 88.2 / 96 kHz mode */

#define STREAM_SNAP_SIZE 0x438

/* clock select register: rate index in bits 8..15 (1 = 44.1 kHz, 2 = 48 kHz),
 * source in the low byte (0x0c = internal crystal, 0x08 = slave to the host stream) */
#define CLOCK_SRC_INTERNAL 0x0cu
#define CLOCK_SRC_ARX1 0x08u

#define RX_PACKETS 512                 /* capture ring, 64 ms */
#define FILL_PATTERN 0xdeadbeefu

#define TX_AHEAD_DEFAULT 224           /* audio is put into a slot this many slots (28 ms) before it is sent */
#define TX_AHEAD_MIN 48
#define TX_AHEAD_MAX 480               /* the ring has 640 slots; the refill needs the rest */
#define TX_SLOTS 640                   /* one lap of the packet pattern: 441 data packets at 44.1 kHz, 480 at 48 kHz */
#define TX_BLOCK_BYTES 36              /* 8 PCM + 1 MIDI quadlet */
#define TX_NODATA_BYTES 8

/* The sample rate of the running (or starting) session. One instance per process. */
static unsigned g_rate = 44100;
static unsigned rate_index(void) { return g_rate == 96000 ? 4u : g_rate == 88200 ? 3u : g_rate == 48000 ? 2u : 1u; }
/* Data blocks (frames) per packet: 8 up to 48 kHz, 16 at 88.2 / 96 kHz. The packet cadence is the same as at half the rate. */
static unsigned bpp(void) { return g_rate > 48000 ? 16u : 8u; }
static uint32_t rate_flag(void) { return g_rate > 48000 ? RATE_FLAG_MID : RATE_FLAG_LOW; }
static unsigned tx_data_bytes(void) { return 8 + bpp() * TX_BLOCK_BYTES; }
static unsigned tx_chan_payload(void) { return bpp() == 16 ? 592 : 304; }   /* what the isochronous channel reserves */
static unsigned rx_chan_payload(void) { return bpp() == 16 ? 1024 : 512; }
/* Buffer geometry follows the channel reservation exactly (as it did at 44.1 / 48 kHz: 516 / 304). */
#define RX_STRIDE (4 + rx_chan_payload())      /* 4-byte iso header + payload */
#define TX_STRIDE tx_chan_payload()            /* 8 + 8 x 36 = 296 -> 304; 8 + 16 x 36 = 584 -> 592 */
static uint32_t clock_sel(uint32_t src) { return (rate_index() << 8) | src; }
#define TX_DATA_PER_LAP (g_rate * 8u / (100u * bpp()))   /* data packets per lap of the ring: 441 or 480 */

#define WARMUP_MIXER_MS 300
#define WARMUP_CLOCK_MS 1000
#define LOCK_MIN_WAIT_MS 500
#define LOCK_SETTLE_MS 300
#define LOCK_TIMEOUT_MS 2500   /* a lock that is going to happen shows within LOCK_MIN_WAIT_MS + LOCK_SETTLE_MS (about 0.8 s) */

/* The 40-entry router table the device held in standalone mode, captured from
 * hardware before taking ownership: analog in 1-6 and S/PDIF in -> host
 * stream (Avs0:0-7); host stream -> mixer inputs 0-7; analog/S/PDIF in ->
 * mixer inputs 8-15; mixer outputs 0-7 -> analog/S/PDIF out; mixer 0/1 ->
 * headphones and line outputs. */
static const uint32_t DEFAULT_ROUTES[40] = {
    0x000040b0, 0x000041b1, 0x000042b2, 0x000043b3, 0x000044b4, 0x000045b5, 0x000000b6, 0x000001b7,
    0x0000b020, 0x0000b121, 0x0000b222, 0x0000b323, 0x0000b424, 0x0000b525, 0x0000b626, 0x0000b727,
    0x00004028, 0x00004129, 0x0000422a, 0x0000432b, 0x0000442c, 0x0000452d, 0x0000002e, 0x0000012f,
    0x00005030, 0x00005131,
    0x00002040, 0x00002141, 0x00002242, 0x00002343, 0x00002444, 0x00002545, 0x00002600, 0x00002701,
    0x00002850, 0x00002951, 0x00002a52, 0x00002b53,
    0x0000b054, 0x0000b155,
};

/* ---------------------------------------------------------------- instance */

enum { ST_IDLE, ST_STARTING, ST_WARMUP, ST_RUN, ST_STOPPING };
enum { CMD_NONE, CMD_START, CMD_STOP, CMD_QUIT };

struct mbox3 {
    uint16_t node;
    uint32_t global;                 /* global section offset */
    uint32_t enable_off, status_off, clock_off, tx_iso_off, rx_iso_off;
    uint8_t snap[3][STREAM_SNAP_SIZE];
    int snap_ok;
    int owned;

    pthread_t thread;
    int thread_started;
    pthread_mutex_t lock;            /* guards state/cmd/result and cond */
    pthread_cond_t cond;
    pthread_mutex_t reg;             /* serializes register access across threads */
    int state, cmd, result;
    mbox3_config cfg;
    uint32_t cfg_routes[64];
    unsigned ring_frames;

    /* audio rings: frame counters grow forever, index = counter & mask */
    int32_t *cap_buf, *play_buf;
    atomic_uint_fast64_t cap_w, cap_r, play_w, play_r;

    /* IO thread state */
    uint64_t t_start_ms, t_clock_ms, t_lock_since_ms, t_poll_ms;
    int mixer_done, clock_switched;

    /* receive (device -> host) */
    IOFireWireLibIsochChannelRef rx_ch;
    IOFireWireLibRemoteIsochPortRef rx_remote;
    IOFireWireLibLocalIsochPortRef rx_local;
    IOFireWireLibNuDCLPoolRef rx_pool;
    CFMutableSetRef rx_bag;
    uint8_t *rx_ring;
    unsigned rx_next;
    int rx_have_dbc;
    uint32_t rx_last_dbc;

    /* send (host -> device) */
    IOFireWireLibIsochChannelRef tx_ch;
    IOFireWireLibRemoteIsochPortRef tx_remote;
    IOFireWireLibLocalIsochPortRef tx_local;
    IOFireWireLibNuDCLPoolRef tx_pool;
    CFMutableSetRef tx_bag;
    uint8_t *tx_buf;
    unsigned tx_next, tx_base, tx_latency;
    uint64_t tx_done, tx_fill;   /* absolute counts: slots completed, slots whose audio is decided */
    unsigned tx_lap[TX_SLOTS];
    uint16_t tx_last_cycle[TX_SLOTS];
    volatile uint32_t tx_ts[TX_SLOTS];

    /* statistics (written by the IO thread) */
    _Atomic uint64_t st_cap_frames, st_play_frames, st_cap_ns;
    _Atomic unsigned st_underruns, st_overruns, st_dbc_errors, st_late;
    _Atomic int st_locked;
    double max_gap_ms, last_service_ms;
    _Atomic uint64_t st_max_gap_us;
};

/* The device forgets its stored stream configs once a host has owned it, so a
 * valid snapshot is kept for re-opens within this process. */
static uint8_t g_snap_cache[3][STREAM_SNAP_SIZE];
static int g_snap_cache_ok;

static mbox3 *g_active;              /* port handlers carry no refcon */
static mbox3_log_fn g_log;

void mbox3_set_log(mbox3_log_fn fn) { g_log = fn; }

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(buf);
}
#define LOG(...) logf_(__VA_ARGS__)

const char *mbox3_strerror(int err)
{
    switch (err) {
    case MBOX3_OK: return "ok";
    case MBOX3_ERR_NODEV: return "no FireWire device";
    case MBOX3_ERR_IO: return "device I/O failed";
    case MBOX3_ERR_BUSY: return "wrong state";
    case MBOX3_ERR_OWNED: return "device is owned by another node";
    case MBOX3_ERR_LOCK: return "clock did not lock";
    case MBOX3_ERR_ARG: return "invalid argument";
    case MBOX3_ERR_NOSNAP: return "no stream config snapshot";
    case MBOX3_ERR_ISOCH: return "isochronous setup failed";
    }
    return "unknown error";
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Nanoseconds on the mach_absolute_time base (does not advance in sleep). */
static uint64_t now_ns(void)
{
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)dice_be32(p) << 32) | dice_be32(p + 4);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* ------------------------------------------------------------ register I/O */

static int rd(mbox3 *m, uint32_t off, void *buf, uint32_t len)
{
    pthread_mutex_lock(&m->reg);
    IOReturn r = dice_read(off, buf, len);
    pthread_mutex_unlock(&m->reg);
    return r == kIOReturnSuccess ? 0 : -1;
}

static int rd_q(mbox3 *m, uint32_t off, uint32_t *v)
{
    uint8_t b[4];
    if (rd(m, off, b, 4) != 0) return -1;
    *v = dice_be32(b);
    return 0;
}

static int wr_q(mbox3 *m, uint32_t off, uint32_t v)
{
    pthread_mutex_lock(&m->reg);
    IOReturn r = dice_write_q(off, v);
    pthread_mutex_unlock(&m->reg);
    return r == kIOReturnSuccess ? 0 : -1;
}

/* Run a device command (opcode in the command section) and wait for it. */
static int run_cmd(mbox3 *m, uint32_t op)
{
    uint32_t off, size, ret;
    uint8_t b[4];
    if (dice_ext_section(1, &off, &size) != 0) return -1;
    if (wr_q(m, off, EXECUTE_FLAG | rate_flag() | op) != 0) return -1;
    for (int i = 0; i < 40; i++) {
        usleep(50000);
        if (rd(m, off, b, 4) != 0) continue;
        if (!(dice_be32(b) & EXECUTE_FLAG)) {
            if (rd_q(m, off + 4, &ret) != 0) return -1;
            return ret == 0 ? 0 : -1;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- ownership */

static int read_owner(mbox3 *m, uint32_t *goff, uint64_t *cur)
{
    uint8_t o[8];
    uint32_t t;
    if (rd_q(m, 0, &t) != 0) return -1;
    *goff = t * 4;
    if (rd(m, *goff, o, 8) != 0) return -1;
    *cur = be64(o);
    return 0;
}

static int claim_owner(mbox3 *m)
{
    uint32_t goff;
    uint64_t cur, old = 0;
    if (read_owner(m, &goff, &cur) != 0) return MBOX3_ERR_IO;
    if (cur != OWNER_NONE) {
        if ((cur >> 48) == m->node) return MBOX3_OK;
        LOG("owned by another node (0x%016llx)", (unsigned long long)cur);
        return MBOX3_ERR_OWNED;
    }
    uint64_t want = ((uint64_t)m->node << 48) | NOTIFY_PLACEHOLDER;
    IOReturn r = dice_lock64(goff, cur, want, &old, 1);
    if (r != kIOReturnSuccess || old != cur) {
        LOG("claim failed (0x%08x, found 0x%016llx)", r, (unsigned long long)old);
        return MBOX3_ERR_IO;
    }
    return MBOX3_OK;
}

static void release_owner(mbox3 *m)
{
    uint32_t goff;
    uint64_t cur, old = 0;
    if (read_owner(m, &goff, &cur) != 0) return;
    if ((cur >> 48) != m->node) return;
    dice_lock64(goff, cur, OWNER_NONE, &old, 1);
}

/* ------------------------------------------------------------ audio rings */

static size_t ring_write(atomic_uint_fast64_t *wp, atomic_uint_fast64_t *rp, int32_t *buf,
                         unsigned size, const int32_t *src, size_t n)
{
    uint64_t w = atomic_load_explicit(wp, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(rp, memory_order_acquire);
    size_t space = size - (size_t)(w - r);
    if (n > space) n = space;
    unsigned mask = size - 1;
    for (size_t i = 0; i < n; i++)
        memcpy(buf + (size_t)((w + i) & mask) * MBOX3_CHANNELS, src + i * MBOX3_CHANNELS,
               MBOX3_CHANNELS * sizeof(int32_t));
    atomic_store_explicit(wp, w + n, memory_order_release);
    return n;
}

static size_t ring_read(atomic_uint_fast64_t *wp, atomic_uint_fast64_t *rp, const int32_t *buf,
                        unsigned size, int32_t *dst, size_t n)
{
    uint64_t r = atomic_load_explicit(rp, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(wp, memory_order_acquire);
    size_t avail = (size_t)(w - r);
    if (n > avail) n = avail;
    unsigned mask = size - 1;
    for (size_t i = 0; i < n; i++)
        memcpy(dst + i * MBOX3_CHANNELS, buf + (size_t)((r + i) & mask) * MBOX3_CHANNELS,
               MBOX3_CHANNELS * sizeof(int32_t));
    atomic_store_explicit(rp, r + n, memory_order_release);
    return n;
}

size_t mbox3_write_frames(mbox3 *m, const int32_t *frames, size_t n)
{
    if (!m || !m->play_buf) return 0;
    return ring_write(&m->play_w, &m->play_r, m->play_buf, m->ring_frames, frames, n);
}

size_t mbox3_read_frames(mbox3 *m, int32_t *frames, size_t n)
{
    if (!m || !m->cap_buf) return 0;
    return ring_read(&m->cap_w, &m->cap_r, m->cap_buf, m->ring_frames, frames, n);
}

/* --------------------------------------------------- isochronous port glue */

static IOReturn tx_on_allocate(IOFireWireLibIsochPortRef p, IOFWSpeed speed, UInt32 chan)
{
    (void)p; (void)speed;
    LOG("device TX stream on iso channel %u", chan);
    return dice_write_q(g_active->tx_iso_off, chan);     /* device -> host */
}

static IOReturn rx_on_allocate(IOFireWireLibIsochPortRef p, IOFWSpeed speed, UInt32 chan)
{
    (void)p; (void)speed;
    LOG("device RX stream on iso channel %u", chan);
    return dice_write_q(g_active->rx_iso_off, chan);     /* host -> device */
}

static IOReturn tx_on_release(IOFireWireLibIsochPortRef p)
{
    (void)p;
    return dice_write_q(g_active->tx_iso_off, 0xffffffffu);
}

static IOReturn rx_on_release(IOFireWireLibIsochPortRef p)
{
    (void)p;
    return dice_write_q(g_active->rx_iso_off, 0xffffffffu);
}

static IOReturn on_start(IOFireWireLibIsochPortRef p)
{
    (void)p;
    return dice_write_q(g_active->enable_off, 1);
}

static IOReturn on_stop(IOFireWireLibIsochPortRef p)
{
    (void)p;
    return dice_write_q(g_active->enable_off, 0);
}

static void on_slot_cb(void *refcon, NuDCLRef dcl)
{
    (void)refcon; (void)dcl;
}

static int read_offsets(mbox3 *m)
{
    uint8_t t[8];
    if (rd(m, 0, t, 8) != 0) return -1;
    m->global = dice_be32(t) * 4;
    m->enable_off = m->global + 0x50;
    m->status_off = m->global + 0x54;
    m->clock_off = m->global + 0x4c;
    if (rd(m, 8, t, 8) != 0) return -1;
    m->tx_iso_off = dice_be32(t) * 4 + 8;
    if (rd(m, 0x10, t, 8) != 0) return -1;
    m->rx_iso_off = dice_be32(t) * 4 + 8;
    return 0;
}

/* ---------------------------------------------------------------- receive */

/* Buffers handed to the FireWire DMA programs: page aligned and a whole number of pages long, so that no other
 * heap object shares the pages the kernel wires for them (a partly used last page made the result depend on the heap layout). */
static void *iso_alloc(size_t bytes)
{
    size_t pg = (size_t)getpagesize();
    size_t n = (bytes + pg - 1) / pg * pg;
    void *p = NULL;
    if (posix_memalign(&p, pg, n) != 0) return NULL;
    memset(p, 0, n);
    return p;
}

#define ISO_FAIL(what) do { LOG("isochronous setup failed: %s", what); return -1; } while (0)

static int rx_begin(mbox3 *m)
{
    IOFireWireLibDeviceRef dev = dice_device();

    m->rx_ch = (*dev)->CreateIsochChannel(dev, true, rx_chan_payload(),
        kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    m->rx_remote = (*dev)->CreateRemoteIsochPort(dev, true,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!m->rx_ch || !m->rx_remote) ISO_FAIL("rx: channel or remote port");
    (*m->rx_remote)->SetAllocatePortHandler(m->rx_remote, tx_on_allocate);
    (*m->rx_remote)->SetReleasePortHandler(m->rx_remote, tx_on_release);
    (*m->rx_remote)->SetStartHandler(m->rx_remote, on_start);
    (*m->rx_remote)->SetStopHandler(m->rx_remote, on_stop);

    m->rx_ring = iso_alloc((size_t)RX_PACKETS * RX_STRIDE);
    if (!m->rx_ring) ISO_FAIL("rx: buffer");
    for (size_t i = 0; i < (size_t)RX_PACKETS * RX_STRIDE; i += 4)
        memcpy(m->rx_ring + i, &(uint32_t){FILL_PATTERN}, 4);
    m->rx_next = 0;
    m->rx_have_dbc = 0;

    m->rx_pool = (*dev)->CreateNuDCLPool(dev, RX_PACKETS,
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!m->rx_pool) ISO_FAIL("rx: DCL pool");
    m->rx_bag = CFSetCreateMutable(kCFAllocatorDefault, 0, NULL);
    NuDCLRef first = NULL, last = NULL;
    for (unsigned i = 0; i < RX_PACKETS; i++) {
        IOVirtualRange range = { (IOVirtualAddress)(m->rx_ring + (size_t)i * RX_STRIDE), RX_STRIDE };
        NuDCLRef d = (NuDCLRef)(*m->rx_pool)->AllocateReceivePacket(m->rx_pool, m->rx_bag, 4, 1, &range);
        if (!d) ISO_FAIL("rx: receive packet DCL");
        if (!first) first = d;
        last = d;
    }
    (*m->rx_pool)->SetDCLBranch(last, first);

    m->rx_local = (*dev)->CreateLocalIsochPort(dev, false,
        (*m->rx_pool)->GetProgram(m->rx_pool), 0, 0, 0, NULL, 0, NULL, 0,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!m->rx_local) ISO_FAIL("rx: local port");

    if ((*m->rx_ch)->SetTalker(m->rx_ch, (IOFireWireLibIsochPortRef)m->rx_remote) != kIOReturnSuccess ||
        (*m->rx_ch)->AddListener(m->rx_ch, (IOFireWireLibIsochPortRef)m->rx_local) != kIOReturnSuccess)
        ISO_FAIL("rx: SetTalker/AddListener");
    {
        IOReturn ar = (*m->rx_ch)->AllocateChannel(m->rx_ch);
        if (ar != kIOReturnSuccess) {
            LOG("rx: AllocateChannel returned 0x%08x (a bus reset, i.e. unplugging the FireWire cable for a few seconds, frees allocations left by an earlier run)", (unsigned)ar);
            (*m->rx_ch)->ReleaseChannel(m->rx_ch);   /* give back whatever was taken */
            ISO_FAIL("rx: AllocateChannel");
        }
    }
    if ((*m->rx_ch)->Start(m->rx_ch) != kIOReturnSuccess) {
        (*m->rx_ch)->ReleaseChannel(m->rx_ch);
        ISO_FAIL("rx: Start");
    }
    return 0;
}

static void rx_end(mbox3 *m)
{
    if (m->rx_ch) {
        (*m->rx_ch)->Stop(m->rx_ch);
        (*m->rx_ch)->ReleaseChannel(m->rx_ch);
    }
    if (m->rx_local) (*m->rx_local)->Release(m->rx_local);
    if (m->rx_remote) (*m->rx_remote)->Release(m->rx_remote);
    if (m->rx_pool) (*m->rx_pool)->Release(m->rx_pool);
    if (m->rx_ch) (*m->rx_ch)->Release(m->rx_ch);
    if (m->rx_bag) CFRelease(m->rx_bag);
    free(m->rx_ring);
    m->rx_ch = NULL; m->rx_local = NULL; m->rx_remote = NULL; m->rx_pool = NULL;
    m->rx_bag = NULL; m->rx_ring = NULL;
}

/* Consume the packets the hardware has written. A slot counts as complete once
 * the next slot's header is present too (the DMA fills header and payload in
 * order, so the previous packet is then fully written). */
static void rx_service(mbox3 *m, bool deliver)
{
    unsigned delivered = 0;
    for (;;) {
        uint8_t *b = m->rx_ring + (size_t)m->rx_next * RX_STRIDE;
        uint8_t *nb = m->rx_ring + (size_t)((m->rx_next + 1) % RX_PACKETS) * RX_STRIDE;
        uint32_t first = *(volatile uint32_t *)b;
        if (first == FILL_PATTERN) break;
        if (*(volatile uint32_t *)nb == FILL_PATTERN) break;

        const uint8_t *cip = b + 4;
        uint32_t q0 = dice_be32(cip), q1 = dice_be32(cip + 4);
        unsigned dbs = (q0 >> 16) & 0xff, syt = q1 & 0xffff;
        const unsigned nb8 = bpp();
        if (syt != 0xffff && dbs == 9) {
            uint32_t dbc = q0 & 0xff;
            if (m->rx_have_dbc && dbc != ((m->rx_last_dbc + nb8) & 0xff)) {
                atomic_fetch_add(&m->st_dbc_errors, 1);
                /* the data block counter counts frames: packets were lost (the receive ring was overrun by a stall).
                 * Put silence in their place so the capture index keeps counting device time. */
                unsigned gap = (dbc - m->rx_last_dbc - nb8) & 0xff;
                if (deliver && gap > 0 && gap < 128) {
                    int32_t zeros[16 * MBOX3_CHANNELS] = {0};
                    for (unsigned done = 0; done < gap; ) {
                        unsigned n = gap - done > nb8 ? nb8 : gap - done;
                        size_t w = ring_write(&m->cap_w, &m->cap_r, m->cap_buf, m->ring_frames, zeros, n);
                        atomic_fetch_add(&m->st_cap_frames, w);
                        if (w < n) break;
                        done += n;
                    }
                }
            }
            m->rx_last_dbc = dbc;
            m->rx_have_dbc = 1;
            if (deliver) {
                int32_t fr[16 * MBOX3_CHANNELS];
                for (unsigned blk = 0; blk < nb8; blk++)
                    for (unsigned ch = 0; ch < MBOX3_CHANNELS; ch++) {
                        uint32_t q = dice_be32(cip + 8 + ((size_t)blk * 9 + ch) * 4);
                        fr[blk * MBOX3_CHANNELS + ch] = (int32_t)(q << 8) >> 8;
                    }
                size_t w = ring_write(&m->cap_w, &m->cap_r, m->cap_buf, m->ring_frames, fr, nb8);
                if (w < nb8) atomic_fetch_add(&m->st_overruns, (unsigned)(nb8 - w));
                atomic_fetch_add(&m->st_cap_frames, w);
                delivered++;
            }
        }
        uint32_t fill = FILL_PATTERN;
        memcpy(b, &fill, 4);
        m->rx_next = (m->rx_next + 1) % RX_PACKETS;
    }
    if (delivered) atomic_store(&m->st_cap_ns, now_ns());
}

/* ------------------------------------------------------------------- send */

/* Slot s carries data iff the Bresenham counter crosses one packet's worth of samples there
 * (8000 slots per second, bpp() samples per data packet). */
static int slot_is_data(unsigned s)
{
    unsigned d = 8000u * bpp();
    return ((uint64_t)(s + 1) * g_rate / d) > ((uint64_t)s * g_rate / d);
}
static uint64_t data_before(unsigned s) { return (uint64_t)s * g_rate / (8000u * bpp()); }

/* Write the header of slot s for ring lap `lap`; `cycle` is the bus cycle
 * (0..7999) the slot will be transmitted in. The payload is silence until
 * tx_fill_payload() puts audio into it shortly before transmission. */
static void tx_fill_slot(mbox3 *m, unsigned s, unsigned lap, unsigned cycle)
{
    uint8_t *b = m->tx_buf + (size_t)s * TX_STRIDE;
    uint64_t n = data_before(s) + (uint64_t)TX_DATA_PER_LAP * lap; /* data packets before this slot */
    uint32_t dbc = (uint32_t)((n * bpp()) & 0xff);
    uint32_t q0 = ((uint32_t)(m->node & 0x3f) << 24) | (9u << 16) | dbc;
    uint32_t syt = 0xffff;
    int data = slot_is_data(s);
    if (data) {
        /* ideal presentation time of this packet minus the slot's own cycle position, in 24.576 MHz ticks */
        int64_t ideal = (int64_t)(data_before(s) * (uint64_t)bpp() * 24576000ull / g_rate);
        int64_t off = ideal - (int64_t)s * 3072;
        int64_t tick = (int64_t)((cycle + m->tx_latency) % 16) * 3072 + off;
        tick %= 49152;
        if (tick < 0) tick += 49152;
        syt = (uint32_t)((((tick / 3072) & 0xf) << 12) | (tick % 3072));
    }
    put32(b, q0);
    put32(b + 4, (g_rate > 48000 ? 0x90000000u | (rate_index() << 16) : 0x90010000u) | syt);   /* FMT 0x10 (AM824), FDF: 1 below 88.2 kHz as before, else the rate code */
    if (!data) return;
    for (unsigned blk = 0; blk < bpp(); blk++) {
        uint8_t *q = b + 8 + blk * 36;
        for (unsigned ch = 0; ch < MBOX3_CHANNELS; ch++) put32(q + ch * 4, 0x40000000u);
        put32(q + 32, 0x80000000u);                       /* no MIDI */
    }
}

/* Put the next bpp() playback frames into data slot s (silence if the ring is empty). */
static void tx_fill_payload(mbox3 *m, unsigned s)
{
    if (!slot_is_data(s)) return;
    uint8_t *b = m->tx_buf + (size_t)s * TX_STRIDE;
    uint64_t avail = atomic_load_explicit(&m->play_w, memory_order_acquire) -
                     atomic_load_explicit(&m->play_r, memory_order_relaxed);
    const unsigned nb = bpp();
    if (avail < nb) {
        atomic_fetch_add(&m->st_underruns, 1);   /* the slot keeps its silence, queued frames stay */
        return;
    }
    int32_t fr[16 * MBOX3_CHANNELS];
    if (ring_read(&m->play_w, &m->play_r, m->play_buf, m->ring_frames, fr, nb) != nb) return;
    atomic_fetch_add(&m->st_play_frames, nb);
    for (unsigned blk = 0; blk < nb; blk++) {
        uint8_t *q = b + 8 + blk * 36;
        for (unsigned ch = 0; ch < MBOX3_CHANNELS; ch++)
            put32(q + ch * 4, 0x40000000u | ((uint32_t)fr[blk * MBOX3_CHANNELS + ch] & 0xffffff));
    }
}

static int tx_begin(mbox3 *m)
{
    IOFireWireLibDeviceRef dev = dice_device();

    m->tx_ch = (*dev)->CreateIsochChannel(dev, true, tx_chan_payload(),
        kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    m->tx_remote = (*dev)->CreateRemoteIsochPort(dev, false,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!m->tx_ch || !m->tx_remote) ISO_FAIL("tx: channel or remote port");
    (*m->tx_remote)->SetAllocatePortHandler(m->tx_remote, rx_on_allocate);
    (*m->tx_remote)->SetReleasePortHandler(m->tx_remote, rx_on_release);
    (*m->tx_remote)->SetStartHandler(m->tx_remote, on_start);
    (*m->tx_remote)->SetStopHandler(m->tx_remote, on_stop);

    m->tx_buf = iso_alloc((size_t)TX_SLOTS * TX_STRIDE);
    if (!m->tx_buf) ISO_FAIL("tx: buffer");
    memset(m->tx_buf, 0, (size_t)TX_SLOTS * TX_STRIDE);
    for (unsigned s = 0; s < TX_SLOTS; s++) {
        m->tx_ts[s] = 0xffffffffu;
        m->tx_lap[s] = 0;
        m->tx_last_cycle[s] = 0xffff;
    }
    m->tx_next = 0;
    m->tx_done = 0;
    m->tx_fill = m->cfg.tx_ahead_slots;

    m->tx_pool = (*dev)->CreateNuDCLPool(dev, TX_SLOTS,
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!m->tx_pool) ISO_FAIL("tx: DCL pool");
    (*m->tx_pool)->SetCurrentTagAndSync(m->tx_pool, 1, 0);

    m->tx_bag = CFSetCreateMutable(kCFAllocatorDefault, 0, NULL);
    NuDCLRef dcls[TX_SLOTS];
    for (unsigned s = 0; s < TX_SLOTS; s++) {
        IOVirtualRange range = { (IOVirtualAddress)(m->tx_buf + (size_t)s * TX_STRIDE),
                                 slot_is_data(s) ? tx_data_bytes() : TX_NODATA_BYTES };
        NuDCLRef d = (NuDCLRef)(*m->tx_pool)->AllocateSendPacket(m->tx_pool, m->tx_bag, 1, &range);
        if (!d) ISO_FAIL("tx: send packet DCL");
        (*m->tx_pool)->SetDCLTimeStampPtr(d, (UInt32 *)&m->tx_ts[s]);
        (*m->tx_pool)->SetDCLFlags(d, kNuDCLDynamic | kNuDCLUpdateBeforeCallback);
        dcls[s] = d;
    }
    (*m->tx_pool)->SetDCLBranch(dcls[TX_SLOTS - 1], dcls[0]);
    for (unsigned s = 15; s < TX_SLOTS; s += 16) {
        (*m->tx_pool)->SetDCLCallback(dcls[s], on_slot_cb);
        for (unsigned j = s - 15; j <= s; j++) (*m->tx_pool)->AppendDCLUpdateList(dcls[s], dcls[j]);
    }

    uint32_t ct = 0;
    (*dev)->GetCycleTime(dev, &ct);
    m->tx_base = (((ct >> 12) & 0x1fff) + 1600) % 8000;
    for (unsigned s = 0; s < TX_SLOTS; s++) tx_fill_slot(m, s, 0, (m->tx_base + s) % 8000);

    m->tx_local = (*dev)->CreateLocalIsochPort(dev, true,
        (*m->tx_pool)->GetProgram(m->tx_pool), 0, 0, 0, NULL, 0, NULL, 0,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!m->tx_local) ISO_FAIL("tx: local port");

    if ((*m->tx_ch)->SetTalker(m->tx_ch, (IOFireWireLibIsochPortRef)m->tx_local) != kIOReturnSuccess ||
        (*m->tx_ch)->AddListener(m->tx_ch, (IOFireWireLibIsochPortRef)m->tx_remote) != kIOReturnSuccess)
        ISO_FAIL("tx: SetTalker/AddListener");
    {
        IOReturn ar = (*m->tx_ch)->AllocateChannel(m->tx_ch);
        if (ar != kIOReturnSuccess) {
            LOG("tx: AllocateChannel returned 0x%08x (a bus reset, i.e. unplugging the FireWire cable for a few seconds, frees allocations left by an earlier run)", (unsigned)ar);
            (*m->tx_ch)->ReleaseChannel(m->tx_ch);   /* give back whatever was taken */
            ISO_FAIL("tx: AllocateChannel");
        }
    }
    if ((*m->tx_ch)->Start(m->tx_ch) != kIOReturnSuccess) {
        (*m->tx_ch)->ReleaseChannel(m->tx_ch);
        ISO_FAIL("tx: Start");
    }
    /* The kernel zeroes the timestamp words when the program is created;
     * clear them again so only real completions are seen. */
    for (unsigned s = 0; s < TX_SLOTS; s++) m->tx_ts[s] = 0xffffffffu;
    return 0;
}

static void tx_end(mbox3 *m)
{
    if (m->tx_ch) {
        (*m->tx_ch)->Stop(m->tx_ch);
        (*m->tx_ch)->ReleaseChannel(m->tx_ch);
    }
    if (m->tx_local) (*m->tx_local)->Release(m->tx_local);
    if (m->tx_remote) (*m->tx_remote)->Release(m->tx_remote);
    if (m->tx_pool) (*m->tx_pool)->Release(m->tx_pool);
    if (m->tx_ch) (*m->tx_ch)->Release(m->tx_ch);
    if (m->tx_bag) CFRelease(m->tx_bag);
    free(m->tx_buf);
    m->tx_ch = NULL; m->tx_local = NULL; m->tx_remote = NULL; m->tx_pool = NULL;
    m->tx_bag = NULL; m->tx_buf = NULL;
}

/* Refill, in transmission order, every slot the hardware has sent, then put
 * audio into the slots that are about to go out. Completions are reported in
 * groups of 16 slots, so the audio is added cfg.tx_ahead_slots (224 by default) slots ahead of the
 * last reported one. A slot that went out before it could be filled counts as
 * an underrun and was sent as silence. */
static void tx_service(mbox3 *m, bool consume)
{
    for (;;) {
        unsigned s = m->tx_next;
        uint32_t v = m->tx_ts[s];
        if (v == 0xffffffffu) break;
        m->tx_ts[s] = 0xffffffffu;
        unsigned cyc = (v >> 12) & 0x1fff;
        unsigned missed = 0;
        if (m->tx_last_cycle[s] != 0xffff) {
            unsigned d = (cyc + 8000 - m->tx_last_cycle[s]) % 8000;
            if (d != TX_SLOTS) {
                atomic_fetch_add(&m->st_late, 1);
                /* after a stall of more than a lap the slot went out again unserviced (stale audio):
                 * those laps are device time that passed, count them or the playback timeline falls behind */
                unsigned laps = (d + TX_SLOTS / 2) / TX_SLOTS;
                if (laps > 1 && laps <= 11) missed = laps - 1;
            }
        }
        m->tx_done += missed;
        m->tx_last_cycle[s] = (uint16_t)cyc;
        m->tx_lap[s]++;
        tx_fill_slot(m, s, m->tx_lap[s], (cyc + TX_SLOTS) % 8000);
        m->tx_next = (s + 1) % TX_SLOTS;
        m->tx_done++;
    }
    if (!consume) {
        if (m->tx_fill < m->tx_done + m->cfg.tx_ahead_slots) m->tx_fill = m->tx_done + m->cfg.tx_ahead_slots;
        return;
    }
    while (m->tx_fill < m->tx_done + m->cfg.tx_ahead_slots) {
        unsigned s = (unsigned)(m->tx_fill % TX_SLOTS);
        if (m->tx_fill < m->tx_done) {
            if (slot_is_data(s)) atomic_fetch_add(&m->st_underruns, 1);   /* already gone */
        } else {
            tx_fill_payload(m, s);
        }
        m->tx_fill++;
    }
}

/* ------------------------------------------------- device preparation/start */

static int prepare_device(mbox3 *m)
{
    /* 1. internal clock at the session's rate */
    if (wr_q(m, m->clock_off, clock_sel(CLOCK_SRC_INTERNAL)) != 0) return MBOX3_ERR_IO;
    usleep(500000);

    /* 2. router */
    const uint32_t *routes = m->cfg.routes ? m->cfg_routes : DEFAULT_ROUTES;
    unsigned n = m->cfg.routes ? m->cfg.n_routes : 40;
    uint32_t mid_routes[64];
    int own_mid = 0;
    if (g_rate > 48000 && !m->cfg.routes) {
        /* the router the device itself holds for 88.2 / 96 kHz (current_config + 0x2000: count, then the entries).
         * Read only when this rate is used, so 44.1 / 48 kHz sessions do not touch it at all. */
        uint32_t cc_off, cc_size;
        uint8_t rt[4 + 64 * 4];
        if (dice_ext_section(6, &cc_off, &cc_size) == 0 && cc_size >= 0x2000 + sizeof rt &&
            rd(m, cc_off + 0x2000, rt, sizeof rt) == 0) {
            unsigned cnt = dice_be32(rt);
            if (cnt >= 1 && cnt <= 64) {
                for (unsigned i = 0; i < cnt; i++) mid_routes[i] = dice_be32(rt + 4 + i * 4);
                const char *ov = getenv("M3_MID_ROUTER");     /* diagnostic: M3_MID_ROUTER=default uses the 44.1 / 48 kHz table */
                if (ov && !strcmp(ov, "default")) LOG("M3_MID_ROUTER=default: using the default router instead");
                else { routes = mid_routes; n = cnt; own_mid = 1; LOG("using the device's own %u kHz router (%u entries)", g_rate / 1000, n); }
            }
        }
        if (!own_mid) LOG("the device's own %u kHz router is not available, using the default router", g_rate / 1000);
    }
    uint32_t r_off, r_size;
    if (dice_ext_section(4, &r_off, &r_size) != 0) return MBOX3_ERR_IO;
    for (int attempt = 0; ; attempt++) {
        if (4 + n * 4 > r_size) return MBOX3_ERR_IO;
        for (unsigned i = 0; i < n; i++)
            if (wr_q(m, r_off + 4 + i * 4, routes[i]) != 0) return MBOX3_ERR_IO;
        if (wr_q(m, r_off, n) != 0) return MBOX3_ERR_IO;
        if (run_cmd(m, OP_LOAD_ROUTER) == 0) break;
        LOG("LoadRouter failed");
        if (!own_mid || attempt) return MBOX3_ERR_IO;
        routes = DEFAULT_ROUTES; n = 40; own_mid = 0;      /* fall back to the 44.1 / 48 kHz table */
        LOG("retrying with the default router");
    }

    /* 3. stream format, from the snapshot taken before ownership */
    if (!m->snap_ok) return MBOX3_ERR_NOSNAP;
    uint32_t sf_off, sf_size;
    if (dice_ext_section(5, &sf_off, &sf_size) != 0 || sf_size < STREAM_SNAP_SIZE) return MBOX3_ERR_IO;
    /* the stream config of the rate class in use; 88.2 / 96 kHz fall back to the low-rate one if theirs was not captured */
    const uint8_t *sc = m->snap[0];
    if (g_rate > 48000 && (dice_be32(m->snap[1]) || dice_be32(m->snap[1] + 4))) sc = m->snap[1];
    for (uint32_t i = 0; i < STREAM_SNAP_SIZE; i += 4) {
        uint32_t v = dice_be32(sc + i);
        if (v && wr_q(m, sf_off + i, v) != 0) return MBOX3_ERR_IO;
    }
    if (run_cmd(m, OP_LOAD_STREAM) != 0) { LOG("LoadStreamConfig failed"); return MBOX3_ERR_IO; }
    return MBOX3_OK;
}

static int write_default_mixer(mbox3 *m)
{
    uint32_t base, size;
    if (dice_ext_section(2, &base, &size) != 0 ||
        size < 4 + MBOX3_MIXER_INPUTS * MBOX3_MIXER_OUTPUTS * 4)
        return -1;
    for (unsigned o = 0; o < 8; o++)
        if (wr_q(m, base + 4 + (o * MBOX3_MIXER_INPUTS + o) * 4, MBOX3_MIXER_UNITY) != 0) return -1;
    /* Headphone path (Ins1:0-3 come from mixer outputs 8-11 in the default
     * router): playback 1 on the left, playback 2 on the right, as in
     * standalone mode. */
    for (unsigned o = 8; o < 12; o++) {
        unsigned in = (o & 1);
        if (wr_q(m, base + 4 + (o * MBOX3_MIXER_INPUTS + in) * 4, MBOX3_MIXER_UNITY) != 0) return -1;
    }
    return 0;
}

static void clock_to_internal(mbox3 *m)
{
    if (m->clock_switched) {
        wr_q(m, m->clock_off, clock_sel(CLOCK_SRC_INTERNAL));
        m->clock_switched = 0;
    }
}

static void teardown(mbox3 *m)
{
    clock_to_internal(m);                 /* leave the device on its own crystal */
    tx_end(m);
    rx_end(m);
    free(m->cap_buf); m->cap_buf = NULL;
    free(m->play_buf); m->play_buf = NULL;
    g_active = NULL;
}

static int bring_up(mbox3 *m)
{
    int rc;
    g_active = m;
    atomic_store(&m->cap_w, 0); atomic_store(&m->cap_r, 0);
    atomic_store(&m->play_w, 0); atomic_store(&m->play_r, 0);
    atomic_store(&m->st_cap_frames, 0); atomic_store(&m->st_play_frames, 0);
    atomic_store(&m->st_underruns, 0); atomic_store(&m->st_overruns, 0);
    atomic_store(&m->st_dbc_errors, 0); atomic_store(&m->st_late, 0);
    atomic_store(&m->st_locked, 0); atomic_store(&m->st_max_gap_us, 0);
    m->mixer_done = 0; m->clock_switched = 0;
    m->tx_latency = m->cfg.syt_latency;

    m->cap_buf = calloc((size_t)m->ring_frames * MBOX3_CHANNELS, sizeof(int32_t));
    m->play_buf = calloc((size_t)m->ring_frames * MBOX3_CHANNELS, sizeof(int32_t));
    if (!m->cap_buf || !m->play_buf) { rc = MBOX3_ERR_IO; goto fail; }

    if (read_offsets(m) != 0) { rc = MBOX3_ERR_IO; goto fail; }
    if ((rc = prepare_device(m)) != MBOX3_OK) goto fail;

    uint16_t node = 0;
    if (dice_local_node_id(&node) == 0) m->node = node;
    if (rx_begin(m) != 0 || tx_begin(m) != 0) { rc = MBOX3_ERR_ISOCH; goto fail; }

    m->t_start_ms = now_ms();
    m->t_clock_ms = 0;
    m->t_lock_since_ms = 0;
    m->last_service_ms = now_ms();
    return MBOX3_OK;
fail:
    teardown(m);
    return rc;
}

/* Warm-up: silence while the device settles, then slave its clock to the
 * host stream and wait for lock. Returns 1 when running, <0 on failure, 0 to
 * keep waiting. */
static int warmup_step(mbox3 *m)
{
    uint64_t t = now_ms(), since = t - m->t_start_ms;
    if (!m->mixer_done && since >= WARMUP_MIXER_MS) {
        m->mixer_done = 1;
        if (!(m->cfg.flags & MBOX3_NO_MIXER_DEFAULTS) && write_default_mixer(m) != 0)
            LOG("writing the default mixer failed");
    }
    if (!m->clock_switched && since >= WARMUP_CLOCK_MS) {
        if (wr_q(m, m->clock_off, clock_sel(CLOCK_SRC_ARX1)) != 0) return MBOX3_ERR_IO;
        m->clock_switched = 1;
        m->t_clock_ms = t;
        return 0;
    }
    if (!m->clock_switched) return 0;
    if (t - m->t_clock_ms < LOCK_MIN_WAIT_MS) return 0;
    if (t - m->t_poll_ms < 20) return 0;
    m->t_poll_ms = t;
    uint32_t st;
    if (rd_q(m, m->status_off, &st) != 0) return MBOX3_ERR_IO;
    if ((st & 1) && ((st >> 8) & 0xff) == rate_index()) {
        if (!m->t_lock_since_ms) m->t_lock_since_ms = t;
        if (t - m->t_lock_since_ms >= LOCK_SETTLE_MS) {
            atomic_store(&m->st_locked, 1);
            LOG("clock locked %llu ms after switching to the host stream", (unsigned long long)(t - m->t_clock_ms));
            return 1;
        }
    } else {
        m->t_lock_since_ms = 0;
    }
    if (t - m->t_clock_ms > LOCK_TIMEOUT_MS) {
        /* what the device said at the end: the lock bit, the rate it thinks it runs at (see "wanted"), and
         * whether the receive stream delivers anything at all */
        LOG("clock lock timed out: status 0x%08x (locked %u, rate index %u, wanted %u), received frames %llu",
            st, st & 1, (st >> 8) & 0xff, rate_index(), (unsigned long long)atomic_load(&m->st_cap_frames));
        return MBOX3_ERR_LOCK;
    }
    return 0;
}

static void note_gap(mbox3 *m)
{
    double t = (double)now_ns() / 1e6;
    double gap = t - m->last_service_ms;
    m->last_service_ms = t;
    if (gap < 0) gap = 0;
    uint64_t us = (uint64_t)(gap * 1000.0);
    if (us > atomic_load(&m->st_max_gap_us)) atomic_store(&m->st_max_gap_us, us);
}

/* ------------------------------------------------------------- IO thread */

void mbox3_make_thread_realtime(unsigned period_us)
{
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    thread_time_constraint_policy_data_t p;
    p.period = (uint32_t)((uint64_t)period_us * 1000u * tb.denom / tb.numer);
    p.computation = p.period / 4;
    p.constraint = p.period / 2;
    p.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
    (void)period_us;
#endif
}

static void *io_thread(void *arg)
{
    mbox3 *m = arg;
    mbox3_make_thread_realtime(2000);   /* a stall here is a dropout in the audio */
    IOFireWireLibDeviceRef dev = dice_device();
    (*dev)->AddCallbackDispatcherToRunLoop(dev, CFRunLoopGetCurrent());
    (*dev)->AddIsochCallbackDispatcherToRunLoop(dev, CFRunLoopGetCurrent());

    for (;;) {
        pthread_mutex_lock(&m->lock);
        int cmd = m->cmd;
        pthread_mutex_unlock(&m->lock);

        if (cmd == CMD_QUIT) break;
        if (cmd == CMD_START) {
            int rc = bring_up(m);
            pthread_mutex_lock(&m->lock);
            m->cmd = CMD_NONE;
            if (rc != MBOX3_OK) {
                m->state = ST_IDLE;
                m->result = rc;
                pthread_cond_broadcast(&m->cond);
            } else {
                m->state = ST_WARMUP;
            }
            pthread_mutex_unlock(&m->lock);
        } else if (cmd == CMD_STOP) {
            pthread_mutex_lock(&m->lock);
            int st = m->state;
            pthread_mutex_unlock(&m->lock);
            if (st == ST_WARMUP || st == ST_RUN) teardown(m);
            pthread_mutex_lock(&m->lock);
            m->state = ST_IDLE;
            m->cmd = CMD_NONE;
            m->result = MBOX3_OK;
            pthread_cond_broadcast(&m->cond);
            pthread_mutex_unlock(&m->lock);
        }

        pthread_mutex_lock(&m->lock);
        int state = m->state;
        pthread_mutex_unlock(&m->lock);

        if (state == ST_WARMUP || state == ST_RUN) {
            uint64_t ta = now_ns();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
            uint64_t tb = now_ns();
            note_gap(m);
            bool run = state == ST_RUN;
            rx_service(m, run);
            uint64_t tc = now_ns();
            tx_service(m, run);
            uint64_t td = now_ns();
            if (state == ST_WARMUP) {
                int w = warmup_step(m);
                if (w != 0) {
                    pthread_mutex_lock(&m->lock);
                    if (w > 0) {
                        if (m->cfg.prefill_frames) {
                            /* silence ahead of the audio: the app is still blocked in
                             * mbox3_start, so the IO thread may act as producer here */
                            static const int32_t zeros[256 * MBOX3_CHANNELS];
                            unsigned left = m->cfg.prefill_frames;
                            while (left) {
                                size_t n = ring_write(&m->play_w, &m->play_r, m->play_buf, m->ring_frames,
                                                      zeros, left > 256 ? 256 : left);
                                if (!n) break;
                                left -= (unsigned)n;
                            }
                        }
                        m->state = ST_RUN;
                        m->result = MBOX3_OK;
                        atomic_store(&m->cap_r, atomic_load(&m->cap_w));
                        /* statistics count from the moment audio flows */
                        atomic_store(&m->st_dbc_errors, 0);
                        atomic_store(&m->st_late, 0);
                        atomic_store(&m->st_underruns, 0);
                        atomic_store(&m->st_overruns, 0);
                        atomic_store(&m->st_max_gap_us, 0);
                        atomic_store(&m->st_cap_frames, 0);
                        atomic_store(&m->st_play_frames, 0);
                    } else {
                        m->state = ST_STOPPING;
                        m->result = w;
                    }
                    pthread_cond_broadcast(&m->cond);
                    pthread_mutex_unlock(&m->lock);
                    if (w < 0) {
                        teardown(m);
                        pthread_mutex_lock(&m->lock);
                        m->state = ST_IDLE;
                        pthread_mutex_unlock(&m->lock);
                    }
                }
            } else if (now_ms() - m->t_poll_ms >= 1000) {
                m->t_poll_ms = now_ms();
                /* never wait for the register lock here: another thread may be in the middle of a long read */
                if (pthread_mutex_trylock(&m->reg) == 0) {
                    uint8_t b[4];
                    IOReturn r = dice_read(m->status_off, b, 4);
                    pthread_mutex_unlock(&m->reg);
                    if (r == kIOReturnSuccess) atomic_store(&m->st_locked, (int)(dice_be32(b) & 1));
                }
            }
            {   /* a stall of the IO thread is a dropout: say where the time went */
                uint64_t te = now_ns();
                if (state == ST_RUN && (tb - ta > 60000000ull || tc - tb > 60000000ull || td - tc > 60000000ull || te - td > 60000000ull))
                    LOG("IO thread stall: run loop %.0f ms, receive %.0f ms, send %.0f ms, status/other %.0f ms",
                        (double)(tb - ta) / 1e6, (double)(tc - tb) / 1e6, (double)(td - tc) / 1e6, (double)(te - td) / 1e6);
            }
        } else {
            if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false) == kCFRunLoopRunFinished)
                usleep(20000);
        }
    }
    if (m->state != ST_IDLE) teardown(m);
    return NULL;
}

/* -------------------------------------------------------------------- API */

int mbox3_open(mbox3 **out)
{
    if (!out || g_active) return MBOX3_ERR_ARG;
    mbox3 *m = calloc(1, sizeof(*m));
    if (!m) return MBOX3_ERR_IO;
    pthread_mutex_init(&m->lock, NULL);
    pthread_mutex_init(&m->reg, NULL);
    pthread_cond_init(&m->cond, NULL);

    if (dice_open() != 0) { free(m); return MBOX3_ERR_NODEV; }
    if (dice_local_node_id(&m->node) != 0) { dice_close(); free(m); return MBOX3_ERR_NODEV; }
    if (read_offsets(m) != 0) { dice_close(); free(m); return MBOX3_ERR_IO; }

    /* The stored stream configs read as empty once the host owns the device,
     * so take them first. */
    uint32_t cc_off, cc_size;
    if (dice_ext_section(6, &cc_off, &cc_size) == 0) {
        int ok = 1, from_cache = 0;
        for (int i = 0; i < 3; i++)
            if (rd(m, cc_off + 0x1000 + i * 0x2000, m->snap[i], STREAM_SNAP_SIZE) != 0) ok = 0;
        /* valid = non-zero stream counts for the 44.1 kHz mode */
        if (ok && dice_be32(m->snap[0]) == 0 && dice_be32(m->snap[0] + 4) == 0) ok = 0;
        if (ok) {
            memcpy(g_snap_cache, m->snap, sizeof(g_snap_cache));
            g_snap_cache_ok = 1;
        } else if (g_snap_cache_ok) {
            memcpy(m->snap, g_snap_cache, sizeof(m->snap));
            ok = 1; from_cache = 1;
            LOG("using the stream config snapshot from an earlier open");
        }
        m->snap_ok = ok;
        LOG("stream config snapshot: %s; stream counts (tx rx) 44.1k-class %u %u, 88.2k-class %u %u",
            ok ? (from_cache ? "cached" : "read from the device") : "none",
            dice_be32(m->snap[0]), dice_be32(m->snap[0] + 4), dice_be32(m->snap[1]), dice_be32(m->snap[1] + 4));
    }
    int rc = claim_owner(m);
    if (rc != MBOX3_OK) { dice_close(); free(m); return rc; }
    m->owned = 1;

    if (pthread_create(&m->thread, NULL, io_thread, m) != 0) {
        release_owner(m);
        dice_close();
        free(m);
        return MBOX3_ERR_IO;
    }
    m->thread_started = 1;
    *out = m;
    return MBOX3_OK;
}

void mbox3_close(mbox3 *m)
{
    if (!m) return;
    mbox3_stop(m);
    if (m->thread_started) {
        pthread_mutex_lock(&m->lock);
        m->cmd = CMD_QUIT;
        pthread_mutex_unlock(&m->lock);
        pthread_join(m->thread, NULL);
    }
    if (m->owned) release_owner(m);
    dice_close();
    pthread_mutex_destroy(&m->lock);
    pthread_mutex_destroy(&m->reg);
    pthread_cond_destroy(&m->cond);
    free(m);
}

int mbox3_start(mbox3 *m, const mbox3_config *cfg)
{
    if (!m) return MBOX3_ERR_ARG;
    mbox3_config c = {0};
    if (cfg) c = *cfg;
    if (c.sample_rate == 0) c.sample_rate = MBOX3_SAMPLE_RATE;
    if (c.syt_latency == 0) c.syt_latency = 6;
    if (c.ring_frames == 0) c.ring_frames = 16384;
    if (c.sample_rate != 44100 && c.sample_rate != 48000 && c.sample_rate != 88200 && c.sample_rate != 96000) return MBOX3_ERR_ARG;
    if (c.syt_latency < 2 || c.syt_latency > 10) return MBOX3_ERR_ARG;
    if (c.ring_frames < 1024 || (c.ring_frames & (c.ring_frames - 1))) return MBOX3_ERR_ARG;
    if (c.routes && (c.n_routes == 0 || c.n_routes > 64)) return MBOX3_ERR_ARG;
    if (c.prefill_frames > c.ring_frames / 2) return MBOX3_ERR_ARG;
    if (c.tx_ahead_slots == 0) c.tx_ahead_slots = TX_AHEAD_DEFAULT;
    if (c.tx_ahead_slots < TX_AHEAD_MIN || c.tx_ahead_slots > TX_AHEAD_MAX) return MBOX3_ERR_ARG;

    pthread_mutex_lock(&m->lock);
    if (m->state != ST_IDLE || m->cmd != CMD_NONE) {
        pthread_mutex_unlock(&m->lock);
        return MBOX3_ERR_BUSY;
    }
    g_rate = c.sample_rate;
    m->cfg = c;
    if (c.routes) {
        memcpy(m->cfg_routes, c.routes, c.n_routes * sizeof(uint32_t));
        m->cfg.routes = m->cfg_routes;
    }
    m->ring_frames = c.ring_frames;
    m->state = ST_STARTING;
    m->cmd = CMD_START;
    while (m->state == ST_STARTING || m->state == ST_WARMUP)
        pthread_cond_wait(&m->cond, &m->lock);
    int rc = m->result;
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int mbox3_stop(mbox3 *m)
{
    if (!m) return MBOX3_ERR_ARG;
    pthread_mutex_lock(&m->lock);
    if (m->state == ST_IDLE) {
        pthread_mutex_unlock(&m->lock);
        return MBOX3_OK;
    }
    m->cmd = CMD_STOP;
    while (m->cmd == CMD_STOP) pthread_cond_wait(&m->cond, &m->lock);
    pthread_mutex_unlock(&m->lock);
    return MBOX3_OK;
}

void mbox3_get_stats(mbox3 *m, mbox3_stats *st)
{
    memset(st, 0, sizeof(*st));
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    st->running = m->state == ST_RUN;
    pthread_mutex_unlock(&m->lock);
    st->clock_locked = atomic_load(&m->st_locked);
    st->capture_frames = atomic_load(&m->st_cap_frames);
    st->playback_frames = atomic_load(&m->st_play_frames);
    st->playback_queued = (unsigned)(atomic_load(&m->play_w) - atomic_load(&m->play_r));
    st->capture_queued = (unsigned)(atomic_load(&m->cap_w) - atomic_load(&m->cap_r));
    st->playback_underruns = atomic_load(&m->st_underruns);
    st->capture_overruns = atomic_load(&m->st_overruns);
    st->capture_dbc_errors = atomic_load(&m->st_dbc_errors);
    st->late_slots = atomic_load(&m->st_late);
    st->max_service_gap_ms = (double)atomic_load(&m->st_max_gap_us) / 1000.0;
    st->capture_host_ns = atomic_load(&m->st_cap_ns);
}

int mbox3_mixer_set(mbox3 *m, unsigned out, unsigned in, unsigned value)
{
    uint32_t base, size;
    if (!m || out >= MBOX3_MIXER_OUTPUTS || in >= MBOX3_MIXER_INPUTS || value > 0x7fff)
        return MBOX3_ERR_ARG;
    if (dice_ext_section(2, &base, &size) != 0) return MBOX3_ERR_IO;
    return wr_q(m, base + 4 + (out * MBOX3_MIXER_INPUTS + in) * 4, value) == 0 ? MBOX3_OK : MBOX3_ERR_IO;
}

int mbox3_mixer_get(mbox3 *m, unsigned out, unsigned in, unsigned *value)
{
    uint32_t base, size, v;
    if (!m || !value || out >= MBOX3_MIXER_OUTPUTS || in >= MBOX3_MIXER_INPUTS) return MBOX3_ERR_ARG;
    if (dice_ext_section(2, &base, &size) != 0) return MBOX3_ERR_IO;
    if (rd_q(m, base + 4 + (out * MBOX3_MIXER_INPUTS + in) * 4, &v) != 0) return MBOX3_ERR_IO;
    *value = v;
    return MBOX3_OK;
}

int mbox3_read_peaks(mbox3 *m, uint32_t *entries, unsigned max_entries)
{
    uint32_t off, size;
    uint8_t buf[128 * 4];
    if (!m || !entries) return MBOX3_ERR_ARG;
    if (dice_ext_section(3, &off, &size) != 0) return MBOX3_ERR_IO;
    if (size > sizeof(buf)) size = sizeof(buf);
    if (rd(m, off, buf, size) != 0) return MBOX3_ERR_IO;
    unsigned n = size / 4;
    if (n > max_entries) n = max_entries;
    for (unsigned i = 0; i < n; i++) entries[i] = dice_be32(buf + i * 4);
    return (int)n;
}

int mbox3_params_get(mbox3 *m, mbox3_params *p)
{
    uint32_t base, size;
    uint8_t raw[MBOX3_APP_SECTION_MIN_SIZE];
    if (!m || !p) return MBOX3_ERR_ARG;
    if (dice_ext_section(8, &base, &size) != 0 || size < MBOX3_APP_SECTION_MIN_SIZE) return MBOX3_ERR_IO;
    if (rd(m, base, raw, sizeof raw) != 0) return MBOX3_ERR_IO;
    return mbox3_params_deserialize(p, raw, sizeof raw) == 0 ? MBOX3_OK : MBOX3_ERR_IO;
}

int mbox3_params_set(mbox3 *m, const mbox3_params *p)
{
    uint32_t base, size;
    uint8_t old[MBOX3_APP_SECTION_MIN_SIZE], nw[MBOX3_APP_SECTION_MIN_SIZE];
    if (!m || !p) return MBOX3_ERR_ARG;
    if (dice_ext_section(8, &base, &size) != 0 || size < MBOX3_APP_SECTION_MIN_SIZE) return MBOX3_ERR_IO;
    if (rd(m, base, old, sizeof old) != 0) return MBOX3_ERR_IO;
    memcpy(nw, old, sizeof nw);
    if (mbox3_params_serialize(p, nw, sizeof nw) != 0) return MBOX3_ERR_ARG;
    for (uint32_t q = 0; q < sizeof nw; q += 4) {
        if (!memcmp(nw + q, old + q, 4)) continue;
        if (wr_q(m, base + q, dice_be32(nw + q)) != 0) return MBOX3_ERR_IO;
    }
    return MBOX3_OK;
}

int mbox3_mixer_read_all(mbox3 *m, uint16_t *cells, uint32_t *saturation)
{
    uint32_t base, size;
    if (!m || !cells) return MBOX3_ERR_ARG;
    if (dice_ext_section(2, &base, &size) != 0 ||
        size < 4 + MBOX3_MIXER_INPUTS * MBOX3_MIXER_OUTPUTS * 4) return MBOX3_ERR_IO;
    uint8_t buf[4 + MBOX3_MIXER_INPUTS * MBOX3_MIXER_OUTPUTS * 4];
    pthread_mutex_lock(&m->reg);
    IOReturn r = dice_read(base, buf, sizeof buf);
    pthread_mutex_unlock(&m->reg);
    if (r != kIOReturnSuccess) return MBOX3_ERR_IO;
    if (saturation) *saturation = dice_be32(buf);
    for (unsigned i = 0; i < MBOX3_MIXER_INPUTS * MBOX3_MIXER_OUTPUTS; i++)
        cells[i] = (uint16_t)dice_be32(buf + 4 + i * 4);
    return MBOX3_OK;
}
