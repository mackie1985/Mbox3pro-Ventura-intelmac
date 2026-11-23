/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "ctlserver.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define UI_FILE_1 "/usr/local/share/mbox3pro/ui/index.html"
#define UI_FILE_2 "ui/index.html"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static mbox3 *g_m;
static unsigned g_rate;
static unsigned short g_port;
static uint16_t g_mix[MBOX3_MIXER_INPUTS * MBOX3_MIXER_OUTPUTS];
static uint32_t g_sat;
static int g_mix_ok;
static int g_dirty;   /* the mixer or reverb changed since the last save */
static int g_restore_wait;   /* seconds until the saved state is restored (0 = nothing pending) */

static void restore_last(void);
static int in_range(long v, long lo, long hi);

void ctl_server_set_device(mbox3 *m, unsigned rate)
{
    pthread_mutex_lock(&g_lock);
    g_m = m; g_rate = rate; g_mix_ok = 0;
    if (m && mbox3_mixer_read_all(m, g_mix, &g_sat) == MBOX3_OK) { g_mix_ok = 1; g_restore_wait = 2; }   /* restored later, off the pump thread */
    else g_restore_wait = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ----------------------------------------------------------------- output */

typedef struct { char *p; size_t n, cap; } sbuf;

static void sb_add(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_add(sbuf *b, const char *fmt, ...)
{
    for (;;) {
        va_list ap; va_start(ap, fmt);
        int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if ((size_t)w < b->cap - b->n) { b->n += (size_t)w; return; }
        b->cap = b->cap * 2 + (size_t)w + 64;
        b->p = realloc(b->p, b->cap);
        if (!b->p) return;
    }
}

static void send_all(int fd, const void *d, size_t n)
{
    const char *p = d;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
        p += w; n -= (size_t)w;
    }
}

static void reply(int fd, int code, const char *type, const void *body, size_t len)
{
    char h[256];
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" :
                      code == 404 ? "Not Found" : code == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
                     code, msg, type, len);
    send_all(fd, h, (size_t)n);
    if (len) send_all(fd, body, len);
}

static void reply_json(int fd, int code, sbuf *b) { reply(fd, code, "application/json", b->p, b->n); }
static void reply_text(int fd, int code, const char *t) { reply(fd, code, "text/plain; charset=utf-8", t, strlen(t)); }

/* ----------------------------------------------------------------- state */

static void json_params(sbuf *b, const mbox3_params *p)
{
    sb_add(b, "{\"usecase\":%d,\"knob\":%u,\"phantom\":%d,\"hpf\":[%d,%d,%d,%d],\"trim\":[%u,%u,%u,%u,%u,%u],"
              "\"rtype\":%d,\"rvol\":%u,\"rdur\":%u,\"rfb\":%u,\"mute\":%d,\"mono\":%d,\"spkr\":%d,\"dim\":%d}",
           p->standalone_use_case, p->master_knob_value, p->phantom_powering,
           p->hpf_enables[0], p->hpf_enables[1], p->hpf_enables[2], p->hpf_enables[3],
           p->output_trims[0], p->output_trims[1], p->output_trims[2], p->output_trims[3],
           p->output_trims[4], p->output_trims[5], p->reverb_type, p->reverb_volume,
           p->reverb_duration, p->reverb_feedback, p->mute_led, p->mono_led, p->spkr_led, p->dim_led);
}

