/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * mbox3d: owns the Mbox 3 Pro, streams through libmbox3 and shares the audio
 * with the Audio Server plug-in through shared memory (see driver/proto.h).
 * Runs as a LaunchDaemon; retries when the device is absent and restarts the
 * stream when it stops delivering audio (cable pulled, power cycled).
 */
#include <pwd.h>
#include "mbox3.h"
#include "ctlserver.h"
#include "../driver/proto.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xpc/xpc.h>

static m3_shm_header *g_shm;
static volatile sig_atomic_t g_quit;

static void log_cb(const char *msg) { fprintf(stderr, "mbox3d: %s\n", msg); }

static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t uptime_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static unsigned g_rate = M3_SAMPLE_RATE;   /* the rate of the running (or next) session */
#define NS_PER_FRAME (1e9 / (double)g_rate)
#define PUMP_BLOCK 64
#define RATE_FILE "/var/db/mbox3d.rate"   /* 44100; other rates add ".<rate>" */
static void rate_path(char *p, size_t n) { if (g_rate == 44100) snprintf(p, n, RATE_FILE); else snprintf(p, n, RATE_FILE ".%u", g_rate); }
#define RATE_TOL 150e-6

/* The host clock and the bus-clocked device differ by a constant few tens of
 * ppm on a given machine. The last measured ns/frame is kept across sessions
 * and daemon restarts so the timeline is right from the first second. */
static double g_prior_nsf = 1e9 / M3_SAMPLE_RATE;

/* Playback pipeline, tunable in /var/db/mbox3d.conf ("prefill N", "queue N"):
 * prefill = silence ahead of the audio, i.e. the output latency apart from the
 *           send look-ahead of the library (ahead slots x 5.5 frames);
 * queue   = frames the pump keeps queued in the library (pump jitter margin).
 * The pump takes shm frame T when the device clock is at T + (prefill - queue),
 * which is also the time CoreAudio has beyond its own write lead to deliver it. */
#define CONF_FILE "/var/db/mbox3d.conf"
static unsigned g_prefill_base = M3_PREFILL_FRAMES, g_queue_base = M3_QUEUE_FRAMES;   /* for 44.1 / 48 kHz */
static unsigned g_prefill = M3_PREFILL_FRAMES, g_queue = M3_QUEUE_FRAMES;               /* for the rate in use */
/* The frame counts are for 44.1 / 48 kHz; at 88.2 / 96 kHz they are doubled so the margins last as long in time. */
static void scale_for_rate(void) { g_prefill = g_prefill_base * M3_RATE_MULT(g_rate); g_queue = g_queue_base * M3_RATE_MULT(g_rate); }
#define MAX_AUTO_RESTARTS 4
static int g_auto_restarts;   /* restarts in a row that were made because the stream was unhealthy */
static unsigned g_ahead = M3_TX_AHEAD_SLOTS;   /* send look-ahead in slots */
static unsigned g_slack_base = M3_OUT_SLACK;   /* write slack of the plug-in's output cursor, frames at 44.1 / 48 kHz */

static void load_conf(void)
{
    FILE *f = fopen(CONF_FILE, "r");
    if (!f) return;
    char key[32]; unsigned v;
    while (fscanf(f, "%31s %u", key, &v) == 2) {
        if (!strcmp(key, "prefill") && v >= 256 && v <= 4096) g_prefill_base = v;
        else if (!strcmp(key, "queue") && v >= 128 && v <= 4096) g_queue_base = v;
        else if (!strcmp(key, "ahead") && v >= 48 && v <= 480) g_ahead = v;
        else if (!strcmp(key, "slack") && v <= 2048) g_slack_base = v;
    }
    fclose(f);
}

static void load_prior(void)
{
    g_prior_nsf = NS_PER_FRAME;
    char p[64]; rate_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    double v = 0;
    if (fscanf(f, "%lf", &v) == 1 && v > NS_PER_FRAME * (1 - RATE_TOL) && v < NS_PER_FRAME * (1 + RATE_TOL))
        g_prior_nsf = v;
    fclose(f);
}

static void save_prior(double v)
{
    g_prior_nsf = v;
    char p[64]; rate_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%.4f\n", v);
    fclose(f);
}

