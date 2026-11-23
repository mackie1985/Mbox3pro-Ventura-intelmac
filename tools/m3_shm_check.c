/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * m3_shm_check: connects to mbox3d the way the plug-in does (XPC + shared
 * memory) and shows what the daemon publishes.
 *
 *   m3_shm_check watch SECS        print the daemon's state once a second
 *   m3_shm_check tone SECS [FREQ [DBFS]]  also play a tone on channels 1/2 by writing
 *                                  into the playback ring like CoreAudio would
 */
#include "../driver/proto.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xpc/xpc.h>

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 3 || (strcmp(argv[1], "watch") && strcmp(argv[1], "tone"))) {
        fprintf(stderr, "usage: %s watch|tone SECS [FREQ [DBFS]]\n", argv[0]);
        return 2;
    }
    int tone = !strcmp(argv[1], "tone");
    unsigned secs = (unsigned)atoi(argv[2]);
    double freq = argc >= 4 ? atof(argv[3]) : 440.0;
    double amp = argc >= 5 ? 8388607.0 * pow(10.0, atof(argv[4]) / 20.0) : 0x180000;   /* DBFS, default about -14.5 */

    xpc_connection_t c = xpc_connection_create_mach_service(M3_SERVICE, NULL, 0);
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) { (void)ev; });
    xpc_connection_resume(c);
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "cmd", "hello");
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(c, msg);
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY) {
        fprintf(stderr, "no reply from %s (is mbox3d installed and running?)\n", M3_SERVICE);
        return 1;
    }
    void *addr = NULL;
    xpc_object_t shm = xpc_dictionary_get_value(reply, "shm");
    size_t len = shm ? xpc_shmem_map(shm, &addr) : 0;
    if (!addr || len < M3_SHM_SIZE) { fprintf(stderr, "no shared memory in the reply\n"); return 1; }
    m3_shm_header *h = addr;
    if (h->magic != M3_MAGIC || h->version != M3_VERSION) { fprintf(stderr, "bad magic/version\n"); return 1; }
    printf("connected: %zu bytes, %u ch, ring %u frames\n", len, h->channels, h->ring_frames);

    int32_t *cap = m3_cap_ring(h), *play = m3_play_ring(h);
    const uint64_t mask = M3_RING_FRAMES - 1;
    double w = 2.0 * M_PI * freq / (h->cur_rate ? h->cur_rate : M3_SAMPLE_RATE);
    uint64_t written = 0, last_cap = 0, seed = 0;
    int32_t peak[M3_CHANNELS] = {0};
    unsigned beat0 = h->heartbeat;

    for (unsigned t = 0; t < secs * 200; t++) {
        uint64_t cap_w = atomic_load(&h->cap_w);
        if (atomic_load(&h->seed) != seed) {            /* new timeline: start over */
            seed = atomic_load(&h->seed);
            written = 0;
        }
        if (atomic_load(&h->state) == M3_STATE_STREAMING) {
            if (tone) {
                /* keep 1024 frames of lead over "now" (the capture index), like CoreAudio does */
                uint64_t upto = cap_w + 1024;
                uint64_t taken = atomic_load(&h->play_pos);
                if (written < taken) written = taken;
                for (; written < upto; written++) {
                    int32_t v = (int32_t)(sin(w * (double)written) * amp);
                    int32_t *s = play + (written & mask) * M3_CHANNELS;
                    s[0] = v; s[1] = v;
                }
            }
            for (uint64_t i = last_cap; i < cap_w; i++) {
                if (cap_w - i > M3_RING_FRAMES) { i = cap_w - M3_RING_FRAMES; }
                const int32_t *s = cap + (i & mask) * M3_CHANNELS;
                for (unsigned ch = 0; ch < M3_CHANNELS; ch++) {
                    int32_t a = abs(s[ch]);
                    if (a > peak[ch]) peak[ch] = a;
                }
            }
            last_cap = cap_w;
        }
        if (t % 200 == 199) {
            printf("t=%2us state %u beat+%u cap %llu play_pos %llu locked %u  under %u over %u dbc %u late %u  peaks",
                   (t + 1) / 200, atomic_load(&h->state), atomic_load(&h->heartbeat) - beat0,
                   (unsigned long long)cap_w, (unsigned long long)atomic_load(&h->play_pos),
                   atomic_load(&h->clock_locked), atomic_load(&h->underruns), atomic_load(&h->overruns),
                   atomic_load(&h->dbc_errors), atomic_load(&h->late_slots));
            for (unsigned ch = 0; ch < M3_CHANNELS; ch++) printf(" %06x", peak[ch]);
            printf("  | playback peak %06x\n", atomic_load(&h->play_peak));
            printf("      plug-in: cycles %u  late writes %u fr  write margin min %d  late reads %u fr  read margin min %d\n",
                   atomic_load(&h->io_cycles), atomic_load(&h->late_write_frames),
                   atomic_load(&h->write_margin_min), atomic_load(&h->late_read_frames),
                   atomic_load(&h->read_margin_min));
            printf("      CoreAudio output: peak %06x (from the app), %u frames per cycle (largest %u), dropped as too far ahead %u fr\n",
                   atomic_exchange(&h->plug_out_peak, 0), atomic_load(&h->io_frames_last), atomic_load(&h->io_frames_max),
                   atomic_load(&h->ahead_write_frames));
            { uint64_t b = atomic_load(&h->tl_nsf_bits); double v; memcpy(&v, &b, 8);
              if (atomic_load(&h->tl_valid)) printf("      timeline: %.3f ns/frame = %.2f Hz (host clock vs device: %+.3f %%)\n", v, 1e9 / v, (1e9 / (double)(h->cur_rate ? h->cur_rate : 44100) / v - 1) * 100); }
            printf("      longest pause: library IO thread %.1f ms, daemon pump loop %.1f ms (since streaming started)\n", atomic_load(&h->max_gap_us) / 1000.0, atomic_load(&h->max_pump_gap_us) / 1000.0);
            unsigned cyc = atomic_load(&h->io_cycles);
            printf("      output: off-cursor cycles %u (largest %d fr, mean %.1f fr, >128 fr: %u), real jumps %u (largest %d fr); input jumps %u\n",
                   atomic_load(&h->io_wobble), atomic_load(&h->io_wobble_max),
                   cyc ? (double)atomic_load(&h->io_wobble_abs_sum) / cyc : 0.0, atomic_load(&h->io_wobble_gt128),
                   atomic_load(&h->io_jumps), atomic_load(&h->io_jump_max), atomic_load(&h->in_jumps));
            printf("      output stretched/moved to follow CoreAudio time: %u times, %lld frames in total\n",
                   atomic_load(&h->io_drift_fixes), (long long)atomic_load(&h->io_drift_frames));
            printf("      CoreAudio wake-up: longest gap between output cycles in the last second %.1f ms; gaps over 5 ms %u, over 10 ms %u, over 20 ms %u\n",
                   atomic_exchange(&h->wake_gap_max_us, 0) / 1000.0, atomic_load(&h->wake_over5), atomic_load(&h->wake_over10), atomic_load(&h->wake_over20));
            atomic_store(&h->write_margin_min, INT32_MAX);
            atomic_store(&h->read_margin_min, INT32_MAX);
            memset(peak, 0, sizeof(peak));
        }
        sleep_ms(5);
    }
    return 0;
}
