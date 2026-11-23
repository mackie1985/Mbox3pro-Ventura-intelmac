/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "dice_io.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <stdbool.h>
#include <stdio.h>

#define DICE_HI 0xffff
#define DICE_LO 0xe0000000u

static IOFireWireLibDeviceRef g_dev;
static io_service_t g_service;

IOFireWireLibDeviceRef dice_device(void) { return g_dev; }

uint32_t dice_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int dice_open(void)
{
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOFireWireDevice"), &it) != KERN_SUCCESS)
        return -1;

    io_service_t s;
    while ((s = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug = NULL;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(s, kIOFireWireLibTypeID,
                kIOCFPlugInInterfaceID, &plug, &score) == kIOReturnSuccess && plug) {
            (*plug)->QueryInterface(plug,
                CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID *)&g_dev);
            IODestroyPlugInInterface(plug);
            if (g_dev) { g_service = s; break; }
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (!g_dev) {
        fprintf(stderr, "no FireWire device found\n");
        return -1;
    }
    IOReturn r = (*g_dev)->Open(g_dev);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "Open failed: 0x%08x\n", r);
        g_dev = NULL;
        return -1;
    }
    return 0;
}

void dice_close(void)
{
    if (g_dev) (*g_dev)->Close(g_dev);
    g_dev = NULL;
}

static IOReturn read_one(uint32_t off, void *buf, uint32_t len)
{
    FWAddress addr = { .nodeID = 0, .addressHi = DICE_HI, .addressLo = DICE_LO + off };
    UInt32 gen = 0, size = len;
    (*g_dev)->GetBusGeneration(g_dev, &gen);
    return (*g_dev)->Read(g_dev, g_service, &addr, buf, &size, false, gen);
}

IOReturn dice_read(uint32_t off, void *buf, uint32_t len)
{
    static const uint32_t sizes[] = {256, 64, 16, 4};
    IOReturn r = kIOReturnSuccess;
    for (unsigned s = 0; s < 4; s++) {
        r = kIOReturnSuccess;
        for (uint32_t done = 0; done < len && r == kIOReturnSuccess;) {
            uint32_t n = len - done > sizes[s] ? sizes[s] : len - done;
            r = read_one(off + done, (uint8_t *)buf + done, n);
            done += n;
        }
        if (r == kIOReturnSuccess) return r;
    }
    return r;
}

IOReturn dice_write_q(uint32_t off, uint32_t value)
{
    uint8_t b[4] = { value >> 24, value >> 16, value >> 8, value };
    FWAddress addr = { .nodeID = 0, .addressHi = DICE_HI, .addressLo = DICE_LO + off };
    UInt32 gen = 0, size = 4;
    (*g_dev)->GetBusGeneration(g_dev, &gen);
    return (*g_dev)->Write(g_dev, g_service, &addr, b, &size, false, gen);
}

int dice_ext_section(unsigned index, uint32_t *off, uint32_t *size)
{
    uint8_t t[0x48];
    if (index > 8 || dice_read(DICE_EXT_BASE, t, sizeof(t)) != kIOReturnSuccess)
        return -1;
    *off = DICE_EXT_BASE + dice_be32(t + index * 8) * 4;
    *size = dice_be32(t + index * 8 + 4) * 4;
    return 0;
}

IOReturn dice_write_block(uint32_t off, const void *buf, uint32_t len)
{
    FWAddress addr = { .nodeID = 0, .addressHi = DICE_HI, .addressLo = DICE_LO + off };
    UInt32 gen = 0, size = len;
    (*g_dev)->GetBusGeneration(g_dev, &gen);
    return (*g_dev)->Write(g_dev, g_service, &addr, buf, &size, false, gen);
}

int dice_local_node_id(uint16_t *id)
{
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOFireWireLocalNode"), &it) != KERN_SUCCESS)
        return -1;
    int rc = -1;
    io_service_t s;
    while (rc != 0 && (s = IOIteratorNext(it))) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(
            s, CFSTR("FireWire Node ID"), kCFAllocatorDefault, 0);
        SInt32 n = 0;
        if (v && CFGetTypeID(v) == CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &n)) {
            *id = (uint16_t)n;
            rc = 0;
        }
        if (v) CFRelease(v);
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return rc;
}

IOReturn dice_lock64(uint32_t off, uint64_t expected, uint64_t newval,
                     uint64_t *old, int raw_order)
{
    FWAddress addr = { .nodeID = 0, .addressHi = DICE_HI, .addressLo = DICE_LO + off };
    UInt32 gen = 0;
    UInt32 e[2] = { (UInt32)(expected >> 32), (UInt32)expected };
    UInt32 n[2] = { (UInt32)(newval >> 32), (UInt32)newval };
    UInt32 o[2] = { 0, 0 };
    if (raw_order) {
        for (int i = 0; i < 2; i++) { e[i] = __builtin_bswap32(e[i]); n[i] = __builtin_bswap32(n[i]); }
    }
    (*g_dev)->GetBusGeneration(g_dev, &gen);
    IOReturn r = (*g_dev)->CompareSwap64(g_dev, g_service, &addr, e, n, o, 8, false, gen);
    if (raw_order) {
        for (int i = 0; i < 2; i++) o[i] = __builtin_bswap32(o[i]);
    }
    if (old) *old = ((uint64_t)o[0] << 32) | o[1];
    return r;
}