static void api_state(int fd, int refresh)
{
    sbuf b = { malloc(4096), 0, 4096 };
    pthread_mutex_lock(&g_lock);
    if (!g_m) {
        sb_add(&b, "{\"online\":0}");
    } else {
        mbox3_stats st; mbox3_get_stats(g_m, &st);
        mbox3_params p;
        int pr = mbox3_params_get(g_m, &p);
        if (refresh || !g_mix_ok) g_mix_ok = mbox3_mixer_read_all(g_m, g_mix, &g_sat) == MBOX3_OK;
        sb_add(&b, "{\"online\":1,\"rate\":%u,\"locked\":%d,\"under\":%u,\"late\":%u,\"dbc\":%u,\"params\":",
               g_rate, st.clock_locked, st.playback_underruns, st.late_slots, st.capture_dbc_errors);
        if (pr == MBOX3_OK) json_params(&b, &p); else sb_add(&b, "null");
        sb_add(&b, ",\"sat\":%u,\"mixer\":", g_sat);
        if (g_mix_ok) {
            sb_add(&b, "[");
            for (unsigned i = 0; i < MBOX3_MIXER_INPUTS * MBOX3_MIXER_OUTPUTS; i++)
                sb_add(&b, "%s%u", i ? "," : "", g_mix[i]);
            sb_add(&b, "]");
        } else sb_add(&b, "null");
        sb_add(&b, "}");
    }
    pthread_mutex_unlock(&g_lock);
    reply_json(fd, 200, &b);
    free(b.p);
}

/* [[route, level], ...] for every router entry that has a route */
static void api_meters(int fd)
{
    sbuf b = { malloc(4096), 0, 4096 };
    uint32_t e[128];
    int n = 0;
    pthread_mutex_lock(&g_lock);
    if (g_m) n = mbox3_read_peaks(g_m, e, 128);
    pthread_mutex_unlock(&g_lock);
    sb_add(&b, "[");
    int first = 1;
    for (int i = 0; i < n; i++) {
        uint32_t route = e[i] & 0xffff;
        if (!route) continue;
        sb_add(&b, "%s[%u,%u]", first ? "" : ",", route, e[i] >> 16);
        first = 0;
    }
    sb_add(&b, "]");
    reply_json(fd, 200, &b);
    free(b.p);
}

/* ------------------------------------------------------------- parameters */

static const char *form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = body; *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t n = strcspn(p, "&"), o = 0;
            for (size_t k = 0; k < n && o < cap - 1; k++) {      /* application/x-www-form-urlencoded */
                if (p[k] == '%' && k + 2 < n && isxdigit((unsigned char)p[k + 1]) && isxdigit((unsigned char)p[k + 2])) {
                    char hx[3] = { p[k + 1], p[k + 2], 0 };
                    out[o++] = (char)strtol(hx, NULL, 16); k += 2;
                } else out[o++] = p[k] == '+' ? ' ' : p[k];
            }
            out[o] = 0;
            return out;
        }
        p += strcspn(p, "&");
        if (*p == '&') p++;
    }
    return NULL;
}

static int in_range(long v, long lo, long hi) { return v >= lo && v <= hi; }

/* name=<phantom|hpf|trim|rtype|rvol|rdur|rfb|usecase>&i=<index>&v=<value> */
static void api_param(int fd, const char *body)
{
    char name[24], si[16], sv[16];
    if (!form_get(body, "name", name, sizeof name) || !form_get(body, "v", sv, sizeof sv)) { reply_text(fd, 400, "name and v are required"); return; }
    long v = strtol(sv, NULL, 10), i = form_get(body, "i", si, sizeof si) ? strtol(si, NULL, 10) : 0;
    pthread_mutex_lock(&g_lock);
    if (!g_m) { pthread_mutex_unlock(&g_lock); reply_text(fd, 503, "device is not streaming"); return; }
    mbox3_params p;
    int rc = mbox3_params_get(g_m, &p), bad = 0;
    if (rc == MBOX3_OK) {
        if (!strcmp(name, "phantom") && in_range(v, 0, 1)) p.phantom_powering = v != 0;
        else if (!strcmp(name, "hpf") && in_range(i, 0, 3) && in_range(v, 0, 1)) p.hpf_enables[i] = v != 0;
        else if (!strcmp(name, "trim") && in_range(i, 0, 5) && in_range(v, 0, 255)) p.output_trims[i] = (uint8_t)v;
        else if (!strcmp(name, "rvol") && in_range(v, 0, 255)) p.reverb_volume = (uint8_t)v;
        else if (!strcmp(name, "rdur") && in_range(v, 0, 255)) p.reverb_duration = (uint8_t)v;
        else if (!strcmp(name, "rfb") && in_range(v, 0, 255)) p.reverb_feedback = (uint8_t)v;
        else if (!strcmp(name, "rtype") && (v == 1 || v == 4 || v == 5 || v == 6 || v == 8 || v == 11 || v == 19 || v == 20))
            p.reverb_type = (mbox3_reverb_type)v;
        else bad = 1;
        if (!bad) { rc = mbox3_params_set(g_m, &p); if (rc == MBOX3_OK) g_dirty = 1; }
    }
    pthread_mutex_unlock(&g_lock);
    if (bad) { reply_text(fd, 400, "bad name or value"); return; }
    if (rc != MBOX3_OK) { reply_text(fd, 503, "device I/O failed"); return; }
    reply_text(fd, 200, "ok");
}

