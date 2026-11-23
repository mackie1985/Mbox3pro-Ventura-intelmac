/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * mbox3_demo: exercises libmbox3 (open, start, play a tone, read the input
 * channels, print statistics, stop).
 *
 *   mbox3_demo tone SECS [FREQ] [loopback] [rate=48000|88200|96000]
 *   mbox3_demo latency [PREFILL_FRAMES] [rate=48000|88200|96000]
 *
 * Plays FREQ Hz (default 440) on playback channels 0 and 1. With "loopback"
 * the router sends playback channel 0 straight back to capture channel 0 and
 * the captured sine is checked sample by sample; without it the default
 * routing is used (tone on outputs 1/2, inputs show up in the peak columns).
 */
#include "mbox3.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned g_rate = 44100;   /* rate=48000 on the command line */

static void log_cb(const char *msg) { fprintf(stderr, "[mbox3] %s\n", msg); }

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#define BLOCK 64
#define TARGET_QUEUE 3072

/* Round trip through the device's internal loopback (playback ch0 -> capture ch0):
 * writes an impulse and reports how many frames later it comes back. The
 * playback stream starts with PREFILL silent frames, so the round trip minus
 * the prefill is the latency of the rest of the path (send ring look-ahead,
 * presentation time, converters, receive side). */
