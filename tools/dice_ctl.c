/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * dice_ctl: command-line control for the Avid Mbox 3 Pro over FireWire.
 *
 *   dice_ctl show
 *   dice_ctl mixer get
 *   dice_ctl mixer set <out 0-15> <in 0-17> <value 0-0x7fff>
 *   dice_ctl set phantom on|off          (48 V! check connected gear first)
 *   dice_ctl set hpf <1-4> on|off
 *   dice_ctl set reverb-volume <0-255>
 *   dice_ctl set trim <1-6> <0-255>      (255 = wire value 0)
 *   dice_ctl owner show|claim|release    (host ownership of the device)
 *   dice_ctl router show
 *   dice_ctl router apply-config low|mid|high   (copy stored router table, then load)
 *   dice_ctl router config low|mid|high         (show stored router table)
 *   dice_ctl router apply-default low|mid|high [--via-config]
 *   dice_ctl cmd load-router|load-stream|load-both low|mid|high
 *   dice_ctl hold [seconds]              (claim, watch the owner register, release)
 *   dice_ctl session "cmd args" "cmd args" ...   (claim, run commands in ONE process, release)
 *       extra session step: "sleep N"
 *
 * Mixer cell layout (out-major, 18 inputs per output) and the 0x4000 = unity
 * guess are inferred from device dumps and still need on-hardware validation.
 */
#include "dice_io.h"
#include "dice_iso.h"
#include "../src/mbox3_params.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIXER_IN 18
#define MIXER_OUT 16

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  dice_ctl show\n"
        "  dice_ctl mixer get\n"
        "  dice_ctl mixer set <out 0-15> <in 0-17> <value 0-0x7fff>\n"
        "  dice_ctl set phantom on|off\n"
        "  dice_ctl set hpf <1-4> on|off\n"
        "  dice_ctl set reverb-volume <0-255>\n"
        "  dice_ctl set trim <1-6> <0-255>\n"
        "  dice_ctl owner show|claim|release [--force] [--host]\n"
        "  dice_ctl router show\n"
        "  dice_ctl router apply-config low|mid|high [--force]\n"
        "  dice_ctl router config low|mid|high\n"
        "  dice_ctl router apply-default low|mid|high [--via-config] [--force]\n"
        "  dice_ctl cmd load-router|load-stream|load-both low|mid|high [--force]\n"
        "  dice_ctl hold [seconds]\n"
        "  dice_ctl session \"cmd args\" \"cmd args\" ...   (e.g. \"router apply-config low\" \"sleep 20\")\n");
    return 2;
}

static int parse_onoff(const char *s)
{
    if (!strcmp(s, "on")) return 1;
    if (!strcmp(s, "off")) return 0;
    return -1;
}

static void print_params(const mbox3_params *p)
{
    printf("use case        %d (0=mixer 1=ad/da 2=preamp 3=undef)\n", p->standalone_use_case);
    printf("master knob     %u\n", p->master_knob_value);
    printf("phantom power   %s\n", p->phantom_powering ? "on" : "off");
    printf("HPF             %d %d %d %d\n", p->hpf_enables[0], p->hpf_enables[1],
           p->hpf_enables[2], p->hpf_enables[3]);
    printf("output trims    %u %u %u %u %u %u\n", p->output_trims[0], p->output_trims[1],
           p->output_trims[2], p->output_trims[3], p->output_trims[4], p->output_trims[5]);
    printf("reverb          type=0x%02x vol=%u dur=%u fb=%u\n", p->reverb_type,
           p->reverb_volume, p->reverb_duration, p->reverb_feedback);
    printf("LEDs            mute=%d mono=%d spkr=%d dim=%d\n", p->mute_led, p->mono_led,
           p->spkr_led, p->dim_led);
}

static int read_app(uint32_t *base, uint8_t *raw, mbox3_params *p)
{
    uint32_t size;
    if (dice_ext_section(8, base, &size) != 0 || size < MBOX3_APP_SECTION_MIN_SIZE) {
        fprintf(stderr, "cannot locate application section\n");
        return -1;
    }
    if (dice_read(*base, raw, MBOX3_APP_SECTION_MIN_SIZE) != kIOReturnSuccess) {
        fprintf(stderr, "application section read failed\n");
        return -1;
    }
    if (mbox3_params_deserialize(p, raw, MBOX3_APP_SECTION_MIN_SIZE) != 0) {
        fprintf(stderr, "application section does not decode as Mbox 3 Pro\n");
        return -1;
    }
    return 0;
}