/* One streaming session. Returns when the device stops delivering audio or
 * on shutdown; the caller closes the device and retries. */
static void run_session(mbox3 *m)
{
    m3_shm_header *h = g_shm;
    int32_t *cap = m3_cap_ring(h), *play = m3_play_ring(h);
    const unsigned mask = M3_RING_FRAMES - 1;
    int32_t buf[256 * M3_CHANNELS];

    /* new timeline */
    atomic_store(&h->cap_w, 0);
    atomic_store(&h->play_pos, 0);
    memset(play, 0, M3_RING_BYTES);
    /* a timeline id that differs from every earlier one, also from those of
     * earlier daemon processes (the plug-in drops its cursors when it changes) */
    {
        uint32_t id = (uint32_t)(uptime_ns() / 1000) | 1u;
        if (id == atomic_load(&h->seed)) id += 2;
        atomic_store(&h->seed, id);
    }
    atomic_store(&h->max_pump_gap_us, 0);
    atomic_store(&h->state, M3_STATE_STREAMING);

    uint64_t cap_w = 0, last_frames = 0, cap_ins = 0, drops_seen = 0;   /* cap_ins: silence the daemon put into the capture ring */
    uint64_t pushed = g_prefill;   /* frames handed to the library, prefill included */
    uint64_t last_progress = uptime_ns(), session_start = last_progress;
    int64_t c0 = 0; int have_c0 = 0, resynced = 0, fit_warned = 0, bad_start_checked = 0, healthy_reset = 0, base_taken = 0;
    uint64_t b_under = 0, b_over = 0, b_late = 0;   /* counters 2 s after the start, the baseline for the 88.2 / 96 kHz start check */
    uint64_t resync_t[4] = {0}; unsigned resync_n = 0;
    uint64_t rt0 = 0, rc0 = 0, rp0 = 0;   /* for the periodic rate log */
    uint32_t pk = 0; uint64_t pk_t = 0;   /* peak of the audio handed to the device, for the diagnostics */
    double base = 0;
    unsigned nbase = 0;
    int have_base = 0;
    /* long-window regression of host time against capture frames: the host
     * uptime clock and the bus-clocked device clock run at slightly different
     * speeds, so the timeline needs the measured rate, not 44100 Hz */
    enum { NPT = 240 };
    static double px[NPT], py[NPT];
    unsigned np = 0, pi = 0;
    uint64_t last_pt = 0, last_fit = 0, last_save = 0, last_iter = 0;
    atomic_store(&h->tl_valid, 0);

    while (!g_quit) {
        atomic_fetch_add(&h->heartbeat, 1);
        {   /* the plug-in asks for another rate: end the session, the caller restarts at the new rate */
            uint32_t rq = atomic_load(&h->req_rate);
            if (M3_RATE_OK(rq) && rq != g_rate) {
                fprintf(stderr, "mbox3d: sample rate change %u -> %u\n", g_rate, rq);
                break;
            }
        }
        {
            uint64_t t_iter = uptime_ns();
            if (last_iter) {
                uint32_t g = (uint32_t)((t_iter - last_iter) / 1000);
                if (g > atomic_load(&h->max_pump_gap_us)) atomic_store(&h->max_pump_gap_us, g);
            }
            last_iter = t_iter;
        }

        /* device -> shared memory */
        size_t n;
        while ((n = mbox3_read_frames(m, buf, 256)) > 0) {
            for (size_t i = 0; i < n; i++)
                memcpy(cap + (size_t)((cap_w + i) & mask) * M3_CHANNELS, buf + i * M3_CHANNELS,
                       M3_CHANNELS * sizeof(int32_t));
            cap_w += n;
            atomic_store_explicit(&h->cap_w, cap_w, memory_order_release);
        }

        mbox3_stats st;
        mbox3_get_stats(m, &st);
        if (st.capture_overruns > drops_seen) {
            /* Frames the library had to drop (its ring was full before the pump got to read it): they are device time
             * that never reached the shared ring. Count them as silence, or the capture timeline, and with it the
             * position the plug-in writes to, slips by that many frames for the rest of the session. */
            uint64_t need = st.capture_overruns - drops_seen;
            drops_seen = st.capture_overruns;
            for (uint64_t i = 0; i < need; i++)
                memset(cap + (size_t)((cap_w + i) & mask) * M3_CHANNELS, 0, M3_CHANNELS * sizeof(int32_t));
            cap_w += need; cap_ins += need;
            atomic_store_explicit(&h->cap_w, cap_w, memory_order_release);
        }
        atomic_store(&h->max_gap_us, (uint32_t)(st.max_service_gap_ms * 1000.0));

        /* Self-healing. A start that went wrong (the IO thread stalled while the streams were starting) leaves the playback
         * timeline misaligned for the whole session: lots of underruns / late slots from the first seconds, and the sound
         * is silent or just pops. A restart gets a clean start. At most MAX_AUTO_RESTARTS in a row. */
        if (!base_taken && uptime_ns() - session_start > 2000000000ull) {
            base_taken = 1;
            b_under = st.playback_underruns; b_over = st.capture_overruns; b_late = st.late_slots;
        }
        if (!bad_start_checked && uptime_ns() - session_start > 6000000000ull) {
            bad_start_checked = 1;
            /* 44.1 / 48 kHz: the counts since the start. 88.2 / 96 kHz: the start-up itself leaves 10-20 thousand
             * overruns behind (the capture ring fills before the pump runs), so only what grows after the first 2 s counts. */
            int hi = M3_RATE_MULT(g_rate) > 1;
            uint64_t u = hi ? st.playback_underruns - b_under : st.playback_underruns;
            uint64_t o = hi ? st.capture_overruns - b_over : st.capture_overruns;
            uint64_t l = hi ? st.late_slots - b_late : st.late_slots;
            if (u > 3000 || o > 3000 || l > 300) {
                if (g_auto_restarts < MAX_AUTO_RESTARTS) {
                    g_auto_restarts++;
                    fprintf(stderr, "mbox3d: bad start (under %u, over %u, late %u, dbc %u); restarting the stream (%d of %d)\n",
                            (unsigned)st.playback_underruns, (unsigned)st.capture_overruns, (unsigned)st.late_slots,
                            (unsigned)st.capture_dbc_errors, g_auto_restarts, MAX_AUTO_RESTARTS);
                    break;
                }
                fprintf(stderr, "mbox3d: bad start (under %u, over %u, late %u); not restarting again\n",
                        (unsigned)st.playback_underruns, (unsigned)st.capture_overruns, (unsigned)st.late_slots);
            }
        }
        if (!healthy_reset && uptime_ns() - session_start > 120000000000ull) { healthy_reset = 1; g_auto_restarts = 0; }   /* it has run for 2 minutes: healthy */

        /* clock anchor: time at which capture frame 0 happened, smoothed */
        if (st.capture_frames != last_frames && st.capture_host_ns) {
            last_frames = st.capture_frames;
            last_progress = uptime_ns();
            double off = (double)st.capture_host_ns - (double)(st.capture_frames + cap_ins) * NS_PER_FRAME;
            /* running mean at first, then a slow exponential average (about 8 s):
             * the plug-in's timeline must be smooth, a few hundred microseconds of
             * noise in it shows up as repeated or dropped samples */
            if (nbase < 4096) nbase++;
            if (!have_base) { base = off; have_base = 1; }
            else base += (off - base) / (double)nbase;
            atomic_store(&h->base_ns, (uint64_t)base);

            uint64_t now = uptime_ns();
            if (now - last_pt >= 250000000ull) {
                last_pt = now;
                px[pi] = (double)(st.capture_frames + cap_ins); py[pi] = (double)st.capture_host_ns;
                pi = (pi + 1) % NPT; if (np < NPT) np++;
            }
            /* Until 20 s of points exist the slope is the machine's known rate. Later it is the median slope
             * between points half a window apart (Theil-Sen): a stall of the IO thread delays some of the host
             * timestamps, and a least-squares fit follows those outliers. The intercept is the median of the
             * residuals for the same reason. */
            if (np >= 8 && now - last_fit >= 1000000000ull) {
                last_fit = now;
                static double xs[NPT], ys[NPT], sl[NPT], rs[NPT];
                for (unsigned k = 0; k < np; k++) { unsigned ix = (pi + NPT - np + k) % NPT; xs[k] = px[ix]; ys[k] = py[ix]; }
                double nsf = g_prior_nsf;
                if (np >= 80) {
                    unsigned half = np / 2, ns = 0;
                    for (unsigned k = 0; k + half < np; k++) {
                        double dx = xs[k + half] - xs[k];
                        if (dx > 0) sl[ns++] = (ys[k + half] - ys[k]) / dx;
                    }
                    if (ns) {
                        qsort(sl, ns, sizeof(double), cmp_double);
                        double v = sl[ns / 2];
                        if (v > NS_PER_FRAME * (1 - RATE_TOL) && v < NS_PER_FRAME * (1 + RATE_TOL)) nsf = v;
                        else if (!fit_warned) { fit_warned = 1; fprintf(stderr, "mbox3d: measured rate %.3f ns/frame is out of range, using %.3f\n", v, nsf); }
                    }
                }
                for (unsigned k = 0; k < np; k++) rs[k] = ys[k] - nsf * xs[k];
                qsort(rs, np, sizeof(double), cmp_double);
                double c = rs[np / 2], x0 = xs[np - 1];
                uint32_t sq = atomic_load(&h->tl_seq);
                atomic_store(&h->tl_seq, sq + 1);
                uint64_t bits; memcpy(&bits, &nsf, 8);
                atomic_store(&h->tl_anchor_frame, (uint64_t)x0);
                atomic_store(&h->tl_anchor_ns, (uint64_t)(c + nsf * x0));
                atomic_store(&h->tl_nsf_bits, bits);
                atomic_store(&h->tl_valid, 1);
                atomic_store(&h->tl_seq, sq + 2);
                if (np == NPT && nsf != g_prior_nsf && now - last_save >= 60000000000ull) { last_save = now; save_prior(nsf); }
            }
        }
        if (uptime_ns() - last_progress > 1500000000ull) {
            fprintf(stderr, "mbox3d: no audio from the device for 1.5 s, restarting\n");
            break;
        }

        /* shared memory -> device, in step with the device clock.
         * V is the device timeline in frames (silence packets sent while the
         * queue was empty count as elapsed time). Lib frame L belongs to shm
         * frame L - prefill. */
        uint64_t V = (uint64_t)st.playback_frames + 8ull * M3_RATE_MULT(g_rate) * st.playback_underruns;   /* a silent packet is 8 frames, 16 at 88.2 / 96 kHz */
        {   /* every 10 min: how fast do capture and playback time run against the host clock? */
            uint64_t tn = uptime_ns();
            uint64_t cnow = st.capture_frames + cap_ins;
            if (!rt0) { rt0 = tn; rc0 = cnow; rp0 = V; }
            else if (tn - rt0 >= 600000000000ull) {
                double dt = (double)(tn - rt0) / 1e9;
                fprintf(stderr, "mbox3d: rates over %.1f s: capture %.1f fr/s, playback %.1f fr/s, lag-c0 %lld, queue %lld, under %u, late %u\n",
                        dt, (double)(cnow - rc0) / dt, (double)(V - rp0) / dt, (long long)((int64_t)cnow - (int64_t)V - c0),
                        (long long)((int64_t)pushed - (int64_t)V), (unsigned)st.playback_underruns, (unsigned)st.late_slots);
                rt0 = tn; rc0 = cnow; rp0 = V;
            }
        }
        {
            /* Capture and playback run on the same device clock, so playback time minus capture time is
             * constant. If the library lost track of playback time (a long stall of its IO thread), the
             * difference grows: follow the capture timeline, which is what CoreAudio's timeline is. */
            int64_t lag = (int64_t)(st.capture_frames + cap_ins) - (int64_t)V;
            if (!have_c0) {
                if (uptime_ns() - session_start > 2500000000ull && st.capture_frames > 0) { c0 = lag; have_c0 = 1; }
            } else if (lag - c0 > (int64_t)(1024 * M3_RATE_MULT(g_rate))) {
                /* playback time fell behind: the library missed laps of its send ring that it could not count */
                if (!resynced) fprintf(stderr, "mbox3d: playback timeline fell behind by %lld frames, following the capture clock (t=%llu ms, cap %llu+%llu, play %llu, under %u, late %u, dbc %u)\n", (long long)(lag - c0), (unsigned long long)((uptime_ns() - session_start) / 1000000), (unsigned long long)st.capture_frames, (unsigned long long)cap_ins, (unsigned long long)st.playback_frames, (unsigned)st.playback_underruns, (unsigned)st.late_slots, (unsigned)st.capture_dbc_errors);
                resynced = 1;
                {   /* the timeline keeps falling behind: the send side is losing laps over and over, restart */
                    uint64_t tnow = uptime_ns();
                    resync_t[resync_n++ % 4] = tnow;
                    if (resync_n >= 4 && tnow - resync_t[resync_n % 4] < 10000000000ull && g_auto_restarts < MAX_AUTO_RESTARTS) {
                        g_auto_restarts++;
                        fprintf(stderr, "mbox3d: the playback timeline keeps falling behind; restarting the stream (%d of %d)\n", g_auto_restarts, MAX_AUTO_RESTARTS);
                        break;
                    }
                }
                V += (uint64_t)(lag - c0);
            } else if (c0 - lag > (int64_t)(1024 * M3_RATE_MULT(g_rate))) {
                /* capture time fell behind: packets were lost beyond what the data block counter can tell.
                 * Put silence into the capture ring so the timeline CoreAudio uses counts device time. */
                uint64_t need = (uint64_t)(c0 - lag);
                if (!resynced) fprintf(stderr, "mbox3d: capture fell behind by %llu frames, inserting silence (t=%llu ms, cap %llu+%llu, play %llu, under %u, late %u, dbc %u)\n", (unsigned long long)need, (unsigned long long)((uptime_ns() - session_start) / 1000000), (unsigned long long)st.capture_frames, (unsigned long long)cap_ins, (unsigned long long)st.playback_frames, (unsigned)st.playback_underruns, (unsigned)st.late_slots, (unsigned)st.capture_dbc_errors);
                resynced = 1;
                for (uint64_t i = 0; i < need; i++)
                    memset(cap + (size_t)((cap_w + i) & mask) * M3_CHANNELS, 0, M3_CHANNELS * sizeof(int32_t));
                cap_w += need; cap_ins += need;
                atomic_store_explicit(&h->cap_w, cap_w, memory_order_release);
            } else resynced = 0;
        }
        if (pushed < V && V - pushed > 4096) {
            /* A big gap (the silent packets the device sent before the first frames arrived: about 2000 packets, more
             * than the library's ring holds at 48 kHz and above). Writing that much silence can never catch up, the
             * pump would then replace every frame with silence for good. The device has played the silence already:
             * only move the time forward and drop the shm frames that were due meanwhile. */
            uint64_t from = pushed - g_prefill, n = V - pushed;
            if (n > (uint64_t)mask + 1) { from += n - ((uint64_t)mask + 1); n = (uint64_t)mask + 1; }
            for (uint64_t i = 0; i < n; i++)
                memset(play + (size_t)((from + i) & mask) * M3_CHANNELS, 0, M3_CHANNELS * sizeof(int32_t));
            pushed = V;
        }
        if (pushed < V) {
            /* the queue ran dry: replace the gap with silence so the timeline stays
             * aligned, and drop the shm frames that were due in the meantime */
            static const int32_t zeros[PUMP_BLOCK * M3_CHANNELS];
            while (pushed < V) {
                unsigned n = V - pushed > PUMP_BLOCK ? PUMP_BLOCK : (unsigned)(V - pushed);
                size_t w = mbox3_write_frames(m, zeros, n);
                for (size_t i = 0; i < w; i++)
                    memset(play + (size_t)((pushed - g_prefill + i) & mask) * M3_CHANNELS, 0,
                           M3_CHANNELS * sizeof(int32_t));
                pushed += w;
                if (w < n) break;
            }
        }
        while (pushed < V + g_queue) {
            uint64_t pos = pushed - g_prefill;
            int32_t out[PUMP_BLOCK * M3_CHANNELS];
            for (unsigned i = 0; i < PUMP_BLOCK; i++)
                memcpy(out + i * M3_CHANNELS, play + (size_t)((pos + i) & mask) * M3_CHANNELS,
                       M3_CHANNELS * sizeof(int32_t));
            size_t w = mbox3_write_frames(m, out, PUMP_BLOCK);
            for (size_t i = 0; i < w * M3_CHANNELS; i++) { int32_t a = out[i] < 0 ? -out[i] : out[i]; if ((uint32_t)a > pk) pk = (uint32_t)a; }
            for (size_t i = 0; i < w; i++)
                memset(play + (size_t)((pos + i) & mask) * M3_CHANNELS, 0, M3_CHANNELS * sizeof(int32_t));
            pushed += w;
            if (w < PUMP_BLOCK) break;
        }
        atomic_store_explicit(&h->play_pos, pushed - g_prefill, memory_order_release);
        if (uptime_ns() - pk_t >= 250000000ull) { pk_t = uptime_ns(); atomic_store(&h->play_peak, pk); pk = 0; }

        atomic_store(&h->underruns, st.playback_underruns);
        atomic_store(&h->overruns, st.capture_overruns);
        atomic_store(&h->dbc_errors, st.capture_dbc_errors);
        atomic_store(&h->late_slots, st.late_slots);
        atomic_store(&h->clock_locked, (uint32_t)st.clock_locked);
        sleep_ms(1);
    }
    atomic_store(&h->cur_rate, 0);
    atomic_store(&h->state, M3_STATE_NO_DEVICE);
}