static int latency_test(unsigned prefill)
{
    mbox3_set_log(log_cb);
    mbox3 *m;
    int rc = mbox3_open(&m);
    if (rc != MBOX3_OK) { fprintf(stderr, "open: %s\n", mbox3_strerror(rc)); return 1; }
    static const uint32_t LOOP[] = { 0xb0b0, 0xb1b1, 0xb040, 0xb141, 0xb050, 0xb151 };
    mbox3_config cfg = {0};
    cfg.routes = LOOP; cfg.n_routes = 6; cfg.prefill_frames = prefill; cfg.sample_rate = g_rate;
    rc = mbox3_start(m, &cfg);
    if (rc != MBOX3_OK) { fprintf(stderr, "start: %s\n", mbox3_strerror(rc)); mbox3_close(m); return 1; }

    enum { TRIALS = 10 };
    double res[TRIALS]; unsigned nres = 0;
    uint64_t wpos = 0;                 /* playback frames written after the prefill */
    int32_t out[BLOCK * MBOX3_CHANNELS], in[256 * MBOX3_CHANNELS];
    uint64_t next_imp = 4410, imp_at = 0; int waiting = 0;
    uint64_t cpos = 0;                 /* capture frame index (counted from the start) */
    int maxabs = 0, chmax[MBOX3_CHANNELS] = {0}; unsigned long long nonzero = 0;
    for (unsigned t = 0; t < 4000 && nres < TRIALS; t++) {
        mbox3_stats st; mbox3_get_stats(m, &st);
        while (st.playback_queued < 2048) {   /* any value works: the round trip does not depend on the queue depth */
            memset(out, 0, sizeof(out));
            /* a gated 1 kHz tone (same amplitude as the tone test) of 441 frames starting at frame next_imp */
            for (unsigned i = 0; i < BLOCK; i++) {
                uint64_t g = wpos + i;
                if (g >= next_imp && g < next_imp + 441) {
                    int32_t v = (int32_t)(sin(2.0 * M_PI * 1000.0 / g_rate * (double)(g - next_imp)) * 0x180000);
                    if (g - next_imp < 4) v = 0x180000;   /* hard onset */
                    out[i * MBOX3_CHANNELS] = v; out[i * MBOX3_CHANNELS + 1] = v; nonzero++;
                }
            }
            size_t n = mbox3_write_frames(m, out, BLOCK);
            if (!waiting && wpos + n > next_imp) { waiting = 1; imp_at = next_imp; }
            wpos += n; st.playback_queued += (unsigned)n;
            if (n < BLOCK) break;
        }
        size_t got;
        while ((got = mbox3_read_frames(m, in, 256)) > 0) {
            for (size_t i = 0; i < got; i++, cpos++) {
                for (unsigned ch = 0; ch < MBOX3_CHANNELS; ch++)
                    if (abs(in[i * MBOX3_CHANNELS + ch]) > chmax[ch]) chmax[ch] = abs(in[i * MBOX3_CHANNELS + ch]);
                if (abs(in[i * MBOX3_CHANNELS]) > maxabs) maxabs = abs(in[i * MBOX3_CHANNELS]);
                if (waiting && abs(in[i * MBOX3_CHANNELS]) > 0x80000) {
                    res[nres++] = (double)cpos - (double)imp_at;   /* includes the prefill */
                    waiting = 0; next_imp = wpos + 8820 + (nres * 37) % 211;
                }
            }
        }
        sleep_ms(2);
    }
    if (!nres) {
        printf("no impulse came back: written %llu fr, captured %llu fr, impulse at %llu, waiting %d, "
               "largest |ch0| seen %d\n", (unsigned long long)wpos, (unsigned long long)cpos,
               (unsigned long long)imp_at, waiting, maxabs);
        printf("largest |value| per capture channel:");
        for (unsigned ch = 0; ch < MBOX3_CHANNELS; ch++) printf(" %06x", chmax[ch]);
        mbox3_stats st2; mbox3_get_stats(m, &st2);
        printf("\nplayback frames taken %llu, queued %u, running %d, clock locked %d, tone frames written %llu, under %u\n",
               (unsigned long long)st2.playback_frames, st2.playback_queued, st2.running, st2.clock_locked,
               nonzero, st2.playback_underruns);
    }
    else {
        double mn = res[0], mx = res[0], sum = 0;
        for (unsigned i = 0; i < nres; i++) { if (res[i] < mn) mn = res[i]; if (res[i] > mx) mx = res[i]; sum += res[i]; }
        printf("round trip over %u impulses: min %.0f, mean %.1f, max %.0f frames (%.1f ms mean)\n",
               nres, mn, sum / nres, mx, 1000.0 * sum / nres / g_rate);
        printf("prefill %u frames; rest of the path (round trip - prefill) = %.1f frames (%.1f ms)\n",
               prefill, sum / nres - prefill, 1000.0 * (sum / nres - prefill) / g_rate);
    }
    mbox3_stats st; mbox3_get_stats(m, &st);
    printf("under %u, late %u, dbc %u\n", st.playback_underruns, st.late_slots, st.capture_dbc_errors);
    mbox3_stop(m); mbox3_close(m);
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "rate=", 5)) { g_rate = (unsigned)atoi(argv[i] + 5); memmove(&argv[i], &argv[i + 1], (size_t)(argc - i) * sizeof(char *)); argc--; i--; }
    if (argc >= 2 && !strcmp(argv[1], "latency")) return latency_test(argc >= 3 ? (unsigned)atoi(argv[2]) : 0);
    if (argc < 3 || strcmp(argv[1], "tone")) {
        fprintf(stderr, "usage: %s tone SECS [FREQ] [loopback]\n", argv[0]);
        return 2;
    }
    unsigned secs = (unsigned)atoi(argv[2]);
    double freq = argc >= 4 && strcmp(argv[3], "loopback") ? atof(argv[3]) : 440.0;
    int loopback = 0;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "loopback")) loopback = 1;

    mbox3_set_log(log_cb);
    mbox3 *m;
    int rc = mbox3_open(&m);
    if (rc != MBOX3_OK) { fprintf(stderr, "open: %s\n", mbox3_strerror(rc)); return 1; }

    static const uint32_t LOOP[] = { 0xb0b0, 0xb1b1, 0xb040, 0xb141, 0xb050, 0xb151 };
    mbox3_config cfg = {0};
    cfg.sample_rate = g_rate;
    if (loopback) { cfg.routes = LOOP; cfg.n_routes = 6; }

    printf("starting (about 2 s) ...\n");
    rc = mbox3_start(m, &cfg);
    if (rc != MBOX3_OK) {
        fprintf(stderr, "start: %s\n", mbox3_strerror(rc));
        mbox3_close(m);
        return 1;
    }
    printf("running, %s routing, tone %.0f Hz, %u s\n", loopback ? "loopback" : "default", freq, secs);

    double w = 2.0 * M_PI * freq / g_rate, c2 = 2.0 * cos(w);
    uint64_t pos = 0;
    int32_t out[BLOCK * MBOX3_CHANNELS], in[256 * MBOX3_CHANNELS];
    int32_t peak[MBOX3_CHANNELS] = {0};
    double s1 = 0, s2 = 0;
    unsigned hist = 0, quiet = 0, glitches = 0, total_glitches = 0, armed = 0;
    uint64_t last_cap = 0;

    for (unsigned t = 0; t < secs * 500; t++) {
        mbox3_stats st;
        mbox3_get_stats(m, &st);
        while (st.playback_queued < TARGET_QUEUE) {
            memset(out, 0, sizeof(out));
            for (unsigned i = 0; i < BLOCK; i++) {
                int32_t v = (int32_t)(sin(w * (double)(pos + i)) * 0x180000);
                out[i * MBOX3_CHANNELS + 0] = v;
                out[i * MBOX3_CHANNELS + 1] = v;
            }
            size_t n = mbox3_write_frames(m, out, BLOCK);
            pos += n;
            st.playback_queued += (unsigned)n;
            if (n < BLOCK) break;
        }
        size_t got;
        while ((got = mbox3_read_frames(m, in, 256)) > 0) {
            for (size_t i = 0; i < got; i++) {
                for (unsigned ch = 0; ch < MBOX3_CHANNELS; ch++) {
                    int32_t a = abs(in[i * MBOX3_CHANNELS + ch]);
                    if (a > peak[ch]) peak[ch] = a;
                }
                double smp = in[i * MBOX3_CHANNELS];
                if (loopback) {
                    if (!armed) {
                        if (fabs(smp) > 1000.0) { armed = 1; hist = 0; quiet = 0; }
                    }
                    if (armed) {
                        if (hist >= 2 && quiet == 0) {
                            if (fabs(smp - (c2 * s1 - s2)) > 3000.0) { glitches++; quiet = 2; }
                        } else if (quiet > 0) quiet--;
                        s2 = s1; s1 = smp;
                        if (hist < 2) hist++;
                    }
                }
            }
        }
        if (t % 500 == 499) {
            mbox3_get_stats(m, &st);
            printf("t=%2us  rx %5llu fr  glitches %u  peaks", (t + 1) / 500,
                   (unsigned long long)(st.capture_frames - last_cap), glitches);
            for (unsigned ch = 0; ch < MBOX3_CHANNELS; ch++) printf(" %06x", peak[ch]);
            printf("  | under %u over %u dbc %u late %u queued %u locked %d\n",
                   st.playback_underruns, st.capture_overruns, st.capture_dbc_errors,
                   st.late_slots, st.playback_queued, st.clock_locked);
            last_cap = st.capture_frames;
            total_glitches += glitches;
            glitches = 0;
            memset(peak, 0, sizeof(peak));
        }
        sleep_ms(2);
    }
    mbox3_stats st;
    mbox3_get_stats(m, &st);
    printf("done: %llu frames in, %llu frames out, %u glitches in total, underruns %u, overruns %u, "
           "dbc errors %u, late slots %u, longest IO pause %.1f ms\n",
           (unsigned long long)st.capture_frames, (unsigned long long)st.playback_frames,
           total_glitches, st.playback_underruns, st.capture_overruns, st.capture_dbc_errors,
           st.late_slots, st.max_service_gap_ms);
    mbox3_stop(m);
    mbox3_close(m);
    return 0;
}