/* Write only the quadlets that changed, then read back and show the result. */
static int apply_params(uint32_t base, const uint8_t *old_raw, const mbox3_params *np)
{
    uint8_t nw[MBOX3_APP_SECTION_MIN_SIZE];
    memcpy(nw, old_raw, sizeof(nw));
    if (mbox3_params_serialize(np, nw, sizeof(nw)) != 0) {
        fprintf(stderr, "serialize failed\n");
        return -1;
    }
    int changed = 0;
    for (uint32_t q = 0; q < sizeof(nw); q += 4) {
        if (memcmp(nw + q, old_raw + q, 4) == 0) continue;
        uint32_t v = dice_be32(nw + q);
        IOReturn r = dice_write_q(base + q, v);
        printf("write +0x%02x <- 0x%08x : %s (0x%08x)\n", q, v,
               r == kIOReturnSuccess ? "ok" : "FAILED", r);
        if (r != kIOReturnSuccess) return -1;
        changed++;
    }
    if (!changed) printf("no change needed\n");

    uint8_t chk[MBOX3_APP_SECTION_MIN_SIZE];
    mbox3_params rp;
    if (dice_read(base, chk, sizeof(chk)) == kIOReturnSuccess &&
        mbox3_params_deserialize(&rp, chk, sizeof(chk)) == 0) {
        printf("--- now ---\n");
        print_params(&rp);
    }
    return 0;
}


/* DICE owner register (global section +0x00, 8 bytes): upper 16 bits = node
 * ID of the owner (0xffff = nobody), lower 48 bits = address the device sends
 * notifications to. We have no notification receiver yet, so claim with a
 * placeholder address: the device's notification writes will simply be
 * rejected by this Mac. */
#define OWNER_NONE 0xffff000000000000ull
#define NOTIFY_PLACEHOLDER 0x000100000000ull

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)dice_be32(p) << 32) | dice_be32(p + 4);
}

static int cmd_owner(int argc, char **argv)
{
    uint8_t t[8], o[8];
    if (argc < 3) return usage();
    if (dice_read(0, t, 8) != kIOReturnSuccess) { fprintf(stderr, "section table read failed\n"); return 1; }
    uint32_t goff = dice_be32(t) * 4;
    if (dice_read(goff, o, 8) != kIOReturnSuccess) { fprintf(stderr, "owner read failed\n"); return 1; }
    uint64_t cur = be64(o);
    uint16_t me = 0;
    int have_me = dice_local_node_id(&me) == 0;
    printf("owner = 0x%016llx (%s)\n", (unsigned long long)cur,
           cur == OWNER_NONE ? "nobody" : "owned");
    if (have_me) printf("this Mac's node id = 0x%04x\n", me);

    /* IOFireWireLib takes lock values as raw wire-order bytes (seen on hardware:
     * the 'old' value came back byte-swapped). --host passes host-order numbers. */
    int force = 0, raw = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--host")) raw = 0;
    }
    uint64_t want;
    if (!strcmp(argv[2], "show")) {
        return 0;
    } else if (!strcmp(argv[2], "claim")) {
        if (!have_me) { fprintf(stderr, "cannot determine this Mac's node id\n"); return 1; }
        if (cur != OWNER_NONE && !force) { fprintf(stderr, "already owned; use --force to override\n"); return 1; }
        want = ((uint64_t)me << 48) | NOTIFY_PLACEHOLDER;
    } else if (!strcmp(argv[2], "release")) {
        if (cur == OWNER_NONE) { printf("nothing to release\n"); return 0; }
        if (!(have_me && (cur >> 48) == me) && !force) {
            fprintf(stderr, "owned by another node; use --force to release anyway\n");
            return 1;
        }
        want = OWNER_NONE;
    } else {
        return usage();
    }

    /* Lock (compare-and-swap) transaction: succeeds only if the register still
     * holds the value we just read. */
    uint64_t old = 0;
    IOReturn r = dice_lock64(goff, cur, want, &old, raw);
    printf("lock owner 0x%016llx -> 0x%016llx : %s (0x%08x)\n",
           (unsigned long long)cur, (unsigned long long)want,
           r == kIOReturnSuccess ? "transaction ok" : "FAILED", r);
    printf("value found at target: 0x%016llx (%s)\n", (unsigned long long)old,
           old == cur ? "matched, swap done" : "MISMATCH, swap not done");
    if (dice_read(goff, o, 8) == kIOReturnSuccess)
        printf("owner now = 0x%016llx\n", (unsigned long long)be64(o));
    return (r == kIOReturnSuccess && old == cur) ? 0 : 1;
}

/* ---- router section and command section (TCAT extension) ----
 * Command register: +0x00 opcode, +0x04 return value. opcode =
 * EXECUTE | rate flag | op. Flash opcodes (load/store) are deliberately not
 * implemented. */
#define EXECUTE_FLAG 0x80000000u
#define OP_LOAD_ROUTER 1u
#define OP_LOAD_STREAM 2u
#define OP_LOAD_BOTH 3u

static const char *src_blk(unsigned id)
{
    switch (id) {
    case 0x0: return "AES"; case 0x1: return "ADAT"; case 0x2: return "Mixer";
    case 0x4: return "Ins0"; case 0x5: return "Ins1"; case 0x6: return "ARM";
    case 0xb: return "Avs0"; case 0xc: return "Avs1"; case 0xf: return "Mute";
    }
    return "?";
}
static const char *dst_blk(unsigned id)
{
    switch (id) {
    case 0x0: return "AES"; case 0x1: return "ADAT"; case 0x2: return "MixerTx0";
    case 0x3: return "MixerTx1"; case 0x4: return "Ins0"; case 0x5: return "Ins1";
    case 0x6: return "ARM"; case 0xb: return "Avs0"; case 0xc: return "Avs1";
    }
    return "?";
}
static void print_route(uint32_t e)
{
    unsigned src = (e >> 8) & 0xff, dst = e & 0xff;
    printf("%s:%u -> %s:%u", src_blk(src >> 4), src & 0xf, dst_blk(dst >> 4), dst & 0xf);
}

