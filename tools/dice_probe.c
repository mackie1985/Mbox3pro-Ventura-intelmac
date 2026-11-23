/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * dice_probe: read-only probe of a DICE (TCAT) FireWire device on macOS.
 *
 *   build:  make probe        (macOS only, needs Xcode command line tools)
 *   run:    ./build/dice_probe
 *
 * It only performs asynchronous READ transactions. It never writes to the
 * device. NOTE: written without access to a Mac; the first build may need
 * small API fixes. Please send back any compiler errors.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <libkern/OSByteOrder.h>
#include "../src/mbox3_params.h"

/* DICE private space: 0xffff_e000_0000 (to be confirmed by this very probe). */
#define DICE_HI 0xffff
#define DICE_LO 0xe0000000u
#define EXT_BASE 0x00200000u /* TCAT extension, relative to DICE base */

static IOFireWireLibDeviceRef g_dev;
static io_service_t g_service;

static IOReturn read_block(uint32_t lo, void *buf, uint32_t len)
{
    FWAddress addr = { .nodeID = 0, .addressHi = DICE_HI, .addressLo = lo };
    UInt32 gen = 0, size = len;
    (*g_dev)->GetBusGeneration(g_dev, &gen);
    return (*g_dev)->Read(g_dev, g_service, &addr, buf, &size, false, gen);
}

static IOReturn write_block(uint32_t lo, const void *buf, uint32_t len)
{
    FWAddress addr = { .nodeID = 0, .addressHi = DICE_HI, .addressLo = lo };
    UInt32 gen = 0, size = len;
    (*g_dev)->GetBusGeneration(g_dev, &gen);
    return (*g_dev)->Write(g_dev, g_service, &addr, buf, &size, false, gen);
}

/* Wire data is big-endian. IOFireWireLib hands back the bytes as received
 * on Intel, so treat the buffer as big-endian explicitly. */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void print_ascii(const char *label, const uint8_t *p, size_t n)
{
    /* DICE strings are stored as big-endian quadlets of chars: swap per 4. */
    char s[257] = {0};
    for (size_t i = 0; i + 3 < n && i < 256; i += 4) {
        s[i] = p[i + 3]; s[i + 1] = p[i + 2]; s[i + 2] = p[i + 1]; s[i + 3] = p[i + 0];
    }
    printf("  %-18s \"%s\"\n", label, s);
}

/* Read in <=256-byte chunks (the device's max_rec is 512 bytes). */
static IOReturn read_chunked(uint32_t lo, uint8_t *buf, uint32_t len)
{
    for (uint32_t done = 0; done < len;) {
        uint32_t n = len - done > 256 ? 256 : len - done;
        IOReturn r = read_block(lo + done, buf + done, n);
        if (r != kIOReturnSuccess) {
            fprintf(stderr, "  chunk read failed at +0x%x: 0x%08x\n", done, r);
            return r;
        }
        done += n;
    }
    return kIOReturnSuccess;
}

/* Router entry = (src << 8) | dst, each byte = block_id << 4 | channel. */
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

static void dump_nonzero(const uint8_t *buf, uint32_t start, uint32_t n, int maxrows)
{
    int rows = 0;
    for (uint32_t i = start & ~15u; i < n; i += 16) {
        int nz = 0;
        for (uint32_t j = 0; j < 16 && i + j + 4 <= n; j += 4)
            if (be32(buf + i + j)) nz = 1;
        if (!nz) continue;
        if (rows++ >= maxrows) { printf("  ... (more rows omitted)\n"); return; }
        printf("  +0x%04x:", i);
        for (uint32_t j = 0; j < 16 && i + j + 4 <= n; j += 4)
            printf(" %08x", be32(buf + i + j));
        printf("\n");
    }
}

/* Best-effort label for FireWire response codes (0xe0008010 + code). */
static const char *fw_err(IOReturn r)
{
    if (((uint32_t)r & 0xfffffff0u) != 0xe0008010u) return "";
    switch ((uint32_t)r & 0xf) {
    case 4: return " (FireWire response: conflict error)";
    case 5: return " (FireWire response: data error)";
    case 6: return " (FireWire response: type error)";
    case 7: return " (FireWire response: address error)";
    }
    return "";
}

