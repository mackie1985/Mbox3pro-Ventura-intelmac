/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Thin abstraction over the OS FireWire stack.
 *
 * On macOS (Intel, Ventura) the implementation will use IOFireWireLib
 * (user space, IOKit) - see DESIGN.md. Keeping this interface small lets the
 * DICE/Mbox3 logic be tested against a fake transport on any host.
 */
#ifndef FW_TRANSPORT_H
#define FW_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

typedef struct fw_transport fw_transport;

/* All addresses are 48-bit IEEE 1394 offsets (DICE private space starts at
 * 0xffffe0000000 - to be confirmed against the TCAT spec / sniffed traffic). */
typedef struct {
    /* Asynchronous block read/write. Return 0 on success, negative errno-style
     * value otherwise. Lengths are multiples of 4 (quadlets), data is in
     * wire (big-endian) byte order. */
    int (*read_block)(fw_transport *t, uint64_t addr, void *buf, size_t len);
    int (*write_block)(fw_transport *t, uint64_t addr, const void *buf, size_t len);

    /* Register a handler for incoming write requests to `addr`
     * (DICE notifications are delivered as quadlet writes to the host). */
    int (*listen_notify)(fw_transport *t, uint64_t addr,
                         void (*cb)(void *ctx, uint32_t msg), void *ctx);

    /* Isochronous streaming (phase 2). */
    int (*iso_start_rx)(fw_transport *t, unsigned channel, size_t packet_bytes);
    int (*iso_start_tx)(fw_transport *t, unsigned channel, size_t packet_bytes);
    void (*iso_stop)(fw_transport *t);
} fw_transport_ops;

struct fw_transport {
    const fw_transport_ops *ops;
    void *priv;
};

#endif /* FW_TRANSPORT_H */