static int parse_rate(const char *s, uint32_t *flag, uint32_t *cc_base)
{
    if (!strcmp(s, "low"))  { *flag = 0x00010000u; *cc_base = 0x0000; return 0; }
    if (!strcmp(s, "mid"))  { *flag = 0x00020000u; *cc_base = 0x2000; return 0; }
    if (!strcmp(s, "high")) { *flag = 0x00040000u; *cc_base = 0x4000; return 0; }
    return -1;
}

static int host_is_owner(void)
{
    uint8_t t[8], o[8];
    uint16_t me;
    if (dice_read(0, t, 8) != kIOReturnSuccess) return 0;
    if (dice_read(dice_be32(t) * 4, o, 8) != kIOReturnSuccess) return 0;
    if (dice_local_node_id(&me) != 0) return 0;
    return (be64(o) >> 48) == me;
}

static int has_force(int argc, char **argv)
{
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--force")) return 1;
    return 0;
}

static int run_cmd(uint32_t op, uint32_t rate_flag, uint32_t *ret)
{
    uint32_t off, size;
    uint8_t b[4];
    if (dice_ext_section(1, &off, &size) != 0) { fprintf(stderr, "no command section\n"); return -1; }
    uint32_t code = EXECUTE_FLAG | rate_flag | op;
    IOReturn r = dice_write_q(off, code);
    printf("command 0x%08x written: %s (0x%08x)\n", code, r == kIOReturnSuccess ? "ok" : "FAILED", r);
    if (r != kIOReturnSuccess) return -1;
    for (int i = 0; i < 40; i++) {
        usleep(50000);
        if (dice_read(off, b, 4) != kIOReturnSuccess) continue;
        if (!(dice_be32(b) & EXECUTE_FLAG)) {
            dice_read(off + 4, b, 4);
            *ret = dice_be32(b);
            printf("command finished after ~%d ms, return value 0x%08x\n", (i + 1) * 50, *ret);
            return 0;
        }
    }
    fprintf(stderr, "command did not finish within 2 s\n");
    return -1;
}

static int show_router(void)
{
    uint32_t off, size;
    static uint8_t buf[0x300];
    if (dice_ext_section(4, &off, &size) != 0 || size < 4) return -1;
    if (size > sizeof(buf)) size = sizeof(buf);
    if (dice_read(off, buf, size) != kIOReturnSuccess) { fprintf(stderr, "router read failed\n"); return -1; }
    uint32_t cnt = dice_be32(buf);
    printf("router section: %u entries\n", cnt);
    for (uint32_t i = 0; i < cnt && 4 + i * 4 + 4 <= size; i++) {
        uint32_t e = dice_be32(buf + 4 + i * 4);
        printf("  [%3u] %08x  ", i, e); print_route(e); printf("\n");
    }
    return 0;
}

/* The 40-entry router table the device itself held in standalone mode,
 * captured from hardware before taking ownership (identical for all rates). */
static const uint32_t DEFAULT_ROUTES[40] = {
    0x000040b0, 0x000041b1, 0x000042b2, 0x000043b3, 0x000044b4, 0x000045b5, 0x000000b6, 0x000001b7,
    0x0000b020, 0x0000b121, 0x0000b222, 0x0000b323, 0x0000b424, 0x0000b525, 0x0000b626, 0x0000b727,
    0x00004028, 0x00004129, 0x0000422a, 0x0000432b, 0x0000442c, 0x0000452d, 0x0000002e, 0x0000012f,
    0x00005030, 0x00005131,
    0x00002040, 0x00002141, 0x00002242, 0x00002343, 0x00002444, 0x00002545, 0x00002600, 0x00002701,
    0x00002850, 0x00002951, 0x00002a52, 0x00002b53,
    0x0000b054, 0x0000b155,
};

static int has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], flag)) return 1;
    return 0;
}

