/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Minimal DICE register I/O over IOFireWireLib (macOS). Offsets are relative
 * to the DICE address base 0xffff_e000_0000.
 */
#ifndef DICE_IO_H
#define DICE_IO_H

#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <stdint.h>

#define DICE_EXT_BASE 0x00200000u /* TCAT extension, relative to DICE base */

/* Open the first FireWire device found. 0 on success. */
int dice_open(void);
void dice_close(void);

/* Reads try 256/64/16/4-byte transactions until one size works (some
 * sections, e.g. mixer, only accept quadlet transactions). */
IOReturn dice_read(uint32_t off, void *buf, uint32_t len);

/* Writes are always done as single quadlets. Value in host order. */
IOReturn dice_write_q(uint32_t off, uint32_t value);

/* One block write transaction of `len` bytes (data in wire order). */
IOReturn dice_write_block(uint32_t off, const void *buf, uint32_t len);

/* 64-bit compare-and-swap (lock transaction) at `off`. `old` receives the
 * value found at the target. Success = return 0 and *old == expected.
 * raw_order != 0 passes the quadlets as raw wire bytes instead of host-order
 * numbers (to diagnose the byte order IOFireWireLib expects). */
IOReturn dice_lock64(uint32_t off, uint64_t expected, uint64_t newval,
                     uint64_t *old, int raw_order);

/* Node ID (bus<<6 | node) of this Mac on the FireWire bus, read from the
 * IORegistry. 0 on success. */
int dice_local_node_id(uint16_t *id);

uint32_t dice_be32(const uint8_t *p);

/* The opened IOFireWireLib device interface (for isochronous setup). */
IOFireWireLibDeviceRef dice_device(void);

/* TCAT extension section table (9 entries). Returns 0 on success.
 * index: 0 caps,1 command,2 mixer,3 peak,4 router,5 stream_format,
 *        6 current_config,7 standalone,8 application.
 * *off is relative to the DICE base (already includes DICE_EXT_BASE). */
int dice_ext_section(unsigned index, uint32_t *off, uint32_t *size);

#endif /* DICE_IO_H */