/* c=out,in,value;out,in,value;... */
static void api_mixer(int fd, const char *body)
{
    char list[2048];
    if (!form_get(body, "c", list, sizeof list)) { reply_text(fd, 400, "c is required"); return; }
    pthread_mutex_lock(&g_lock);
    if (!g_m) { pthread_mutex_unlock(&g_lock); reply_text(fd, 503, "device is not streaming"); return; }
    int rc = MBOX3_OK;
    char *save = NULL;
    for (char *tok = strtok_r(list, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        unsigned o, in, v;
        if (sscanf(tok, "%u,%u,%u", &o, &in, &v) != 3 || o >= MBOX3_MIXER_OUTPUTS || in >= MBOX3_MIXER_INPUTS || v > 0x7fff) {
            rc = MBOX3_ERR_ARG; break;
        }
        rc = mbox3_mixer_set(g_m, o, in, v);
        if (rc != MBOX3_OK) break;
        g_mix[o * MBOX3_MIXER_INPUTS + in] = (uint16_t)v;
        g_dirty = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (rc == MBOX3_ERR_ARG) { reply_text(fd, 400, "bad cell"); return; }
    if (rc != MBOX3_OK) { reply_text(fd, 503, "device I/O failed"); return; }
    reply_text(fd, 200, "ok");
}

/* ------------------------------------------------------- last-state memory */

/* The mixer and the reverb are written to LAST_FILE shortly after every change and applied again each time
 * streaming starts (the library resets some cells then). The 48 V phantom power is never stored or restored. */
#define LAST_FILE "/var/db/mbox3d-last.state"
#define MIX_CELLS (MBOX3_MIXER_INPUTS * MBOX3_MIXER_OUTPUTS)


static int reverb_ok(long v) { return v == 1 || v == 4 || v == 5 || v == 6 || v == 8 || v == 11 || v == 19 || v == 20; }

/* g_lock held */
static void restore_last(void)
{
    FILE *f = fopen(LAST_FILE, "r");
    if (!f) return;
    uint16_t mix[MIX_CELLS];
    long rtype = 0, rvol = 0, rdur = 0, rfb = 0;
    int bad = 0, have_mix = 0, have_rt = 0, have_rv = 0, have_rd = 0, have_rf = 0;
    static char line[MIX_CELLS * 7 + 64];
    if (!fgets(line, sizeof line, f) || strncmp(line, "mbox3-last 1", 12)) bad = 1;
    while (!bad && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "mixer", 5)) {
            char *p = line + 5;
            unsigned n = 0;
            for (; n < MIX_CELLS; n++) {
                char *e;
                long v = strtol(p, &e, 10);
                if (e == p || v < 0 || v > 0x7fff) { bad = 1; break; }
                mix[n] = (uint16_t)v; p = e;
            }
            if (n != MIX_CELLS) bad = 1; else have_mix = 1;
        }
        else if (!strncmp(line, "rtype ", 6)) { have_rt = sscanf(line + 6, "%ld", &rtype) == 1; if (!reverb_ok(rtype)) bad = 1; }
        else if (!strncmp(line, "rvol ", 5)) { have_rv = sscanf(line + 5, "%ld", &rvol) == 1; if (!in_range(rvol, 0, 255)) bad = 1; }
        else if (!strncmp(line, "rdur ", 5)) { have_rd = sscanf(line + 5, "%ld", &rdur) == 1; if (!in_range(rdur, 0, 255)) bad = 1; }
        else if (!strncmp(line, "rfb ", 4)) { have_rf = sscanf(line + 4, "%ld", &rfb) == 1; if (!in_range(rfb, 0, 255)) bad = 1; }
    }
    fclose(f);
    if (bad || !have_mix || !have_rt || !have_rv || !have_rd || !have_rf) {
        fprintf(stderr, "mbox3d: the saved mixer state is damaged; ignored\n");
        return;
    }
    uint16_t cur[MIX_CELLS]; uint32_t sat;
    int rc = mbox3_mixer_read_all(g_m, cur, &sat);
    unsigned changed = 0;
    for (unsigned i = 0; rc == MBOX3_OK && i < MIX_CELLS; i++) {
        if (cur[i] == mix[i]) continue;
        rc = mbox3_mixer_set(g_m, i / MBOX3_MIXER_INPUTS, i % MBOX3_MIXER_INPUTS, mix[i]);
        if (rc == MBOX3_OK) { cur[i] = mix[i]; changed++; }
    }
    memcpy(g_mix, cur, sizeof cur); g_sat = sat; g_mix_ok = rc == MBOX3_OK;
    if (rc == MBOX3_OK) {
        mbox3_params p;
        rc = mbox3_params_get(g_m, &p);
        if (rc == MBOX3_OK) {
            p.reverb_type = (mbox3_reverb_type)rtype;
            p.reverb_volume = (uint8_t)rvol; p.reverb_duration = (uint8_t)rdur; p.reverb_feedback = (uint8_t)rfb;
            rc = mbox3_params_set(g_m, &p);
        }
    }
    if (rc == MBOX3_OK) fprintf(stderr, "mbox3d: restored the last mixer state (%u cells changed)\n", changed);
    else fprintf(stderr, "mbox3d: restoring the last mixer state failed (%d)\n", rc);
}