static int cmd_router(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "show")) return show_router() == 0 ? 0 : 1;

    if (argc >= 4 && !strcmp(argv[2], "config")) {
        uint32_t flag, ccb, cc_off, cc_size;
        static uint8_t buf[0x300];
        if (parse_rate(argv[3], &flag, &ccb) != 0) return usage();
        if (dice_ext_section(6, &cc_off, &cc_size) != 0 ||
            dice_read(cc_off + ccb, buf, sizeof(buf)) != kIOReturnSuccess) {
            fprintf(stderr, "current_config read failed\n");
            return 1;
        }
        uint32_t cnt = dice_be32(buf);
        printf("current_config router table (%s): %u entries\n", argv[3], cnt);
        for (uint32_t i = 0; i < cnt && i < 128 && 4 + i * 4 + 4 <= sizeof(buf); i++) {
            uint32_t e = dice_be32(buf + 4 + i * 4);
            printf("  [%3u] %08x  ", i, e); print_route(e); printf("\n");
        }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[2], "apply")) {
        /* router apply RATE HEX...: write the given (src<<8|dst) entries. */
        uint32_t flag, ccb, r_off, r_size;
        if (parse_rate(argv[3], &flag, &ccb) != 0) return usage();
        if (!host_is_owner() && !has_force(argc, argv)) { fprintf(stderr, "not the owner\n"); return 1; }
        if (dice_ext_section(4, &r_off, &r_size) != 0) return 1;
        uint32_t n = 0;
        for (int i = 4; i < argc; i++) {
            if (!strncmp(argv[i], "--", 2)) continue;
            uint32_t e = (uint32_t)strtoul(argv[i], NULL, 16);
            if (4 + (n + 1) * 4 > r_size) { fprintf(stderr, "too many entries\n"); return 1; }
            IOReturn r = dice_write_q(r_off + 4 + n * 4, e);
            if (r != kIOReturnSuccess) { fprintf(stderr, "write entry failed: 0x%08x\n", r); return 1; }
            n++;
        }
        IOReturn r = dice_write_q(r_off, n);
        if (r != kIOReturnSuccess) { fprintf(stderr, "write count failed: 0x%08x\n", r); return 1; }
        uint32_t ret;
        if (run_cmd(OP_LOAD_ROUTER, flag, &ret) != 0) return 1;
        return show_router() == 0 ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[2], "apply-standalone")) {
        /* The 22 routes the firmware itself uses in standalone mode, read back
         * from the peak-meter section (analog in 1-6 -> mixer in 0-5, S/PDIF in
         * -> mixer in 6-7, mixer out 0-5 -> analog out, 6-7 -> S/PDIF, out 0/1
         * -> headphones). */
        static const uint32_t SA[22] = {
            0x4020, 0x4121, 0x4222, 0x4323, 0x4424, 0x4525, 0x0026, 0x0127,
            0x2040, 0x2141, 0x2242, 0x2343, 0x2444, 0x2545, 0x2600, 0x2701,
            0x2050, 0x2151, 0x2052, 0x2153, 0x2054, 0x2155,
        };
        uint32_t flag, ccb, r_off, r_size;
        if (parse_rate(argv[3], &flag, &ccb) != 0) return usage();
        if (!host_is_owner() && !has_force(argc, argv)) {
            fprintf(stderr, "this Mac is not the owner; run 'owner claim' first (or use a session)\n");
            return 1;
        }
        if (dice_ext_section(4, &r_off, &r_size) != 0) return 1;
        printf("writing 22 standalone router entries into the router section\n");
        for (uint32_t i = 0; i < 22; i++) {
            IOReturn r = dice_write_q(r_off + 4 + i * 4, SA[i]);
            if (r != kIOReturnSuccess) { fprintf(stderr, "write entry %u failed: 0x%08x\n", i, r); return 1; }
        }
        IOReturn r = dice_write_q(r_off, 22);
        if (r != kIOReturnSuccess) { fprintf(stderr, "write count failed: 0x%08x\n", r); return 1; }
        uint32_t ret;
        if (run_cmd(OP_LOAD_ROUTER, flag, &ret) != 0) return 1;
        return show_router() == 0 ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[2], "apply-default")) {
        uint32_t flag, ccb, cc_off, cc_size, r_off, r_size;
        if (parse_rate(argv[3], &flag, &ccb) != 0) return usage();
        if (!host_is_owner() && !has_force(argc, argv)) {
            fprintf(stderr, "this Mac is not the owner; run 'owner claim' first (or use a session)\n");
            return 1;
        }
        if (dice_ext_section(6, &cc_off, &cc_size) != 0 || dice_ext_section(4, &r_off, &r_size) != 0) return 1;
        int via_config = has_flag(argc, argv, "--via-config");
        uint32_t base = via_config ? cc_off + ccb : r_off;
        printf("writing %d default router entries into the %s\n", 40,
               via_config ? "current_config area" : "router section");
        for (uint32_t i = 0; i < 40; i++) {
            IOReturn r = dice_write_q(base + 4 + i * 4, DEFAULT_ROUTES[i]);
            if (r != kIOReturnSuccess) { fprintf(stderr, "write entry %u failed: 0x%08x\n", i, r); return 1; }
        }
        IOReturn r = dice_write_q(base, 40);
        if (r != kIOReturnSuccess) { fprintf(stderr, "write count failed: 0x%08x\n", r); return 1; }
        uint32_t ret;
        if (run_cmd(OP_LOAD_ROUTER, flag, &ret) != 0) return 1;
        return show_router() == 0 ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[2], "apply-config")) {
        uint32_t flag, ccb;
        if (parse_rate(argv[3], &flag, &ccb) != 0) return usage();
        if (!host_is_owner() && !has_force(argc, argv)) {
            fprintf(stderr, "this Mac is not the owner; run 'owner claim' first (or --force)\n");
            return 1;
        }
        uint32_t cc_off, cc_size, r_off, r_size;
        if (dice_ext_section(6, &cc_off, &cc_size) != 0 || dice_ext_section(4, &r_off, &r_size) != 0) return 1;
        static uint8_t buf[0x300];
        if (dice_read(cc_off + ccb, buf, sizeof(buf)) != kIOReturnSuccess) { fprintf(stderr, "current_config read failed\n"); return 1; }
        uint32_t cnt = dice_be32(buf);
        if (cnt == 0 || cnt > 128 || 4 + cnt * 4 > r_size) { fprintf(stderr, "implausible router table (%u entries)\n", cnt); return 1; }
        printf("copying %u router entries from current_config (%s) to the router section\n", cnt, argv[3]);
        for (uint32_t i = 0; i < cnt; i++) {
            IOReturn r = dice_write_q(r_off + 4 + i * 4, dice_be32(buf + 4 + i * 4));
            if (r != kIOReturnSuccess) { fprintf(stderr, "write entry %u failed: 0x%08x\n", i, r); return 1; }
        }
        IOReturn r = dice_write_q(r_off, cnt);
        if (r != kIOReturnSuccess) { fprintf(stderr, "write count failed: 0x%08x\n", r); return 1; }
        uint32_t ret;
        if (run_cmd(OP_LOAD_ROUTER, flag, &ret) != 0) return 1;
        return show_router() == 0 ? 0 : 1;
    }
    return usage();
}