static void *pump_thread(void *arg)
{
    (void)arg;
    mbox3_make_thread_realtime(1000);   /* the pump must not stall: a stall is an underrun */
    int waiting = 0;   /* logged once per absence, not every 2 s */
    int lock_failed = 0;
    while (!g_quit) {
        mbox3 *m = NULL;
        atomic_store(&g_shm->state, M3_STATE_NO_DEVICE);
        int rc = mbox3_open(&m);
        if (rc != MBOX3_OK) {
            if (!waiting) { waiting = 1; fprintf(stderr, "mbox3d: no device (%s); waiting for it to be plugged in\n", mbox3_strerror(rc)); }
            atomic_fetch_add(&g_shm->heartbeat, 1);
            sleep_ms(2000);
            continue;
        }
        if (waiting) { waiting = 0; fprintf(stderr, "mbox3d: device found again\n"); }
        {
            uint32_t rq = atomic_load(&g_shm->req_rate);
            if (M3_RATE_OK(rq) && rq != g_rate) { g_rate = rq; load_prior(); }
            scale_for_rate();
        }
        {
            int extra = (int)g_prefill - (int)g_queue;
            if (extra < 0) extra = 0;
            atomic_store(&g_shm->out_lead_extra, (uint32_t)extra + g_slack_base * M3_RATE_MULT(g_rate));
        }
        atomic_store(&g_shm->state, M3_STATE_STARTING);
        mbox3_config cfg = {0};
        cfg.sample_rate = g_rate;
        cfg.ring_frames = 16384 * M3_RATE_MULT(g_rate);   /* the same time span at 88.2 / 96 kHz: the pump starts reading about 0.35 s after the lock */
        cfg.prefill_frames = g_prefill;
        cfg.tx_ahead_slots = g_ahead;
        rc = mbox3_start(m, &cfg);
        if (rc == MBOX3_OK) {
            fprintf(stderr, "mbox3d: streaming at %u Hz\n", g_rate);
            atomic_store(&g_shm->cur_rate, g_rate);
            ctl_server_set_device(m, g_rate);
            run_session(m);
        } else {
            fprintf(stderr, "mbox3d: start failed: %s\n", mbox3_strerror(rc));
            if (rc == MBOX3_ERR_LOCK) {
                lock_failed = 1;
                fprintf(stderr, "mbox3d: (the device often needs a few seconds after the previous program that used it; trying again)\n");
            }
        }
        ctl_server_set_device(NULL, 0);
        mbox3_stop(m);
        mbox3_close(m);
        atomic_store(&g_shm->state, M3_STATE_NO_DEVICE);
        {
            uint32_t rq = atomic_load(&g_shm->req_rate);
            int switching = M3_RATE_OK(rq) && rq != g_rate;
            /* a failed clock lock is retried quickly: the second start has always worked */
            if (!g_quit) sleep_ms((switching || lock_failed) ? 500 : 2000);
            lock_failed = 0;
        }
    }
    return NULL;
}