static void save_last(void)
{
    uint16_t mix[MIX_CELLS];
    mbox3_params p;
    pthread_mutex_lock(&g_lock);
    if (!g_dirty || !g_m || !g_mix_ok || mbox3_params_get(g_m, &p) != MBOX3_OK) { pthread_mutex_unlock(&g_lock); return; }
    memcpy(mix, g_mix, sizeof mix);
    g_dirty = 0;
    pthread_mutex_unlock(&g_lock);
    char tmp[] = LAST_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) goto fail;
    fprintf(f, "mbox3-last 1\nmixer");
    for (unsigned i = 0; i < MIX_CELLS; i++) fprintf(f, " %u", mix[i]);
    fprintf(f, "\nrtype %d\nrvol %u\nrdur %u\nrfb %u\n", (int)p.reverb_type, p.reverb_volume, p.reverb_duration, p.reverb_feedback);
    int bad = fflush(f) != 0 || ferror(f);
    bad |= fclose(f) != 0;
    if (!bad && rename(tmp, LAST_FILE) == 0) return;
    unlink(tmp);
fail:
    pthread_mutex_lock(&g_lock); g_dirty = 1; pthread_mutex_unlock(&g_lock);
}

static void *saver_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        /* The saved state is restored a moment after streaming has started and from this thread: register writes in
           the pump thread before the first service call stall the stream start and leave the playback timeline misaligned. */
        pthread_mutex_lock(&g_lock);
        if (g_restore_wait && --g_restore_wait == 0 && g_m && g_mix_ok) restore_last();
        pthread_mutex_unlock(&g_lock);
        save_last();
    }
    return NULL;
}