static int cmd_cmd(int argc, char **argv)
{
    if (argc < 4) return usage();
    uint32_t op, flag, ccb;
    if (!strcmp(argv[2], "load-router")) op = OP_LOAD_ROUTER;
    else if (!strcmp(argv[2], "load-stream")) op = OP_LOAD_STREAM;
    else if (!strcmp(argv[2], "load-both")) op = OP_LOAD_BOTH;
    else return usage();
    if (parse_rate(argv[3], &flag, &ccb) != 0) return usage();
    if (!host_is_owner() && !has_force(argc, argv)) {
        fprintf(stderr, "this Mac is not the owner; run 'owner claim' first (or --force)\n");
        return 1;
    }
    uint32_t ret;
    if (run_cmd(op, flag, &ret) != 0) return 1;
    return show_router() == 0 ? 0 : 1;
}

/* ---- single-process ownership helpers (hold / session) ---- */
static int read_owner(uint32_t *goff, uint64_t *cur)
{
    uint8_t t[8], o[8];
    if (dice_read(0, t, 8) != kIOReturnSuccess) return -1;
    *goff = dice_be32(t) * 4;
    if (dice_read(*goff, o, 8) != kIOReturnSuccess) return -1;
    *cur = be64(o);
    return 0;
}

static int claim_owner(void)
{
    uint32_t goff;
    uint64_t cur, old = 0;
    uint16_t me;
    if (read_owner(&goff, &cur) != 0) { fprintf(stderr, "owner read failed\n"); return 1; }
    if (dice_local_node_id(&me) != 0) { fprintf(stderr, "cannot determine this Mac's node id\n"); return 1; }
    if (cur != OWNER_NONE) {
        if ((cur >> 48) == me) { printf("[owner] already ours\n"); return 0; }
        fprintf(stderr, "[owner] owned by another node (0x%016llx)\n", (unsigned long long)cur);
        return 1;
    }
    uint64_t want = ((uint64_t)me << 48) | NOTIFY_PLACEHOLDER;
    IOReturn r = dice_lock64(goff, cur, want, &old, 1);
    if (r != kIOReturnSuccess || old != cur) {
        fprintf(stderr, "[owner] claim failed (0x%08x, found 0x%016llx)\n", r, (unsigned long long)old);
        return 1;
    }
    printf("[owner] claimed: 0x%016llx\n", (unsigned long long)want);
    return 0;
}

static void release_owner(void)
{
    uint32_t goff;
    uint64_t cur, old = 0;
    uint16_t me;
    if (read_owner(&goff, &cur) != 0 || dice_local_node_id(&me) != 0) return;
    if ((cur >> 48) != me) { printf("[owner] not ours any more (0x%016llx), nothing to release\n", (unsigned long long)cur); return; }
    IOReturn r = dice_lock64(goff, cur, OWNER_NONE, &old, 1);
    printf("[owner] release: %s\n", (r == kIOReturnSuccess && old == cur) ? "ok" : "FAILED");
}

static int cmd_hold(int argc, char **argv)
{
    int secs = argc >= 3 ? atoi(argv[2]) : 10;
    if (secs < 1 || secs > 120) secs = 10;
    if (claim_owner() != 0) return 1;
    for (int i = 1; i <= secs * 4; i++) {
        usleep(250000);
        uint32_t g;
        uint64_t c;
        if (read_owner(&g, &c) == 0)
            printf("t=%5d ms  owner=0x%016llx%s\n", i * 250, (unsigned long long)c,
                   c == OWNER_NONE ? "  (nobody)" : "");
    }
    release_owner();
    return 0;
}

static int dispatch(int argc, char **argv);

/* Stream configs stored by the device (current_config, one per rate mode),
 * snapshotted before the host takes ownership because they read as empty
 * afterwards. Block size 0x438 matches the stream_format section. */
#define STREAM_SNAP_SIZE 0x438
static uint8_t g_snap[3][STREAM_SNAP_SIZE];
static int g_snap_ok;