/* who may connect over XPC: root and the Core Audio user. The lookup is done once; if the user does not exist
 * nobody but root is accepted, and the log says so. */
static int peer_allowed(uid_t uid)
{
    static uid_t ca_uid = (uid_t)-1;
    static int looked_up;
    if (!looked_up) {
        looked_up = 1;
        struct passwd *pw = getpwnam("_coreaudiod");
        if (pw) ca_uid = pw->pw_uid;
        else fprintf(stderr, "mbox3d: user _coreaudiod not found; only root may connect over XPC\n");
    }
    return uid == 0 || uid == ca_uid;
}

static void on_signal(int sig) { (void)sig; g_quit = 1; }

int main(void)
{
    mbox3_set_log(log_cb);
    load_prior();
    load_conf();
    scale_for_rate();
    fprintf(stderr, "mbox3d: prefill %u frames, queue %u frames, send look-ahead %u slots, write slack %u frames\n", g_prefill, g_queue, g_ahead, g_slack_base);
    fprintf(stderr, "mbox3d: starting timeline rate %.3f ns/frame\n", g_prior_nsf);
    signal(SIGTERM, SIG_IGN);       /* handled by the dispatch source below */
    signal(SIGINT, on_signal);

    void *mem = mmap(NULL, M3_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    g_shm = mem;
    g_shm->magic = M3_MAGIC;
    g_shm->version = M3_VERSION;
    g_shm->channels = M3_CHANNELS;
    g_shm->ring_frames = M3_RING_FRAMES;
    g_shm->sample_rate = M3_SAMPLE_RATE;
    atomic_store(&g_shm->req_rate, 0);
    atomic_store(&g_shm->cur_rate, 0);
    g_shm->header_size = M3_HEADER_SIZE;
    atomic_store(&g_shm->write_margin_min, INT32_MAX);
    atomic_store(&g_shm->io_jump_max, 0);
    atomic_store(&g_shm->read_margin_min, INT32_MAX);

    xpc_connection_t listener = xpc_connection_create_mach_service(
        M3_SERVICE, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) { fprintf(stderr, "mbox3d: cannot create the mach service\n"); return 1; }
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        /* The shared memory holds the device's input audio and is the way to play through the device, so only the
         * process that hosts the plug-in (coreaudiod, user _coreaudiod) and root may have it. */
        uid_t peer_uid = xpc_connection_get_euid(peer);
        if (!peer_allowed(peer_uid)) {
            static int warned;
            if (warned < 5) { warned++; fprintf(stderr, "mbox3d: refused an XPC connection from uid %u (pid %d)\n", (unsigned)peer_uid, (int)xpc_connection_get_pid(peer)); }
            xpc_connection_cancel(peer);
            return;
        }
        xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
            if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) return;
            xpc_object_t reply = xpc_dictionary_create_reply(msg);
            if (!reply) return;
            xpc_object_t shm = xpc_shmem_create(g_shm, M3_SHM_SIZE);
            xpc_dictionary_set_uint64(reply, "version", M3_VERSION);
            xpc_dictionary_set_uint64(reply, "size", M3_SHM_SIZE);
            if (shm) xpc_dictionary_set_value(reply, "shm", shm);
            xpc_connection_send_message(xpc_dictionary_get_remote_connection(msg), reply);
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);

    if (ctl_server_start(CTL_PORT) != 0) fprintf(stderr, "mbox3d: control server: cannot listen on 127.0.0.1:%d\n", CTL_PORT);
    else fprintf(stderr, "mbox3d: control panel at http://127.0.0.1:%d/\n", CTL_PORT);
    pthread_t th;
    pthread_create(&th, NULL, pump_thread, NULL);

    dispatch_source_t sig = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sig, ^{ g_quit = 1; pthread_join(th, NULL); exit(0); });
    dispatch_resume(sig);
    dispatch_main();
}