/* ------------------------------------------------------------------ http */

static void serve_ui(int fd)
{
    FILE *f = fopen(UI_FILE_1, "rb");
    if (!f) f = fopen(UI_FILE_2, "rb");
    if (!f) { reply_text(fd, 404, "ui/index.html is not installed (scripts/install.sh daemon)"); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *d = malloc((size_t)n + 1);
    size_t got = d ? fread(d, 1, (size_t)n, f) : 0;
    fclose(f);
    reply(fd, 200, "text/html; charset=utf-8", d, got);
    free(d);
}

static const char *header_value(char *req, const char *name)
{
    size_t nl = strlen(name);
    for (char *l = strstr(req, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        if (!strncasecmp(l + 2, name, nl) && l[2 + nl] == ':') {
            const char *v = l + 3 + nl;
            while (*v == ' ') v++;
            return v;
        }
    }
    return NULL;
}

static void handle(int fd)
{
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[8192];
    size_t have = 0;
    char *hend = NULL;
    while (have < sizeof req - 1) {
        ssize_t r = recv(fd, req + have, sizeof req - 1 - have, 0);
        if (r <= 0) return;
        have += (size_t)r; req[have] = 0;
        if ((hend = strstr(req, "\r\n\r\n"))) break;
    }
    if (!hend) { reply_text(fd, 400, "bad request"); return; }
    size_t hlen = (size_t)(hend - req) + 4;
    const char *cl = header_value(req, "Content-Length");
    size_t clen = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (clen > 4000 || hlen + clen > sizeof req - 1) { reply_text(fd, 400, "body too large"); return; }
    while (have < hlen + clen) {
        ssize_t r = recv(fd, req + have, sizeof req - 1 - have, 0);
        if (r <= 0) return;
        have += (size_t)r;
    }
    req[hlen + clen] = 0;
    char *body = req + hlen;

    /* DNS rebinding: only our own host names */
    const char *host = header_value(req, "Host");
    char want1[48], want2[48];
    snprintf(want1, sizeof want1, "127.0.0.1:%u", g_port);
    snprintf(want2, sizeof want2, "localhost:%u", g_port);
    if (!host || (strncmp(host, want1, strlen(want1)) && strncmp(host, want2, strlen(want2)))) { reply_text(fd, 403, "forbidden host"); return; }

    char method[8], path[128];
    if (sscanf(req, "%7s %127s", method, path) != 2) { reply_text(fd, 400, "bad request"); return; }
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/") || !strcmp(path, "/index.html")) serve_ui(fd);
        else if (!strcmp(path, "/api/state")) api_state(fd, 0);
        else if (!strcmp(path, "/api/state?refresh=1")) api_state(fd, 1);
        else if (!strcmp(path, "/api/meters")) api_meters(fd);
        else reply_text(fd, 404, "not found");
    } else if (!strcmp(method, "POST")) {
        /* a custom header cannot be sent by another web page without a CORS preflight, which is never granted */
        if (!header_value(req, "X-Mbox3")) { reply_text(fd, 403, "missing X-Mbox3 header"); return; }
        if (!strcmp(path, "/api/param")) api_param(fd, body);
        else if (!strcmp(path, "/api/mixer")) api_mixer(fd, body);
        else reply_text(fd, 404, "not found");
    } else reply_text(fd, 400, "bad method");
}

static void *server_thread(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; sleep(1); continue; }
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        handle(fd);
        close(fd);
    }
    return NULL;
}

int ctl_server_start(unsigned short port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_len = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) { close(ls); return -1; }
    g_port = port;
    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, (void *)(intptr_t)ls) != 0) { close(ls); return -1; }
    pthread_detach(th);
    pthread_t sv;
    if (pthread_create(&sv, NULL, saver_thread, NULL) == 0) pthread_detach(sv);
    return 0;
}