static void snapshot_streams(void)
{
    uint32_t cc_off, cc_size;
    g_snap_ok = 0;
    if (dice_ext_section(6, &cc_off, &cc_size) != 0) return;
    int ok = 1;
    for (int m = 0; m < 3; m++)
        if (dice_read(cc_off + 0x1000 + m * 0x2000, g_snap[m], STREAM_SNAP_SIZE) != kIOReturnSuccess) ok = 0;
    g_snap_ok = ok;
    if (ok)
        printf("[session] stream config snapshot: low tx/rx counts %u/%u\n",
               dice_be32(g_snap[0]), dice_be32(g_snap[0] + 4));
}

/* stream apply-snapshot low|mid|high: write the snapshot into the
 * stream_format section, then run LoadStreamConfig. */
static int cmd_stream(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "play"))
        return dice_iso_play(argc >= 4 ? (unsigned)atoi(argv[3]) : 5,
                             argc >= 5 ? (unsigned)atoi(argv[4]) : 6,
                             argc >= 6 ? (unsigned)atoi(argv[5]) : 440);
    if (argc >= 3 && !strcmp(argv[2], "duplex"))
        return dice_iso_duplex(argc >= 4 ? (unsigned)atoi(argv[3]) : 10,
                               argc >= 5 ? (unsigned)atoi(argv[4]) : 6,
                               argc >= 6 ? (unsigned)atoi(argv[5]) : 440,
                               argc >= 7 ? (int)strtoul(argv[6], NULL, 0) : -1);
    if (argc >= 3 && !strcmp(argv[2], "capture"))
        return dice_iso_capture(argc >= 4 ? (unsigned)atoi(argv[3]) : 3);
    if (argc >= 4 && !strcmp(argv[2], "apply-snapshot")) {
        uint32_t flag, ccb, sf_off, sf_size;
        if (parse_rate(argv[3], &flag, &ccb) != 0) return usage();
        if (!g_snap_ok) { fprintf(stderr, "no stream snapshot (only available inside a session)\n"); return 1; }
        if (!host_is_owner() && !has_force(argc, argv)) { fprintf(stderr, "not the owner\n"); return 1; }
        if (dice_ext_section(5, &sf_off, &sf_size) != 0 || sf_size < STREAM_SNAP_SIZE) return 1;
        const uint8_t *s = g_snap[ccb / 0x2000];
        unsigned n = 0;
        for (uint32_t i = 0; i < STREAM_SNAP_SIZE; i += 4) {
            uint32_t v = dice_be32(s + i);
            if (!v) continue;
            IOReturn r = dice_write_q(sf_off + i, v);
            if (r != kIOReturnSuccess) { fprintf(stderr, "write +0x%x failed: 0x%08x\n", i, r); return 1; }
            n++;
        }
        printf("wrote %u non-zero quadlets into stream_format\n", n);
        uint32_t ret;
        return run_cmd(OP_LOAD_STREAM, flag, &ret);
    }
    return usage();
}

static int cmd_session(int argc, char **argv)
{
    if (argc < 3) return usage();
    snapshot_streams();
    if (claim_owner() != 0) return 1;
    int rc = 0;
    for (int i = 2; i < argc && rc == 0; i++) {
        char line[1024];
        strncpy(line, argv[i], sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        char *tok[96];
        int n = 0;
        tok[n++] = (char *)"dice_ctl";
        for (char *p = strtok(line, " "); p && n < 95; p = strtok(NULL, " ")) tok[n++] = p;
        tok[n] = NULL;
        printf("\n[session] > %s\n", argv[i]);
        if (n >= 2 && !strcmp(tok[1], "sleep")) { sleep(n >= 3 ? (unsigned)atoi(tok[2]) : 1); continue; }
        rc = dispatch(n, tok);
        uint32_t g;
        uint64_t c;
        if (read_owner(&g, &c) == 0)
            printf("[session] owner now 0x%016llx\n", (unsigned long long)c);
    }
    printf("\n[session] releasing ownership\n");
    release_owner();
    return rc;
}

static int cmd_mixer(int argc, char **argv)
{
    uint32_t base, size;
    if (dice_ext_section(2, &base, &size) != 0 || size < 4 + MIXER_IN * MIXER_OUT * 4) {
        fprintf(stderr, "cannot locate mixer section\n");
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[2], "get")) {
        static uint8_t buf[0x500];
        if (dice_read(base, buf, size) != kIOReturnSuccess) {
            fprintf(stderr, "mixer read failed\n");
            return 1;
        }
        printf("saturation flags: 0x%08x\n", dice_be32(buf));
        int n = 0;
        for (int o = 0; o < MIXER_OUT; o++)
            for (int i = 0; i < MIXER_IN; i++) {
                uint32_t v = dice_be32(buf + 4 + (o * MIXER_IN + i) * 4);
                if (v) { printf("out %2d <- in %2d : 0x%04x\n", o, i, v); n++; }
            }
        printf("(%d non-zero cells)\n", n);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[2], "set")) {
        long o = strtol(argv[3], NULL, 0), i = strtol(argv[4], NULL, 0);
        long v = strtol(argv[5], NULL, 0);
        if (o < 0 || o >= MIXER_OUT || i < 0 || i >= MIXER_IN || v < 0 || v > 0x7fff)
            return usage();
        uint32_t off = base + 4 + (uint32_t)(o * MIXER_IN + i) * 4;
        uint8_t b[4];
        dice_read(off, b, 4);
        printf("out %ld <- in %ld : 0x%04x -> 0x%04lx\n", o, i, dice_be32(b), v);
        IOReturn r = dice_write_q(off, (uint32_t)v);
        if (r != kIOReturnSuccess) {
            fprintf(stderr, "write failed: 0x%08x\n", r);
            return 1;
        }
        dice_read(off, b, 4);
        printf("read back: 0x%04x\n", dice_be32(b));
        return 0;
    }
    return usage();
}