static IOReturn read_sized(uint32_t lo, uint8_t *buf, uint32_t len, uint32_t chunk)
{
    for (uint32_t done = 0; done < len;) {
        uint32_t n = len - done > chunk ? chunk : len - done;
        IOReturn r = read_block(lo + done, buf + done, n);
        if (r != kIOReturnSuccess) return r;
        done += n;
    }
    return kIOReturnSuccess;
}

/* Try progressively smaller transactions; report which size worked. */
static IOReturn read_fallback(uint32_t lo, uint8_t *buf, uint32_t len)
{
    static const uint32_t sizes[] = {256, 64, 16, 4};
    IOReturn r = kIOReturnSuccess;
    for (unsigned i = 0; i < 4; i++) {
        r = read_sized(lo, buf, len, sizes[i]);
        if (r == kIOReturnSuccess) {
            printf("  (read ok with %u-byte transactions)\n", sizes[i]);
            return r;
        }
    }
    printf("  read failed with all sizes: 0x%08x%s\n", r, fw_err(r));
    return r;
}

static void print_names(const uint8_t *p, size_t n)
{
    char s[257] = {0};
    for (size_t i = 0; i + 3 < n && i < 256; i += 4) {
        s[i] = p[i + 3]; s[i + 1] = p[i + 2]; s[i + 2] = p[i + 1]; s[i + 3] = p[i];
    }
    printf("%s", s);
}

/* Stream config: [tx count][rx count] then entries of 0x10c bytes each:
 * [pcm channels][midi ports][names: 256 bytes][ac3 caps]. */
static void decode_stream_cfg(const uint8_t *buf, uint32_t base, uint32_t limit, const char *label)
{
    if (base + 8 > limit) return;
    uint32_t txn = be32(buf + base), rxn = be32(buf + base + 4);
    printf("  [%s] stream config at +0x%x: tx streams=%u rx streams=%u\n", label, base, txn, rxn);
    uint32_t off = base + 8;
    for (int dir = 0; dir < 2; dir++) {
        uint32_t cnt = dir ? rxn : txn;
        for (uint32_t i = 0; i < cnt && i < 4; i++, off += 0x10c) {
            if (off + 0x10c > limit) return;
            printf("    %s stream %u: pcm=%u midi=%u names: ", dir ? "rx" : "tx", i,
                   be32(buf + off), be32(buf + off + 4));
            print_names(buf + off + 8, 256);
            printf("\n");
        }
    }
}