static int cmd_set(int argc, char **argv)
{
    uint32_t base;
    uint8_t raw[MBOX3_APP_SECTION_MIN_SIZE];
    mbox3_params p;
    if (argc < 4 || read_app(&base, raw, &p) != 0) return argc < 4 ? usage() : 1;

    const char *what = argv[2];
    if (!strcmp(what, "phantom") && argc == 4) {
        int on = parse_onoff(argv[3]);
        if (on < 0) return usage();
        if (on) fprintf(stderr, "WARNING: enabling 48 V phantom power. Make sure no ribbon "
                                "mics or unbalanced gear are connected.\n");
        p.phantom_powering = on;
    } else if (!strcmp(what, "hpf") && argc == 5) {
        long n = strtol(argv[3], NULL, 0);
        int on = parse_onoff(argv[4]);
        if (n < 1 || n > 4 || on < 0) return usage();
        p.hpf_enables[n - 1] = on;
    } else if (!strcmp(what, "reverb-volume") && argc == 4) {
        long v = strtol(argv[3], NULL, 0);
        if (v < 0 || v > 255) return usage();
        p.reverb_volume = (uint8_t)v;
    } else if (!strcmp(what, "trim") && argc == 5) {
        long n = strtol(argv[3], NULL, 0), v = strtol(argv[4], NULL, 0);
        if (n < 1 || n > 6 || v < 0 || v > 255) return usage();
        p.output_trims[n - 1] = (uint8_t)v;
    } else {
        return usage();
    }
    return apply_params(base, raw, &p) == 0 ? 0 : 1;
}

/* peak [seconds]: sample the peak-meter section 5x/s, keep the maximum per
 * routed connection, and print every connection that ever showed a level.
 * Entry = (level << 16) | (src << 8) | dst, in router-table order. */
static int cmd_peak(int argc, char **argv)
{
    int all = argc >= 3 && !strcmp(argv[2], "all");
    unsigned secs = all ? 1 : argc >= 3 ? (unsigned)atoi(argv[2]) : 5;
    uint32_t off, size;
    if (dice_ext_section(3, &off, &size) != 0) { fprintf(stderr, "peak section lookup failed\n"); return 1; }
    if (size > 128 * 4) size = 128 * 4;
    uint32_t n = size / 4;
    uint32_t maxlvl[128] = {0}, route[128] = {0};
    uint8_t buf[128 * 4];
    for (unsigned t = 0; t < secs * 5; t++) {
        IOReturn r = dice_read(off, buf, size);
        if (r != kIOReturnSuccess) { fprintf(stderr, "peak read failed: 0x%08x\n", r); return 1; }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t e = dice_be32(buf + i * 4);
            route[i] = e & 0xffff;
            if ((e >> 16) > maxlvl[i]) maxlvl[i] = e >> 16;
        }
        usleep(200000);
    }
    unsigned shown = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!route[i]) continue;
        if (all) {
            /* Stop at the first row whose block IDs are not valid (the section
             * continues with stale memory after the real table). */
            if (src_blk((route[i] >> 12) & 0xf)[0] == '?' || dst_blk((route[i] >> 4) & 0xf)[0] == '?') break;
        } else if (!maxlvl[i]) continue;
        printf("  [%3u] %04x  ", i, route[i]);
        print_route(route[i]);
        printf("  peak 0x%04x\n", maxlvl[i]);
        shown++;
    }
    printf("(%u connections with signal, %u s)\n", shown, secs);
    return 0;
}

/* Global section registers (relative to the global section base):
 * +0x4c clock select (rate index << 8 | source), +0x50 enable,
 * +0x54 status, +0x5c nominal rate. */
static const uint32_t RATES[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};

static int global_base(uint32_t *base)
{
    uint8_t t[4];
    if (dice_read(0, t, 4) != kIOReturnSuccess) return -1;
    *base = dice_be32(t) * 4;
    return 0;
}

static int rd_q(uint32_t off, uint32_t *v)
{
    uint8_t b[4];
    if (dice_read(off, b, 4) != kIOReturnSuccess) return -1;
    *v = dice_be32(b);
    return 0;
}

static int show_clock(uint32_t g)
{
    uint32_t sel = 0, en = 0, st = 0, nom = 0;
    if (rd_q(g + 0x4c, &sel) || rd_q(g + 0x50, &en) ||
        rd_q(g + 0x54, &st) || rd_q(g + 0x5c, &nom)) return 1;
    unsigned ri = (sel >> 8) & 0xff, si = (st >> 8) & 0xff;
    printf("clock select 0x%08x (rate idx %u = %u Hz, source 0x%02x)\n",
           sel, ri, ri < 7 ? RATES[ri] : 0, sel & 0xff);
    printf("enable       0x%08x\n", en);
    printf("status       0x%08x (source locked %u, nominal rate idx %u = %u Hz)\n",
           st, st & 1, si, si < 7 ? RATES[si] : 0);
    printf("nominal rate 0x%08x (%u Hz)\n", nom, nom);
    return 0;
}

/* clock show | clock set RATE [SRC] | clock enable on|off */
static int cmd_clock(int argc, char **argv)
{
    uint32_t g;
    if (global_base(&g)) return 1;
    if (argc >= 3 && !strcmp(argv[2], "set") && argc >= 4) {
        unsigned ri = 99;
        for (unsigned i = 0; i < 7; i++) if (RATES[i] == (uint32_t)atoi(argv[3])) ri = i;
        if (ri == 99) { fprintf(stderr, "rate must be 32000/44100/48000/88200/96000/176400/192000\n"); return 1; }
        unsigned src = argc >= 5 ? (unsigned)strtoul(argv[4], NULL, 0) : 0x0c;
        uint32_t v = (ri << 8) | src;
        IOReturn r = dice_write_q(g + 0x4c, v);
        printf("write clock select 0x%08x: 0x%08x\n", v, r);
        usleep(500000);
    } else if (argc >= 4 && !strcmp(argv[2], "enable")) {
        uint32_t v = !strcmp(argv[3], "on") ? 1 : 0;
        IOReturn r = dice_write_q(g + 0x50, v);
        printf("write enable %u: 0x%08x\n", v, r);
        usleep(200000);
    }
    return show_clock(g);
}

/* dump N: print the non-zero quadlets of TCAT extension section N
 * (0 caps,1 command,2 mixer,3 peak,4 router,5 stream_format,6 current_config,
 * 7 standalone,8 application). */
static int cmd_dump(int argc, char **argv)
{
    if (argc < 3) return usage();
    uint32_t off, size;
    unsigned idx = (unsigned)atoi(argv[2]);
    if (dice_ext_section(idx, &off, &size) != 0) { fprintf(stderr, "section lookup failed\n"); return 1; }
    if (size > 0x1000) size = 0x1000;
    static uint8_t buf[0x1000];
    if (dice_read(off, buf, size) != kIOReturnSuccess) { fprintf(stderr, "read failed\n"); return 1; }
    unsigned shown = 0;
    printf("section %u at +0x%x, %u bytes (non-zero quadlets)\n", idx, off, size);
    for (uint32_t i = 0; i + 4 <= size; i += 4) {
        uint32_t v = dice_be32(buf + i);
        if (!v) continue;
        printf("  +0x%03x: %08x\n", i, v);
        shown++;
    }
    printf("(%u non-zero)\n", shown);
    return 0;
}

/* dumpraw OFF LEN: non-zero quadlets at DICE-relative offset OFF. */
static int cmd_dumpraw(int argc, char **argv)
{
    if (argc < 4) return usage();
    uint32_t off = (uint32_t)strtoul(argv[2], NULL, 0), len = (uint32_t)strtoul(argv[3], NULL, 0);
    if (len > 0x1000) len = 0x1000;
    static uint8_t buf[0x1000];
    if (dice_read(off, buf, len) != kIOReturnSuccess) { fprintf(stderr, "read failed\n"); return 1; }
    printf("raw +0x%x, %u bytes\n", off, len);
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
        uint32_t v = dice_be32(buf + i);
        if (v) printf("  +0x%03x: %08x\n", off + i, v);
    }
    return 0;
}

static int dispatch(int argc, char **argv)
{
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "dumpraw")) return cmd_dumpraw(argc, argv);
    if (!strcmp(argv[1], "dump")) return cmd_dump(argc, argv);
    if (!strcmp(argv[1], "stream")) return cmd_stream(argc, argv);
    if (!strcmp(argv[1], "clock")) return cmd_clock(argc, argv);
    if (!strcmp(argv[1], "peak")) return cmd_peak(argc, argv);
    if (!strcmp(argv[1], "show")) {
        uint32_t base;
        uint8_t raw[MBOX3_APP_SECTION_MIN_SIZE];
        mbox3_params p;
        int rc = read_app(&base, raw, &p);
        if (rc == 0) print_params(&p);
        return rc;
    }
    if (!strcmp(argv[1], "mixer")) return cmd_mixer(argc, argv);
    if (!strcmp(argv[1], "set")) return cmd_set(argc, argv);
    if (!strcmp(argv[1], "owner")) return cmd_owner(argc, argv);
    if (!strcmp(argv[1], "router")) return cmd_router(argc, argv);
    if (!strcmp(argv[1], "cmd")) return cmd_cmd(argc, argv);
    if (!strcmp(argv[1], "hold")) return cmd_hold(argc, argv);
    if (!strcmp(argv[1], "session")) return cmd_session(argc, argv);
    return usage();
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    if (dice_open() != 0) return 1;
    int rc = dispatch(argc, argv);
    dice_close();
    return rc;
}