static int open_first_device(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOFireWireDevice");
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) {
        fprintf(stderr, "no IOFireWireDevice matching service\n");
        return -1;
    }
    int idx = 0;
    io_service_t s;
    while ((s = IOIteratorNext(it))) {
        CFMutableDictionaryRef props = NULL;
        if (IORegistryEntryCreateCFProperties(s, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS) {
            printf("=== FireWire device #%d (registry properties) ===\n", idx);
            CFShow(props);
            CFRelease(props);
        }
        if (!g_dev) {
            IOCFPlugInInterface **plug = NULL;
            SInt32 score = 0;
            IOReturn r = IOCreatePlugInInterfaceForService(
                s, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score);
            if (r == kIOReturnSuccess && plug) {
                (*plug)->QueryInterface(
                    plug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
                    (LPVOID *)&g_dev);
                IODestroyPlugInInterface(plug);
                if (g_dev) {
                    g_service = s; /* keep reference for transactions */
                    idx++;
                    continue;
                }
            }
        } else {
            IOObjectRelease(s);
        }
        idx++;
    }
    IOObjectRelease(it);
    if (!g_dev) {
        fprintf(stderr, "could not create device interface\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (open_first_device() != 0)
        return 1;

    IOReturn r = (*g_dev)->Open(g_dev);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "Open failed: 0x%08x\n", r);
        return 1;
    }

    /* 1) Section pointer table: offsets/sizes in quadlets. */
    uint8_t tbl[0x28];
    r = read_block(DICE_LO, tbl, sizeof(tbl));
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "read of section table failed: 0x%08x\n"
                        "(device may not be a DICE device, or the address base differs)\n", r);
        (*g_dev)->Close(g_dev);
        return 1;
    }
    printf("\n=== DICE section table (offset/size in bytes) ===\n");
    const char *names[] = {"global", "tx", "rx", "ext_sync", "reserved"};
    uint32_t off[5], sz[5];
    for (int i = 0; i < 5; i++) {
        off[i] = be32(tbl + i * 8) * 4;
        sz[i] = be32(tbl + i * 8 + 4) * 4;
        printf("  %-9s offset=0x%06x size=0x%04x\n", names[i], off[i], sz[i]);
    }

    /* 2) Global section. */
    if (sz[0] >= 0x68) {
        uint8_t g[0x68 + 256];
        uint32_t n = sz[0] < sizeof(g) ? sz[0] : sizeof(g);
        r = read_block(DICE_LO + off[0], g, n);
        if (r == kIOReturnSuccess) {
            printf("\n=== DICE global section ===\n");
            printf("  owner              0x%08x%08x\n", be32(g), be32(g + 4));
            printf("  notification       0x%08x\n", be32(g + 0x08));
            print_ascii("nickname", g + 0x0c, 64);
            printf("  clock select       0x%08x\n", be32(g + 0x4c));
            printf("  enable             0x%08x\n", be32(g + 0x50));
            printf("  status             0x%08x\n", be32(g + 0x54));
            printf("  extended status    0x%08x\n", be32(g + 0x58));
            printf("  sample rate (nom.) %u Hz\n", be32(g + 0x5c));
            printf("  version            0x%08x\n", be32(g + 0x60));
            printf("  clock caps         0x%08x\n", be32(g + 0x64));
            if (n >= 0x68 + 256)
                print_ascii("clock source names", g + 0x68, 256);
        } else {
            fprintf(stderr, "global section read failed: 0x%08x\n", r);
        }
    }

    /* 2b) TX / RX stream sections and ext_sync: what streaming setup needs.
     * Layout (per TCAT): [stream count][entry size in quadlets] then entries.
     * TX entry: iso channel, pcm, midi, speed, names... (raw shown here). */
    {
        static uint8_t tb[0x470];
        const char *sn[2] = {"tx", "rx"};
        for (int k = 0; k < 2; k++) {
            uint32_t o = off[1 + k], n = sz[1 + k];
            if (n > sizeof(tb)) n = sizeof(tb);
            printf("\n=== DICE %s section ===\n", sn[k]);
            if (read_fallback(DICE_LO + o, tb, n) != kIOReturnSuccess) continue;
            uint32_t cnt = be32(tb), esz_q = be32(tb + 4);
            printf("  streams=%u entry size=%u quadlets (%u bytes)\n", cnt, esz_q, esz_q * 4);
            for (uint32_t i = 0; i < cnt && i < 4; i++) {
                uint32_t eo = 8 + i * esz_q * 4;
                if (eo + 24 > n) break;
                printf("  entry %u:", i);
                for (uint32_t j = 0; j < 6; j++) printf(" %08x", be32(tb + eo + j * 4));
                printf("\n");
            }
        }
        printf("\n=== DICE ext_sync section (raw) ===\n");
        if (read_fallback(DICE_LO + off[3], tb, sz[3]) == kIOReturnSuccess)
            for (uint32_t i = 0; i + 4 <= sz[3]; i += 4)
                printf("  +0x%02x: %08x\n", i, be32(tb + i));
    }

    /* 3) TCAT extension section table. Base 0x00200000 inside DICE space
     *    (from memory of the TCAT layout - verify against this output). */
    uint8_t et[0x48];
    r = read_block(DICE_LO + EXT_BASE, et, sizeof(et));
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "\nextension table read failed: 0x%08x\n", r);
        (*g_dev)->Close(g_dev);
        return 0;
    }
    printf("\n=== TCAT extension section table (offset/size in bytes) ===\n");
    const char *enames[] = {"caps", "command", "mixer", "peak", "router",
                            "stream_format", "current_config", "standalone",
                            "application"};
    uint32_t eoff[9], esz[9];
    for (int i = 0; i < 9; i++) {
        eoff[i] = be32(et + i * 8) * 4;
        esz[i] = be32(et + i * 8 + 4) * 4;
        printf("  %-15s offset=0x%06x size=0x%04x\n", enames[i], eoff[i], esz[i]);
    }

    /* 4) Application section: Mbox 3 Pro specific parameters. */
    if (esz[8] >= MBOX3_APP_SECTION_MIN_SIZE) {
        uint8_t app[MBOX3_APP_SECTION_MIN_SIZE];
        r = read_block(DICE_LO + EXT_BASE + eoff[8], app, sizeof(app));
        if (r == kIOReturnSuccess) {
            printf("\n=== application section (raw, 0x%x bytes) ===", (unsigned)sizeof(app));
            for (size_t i = 0; i < sizeof(app); i += 4) {
                if (i % 16 == 0) printf("\n  +0x%02zx:", i);
                printf(" %02x%02x%02x%02x", app[i], app[i+1], app[i+2], app[i+3]);
            }
            printf("\n");
            mbox3_params p;
            if (mbox3_params_deserialize(&p, app, sizeof(app)) == 0) {
                printf("=== decoded as Mbox 3 Pro parameters ===\n");
                printf("  standalone use case %d (0=mixer 1=ad/da 2=preamp 3=undef)\n", p.standalone_use_case);
                printf("  master knob         %u\n", p.master_knob_value);
                printf("  phantom power       %s\n", p.phantom_powering ? "on" : "off");
                printf("  HPF                 %d %d %d %d\n", p.hpf_enables[0], p.hpf_enables[1], p.hpf_enables[2], p.hpf_enables[3]);
                printf("  output trims        %u %u %u %u %u %u\n", p.output_trims[0], p.output_trims[1], p.output_trims[2], p.output_trims[3], p.output_trims[4], p.output_trims[5]);
                printf("  reverb              type=0x%02x vol=%u dur=%u fb=%u\n", p.reverb_type, p.reverb_volume, p.reverb_duration, p.reverb_feedback);
                printf("  knob assigns        %d %d %d %d %d %d\n", p.master_knob_assigns[0], p.master_knob_assigns[1], p.master_knob_assigns[2], p.master_knob_assigns[3], p.master_knob_assigns[4], p.master_knob_assigns[5]);
                printf("  LEDs                mute=%d mono=%d spkr=%d dim=%d hold=%u\n", p.mute_led, p.mono_led, p.spkr_led, p.dim_led, p.duration_hold);
            } else {
                printf("  (decode failed: unknown reverb type or layout mismatch)\n");
            }
        }
    }

    static uint8_t buf[0x6000];

    /* 4b) caps / command sections. */
    printf("\n=== caps section (raw) ===\n");
    if (read_fallback(DICE_LO + EXT_BASE + eoff[0], buf, esz[0]) == kIOReturnSuccess)
        for (uint32_t i = 0; i + 4 <= esz[0]; i += 4)
            printf("  +0x%02x: %08x\n", i, be32(buf + i));
    printf("\n=== command section (raw) ===\n");
    if (read_fallback(DICE_LO + EXT_BASE + eoff[1], buf, esz[1]) == kIOReturnSuccess)
        for (uint32_t i = 0; i + 4 <= esz[1]; i += 4)
            printf("  +0x%02x: %08x\n", i, be32(buf + i));

    /* 5) Live router section. */
    printf("\n=== router section (live) ===\n");
    if (esz[4] >= 4 && read_chunked(DICE_LO + EXT_BASE + eoff[4], buf, esz[4]) == kIOReturnSuccess) {
        uint32_t cnt = be32(buf);
        printf("  %u entries\n", cnt);
        for (uint32_t i = 0; i < cnt && 4 + i * 4 + 4 <= esz[4]; i++) {
            uint32_t e = be32(buf + 4 + i * 4);
            printf("  [%3u] %08x  ", i, e); print_route(e); printf("\n");
        }
    }

    /* 6) Mixer. */
    printf("\n=== mixer section ===\n");
    if (read_fallback(DICE_LO + EXT_BASE + eoff[2], buf, esz[2]) == kIOReturnSuccess) {
        printf("  head=0x%08x\n  non-zero cells (quadlet index, value):\n", be32(buf));
        int shown = 0;
        for (uint32_t i = 1; i * 4 + 4 <= esz[2]; i++) {
            uint32_t v = be32(buf + i * 4);
            if (v) { printf("  [%3u] %08x%s", i - 1, v, (++shown % 4) ? "" : "\n"); }
        }
        printf("%s  (%d non-zero)\n", (shown % 4) ? "\n" : "", shown);
    }

    /* 6b) Peak meters. */
    printf("\n=== peak section (non-zero rows) ===\n");
    if (read_fallback(DICE_LO + EXT_BASE + eoff[3], buf, esz[3]) == kIOReturnSuccess)
        dump_nonzero(buf, 0, esz[3], 16);

    /* 7) Standalone section. */
    printf("\n=== standalone section (non-zero rows) ===\n");
    if (read_chunked(DICE_LO + EXT_BASE + eoff[7], buf, esz[7]) == kIOReturnSuccess)
        dump_nonzero(buf, 0, esz[7], 16);

    /* 8) Stream format. */
    printf("\n=== stream_format section (non-zero rows) ===\n");
    if (read_chunked(DICE_LO + EXT_BASE + eoff[5], buf, esz[5]) == kIOReturnSuccess)
        dump_nonzero(buf, 0, esz[5], 40);

    /* 9) current_config: first block is a router table (count + entries). */
    printf("\n=== current_config ===\n");
    if (read_chunked(DICE_LO + EXT_BASE + eoff[6], buf, esz[6]) == kIOReturnSuccess) {
        uint32_t cnt = be32(buf);
        printf("  router table: %u entries\n", cnt);
        for (uint32_t i = 0; i < cnt && i < 128 && 4 + i * 4 + 4 <= esz[6]; i++) {
            uint32_t e = be32(buf + 4 + i * 4);
            printf("  [%3u] %08x  ", i, e); print_route(e); printf("\n");
        }
        printf("\n  stream configs per rate mode:\n");
        decode_stream_cfg(buf, 0x1000, esz[6], "low  (44.1/48k)");
        decode_stream_cfg(buf, 0x3000, esz[6], "mid  (88.2/96k)");
        decode_stream_cfg(buf, 0x5000, esz[6], "high (176.4/192k)");
    }

    /* 10) Optional write test (only with --test-write): changes the reverb
     * volume (application section +0x44) by one step, reads it back, then
     * restores the original value. Harmless to audio/routing. */
    if (argc > 1 && strcmp(argv[1], "--test-write") == 0) {
        uint32_t lo = DICE_LO + EXT_BASE + eoff[8] + 0x44;
        uint8_t b[4];
        printf("\n=== write test (reverb volume) ===\n");
        if (read_block(lo, b, 4) != kIOReturnSuccess) { printf("  initial read failed\n"); goto done; }
        uint32_t orig = be32(b), test = (orig == 0x30) ? 0x31 : 0x30;
        printf("  original value: 0x%08x\n", orig);
        uint8_t t[4] = { test >> 24, test >> 16, test >> 8, test };
        IOReturn wr = write_block(lo, t, 4);
        printf("  write 0x%08x -> 0x%08x%s\n", test, wr, fw_err(wr));
        if (wr == kIOReturnSuccess && read_block(lo, b, 4) == kIOReturnSuccess)
            printf("  read back: 0x%08x (%s)\n", be32(b), be32(b) == test ? "OK" : "MISMATCH");
        uint8_t o[4] = { orig >> 24, orig >> 16, orig >> 8, orig };
        IOReturn rr = write_block(lo, o, 4);
        if (rr == kIOReturnSuccess && read_block(lo, b, 4) == kIOReturnSuccess)
            printf("  restored: 0x%08x (%s)\n", be32(b), be32(b) == orig ? "OK" : "NOT RESTORED");
        else
            printf("  restore failed: 0x%08x%s - reset the value by power-cycling the Mbox\n", rr, fw_err(rr));
    }
done:
    (*g_dev)->Close(g_dev);
    return 0;
}
